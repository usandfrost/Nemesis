#include <type_traits>
#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/FastMath.h>
#include <nemesis/dsp/ScreamFilter.h>

#include <nemesis/dsp/FilterResonance.h>

#include <algorithm>
#include <cmath>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;

        // [T-CMP-31] The resonance ceiling moved to `resonance::ceiling::kScreamMaxR`
        // (FilterResonance.h), with the rationale. It was TU-private here, so nothing
        // outside could read it and no self-osc threshold law could be written at all.

        // [b50 NaN] STABILITY BOUND OF THE LAGGED CLIP — the ClassicFilter defect, same idiom. psi is seeded
        // from the PREVIOUS OS sample's VC2, so in saturation (dpsi/dVC2 = 1) the r*psi feedback is explicit and
        // its OS-Nyquist mode grows once (1+g)^2 > r/(r-1) (linearised map, matched to 3 places at r 1.2/1.5/1.8;
        // r <= 1 never crosses). Capping (1+g)^2 at 1 + 1/(kLagStabilityR*(r-1)) keeps a margin; it bites only at
        // high reso (r 1.98: above 0.110*fsOs, 19.4 kHz at 4x/44.1k); any cutoff under the cap is bit-identical.
        constexpr Real kLagStabilityR = 1.2;

        template <typename T>
        T clampT (T v, typename std::common_type<T>::type lo, typename std::common_type<T>::type hi) noexcept
        {
            return std::min (std::max (v, lo), hi);
        }
    }

    void ScreamFilter::prepare (Real sampleRate, int oversampleFactor)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        os_.prepare (oversampleFactor);
        reset();
    }

    void ScreamFilter::reset() noexcept
    {
        s1_ = 0.0;
        s2_ = 0.0;
        vc2Est_ = 0.0;
        os_.reset();
    }

    void ScreamFilter::setCutoff (float cutoffHz, float resonance) noexcept
    {
        // Coefficients are computed at the OS rate (the core runs inside the oversampler), so the analog
        // cutoff is matched across oversampling factors. Keep the prewarped cutoff below the OS Nyquist.
        const Real fsOs = sampleRate_ * static_cast<Real> (os_.factor());
        const Real nyquistGuard = 0.49 * fsOs;
        const Real fc = clampT (static_cast<Real> (cutoffHz), 1.0, nyquistGuard);
        const Real reso = clampT (static_cast<Real> (resonance), 0.0, 1.0);

        const Real r = resonance::ceiling::feedbackK (resonance::ceiling::kScreamMaxR, reso); // r = K - 1 in [0, ~2)
        const Real gTan = fm::tan (kPi * fc / fsOs);
        Real g = gTan;
        if constexpr (fork::kExact) // FIREFORK E3c: the same limit, recomputed only when r moves
        {
            if (r > 1.0)
            {
                if (r != cachedR_)
                {
                    cachedR_ = r;
                    cachedGLimit_ = std::sqrt (1.0 + 1.0 / (kLagStabilityR * (r - 1.0))) - 1.0;
                }
                g = std::min (gTan, cachedGLimit_);
            }
        }
        else
            g = r > 1.0 ? std::min (gTan, std::sqrt (1.0 + 1.0 / (kLagStabilityR * (r - 1.0))) - 1.0) : gTan;

        g_ = g;
        r_ = r;
        gr_ = g * r;
        twoGR_ = 2.0 * g * r;
        oneMinusGR_ = 1.0 - g * r;
        gOnePlus2r_ = g * (1.0 + 2.0 * r);
        onePlus2g_ = 1.0 + 2.0 * g;
        invDet_ = 1.0 / ((1.0 + g) * (1.0 + g) - g * r); // det = (1+g)^2 - g*r > 0 for r < 2
        damp_ = 2.0 - r; // 3 - K, the HP tap coefficient
    }
}
