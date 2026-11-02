#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/DelayCore.h>

#include <cmath>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kTwoPi = 6.283185307179586476925286766559;

        Real clampD (Real v, Real lo, Real hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        // One-pole coefficient for an `hz` corner at `fs`.
        Real onePoleHz (Real hz, Real fs) noexcept
        {
            return 1.0 - std::exp (-kTwoPi * clampD (hz, 1.0, 0.49 * fs) / fs);
        }

        // One-pole coefficient for a time constant in ms.
        Real onePoleMs (Real ms, Real fs) noexcept
        {
            const Real samples = clampD (ms, 0.01, 10000.0) * fs * 0.001;
            return 1.0 - std::exp (-1.0 / samples);
        }
    }

    void DelayCore::prepare (Real sampleRate, Real maxDelayMs)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        // §7 buffer sizing: max_time + mod_depth + interp_margin, power-of-2 for masked
        // indexing. The +1 is the write slot itself — the read stencil may reach the
        // sample written this very call (a 2-sample delay does).
        const Real maxDelay = clampD (maxDelayMs, 0.0, 1.0e6) * sampleRate_ * 0.001;
        const Real modMargin = kModDepthMarginMs * sampleRate_ * 0.001;
        const Real need = maxDelay + modMargin + static_cast<Real> (kInterpMarginSamples) + 1.0;

        int pow2 = 8;
        while (static_cast<Real> (pow2) < need)
            pow2 <<= 1;

        size_ = pow2;
        mask_ = pow2 - 1;
        // What the ring can actually SERVE, which is not what was asked for: the stencil
        // and the mod reserve have to stay inside it whatever the caller set.
        maxDelay_ = clampD (maxDelay,
            kMinDelaySamples,
            static_cast<Real> (size_) - modMargin - static_cast<Real> (kInterpMarginSamples) - 1.0);

        buf_.assign (static_cast<std::size_t> (size_), 0.0);

        requested_ = clampD (requested_, kMinDelaySamples, maxDelay_);
        coeffsDirty_ = true;
        reset();
    }

    void DelayCore::reset() noexcept
    {
        for (Real& s : buf_)
            s = 0.0;
        snap();
    }

    void DelayCore::snap() noexcept
    {
        writePos_ = 0;
        fbState_ = 0.0;
        dcState_ = dcPrev_ = 0.0;
        hpState_ = 0.0;
        lpState_ = 0.0;
        denorm_ = kDenormalGuard;
        clip_.reset();

        // SNAP, don't fade or glide: after a reset the line is at steady state for the
        // params in force, exactly like BbdDelayLine::reset.
        current_ = requested_;
        fading_ = false;
        fadeFrom_ = fadeTo_ = requested_;
        fadePos_ = 0;

        coeffsDirty_ = true;
        chunkCountdown_ = 0; // the next sample resolves, so a pre-render setter lands at n = 0
        resolveControls();
    }

    void DelayCore::setDelaySamples (Real samples) noexcept
    {
        requested_ = clampD (samples, kMinDelaySamples, maxDelay_ > 0.0 ? maxDelay_ : kMinDelaySamples);
    }

    void DelayCore::resolveControls() noexcept
    {
        if (coeffsDirty_)
        {
            lpActive_ = toneLpHz_ > 0.0;
            lpCoef_ = lpActive_ ? onePoleHz (toneLpHz_, sampleRate_) : 1.0;

            hpActive_ = toneHpHz_ > 0.0;
            hpCoef_ = hpActive_ ? onePoleHz (toneHpHz_, sampleRate_) : 0.0;

            glideCoef_ = onePoleMs (glideMs_, sampleRate_);

            const Real fadeSamples = clampD (crossfadeMs_, 0.0, kModDepthMarginMs * 4.0) * sampleRate_ * 0.001;
            fadeLen_ = fadeSamples < 1.0 ? 1 : static_cast<int> (fadeSamples);

            coeffsDirty_ = false;
        }

        // §0.3's hand-over. Varispeed ARMS nothing — the per-sample chase reads
        // `requested_` directly, which is exactly why a knob turn glides — but it must
        // retire a fade that was in flight when the mode changed, or the flag would
        // outlive the pointer that owned it and stall the NEXT crossfade.
        if (timeMode_ == TimeMode::Varispeed)
        {
            fading_ = false;
            return;
        }

        // Crossfade has to ARM a second pointer, and only when one is not already in
        // flight; `requested_` then doubles as the pending target (see the header).
        if (!fading_ && requested_ != current_)
        {
            fadeFrom_ = current_;
            fadeTo_ = requested_;
            fadePos_ = 0;
            fading_ = true;
        }
    }
}
