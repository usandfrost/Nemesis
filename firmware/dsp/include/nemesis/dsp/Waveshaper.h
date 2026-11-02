#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ForkConfig.h>
#include <array>
#include <cmath>

namespace nemesis::dsp
{
    // First-order ADAA (antiderivative anti-aliasing) cubic soft-clip waveshaper — the
    // shared drive/saturation §0.3 nonlinearity (drive.md §0.2-0.3). A memoryless
    // waveshaper folds the harmonics it generates back as inharmonic aliasing; ADAA
    // replaces the point evaluation f(x) with the average of f over the sample interval,
    // i.e. the divided difference of its antiderivative F1 — a built-in low-pass on the
    // generated harmonics that drops the alias floor without oversampling. This is the
    // keystone (cleanpath_build_plan W1a): consumers can drop the OS factor because the
    // primitive carries the floor. RT-safe, deterministic, no JUCE, no allocation, no libm
    // in the hot path (the curve and its antiderivative are pure polynomials/lines).
    //
    // Method: S. Parker, V. Zavalishin, E. Le Bivic, "Reducing the Aliasing of Nonlinear
    // Waveshaping Using Continuous-Time Convolution" (DAFx-16), first-order case.
    //
    // Curve: cubic soft clip  f(x) = x - x^3/3 for |x| < 1, clamped to +-2/3 beyond — an
    // odd (DC-free) polynomial with a C1 knee; the constant clamp region is where high
    // harmonics (hence aliasing) are generated. Asymmetric/hard/near-square/Chebyshev
    // curves and second-order ADAA are separate follow-on cards.
    class AdaaSoftClip
    {
    public:
        // Clears the one-sample memory (x[n-1] and its cached antiderivative). Call before
        // use; there is no sample-rate state (ADAA works in the sample domain).
        void reset() noexcept
        {
            x1_ = 0.0;
            f1x1_ = antiderivative1 (0.0); // == 0
        }

        // Processes one sample. Hot path: inline, pure arithmetic (no libm) so it inlines
        // into callers. When x is far from x[n-1] the output is the divided difference of
        // F1; when x ~= x[n-1] that quotient is ill-conditioned, so fall back to the
        // l'Hopital limit f((x + x1)/2). Computed in double, emitted as float.
        float processSample (float x) noexcept
        {
            const Real x0 = static_cast<Real> (x);
            const Real diff = x0 - x1_;
            const Real f1x0 = antiderivative1 (x0);

            Real y;
            if (fork::kApprox && x0 < 1.0 && x0 > -1.0 && x1_ < 1.0 && x1_ > -1.0) // FIREFORK N2: the closed form inside the knee
                y = (x0 + x1_) * (0.5 - (x0 * x0 + x1_ * x1_) * (1.0 / 12.0));
            else if (diff > kEps || diff < -kEps)
                y = (f1x0 - f1x1_) / diff;
            else
                y = shape (0.5 * (x0 + x1_));

            x1_ = x0;
            f1x1_ = f1x0;
            return static_cast<float> (y);
        }

        // [T-CMP-37 L3] The two ADAA doubles (x[n-1], F1 (x[n-1])) as values, for the lane-wise drive to take in and give back.
        [[nodiscard]] std::array<Real, 2> state() const noexcept { return { x1_, f1x1_ }; }
        void setState (const std::array<Real, 2>& s) noexcept
        {
            x1_ = s[0];
            f1x1_ = s[1];
        }

        // Memoryless cubic soft clip f(x). Exposed for reuse and for tests to build the
        // naive (aliasing) reference the ADAA path is measured against.
        static Real shape (Real x) noexcept
        {
            if (x >= 1.0)
                return 2.0 / 3.0;
            if (x <= -1.0)
                return -2.0 / 3.0;
            if constexpr (fork::kApprox) // FIREFORK N2
                return x - x * x * x * (1.0 / 3.0);
            return x - x * x * x / 3.0;
        }

        // First antiderivative F1 of the cubic soft clip (F1(0) = 0), the integral the
        // ADAA divided difference draws on. Even function (f is odd).
        static Real antiderivative1 (Real x) noexcept
        {
            if constexpr (fork::kApprox) // FIREFORK N2
            {
                if (x >= 1.0)
                    return x * (2.0 / 3.0) - 0.25;
                if (x <= -1.0)
                    return -x * (2.0 / 3.0) - 0.25;
                const Real x2 = x * x;
                return x2 * 0.5 - x2 * x2 * (1.0 / 12.0);
            }
            if (x >= 1.0)
                return 2.0 * x / 3.0 - 0.25;
            if (x <= -1.0)
                return -2.0 * x / 3.0 - 0.25;
            const Real x2 = x * x;
            return x2 * 0.5 - x2 * x2 / 12.0;
        }

