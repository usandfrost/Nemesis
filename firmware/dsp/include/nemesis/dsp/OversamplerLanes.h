#pragma once

#include <nemesis/dsp/FirKernel.h>
#include <nemesis/dsp/Oversampler.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#if defined(__ARM_NEON)
    #include <arm_neon.h>
#endif

// [T-CMP-37 L1] LANE-WISE (ACROSS-VOICES) OVERSAMPLER — BENCH ONLY, NOT WIRED INTO THE ENGINE.
//
// N independent `Oversampler`s (one per voice) advanced in lock step: lane `l` of every vector is voice `l`, and each
// lane's arithmetic is the scalar `Oversampler::processSample` operation for operation, so a lane's output is
// BIT-IDENTICAL to the scalar object fed the same input (the owner's ruling: no golden may move). What makes that
// true, and what a change here must keep:
//   * the serial `acc += c * x` reductions keep their j order; per lane it is ONE chain, the lanes run side by side;
//   * the FIR reductions round as `fir::unfusedEnd` says (FirKernel.h, shared with the scalar's `fir::dot`): taps before it are
//     mul-THEN-add (two roundings), the rest fused. [T-CMP-37 L2] Before that pin the scalar's rounding depended on the opt
//     level (-O0 fused every tap, -O3 vectorised into ordered mul-then-add blocks of 8; perf-budgets.md §3k); the pin keeps the
//     -O3 pattern, so the shipping build did not move. The float output hides a one-tap change almost always; the per-tap
//     probes in the test do not rely on that;
//   * everywhere else `B::fma (a, b, c)` is a*b + c fused (vfmaq_f64 / std::fma) exactly where the scalar source fuses; a plain
//     mul or add is never fused here (the named intrinsics are single operations);
//   * rounding to float where the scalar casts (`static_cast<float>`) is `roundF`/`storeF` (round-to-nearest, same as fcvt);
//   * the FIR coefficients are the scalar design, copied (the scalar's are private); the test pins them end to end.
// The shaper between the two FIRs runs on a whole vector set at once and is supplied by the caller (ClassicFilterLanes).
//
// Layout: SoA. Every history slot holds N doubles (lane-contiguous); coefficients are shared by all lanes (broadcast).
// N = 2 is one float64x2, N = 4 two independent ones (ILP across the serial chains), N = 8 four.
//
// Backends: `NeonBackend` (float64x2_t, arm64) and `ScalarBackend` (plain doubles + std::fma) — the same kernel over
// either, and the test runs both against the scalar object. Not RT-safe to prepare() (allocates); the hot path is not.
namespace nemesis::dsp::lanes
{
    // The fallback: two doubles, every operation per element. Fused ops use std::fma (exactly one rounding).
    struct ScalarBackend
    {
        struct V
        {
            double d[2];
        };
        struct M
        {
            std::uint64_t m[2];
        };
        static V set1 (double x) noexcept { return { { x, x } }; }
        static V load (const double* p) noexcept { return { { p[0], p[1] } }; }
        static void store (double* p, V v) noexcept
        {
            p[0] = v.d[0];
            p[1] = v.d[1];
        }
        static V add (V a, V b) noexcept { return { { a.d[0] + b.d[0], a.d[1] + b.d[1] } }; }
        static V sub (V a, V b) noexcept { return { { a.d[0] - b.d[0], a.d[1] - b.d[1] } }; }
        static V mul (V a, V b) noexcept { return { { a.d[0] * b.d[0], a.d[1] * b.d[1] } }; }
        static V div (V a, V b) noexcept { return { { a.d[0] / b.d[0], a.d[1] / b.d[1] } }; }
        static V neg (V a) noexcept { return { { -a.d[0], -a.d[1] } }; }
        // a*b + c, ONE rounding.
        static V fma (V a, V b, V c) noexcept { return { { std::fma (a.d[0], b.d[0], c.d[0]), std::fma (a.d[1], b.d[1], c.d[1]) } }; }
        static M ge (V a, V b) noexcept { return { { a.d[0] >= b.d[0] ? ~0ull : 0ull, a.d[1] >= b.d[1] ? ~0ull : 0ull } }; }
        static M le (V a, V b) noexcept { return { { a.d[0] <= b.d[0] ? ~0ull : 0ull, a.d[1] <= b.d[1] ? ~0ull : 0ull } }; }
        static M gt (V a, V b) noexcept { return { { a.d[0] > b.d[0] ? ~0ull : 0ull, a.d[1] > b.d[1] ? ~0ull : 0ull } }; }
        static M lt (V a, V b) noexcept { return { { a.d[0] < b.d[0] ? ~0ull : 0ull, a.d[1] < b.d[1] ? ~0ull : 0ull } }; }
        static M orr (M a, M b) noexcept { return { { a.m[0] | b.m[0], a.m[1] | b.m[1] } }; }
        static M loadMask (const std::uint64_t* p) noexcept { return { { p[0], p[1] } }; }
        // m ? a : b
        static V select (M m, V a, V b) noexcept { return { { m.m[0] ? a.d[0] : b.d[0], m.m[1] ? a.d[1] : b.d[1] } }; }
        static V loadF (const float* p) noexcept { return { { static_cast<double> (p[0]), static_cast<double> (p[1]) } }; }
        static void storeF (float* p, V v) noexcept
        {
            p[0] = static_cast<float> (v.d[0]);
            p[1] = static_cast<float> (v.d[1]);
        }
        static V roundF (V v) noexcept { return { { static_cast<double> (static_cast<float> (v.d[0])), static_cast<double> (static_cast<float> (v.d[1])) } }; }
    };

#if defined(__ARM_NEON)
    struct NeonBackend
    {
        using V = float64x2_t;
        using M = uint64x2_t;
        static V set1 (double x) noexcept { return vdupq_n_f64 (x); }
        static V load (const double* p) noexcept { return vld1q_f64 (p); }
        static void store (double* p, V v) noexcept { vst1q_f64 (p, v); }
        static V add (V a, V b) noexcept { return vaddq_f64 (a, b); }
        static V sub (V a, V b) noexcept { return vsubq_f64 (a, b); }
        static V mul (V a, V b) noexcept { return vmulq_f64 (a, b); }
        static V div (V a, V b) noexcept { return vdivq_f64 (a, b); }
        static V neg (V a) noexcept { return vnegq_f64 (a); }
        static V fma (V a, V b, V c) noexcept { return vfmaq_f64 (c, a, b); } // c + a*b, fused
        static M ge (V a, V b) noexcept { return vcgeq_f64 (a, b); }
        static M le (V a, V b) noexcept { return vcleq_f64 (a, b); }
        static M gt (V a, V b) noexcept { return vcgtq_f64 (a, b); }
        static M lt (V a, V b) noexcept { return vcltq_f64 (a, b); }
        static M orr (M a, M b) noexcept { return vorrq_u64 (a, b); }
        static M loadMask (const std::uint64_t* p) noexcept { return vld1q_u64 (p); }
        static V select (M m, V a, V b) noexcept { return vbslq_f64 (m, a, b); }
        static V loadF (const float* p) noexcept { return vcvt_f64_f32 (vld1_f32 (p)); }
        static void storeF (float* p, V v) noexcept { vst1_f32 (p, vcvt_f32_f64 (v)); }
        static V roundF (V v) noexcept { return vcvt_f64_f32 (vcvt_f32_f64 (v)); }
    };
    using DefaultBackend = NeonBackend;
#else
    using DefaultBackend = ScalarBackend;
#endif

