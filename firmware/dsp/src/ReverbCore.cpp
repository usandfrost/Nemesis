#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ReverbCore.h>

#include <cmath>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kTwoPi = 6.283185307179586476925286766559;

        // The tonal-correction shelf's pole, clamped so a nearly-black damping setting
        // cannot ask for an unbounded top-end boost. 0.7 caps it at (1+b)/(1-b) = 5.7x
        // (~+15 dB) at Nyquist.
        constexpr Real kMaxTonalCorrectionB = 0.7;

        // §0.6 start phases: consecutive multiples of the golden ratio spread N points
        // around the circle about as evenly as a fixed rule can, for ANY N — which is
        // what "uncorrelated LFO phases per line" needs at M = 4, 8 and 16 alike.
        constexpr Real kPhaseSpread = 0.61803398874989484820458683436564;

        Real clampD (Real v, Real lo, Real hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        // One-pole coefficient for an `hz` corner at `fs` (unity DC gain form).
        Real onePoleHz (Real hz, Real fs) noexcept
        {
            return 1.0 - std::exp (-kTwoPi * clampD (hz, 1.0, 0.49 * fs) / fs);
        }

        bool isPrimeInt (int n) noexcept
        {
            if (n < 2)
                return false;
            if (n % 2 == 0)
                return n == 2;
            for (int f = 3; f * f <= n; f += 2)
                if (n % f == 0)
                    return false;
            return true;
        }

        // The nearest prime to `want` inside [lo, hi] that no earlier line has taken.
        // §0.4 asks for MUTUALLY prime lengths; distinct primes are mutually prime by
        // construction, so the whole rule reduces to "never hand out the same prime
        // twice". Searching outward from `want` keeps each line as close as possible to
        // the length its caller asked for.
        int pickDistinctPrime (Real want, const Real* taken, int takenCount, int lo, int hi) noexcept
        {
            if (hi < lo)
                return lo;

            const int centre = static_cast<int> (
                std::lround (clampD (want, static_cast<Real> (lo), static_cast<Real> (hi))));
            const int span = (hi - lo) + 1;

            for (int r = 0; r < span; ++r)
            {
                for (int sign = 0; sign < 2; ++sign)
                {
                    const int c = sign == 0 ? centre - r : centre + r;
                    if (c < lo || c > hi || !isPrimeInt (c))
                        continue;

                    bool used = false;
                    for (int k = 0; k < takenCount; ++k)
                        // `taken` holds prime sample counts, i.e. exact integral doubles.
                        if (taken[k] == static_cast<Real> (c))
                        {
                            used = true;
                            break;
                        }
                    if (!used)
                        return c;

                    if (r == 0)
                        break; // c is the same value for both signs at r == 0
                }
            }
            return centre;
        }
    }

    // =============================================================================
    // Allpass
    // =============================================================================

    void ReverbCore::Allpass::prepare (int maxDelaySamples)
    {
        int pow2 = 8;
        while (pow2 < maxDelaySamples + 2)
            pow2 <<= 1;

        size_ = pow2;
        mask_ = pow2 - 1;
        buf_.assign (static_cast<std::size_t> (size_), 0.0);
        pos_ = 0;
        if (delay_ > size_ - 1)
            delay_ = size_ - 1;
    }

    void ReverbCore::Allpass::reset() noexcept
    {
        for (Real& s : buf_)
            s = 0.0;
        pos_ = 0;
    }

    // =============================================================================
    // Analytic laws
    // =============================================================================

    Real ReverbCore::decayGainFor (Real delaySamples, Real rt60Seconds, Real fs) noexcept
    {
        // §7 "Freeze / infinite": loop gain exactly 1.0 is the lossless prototype.
        if (rt60Seconds <= 0.0 || fs <= 0.0)
            return 1.0;

        // §0.2: g = 10^(-3·D/RT60), D in seconds.
        return std::pow (10.0, -3.0 * (delaySamples / fs) / rt60Seconds);
    }

    // =============================================================================
    // Lifecycle
    // =============================================================================

    void ReverbCore::prepare (Real sampleRate,
        int numLines,
        Mixing mixing,
        Real maxLineDelayMs,
        Real maxPreDelayMs)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        // Snap to the sizes the three FDN types actually use. Hadamard needs a power of
        // two, and a size no type asks for is untested surface.
        numLines_ = numLines <= 4 ? 4 : (numLines <= 8 ? 8 : 16);
        mixing_ = mixing;

        // §7 buffer sizing: longest delay + max modulation depth + interpolation margin,
        // rounded to a power of two for masked indexing.
        const Real wantLine = clampD (maxLineDelayMs, 1.0, 1.0e6) * sampleRate_ * 0.001;
        const Real need = wantLine + kMaxModDepthSamples + 2.0;

        int pow2 = 8;
        while (static_cast<Real> (pow2) < need)
            pow2 <<= 1;

        lineSize_ = pow2;
        lineMask_ = pow2 - 1;
        maxLineDelay_ = clampD (wantLine,
            kMinLineDelaySamples,
            static_cast<Real> (lineSize_) - kMaxModDepthSamples - 2.0);

        lineStride_ = lineSize_ + 261; // FIREFORK B12: each line's write in a D-cache set of its own
        lineBuf_.assign (static_cast<std::size_t> (numLines_) * static_cast<std::size_t> (lineStride_), 0.0);

        // §0.5 pre-delay ring.
        const Real wantPre = clampD (maxPreDelayMs, 0.0, 1.0e6) * sampleRate_ * 0.001;
        int prePow2 = 8;
        while (static_cast<Real> (prePow2) < wantPre + 2.0)
            prePow2 <<= 1;

        preSize_ = prePow2;
        preMask_ = prePow2 - 1;
        maxPreDelaySamples_ = clampD (wantPre, 0.0, static_cast<Real> (preSize_) - 2.0);
        preBuf_.assign (static_cast<std::size_t> (preSize_), 0.0);

        // §0.3 diffusers, sized for the longest stage any type asks for.
        const int maxDiffSamples = static_cast<int> (kMaxDiffuserMs * sampleRate_ * 0.001) + 1;
        for (auto& ap : diffusers_)
            ap.prepare (maxDiffSamples);

        // Defaults a caller can override: line lengths spread across a hall-ish 20–80 ms
        // (§2.3's band), diffuser stages on §2.2's mutually-prime ms set. Without these a
        // freshly prepared core would have every line the same length, which is a
        // degenerate network rather than a neutral one.
        for (int i = 0; i < kMaxLines; ++i)
        {
            const Real frac = numLines_ > 1 ? static_cast<Real> (i) / static_cast<Real> (numLines_ - 1) : 0.0;
            requestedDelay_[static_cast<std::size_t> (i)] = (20.0 + 60.0 * frac) * sampleRate_ * 0.001;
        }

        constexpr Real kDefaultDiffuserMs[kMaxDiffusers] = { 4.8, 7.3, 10.0, 12.7, 16.1, 19.3 };
        for (int i = 0; i < kMaxDiffusers; ++i)
            diffuserMs_[static_cast<std::size_t> (i)] = kDefaultDiffuserMs[i];

        // b_i = 1/sqrt(M) so an impulse injects exactly unit ENERGY into the network, and
        // c_i = sqrt(2/M) over the M/2 lines feeding each side so the tap is level-neutral
        // for uncorrelated lines. Both are pure scale factors; nothing in §0 fixes them.
        const Real m = static_cast<Real> (numLines_);
        inputGain_ = 1.0 / std::sqrt (m);
        tapGain_ = std::sqrt (2.0 / m);

        dirty_ = true;
        reset();
    }

    void ReverbCore::reset() noexcept
    {
        for (Real& s : lineBuf_)
            s = 0.0;
        for (Real& s : preBuf_)
            s = 0.0;
        for (auto& ap : diffusers_)
            ap.reset();

        writePos_ = 0;
        prePos_ = 0;

        for (int i = 0; i < kMaxLines; ++i)
        {
            dampState_[static_cast<std::size_t> (i)] = 0.0;
            dcState_[static_cast<std::size_t> (i)] = 0.0;
            dcPrev_[static_cast<std::size_t> (i)] = 0.0;

            // §0.6 uncorrelated phases — re-seeded here (and only here), so a reset render
            // repeats bit for bit.
            const Real turns = static_cast<Real> (i) * kPhaseSpread;
            const Real phase = kTwoPi * (turns - std::floor (turns));
            lfoU_[static_cast<std::size_t> (i)] = std::cos (phase);
            lfoV_[static_cast<std::size_t> (i)] = std::sin (phase);
        }

        inLpState_ = 0.0;
        inHpState_ = 0.0;
        tcPrevL_ = 0.0;
        tcPrevR_ = 0.0;
        denorm_ = kDenormalGuard;

        dirty_ = true;
        chunkCountdown_ = 0; // the next sample resolves, so a pre-render setter lands at n = 0
        resolveControls();
    }

    void ReverbCore::setLineDelaySamples (int line, Real samples) noexcept
    {
        if (line < 0 || line >= kMaxLines)
            return;

        requestedDelay_[static_cast<std::size_t> (line)] = samples;
        dirty_ = true;
    }

    void ReverbCore::setDiffuserDelayMs (int idx, Real ms) noexcept
    {
        if (idx < 0 || idx >= kMaxDiffusers)
            return;

        diffuserMs_[static_cast<std::size_t> (idx)] = ms;
        dirty_ = true;
    }

    // =============================================================================
    // Control-rate resolution — the only place this class calls libm.
    // =============================================================================

    void ReverbCore::resolveControls() noexcept
    {
        const int lo = static_cast<int> (kMinLineDelaySamples);
        const int hi = static_cast<int> (maxLineDelay_);

        for (int i = 0; i < numLines_; ++i)
        {
            const int p = pickDistinctPrime (requestedDelay_[static_cast<std::size_t> (i)],
                lineDelay_.data(),
                i,
                lo,
                hi);
            lineDelay_[static_cast<std::size_t> (i)] = static_cast<Real> (p);
            decayGain_[static_cast<std::size_t> (i)] = decayGainFor (static_cast<Real> (p), rt60_, sampleRate_);
        }

        // §0.4 per-line damping.
        dampActive_ = dampingHz_ > 0.0 && dampingHz_ < 0.49 * sampleRate_;
        dampCoef_ = dampActive_ ? onePoleHz (dampingHz_, sampleRate_) : 1.0;

        // §0.4 tonal correction. `a` is the damping filter's Nyquist-to-DC gain ratio
        // (1-d)/(1+d); Jot's shelf pole is b = (1-a)/(1+a), so damping off (a == 1) gives
        // b == 0 and E(z) == 1 EXACTLY — the correction cannot colour an undamped tail.
        tcActive_ = false;
        tcB_ = 0.0;
        tcGain_ = 1.0;
        if (tonalCorrectionOn_ && dampActive_)
        {
            const Real d = 1.0 - dampCoef_;
            const Real a = (1.0 - d) / (1.0 + d);
            const Real b = clampD ((1.0 - a) / (1.0 + a), 0.0, kMaxTonalCorrectionB);
            if (b > 0.0)
            {
                tcB_ = b;
                tcGain_ = 1.0 / (1.0 - b);
                tcActive_ = true;
            }
        }

        // §0.5 input tilt.
        inLpActive_ = inLpHz_ > 0.0 && inLpHz_ < 0.49 * sampleRate_;
        inLpCoef_ = inLpActive_ ? onePoleHz (inLpHz_, sampleRate_) : 1.0;
        inHpActive_ = inHpHz_ > 0.0;
        inHpCoef_ = inHpActive_ ? onePoleHz (inHpHz_, sampleRate_) : 0.0;

        // §0.5 pre-delay.
        preDelaySamples_ = clampD (preDelayMs_ * sampleRate_ * 0.001, 0.0, maxPreDelaySamples_);

        // §0.6 modulation. The rotation runs at the LFO rate; the phases themselves are
        // owned by reset(), so a rate change bends the sweep rather than restarting it.
        modActive_ = modDepth_ > 0.0 && modRateHz_ > 0.0;
        modDepth_ = clampD (modDepth_, 0.0, kMaxModDepthSamples);
        if (modActive_)
        {
            const Real w = kTwoPi * clampD (modRateHz_, 0.0, 0.49 * sampleRate_) / sampleRate_;
            lfoCos_ = std::cos (w);
            lfoSin_ = std::sin (w);
        }
        else
        {
            lfoCos_ = 1.0;
            lfoSin_ = 0.0;
        }

        // §0.3 diffuser stage lengths — mutually prime, like the FDN lines.
        {
            std::array<Real, kMaxDiffusers> taken {};
            const int dLo = 1;
            const int dHi = static_cast<int> (kMaxDiffuserMs * sampleRate_ * 0.001);
            for (int i = 0; i < kMaxDiffusers; ++i)
            {
                const Real want = diffuserMs_[static_cast<std::size_t> (i)] * sampleRate_ * 0.001;
                const int p = pickDistinctPrime (want, taken.data(), i, dLo, dHi);
                taken[static_cast<std::size_t> (i)] = static_cast<Real> (p);
                diffusers_[static_cast<std::size_t> (i)].setDelaySamples (p);
            }
        }

        dirty_ = false;
    }
}