    private:
        // Divided-difference ill-conditioning guard: below this |x - x1| we take the
        // analytic midpoint limit instead of dividing by a near-zero denominator.
        static constexpr Real kEps = 1.0e-5;

        Real x1_ = 0.0; // previous input x[n-1]
        Real f1x1_ = 0.0; // cached F1(x[n-1]) to avoid recomputing the antiderivative
    };

    // [D-M-0571 S5R] The in-loop drive's BOUND, faded in with the drive: Scream/Acid/Soviet/Classic/Reed (setLoopDrive)
    // and the bank's ceiling reference take the driven input u·g as `apply (u·g, mixFor (g))`. Switched on at the first
    // non-zero drive, the bound was a step (a hot input hard-limited to +-2/3 by drive 1/127). Mix 0 at g = 1 returns the
    // driven input itself (drive 0 bit-exact); from g = 1 + kFadeGain on it returns `AdaaSoftClip::shape` exactly.
    struct LoopDriveBound
    {
        static constexpr Real kFadeGain = 1.5; // fully bounded at pre-gain 2.5: drive 0.25 on the 1 + 6d cores

        [[nodiscard]] static float mixFor (Real driveGain) noexcept
        {
            const Real m = (driveGain - 1.0) / kFadeGain;
            if (m <= 0.0)
                return 0.0f;
            return m >= 1.0 ? 1.0f : static_cast<float> (m);
        }

        [[nodiscard]] static Real apply (Real driven, float mix) noexcept
        {
            if (mix <= 0.0f)
                return driven;
            const Real bounded = AdaaSoftClip::shape (driven);
            return mix >= 1.0f ? bounded : driven + static_cast<Real> (mix) * (bounded - driven);
        }
    };

    // [T-FX-12 leaf 1] The parameterised ADAA clip FAMILY — the follow-on the class above
    // has named since M1-02 ("Asymmetric/hard/near-square … are separate follow-on cards"),
    // and the ONLY new DSP `specs/dsp/drive.md` §9.3 says the `DRV` type needs: "one
    // parameterised ADAA curve family (each curve needs its antiderivative F1 in closed
    // form, which is why it is real work and not a `std::tanh` call)".
    //
    // TWO AXES, and they are genuinely different things:
    //
    //   HARDNESS `p` (an integer) — the odd power-law knee
    //       g(x) = x − x·|x|^p/(p+1)      for |x| < 1
    //            = sign(x)·p/(p+1)        beyond,
    //   whose derivative is exactly `1 − |x|^p`: unity at 0, ZERO at |x| = 1, so the curve
    //   is C1 at the knee for every p and monotone in between. `p = 2` IS `AdaaSoftClip`'s
    //   `x − x³/3` clamped at ±2/3, so the family CONTAINS the shipped curve rather than
    //   approximating it — the two statics below DELEGATE at p = 2, which makes that
    //   bit-exact by construction and not by luck (drive.md §10.4: "AdaaSoftClip is already
    //   exactly the tape case … Saturate needs no new DSP at all", a sentence a test pins).
    //   Larger p keeps the transfer linear for longer and then turns over abruptly — §0.3's
    //   "hard clip with a soft knee" (Distortion, §2.1/§10.2) and, cascaded, its
    //   "near-square / staircase" (Fuzz, §3.2/§10.3). p → ∞ is the pure `clamp(x, −1, 1)`.
    //   F1 for the polynomial region is `x²/2 − |x|^(p+2)/((p+1)(p+2))` (even, as it must be
    //   for an odd f) and a straight line of slope `p/(p+1)` beyond the knee.
    //
    //   PER-SIDE KNEE SCALE `s` — the ASYMMETRY, `f(x) = s·g(x/s)` with its own `s` for each
    //   half, so `F(x) = s²·G(x/s)` per half (both halves give F(0) = 0, so F is continuous
    //   and the ADAA divided difference stays valid across zero). This is drive.md's
    //   OI-DRV-4 ruling — per-half knee scaling, i.e. the Boss OD-1's two-diodes-against-one
    //   clipper, where the two halves clip at different THRESHOLDS rather than the operating
    //   point being shifted. Why the distinction is load-bearing: a DC offset into an ODD
    //   curve makes even harmonics of IDENTICAL magnitude for +b and −b, so a bipolar `bias`
    //   row's sign would be inaudible on a symmetric input — half the travel decoration.
    //   Knee scaling instead keeps `f'(0) = 1` on BOTH sides (small-signal identity: a quiet
    //   passage passes through untouched however hard the knees are pulled in) while making
    //   the sign a real circuit choice. `s < 1` clips that half EARLIER.
    //
    //   The two axes do NOT substitute for each other, which is the reason both exist:
    //   `s·g(x/s)` is the same curve RESCALED (self-similar), so a knee scale can make a
    //   drive louder or quieter but never harder — only `p` moves the knee's sharpness.
    //
    // Hardness is an INTEGER on purpose: `|x|^p` is then a handful of multiplies (a loop),
    // so the hot path stays pure arithmetic and libm-free like the class above, and the
    // header needs no .cpp. A fractional/continuous hardness would put `std::pow` on the
    // per-sample path at OS rate — the M1-05 sanctioned-deviation category, but far more
    // expensive than the effect is worth, and no shipped row asks for it (drive.md gives
    // hardness no row at all: it is a per-TYPE constant, §9.2's dispatch).
    //
    // ASYMMETRY INJECTS DC (§0.3, §7: "Always DC-block after any asymmetric stage") — that
    // blocker is the CONSUMER's, not this primitive's, because it is a base-rate stage
    // outside the oversampler while this runs inside it (`DriveEffect` owns it, gated on the
    // bias being nonzero so a symmetric setting stays bit-exact).
    //
    // RT-safe, deterministic, no JUCE, zero allocation, no libm in the hot path. Memoryless
    // apart from ADAA's one-sample history, which `reset()` clears.
    class AsymAdaaClip
    {
    public:
        // Hardness bounds. 1 is the softest usable knee (a quadratic clip); 8 is squarer
        // than any of drive.md's four analog types needs (Fuzz reaches its near-square by
        // CASCADING stages, §10.3's Big Muff architecture, not by a single huge exponent).
        static constexpr int kMinHardness = 1;
        static constexpr int kMaxHardness = 8;
        // The knee scale floor: at 0.05 the half in question clips at 5 % of full scale,
        // which is far past any musical asymmetry, and it keeps the `x/s` division bounded.
        static constexpr float kMinKneeScale = 0.05f;