    // N lanes of the scalar `Oversampler`, factor 1..8. `process (in, out, shape)`: in/out are N floats (lane l = voice l);
    // `shape (V (&u)[NV])` is called once per oversampled step, in phase order, with the N upsampled values ROUNDED TO FLOAT
    // (as doubles — what the scalar's `static_cast<float> (os)` hands the shaper) and must leave there the N shaped values,
    // also float-exact (what the scalar's `static_cast<double> (shape (...))` pushes).
    template <class B = DefaultBackend, int N = 4>
    class OversamplerLanes
    {
        static_assert (N >= 2 && N % 2 == 0, "lanes come in float64x2 pairs");

    public:
        static constexpr int kLanes = N;
        static constexpr std::size_t kNV = N / 2;
        static constexpr std::size_t kStride = N;
        using V = typename B::V;

        // Same design as Oversampler::prepare (the scalar's coefficients are private, so this is a copy of that code:
        // the test pins it bit for bit). Allocates; not RT-safe.
        void prepare (int factor)
        {
            factor_ = std::clamp (factor, 1, 8);
            if (factor_ == 1)
            {
                phaseTaps_ = downTaps_ = ohistLen_ = 0;
                up_.clear();
                down_.clear();
                xhist_.clear();
                ohist_.clear();
                xpos_ = opos_ = 0;
                return;
            }
            constexpr double kPi = 3.14159265358979323846;
            constexpr int m = 8; // kHalfTapsPerPhase in Oversampler.cpp
            const int l = factor_;
            const int nTaps = 2 * m * l + 1;
            const int centre = m * l;
            phaseTaps_ = 2 * m + 1;
            downTaps_ = nTaps;
            ohistLen_ = nTaps + l - 1;

            std::vector<double> h (static_cast<std::size_t> (nTaps));
            double sum = 0.0;
            const double denom = static_cast<double> (nTaps - 1);
            for (int n = 0; n < nTaps; ++n)
            {
                const int k = n - centre;
                const double sinc = (k == 0)
                                        ? 1.0 / static_cast<double> (l)
                                        : std::sin (kPi * static_cast<double> (k) / static_cast<double> (l))
                                              / (kPi * static_cast<double> (k));
                const double t = 2.0 * kPi * static_cast<double> (n) / denom;
                const double win = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2.0 * t)
                                   - 0.01168 * std::cos (3.0 * t);
                const double v = sinc * win;
                h[static_cast<std::size_t> (n)] = v;
                sum += v;
            }
            for (double& v : h)
                v /= sum;

