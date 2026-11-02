#include <type_traits>
#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/FastMath.h>
#include <nemesis/dsp/Svf.h>

#include <algorithm>
#include <cmath>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;

        template <typename T>
        T clampT (T v, typename std::common_type<T>::type lo, typename std::common_type<T>::type hi) noexcept
        {
            return std::min (std::max (v, lo), hi);
        }
    }

    void TptSvf::prepare (Real sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        reset();
    }

    void TptSvf::reset() noexcept
    {
        ic1eq_ = 0.0;
        ic2eq_ = 0.0;
    }

    void TptSvf::setCutoff (float cutoffHz, float q) noexcept
    {
        // Keep the prewarped cutoff strictly below Nyquist so tan() stays bounded,
        // and the damping term k = 1/q finite for any requested resonance.
        const Real nyquistGuard = 0.49 * sampleRate_;
        const Real fc = clampT (static_cast<Real> (cutoffHz), 1.0, nyquistGuard);
        const Real safeQ = clampT (static_cast<Real> (q), 1.0e-3, 1.0e4);

        g_ = fm::tan (kPi * fc / sampleRate_);
        k_ = 1.0 / safeQ;
        a1_ = 1.0 / (1.0 + g_ * (g_ + k_));
        a2_ = g_ * a1_;
        a3_ = g_ * a2_;
    }
}