        // Clears the one-sample ADAA memory (x[n-1] and its cached antiderivative). Call
        // before use. There is no sample-rate state — ADAA works in the sample domain, so
        // like `AdaaSoftClip` this primitive is fs-independent.
        void reset() noexcept
        {
            x1_ = 0.0;
            f1x1_ = antiderivative1Sided (0.0, hardness_, sPos_, sNeg_); // == 0
        }

        // Shifts the ADAA history by `dx` in INPUT space and re-evaluates its cached antiderivative
        // on the CURRENT curve: the consumer moved an offset it adds to the input live, so the
        // previous sample must move with it or the next divided difference spans the two operating
        // points. Call after `setHardness` / `setKneeScales`, which also leave the cache stale.
        void rebase (Real dx) noexcept
        {
            x1_ += dx;
            f1x1_ = antiderivative1Sided (x1_, hardness_, sPos_, sNeg_);
        }

        // Knee sharpness, clamped to [kMinHardness, kMaxHardness]. Control-rate (an int
        // store) but safe anywhere: it changes the curve, not the state, so a live edit is
        // just the next sample taking a different exponent.
        void setHardness (int p) noexcept
        {
            hardness_ = p < kMinHardness ? kMinHardness : (p > kMaxHardness ? kMaxHardness : p);
        }

        // The two halves' knee scales in (0, 1]: 1 = that half clips at the family's own
        // knee, smaller = it clips EARLIER. Equal values are a symmetric (odd) curve.
        void setKneeScales (float sPos, float sNeg) noexcept
        {
            sPos_ = clampKnee (sPos);
            sNeg_ = clampKnee (sNeg);
        }

        [[nodiscard]] int hardness() const noexcept { return hardness_; }
        [[nodiscard]] float kneeScalePositive() const noexcept { return sPos_; }
        [[nodiscard]] float kneeScaleNegative() const noexcept { return sNeg_; }

        // Processes one sample — first-order ADAA over the two-sided curve, structurally
        // identical to `AdaaSoftClip::processSample` (same divided difference, same kEps
        // ill-conditioning guard, same l'Hopital midpoint fallback), which is what makes the
        // p = 2 / symmetric case bit-identical to it. Inline, pure arithmetic.
        float processSample (float x) noexcept
        {
            const Real x0 = static_cast<Real> (x);
            const Real diff = x0 - x1_;
            const Real f1x0 = antiderivative1Sided (x0, hardness_, sPos_, sNeg_);

            Real y;
            if (diff > kEps || diff < -kEps)
                y = (f1x0 - f1x1_) / diff;
            else
                y = shapeSided (0.5 * (x0 + x1_), hardness_, sPos_, sNeg_);

            x1_ = x0;
            f1x1_ = f1x0;
            return static_cast<float> (y);
        }