            up_.assign (static_cast<std::size_t> (l) * static_cast<std::size_t> (phaseTaps_), 0.0);
            for (int p = 0; p < l; ++p)
                for (int j = 0; j < phaseTaps_; ++j)
                {
                    const int idx = j * l + p;
                    if (idx < nTaps)
                        up_[static_cast<std::size_t> (p) * static_cast<std::size_t> (phaseTaps_) + static_cast<std::size_t> (j)] = static_cast<double> (l) * h[static_cast<std::size_t> (idx)];
                }
            down_ = h;
            xhist_.assign (2 * static_cast<std::size_t> (phaseTaps_) * kStride, 0.0);
            ohist_.assign (2 * static_cast<std::size_t> (ohistLen_) * kStride, 0.0);
            xpos_ = opos_ = 0;
        }

        // All lanes. (Positions are shared and need not return to 0: only the window VALUES reach the output.)
        void reset() noexcept
        {
            std::fill (xhist_.begin(), xhist_.end(), 0.0);
            std::fill (ohist_.begin(), ohist_.end(), 0.0);
            xpos_ = opos_ = 0;
        }

        // One lane back to silence, the others untouched: zero that lane's slot in every history entry. A scalar
        // `Oversampler::reset()` zeroes the buffers AND the positions; with the window all zero the position is invisible.
        void resetLane (int lane) noexcept
        {
            for (std::size_t i = static_cast<std::size_t> (lane); i < xhist_.size(); i += kStride)
                xhist_[i] = 0.0;
            for (std::size_t i = static_cast<std::size_t> (lane); i < ohist_.size(); i += kStride)
                ohist_[i] = 0.0;
        }

