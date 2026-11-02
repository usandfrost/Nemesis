#include <type_traits>
#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/FastMath.h>
#include <nemesis/dsp/AcidFilter.h>

#include <nemesis/dsp/FilterResonance.h>

#include <algorithm>
#include <cmath>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;

        // [T-CMP-31] The resonance ceiling moved to `resonance::ceiling::kAcidMaxK`
        // (FilterResonance.h), with the rationale. It was TU-private here, so nothing
        // outside could read it and no self-osc threshold law could be written at all.

        // A 3-pole ladder's resonant peak (loop phase = -180 deg) sits where each one-pole hits -60 deg,
        // i.e. at f = sqrt(3)*f_pole. So to make the resonant peak track the SET cutoff (musically correct
        // — a 303's cutoff knob sweeps the peak), the internal one-pole corner is placed at fc/sqrt(3).
        // The asymptotic roll-off stays 18 dB/oct (three poles), the distinguishing 303 slope vs the Moog
        // 4-pole "Ladder" (24 dB). (The 4-pole peaks at fc for free — 4*45 deg = 180 — hence no scaling there.)
        constexpr Real kPeakScale = 1.7320508075688772; // sqrt(3) = tan(60 deg)

        template <typename T>
        T clampT (T v, typename std::common_type<T>::type lo, typename std::common_type<T>::type hi) noexcept
        {
            return std::min (std::max (v, lo), hi);
        }
    }

    void AcidFilter::prepare (Real sampleRate, int oversampleFactor)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        os_.prepare (oversampleFactor);
        reset();
    }

    void AcidFilter::reset() noexcept
    {
        s1_ = 0.0;
        s2_ = 0.0;
        s3_ = 0.0;
        y3Est_ = 0.0;
        os_.reset();
    }

    void AcidFilter::setCutoff (float cutoffHz, float resonance) noexcept
    {
        // Coefficients are computed at the OS rate (the core runs inside the oversampler), so the analog
        // cutoff is matched across oversampling factors. Keep the prewarped cutoff below the OS Nyquist.
        const Real fsOs = sampleRate_ * static_cast<Real> (os_.factor());
        const Real nyquistGuard = 0.49 * fsOs;
        const Real fc = clampT (static_cast<Real> (cutoffHz), 1.0, nyquistGuard);
        const Real res = clampT (static_cast<Real> (resonance), 0.0, 1.0);

        // Place the one-pole corner at fc/sqrt(3) so the resonant peak lands ON the set cutoff (see note).
        const Real fPole = fc / kPeakScale;
        const Real gTan = fm::tan (kPi * fPole / fsOs);
        g_ = gTan / (1.0 + gTan); // TPT one-pole coefficient G
        oneMinusG_ = 1.0 - g_;
        g2_ = g_ * g_;
        g3_ = g2_ * g_;
        k_ = resonance::ceiling::feedbackK (resonance::ceiling::kAcidMaxK, res);
        gComp_ = 1.0 / (1.0 + k_ * g3_);
    }
}
