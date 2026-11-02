#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/FastMath.h>
#include <cmath>

namespace nemesis::dsp
{
    // Broken-converter grit — the shared sampler-emulation §0.2 amplitude primitive
    // (sampler_emulation.md §0.2; drive.md §4 "Bit crush"). It models the storage word of a
    // vintage sampler: a linear fractional-bit-depth quantiser, optionally wrapped in a
    // μ-law COMPANDER (encode -> quantise -> decode). Reused by CATHODE/DROSS/WERK and the
    // Emulator/SP-1200 device chains, which add resample+anti-alias, analog SSM/Curtis LP,
    // ZOH reconstruction, transpose interpolation and noise around this core (separate cards).
    //
    // Linear vs companded is the whole point: a linear B-bit quantiser has a level-INDEPENDENT
    // absolute error (~LSB/sqrt(12)), so quiet tails carry the same grain as loud bodies — the
    // raw, constant grit of linear 8-bit (Emulator I, Fairlight). μ-law companding quantises in
    // a compressed domain, so the decoded error TRACKS the signal level: quiet passages get
    // finer effective steps (cleaner), loud passages coarser. That "breathing" level-dependent
    // grain is why companded 8-bit (Emulator II) and 12-bit (SP-1200) read as warm/rich rather
    // than merely crunchy. This is the keystone the acceptance tests prove.
    //
    // Sample-domain (fs-independent), deterministic, no JUCE, zero allocation, memoryless
    // (no reset needed). Do NOT oversample this: like every digital lo-fi stage its aliasing
    // is the effect (drive.md §0.1).
    //
    // libm on the hot path — a deliberate, owner-reviewable deviation from the LESSONS
    // "libm-free hot path" convention (NOT a §2.3 RT rule). μ-law is an irreducibly
    // transcendental per-sample nonlinearity with no control-rate escape (unlike the Svf's
    // coefficient math or the polynomial waveshaper). A precomputed companding LUT was
    // rejected: near zero the encode curve is steep, so linear-interpolation error is
    // comparable to the sub-LSB quiet-signal quantisation step the compander must produce —
    // it would corrupt the very level-dependent-grain behaviour above. std::log1p/std::expm1
    // are RT-safe (non-allocating, lock-free, bounded latency). A rational-approximation
    // replacement is a clean follow-on; the golden guards its equivalence. The linear path
    // (companding off) stays pure arithmetic.
    class BrokenConverter
    {
    public:
        // Storage word length in bits (fractional allowed for smooth, automatable sweeps).
        // levelsHalf = 2^(B-1): the mid-rise quantiser is y = round(x·levelsHalf)/levelsHalf
        // (drive.md §4.1). Control-rate; clamped to [1, 24].
        void setBitDepth (float bits) noexcept
        {
            const Real b = clamp (static_cast<Real> (bits), 1.0, 24.0);
            levelsHalf_ = fm::exp2 (b - 1.0); // FIREFORK: exact at integer b; runs per sample when bit depth is modulated
        }

        // Companding amount in [0, 1]: 0 = linear quantise (grain constant with level), 1 =
        // full classic μ-law (μ = 255, strongest level-dependent grain). Maps linearly to μ.
        // Control-rate.
        void setCompanding (float amount) noexcept
        {
            mu_ = clamp (static_cast<Real> (amount), 0.0, 1.0) * kMuMax;
            companding_ = mu_ > kMuMin;
            // FIREFORK: ln(1+mu) is constant between control changes; the per-sample path used to recompute it twice
            // per call. Same value, so the output is bit-identical.
            log1pMu_ = companding_ ? std::log1p (mu_) : static_cast<Real> (0.0);
        }

        // [T-CORE-18] The two CONTROL values back out (introspection / tests). `bitDepth()` is
        // the GRIT macro's second geometric leg; `companding()` is one of the three §2.2 rows
        // the macro must NOT touch, and the spec's exclusion is only observable as a VALUE —
        // companding is inaudible at the macro's own clean anchor (16 bit), so a render can
        // never distinguish "not scaled" from "not audible there".
        [[nodiscard]] float bitDepth() const noexcept
        {
            return static_cast<float> (std::log2 (levelsHalf_) + 1.0);
        }
        [[nodiscard]] float companding() const noexcept { return static_cast<float> (mu_ / kMuMax); }

        // Processes one sample. Hot path: the linear branch is pure arithmetic; the companded
        // branch is encode -> quantise-in-compressed-domain -> decode (libm, see the class note).
        // Computed in double, emitted as float. Handles |x| > 1 gracefully (monotonic curve).
        float processSample (float x) noexcept
        {
            Real v = static_cast<Real> (x);
            if (companding_)
            {
                const Real c = encodeHoisted (v);
                const Real cq = std::round (c * levelsHalf_) / levelsHalf_;
                v = decodeHoisted (cq);
            }
            else
            {
                v = std::round (v * levelsHalf_) / levelsHalf_;
            }
            return static_cast<float> (v);
        }

        // Exact μ-law encode c = sign(x)·ln(1+μ|x|)/ln(1+μ), mapping [-1,1] onto [-1,1] (odd,
        // monotonic, pins ±1). The single source of truth for the compander; tests assert it
        // is the analytic inverse of muLawDecode. μ > 0.
        static Real muLawEncode (Real x, Real mu) noexcept
        {
            const Real s = (x < 0.0) ? -1.0 : 1.0;
            return s * std::log1p (mu * std::abs (x)) / std::log1p (mu);
        }

        // Exact μ-law decode x = sign(c)·((1+μ)^|c| - 1)/μ, the analytic inverse of
        // muLawEncode. μ > 0 (the caller guarantees this via setCompanding's kMuMin gate).
        static Real muLawDecode (Real c, Real mu) noexcept
        {
            const Real s = (c < 0.0) ? -1.0 : 1.0;
            return s * std::expm1 (std::abs (c) * std::log1p (mu)) / mu;
        }

    private:
        // FIREFORK: the per-sample forms of muLawEncode / muLawDecode: ln(1+mu) from log1pMu_, transcendental via fm::
        // (std:: unless NEMESIS_DSP_FAST_MATH). Operation order matches the static versions, which stay as the tested reference.
        Real encodeHoisted (Real x) const noexcept
        {
            const Real s = (x < 0.0) ? -1.0 : 1.0;
            return s * fm::log1p (mu_ * std::abs (x)) / log1pMu_;
        }
        Real decodeHoisted (Real c) const noexcept
        {
            const Real s = (c < 0.0) ? -1.0 : 1.0;
            return s * fm::expm1 (std::abs (c) * log1pMu_) / mu_;
        }

        static constexpr Real kMuMax = 255.0; // classic μ-law μ at full companding
        static constexpr Real kMuMin = 1.0e-6; // below this, treat as linear (no companding)

        static Real clamp (Real v, Real lo, Real hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        Real levelsHalf_ = 128.0; // 2^(B-1), B = 8 by default
        Real mu_ = 0.0; // companding μ (0 => linear)
        Real log1pMu_ = 0.0; // ln(1 + mu_), valid while companding_
        bool companding_ = false;
    };
}