        // [T-CMP-37 L2] A voice's scalar delay lines IN (its history becomes this lane's, the other lanes untouched) and OUT (the
        // lane's history becomes the scalar's). Values only: the lanes share one ring position and a scalar has its own, but
        // neither is state a read can see (Oversampler.h `copyWindowsTo`), so after an import the lane's next sample is the
        // scalar's next sample, and after an export the scalar's is the lane's. A mismatched design is left untouched (false).
        bool importLane (int lane, const Oversampler& s) noexcept
        {
            if (!sameDesign (s))
                return false;
            double x[kMaxPhaseTaps];
            double o[kMaxOsLen];
            s.copyWindowsTo (x, o);
            const auto l = static_cast<std::size_t> (lane);
            const auto xl = static_cast<std::size_t> (phaseTaps_);
            for (std::size_t i = 0; i < xl; ++i)
            {
                const std::size_t slot = (static_cast<std::size_t> (xpos_) + xl - i) % xl;
                xhist_[slot * kStride + l] = x[i];
                xhist_[(slot + xl) * kStride + l] = x[i];
            }
            const auto ol = static_cast<std::size_t> (ohistLen_);
            for (std::size_t i = 0; i < ol; ++i)
            {
                const std::size_t slot = (static_cast<std::size_t> (opos_) + ol - i) % ol;
                ohist_[slot * kStride + l] = o[i];
                ohist_[(slot + ol) * kStride + l] = o[i];
            }
            return true;
        }
        bool exportLane (int lane, Oversampler& s) const noexcept
        {
            if (!sameDesign (s))
                return false;
            double x[kMaxPhaseTaps];
            double o[kMaxOsLen];
            const auto l = static_cast<std::size_t> (lane);
            const auto xl = static_cast<std::size_t> (phaseTaps_);
            for (std::size_t i = 0; i < xl; ++i)
                x[i] = xhist_[((static_cast<std::size_t> (xpos_) + xl - i) % xl) * kStride + l];
            const auto ol = static_cast<std::size_t> (ohistLen_);
            for (std::size_t i = 0; i < ol; ++i)
                o[i] = ohist_[((static_cast<std::size_t> (opos_) + ol - i) % ol) * kStride + l];
            s.setWindows (x, o);
            return true;
        }

        [[nodiscard]] int factor() const noexcept { return factor_; }
        // The designed FIRs (inspection / tests): `upRow (p)` is phase p's phaseTaps() coefficients.
        [[nodiscard]] int phaseTaps() const noexcept { return phaseTaps_; }
        [[nodiscard]] int downTaps() const noexcept { return downTaps_; }
        [[nodiscard]] const double* upRow (int p) const noexcept { return up_.data() + static_cast<std::size_t> (p) * static_cast<std::size_t> (phaseTaps_); }
        [[nodiscard]] const double* downCoeffs() const noexcept { return down_.data(); }

        template <class Shaper>
        void process (const float* in, float* out, Shaper&& shape) noexcept
        {
            switch (factor_)
            {
                case 1:
                    processFactor1 (in, out, shape);
                    break;
                case 2:
                    run<2> (in, out, shape);
                    break;
                case 4:
                    run<4> (in, out, shape);
                    break;
                case 8:
                    run<8> (in, out, shape);
                    break;
                default:
                    run<0> (in, out, shape);
                    break; // 3, 5, 6, 7: the same code, factor read at run time
            }
        }

        // [T-CMP-37 L3] THE DECIMATING HALF ALONE — `Oversampler::decimateSample` for N voices (PhaseDistortionOscillator, whose
        // generator supplies the OS-rate stream itself). `steps` is `factor` rows of N floats: row p holds this step's p-th OS-rate
        // sample (oldest first) for each lane, exactly what the lane's `next()` would have returned. Bit-identical to N scalar
        // `decimateSample` calls; factor 1 returns the one step. The interpolating FIR is not run (a generator discards it).
        void decimateSteps (const float* steps, float* out) noexcept
        {
            switch (factor_)
            {
                case 1:
                    for (int l = 0; l < N; ++l)
                        out[l] = steps[l];
                    break;
                case 2:
                    decimateRun<2> (steps, out);
                    break;
                case 4:
                    decimateRun<4> (steps, out);
                    break;
                case 8:
                    decimateRun<8> (steps, out);
                    break;
                default:
                    decimateRun<0> (steps, out);
                    break;
            }
        }

