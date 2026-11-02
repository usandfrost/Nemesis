#pragma once

#include <nemesis/dsp/Real.h>
#include <cmath>

namespace nemesis::dsp
{
    // [T-CGUI-64] THE SHARED §0 WAVEFOLDER (`specs/dsp/wavefolder.md` §0) — a triangle (reflective,
    // Serge-like) fold at a fixed ±1 threshold, anti-aliased by FIRST-ORDER ADAA and nothing else (no
    // oversampler, owner 2026-09-27). Family-scoped: CATHODE's `synth.fold.*` is the one consumer built;
    // MAGMA's Foldback and BOMBA's pad fold cite the same law.
    //
    //   u = g·x,  f(u) = 1 − |mod4(u + 1) − 2|        identity on |u| ≤ 1, reflected at every odd integer
    //   F(u) = (t − 1)²/2 on t ∈ [0,2),  1 − (t − 3)²/2 on t ∈ [2,4),  t = mod4(u + 1)
    //
    // F is PERIODIC (f has zero mean over its period 4) and bounded in [0, 1], so the ADAA divided
    // difference never loses precision to a growing antiderivative however hard the input is driven.
    // ADAA1's group delay is half a sample on the wet leg; it is stated, not compensated (§0.2).
    //
    // AMOUNT LAW (§0.3): g = 1 + 7a (×1…×8), wet mix m = min(1, a / 0.05), y = x + m·(fold − x). a = 0 is
    // a BIT-EXACT bypass — the input is returned untouched and the ADAA memory is re-primed, so the first
    // live sample takes the ill-conditioned branch rather than a stale u[n−1].
    //
    // RT-safe, deterministic, no JUCE, no allocation; the hot path is arithmetic plus one `floor`.
    class Wavefolder
    {
    public:
        static constexpr float kMaxGain = 8.0f; // the pre-gain at amount 1
        static constexpr float kCrossfadeAmount = 0.05f; // the bottom of travel the dry crossfade spans

        // Forgets u[n−1]: the next live sample is evaluated at its own point (no stale divided difference).
        void reset() noexcept { primed_ = false; }

        // The pre-gain for an amount in [0, 1].
        [[nodiscard]] static float gainFor (float amount) noexcept { return 1.0f + (kMaxGain - 1.0f) * amount; }
        // The wet mix for an amount in [0, 1]: a linear ramp over the bottom `kCrossfadeAmount`, then 1.
        [[nodiscard]] static float wetMixFor (float amount) noexcept
        {
            return amount >= kCrossfadeAmount ? 1.0f : amount / kCrossfadeAmount;
        }

        // One sample. `amount` in [0, 1] (clamped); `mixScale` in [0, 1] scales the wet mix — a consumer's
        // own fade (CATHODE's Pre/Post switch). amount 0 (or mixScale 0) returns `x` bit for bit.
        float processSample (float x, float amount, float mixScale = 1.0f) noexcept
        {
            const float a = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
            const float m = wetMixFor (a) * mixScale;
            if (!(m > 0.0f))
            {
                primed_ = false;
                return x;
            }
            const float wet = fold (static_cast<Real> (gainFor (a)) * static_cast<Real> (x));
            if (m >= 1.0f)
                return wet;
            return x + m * (wet - x);
        }

        // The ADAA fold of an already pre-gained `u` (the primitive under the amount law).
        float fold (Real u) noexcept
        {
            const Real fu = antiderivative (u);
            if (!primed_)
            {
                u1_ = u;
                f1_ = fu;
                primed_ = true;
            }
            const Real diff = u - u1_;
            const Real y = (diff > kEps || diff < -kEps) ? (fu - f1_) / diff : shape (0.5 * (u + u1_));
            u1_ = u;
            f1_ = fu;
            return static_cast<float> (y);
        }

        // The memoryless triangle fold f(u) — exposed as the naive (aliasing) reference and for the law tests.
        [[nodiscard]] static Real shape (Real u) noexcept
        {
            const Real t = wrap4 (u + 1.0);
            return 1.0 - std::abs (t - 2.0);
        }

        // Its antiderivative F(u), F(0) = 0, period 4.
        [[nodiscard]] static Real antiderivative (Real u) noexcept
        {
            const Real t = wrap4 (u + 1.0);
            if (t < 2.0)
            {
                const Real d = t - 1.0;
                return 0.5 * d * d;
            }
            const Real d = t - 3.0;
            return 1.0 - 0.5 * d * d;
        }

    private:
        // Floored modulo 4, result in [0, 4).
        [[nodiscard]] static Real wrap4 (Real v) noexcept
        {
            const Real t = v - 4.0 * std::floor (v * 0.25);
            return t >= 4.0 ? t - 4.0 : t;
        }

        // Divided-difference ill-conditioning guard (the `AdaaSoftClip` constant): below this |u − u1| the
        // analytic midpoint limit f((u + u1)/2) replaces the quotient.
        static constexpr Real kEps = 1.0e-5;

        Real u1_ = 0.0;
        Real f1_ = 0.0;
        bool primed_ = false;
    };
}
