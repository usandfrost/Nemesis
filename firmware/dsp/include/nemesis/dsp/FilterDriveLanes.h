#pragma once

#include <nemesis/dsp/FilterDrive.h>
#include <nemesis/dsp/OversamplerLanes.h>

#include <array>
#include <cstddef>

// [T-CMP-37 L3] LANE-WISE (ACROSS-VOICES) FILTER DRIVE — the output stage's `outDrive_` for four voices at a time.
//
// N `FilterDrive`s advanced in lock step as float64x2 lanes, BIT-IDENTICAL to N scalar objects: `FilterDrive::processSample`
// -> `Oversampler::processSample` with the shaper `adaa_.processSample (u * preGain_)`, each lane operation for operation
// (OversamplerLanes.h states the contract and what makes it hold). The scalar header is not touched.
//
// WHAT THE SHAPER DOES, and how each step is mirrored (AdaaSoftClip::processSample, Waveshaper.h):
//   * `u * preGain_` is a FLOAT product (both operands are floats): in double it is exact (24 + 24 bits), so rounding the double
//     product to float once (`roundF`) is the float multiply, bit for bit;
//   * `diff = x0 - x1_`, `f1x0 = antiderivative1 (x0)`; |diff| above kEps -> `(f1x0 - f1x1_) / diff` (a real divide, no reciprocal),
//     else `shape (0.5 * (x0 + x1_))`; both are computed in every lane and SELECTED, as the comparison (`>` / `<`, false for NaN
//     exactly as the scalar's `||` of two compares) says;
//   * antiderivative1: |x| >= 1 -> `2.0 * x / 3.0 - 0.25` (a divide between the product and the subtraction, so nothing
//     contracts), inside -> `x2 * 0.5 - x2 * x2 / 12.0` (the left product is `x2 * 0.5`, exact, so the contracted and the plain
//     form agree whatever the frontend does); shape: `x - x * x * x / 3.0`, its divide kept a divide;
//   * the result is rounded to float (`static_cast<float> (y)`) before it is pushed into the OS history.
// The two ADAA doubles (x1_, f1x1_) are the only state beside the FIR windows; both are taken in and given back by value.
namespace nemesis::dsp::lanes
{
    template <class B = DefaultBackend, int N = 4>
    class FilterDriveLanes
    {
    public:
        static constexpr int kLanes = N;
        static constexpr std::size_t kNV = N / 2;
        using V = typename B::V;

        // Designs the oversampler FIRs (the scalar's own design) and clears every lane. Allocates.
        void prepare (int oversampleFactor = FilterDrive::kDefaultOversample)
        {
            os_.prepare (oversampleFactor);
            reset();
        }
        void reset() noexcept
        {
            for (int l = 0; l < N; ++l)
                clearLaneState (l);
            os_.reset();
        }
        void resetLane (int lane) noexcept
        {
            clearLaneState (lane);
            os_.resetLane (lane);
        }

        // The engine's seam, as ClassicFilterLanes': a voice's scalar drive IN (its delay lines and the two ADAA doubles become the
        // lane's) and OUT (the lane's become the scalar's, and the lane is cleared — it would run on as a stale voice otherwise).
        bool importLane (int lane, const FilterDrive& d) noexcept
        {
            if (!os_.importLane (lane, d.oversampler()))
                return false;
            const auto st = d.adaaState();
            const auto i = static_cast<std::size_t> (lane);
            x1_[i] = st[0];
            f1x1_[i] = st[1];
            return true;
        }
        bool exportLane (int lane, FilterDrive& d) noexcept
        {
            if (!os_.exportLane (lane, d.oversampler()))
                return false;
            const auto i = static_cast<std::size_t> (lane);
            d.setAdaaState ({ static_cast<Real> (x1_[i]), static_cast<Real> (f1x1_[i]) });
            resetLane (lane);
            return true;
        }
        // Whether `d` is a drive this batch can stand in for (same oversampling design).
        [[nodiscard]] bool matches (const FilterDrive& d) const noexcept { return d.factor() == os_.factor() && os_.factor() > 1; }

        // The scalar's pre-gain as it stands (a float; exact in a double): every sample, for every member lane.
        void loadLaneGain (int lane, const FilterDrive& d) noexcept { preGain_[static_cast<std::size_t> (lane)] = static_cast<double> (d.preGain()); }

