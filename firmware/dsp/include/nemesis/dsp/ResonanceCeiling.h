#pragma once

#include <nemesis/dsp/Real.h>
#include <algorithm>
#include <cmath>

namespace nemesis::dsp
{
    // ResonanceCeiling — the CATHODE filter stage's LEVEL-DEPENDENT CEILING (D-M-0511 + D-M-0373, owner
    // ruling B31: build one design now). It bounds the FILTER STAGE'S OUTPUT — the sum of F1 and F2 as
    // routed — at max(kFloor, kRatio · the stage input's level), in every mode and at every resonance.
    //
    // WHY A CEILING ON THE SUM, AND WHY RELATIVE TO THE INPUT. Below the self-osc onset nothing bounded
    // the stage: a resonant core boosts the harmonics at its cutoff by its Q, so a Soviet chord at cutoff
    // plain 40 ran at ~30 per voice (19.7 at the plugin output) where the same chord unfiltered is ~1.3,
    // and the Parallel route summed two banks. And the T-CMP-32 crossfade into the self-osc clip only
    // ceilinged its WET term, so mid-crossfade (reso plain ~98-106) a voice sat ~3x above the top of the
    // knob. A filter that is emphasising has no business being louder than its input by more than a
    // resonant lift; one that is GENERATING (self-osc, input silent) is held by the clip at ~1, and
    // kFloor sits above that clip (and its 2x path's Gibbs overshoot, 1.204 worst) so the top of the
    // knob — the owner-approved T-CMP-32 loudness — is not touched.
    //
    // WHY A GAIN AND NOT A WAVESHAPE — THE LESSON OF THE REJECTED B29 FIX. That fix clamped the dry term
    // with a curve whose ceiling depended on the RESONANCE and fell from unbounded to 1.1 within a few
    // knob steps of the onset: a hard clip of a chord plus its resonance suppresses the weaker component
    // (capture), so the played notes vanished under the resonant peak at one knob step — "a jump, almost
    // like another note". Here nothing depends on the resonance (so a sweep can only move the level as
    // continuously as the core's own output moves), and the bound is a GAIN, which keeps the balance of
    // notes and resonance exactly as the core made it: no harmonics added, no aliasing, no pitch capture.
    //
    // THE DETECTORS. Both levels are instant-attack peak followers with a hold (longer than the period of
    // any note, and refreshed by any peak within 3 % of the envelope, so a steady tone holds its envelope
    // flat and the gain does not ripple per cycle) and a one-pole release. Instant attack is what makes the ceiling a ceiling: the output envelope is >= the
    // sample, so `sample * c / env` cannot exceed `c`. While the output envelope is at or under the
    // ceiling the gain is exactly 1 and the caller passes the sample through untouched — every stage
    // that never crosses it is BIT-EXACT.
    //
    // RT-safety: prepare() computes the release coefficient (libm, prepare time); process is inline,
    // allocation-free, no libm (one divide while limiting). No JUCE.
    class ResonanceCeiling
    {
    public:
        // The absolute floor of the ceiling: above the self-osc clip's unity ceiling and its 2x path's
        // band-limited overshoot (`CathodeFilterBank`, D-M-0372: 1.204 worst), so a single singing bank
        // is never held by this stage.
        static constexpr float kFloor = 1.25f;
        // How far above its input's level the stage may lift: a resonant boost of up to x2 (+6 dB) passes
        // untouched. MEASURED at the plugin over models x cutoff x reso (a four-note chord): the musical
        // body of the travel needs up to ~2 (SVF reso plain 80 at cutoff 76: 1.9 · Scream 1.27 · Reed
        // cutoff 76 1.83 · the Init chord 1.18), while the defects sit far past it — Soviet / Reed at
        // cutoff 40 up to 26.5, SVF at reso 127 9.5, the Ladder's T-CMP-32 crossfade 2.83 at plain 100.
        static constexpr float kRatio = 2.0f;
        static constexpr Real kHoldMs = 25.0; // longer than a 40 Hz period
        // A peak within this fraction of the envelope REFRESHES the hold without moving the envelope: a
        // sampled steady tone's per-cycle maximum wanders by the sampling phase, and without this a
        // slightly lower cycle would let the hold run out and the gain ripple once per cycle.
        static constexpr float kHoldRefresh = 0.97f;
        static constexpr Real kReleaseMs = 150.0;

        void prepare (Real sampleRate) noexcept
        {
            const Real fs = sampleRate > 0.0 ? sampleRate : 48000.0;
            holdSamples_ = static_cast<int> (std::lround (kHoldMs * 0.001 * fs));
            release_ = static_cast<float> (std::exp (-1.0 / (kReleaseMs * 0.001 * fs)));
            reset();
        }

        void reset() noexcept
        {
            envIn_ = 0.0f;
            envOut_ = 0.0f;
            holdIn_ = 0;
            holdOut_ = 0;
        }

        // One sample: `in` the stage's input sample, `outPeak` the larger magnitude of its output pair.
        // Returns the gain to apply — EXACTLY 1.0f while the output is under the ceiling.
        [[nodiscard]] float gainFor (float in, float outPeak) noexcept
        {
            follow (envIn_, holdIn_, std::abs (in));
            follow (envOut_, holdOut_, outPeak);
            const float c = std::max (kFloor, kRatio * envIn_);
            return envOut_ > c ? c / envOut_ : 1.0f;
        }

        [[nodiscard]] float inputLevel() const noexcept { return envIn_; }
        [[nodiscard]] float outputLevel() const noexcept { return envOut_; }
        [[nodiscard]] float ceiling() const noexcept { return std::max (kFloor, kRatio * envIn_); }

    private:
        void follow (float& env, int& hold, float x) const noexcept
        {
            if (!(x >= 0.0f))
                return; // a NaN leaves the envelope AND its hold alone (the release arm would store it)
            if (x >= env) // instant attack
            {
                env = x;
                hold = holdSamples_;
            }
            else if (x >= kHoldRefresh * env)
                hold = holdSamples_; // a steady tone's next peak, a few ulps under the sampled maximum
            else if (hold > 0)
                --hold;
            else
            {
                const float d = env * release_;
                env = d > x ? d : x;
                if (env < 1.0e-20f)
                    env = 0.0f; // no denormal tail
            }
        }

        float envIn_ = 0.0f;
        float envOut_ = 0.0f;
        int holdIn_ = 0;
        int holdOut_ = 0;
        int holdSamples_ = 1200;
        float release_ = 0.99986f;
    };
}