        // The SYMMETRIC member of the family at hardness p. Exposed for the same two reasons
        // `AdaaSoftClip::shape` is: a naive (aliasing) reference for the alias-floor test,
        // and a law a consumer's faithfulness keystone can call directly.
        static Real shape (Real x, int p) noexcept
        {
            if (p == 2)
                return AdaaSoftClip::shape (x); // the shipped curve, by construction
            const Real ax = x < 0.0 ? -x : x;
            const Real pd = static_cast<Real> (p);
            if (ax >= 1.0)
                return x < 0.0 ? -pd / (pd + 1.0) : pd / (pd + 1.0);
            return x - x * intPow (ax, p) / (pd + 1.0);
        }

        // First antiderivative of `shape` with F1(0) = 0 — even, as it must be for an odd f.
        static Real antiderivative1 (Real x, int p) noexcept
        {
            if (p == 2)
                return AdaaSoftClip::antiderivative1 (x); // the shipped curve, again
            const Real ax = x < 0.0 ? -x : x;
            const Real pd = static_cast<Real> (p);
            if (ax >= 1.0)
                // Written as `p·|x|/(p+1) − c` (not `F1(1) + slope·(|x|−1)`) so it matches
                // the sibling's algebraic form; `c` is what makes it continuous at the knee.
                return pd * ax / (pd + 1.0) - clampOffset (p);
            const Real x2 = x * x;
            return x2 * 0.5 - intPow (ax, p) * x2 / ((pd + 1.0) * (pd + 2.0));
        }

        // The TWO-SIDED curve: `s·g(x/s)` with `s` chosen by the sign of x. `s == 1` returns
        // the symmetric curve unchanged (an exact early return, so a symmetric setting is
        // bit-identical to `shape` and — at p = 2 — to `AdaaSoftClip::shape`).
        static Real shapeSided (Real x, int p, float sPos, float sNeg) noexcept
        {
            const Real s = static_cast<Real> (x < 0.0 ? sNeg : sPos);
            if (s == 1.0)
                return shape (x, p);
            return s * shape (x / s, p);
        }

        // …and its antiderivative, `s²·G(x/s)`. Both halves vanish at 0, so F is continuous
        // there and the ADAA divided difference is valid across a zero crossing.
        static Real antiderivative1Sided (Real x, int p, float sPos, float sNeg) noexcept
        {
            const Real s = static_cast<Real> (x < 0.0 ? sNeg : sPos);
            if (s == 1.0)
                return antiderivative1 (x, p);
            return s * s * antiderivative1 (x / s, p);
        }

        // The output magnitude one half saturates at — `s·p/(p+1)`. Exposed because a
        // cascading consumer has to re-gain between stages by exactly this (drive.md §10.3's
        // "two identical cascaded clipping stages": stage 2 only clips if stage 1's ceiling
        // is brought back up to its own knee).
        [[nodiscard]] static Real clampLevel (int p, float kneeScale) noexcept
        {
            const Real pd = static_cast<Real> (p);
            return static_cast<Real> (kneeScale) * pd / (pd + 1.0);
        }

    private:
        // Same guard and same value as the sibling: below this |x − x1| the divided
        // difference is ill-conditioned and the analytic midpoint limit is taken instead.
        static constexpr Real kEps = 1.0e-5;

        static float clampKnee (float s) noexcept
        {
            return s < kMinKneeScale ? kMinKneeScale : (s > 1.0f ? 1.0f : s);
        }

        // |x|^p for a small positive integer p — a loop of multiplies, so the hot path stays
        // libm-free. p is at most kMaxHardness, so this is at most 7 multiplies.
        static Real intPow (Real ax, int p) noexcept
        {
            Real r = ax;
            for (int i = 1; i < p; ++i)
                r *= ax;
            return r;
        }

        // The constant that makes the clamped-region antiderivative continuous at the knee:
        // `p/(p+1) − F1(1)` with `F1(1) = 1/2 − 1/((p+1)(p+2))`.
        static Real clampOffset (int p) noexcept
        {
            const Real pd = static_cast<Real> (p);
            return pd / (pd + 1.0) - 0.5 + 1.0 / ((pd + 1.0) * (pd + 2.0));
        }

        int hardness_ = 2;
        float sPos_ = 1.0f;
        float sNeg_ = 1.0f;
        Real x1_ = 0.0;
        Real f1x1_ = 0.0;
    };
}
