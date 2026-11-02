#include <type_traits>
#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/FastMath.h>
#include <nemesis/dsp/ClassicFilter.h>

#include <nemesis/dsp/FilterResonance.h>

#include <algorithm>
#include <cmath>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;

        // [T-CMP-31] The resonance ceiling moved to `resonance::ceiling::kClassicMaxK`
        // (FilterResonance.h), with the rationale. It was TU-private here, so nothing
        // outside could read it and no self-osc threshold law could be written at all.

        // [b50 NaN] STABILITY BOUND OF THE LAGGED CLIP. psi_i is seeded from the PREVIOUS OS sample, so the
        // nonlinear correction is EXPLICIT; in saturation dpsi/dc = 1 and, fed back through k*S, it drives an
        // OS-Nyquist mode whose growth passes 1 at G ~ 1/sqrt(1+k) (linearised map, every per-cell slope in
        // [0,1]: exactly 1/sqrt(2) at k=1, above it elsewhere). The engine's deep cutoff mod reached G 0.97 at
        // k 2.2 and went non-finite. Capping G at 1/sqrt(1 + 1.5k) keeps that mode's growth <= ~0.97 for every
        // k; k=0 has no loop and keeps the plain guard. The cap only bites above ~0.23*fsOs (44 kHz at 4x/48k,
        // k 2.2); a cutoff under the cap is bit-identical to before. ⚠ At oversampling factor 1 the same cap lands
        // near 11.4 kHz (k 2.2, 48k) — audible — so D-M-0573's plan to run low drive without oversampling must account
        // for it (re-derive the bound for that path, or keep the oversampler wherever the cap would bite).
        constexpr Real kLagStabilityK = 1.5;

        template <typename T>
        T clampT (T v, typename std::common_type<T>::type lo, typename std::common_type<T>::type hi) noexcept
        {
            return std::min (std::max (v, lo), hi);
        }
    }

    void ClassicFilter::prepare (Real sampleRate, int oversampleFactor)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        os_.prepare (oversampleFactor);
        reset();
    }

    void ClassicFilter::reset() noexcept
    {
        s1_ = 0.0;
        s2_ = 0.0;
        s3_ = 0.0;
        s4_ = 0.0;
        uEst_ = 0.0;
        y1Est_ = 0.0;
        y2Est_ = 0.0;
        y3Est_ = 0.0;
        os_.reset();
    }

    void ClassicFilter::setCutoff (float cutoffHz, float resonance) noexcept
    {
        // Coefficients are computed at the OS rate (the core runs inside the oversampler), so the analog
        // cutoff is matched across oversampling factors. Keep the prewarped cutoff below the OS Nyquist.
        // Unlike the odd-order AcidFilter, a 4-pole cascade's resonant peak sits AT the pole corner
        // (4*-45 deg = -180 deg at cutoff), so no sqrt(3)-style peak-tracking scale is needed.
        const Real fsOs = sampleRate_ * static_cast<Real> (os_.factor());
        const Real nyquistGuard = 0.49 * fsOs;
        const Real fc = clampT (static_cast<Real> (cutoffHz), 1.0, nyquistGuard);
        const Real res = clampT (static_cast<Real> (resonance), 0.0, 1.0);

        const Real gTan = fm::tan (kPi * fc / fsOs);
        if constexpr (fork::kExact) // FIREFORK E3b: same expressions, evaluated only when the resonance moves
        {
            if (res != cachedRes_)
            {
                cachedRes_ = res;
                k_ = resonance::ceiling::feedbackK (resonance::ceiling::kClassicMaxK, res);
                cachedGStable_ = 1.0 / std::sqrt (1.0 + kLagStabilityK * k_);
                compGain_ = 1.0 + k_;
                invComp_ = 1.0 / compGain_;
            }
            g_ = std::min (gTan / (1.0 + gTan), cachedGStable_);
            oneMinusG_ = 1.0 - g_;
            g2_ = g_ * g_;
            g3_ = g2_ * g_;
            g4_ = g3_ * g_;
            gComp_ = 1.0 / (1.0 + k_ * g4_);
            ++coefGen_;
            return;
        }
        k_ = resonance::ceiling::feedbackK (resonance::ceiling::kClassicMaxK, res);
        const Real gStable = 1.0 / std::sqrt (1.0 + kLagStabilityK * k_); // see kLagStabilityK
        g_ = std::min (gTan / (1.0 + gTan), gStable); // TPT one-pole coefficient G
        oneMinusG_ = 1.0 - g_;
        g2_ = g_ * g_;
        g3_ = g2_ * g_;
        g4_ = g3_ * g_;
        compGain_ = 1.0 + k_; // resonance compensation -> linear DC gain 1 for all reso (Curtis signature)
        invComp_ = 1.0 / compGain_; // passband normaliser for the BP/HP mode-mix taps
        gComp_ = 1.0 / (1.0 + k_ * g4_);
        ++coefGen_;
    }
}