    private:
        static constexpr std::size_t kMaxPhaseTaps = 17; // 2 * 8 + 1 (kHalfTapsPerPhase 8, every factor)
        static constexpr std::size_t kMaxOsLen = 136; // 2 * 8 * 8 + 1 + 7 (factor 8)

        [[nodiscard]] bool sameDesign (const Oversampler& s) const noexcept
        {
            return factor_ > 1 && s.factor() == factor_ && s.inputWindowLen() == phaseTaps_ && s.osWindowLen() == ohistLen_;
        }

        template <class Shaper>
        void processFactor1 (const float* in, float* out, Shaper& shape) noexcept
        {
            V u[kNV];
            for (std::size_t v = 0; v < kNV; ++v)
                u[v] = B::loadF (in + 2 * v);
            shape (u);
            for (std::size_t v = 0; v < kNV; ++v)
                B::storeF (out + 2 * v, u[v]);
        }

        void pushOs (const V (&w)[kNV]) noexcept
        {
            opos_ = opos_ + 1;
            if (opos_ >= ohistLen_)
                opos_ = 0;
            double* a = ohist_.data() + static_cast<std::size_t> (opos_) * kStride;
            double* b = ohist_.data() + (static_cast<std::size_t> (opos_) + static_cast<std::size_t> (ohistLen_)) * kStride;
            for (std::size_t v = 0; v < kNV; ++v)
            {
                B::store (a + 2 * v, w[v]);
                B::store (b + 2 * v, w[v]);
            }
        }

        // acc = acc + c*x: ONE rounding (fused) or two (mul, then add) — which taps do which is fir::unfusedEnd's business.
        template <bool Fused>
        static V tap (V c, V x, V acc) noexcept
        {
            if constexpr (Fused)
                return B::fma (c, x, acc);
            else
                return B::add (acc, B::mul (c, x));
        }
        // Taps [0, unfusedEnd) take the two-rounding form, the rest fuse: the scalar's rule, from the one definition.
        static std::size_t unfusedEnd (std::size_t taps) noexcept { return static_cast<std::size_t> (fir::unfusedEnd (static_cast<int> (taps))); }

        template <int F, class Shaper>
        void run (const float* in, float* out, Shaper& shape) noexcept
        {
            const int f = F != 0 ? F : factor_;
            constexpr int kMaxF = F != 0 ? F : 8;

            // Push the newest input into both mirror halves.
            xpos_ = xpos_ + 1;
            if (xpos_ >= phaseTaps_)
                xpos_ = 0;
            {
                double* a = xhist_.data() + static_cast<std::size_t> (xpos_) * kStride;
                double* b = xhist_.data() + (static_cast<std::size_t> (xpos_) + static_cast<std::size_t> (phaseTaps_)) * kStride;
                for (std::size_t v = 0; v < kNV; ++v)
                {
                    // float -> double is exact in either backend; reuse loadF so the input path is one conversion.
                    const V x = B::loadF (in + 2 * v);
                    B::store (a + 2 * v, x);
                    B::store (b + 2 * v, x);
                }
            }
            const std::size_t newest = (static_cast<std::size_t> (xpos_) + static_cast<std::size_t> (phaseTaps_)) * kStride;

            // Phase sums: every (phase, vector) is an independent serial chain in j order, acc = fma (c, x, acc).
            V acc[kMaxF][kNV];
            for (int p = 0; p < f; ++p)
                for (std::size_t v = 0; v < kNV; ++v)
                    acc[p][v] = B::set1 (0.0);
            const auto taps = static_cast<std::size_t> (phaseTaps_);
            const auto upStep = [&] (std::size_t j, auto fused) noexcept {
                V x[kNV];
                for (std::size_t v = 0; v < kNV; ++v)
                    x[v] = B::load (xhist_.data() + (newest - j * kStride) + 2 * v);
                for (int p = 0; p < f; ++p)
                {
                    const V c = B::set1 (up_[static_cast<std::size_t> (p) * taps + j]);
                    for (std::size_t v = 0; v < kNV; ++v)
                        acc[p][v] = tap<decltype (fused)::value> (c, x[v], acc[p][v]);
                }
            };
            for (std::size_t j = 0; j < unfusedEnd (taps); ++j)
                upStep (j, std::false_type {});
            for (std::size_t j = unfusedEnd (taps); j < taps; ++j)
                upStep (j, std::true_type {});
            // The shaper runs on the phases in order (it is stateful), each pushed into the OS history.
            for (int p = 0; p < f; ++p)
            {
                V u[kNV];
                for (std::size_t v = 0; v < kNV; ++v)
                    u[v] = B::roundF (acc[p][v]);
                shape (u);
                pushOs (u);
            }

            decimateInto<F> (out);
        }

