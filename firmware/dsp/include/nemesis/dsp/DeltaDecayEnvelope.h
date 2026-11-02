#include <nemesis/dsp/Real.h>
#pragma once

namespace nemesis::dsp
{
    // DeltaDecayEnvelope — the per-note TRANSIENT envelope (T-MASK-03 leaf F1, ADR-037).
    //
    // The reference instrument (Kodamo MASK 1) spends three of its seven per-voice envelopes on
    // "delta-decay" shapes — osc1 mask, osc2 mask, pitch — and that choice is most of why its bitmask
    // oscillator sounds like an instrument rather than a waveform selector: the mask NUMBER snaps on
    // every note and settles, so a note has an attack transient in TIMBRE rather than only in level.
    // This is that shape: an instant unit step at note-on, decaying to nothing.
    //
    // WHY IT IS NOT AN ADSR WITH THE STAGES TURNED OFF. `AdsrEnvelope` can be coaxed into a similar
    // contour (zero delay, zero attack, zero sustain), but its state machine then carries four stages,
    // two more coefficients and a gate it must be told to release — for a shape whose entire definition
    // is one multiply. The cost of the wrong abstraction here is not size but a class of bug: an ADSR
    // left holding at sustain is a STUCK modulation, and a mask stuck off its base is an instrument
    // that has quietly changed timbre with no control to explain it. This envelope cannot hold: it
    // decays from the moment it fires, needs no note-off, and reaches zero on its own.
    //
    // THE LAW, and it is the house one rather than a second opinion: the per-sample factor is
    // `10^(−3/decaySamples)`, i.e. the value falls to **−60 dB at the set decay time** — the analog RC
    // convention `AdsrEnvelope::updateCoefficients` uses for its own decay and release, and
    // `ModalResonator`'s T60. One law, so "decay = 200 ms" means the same thing everywhere in the
    // engine. A zero (or negative) time gives factor 0: the peak sample survives and the next is zero,
    // which is a single-sample blip rather than a latch at unity (a latched mask would be a timbre
    // change with no control behind it) and rather than silence (the shortest setting must still fire).
    //
    // READ-THEN-ADVANCE, matching every other source `CathodeModulatedVoice` ticks: the first sample
    // after `trigger()` carries the peak, so a note's transient starts on the note's first sample and
    // not one later.
    //
    // A RE-TRIGGER RESTARTS AT EXACTLY 1.0 rather than adding to whatever is left. On a fast repeat an
    // additive step would climb past unity and push its destination beyond the range a depth control
    // promises — for the mask that means folding through the 9-bit space at a moment the player did not
    // ask for. The peak is a SET, and that is a behavioural claim with its own test.
    //
    // RT-safety: `prepare()` and `setDecayMs()` resolve the coefficient (one `pow`, control-rate — and
    // `setDecayMs` is called from the audio thread when the time is automated, which is why it is
    // `noexcept` and allocation-free rather than merely off the hot path); `trigger()`, `reset()` and
    // `processSample()` are inline, branch-light, libm-free. Deterministic — no randomness, so a
    // re-render is bit-identical. Computed in double, emitted as float. No JUCE. Not thread-safe
    // against prepare().
    class DeltaDecayEnvelope
    {
    public:
        // Resolves the coefficient against `sampleRate` and returns the envelope to REST (silent).
        // Safe to call at any time; not RT-safe only in the sense that it also clears state.
        void prepare (Real sampleRate) noexcept;

        // Decay time in ms — the time to −60 dB. Values <= 0 give the single-sample blip. Control-rate
        // (one `pow`), and it deliberately does NOT touch the running value: a time change is a change
        // of RATE, so an automated decay cannot step the output. Audio-thread safe.
        void setDecayMs (float ms) noexcept;

        // Fires the transient: the value is SET to 1.0, never added to (see the class note).
        void trigger() noexcept { value_ = 1.0; }

        // Returns to rest. RT-safe (a store).
        void reset() noexcept { value_ = 0.0; }

        // One sample, read-then-advance: the value this sample sees, then the decay step.
        float processSample() noexcept
        {
            const auto out = static_cast<float> (value_);
            value_ *= coef_;
            return out;
        }

        // The value the NEXT `processSample` will return (introspection / tests / read-backs).
        [[nodiscard]] float value() const noexcept { return static_cast<float> (value_); }

        // The set decay time in ms (introspection / read-backs — the engine exposes it so a test can
        // name the time rather than infer it from a slope).
        [[nodiscard]] float decayMs() const noexcept { return decayMs_; }

    private:
        Real sampleRate_ = 48000.0;
        float decayMs_ = 0.0f;
        Real coef_ = 0.0; // per-sample multiplier; 0 = the blip case
        Real value_ = 0.0; // [0, 1], the only state
    };
}
