#include <type_traits>
#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/FastMath.h>
#include <nemesis/dsp/ReedFilter.h>

#include <nemesis/dsp/FilterResonance.h>

#include <algorithm>
#include <cmath>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;

        // [T-CMP-31] The resonance ceiling moved to `resonance::ceiling::kReedDampMin`
        // (FilterResonance.h), with the rationale. It was TU-private here, so nothing
        // outside could read it and no self-osc threshold law could be written at all.

        template <typename T>
        T clampT (T v, typename std::common_type<T>::type lo, typename std::common_type<T>::type hi) noexcept
        {
            return std::min (std::max (v, lo), hi);
        }
    }

    void ReedFilter::prepare (Real sampleRate, int oversampleFactor)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        os_.prepare (oversampleFactor);
        reset();
    }

    void ReedFilter::reset() noexcept
    {
        s1_ = 0.0;
        s2_ = 0.0;
        bpEst_ = 0.0;
        os_.reset();
    }

    void ReedFilter::setCutoff (float cutoffHz, float resonance) noexcept
    {
        // Coefficients are computed at the OS rate (the core runs inside the oversampler), so the analog
        // cutoff is matched across oversampling factors. Keep the prewarped cutoff below the OS Nyquist.
        const Real fsOs = sampleRate_ * static_cast<Real> (os_.factor());
        const Real nyquistGuard = 0.49 * fsOs;
        const Real fc = clampT (static_cast<Real> (cutoffHz), 1.0, nyquistGuard);
        const Real reso = clampT (static_cast<Real> (resonance), 0.0, 1.0);

        const Real g = fm::tan (kPi * fc / fsOs);
        const Real k = resonance::ceiling::dampingK (resonance::ceiling::kReedDampMin, reso); // damping 1/Q

        // Cytomic resolved zero-delay SVF coefficients: a1 = 1/(1 + g*(g + k)), a2 = g*a1, a3 = g*a2.
        const Real a1 = 1.0 / (1.0 + g * (g + k));
        k_ = k;
        a1_ = a1;
        a2_ = g * a1;
        a3_ = g * a2_;
    }
}