        [[nodiscard]] int factor() const noexcept { return os_.factor(); }
        // One lane's two ADAA doubles (x[n-1], F1 (x[n-1])), for a bit-level comparison with the scalar's.
        [[nodiscard]] std::array<double, 2> adaaState (int lane) const noexcept
        {
            const auto i = static_cast<std::size_t> (lane);
            return { x1_[i], f1x1_[i] };
        }

        // One base-rate sample for all N lanes: in/out are N floats.
        void processSample (const float* in, float* out) noexcept
        {
            os_.process (in, out, [this] (V (&u)[kNV]) noexcept { shape (u); });
        }

        // ---- the ADAA arithmetic, public so a test can read it BEFORE the float cast ----------------------------------------------
        // `y` only ever leaves the shaper as a float, so a one-ulp double difference in it flips the output with probability ~2^-29: the
        // output compare cannot see an op-order change here. These are compared with AdaaSoftClip's own doubles (test_filter_drive_lanes).
        // AdaaSoftClip::shape
        static V shapeV (V x) noexcept
        {
            const V one = B::set1 (1.0);
            const V c23 = B::set1 (2.0 / 3.0);
            const V mid = B::sub (x, B::div (B::mul (B::mul (x, x), x), B::set1 (3.0)));
            return B::select (B::ge (x, one), c23, B::select (B::le (x, B::neg (one)), B::neg (c23), mid));
        }

        // AdaaSoftClip::antiderivative1
        static V antiderivativeV (V x) noexcept
        {
            const V one = B::set1 (1.0);
            const V two = B::set1 (2.0);
            const V three = B::set1 (3.0);
            const V quarter = B::set1 (0.25);
            const V x2 = B::mul (x, x);
            const V mid = B::sub (B::mul (x2, B::set1 (0.5)), B::div (B::mul (x2, x2), B::set1 (12.0)));
            const V hi = B::sub (B::div (B::mul (two, x), three), quarter);
            const V lo = B::sub (B::div (B::mul (B::neg (two), x), three), quarter);
            return B::select (B::ge (x, one), hi, B::select (B::le (x, B::neg (one)), lo, mid));
        }

        // AdaaSoftClip::processSample up to its float cast: y for input x0 given the previous input and its antiderivative; `f1x0` is
        // the new antiderivative (the state to keep).
        static V adaaStepV (V x0, V x1, V f1x1, V& f1x0) noexcept
        {
            constexpr double kEps = 1.0e-5; // AdaaSoftClip::kEps
            const V diff = B::sub (x0, x1);
            f1x0 = antiderivativeV (x0);
            const V quotient = B::div (B::sub (f1x0, f1x1), diff);
            const V limit = shapeV (B::mul (B::set1 (0.5), B::add (x0, x1)));
            const auto far = B::orr (B::gt (diff, B::set1 (kEps)), B::lt (diff, B::set1 (-kEps)));
            return B::select (far, quotient, limit);
        }

    private:
        void clearLaneState (int lane) noexcept
        {
            const auto i = static_cast<std::size_t> (lane);
            x1_[i] = 0.0;
            f1x1_[i] = 0.0; // AdaaSoftClip::reset: antiderivative1 (0.0) == 0
        }

        // One OS-rate sample for all lanes: `u` in = the upsampled values rounded to float (as doubles), out = float-exact.
        void shape (V (&u)[kNV]) noexcept
        {
            for (std::size_t v = 0; v < kNV; ++v)
            {
                const std::size_t o = 2 * v;
                const V x0 = B::roundF (B::mul (u[v], B::load (&preGain_[o]))); // the float product u * preGain_
                V f1x0;
                u[v] = B::roundF (adaaStepV (x0, B::load (&x1_[o]), B::load (&f1x1_[o]), f1x0));
                B::store (&x1_[o], x0);
                B::store (&f1x1_[o], f1x0);
            }
        }

        OversamplerLanes<B, N> os_;
        alignas (16) double preGain_[N] {}, x1_[N] {}, f1x1_[N] {};
    };

    // [T-CMP-37 L3] The engine's batch of four output drives, and one voice's seat in it (the ClassicLaneSlot shape).
    using DriveLanes4 = FilterDriveLanes<DefaultBackend, 4>;
    struct DriveLaneSlot
    {
        DriveLanes4* lanes = nullptr;
        int lane = 0;
        bool resident = false;

        void pull (FilterDrive& scalar) noexcept
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
