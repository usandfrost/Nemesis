#include <type_traits>
#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/FastMath.h>
#include <nemesis/dsp/LadderFilter.h>

#include <nemesis/dsp/FilterResonance.h>

#include <algorithm>
#include <cassert>
#include <cmath>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;
        // [T-CMP-31] The resonance ceiling moved to `resonance::ceiling::kLadderMaxK`
        // (FilterResonance.h), with the rationale. It was TU-private here, so nothing
        // outside could read it and no self-osc threshold law could be written at all.

        template <typename T>
        T clampT (T v, typename std::common_type<T>::type lo, typename std::common_type<T>::type hi) noexcept
        {
            return std::min (std::max (v, lo), hi);
        }
    }

    void LadderFilter::prepare (Real sampleRate, int oversampleFactor)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        const int f = clampT (oversampleFactor, 1, kMaxOversample);
        os24_.prepare (f);
        os12_.prepare (f);
        const Real n = kDriveCrossfadeMs * 0.001 * sampleRate_ + 0.5; // round, no libm
        // The glide must rise slower than one sample per sample (see `ringRead`), and the ring must
        // hold the whole latency: the glide reads up to `latency` behind the newest sample, so a
        // latency above kRingMask would wrap onto fresh data. [T-CGUI-81] The cascade's latency is
        // 2 x its half-taps (16) at EVERY factor today; this is the check if that ever grows.
        assert (os24_.latencySamples() <= static_cast<int> (kRingMask) && "the glide history ring must hold the latency");
        xfLen_ = std::max (static_cast<int> (n), os24_.latencySamples() + 1);
        reset();
    }

    void LadderFilter::setDrive (float amount) noexcept
    {
        const Real a = clampT (static_cast<Real> (amount), 0.0, 1.0);
        const bool nowDriven = a > 0.0;
        driveGain_ = 1.0 + kDriveRange * a;
        if (nowDriven == driven_)
            return;
        driven_ = nowDriven;

        // [T-CMP-27] The crossing is a crossfade (see `kDriveCrossfadeMs`), so no state is flushed from
        // a path that is still sounding. The driven path starts clean only when it was NOT running: its
        // oversampler FIR history and lagged excesses would otherwise belong to an old signal epoch
        // (T-CMP-23 S5R f3 — a note that ended seconds ago spliced into silence).
        if (driven_ && !drivenLive_)
            startDrivenPath();

        // A filter that holds no signal has nothing to cross from: the crossing lands at once, so a
        // drive set before the first sample (every fresh voice, every driven golden) is the settled path.
        const bool silent = s1_ == 0.0 && s2_ == 0.0 && s3_ == 0.0 && s4_ == 0.0 && d1_ == 0.0 && d2_ == 0.0
                            && d3_ == 0.0 && d4_ == 0.0;
        if (silent)
        {
            xfPos_ = driven_ ? 2 * xfLen_ : 0;
            drivenLive_ = driven_;
        }
    }

    void LadderFilter::startDrivenPath() noexcept
    {
        os24_.reset();
        os12_.reset();
        fifo_.fill (0.0f);
        fifoFill_ = 0;
        fifoRead_ = 0;
        psi1_ = 0.0;
        psi2_ = 0.0;
        psi3_ = 0.0;
        psi4_ = 0.0;
        // The integrators are the filter's signal memory and mean the same thing at either rate.
        d1_ = s1_;
        d2_ = s2_;
        d3_ = s3_;
        d4_ = s4_;
        drivenLive_ = true;
        design (drv_, static_cast<Real> (os24_.factor()) * sampleRate_);
    }

    int LadderFilter::latencySamples() const noexcept
    {
        return driven_ ? os24_.latencySamples() : 0;
    }

    void LadderFilter::reset() noexcept
    {
        os24_.reset();
        os12_.reset();
        fifo_.fill (0.0f);
        fifoFill_ = 0;
        fifoRead_ = 0;
        psi1_ = 0.0;
        psi2_ = 0.0;
        psi3_ = 0.0;
        psi4_ = 0.0;
        s1_ = 0.0;
        s2_ = 0.0;
        s3_ = 0.0;
        s4_ = 0.0;
        d1_ = 0.0;
        d2_ = 0.0;
        d3_ = 0.0;
        d4_ = 0.0;
        ring24_.fill (0.0f);
        ring12_.fill (0.0f);
        ringPos_ = 0;
        // A reset lands any crossing on its target: nothing is sounding to cross from.
        xfPos_ = driven_ ? 2 * xfLen_ : 0;
        if (driven_ && !drivenLive_)
            design (drv_, static_cast<Real> (os24_.factor()) * sampleRate_);
        drivenLive_ = driven_;
    }

    void LadderFilter::setCutoff (float cutoffHz, float resonance) noexcept
    {
        cutoffHz_ = cutoffHz;
        reso_ = resonance;
        const Real res = clampT (static_cast<Real> (resonance), 0.0, 1.0);
        k_ = resonance::ceiling::feedbackK (resonance::ceiling::kLadderMaxK, res);

        // Each core is designed at the rate it runs at (filter.md §4 "g is computed at the OS rate, so
        // the analog cutoff is matched across factors"): the linear one always (it runs on through a
        // crossing), the driven one while it is live.
        design (lin_, sampleRate_);
        if (drivenLive_)
            design (drv_, static_cast<Real> (os24_.factor()) * sampleRate_);
    }

    void LadderFilter::design (Coeffs& c, Real coreRate) const noexcept
    {
        // Keep the prewarped cutoff strictly below Nyquist so tan() stays bounded.
        //
        // ⚠ [S5R f9] THIS GUARD IS FOUR TIMES WIDER FOR THE DRIVEN CORE, and that is a second behaviour
        // the drive predicate switches — named here because the card sells the predicate as "verbatim
        // linear vs. oversampled nonlinear" and this is neither. Undriven the clamp is 0.49*fs (23,520 Hz
        // at 48 k); driven it is 0.49*4*fs (94,080 Hz). A request ABOVE the undriven ceiling therefore
        // designs two different filters across a flip: at 80 kHz the undriven core clamps to 23,520
        // (g ~ 0.97) while the driven one designs g ~ 0.79. Both call sites can reach there — CATHODE
        // through keytrack + envelope, FltrEffect through `envCutoffHz`'s exponential octaves.
        // Left as-is deliberately: the driven arm is the more correct of the two (it is the one NOT
        // clamping an audible request away), and equalising them would mean clamping the driven core to
        // a limit its own rate does not have. The audible difference sits above 23 kHz either way.
        const Real nyquistGuard = 0.49 * coreRate;
        const Real fc = clampT (static_cast<Real> (cutoffHz_), 1.0, nyquistGuard);

        const Real gTan = fm::tan (kPi * fc / coreRate);
        c.g = gTan / (1.0 + gTan); // TPT one-pole coefficient G
        c.oneMinusG = 1.0 - c.g;
        c.g2 = c.g * c.g;
        c.g3 = c.g2 * c.g;
        c.g4 = c.g3 * c.g;
        c.gComp = 1.0 / (1.0 + k_ * c.g4);
    }
}
