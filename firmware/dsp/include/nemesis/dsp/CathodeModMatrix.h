#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ForkConfig.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace nemesis::dsp
{
    // CathodeModMatrix — the CATHODE §4 mod-matrix evaluator (CATHODE.md §4 "Mod matrix: sources
    // (envs, LFOs, S&H, step-mod, velocity, key, mod-wheel, the synth's own random/drift) →
    // destinations (osc position, PD amount, pitch, sync, converter rate/bits/loop-err/drift, filter
    // cutoff/res/vowel, amp, pan, noise level…)"). The §4 KEYSTONE piece that the signed-off sources
    // (Lfo M1-17a, StepModulator M1-17b, AdsrEnvelope M1-10a) and the runtime scalars (velocity, key
    // track, mod-wheel, converter drift) plug INTO.
    //
    // A PURE combinational routing/summing engine. It does NOT own or tick the sources: the caller
    // supplies each source's current value per sample (the voice/engine reads its Lfo/StepModulator/
    // env/velocity/… and feeds them in via setSourceValue). Up to kMaxSlots assignment slots, each
    // routing one Source → one Dest with a bipolar depth ∈ [-1, 1]. Per sample, evaluate() sums
    // (sourceValue · depth) over the active slots for every destination; modulation(dest) reads the
    // resulting offset. Because it only multiplies and adds, the evaluator is RANGE-AGNOSTIC — a
    // bipolar LFO in [-1,1] and a unipolar envelope in [0,1] both route the same way.
    //
    // The summed offset is a raw modulation amount; ADDING it to a base parameter and CLAMPING to the
    // param's real range (and any bipolar↔unipolar remap or smoothing) is the DESTINATION / param-layer
    // concern — exactly as anti-click smoothing is the destination's job for the LFO/step sources, not
    // the source's. This keeps the matrix a pure router.
    //
    // RT-safety (§11): no allocation, no libm, no RNG, no locks — evaluate() zeroes the destination
    // accumulators then walks the fixed-size slot table (accumulating in double for a deterministic,
    // slot-index-ordered sum). Memoryless and sample-rate-independent → no prepare(). Deterministic
    // (§11): a given (slot config + source values) yields bit-identical output; reset() zeroes the
    // source/destination values (slot config persists). Computed in double, emitted as float. No
    // JUCE. Not thread-safe against concurrent setSlot.
    //
    // No committed golden (owner decision, 2026-07-15): the output is exactly Σ sourceValue·depth, so
    // the analytic keystones in the test ARE the conformance proof — a golden would be a trivially-
    // reproducible drift-guard adding nothing. (The FIRST DSP primitive without a §4.6 golden.)
    class CathodeModMatrix
    {
    public:
        // Modulation sources (§4). One entry per distinct source the voice can feed in.
        enum class Source : std::uint8_t {
            Env1,
            Env2,
            Env3,
            Lfo1,
            Lfo2,
            Lfo3,
            StepMod,
            SampleHold,
            Velocity,
            KeyTrack,
            ModWheel,
            // [T-MOD-01 leaf B] RENAMED from `Drift`, which was never this thing. This slot has
            // always meant "`dsp::ConverterDrift`'s instantaneous value" — a lo-fi converter-artefact
            // READOUT, CATHODE-specific, fed since T-CGUI-24 — and it collided by name with
            // the shared-spine DRIFTER the same card adds three entries for below. A pure C++
            // identifier rename: the ORDINAL is unchanged (11) and stored presets keep their meaning,
            // because a matrix slot serialises its source as an INDEX (`ProjectJson::writeMatrixSlots`
            // writes `slot.src` as an int, never a name). Not to be confused with `Dest::ConvDrift`,
            // which is the converter's drift PARAMETER as a modulation target — a different concept
            // that happens to share the word.
            ConvDriftReadout,
            // [T-MASK-03 leaf F1] AFTERTOUCH AND SLIDE ARE HERE AS INERT PLACEHOLDERS, and this
            // REVERSES the T-CORE-14 note that used to explain their absence ("two enum entries that no
            // `setSourceValue` call ever feeds would be dead L1 surface … they resolve INACTIVE, which
            // is the same sound and no new enum"). That reasoning was sound while this enum was the
            // SHORTER list. It stops being sound the moment L1 gains a source of its own, because
            // `CathodeMachine::matrixSourceFor` is an IDENTITY cast bounded by `kSourceCount`: L2's
            // roster already carried Aftertouch at 12 and Slide at 13 (T-SPEC-17), so appending a real
            // source at 12 silently makes every stored "Aftertouch" routing mean it. Caught by
            // `test_apply_table.cpp`'s deferred-source case going red, which is precisely what it was
            // written to catch. So the two lists are index-identical again, and the placeholders cost
            // one `setSourceValue` call each when T-CMP-06 makes them live.
            Aftertouch, // LIVE per note since T-CMP-06: max(poly pressure, channel pressure) (CathodeModulatedVoice)
            Slide, // still 0-valued — nothing feeds it, so it contributes nothing
            // The two per-note TRANSIENT envelopes (`dsp::DeltaDecayEnvelope`),
            // one per oscillator slot — the reference instrument dedicates an envelope of this shape
            // to each mask. APPENDED, like every roster in this project whose index is stored: a
            // reorder would re-map every saved routing. They are ordinary sources, so they can drive
            // ANY destination (the reference's third delta-decay target, pitch, is a user routing
            // rather than a third instance).
            DeltaDecay1,
            DeltaDecay2,
            // [T-MOD-01 leaf B] The three DRIFTER ring members (`dsp::DrifterBank`) — the first
            // ENGINE-GLOBAL generator in this roster. Every source above is either owned by the voice
            // (envs, LFOs, S&H, step) or latched onto it per note (velocity, key track); these three
            // are computed ONCE PER SAMPLE by `CathodeVoiceEngine` and broadcast identically into every
            // voice's matrix, which is why they arrive through `setDrifterValues` rather than a tick
            // inside `CathodeModulatedVoice::processSample`.
            //
            // Named `Drifter*` and not `Drift*` deliberately: in this codebase "drift" is already the
            // CONVERTER-artefact word (`ConverterDrift`, `Dest::ConvDrift`, `Source::ConvDriftReadout`
            // above, `SpreadTarget::ConvDrift`, `synth.conv.drift`), and "drifter" is unambiguously the
            // new bounded-random-walk GENERATOR. `Drift1` beside `ConvDriftReadout` would read as two
            // spellings of one thing.
            //
            // APPENDED, like every roster here whose index is stored.
            Drifter1,
            Drifter2,
            Drifter3,
            // [T-CMP-07 leaf 6] The four MACROS as sources. The card asks for them and the direction
            // is the point: all four also sit on the app layer's REFUSED destination list, because a
            // macro IS the depth-scaling law for other rows (`core::gritScale`, `core::motionScale`,
            // and the two oscillator riders) rather than a parameter that makes sound. The same four
            // tokens, and only one direction makes sense.
            //
            // ENGINE-GLOBAL like the drifters above, and broadcast for the same reason — a macro is
            // one machine-wide control, so a per-voice copy would be a second record that could
            // disagree with the one the apply table scales by. They arrive through `setMacroValues`.
            //
            // APPENDED, like every roster here whose index is stored — `slot.src` is still an
            // ordinal (this card's stated boundary), so a reorder would re-map every saved routing.
            MacroGrit,
            MacroMotion,
            MacroOscPos,
            MacroPdAmt,
            Count
        };

        // Modulation destinations (§4 / §9 `synth.*`). One entry per modulatable parameter.
        enum class Dest : std::uint8_t {
            OscPos,
            PdAmount,
            Pitch,
            Sync,
            ConvRate,
            ConvBits,
            ConvLoopErr,
            ConvDrift,
            FiltCutoff,
            FiltReso,
            FiltVowel,
            Amp,
            Pan,
            NoiseLevel,
            // [T-MASK-03 leaf F1] The two BITMASK destinations (`synth.osc{1,2}.mask`), and they are
            // PER SLOT where `OscPos` and `PdAmount` are one dest applied to both. That asymmetry is
            // deliberate: those two carry one offset onto each slot's own base, which is right for a
            // shared scan position — but the reference's whole gesture is osc1 and osc2 snapping
            // INDEPENDENTLY, and a single shared dest would make the two masks always move together.
            // Appended, so stored routings keep their meaning.
            Osc1Mask,
            Osc2Mask,
            // [T-CMP-15 leaf D2 / R2] The two PULSE-WIDTH destinations (`synth.osc{1,2}.pw`), and
            // they are PER SLOT for exactly the reason the two masks above are: the musical gesture
            // is osc1 and osc2 breathing INDEPENDENTLY, and one shared dest would make the two
            // widths always move together — which is the sound this mode exists to avoid. A shared
            // `OscPw`, and a reuse of `OscPos`, were both offered at the plan gate and declined.
            // Appended, so stored routings keep their meaning.
            Osc1Pw,
            Osc2Pw,
            Count
        };

        static constexpr int kSourceCount = static_cast<int> (Source::Count);
        // [D-M-0193] ONE INTERNAL SOURCE ABOVE THE ROSTER: the SUB's mask transient (a third
        // `DeltaDecayEnvelope`), fed by the voice and read by the SUB's MASK ENV hard-wire alone. It is
        // NOT a `Source` enumerator because `Source` is index-identical to L2's stored roster
        // (`core::matrixSourceCount`): appending there would make it user-routable and move four
        // copies of one list for a route nothing asked for. Only a RESERVED slot may carry it
        // (`setSlot` refuses it on a user slot), and `setSourceValue` stores it like any other.
        static constexpr int kSubMaskEnvSource = kSourceCount;
        static constexpr int kSourceStorage = kSourceCount + 1;
        // The NAMED destinations — the ones with a bespoke law in `CathodeModulatedVoice`. Still 18,
        // and deliberately NOT the size of the id space below: L2's roster of named legacy dests is
        // pinned against this number (`test_apply_table.cpp`), and that pin is about the enum, not
        // about how many ids the evaluator can carry.
        static constexpr int kDestCount = static_cast<int> (Dest::Count);

        // [T-CMP-07 leaf 3] A DESTINATION IS AN ID, AND THE ENUM IS NOW A NAMED SUBSET OF THE ID
        // SPACE. `Dest` above stays exactly as it was — same enumerators, same ordinals, same
        // bespoke laws — because widening a routing target is not the same question as renaming one,
        // and the card's claim for this leaf is that NO AUDIO MOVES. What changes is that a slot may
        // now carry an id ABOVE the named set, which is what leaf 4's generic law will route.
        //
        // `int` rather than a wrapper struct: `Slot::dest` has been an `int` since M1-17c and the
        // arithmetic below already works in that domain, so a wrapper would be a new type over an
        // unchanged representation — cost with no claim attached. The `Dest` overloads are kept so
        // that every existing call site compiles untouched; an `enum class` does not convert to
        // `int` implicitly, so the pair is unambiguous rather than a trap.
        using DestId = int;

        // WHY 192 — AND WHY THE CAPACITY AND THE BUDGET ARE NOW TWO NUMBERS.
        // T-CMP-07 leaf 1 sized this at 128 against a roster of 88: "a power of two with real
        // headroom", and it was. Two cards later the roster is 126. The headroom was spent without
        // anyone noticing, because the only thing watching it was a strict `roster < capacity` —
        // a WALL, silent at 126 and fatal at 128, and 128 is where the next card already stands.
        // Measured 2026-09-06: ONE row of slack against a FOLD pair that needs two.
        //
        // THE FIRST FIX WAS WRONG IN SHAPE, AND THAT IS THE LESSON. Raising to 256 was refused by
        // `EngineCore.cpp`'s eager-instance `static_assert`; the reflex was then to take 151, the
        // maximum the OLD 4 MB budget allowed. That conflates two questions. **Capacity is a
        // roadmap number; budget is a multiple of measured cost.** Deriving one from the other is
        // what produced a guard that was silent at 93 % and fatal at 101 %.
        //
        // CAPACITY comes from the work: roster 126, the remaining CAT-4 and §8 registry deltas add
        // ~10, so ~136. **192 leaves 56 rows** — years of growth, and it restores the power-of-two
        // style the original sizing used. It is deliberately NOT the ~485 the new budget would
        // permit: spending the whole budget on the mod matrix would mean the eager-instance
        // tripwire could never fire for anything else — a new voice member, a voice-count change,
        // a second eager engine — which is the entire point of having it.
        //
        // BUDGET is owner-ruled at 8 MB (`nemesis::app::budget`, `app/src/CathodeMachine.h`), which
        // puts 192 at **1.78×** — almost exactly the ~1.8× the original T-PAT-19 ruling was written
        // to hold, and which had silently eroded to **1.07×** under the old 4 MB literal. So 8 MB
        // does not invent a policy; it restores the one already intended, at a capacity that covers
        // the work.
        //
        // WHAT IT COSTS (probe build, `sizeof` reported by a deliberately-failing `static_assert`
        // on this tree — a measurement, not an estimate): capacity 128 → `sizeof (CathodeMachine)`
        // 243,944 B, capacity 256 → 344,296 B. That is **784 B per slot per instance**, matching the
        // analytic figure — 49 B of per-voice arrays (`destValue_` double 8 · `genericDest_` 32
        // after padding · `liveCount_` 4 · `genericBase_` 4 · `destLive_` 1) across
        // `CathodeVoiceEngine::kMaxVoices` = 16 (then; 32 since D-M-0574). Nothing here is per-sample: both loops that walk
        // the full capacity live in `CathodeModulatedVoice::refreshLive()`, which runs when a slot
        // CHANGES. **Leaf F then deletes the 15 redundant copies of `genericDest_`,** taking the
        // per-slot cost to ~304 B — so this figure is the PRE-hoist one and the margin improves.
        //
        // NOTHING HERE CAN SEE L2, so this capacity and the roster it must hold are two numbers in
        // two layers, which is the shape that produced the leaf 0 defect. The guard therefore lives
        // where both are visible: `test_apply_table.cpp` asserts `core::modDestCount("cathode")`
        // fits, with a 16-row BUDGET rather than a boundary. An id at or above the capacity is
        // IGNORED by `setSlot`, never wrapped. No power of two is required — every use is a bounds
        // compare or an array extent, never a mask — but 192 keeps the original style anyway.
        static constexpr DestId kDestCapacity = 192;
        static_assert (kDestCapacity >= kDestCount, "the named destinations must fit in the id space");

        // The §2.6 free editor's range: "16 slots × {src, dest, depth}, free 8-of-16 paged". These
        // are the slots a preset stores and a user can re-point, and `core::kNumMatrixSlots` is the
        // same number at L2.
        static constexpr int kUserSlots = 16;

        // [T-CORE-14, owner ruling] Two RESERVED hard-wire slots sit above the editor range, giving
        // the storage ceiling 18. They exist because Param Ref:139/141 left `filt.envAmt` /
        // `filt.keytrack` as "the L2 registry card's pick" between a dedicated hard-wire and an alias
        // onto Init slots 1 and 6 — and the alias loses, because §2.6 is a FREE editor: re-pointing
        // slot 1 would leave the FILTER·Env-amt control writing depth into an unrelated route, two
        // controls sharing one cell with no invariant. Reserving two slots keeps both knobs
        // always-working while still running through ONE evaluator and ONE depth law (the objection
        // to giving the filter its own second modulation path instead).
        //
        // Nothing but the hard-wires may write kUserSlots..kMaxSlots-1: the editor, presets and
        // `MachineParams::matrix_` are all bounded by kUserSlots.
        // [T-MASK-03 leaf F1] TWO MORE RESERVED HARD-WIRES, for exactly the T-CORE-14 reason: the
        // per-slot MASK ENV depth (`synth.osc{1,2}.maskEnv`) must be an always-working knob, and
        // aliasing it onto a user slot would leave it writing depth into whatever route the player
        // last pointed that slot at. Same evaluator, same depth law, no second modulation path — the
        // objection T-CORE-14 sustained. The ceiling moves 18 → 20; nothing but the hard-wires may
        // write kUserSlots..kMaxSlots−1, so presets and `MachineParams::matrix_` are untouched.
        // [T-CMP-15 leaf D2] TWO MORE RESERVED HARD-WIRES, ceiling 20 → 22, for the T-CORE-14 reason
        // a third time. `synth.osc{1,2}.pwm` is a DEPTH knob that must always work, and `pwSrc`
        // picks what drives it — so the pair is one hard-wire slot per oscillator whose `src` the
        // enum sets and whose depth the knob sets. The alternative, a PW SRC picker BESIDE two
        // user-routable `Osc{1,2}Pw` dests, would be two paths to one destination with no
        // invariant — the objection T-CORE-14 sustained. `pwSrc = Manual` CLEARS the slot, which is
        // the off-anchor itself rather than a route at depth 0.
        //
        // THE VARIABLE-SOURCE PART IS GENUINELY NEW ON THIS MECHANISM: `kEnvAmtSlot` and the two
        // mask hard-wires each have a FIXED source, so `setSlot`'s src argument has never varied on
        // a reserved slot before. That is why the battery aims a mutation at it.
        //
        // Nothing but the hard-wires may write kUserSlots..kMaxSlots−1, so presets and
        // `MachineParams::matrix_` are untouched and `kUserSlots` stays 16.
        // [D-M-0193] TWO MORE, ceiling 22 -> 24: the SUB's MASK ENV (the internal source above -> the
        // GENERIC dest of `synth.sub.mask`) and its PWM (`synth.sub.pwSrc` -> the generic dest of
        // `synth.sub.pw`). The SUB's mask and width have always been GENERIC destinations (no named
        // `Dest` — the frozen eighteen are an ordinal roster, and widening it would move every generic
        // id a pre-T-CMP-16 project stored as an int), so the hard-wire targets the same id a user
        // route to those rows does, and the two SUM in one evaluator — no second writer per sample.
        static constexpr int kMaxSlots = kUserSlots + 8;
        static constexpr int kEnvAmtSlot = kUserSlots; // Env2 → FiltCutoff (`synth.filt.envAmt`)
        static constexpr int kKeyTrackSlot = kUserSlots + 1; // KeyTrack → FiltCutoff (`synth.filt.keytrack`)
        static constexpr int kMaskEnv1Slot = kUserSlots + 2; // DeltaDecay1 → Osc1Mask (`synth.osc1.maskEnv`)
        static constexpr int kMaskEnv2Slot = kUserSlots + 3; // DeltaDecay2 → Osc2Mask (`synth.osc2.maskEnv`)
        static constexpr int kPwm1Slot = kUserSlots + 4; // pwSrc1 → Osc1Pw (`synth.osc1.pwm`)
        static constexpr int kPwm2Slot = kUserSlots + 5; // pwSrc2 → Osc2Pw (`synth.osc2.pwm`)
        static constexpr int kMaskEnvSubSlot = kUserSlots + 6; // SUB transient → `synth.sub.mask` (`synth.sub.maskEnv`)
        static constexpr int kPwmSubSlot = kUserSlots + 7; // sub pwSrc → `synth.sub.pw` (`synth.sub.pwm`)

        // Assign routing slot `slot` to route `src` → `dest` with bipolar `depth` (clamped to
        // [-1, 1]). Out-of-range slot / src / dest indices are IGNORED (no write). Control-rate.
        //
        // [T-CMP-07 leaf 3] The bound is now the ID SPACE, not the named enum — that is the whole
        // behavioural change in this leaf, and it is additive: every id that was accepted before is
        // accepted now, with the same meaning. The named overload below forwards here.
        void setSlot (int slot, Source src, DestId dest, float depth) noexcept
        {
            if (slot < 0 || slot >= kMaxSlots)
                return;
            const int s = static_cast<int> (src);
            const int d = dest;
            // [D-M-0193] the internal source only on a reserved slot — a user slot is the stored roster's
            const int sourceBound = slot >= kUserSlots ? kSourceStorage : kSourceCount;
            if (s < 0 || s >= sourceBound || d < 0 || d >= kDestCapacity)
                return;

            auto& slotRef = slots_[static_cast<std::size_t> (slot)];
            const bool sameDest = slotRef.active && slotRef.dest == d; // [S5R B31 dsp2 F4] the touched set holds
            slotRef.src = static_cast<std::int16_t> (s);
            slotRef.dest = static_cast<std::int16_t> (d);
            slotRef.depth = depth < -1.0f ? -1.0 : (depth > 1.0f ? 1.0 : static_cast<Real> (depth));
            slotRef.active = true;
            if (!sameDest)
                rebuildTouched();
            rebuildActive(); // FIREFORK E8: the source may change while the destination stays
        }

        // The NAMED form, unchanged for every caller that spells a destination out.
        void setSlot (int slot, Source src, Dest dest, float depth) noexcept
        {
            setSlot (slot, src, static_cast<DestId> (dest), depth);
        }

        // Deactivate a single routing slot (its contribution drops to 0). Control-rate.
        void clearSlot (int slot) noexcept
        {
            if (slot < 0 || slot >= kMaxSlots)
                return;
            slots_[static_cast<std::size_t> (slot)] = Slot {};
            rebuildTouched();
            rebuildActive(); // FIREFORK E8
        }

        // Deactivate every routing slot. Control-rate.
        void clearAllSlots() noexcept
        {
            for (auto& slot : slots_)
                slot = Slot {};
            rebuildTouched();
            rebuildActive(); // FIREFORK E8
        }

        // Set the current value of one source (hot path — a cheap store). Out-of-range ignored.
        void setSourceValue (Source src, float value) noexcept
        {
            const int s = static_cast<int> (src);
            if (s < 0 || s >= kSourceStorage) // [D-M-0193] the internal source is stored too
                return;
            sourceValue_[static_cast<std::size_t> (s)] = static_cast<Real> (value);
        }

        // [T-MOD-01 leaf B] The value a source currently carries — the read-back half of
        // setSourceValue, and it exists for the same reason `modulation` does: a BROADCAST source is
        // fed from outside this object, so "the same number reached all sixteen voices" is only
        // assertable if a voice can be asked what it holds. Out-of-range → 0.
        [[nodiscard]] float sourceValue (Source src) const noexcept
        {
            const int s = static_cast<int> (src);
            if (s < 0 || s >= kSourceStorage)
                return 0.0f;
            return static_cast<float> (sourceValue_[static_cast<std::size_t> (s)]);
        }

        // Compute every destination's summed modulation from the current source values + slot config.
        // Hot path: zero the accumulators the slot table TARGETS, then walk the slot table in index
        // order (deterministic summation). Pure arithmetic — no libm / allocation / locks.
        //
        // [T-CMP-07 leaf 3] THIS USED TO BE `destValue_.fill (0.0)`, AND THREE MEASUREMENTS DECIDED
        // WHAT REPLACED IT. The cost is paid PER SAMPLE PER VOICE, so the 16-voice engine benchmark
        // is the instrument; std dev across runs is ~1.5 %.
        //
        //   `fill`, capacity 18 (the shipped baseline)            340.0 us
        //   `fill`, capacity 128                                  369.9 us   +8.8 %
        //   zero-by-walking-the-slots, capacity 128               353.5 us   +4.0 %
        //   zero-by-walking-the-slots, capacity 18 (the CONTROL)  351.5 us   +3.4 %
        //   zero from a control-rate TOUCHED LIST, capacity 128   340.8 us   +0.2 %  (this code)
        //
        // Two things fall out of the control row, and only one of them was the obvious suspect.
        // Widening the id space is nearly FREE once the zeroing does not scale with it (353.5 vs
        // 351.5, inside noise): the capacity was never the cost. What cost 3.4 % was the fix's own
        // first shape — a second branchy walk over all 22 slots, where the thing it replaced was a
        // single vectorised 144-byte store. So the zeroing walks a list of the DISTINCT destinations
        // actually targeted (typically ~8, never more than 22), built once per slot edit at control
        // rate. Reasoning ahead of the benchmark predicted "under one percent" for the widening and
        // said nothing about the second walk; both halves of that were wrong. The final row is
        // inside noise of the baseline, so a 7x wider destination space costs nothing measurable.
        //
        // BIT-EXACTNESS, which is this leaf's actual claim: the accumulate pass below is byte for
        // byte the loop that was always here, in the same slot-index order, so every destination is
        // the same sequence of additions on the same values. Only the zeroing moved.
        //
        // AND THE HALF THAT IS NOT IN THIS FUNCTION. A destination that stops being targeted is
        // never visited here, so it would hold its LAST value forever — a routing outliving its
        // slot, which is a worse bug than the cost this saves. `rebuildTouched` zeroes the OUTGOING
        // list before rebuilding, which is why it is the one place slot edits go through.
        //
        // [T-CGUI-08] `synth.mtx.bypass` scales the USER slots (0..kUserSlots-1) by `userSlotGain_`;
        // the six hard-wires stay whole (owner 2026-09-23: keytrack, Env-amt and PWM survive an MTX
        // bypass). At unity the loop is the built one, expression for expression, so no golden moves.
        void evaluate() noexcept
        {
            for (int i = 0; i < touchedCount_; ++i)
                destValue_[static_cast<std::size_t> (touched_[static_cast<std::size_t> (i)])] = 0.0;
            if constexpr (fork::kExact) // FIREFORK E8: the active slots only, in slot order (the sums are unchanged)
            {
                if (userSlotGain_ == 1.0)
                {
                    for (int a = 0; a < activeCount_; ++a)
                    {
                        const auto& slot = slots_[static_cast<std::size_t> (activeIdx_[static_cast<std::size_t> (a)])];
                        destValue_[static_cast<std::size_t> (slot.dest)] += sourceValue_[static_cast<std::size_t> (slot.src)] * slot.depth;
                    }
                    return;
                }
                for (int a = 0; a < activeCount_; ++a)
                {
                    const int i = activeIdx_[static_cast<std::size_t> (a)];
                    const auto& slot = slots_[static_cast<std::size_t> (i)];
                    const Real v = sourceValue_[static_cast<std::size_t> (slot.src)] * slot.depth;
                    destValue_[static_cast<std::size_t> (slot.dest)] += i < kUserSlots ? v * userSlotGain_ : v;
                }
                return;
            }
            if (userSlotGain_ == 1.0)
            {
                for (const auto& slot : slots_)
                {
                    if (!slot.active)
                        continue;
                    destValue_[static_cast<std::size_t> (slot.dest)] +=
                        sourceValue_[static_cast<std::size_t> (slot.src)] * slot.depth;
                }
                return;
            }
            for (int i = 0; i < kMaxSlots; ++i)
            {
                const auto& slot = slots_[static_cast<std::size_t> (i)];
                if (!slot.active)
                    continue;
                const Real v = sourceValue_[static_cast<std::size_t> (slot.src)] * slot.depth;
                destValue_[static_cast<std::size_t> (slot.dest)] += i < kUserSlots ? v * userSlotGain_ : v;
            }
        }

        // FIREFORK: true when no slot is active. Then evaluate() changes nothing (rebuildTouched zeroed whatever left the
        // route list), so a caller may skip feeding and evaluating it.
        [[nodiscard]] bool idle() const noexcept { return touchedCount_ == 0; }
        // FIREFORK E8: whether any active slot reads `src` (always true without NEMESIS_FORK_EXACT).
        [[nodiscard]] bool reads (Source src) const noexcept
        {
            return !fork::kExact || ((srcMask_ >> static_cast<unsigned> (src)) & 1u) != 0u;
        }

        // [T-CGUI-08] The MTX bypass fade's gain on the user slots, set per sample by the voice.
        void setUserSlotGain (Real gain) noexcept { userSlotGain_ = gain; }
        [[nodiscard]] Real userSlotGain() const noexcept { return userSlotGain_; }

        // The summed modulation offset for `dest` from the last evaluate(). Out-of-range → 0.
        [[nodiscard]] float modulation (DestId dest) const noexcept
        {
            if (dest < 0 || dest >= kDestCapacity)
                return 0.0f;
            return static_cast<float> (destValue_[static_cast<std::size_t> (dest)]);
        }
        [[nodiscard]] float modulation (Dest dest) const noexcept
        {
            return modulation (static_cast<DestId> (dest));
        }

        // ---- Slot read-back (test/metering anchors, like modulation() above) ----------------------
        // [T-CORE-14] The core→dsp routing edge has to be pinnable against an INDEPENDENT
        // transcription of Param Ref §2.6's Init patch — src, dest AND depth per slot — and a
        // behavioural probe cannot separate "slot 3 routes S&H→ConvRate at +26" from any other
        // routing that happens to sum to the same offset. Out-of-range → the inactive answer.
        [[nodiscard]] bool slotActive (int slot) const noexcept
        {
            if (slot < 0 || slot >= kMaxSlots)
                return false;
            return slots_[static_cast<std::size_t> (slot)].active;
        }
        [[nodiscard]] int slotSrc (int slot) const noexcept
        {
            if (!slotActive (slot))
                return -1;
            return slots_[static_cast<std::size_t> (slot)].src;
        }
        [[nodiscard]] int slotDest (int slot) const noexcept
        {
            if (!slotActive (slot))
                return -1;
            return slots_[static_cast<std::size_t> (slot)].dest;
        }
        [[nodiscard]] float slotDepth (int slot) const noexcept
        {
            if (!slotActive (slot))
                return 0.0f;
            return static_cast<float> (slots_[static_cast<std::size_t> (slot)].depth);
        }

        // Zeroes the source values and destination outputs. Does NOT change the slot config. RT-safe.
        void reset() noexcept
        {
            sourceValue_.fill (0.0);
            destValue_.fill (0.0);
        }

    private:
        // [T-CMP-07 leaf 3] The DISTINCT destinations the slot table currently targets — what
        // `evaluate` zeroes each sample, and the reason that zeroing no longer scales with the id
        // space. Rebuilt on every slot edit, which is control-rate; the O(22^2) distinct-check is
        // 484 integer compares in the worst case and never touches the audio path.
        //
        // IT ZEROES ONLY THE DESTINATIONS THAT LEFT THE LIST, and that is the whole correctness argument:
        // a destination dropped from the list is never visited by `evaluate` again, so this is its last
        // chance to go back to 0. One still targeted is NOT zeroed: it keeps this sample's sum until the
        // next `evaluate` re-sums it (a writer inside the voice's generic loop may still read it).
        void rebuildTouched() noexcept
        {
            const auto outgoing = touched_;
            const int outgoingCount = touchedCount_;
            touchedCount_ = 0;
            for (const auto& slot : slots_)
            {
                if (!slot.active)
                    continue;
                bool seen = false;
                for (int i = 0; i < touchedCount_ && !seen; ++i)
                    seen = touched_[static_cast<std::size_t> (i)] == slot.dest;
                if (!seen)
                    touched_[static_cast<std::size_t> (touchedCount_++)] = slot.dest;
            }
            // [S5R B31 dsp2 F4] Zero only what LEFT the set (an untouched destination holds 0 — `evaluate` never
            // visits it). A destination still touched keeps this sample's sum: a slot edit from a writer inside
            // the voice's generic loop must not zero what the rest of that loop is about to read.
            for (int i = 0; i < outgoingCount; ++i)
            {
                const auto d = outgoing[static_cast<std::size_t> (i)];
                bool stays = false;
                for (int j = 0; j < touchedCount_ && !stays; ++j)
                    stays = touched_[static_cast<std::size_t> (j)] == d;
                if (!stays)
                    destValue_[static_cast<std::size_t> (d)] = 0.0;
            }
        }

        // [D-M-0193] PACKED (24 -> 16 B): the two new SUB hard-wires and the stage ceilings put
        // `sizeof (CathodeMachine)` over the T-CMP-20 eager-storage WARNING tier (370,320 B against
        // 370,085), and the slot table is the one structure here whose padding was paying for nothing —
        // an id fits 16 bits by construction (the static_assert below). 16 voices x 24 slots x 8 B = 3 KB.
        struct Slot
        {
            Real depth = 0.0;
            std::int16_t src = 0;
            std::int16_t dest = 0;
            bool active = false;
        };
        static_assert (kDestCapacity <= 32767 && kSourceStorage <= 32767, "a slot's ids are stored in 16 bits");

        std::array<Slot, kMaxSlots> slots_ {};
        std::array<Real, kSourceStorage> sourceValue_ {};
        std::array<Real, static_cast<std::size_t> (kDestCapacity)> destValue_ {};
        std::array<int, kMaxSlots> touched_ {}; // distinct targeted dests; see rebuildTouched
        int touchedCount_ = 0;
        // FIREFORK E8: the active slots in slot order, and the sources they read.
        static_assert (kSourceStorage <= 64 && kMaxSlots <= 255, "FIREFORK E8: the source mask is 64 bits, slot indices 8");
        std::array<std::uint8_t, kMaxSlots> activeIdx_ {};
        int activeCount_ = 0;
        std::uint64_t srcMask_ = 0;
        void rebuildActive() noexcept
        {
            activeCount_ = 0;
            srcMask_ = 0;
            for (int i = 0; i < kMaxSlots; ++i)
            {
                const auto& slot = slots_[static_cast<std::size_t> (i)];
                if (!slot.active)
                    continue;
                activeIdx_[static_cast<std::size_t> (activeCount_++)] = static_cast<std::uint8_t> (i);
                srcMask_ |= std::uint64_t { 1 } << static_cast<unsigned> (slot.src);
            }
        }
        Real userSlotGain_ = 1.0; // [T-CGUI-08] see evaluate
    };
}
