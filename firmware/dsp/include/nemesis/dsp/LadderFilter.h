#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ForkConfig.h>
#include <nemesis/dsp/Oversampler.h>
#include <nemesis/dsp/Waveshaper.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace nemesis::dsp
{
    // Zero-delay-feedback 4-pole transistor ladder — the "Ladder"/Moog-style 24 dB filter
    // (filter.md §3, CATHODE §3 `synth.filt.model = ladder`). Four cascaded TPT one-pole low-passes
    // with a global resonance feedback  u = x - k*y4  resolved zero-delay in CLOSED FORM. Two authentic
    // ladder taps are exposed: LP24 (4-pole, 24 dB/oct) and LP12 (2-pole tap, 12 dB/oct). The classic
    // Moog resonance-loss (the low end thins as resonance climbs — settled DC gain = 1/(1+k)) is
    // PRESERVED as character, not compensated. RT-safe and deterministic, no JUCE; `prepare()` allocates
    // the two oversamplers and is the only method that may.
    //
    // ⚠ IT IS NO LONGER "THE LINEAR CORE ONLY" (T-CMP-23). This block used to end with the sentence
    // "the per-stage tanh SATURATION + oversampling that give the driven-Moog 'growl' … are separate
    // follow-on cards — this is the linear core only", and T-CMP-23's card quotes exactly that as its
    // WHAT-IS-WRONG. filter.md §3's growl is now BUILT: `setDrive` puts a per-stage bounded soft-clip
    // inside the feedback path and runs the core in a 4x oversampler, and the whole nonlinear apparatus
    // is gated on drive > 0 so an undriven instance is bit-exact and latency-identical to the core this
    // header used to describe (`linearStep` is that core, verbatim). Still separate follow-on cards: the
    // derived BP/HP taps, and true sustained self-oscillation.
    //
    // Topology: Zavalishin, "The Art of VA Filter Design" (public algorithm; same non-GPL lineage as the
    // M1-01 TptSvf).
    class LadderFilter
    {
    public:
        struct Outputs
        {
            float lp24 = 0.0f; // 4-pole ladder output (24 dB/oct)
            float lp12 = 0.0f; // 2-pole tap (12 dB/oct)
        };

        // Oversampling factor for the DRIVEN (nonlinear) core (filter.md §3: 2x-4x). 1 == no
        // oversampling (the naive alias baseline the keystone measures against).
        static constexpr int kDefaultOversample = 4;

        // Sets the sample rate, designs the oversampler FIRs for `oversampleFactor`, and clears state.
        // Allocates; call off the audio thread (like prepareToPlay).
        void prepare (Real sampleRate, int oversampleFactor = kDefaultOversample);

        // Drive amount in [0,1] (`synth.filt.drive` in a Ladder mode; owner ruling 2026-09-07 makes
        // `drivePos = Pre` mean IN-LOOP here). 0 == the linear core runs verbatim: bit-exact and
        // latency-free. Above 0 the per-stage saturators engage inside the oversampler.
        void setDrive (float amount) noexcept;

        // Oversampling factor of the driven core (1 == bypass).
        [[nodiscard]] int factor() const noexcept { return os24_.factor(); }

        // Base-rate latency. 0 while drive == 0 (owner ruling 2: an undriven Ladder patch is
        // latency-identical to the pre-growl core); the OS cascade's group delay once driven. It is the
        // TARGET path's: across a drive-0 crossing the output's delay glides (see `kDriveCrossfadeMs`).
        [[nodiscard]] int latencySamples() const noexcept;

        // [T-CMP-27] A drive-0 CROSSING is a crossfade, not a switch: the undriven core runs on through
        // it, so no filter memory is discarded mid-note. The driven path lags by `latencySamples()`, so
        // the undriven output is first GLIDED (a varispeed read) from 0 to that lag — both paths aligned
        // before they overlap, or the sum combs — and then crossfaded linearly. Up: glide, then fade in.
        // Down: fade out, then glide back to 0. Each phase takes this long; both endpoints are
        // bit-exact (undriven: the linear core alone; driven: the driven path alone).
        static constexpr Real kDriveCrossfadeMs = 5.0;

        // Samples each crossfade phase takes at the prepared rate (the glide, then the fade).
        [[nodiscard]] int crossfadeSamples() const noexcept { return xfLen_; }
        // True while a drive-0 crossing is in progress.
        [[nodiscard]] bool crossfading() const noexcept { return xfPos_ != 0 && xfPos_ != 2 * xfLen_; }

        // Clears the four integrator states (silences the filter) without touching coefficients.
        void reset() noexcept;

        // Recomputes coefficients for a cutoff (Hz) and resonance. cutoffHz is clamped below Nyquist
        // (tan bound); resonance in [0,1] maps to the feedback k in [0, ~4) — the top APPROACHES
        // self-oscillation but k is clamped just under the k=4 marginal threshold, so the filter rings
        // (bounded) rather than blowing up. Allocation-free.
        void setCutoff (float cutoffHz, float resonance) noexcept;
        // FIREFORK E4: what setCutoff would compute, taken from a twin that just ran it with the same inputs.
        void copyCoefficientsFrom (const LadderFilter& o) noexcept { cutoffHz_ = o.cutoffHz_; reso_ = o.reso_; k_ = o.k_; lin_ = o.lin_; drv_ = o.drv_; }

        // FIREFORK E6: which driven tap the caller reads (CathodeFilterBank sets it with the mode). With NEMESIS_FORK_EXACT the
        // other tap's decimation is skipped; its value in Outputs is then 0 and must not be read.
        void setUsedTap (bool lp24) noexcept { use24_ = lp24 ? 1 : 0; use12_ = lp24 ? 0 : 1; }
        void setBothTapsUsed() noexcept { use24_ = use12_ = 1; }

        // Resonance feedback k of the current coefficients (0 .. ~4). Exposed for tests/inspection.
        Real feedbackK() const noexcept { return k_; }

        // Processes one input sample, returning both ladder taps. Hot path: kept inline (pure
        // arithmetic, no libm, no division — the loop divisor is precomputed control-rate) so it inlines.
        // While drive == 0 (and no crossing is in progress) this is the LINEAR core, unchanged and
        // bit-exact (owner ruling 2026-09-07); above 0 the per-stage saturators run inside the oversampler.
        Outputs processSample (float x) noexcept
        {
            if (!driven_ && xfPos_ == 0)
                return linearStep (static_cast<Real> (x));
            return crossStep (x);
        }

    private:
        // Everything that is not the settled undriven path: the linear core runs on (its lagged history
        // feeds the glide), the driven path runs while it is live, and the two are aligned + blended.
        Outputs crossStep (float x) noexcept
        {
            const Outputs lin = linearStep (static_cast<Real> (x));
            ring24_[ringPos_] = lin.lp24;
            ring12_[ringPos_] = lin.lp12;
            const Outputs drv = drivenLive_ ? drivenStep (x) : Outputs {};

            Outputs out;
            if (xfPos_ >= 2 * xfLen_)
                out = drv; // settled driven: the driven path alone, bit for bit
            else
            {
                // Phase 1 (xfPos_ 0..N): the linear output read `lag` behind, lag gliding 0 → latency.
                // Phase 2 (N..2N): at full lag, faded linearly into the driven path.
                const int t = xfPos_ < xfLen_ ? xfPos_ : xfLen_;
                const Real lag = static_cast<Real> (os24_.latencySamples()) * static_cast<Real> (t) / static_cast<Real> (xfLen_);
                const Outputs aligned { ringRead (ring24_.data(), lag), ringRead (ring12_.data(), lag) };
                if (xfPos_ <= xfLen_)
                    out = aligned;
                else
                {
                    const float w = static_cast<float> (xfPos_ - xfLen_) / static_cast<float> (xfLen_);
                    out = { aligned.lp24 + w * (drv.lp24 - aligned.lp24), aligned.lp12 + w * (drv.lp12 - aligned.lp12) };
                }
            }
            ringPos_ = (ringPos_ + 1) & kRingMask;

            if (driven_)
                xfPos_ = xfPos_ < 2 * xfLen_ ? xfPos_ + 1 : xfPos_;
            else if (xfPos_ > 0)
            {
                --xfPos_;
                drivenLive_ = xfPos_ > 0; // back on the linear core alone: the driven path stops
            }
            return out;
        }

        // A read `lag` samples behind the one just written, linearly interpolated. The glide rises at
        // latency/N < 1 sample per sample, so it never reaches back past the crossing that started it.
        [[nodiscard]] float ringRead (const float* ring, Real lag) const noexcept
        {
            const auto whole = static_cast<std::size_t> (lag);
            const auto frac = static_cast<float> (lag - static_cast<Real> (whole));
            const float a = ring[(ringPos_ - whole) & kRingMask];
            if (frac == 0.0f)
                return a;
            const float b = ring[(ringPos_ - whole - 1) & kRingMask];
            return a + frac * (b - a);
        }

        // The DRIVEN path: the nonlinear core inside the oversampler, on its own states and coefficients.
        Outputs drivenStep (float x) noexcept
        {
            // One core pass, two decimated taps. os24_ runs the core and stashes each OS-rate lp12 into
            // fifo_; os12_ then only DECIMATES that stashed stream [D-M-0356] — it has no input to
            // upsample, so it runs no interpolating FIR (bit-identical to the old discarded upsample).
            fifoFill_ = 0;
            const auto core = [this] (float u) noexcept {
                const Outputs o = coreStep (static_cast<Real> (u));
                fifo_[static_cast<std::size_t> (fifoFill_)] = o.lp12;
                ++fifoFill_;
                return o.lp24;
            };
            fifoRead_ = 0;
            const auto stashed = [this]() noexcept {
                const float v = fifo_[static_cast<std::size_t> (fifoRead_)];
                ++fifoRead_;
                return v;
            };
            if constexpr (fork::kExact) // FIREFORK E6
            {
                float a = 0.0f, b = 0.0f;
                if (use24_)
                    a = os24_.processSample (x, core);
                else
                    os24_.processNoDecimate (x, core);
                if (use12_)
                    b = os12_.decimateSample (stashed);
                else
                    os12_.pushOsOnly (stashed);
                return { a, b };
            }
            const float a = os24_.processSample (x, core);
            const float b = os12_.decimateSample (stashed);
            return { a, b };
        }

        // The LINEAR ladder, verbatim as it shipped before T-CMP-23. Reached whenever drive == 0, which
        // is what makes an undriven patch bit-exact — the bypass is the DRIVE PREDICATE, never "psi
        // happens to be small" (psi = v - softclip(v) is O(v^3), small but never exactly 0).
        Outputs linearStep (Real in) noexcept
        {
            // Zero-delay resolution. Each TPT one-pole obeys  y_i = G*u_i + s_i*(1-G), so the four
            // cascaded stages give  y4 = G^4 * u + S  with  S = (1-G)*(G^3 s1 + G^2 s2 + G s3 + s4)  the
            // instantaneous response to the integrator states, and  u = in - k*y4  the feedback node.
            // Solving that linear loop yields the closed form for y4 (hence u) below.
            const Coeffs& c = lin_;
            const Real s = c.oneMinusG * (c.g3 * s1_ + c.g2 * s2_ + c.g * s3_ + s4_);
            const Real y4 = (c.g4 * in + s) * c.gComp;
            const Real u = in - k_ * y4;

            // Forward pass through the four one-poles, updating states (TPT: s_new = 2v + s).
            const Real v1 = (u - s1_) * c.g;
            const Real y1 = v1 + s1_;
            s1_ = y1 + v1;

            const Real v2 = (y1 - s2_) * c.g;
            const Real y2 = v2 + s2_;
            s2_ = y2 + v2;

            const Real v3 = (y2 - s3_) * c.g;
            const Real y3 = v3 + s3_;
            s3_ = y3 + v3;

            const Real v4 = (y3 - s4_) * c.g;
            const Real y4b = v4 + s4_; // == y4 (the closed form), now state-consistent
            s4_ = y4b + v4;

            return { static_cast<float> (y4b), static_cast<float> (y2) };
        }

        // One OS-rate sample through the NONLINEAR ladder — filter.md §3's "tanh per stage", resolved by
        // §0's "one cheap fixed-point iteration": each stage's saturator is split as sat(v) = v - psi
        // with psi = v - softclip(v) the CLIPPING EXCESS, seeded from the previous OS sample. The linear
        // part is still solved EXACTLY in closed form; only psi lags.
        //
        // Algebra: psi_i enters stage i as -g*psi_i and is multiplied by g once per later stage, so it
        // reaches y4 with weight g^(5-i). Hence  y4 = G^4*u + S - E  with
        //   E = G^4*psi1 + G^3*psi2 + G^2*psi3 + G*psi4,
        // and the SAME closed-form solve as the linear core, with one extra term:
        //   y4 = (G^4*in + S - E) * gComp.
        // (The house substitutes the bounded cubic AdaaSoftClip::shape for the spec's tanh, exactly as
        // ScreamFilter does for its "soft diode clipper" — it keeps this path libm-free.)
        Outputs coreStep (Real u) noexcept
        {
            const Real in = u * driveGain_;

            // (1) PREDICT y4 to resolve the feedback node. This is the only place the lag is needed:
            // y4 depends on the psi's, which depend on uf, which depends on y4. Lagged psi's break that
            // circle for one OS sample -- the "one cheap fixed-point iteration" of filter.md §0.
            const Coeffs& c = drv_;
            const Real s = c.oneMinusG * (c.g3 * d1_ + c.g2 * d2_ + c.g * d3_ + d4_);
            const Real e = c.g4 * psi1_ + c.g3 * psi2_ + c.g2 * psi3_ + c.g * psi4_;
            const Real y4pred = (c.g4 * in + s - e) * c.gComp;
            const Real uf = in - k_ * y4pred; // the feedback node = stage 1's input

            // (2) Forward pass with the EXACT per-stage saturation. Each stage's input is in hand here,
            // so nothing is approximated: sat() is applied directly, not as v - psi_lagged. That is what
            // keeps the filter BOUNDED at any drive -- every integrator sees a clipper-bounded input,
            // whereas a lagged excess is unbounded for large v and lets the states run away.
            const Real a1 = AdaaSoftClip::shape (uf);
            const Real v1 = (a1 - d1_) * c.g;
            const Real y1 = v1 + d1_;
            d1_ = y1 + v1;

            const Real a2 = AdaaSoftClip::shape (y1);
            const Real v2 = (a2 - d2_) * c.g;
            const Real y2 = v2 + d2_;
            d2_ = y2 + v2;

            const Real a3 = AdaaSoftClip::shape (y2);
            const Real v3 = (a3 - d3_) * c.g;
            const Real y3 = v3 + d3_;
            d3_ = y3 + v3;

            const Real a4 = AdaaSoftClip::shape (y3);
            const Real v4 = (a4 - d4_) * c.g;
            const Real y4b = v4 + d4_;
            d4_ = y4b + v4;

            // (3) Seed the next OS sample's excesses from the EXACT values just used.
            psi1_ = uf - a1;
            psi2_ = y1 - a2;
            psi3_ = y2 - a3;
            psi4_ = y3 - a4;

            return { static_cast<float> (y4b), static_cast<float> (y2) };
        }

        // Pre-gain span into the per-stage saturators; matches ScreamFilter/FilterDrive so "harmonics
        // grow monotonically with drive" is the same keystone shape across the nonlinear cores.
        static constexpr Real kDriveRange = 6.0;

        // Largest oversampling factor the tap FIFO can carry; prepare() clamps to it.
        static constexpr int kMaxOversample = 16;
        // The linear output's history for the glide: a power of two above the OS cascade's latency.
        static constexpr std::size_t kRingMask = 63;
        static_assert (((kRingMask + 1) & kRingMask) == 0, "the ring index wraps by mask: a power of two");

        // One rate's TPT coefficients: G = tan(pi*fc/rate) / (1 + tan), its powers, 1 - G, and the
        // zero-delay loop divisor 1 / (1 + k*G^4), all precomputed control-rate.
        struct Coeffs
        {
            Real g = 0.0;
            Real oneMinusG = 1.0;
            Real g2 = 0.0, g3 = 0.0, g4 = 0.0;
            Real gComp = 1.0;
        };
        void design (Coeffs& c, Real coreRate) const noexcept;
        // Starts the driven path from the linear core's states with an empty oversampler: its FIR
        // history and lagged excesses belong to no signal the path has seen.
        void startDrivenPath() noexcept;

        Oversampler os24_, os12_; // one core pass, two decimated taps (FltrEffect.cpp:131 needs both)
        std::array<float, kMaxOversample> fifo_ {}; // this step's OS-rate lp12 samples
        int fifoFill_ = 0, fifoRead_ = 0;
        unsigned char use24_ = 1, use12_ = 1; // FIREFORK E6
        bool driven_ = false;
        Real driveGain_ = 1.0;
        Real psi1_ = 0.0, psi2_ = 0.0, psi3_ = 0.0, psi4_ = 0.0; // per-stage clipping excesses (lagged)

        Real sampleRate_ = 48000.0;
        float cutoffHz_ = 1000.0f, reso_ = 0.0f; // held so the driven coefficients can be designed on a flip
        Coeffs lin_; // the linear core's, at fs
        Coeffs drv_; // the driven core's, at factor*fs (designed while the driven path is live)
        Real k_ = 0.0; // resonance feedback (0 .. ~4), rate-independent
        Real s1_ = 0.0, s2_ = 0.0, s3_ = 0.0, s4_ = 0.0; // the linear core's integrator states
        Real d1_ = 0.0, d2_ = 0.0, d3_ = 0.0, d4_ = 0.0; // the driven core's integrator states

        // [T-CMP-27] The crossing: xfPos_ runs 0 (undriven, settled) .. N (glided to the latency) ..
        // 2N (driven, settled), N = xfLen_. The driven path runs while drivenLive_.
        bool drivenLive_ = false;
        int xfPos_ = 0;
        int xfLen_ = 240;
        std::array<float, kRingMask + 1> ring24_ {};
        std::array<float, kRingMask + 1> ring12_ {};
        std::size_t ringPos_ = 0;
    };
}