        // Decimate: ONE serial chain per vector, j order kept. (Oversampler::decimate: the FIR over the OS history, referenced at the
        // phase-0 sample of this step.)
        template <int F>
        void decimateInto (float* out) noexcept
        {
            const int f = F != 0 ? F : factor_;
            V z[kNV];
            for (std::size_t v = 0; v < kNV; ++v)
                z[v] = B::set1 (0.0);
            const int phase0 = f - 1;
            const std::size_t ref = (static_cast<std::size_t> (opos_) + static_cast<std::size_t> (ohistLen_) - static_cast<std::size_t> (phase0)) * kStride;
            const auto dTaps = static_cast<std::size_t> (downTaps_);
            const auto downStep = [&] (std::size_t j, auto fused) noexcept {
                const V c = B::set1 (down_[j]);
                for (std::size_t v = 0; v < kNV; ++v)
                    z[v] = tap<decltype (fused)::value> (c, B::load (ohist_.data() + (ref - j * kStride) + 2 * v), z[v]);
            };
            for (std::size_t j = 0; j < unfusedEnd (dTaps); ++j)
                downStep (j, std::false_type {});
            for (std::size_t j = unfusedEnd (dTaps); j < dTaps; ++j)
                downStep (j, std::true_type {});
            for (std::size_t v = 0; v < kNV; ++v)
                B::storeF (out + 2 * v, z[v]);
        }

        // The decimating half alone, `F` rows (0: factor_ at run time).
        template <int F>
        void decimateRun (const float* steps, float* out) noexcept
        {
            const int f = F != 0 ? F : factor_;
            for (int p = 0; p < f; ++p)
            {
                V w[kNV];
                for (std::size_t v = 0; v < kNV; ++v)
                    w[v] = B::loadF (steps + static_cast<std::size_t> (p) * kStride + 2 * v);
                pushOs (w);
            }
            decimateInto<F> (out);
        }

        int factor_ = 1;
        int phaseTaps_ = 0;
        int downTaps_ = 0;
        int ohistLen_ = 0;
        int xpos_ = 0;
        int opos_ = 0;
        std::vector<double> up_;
        std::vector<double> down_;
        std::vector<double> xhist_; // [2 * phaseTaps_][N], mirrored
        std::vector<double> ohist_; // [2 * ohistLen_][N], mirrored
    };

    // [T-CMP-37 L3] The engine's batch of four oversamplers (the PD oscillators' decimators), and one voice's seat in it — the
    // ClassicLaneSlot shape: `resident` says the voice's delay lines LIVE in the lane (the scalar's are stale); `pull` hands them
    // back, `drop` says the scalar was just zeroed so the lane's copy is dead.
    using OsLanes4 = OversamplerLanes<DefaultBackend, 4>;
    struct OsLaneSlot
    {
        OsLanes4* lanes = nullptr;
        int lane = 0;
        bool resident = false;

        void pull (Oversampler& scalar) noexcept
        {
            if (resident)
            {
                lanes->exportLane (lane, scalar);
                resident = false;
            }
        }
        void drop() noexcept
        {
            if (resident)
            {
                lanes->resetLane (lane);
                resident = false;
            }
        }
    };
}
