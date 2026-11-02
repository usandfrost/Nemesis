#pragma once

#include <nemesis/dsp/Real.h>
#include <optional>
#include <string_view>

namespace nemesis::dsp
{
    // [T-FX-01 leaf B] What an INSTRUMENT FX instance's processor has to be
    // (ADR-038 D1/D6). One interface for all ten `tid`s; batch 1 implements CHORUS
    // and leaves the other nine DECLARED-AND-UNBUILT, which the chain represents as
    // a null processor and renders as a transparent pass-through — not silence, and
    // not a placeholder effect that colours the sound.
    //
    // THREAD CONTRACT, per method, because it is the whole risk on this path:
    //   * `prepare` — MESSAGE thread, may allocate. Called before the instance is
    //     published into a graph the audio thread can see.
    //   * `reset` — AUDIO thread. Flushes tails for the §DSP D6 hard-switch, so it
    //     must be alloc-free and noexcept. It is called on the first render after a
    //     reorder, never from the message thread, because the object may still be
    //     live in the outgoing graph until the swap is observed.
    //   * `process` — AUDIO thread, alloc-free. In place, stereo, `numSamples` frames.
    //   * `latencySamples` — read on both, so it must be a settled value after
    //     `prepare` and must not change per block; the chain's parallel-path
    //     alignment is computed once at publish time from it.
    class FxEffect
    {
    public:
        FxEffect() = default;
        virtual ~FxEffect() = default;

        FxEffect (const FxEffect&) = delete;
        FxEffect& operator= (const FxEffect&) = delete;
        FxEffect (FxEffect&&) = delete;
        FxEffect& operator= (FxEffect&&) = delete;

        virtual void prepare (Real sampleRate, int maxBlockSamples) = 0;
        virtual void reset() noexcept = 0;
        virtual void process (float* left, float* right, int numSamples) noexcept = 0;

        [[nodiscard]] virtual int latencySamples() const noexcept { return 0; }

        // [T-FX-01 leaf E] THE SEAM PAIR — called from the MESSAGE thread (the editor's apply)
        // AND the AUDIO thread (`FxChain::pushFirstOfTidUnit` / `appliedFirstOfTidUnit`: the host's
        // per-block handle push and a trig's FX locks), so both must be allocation- and lock-free.
        // A float written on one thread while read on the other is the accepted torn-value class.
        //
        // An instance's parameters are a value block over its TYPE's existing
        // `<store>.<field>` registry rows (ADR-038's central finding), so the app
        // seam that carries them down is generic over `tid`: it walks the type's
        // rows, converts each one through its own §22.8 curve, and hands the result
        // over BY FIELD NAME. That keeps the field→setter map inside the effect's own
        // TU, where the effect's author can see it, and it is why the nine remaining
        // types cost the assembly nothing — a new effect implements these two and
        // needs no change above the dsp layer.
        //
        // WHY BY NAME AND NOT BY INDEX. The index space is the REGISTRY's order, so an
        // index-keyed effect would silently re-point if a row were ever inserted into
        // its store; the field name is the serialised key and the stable one.
        //
        // `value` is the row's §22.8 UNIT where the row has a curve (Hz, ms, %) and
        // its PLAIN value where it has none (an Enum ordinal, a Bool 0/1) — the
        // T-CORE-06 rule that plain units cross a façade and Hz is resolved on the
        // core side of it. An unknown field is IGNORED, which is what makes an
        // unbuilt type inert rather than a crash (ADR-038 D6).
        virtual void setAppliedUnit (std::string_view field, Real value) noexcept
        {
            (void) field;
            (void) value;
        }

        // The read-back, in the SAME units — what this processor is actually holding,
        // read at the processor rather than from a bookkeeping copy of the last push.
        // An effect may answer with the value IN FORCE rather than the value set
        // (Vibrato's mix is pinned at 100 %), because that is the claim a render
        // cannot make; each effect documents which of its fields do that. nullopt for
        // a field this type does not have — including EVERY field on an unbuilt type,
        // which is how a caller tells "declared but unbuilt" from "holding a zero".
        [[nodiscard]] virtual std::optional<Real> appliedUnit (std::string_view field) const noexcept
        {
            (void) field;
            return std::nullopt;
        }
    };
}
