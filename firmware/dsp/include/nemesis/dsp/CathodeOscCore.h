#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ForkConfig.h>
#include <nemesis/dsp/AdsrEnvelope.h>
#include <nemesis/dsp/BitmaskOscillator.h>
#include <nemesis/dsp/MacroOscillator.h>
#include <nemesis/dsp/MinBlep.h>
#include <nemesis/dsp/Oversampler.h>
#include <nemesis/dsp/OversamplerLanes.h>
#include <nemesis/dsp/PhaseDistortionOscillator.h>
#include <nemesis/dsp/PulseOscillator.h>
#include <nemesis/dsp/SubOscillator.h>
#include <nemesis/dsp/WavetableOscillator.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

namespace nemesis::dsp
{
    // CathodeOscCore — CATHODE's two-oscillator core (CATHODE.md §1). A COMPOSITION of the
    // signed-off oscillator primitives: each of two independent slots is switchable between the
    // band-limited WT-scan oscillator (M1-04 WavetableOscillator), the Casio-CZ phase-
    // distortion oscillator (M1-08 PhaseDistortionOscillator), the Plaits-backed macro
    // oscillator (T-PLAITS-01b/01d MacroOscillator, `synth.osc*.mode = macro` per ADR-008), and
    // the render-time 9-bit bitmask oscillator (T-MASK-03 / ADR-037 BitmaskOscillator).
    // All are driven from one note frequency, each slot detuned by its own octave/semi/fine
    // offset, scaled by a per-slot level, and summed. The per-oscillator DSP is
    // already golden-tested — this layer only adds the pitch-offset math, the mode routing,
    // and the level-scaled sum.
    //
    // Each slot OWNS all four generators (all prepared in prepare()); setMode() just selects
    // which one processSample() reads. Holding all is the RT-safe choice — a mode switch is
    // then a single branch with no re-prepare (no allocation) on the audio thread. Only the
    // active generator is advanced; a mode switch resumes the newly-active oscillator from its
    // frozen state (deterministic; the frozen macro resumes mid block-buffer exactly where it
    // left off).
    //
    // BITMASK mode notes (T-MASK-03 leaf D). The mask NUMBER is the timbre control
    // (`synth.osc{1,2}.mask`, 0..511) and mask 0 is bit-exactly a pure sine — which makes a
    // Bitmask slot at mask 0 the SAME waveform as a PD slot at `pdAmount = 0`, since both are a
    // 2048-point sine table read inside a ×4 `dsp::Oversampler`. That is the ADR-037 anchor
    // working, not a collision, and it is why a test that has to tell the two modes apart reads
    // the MODE back rather than comparing renders. The slot also carries ruling 1's per-slot
    // clean/dirty switch (`setMaskOversamplingBypassed`) — a runtime flag, never a re-prepare.
    // The oversampling FACTOR is the primitive's own PITCH-ADAPTIVE default (leaf C2: ×4 below
    // 1100 Hz, ×8 above, committed at reset()), so `prepare`'s `pdOversample` argument is
    // deliberately NOT forwarded to it: that figure is the PD slot's contract.
    //
    // MACRO mode notes: MacroOscillator adds NO signal latency (nothing new to report or
    // compensate) but its control changes are block-quantised (<= 12 samples). Its RNG is a
    // per-instance counter pinned into stmlib's thread_local LCG each block (T-CORE-28). Each
    // slot's seed comes from `setMacroSeed`; [T-CGUI-44] the engine gives every (voice, slot) its
    // own, so two slots or two voices on one RNG-driven engine no longer render one stream.
    //
    // Tuning (§8): slot frequency = noteHz · 2^(octave + semitones/12 + cents/1200). The exp2 is
    // CONTROL-RATE (resolved in the setters, like Svf coeffs); the per-sample hot path is pure
    // arithmetic. octave+1, semitones+12 and cents+1200 all yield an exact ×2 multiplier, so
    // they are interchangeable to the last bit.
    //
    // Sum [T-CGUI-80, owner 2026-09-30; `synth.oscMix` retired]: out = level0·osc0 + level1·osc1.
    // The per-slot LEVEL is the only oscillator gain. `setOsc2ModOnly` drops slot 1 from the sum
    // (`setOsc2SumGain` fades it) while it still modulates (FM, ring) at its level; level 0 mutes a slot AND its modulation.
    //
    // LATENCY / ALIGNMENT: a WT slot has zero latency; a PD slot — and a band-limited BITMASK
    // slot — has the decimating-FIR group delay of its Oversampler (§11), which for a signal
    // GENERATED inside the shaper is the fractional `generatedLatencySamples()` figure and not
    // the pass-through cascade's 2M. The core does NOT sample-align the two paths: both
    // oscillators are free-running, so a fixed latency offset between them is only an (inaudible)
    // initial-phase difference and stays fully deterministic (bounce-parity holds). Reporting a
    // single core latency for PDC and any cross-slot alignment are later voice/engine concerns
    // (hard sync / cross-mod, §1 "later add", are out of scope here too).
    //
    // RT-safety: prepare() builds/allocates both primitives' tables (off the audio thread);
    // setters resolve tuning/mix at control rate; processSample() is inline, pure arithmetic
    // (it only calls the two primitives' inline hot paths) — no libm calls, no allocation, no
    // locks. Deterministic → bit-identical re-render. Computed in double, emitted as float. No
    // JUCE. Not thread-safe against prepare().
    class CathodeOscCore
    {
    public:
        // [T-MASK-03 leaf D] `Bitmask` is APPENDED at index 3 and the order is FROZEN: the
        // registry's `kOscModes` stores this ordinal, so a reorder re-maps saved sound (the
        // enum-row subset law, `lessons/registry-and-harness.md`).
        enum class Mode : std::uint8_t {
            Wavetable,
            PhaseDistortion,
            Macro,
            Bitmask,
            // [T-CMP-15 leaf C] `Pulse` is APPENDED at index 4 and the order is FROZEN for exactly
            // the reason index 3 was: `kOscModes` stores this ordinal, so a reorder re-maps saved
            // sound. It is the FIFTH value — the card that asked for it said "a 4th value alongside
            // WT/PD/Macro" and predates `Bitmask`.
            Pulse
        };

        // [T-CGUI-57, owner 2026-09-26] THREE slots: OSC 1 · OSC 2 are the summed pair, and slot 2
        // is SUB's — a full oscillator slot the sub switches to when `SubMode` is not `Sub`. It is
        // read AFTER the pair's sum, exactly where the sub always summed, so `modOnly`, FM, ring, hard
        // sync and OSC 1/2's envelopes stay the pair's (`kNumMixSlots`) and never reach it — SUB has its
        // own (`oscEnv (kSubSlot)`, T-CGUI-58 leaf 4).
        static constexpr int kNumMixSlots = 2;
        static constexpr int kSubSlot = 2;
        static constexpr int kNumSlots = 3;

        // [T-CGUI-57] `synth.sub.mode`'s roster, ORDINAL-FOR-ORDINAL (`kSubModes` in the registry):
        // `Sub` = the three simple waves (`SubOscillator`, today's sub), then `Mode` + 1.
        enum class SubMode : std::uint8_t {
            Sub,
            Wavetable,
            PhaseDistortion,
            Macro,
            Bitmask,
            Pulse
        };
        static constexpr int kSubMinOctave = -2;
        static constexpr int kSubMaxOctave = 0;
        static constexpr int kSubDefaultOctave = -1; // = SubOscillator::kOctaveDown, today's ×0.5

        // [T-CMP-03 leaf 1] The RING cross-mod's oversampling factor. 4 is the house default the PD
        // shaper already uses; 1 is a TRUE bypass (`Oversampler` runs the shaper once, unfiltered),
        // which is the falsifying control the alias test needs and NOT a shipping setting.
        static constexpr int kDefaultXmodOversample = 4;

        // [T-CMP-03 leaf 2d] The Macro carrier's pitch-modulation depth at `synth.xmod.fm` = 1,
        // in semitones either side. Owner-tunable, like T-CORE-14's `kPitchModSemitones`.
        static constexpr Real kMacroFmSemitones = 12.0;
        static constexpr int kMaxXmodOversample = 16;

        // [T-CGUI-63] OSC UNISON (CATHODE §1.3, owner rulings 2026-09-27): up to 8 phases per OSC 1/2
        // slot. `kOscUnisonDetuneCents` is the stack's OWN full-scale TOTAL span — outer members sit at
        // ±50 c at detune 1 — an owner-tunable default, twice the voice unison's 50.
        static constexpr int kMaxOscUnison = 8;
        static constexpr Wide kOscUnisonDetuneCents = 100.0;
        // The fixed seed of the start-phase stream (`unisonStartPhase`). Changing it moves every stack.
        static constexpr std::uint64_t kUnisonPhaseSeed = 0x63C0FFEE5EED0063ull;

        // [T-CGUI-57] SUB's slot starts where the sub does: silent (level 0 = `synth.sub.level`'s
        // default) and one octave down (×0.5, exact) — so a mode switch alone plays today's interval.
        CathodeOscCore() noexcept
        {
            Slot& s = slots_[kSubSlot];
            s.level = 0.0f;
            s.octave = kSubDefaultOctave;
            s.mult = 0.5; // = 2^kSubDefaultOctave, exactly what `updateTuning` resolves
        }

        // Builds/prepares both oscillators in both slots (WT sample rate + PD sine table and
        // decimator for `pdOversample`), and the cross-mod oversampler PAIR. Not RT-safe
        // (allocates) — call like prepareToPlay.
        void prepare (Wide sampleRate,
            int pdOversample = PhaseDistortionOscillator::kDefaultOversample,
            int xmodOversample = kDefaultXmodOversample);

        // Clears both slots' oscillator phase, the PD oversampler state, and the macro's
        // deterministic engine/RNG state. RT-safe (the macro re-Inits over its pre-allocated
        // storage; zero heap).
        void reset() noexcept
        {
            for (Slot& s : slots_)
            {
                s.wt.reset();
                s.pd.reset();
                s.macro.reset();
                s.macro.armTrigger(); // [b39 A] a voice's percussive ALGO slot rests silent, never drones while idle
                s.bm.reset(); // clears the phasor AND commits the pitch-adaptive factor (leaf C2)
                s.pulse.reset(); // [T-CMP-15] one phase, read twice — so one reset
            }
            for (std::size_t i = 0; i < static_cast<std::size_t> (kNumMixSlots); ++i)
            {
                pdStage_[i] = {};
                if (pdSeat_[i] != nullptr)
                    pdSeat_[i]->drop(); // [T-CMP-37 L3] the scalar decimator was just zeroed: a lane's copy of the old state is dead
            }
            // [D-M-0208] The sync pair restarts with the oscillators: the master with OSC 2's phase,
            // the synced copy re-seeded from it, the pending correction dropped.
            syncMasterPhase_ = 0.0;
            syncMasterRead_ = 0.0;
            syncSlavePhase_ = 0.0;
            syncCrossU_ = -1.0;
            syncBlep_.reset();
            syncSeedPending_ = true;
            sub_.reset(); // [T-CMP-02] the sub is a fourth generator on the same clock
            for (AdsrEnvelope& e : oscEnv_)
                e.reset(); // [T-CGUI-58 leaf 2]
            xmodA_.reset(); // [T-CMP-03] the lockstep pair's delay lines
            xmodB_.reset();
            aPhase_.fill (0.0f);
            fmA_.reset(); // [T-CMP-03 leaf 2a] the FM leg's own pair
            fmB_.reset();
            mPhase_.fill (0.0f);
            seedUnisonPhases(); // [T-CGUI-63] the stack restarts at its fixed seeded phases
            stackFresh_ = true; // [row 323] nothing is sounding: the next sample lands any collapse at once
            xmodFresh_ = true; // [T-CAL-23 G1] …and any live cross-mod leg
            stackMatch_.fill (1.0f);
            width_ = widthTarget_; // [S5R 2] nothing sounding: the width lands
            sidePrev_ = { false, false }; // [stereo voice S4] the side decimators restart clean (`stackSampleSide`)
            sideRingPrev_ = false;
            sideRingNow_ = false;
            sideNow_ = { false, false };
            sideLive_ = false;
        }

        // [T-MASK-03 leaf D] The NOTE-ON hook, and today it does exactly one thing: commit each
        // Bitmask slot's armed pitch-adaptive oversampling factor (leaf C2). It deliberately does
        // NOT touch phase — this core's two slots are free-running across notes by design (see the
        // LATENCY / ALIGNMENT note; the start-phase question is T-CMP-10's, not this leaf's), and
        // `reset()` remains the one call that zeroes phase.
        //
        // It exists because a CATHODE voice's note-on does not reset its oscillators, so C2's
        // "committed at reset()" contract had no reachable caller in the instrument: every voice
        // decided its factor once inside prepare(), at frequency 0, and never escalated.
        // RT-safe (called from the audio thread's note-on).
        void commitPendingMaskFactors() noexcept
        {
            for (Slot& s : slots_)
                s.bm.commitPendingFactor();
            if (sideOs_ != nullptr) // [stereo voice S4] the side twins clear with the decimators they mirror
                for (std::size_t i = 0; i < static_cast<std::size_t> (kNumMixSlots); ++i)
                {
                    sideOs_->bmBase[i].reset();
                    sideOs_->bmHigh[i].reset();
                }
        }

        // Selects which oscillator a slot reads. Control-rate; zero-alloc (both are prepared).
        // [T-CMP-18 leaf B] RE-PUSHES the pitch: `updateFrequency` writes only the active mode's
        // generator, so the one switched to must be told the frequency here or it plays stale.
        // FIREFORK: process-wide switch, set BEFORE prepare(). Off (default) = every slot prepares its Plaits block up
        // front (upstream). On = defer it until a slot uses Macro; saves 54.8 KB per voice for patches that never do.
        static void setLazyMacro (bool on) noexcept { lazyMacro_ = on; }
        [[nodiscard]] static bool lazyMacro() noexcept { return lazyMacro_; }

        void setMode (int slot, Mode mode) noexcept
        {
            // FIREFORK: with lazy macro on, the ~27 KB Plaits block is allocated the first time a slot is switched to
            // Macro, not in prepare(). That allocates: call this from the main loop, never the audio interrupt.
            if (mode == Mode::Macro && lazyMacro_ && !slots_[idx (slot)].macro.isPrepared())
                slots_[idx (slot)].macro.prepare (sampleRate_);
            slots_[idx (slot)].mode = mode;
            updateFrequency (idx (slot));
            if (idx (slot) == 0)
                syncSeedPending_ = true; // [D-M-0208] the new generator's synced copy re-seeds
        }

        // Binds a Wavetable to a slot's WT oscillator (non-owning; must outlive the core and not
        // be rebuilt while in use). Control-rate.
        // [T-CGUI-74 leaf 2] NO FREQUENCY RE-APPLY: `WavetableOscillator::setWavetable` keeps `inc_`
        // (it zeroes phase only), so `updateFrequency` stays the one writer of the slave's pitch. The
        // re-apply this replaced wrote the UNSYNCED `freqHz`, so a bank change under sync dropped the
        // slave to the master pitch until the next pitch write — tested as a re-bind render pair.
        void setWavetable (int slot, const Wavetable& wt) noexcept
        {
            Slot& s = slots_[idx (slot)];
            s.wt.setWavetable (wt);
            s.wtBound = true;
        }

        // WT morph position in [0,1] for a slot. Control-rate.
        void setPosition (int slot, float pos) noexcept { slots_[idx (slot)].wt.setPosition (pos); }

        // PD warp knee in [0,1] for a slot (§1.2 pdShape). Control-rate.
        void setPdShape (int slot, float shape) noexcept { slots_[idx (slot)].pd.setPdShape (shape); }

        // PD warp depth in [0,1] for a slot (§1.2 pdAmount; 0 = clean sine). Control-rate.
        void setPdAmount (int slot, float amount) noexcept { slots_[idx (slot)].pd.setPdAmount (amount); }

        // [T-CGUI-10 leaf B] The PD type for a slot (`synth.osc{1,2}.pdType`; CATHODE §1.2). Control-rate.
        void setPdType (int slot, PhaseDistortionOscillator::PdType type) noexcept { slots_[idx (slot)].pd.setPdType (type); }

        // [T-MASK-03 leaf D] The BITMASK mask number for a slot — 0..511, wrapped into the 9-bit
        // space by the primitive so a modulated request that runs off either end folds rather than
        // reading bits nobody defined. Control-rate but O(1) and allocation-free, because this is a
        // MODULATION destination (leaf F's delta-decay envelope drives it at audio rate).
        void setMask (int slot, int mask) noexcept { slots_[idx (slot)].bm.setMask (mask); }

        // The per-slot clean/dirty switch (ruling 1): true = the naive base-rate readout, aliasing
        // and all. RT-safe — a flag on the primitive, never a re-`prepare()`.
        void setMaskOversamplingBypassed (int slot, bool bypassed) noexcept
        {
            slots_[idx (slot)].bm.setOversamplingBypassed (bypassed);
        }

        // Macro-mode engine + macro controls for a slot (ADR-008 house labels; harmonics/
        // timbre/morph clamped [0,1] upstream). Control-rate; changes land at the macro's next
        // 12-sample block boundary.
        void setMacroEngine (int slot, MacroOscillator::Engine engine) noexcept { slots_[idx (slot)].macro.setEngine (engine); }
        // [T-CGUI-44] The slot macro's RNG seed; takes effect at once and on every reset().
        void setMacroSeed (int slot, std::uint32_t seed) noexcept { slots_[idx (slot)].macro.setSeed (seed); }
        void setMacroHarmonics (int slot, float value) noexcept { slots_[idx (slot)].macro.setHarmonics (value); }
        void setMacroTimbre (int slot, float value) noexcept { slots_[idx (slot)].macro.setTimbre (value); }
        void setMacroMorph (int slot, float value) noexcept { slots_[idx (slot)].macro.setMorph (value); }

        // Per-slot pitch offsets (§8). Each recomputes the slot's frequency multiplier.
        void setOctave (int slot, int octaves) noexcept
        {
            slots_[idx (slot)].octave = octaves;
            updateTuning (idx (slot));
        }
        void setSemitones (int slot, int semis) noexcept
        {
            slots_[idx (slot)].semitones = semis;
            updateTuning (idx (slot));
        }
        void setFine (int slot, Wide cents) noexcept
        {
            slots_[idx (slot)].cents = cents;
            updateTuning (idx (slot));
        }

        // Per-slot output level (clamped >= 0). Control-rate.
        void setLevel (int slot, float level) noexcept { slots_[idx (slot)].level = level < 0.0f ? 0.0f : level; }

        // Base note frequency in Hz for both slots (each detuned by its own offset). Control-rate.
        void setNoteHz (Wide hz) noexcept
        {
            noteHz_ = hz;
            // [T-CGUI-76 S5R] Slot 2 does not sound in `Sub` mode, so it is not retuned there (a routed
            // Pitch pushes every sample); `setSubMode` re-pushes it from `noteHz_` on the way out.
            const int slots = subMode_ == SubMode::Sub ? kNumMixSlots : kNumSlots;
            for (int i = 0; i < slots; ++i)
                updateFrequency (i);
            // [T-CGUI-76 S5R2] …but its Bitmask factor is still ARMED, unconditionally (`updateFrequency`):
            // the note-on commit reads it, and a mid-note switch into Bitmask plays what was committed.
            if (subMode_ == SubMode::Sub)
                slots_[kSubSlot].bm.setFrequency (noteHz_ * slots_[kSubSlot].mult);
            // [T-CMP-02] The sub takes the NOTE frequency, not a slot's; its octave (`setSubOctave`,
            // default −1 = ×0.5) is `SubOscillator`'s own law, applied there. [T-CGUI-57] Slot 2 above
            // is SUB's full-oscillator slot, tuned by the same octave through its `mult`.
            sub_.setNoteHz (hz);
        }

        // [T-CGUI-80] `synth.osc2.modOnly`: true = slot 1 (OSC 2) leaves the SUM but is still read
        // and still feeds FM and ring at its level. It gates the sum ONLY — a slot at level 0 is a
        // full mute, its modulation included (owner 2026-09-30). Control-rate.
        void setOsc2ModOnly (bool on) noexcept { setOsc2SumGain (on ? 0.0f : 1.0f); }
        // [T-CGUI-83] …as the app FADES it: slot 1's share of the SUM, 0..1 (1 = summed, 0 = modOnly; NaN
        // lands on 0). FM and ring still read slot 1 unscaled. Control-rate.
        void setOsc2SumGain (float gain) noexcept { osc2SumGain_ = gain > 0.0f ? (gain < 1.0f ? gain : 1.0f) : 0.0f; }

        // [T-CMP-03 leaf 1] `synth.xmod.ring` — the osc2 -> osc1 ring depth (Param Ref:111, 0..1
        // after the registry's 0..127 curve). The spec reserves ONE direction, so this acts on slot
        // 0's leg only, BEFORE the pair's sum: out0 = (1-d)*s0 + d*(s0*s1).
        //
        // 0 is a BIT-EXACT bypass by construction (plan gate R2, the T-CMP-02 sub's guard shape):
        // the whole path is skipped, so the oversamplers neither run nor advance and every shipped
        // golden is untouched. Control-rate; zero-alloc.
        // ---- HARD SYNC: OSC 2 is the MASTER, OSC 1 the SLAVE (D-M-0208, owner 2026-10-06) --------
        //
        // THE LAW. OSC 1's phase restarts on every OSC 2 cycle, so OSC 1's own pitch (its oct/semi/fine
        // against OSC 2) sets the TIMBRE and OSC 2's sets the period — the classic sync sweep. The
        // amount CROSSFADES the SIGNAL: OSC 1's leg is `(1 − amt)·free + amt·synced`, where `free` is the
        // slot's own generator (exactly what plays at amt 0) and `synced` is a second phase on the same
        // band-limited generator, restarted by the master. A signal blend, not a scaled reset: a partial
        // phase reset is a contraction map that locks the period for ANY amount > 0 (often to a
        // sub-multiple), so it is not a crossfade between the two endpoints at all.
        //
        // This REPLACED the T-CMP-04 self-sync (each slot restarted against its own pitch, its slave at
        // `1 + amt·8` times it), whose `kSyncRange` and ratio law are gone with it.
        //
        // THE MASTER is a phasor at OSC 2's frequency — `slots_[1].freqHz`, i.e. the note, OSC 2's
        // tuning and any Pitch route — advanced EVERY sample whatever the amount, with the same
        // arithmetic `WavetableOscillator` uses, so for a Wavetable/Pulse OSC 2 it is OSC 2's own phase
        // bit for bit from a reset. It is independent of OSC 2's level and `modOnly`: a muted master
        // still syncs, which is how the idiom is played (hear the slave alone).
        //
        // `amt = 0` takes the untouched single-generator path — a BIT-EXACT bypass. SUB never syncs.
        // FM on OSC 1 takes its own leg (`fmLeg`) and the amount is inert there; ring, PD, Pulse and
        // Bitmask compose as they always did (ring multiplies the blended leg). Macro has no phase to
        // restart: a DEFINED bit-exact bypass.
        void setSyncAmount (float amount) noexcept
        {
            const bool was = syncActive();
            syncAmount_ = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
            // ENGAGE: the synced copy is re-seeded onto the trajectory it would be on had sync always
            // been live (`syncSeedPhase`), so the synced leg never depends on when sync was engaged.
            if (!was && syncActive())
                syncSeedPending_ = true;
        }

        [[nodiscard]] float syncAmount() const noexcept { return syncAmount_; }
        [[nodiscard]] bool syncActive() const noexcept { return syncAmount_ > 0.0f; }

        // Read-backs of the sync pair (tests). The master is OSC 2's cycle; the slave phase is OSC 1's
        // SYNCED copy (the free generator's is `phase (0)`).
        [[nodiscard]] Wide syncMasterPhase() const noexcept { return syncMasterPhase_; }
        [[nodiscard]] Wide syncSlavePhase() const noexcept { return syncSlavePhase_; }

        // The frequency a slot's WAVETABLE generator is actually running at, read off the oscillator
        // rather than recomputed (the T-CORE-14 read-back rule). Under the OSC 2-master law no slot is
        // retuned by sync, so this is the slot's own pitch.
        [[nodiscard]] Wide slaveFrequencyHz (int slot) const noexcept
        {
            return slots_[idx (slot)].wt.frequencyHz();
        }

        // THE FALSIFYING CONTROL, and it is production code on purpose (the same shape
        // `setMaskOversamplingBypassed` ships for the bitmask slot): with the band-limiting off,
        // the reset is a raw discontinuity and the alias floor must be MEASURABLY worse. Without
        // a leg that fails, a keystone comparing the corrected render against nothing would pass
        // for a primitive that did nothing at all (T-WT-03 leaf E).
        void setSyncBandlimitBypassed (bool bypassed) noexcept { syncNaive_ = bypassed; }
        [[nodiscard]] bool syncBandlimitBypassed() const noexcept { return syncNaive_; }
        // [D-M-0207 / T-CGUI-118] …and the SLOPE half's own control: off, the restart takes the MinBLEP alone (the
        // value step), the leg the BLAMP's keystone is measured against. Test/listening only, ON by default.
        void setSyncSlopeBandlimitBypassed (bool bypassed) noexcept { syncRampNaive_ = bypassed; }
        [[nodiscard]] bool syncSlopeBandlimitBypassed() const noexcept { return syncRampNaive_; }

        void setXmodRing (float depth) noexcept
        {
            ringDepth_ = depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
            ringLive_ = ringDepth_ > 0.0f;
        }

        [[nodiscard]] float xmodRing() const noexcept { return ringDepth_; }
        // [D-M-0574 review 3] Read-backs: the ring / FM leg's heard share (0 while the leg is dead or priming).
        [[nodiscard]] float ringLegGain() const noexcept { return ringFade_.live && ringFade_.prime == 0 ? ringFade_.gain : 0.0f; }
        [[nodiscard]] float fmLegGain() const noexcept { return fmFade_.live && fmFade_.prime == 0 ? fmFade_.gain : 0.0f; }

        // [T-CMP-03 leaf 2a] `synth.xmod.fm` — the osc2 -> osc1 PHASE-modulation depth (Param Ref:112,
        // "index law at the build card"). The law: the carrier is read at phase
        // `carrierPhase + d * m`, where `m` is slot 1's level-scaled output, so d = 1 is a full-cycle
        // deviation. That bound is what lets the read stay inside `sampleAt`'s [0,1) precondition with
        // two conditional subtracts and no libm.
        //
        // 0 is a BIT-EXACT bypass on the same guard shape as the ring. Control-rate; zero-alloc.
        void setXmodFm (float depth) noexcept
        {
            fmDepth_ = depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
            fmLive_ = fmDepth_ > 0.0f;
        }

        [[nodiscard]] float xmodFm() const noexcept { return fmDepth_; }

        // ---- [T-CAL-23 G1] EAR-ROW A/B SWITCHES — test/listening only, NOT host params, ON by default ----
        // (owner ruling 2026-10-07, after listening; the setters keep the old OFF path reachable.)
        //
        // THE CROSS-MOD EDGE FADE (D-M-0168(1), D-M-0171(3)). At 0 (the old shipped sound): when FM
        // or ring goes live or dead, slot 0 switches between its plain generator and the oversampled
        // cross-mod leg in ONE sample — a splice, because the leg carries the cascade's delay (ring: 16
        // samples) or fractional decimator delay (FM, Wavetable carrier) and starts from a stale or empty
        // FIR. Above 0 (default `kFmEdgeFadeSeconds`, 5 ms), the edge CROSSFADES the two legs over that time instead: the leg is reset and
        // run silent for one FIR length first (primed), then faded in; going off, it keeps the last live
        // depth and fades out. FM fades only on a single (unstacked, unsynced) Wavetable carrier — PD and
        // Bitmask FM run on the generator's own oversampler, Pulse and Macro FM have none, so their FM
        // edge is not a leg switch; a stack's edge is the row-323 dip, the FM leg carried through it (D-M-0505(a)).
        // Ring fades in every mode.
        static constexpr Real kFmEdgeFadeSeconds = 0.005;
        void setXmodEdgeFadeSeconds (Real seconds) noexcept
        {
            xmodFadeSeconds_ = seconds > 0.0 ? seconds : 0.0;
            resolveXmodFade();
        }
        [[nodiscard]] Real xmodEdgeFadeSeconds() const noexcept { return xmodFadeSeconds_; }

        // THE STACK/SINGLE LEVEL MATCH (D-M-0171(1)). A stack near detune 0 sums its members nearly
        // coherently at their CURRENT phases, so its level is anything from far under to above the
        // single oscillator's, and a detune crossing 0 steps the level inside the 5 ms collapse fade
        // (measured −16 to +14 dB). On: at a switch DETUNE caused, the incoming arm starts at the outgoing
        // arm's current level and glides back to unity over `kStackMatchGlideSeconds` (default); a switch
        // FM, ring, sync, Macro or the voice count caused lands at unity, as Off does. Off: unity.
        static constexpr Wide kStackMatchGlideSeconds = 0.3;
        // [D-M-0539..0542] A voice-count edge's length: the member-gain morph. [D-M-0549] Public because the engine's parameter ramp
        // (`EngineCore::kSmoothingSeconds`) must not outlast it: a detune ramp to 0 that starts with a count edge then
        // lands inside it and keeps the count's cause (unity), which an app test pins.
        static constexpr Real kStackXfadeSeconds = 0.010;
        void setStackLevelMatch (bool on) noexcept
        {
            stackMatchOn_ = on;
            if (!on)
                stackMatch_.fill (1.0f);
        }
        [[nodiscard]] float stackLevelMatchGain (int slot) const noexcept
        {
            return slot >= 0 && slot < kNumMixSlots ? stackMatch_[static_cast<std::size_t> (slot)] : 1.0f;
        }
        // [D-M-0506, owner ruling 2026-10-08] …on SAW-LIKE slots only (see `stackSawLike`): the match models a
        // saw, and on another spectrum a near-cancelling stack swells or dips for the whole glide. A/B switch,
        // test/listening only, ON by default; off is the B28 sound, the match on every carrier.
        void setStackLevelMatchSawOnly (bool on) noexcept { stackMatchSawOnly_ = on; }
        [[nodiscard]] bool stackLevelMatchSawOnly() const noexcept { return stackMatchSawOnly_; }

        // ─── [T-CMP-02] The SUB oscillator (`synth.sub.{wave,level}`) ──────────────────────────
        //
        // [T-CGUI-57] In `SubMode::Sub` (the default) it is exactly what follows; any other SubMode
        // swaps it for slot 2, a full oscillator slot at the SAME place in the sum.
        //
        // It is NOT one of the OSC 1/2 pair — that is the whole point of the reservation ("the
        // Juno/BS2 low-end anchor without spending osc slot 2"). It sums AFTER the pair, untouched
        // by `modOnly`, FM or ring.
        //
        // Its table is SHARED and bound from outside, exactly like a slot's bank: one three-frame
        // pyramid is ~540 KB and owning one per voice would be 4 MB of identical content. See
        // `SubOscillator.h` for the rest of the contract.
        // [T-CMP-15 leaf C] The PULSE mode's shared saw table, bound exactly like the sub's and for
        // the same reasons: one pyramid, non-owning, built off the audio thread by whoever owns the
        // core. An unbound Pulse slot is SILENT rather than undefined — `slotSample` tests `live()`,
        // the same shape the WT slot's `wtBound` has carried since M1-04.
        // NO RE-APPLY OF FREQUENCY HERE, and its ABSENCE is now the tested part. The first version
        // called `setFrequency` after the bind, on the reasoning — copied from the WT slot's line,
        // deleted at T-CGUI-74 leaf 2 — that `setWavetable` clears the increment. **That premise is false.**
        // `WavetableOscillator::setWavetable` calls `reset()`, which zeroes PHASE only; `inc_`
        // survives and `resolveMip()` re-derives from it. So the re-apply was a guard nothing could
        // break, and its mutation SURVIVED the leaf-C battery — classification (b), dead code.
        //
        // T-CMP-02 met this exact case one primitive over and ruled it the same way
        // (`SubOscillator`'s header): delete a redundant guard rather than label it (T-B26). The
        // order-independence it was there to buy is REAL and still owed, so it is asserted directly
        // in `test_cathode_osc_core.cpp` — where it reads as a claim about the contract rather than
        // as a line of ours nobody can question.
        void setPulseWavetable (const Wavetable& table) noexcept
        {
            for (Slot& s : slots_)
                s.pulse.setWavetable (table);
        }

        // The DUTY for a slot, as a fraction of the cycle spent high. Clamped to the 5–95 % rails by
        // the primitive, which is where Param Ref puts them — a modulated request meets them too.
        void setPulseWidth (int slot, Real duty) noexcept { slots_[idx (slot)].pulse.setDuty (duty); }

        // How much naked saw to blend in alongside the pulse (0..1), a slot at a time.
        void setPulseSawMix (int slot, float mix) noexcept { slots_[idx (slot)].pulse.setSawMix (mix); }

        // [T-PROTO-02 leaf 4] `synth.osc{1,2}.scanOrder` — the REQUESTED half of the scan-order
        // law, per slot. Reaches the WT oscillator ONLY: the law permutes how a POSITION scan walks
        // a pre-rendered table's frames, and the PD, Macro, Bitmask and Pulse modes have no table
        // and no position to permute. Nothing modulates it, so it goes straight through — the
        // `setPulseSawMix` shape above, stated rather than inferred.
        void setScanOrder (int slot, ScanOrder order) noexcept { slots_[idx (slot)].wt.setScanOrder (order); }

        // Read-backs, for the reason `WavetableOscillator::position()` gives: a test that recomputed
        // the clamp itself would pass whether or not the oscillator was ever told.
        [[nodiscard]] Real pulseWidth (int slot) const noexcept { return slots_[idx (slot)].pulse.duty(); }
        [[nodiscard]] float pulseSawMix (int slot) const noexcept { return slots_[idx (slot)].pulse.sawMix(); }
        [[nodiscard]] bool pulseLive (int slot) const noexcept { return slots_[idx (slot)].pulse.live(); }

        void setSubWavetable (const Wavetable& table) noexcept { sub_.setWavetable (table); }
        void setSubWave (SubOscillator::Wave wave) noexcept { sub_.setWave (wave); }
        // [T-CGUI-57] ONE level for SUB whatever its mode — the Sub-mode oscillator's and slot 2's.
        void setSubLevel (float level) noexcept
        {
            sub_.setLevel (level);
            setLevel (kSubSlot, level);
        }

        // [T-CGUI-57] `synth.sub.mode`. `Sub` reads the `SubOscillator` exactly as before; any other
        // value selects slot 2's generator (`setMode`, so the pitch is re-pushed). Control-rate,
        // zero-alloc: every generator in slot 2 was prepared in `prepare()`. Slot 2's own mode is
        // left alone on the way back to `Sub`, so switching back and forth is a single branch.
        void setSubMode (SubMode mode) noexcept
        {
            subMode_ = mode;
            if (mode != SubMode::Sub)
                setMode (kSubSlot, static_cast<Mode> (static_cast<int> (mode) - 1));
        }
        [[nodiscard]] SubMode subMode() const noexcept { return subMode_; }

        // [T-CGUI-57] `synth.sub.oct` (−2…0): the octave below the note for EVERY mode — the Sub-mode
        // oscillator's interval and slot 2's octave. Clamped to the row's rails.
        void setSubOctave (int octaves) noexcept
        {
            const int o = octaves < kSubMinOctave ? kSubMinOctave : (octaves > kSubMaxOctave ? kSubMaxOctave : octaves);
            subOctave_ = o;
            sub_.setOctave (o);
            setOctave (kSubSlot, o);
        }
        [[nodiscard]] int subOctave() const noexcept { return subOctave_; }

        [[nodiscard]] float subLevel() const noexcept { return sub_.level(); }
        [[nodiscard]] SubOscillator::Wave subWave() const noexcept { return sub_.wave(); }
        // Read at the oscillator, not recomputed from `noteHz_` — the same rule `position` and
        // `pdAmount` follow: a sub that was never told still halves correctly in the arithmetic.
        [[nodiscard]] Wide subFrequencyHz() const noexcept { return sub_.frequencyHz(); }
        [[nodiscard]] const Wavetable* subBoundTable() const noexcept { return sub_.boundTable(); }

        [[nodiscard]] Mode mode (int slot) const noexcept { return slots_[idx (slot)].mode; }
        // [T-CORE-14 leaf D] A slot's applied WT morph position (introspection / tests). Held on the
        // WT sub-oscillator whatever the slot's mode is, so it reports what a later switch to WT
        // would use — the same shape as setPosition, which also writes it in every mode.
        [[nodiscard]] float position (int slot) const noexcept { return slots_[idx (slot)].wt.position(); }
        // [T-CORE-16] A slot's applied PD warp depth (introspection / tests). Held on the PD
        // sub-oscillator in every mode, exactly like `position` above and for the same reason —
        // setPdAmount also writes it in every mode, so it reports what a switch to PD would use.
        [[nodiscard]] float pdAmount (int slot) const noexcept { return slots_[idx (slot)].pd.pdAmount(); }
        // [T-CGUI-10 leaf B] The applied PD type (read-back).
        [[nodiscard]] PhaseDistortionOscillator::PdType pdType (int slot) const noexcept { return slots_[idx (slot)].pd.pdType(); }
        // [T-WT-02 leaf C] The Wavetable a slot's WT oscillator is bound to, or nullptr before
        // any bind — see `WavetableOscillator::boundTable` for why the read goes all the way
        // down to the instrument. Reported whatever the slot's MODE is, exactly like
        // `position`/`pdAmount` above, because the binding survives a mode switch.
        [[nodiscard]] const Wavetable* boundTable (int slot) const noexcept
        {
            return slots_[idx (slot)].wt.boundTable();
        }
        // [T-MASK-03 leaf D] The two BITMASK read-backs, held on the bitmask sub-oscillator in
        // every mode for the same reason `position`/`pdAmount` are: the setters write in every
        // mode, so these report what a switch to Bitmask would use.
        [[nodiscard]] int mask (int slot) const noexcept { return slots_[idx (slot)].bm.mask(); }
        [[nodiscard]] bool maskOversamplingBypassed (int slot) const noexcept
        {
            return slots_[idx (slot)].bm.oversamplingBypassed();
        }
        // The oversampling factor a Bitmask slot is RUNNING (leaf C2 makes it a per-note value, so
        // this is the only way to observe whether the adaptive policy is live at all).
        [[nodiscard]] int maskOversampleFactor (int slot) const noexcept
        {
            return slots_[idx (slot)].bm.oversampleFactor();
        }

        // Phase in turns of a slot's ACTIVE oscillator (introspection / tests). A Macro slot
        // has no phase concept (block-rendered vendored engines) — reports 0.0.
        [[nodiscard]] Wide phase (int slot) const noexcept
        {
            const Slot& s = slots_[idx (slot)];
            switch (s.mode)
            {
                case Mode::Macro:
                    return 0.0;
                case Mode::PhaseDistortion:
                    return s.pd.phase();
                case Mode::Bitmask:
                    return s.bm.phase();
                case Mode::Pulse:
                    return s.pulse.phase();
                case Mode::Wavetable:
                    break;
            }
            return s.wt.phase();
        }

        // [T-CMP-03 leaf 2e] The OFFSET WRAP, extracted so it can be TESTED. Both cross-mod carriers
        // that inherit `WavetableOscillator::sampleAt`'s [0,1) precondition owe this, and the
        // precondition is the caller's by design — `sampleAt` refuses to clamp so that a wrong offset
        // fails loudly rather than quietly serving a different instant.
        //
        // WHY IT IS A NAMED FUNCTION AND NOT TWO PAIRS OF INLINE `if`s. Its mutation battery found
        // the pair UNKILLABLE from the audio: violating it makes the oscillator read a table index
        // that is out of range but lands in NEIGHBOURING TABLE DATA, so the render differs in the
        // sixth significant figure and no spectrum, duty or amplitude claim can see it. It is real
        // UNDEFINED BEHAVIOUR — UBSan traps it as a pointer-arithmetic overflow in `blendMip` — but
        // the gate's sanitizer preset runs UBSan in RECOVER mode, so it prints and the run still
        // exits 0. Rather than leave a load-bearing law observable by nothing, the law is lifted to
        // a pure function and asserted directly.
        //
        // IT IS A TOTAL WRAP, AND IT DID NOT START THAT WAY. The first version used two conditional
        // subtracts and justified them: "the ramp term is under one turn and the modulation term is
        // bounded by depth <= 1, so the input lies in (-1, 2)". **The second half of that was
        // false.** The modulation term is `depth * slot1's output`, and a slot's output is scaled by
        // `setLevel`, which clamps at zero and has NO UPPER BOUND — so a level above 1, or any
        // generator that leaves [-1, 1], puts `off` outside the assumed domain, the two subtracts do
        // not bring it back, and `sampleAt` indexes past its frame. Caught by the FULL GATE, on the
        // committed tree, by the generated apply-table sweep that drives every row: UBSan reported a
        // pointer-arithmetic overflow in `blendMip` and the process aborted.
        //
        // The lesson is the shape of the original comment: it stated a bound and reasoned from it
        // without checking that anything enforced it. `std::floor` is the house's own answer here
        // (`PhaseDistortionOscillator::wrap01`, `BitmaskOscillator::wrap01`) and it lowers to a
        // single arm64 `frintm` — not a libm call, the same sanctioned category as BBD's `fsqrt` —
        // so totality costs nothing the conditionals were saving.
        [[nodiscard]] static Wide wrapOffsetTurns (Real off) noexcept
        {
            return off - std::floor (off);
        }

        // ---- [T-CGUI-58 leaf 2] PER-OSCILLATOR AMP ENVELOPES (owner 2026-09-23 / 2026-09-25) ----
        // Each slot's output × its own DAHDSR, BEFORE the OSC 1/2 mix; the voice AMP envelope stays
        // the final VCA and still ends the voice. OSC 2's envelope shapes OSC 2 as a MODULATOR too
        // (owner 2026-09-25): it is applied to `s1` where it is read, so the FM input, the ring leg
        // and the mix all see it. OSC 1's is applied after FM and ring, before the mix — on the FM
        // path its output is generated inside the leg. OFF (the default) does not multiply, so it is
        // bit-exact; the gates and the tick always reach both envelopes, so switching one on mid-note
        // joins the note where it stands.
        [[nodiscard]] AdsrEnvelope& oscEnv (int slot) noexcept { return oscEnv_[envIdx (slot)]; }
        void setOscEnvOn (int slot, bool on) noexcept { oscEnvOn_[envIdx (slot)] = on; }
        [[nodiscard]] bool oscEnvOn (int slot) const noexcept { return oscEnvOn_[envIdx (slot)]; }
        // `fromSilence`: the voice is starting from zero AMP gain (idle or stolen), so a per-osc
        // release that outlived the AMP one is CUT (owner: "a longer per-osc release is cut") rather
        // than resumed. `restrike`: the Mono-RT retrigger — these restrike WITH AMP (owner 2026-09-25).
        void oscEnvGateOn (bool restrike, bool fromSilence) noexcept
        {
            // [T-CGUI-63] A note from SILENCE restarts every unison member at its fixed seeded phase — the
            // voice is silent, so the jump cannot click. A legato / re-strike gate leaves them running.
            // [D-M-0544] …and lands any unison edge still under way (a count edge, a collapse dip) and the level
            // match at once, as `reset()` does: resumed, the edge would play on under the attack with members the
            // reseed just moved. Settled, both are no-ops.
            if (fromSilence)
            {
                seedUnisonPhases();
                stackFresh_ = true;
                stackMatch_.fill (1.0f);
                width_ = widthTarget_; // [S5R 2] the voice is silent: the width lands
                xmodFresh_ = true; // [D-M-0574 review 2] …and a cross-mod edge still fading, as `reset()` lands it
                // [review 3] …from a CLEAN history, as `reset()` does: a leg landed live reads its FIR and phase at once,
                // and a depth that went live while the voice was skipped would read a stale (or never-run) history.
                xmodA_.reset();
                xmodB_.reset();
                aPhase_.fill (0.0f);
                fmA_.reset();
                fmB_.reset();
                mPhase_.fill (0.0f);
                // [final review] …and the side ring's restart flags, as `reset()` clears them. Belt and braces: a live
                // ring collapses OSC 1's stack (`stackLive`) and this landing ends any collapse dip at once, so no side
                // reaches the ring leg on the next sample — the osc-core twin case lands mid-dip and is equal either way.
                sideRingPrev_ = false;
                sideRingNow_ = false;
            }
            for (AdsrEnvelope& e : oscEnv_)
            {
                if (fromSilence)
                    e.reset();
                if (restrike)
                    e.restrike();
                else
                    e.gateOn();
            }
        }
        void oscEnvGateOff() noexcept
        {
            for (AdsrEnvelope& e : oscEnv_)
                e.gateOff();
        }
        void silenceOscEnvelopes() noexcept // [b39 review 1] a preset load's cut: Idle, phases untouched
        {
            for (AdsrEnvelope& e : oscEnv_)
                e.reset();
        }

        // ---- [T-CGUI-63] OSC UNISON — the per-oscillator detuned stack (CATHODE §1.3) ----------------
        // A stack of up to `kMaxOscUnison` PHASES on the slot's own generator, not extra voices. OSC 1/2
        // only — a SUB-slot call is ignored. `voices = 1` never enters the stack path, so it is the
        // single oscillator bit for bit. The stack COLLAPSES to 1 (`unisonActiveVoices`) at detune 0, on a
        // Macro slot and on both slots while FM, ring or sync is live; the requested count is kept.
        //
        // Detune: member k of N runs at the slot's live frequency × `unisonMultiplier (slot, k)` =
        // 2^(detune · kOscUnisonDetuneCents · (k/(N−1) − ½) / 1200) — on the shared multiplier, so it
        // stacks on the note, oct/semi/fine, voice-unison detune and pitch routes, and follows them
        // every sample. Level: constant power with BLEND (`unisonGainFor`). Phases: fixed and seeded
        // (`unisonStartPhase`), restored at reset() and at a note from silence; a member joining a
        // sounding stack starts at its own, and a count rising from ONE starts on the rise set
        // (`unisonRiseOffset`). Control-rate setters, cheap enough to be written every sample (two exp2
        // on a detune move, one sqrt on a blend move); zero-alloc.
        //
        // [D-M-0539..0542, owner 2026-10-09] A COUNT CHANGE ON A SOUNDING STACK IS A MEMBER-GAIN MORPH on the
        // one generator, never a splice, a dip or a second arm: over `kStackXfadeSeconds` (a smoothstep) every
        // member glides from its gain and pitch to the new count's — arriving members from 0 at their start
        // phase, leaving ones to 0 before the count shrinks, the rest re-matched to the nearest new pitch so
        // few move at all. The SINGLE VOICE is a member too (mult 1, at the generator's phase), so a count
        // crossing one is the same glide, on Wavetable, Pulse, PD and Bitmask alike (one decimator); once it
        // is alone it hands its phase back to the generator, which plays on from there. The request reads
        // back at once. Nothing of the stack sounding (collapsed, or fresh) lands it in place.
        void setUnisonVoices (int slot, int voices) noexcept
        {
            if (slot < 0 || slot >= kNumMixSlots)
                return;
            Stack& st = stacks_[static_cast<std::size_t> (slot)];
            const int n = voices < 1 ? 1 : (voices > kMaxOscUnison ? kMaxOscUnison : voices);
            if (n == st.voices)
                return;
            const int from = st.voices;
            st.voices = n;
            StackDip& d = stackDips_[static_cast<std::size_t> (slot)];
            if (!stackFresh_ && d.shown)
                startMorph (slot, from); // the stack sounds: its members glide
            else if (!stackFresh_ && d.dir == 0 && stackLive (slot) && soloOk (slot))
            {
                // the single generator sounds and the stack may: it becomes the stack's lone member, at its own phase
                st.phase[0] = generatorPhase (slot);
                st.gain[0] = 1.0f;
                st.members = 0;
                d.shown = true;
                startMorph (slot, from);
            }
            else
                landLayout (slot, from); // nothing of the stack sounds: in place
        }
        void setUnisonDetune (int slot, float amount01) noexcept
        {
            if (slot < 0 || slot >= kNumMixSlots)
                return;
            Stack& st = stacks_[static_cast<std::size_t> (slot)];
            const float d = amount01 < 0.0f ? 0.0f : (amount01 > 1.0f ? 1.0f : amount01);
            if (d == st.detune)
                return;
            st.detune = d;
            resolveUnisonMultipliers (st);
        }
        void setUnisonBlend (int slot, float blend01) noexcept
        {
            if (slot < 0 || slot >= kNumMixSlots)
                return;
            Stack& st = stacks_[static_cast<std::size_t> (slot)];
            const float b = blend01 < 0.0f ? 0.0f : (blend01 > 1.0f ? 1.0f : blend01);
            if (b == st.blend)
                return;
            st.blend = b;
            resolveUnisonGains (st);
        }

        // Read-backs — the REQUESTED count, the count SOUNDING after the collapse rules, the applied
        // detune/blend, and member k's multiplier, gain and running phase (1 / 1 / 0 off the stack).
        [[nodiscard]] int unisonVoices (int slot) const noexcept
        {
            return slot >= 0 && slot < kNumMixSlots ? stacks_[static_cast<std::size_t> (slot)].voices : 1;
        }
        [[nodiscard]] int unisonActiveVoices (int slot) const noexcept { return stackLive (slot) ? unisonVoices (slot) : 1; }
        [[nodiscard]] float unisonDetune (int slot) const noexcept
        {
            return slot >= 0 && slot < kNumMixSlots ? stacks_[static_cast<std::size_t> (slot)].detune : 0.0f;
        }
        [[nodiscard]] float unisonBlend (int slot) const noexcept
        {
            return slot >= 0 && slot < kNumMixSlots ? stacks_[static_cast<std::size_t> (slot)].blend : 1.0f;
        }
        [[nodiscard]] Real unisonMultiplier (int slot, int member) const noexcept
        {
            return memberOk (slot, member) ? stacks_[static_cast<std::size_t> (slot)].mult[static_cast<std::size_t> (member) + 1] : 1.0;
        }
        [[nodiscard]] float unisonGain (int slot, int member) const noexcept
        {
            return memberOk (slot, member) ? stacks_[static_cast<std::size_t> (slot)].gain[static_cast<std::size_t> (member) + 1] : 1.0f;
        }
        [[nodiscard]] Wide unisonPhase (int slot, int member) const noexcept
        {
            return memberOk (slot, member) ? stacks_[static_cast<std::size_t> (slot)].phase[static_cast<std::size_t> (member) + 1] : 0.0;
        }

        // THE LEVEL LAW: the centre member(s) — the middle one, or the two middle ones when N is even —
        // weigh 1, the rest `blend`; each gain is weight / √(Σ weight²), so Σ gain² = 1 at every N and
        // blend. blend 1 → 1/√N each (the voice unison's `stackGainFor`); blend 0 → the centre alone.
        [[nodiscard]] static float unisonGainFor (int member, int voices, float blend) noexcept
        {
            if (voices <= 1)
                return 1.0f;
            const bool even = voices % 2 == 0;
            const int nCentre = even ? 2 : 1;
            const bool centre = even ? (member == voices / 2 - 1 || member == voices / 2) : member == (voices - 1) / 2;
            const Real b = static_cast<Real> (blend);
            const Real norm = 1.0 / std::sqrt (static_cast<Real> (nCentre) + static_cast<Real> (voices - nCentre) * b * b);
            return static_cast<float> ((centre ? 1.0 : b) * norm);
        }

        // THE START PHASES: one fixed seeded stream (splitmix64 over (slot, member)), in [0, 1). The
        // same table for every voice — deterministic, never random per note.
        [[nodiscard]] static Wide unisonStartPhase (int slot, int member) noexcept
        {
            std::uint64_t z = kUnisonPhaseSeed
                              + static_cast<std::uint64_t> (slot * kMaxOscUnison + member + 1) * 0x9E3779B97F4A7C15ull;
            z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
            z ^= z >> 31u;
            return static_cast<Real> (z >> 11u) * (1.0 / 9007199254740992.0); // 2^-53
        }

        // [D-M-0539, owner ruling 2026-10-09] THE RISE SET: where member k of a count rising from ONE starts, in
        // turns from the single voice's phase — −λ·u_k with u_k = (k − (N−1)/2)/N, at the smallest λ ≥ 0 where the
        // members' summed fundamental F(λ) = Σ g_k cos 2πλu_k is exactly 1. u and the level law's gains are symmetric,
        // so the sum is REAL: the stack starts with the single voice's fundamental, amplitude AND phase, and the
        // morph between them (both ends 1, every gain linear in one weight) holds it there. The members above the
        // note start BEHIND it (the sign), and each member's detune offset is proportional to its u, so the set
        // runs toward coherence: F(λ − ct) — the fundamental rises from the single voice's, comes back to it at
        // twice that time, then beats as any stack does. No dip. F(0) = Σ g_k ≥ 1 (Σ g_k² = 1), and F's mean
        // over its period (2N in λ) is the centre's gain (odd N) or 0, so F falls to 1 unless it is 1 throughout
        // (one member sounding: λ = 0). At an edge only — a 1/32 scan and a bisection, a few dozen cos.
        [[nodiscard]] static Real unisonRiseOffset (int member, int voices, float blend) noexcept
        {
            if (voices <= 1 || member < 0 || member >= voices)
                return 0.0;
            return -riseSpread (voices, blend) * riseUnit (member, voices);
        }

        // Produces one mixed base-rate sample. Hot path: two primitive reads + their sum,
        // plus the sub's read when it is live.
        float processSample() noexcept { return processStereo().mid; }

        // [stereo voice S4, D-M-0188] One sample as MID and SIDE: the voice's left is mid + side, its right
        // mid − side, so ½(L + R) is the mid — the mono sum — exactly as wide as it is narrow. The mid is
        // `processSample`'s value bit for bit; the side is the unison members panned by `setUnisonWidth`
        // (see `unisonPan`), and exactly 0 — never computed — unless `sideLive()` after the call.
        struct StereoOut
        {
            float mid = 0.0f;
            float side = 0.0f;
        };
        StereoOut processStereo() noexcept
        {
            beginSample();
            return finishSample();
        }

        // [T-CMP-37 L3] `processStereo` IN TWO HALVES, so the engine can run the PD oscillators' DECIMATORS for four voices at once
        // (OversamplerLanes::decimateSteps). BEGIN is the per-sample prologue (the width / stack-collapse followers, the sync
        // master) and then, for each PD slot whose plain generator is CERTAIN to be read this sample, the generator's OS-rate steps
        // (`PhaseDistortionOscillator::generateSteps`) — they depend on no signal, only on the slot's own phasor and coefficients,
        // so they can run before the rest. FINISH is the rest; a staged slot's single arm takes its decimated sample from the lane
        // phase (`setPdLaneResult`) or, with none, decimates its own steps. `processStereo` is begin → finish, and with no lane
        // the staged path is the same floats pushed in the same order into the same FIR as `s.pd.processSample ()`.
        void beginSample() noexcept
        {
            sideLive_ = false;
            slotSide_ = { 0.0f, 0.0f };
            sidePrev_ = sideNow_;
            sideNow_ = { false, false };
            sideRingPrev_ = sideRingNow_;
            sideRingNow_ = false;
            followWidth();
            followStackCollapse();
            advanceSyncMaster(); // [D-M-0208] OSC 2's cycle runs whatever the sync amount
            stagePd (1);
            stagePd (0);
        }

        // ---- [T-CMP-37 L3] the lane-wise PD decimator's seam (CathodeVoiceEngine::runPdLanes) ---------------------------------
        // Whether slot `slot`'s generator ran in BEGIN and awaits its decimation, and its `factor` OS-rate steps.
        [[nodiscard]] bool pdStaged (int slot) const noexcept { return pdStage_[static_cast<std::size_t> (slot)].staged; }
        [[nodiscard]] const float* pdSteps (int slot) const noexcept { return pdStage_[static_cast<std::size_t> (slot)].steps.data(); }
        // The decimated sample a lane produced for the staged slot (FINISH takes it instead of decimating).
        void setPdLaneResult (int slot, float decimated) noexcept
        {
            PdStage& st = pdStage_[static_cast<std::size_t> (slot)];
            st.out = decimated;
            st.done = true;
        }
        [[nodiscard]] PhaseDistortionOscillator& pdOscillator (int slot) noexcept { return slots_[slot].pd; }
        [[nodiscard]] int pdOscillatorFactor (int slot) const noexcept { return slots_[slot].pd.oversampleFactor(); }
        // The slot's seat in the engine's PD lane batch; the scalar decimator is stale while `resident` — `pullPd` before it is read.
        void setPdLaneSlot (int slot, lanes::OsLaneSlot* seat) noexcept { pdSeat_[static_cast<std::size_t> (slot)] = seat; }
        void pullPd (int slot) noexcept
        {
            if (pdSeat_[static_cast<std::size_t> (slot)] != nullptr)
                pdSeat_[static_cast<std::size_t> (slot)]->pull (slots_[slot].pd.oversampler());
        }
        // Staged slots FINISH left unread: must stay 0 — each one advanced a phasor whose sample nobody took. A probe for tests.
        [[nodiscard]] std::uint64_t pdOrphans() const noexcept { return pdOrphans_; }

        StereoOut finishSample() noexcept
        {
            const StereoOut out = finishBody();
            for (PdStage& st : pdStage_) // a staged slot the flow never reached (the predicate was looser than the flow)
                if (st.staged)
                {
                    st.staged = false;
                    ++pdOrphans_;
                }
            return out;
        }

    private:
        // FIREFORK: a mixing slot may be skipped only when its level is 0 and nothing couples it to the other slot: no FM or ring
        // leg live OR still fading, no hard sync, no priming, and it is not showing a unison stack. Then its contribution is
        // exactly 0 anyway. (xmodFadeStep_ > 0 only means fades are ENABLED; it is the default, so it is not a condition.)
        [[nodiscard]] bool slotUncoupledAndSilent (int slot) const noexcept
        {
            return fork::Config::skipSilentSlots && slots_[slot].level <= 0.0f && !stackDips_[static_cast<std::size_t> (slot)].shown
                   && !fmLive_ && !ringLive_ && syncAmount_ <= 0.0f && !fmFade_.live && !ringFade_.live && fmFade_.prime == 0
                   && ringFade_.prime == 0 && !xmodFresh_;
        }
        Real slotOrSilence (int slot) noexcept
        {
            if (slotUncoupledAndSilent (slot))
            {
                NEMESIS_FORK_COUNT (slotSkips);
                return static_cast<Real> (0.0);
            }
            return static_cast<Real> (slotSample (slot));
        }

        StereoOut finishBody() noexcept
        {
            Real s1 = slotOrSilence (1);
            // [T-CGUI-58] Both envelopes TICK whatever `env.on` says (S5R): an Off one then stands where
            // the note is, so switching it on mid-note joins the note instead of restarting its attack
            // from zero. Only the MULTIPLY is gated, which is what keeps Off bit-exact.
            const float e0 = oscEnv_[0].processSample();
            const float e1 = oscEnv_[1].processSample();
            const float e2 = oscEnv_[kSubSlot].processSample(); // [T-CGUI-58 leaf 4] SUB's, ticked the same way
            if (oscEnvOn_[1])
            {
                s1 *= static_cast<Real> (e1); // the modulator too
                slotSide_[1] *= e1;
            }
            // [T-CMP-03 leaf 2a] The FM leg REPLACES slot 0's generation rather than post-processing
            // it: a phase modulation has to happen where the phase is read, which is inside the
            // carrier. `fmCarrierLive()` is the MODE test — leaf 2a covers the Wavetable carrier, and
            // any other mode falls through to the ordinary generation with the depth inert until
            // leaves 2b-2d reach it (the honest fallback: silent-but-unmodulated, never a wrong
            // waveform). Slot 1 is read FIRST and once, because it is both the modulator and, through
            // the sum, a voice in its own right — reading it twice would advance it twice. It is read
            // even under `modOnly`: the modulator still has to run.
            // [row 323] …unless slot 0's stack is still SOUNDING, fading out of a collapse FM caused; the
            // FM leg takes over at the switch and fades in (see `followStackCollapse`).
            Real s0 = 0.0;
            if (xmodFadeStep_ <= 0.0f) // [T-CAL-23 G1] the shipped instant switch, untouched
            {
                s0 = fmActive() && !stackDips_[0].shown
                         ? static_cast<Real> (stackDips_[0].gain) * static_cast<Real> (fmLeg (static_cast<float> (s1), fmDepth_))
                         : slotOrSilence (0);
                if (ringLive_)
                    s0 = static_cast<Real> (ringLeg (static_cast<float> (s0), static_cast<float> (s1), ringDepth_, sideForRing()));
            }
            else
                s0 = fadedXmodLeg (s1);
            if (oscEnvOn_[0])
            {
                s0 *= static_cast<Real> (e0); // [T-CGUI-58]
                slotSide_[0] *= e0;
            }
            // [T-CGUI-80] THE SUM: levels are already inside `slotSample`, so this is the whole law.
            // `modOnly` drops slot 1 from it here and nowhere else — FM and ring above already read it.
            // [T-CGUI-83] Scaled only while the gate travels, so both rests are the old bits exactly.
            const float g = osc2SumGain_;
            const auto mixed = static_cast<float> (g >= 1.0f ? s0 + s1 : (g <= 0.0f ? s0 : s0 + s1 * static_cast<Real> (g)));
            // The side takes the same sum law; slot 2 (the sub) has no stack, so none. [S5R 3] It is LIVE only where
            // it reaches the sum: a slot at level 0, under a silent osc env or a faded-out arm, and OSC 2 out of the
            // sum (modOnly), carry none — so the voice's right chain is not kept running for a side nobody hears.
            const auto audible = [this] (int slot, float env) noexcept {
                const auto i = static_cast<std::size_t> (slot);
                return sideNow_[i] && slots_[slot].level > 0.0f && stackDips_[i].gain * stackMatch_[i] != 0.0f
                       && (!oscEnvOn_[i] || env > 0.0f);
            };
            const bool side0 = audible (0, e0);
            const bool side1 = g > 0.0f && audible (1, e1);
            sideLive_ = side0 || side1;
            float side = 0.0f;
            if (sideLive_)
            {
                const float a = side0 ? slotSide_[0] : 0.0f;
                side = !side1 ? a : (g >= 1.0f ? a + slotSide_[1] : a + slotSide_[1] * g);
            }
            // [T-CMP-02] The sub joins the OSC SUM, which is what puts it before the broken
            // converter and inside `synth.hpf` (plan gate R2) without either of those stages
            // knowing it exists. Guarded rather than computed: at level 0 the sub neither runs NOR
            // ADVANCES, so a bypassed sub is bit-exact against every shipped golden (adding a
            // literal 0.0f would flip a −0.0f sign bit) and cannot colour the note that switches it
            // on. Same guard shape as `CathodeVoiceStage`'s `hpfLive_`.
            // [T-CGUI-57] …and SUB as a full oscillator takes the same place and the same guard: slot 2
            // at level 0 neither runs nor advances.
            // [T-CGUI-58 leaf 4] SUB's own envelope multiplies whichever generator plays it — Sub mode's
            // `SubOscillator` or slot 2 — and, like OSC 1/2's, only while `env.on` (Off = bit-exact).
            float sub = 0.0f;
            if (subMode_ != SubMode::Sub)
            {
                if (slots_[kSubSlot].level <= 0.0f)
                    return { mixed, side };
                sub = slotSample (kSubSlot);
            }
            else
            {
                if (!sub_.live())
                    return { mixed, side };
                sub = sub_.processSample();
            }
            if (oscEnvOn_[kSubSlot])
                sub *= e2;
            return { mixed + sub, side };
        }

    public:
        // [stereo voice S4] `synth.osc{1,2}.uni.width` in [0, 1]: how far the stack's members spread from the
        // centre (`unisonPan`). 0 — the default — is the mono stack bit for bit: no side is computed at all.
        void setUnisonWidth (int slot, float width01) noexcept
        {
            if (slot < 0 || slot >= kNumMixSlots)
                return;
            const auto i = static_cast<std::size_t> (slot);
            widthTarget_[i] = width01 < 0.0f ? 0.0f : (width01 > 1.0f ? 1.0f : width01);
            if (stackFresh_) // [S5R 2] nothing sounds yet: no step to smooth, the next note starts at the value
                width_[i] = widthTarget_[i];
        }
        // The REQUESTED width (the applied one glides to it — `followWidth`).
        [[nodiscard]] float unisonWidth (int slot) const noexcept
        {
            return slot >= 0 && slot < kNumMixSlots ? widthTarget_[static_cast<std::size_t> (slot)] : 0.0f;
        }
        [[nodiscard]] float appliedUnisonWidth (int slot) const noexcept
        {
            return slot >= 0 && slot < kNumMixSlots ? width_[static_cast<std::size_t> (slot)] : 0.0f;
        }
        // Whether the last `processStereo` computed a side — a stack sounding at width > 0. False: side == 0.
        [[nodiscard]] bool sideLive() const noexcept { return sideLive_; }

        // THE PAN LAW (M1-10d's unisonPan): member k of N sits at 2·(k/(N−1) − ½) ∈ [−1, 1] — the outer
        // members hard left and right at width 1, the rest evenly between, the SUM of the positions 0 under
        // the symmetric level law, so the side carries no mono. One member (and the single voice) is centred.
        [[nodiscard]] static float unisonPan (int member, int voices) noexcept
        {
            if (voices <= 1)
                return 0.0f;
            return static_cast<float> (2 * member - (voices - 1)) / static_cast<float> (voices - 1);
        }

        // [T-CGUI-122] NOTE-ON for the percussive ALGO engines: strikes each slot that PLAYS in Macro mode
        // (`MacroOscillator::strike` is a no-op on the sustained engines). SUB only while its mode is Macro.
        void strikeMacros() noexcept
        {
            for (int i = 0; i < kNumSlots; ++i)
                if (slots_[i].mode == Mode::Macro && (i != kSubSlot || subMode_ == SubMode::Macro))
                    slots_[i].macro.strike();
        }

    private:
        struct Slot
        {
            WavetableOscillator wt;
            PhaseDistortionOscillator pd;
            MacroOscillator macro; // move-only (pimpl) — the Slot/core follow
            BitmaskOscillator bm; // [T-MASK-03 leaf D] ADR-037's render-time mask oscillator
            PulseOscillator pulse; // [T-CMP-15 leaf C] the continuous-duty pulse (R1's two-saw read)
            Mode mode = Mode::PhaseDistortion; // safe default: self-contained, needs no table bind
            bool wtBound = false;

            int octave = 0;
            int semitones = 0;
            Wide cents = 0.0;
            Wide mult = 1.0; // 2^(octave + semitones/12 + cents/1200)
            Wide freqHz = 0.0; // noteHz · mult
            Wide incKeyHz = -1.0, incKeySr = -1.0, incCached = 0.0; // FIREFORK E13: freqHz / sampleRate_, cached

            float level = 1.0f;
        };

        static int idx (int slot) noexcept { return slot < 0 ? 0 : (slot >= kNumSlots ? kNumSlots - 1 : slot); }
        // [T-CGUI-58 leaf 4] One envelope per slot — OSC 1 · OSC 2 · SUB (`kSubSlot`, every SUB mode).
        static std::size_t envIdx (int slot) noexcept
        {
            return static_cast<std::size_t> (slot < 0 ? 0 : (slot >= kNumSlots ? kNumSlots - 1 : slot));
        }
        // [D-M-0208] Only OSC 1 is ever synced: OSC 2 is the master and SUB plays unsynced.
        [[nodiscard]] bool slotSynced (int slot) const noexcept { return syncActive() && slot == 0; }

        // Applies a slot's resolved frequency to its ACTIVE oscillator. [T-CMP-18 leaf B] It used to
        // write all five, and on a routed Pitch this runs every sample: four generators' `log2`
        // mip/note resolves were paid per sample and never read (`slotSample` ticks one). `setMode`
        // re-runs this, so a switch still finds the right pitch. Control-rate otherwise.
        // FIREFORK E13: the same expression as before, evaluated only when the frequency or the rate moved.
        Wide stackInc (Slot& s) noexcept
        {
            if constexpr (!fork::kExact)
                return sampleRate_ > 0.0 ? s.freqHz / sampleRate_ : 0.0;
            if (s.freqHz != s.incKeyHz || sampleRate_ != s.incKeySr)
            {
                s.incKeyHz = s.freqHz;
                s.incKeySr = sampleRate_;
                s.incCached = sampleRate_ > 0.0 ? (fork::kApprox ? s.freqHz * invSampleRate_ : s.freqHz / sampleRate_) : 0.0;
            }
            return s.incCached;
        }

        void updateFrequency (int slot) noexcept
        {
            Slot& s = slots_[slot];
            s.freqHz = noteHz_ * s.mult;
            // [D-M-0208] Sync retunes nothing: OSC 2's pitch is the master's increment (the same
            // `freq / rate` `WavetableOscillator::setFrequency` computes, so the two phasors agree to
            // the bit) and OSC 1's is its synced copy's.
            if (slot == 1)
                syncMasterInc_ = fork::kApprox ? s.freqHz * invSampleRate_ : s.freqHz / sampleRate_; // FIREFORK N3
            else if (slot == 0)
                syncSlaveInc_ = fork::kApprox ? s.freqHz * invSampleRate_ : s.freqHz / sampleRate_; // FIREFORK N3
            switch (s.mode)
            {
                case Mode::Wavetable:
                    s.wt.setFrequency (s.freqHz);
                    break;
                case Mode::Pulse:
                    s.pulse.setFrequency (s.freqHz);
                    break;
                case Mode::PhaseDistortion:
                    s.pd.setFrequency (s.freqHz);
                    break;
                case Mode::Macro:
                    s.macro.setNoteHz (s.freqHz);
                    break;
                case Mode::Bitmask:
                    break; // written below, whatever the mode
            }
            // [T-CMP-04 leaf 6] This also ARMS the adaptive factor that note-on commits. It stays
            // UNCONDITIONAL: a factor armed only while Bitmask is active would commit a stale one
            // after a mid-note switch into Bitmask — a behaviour change.
            s.bm.setFrequency (s.freqHz);
        }

        // Control-rate: recompute a slot's pitch multiplier (uses exp2) and re-apply frequency.
        void updateTuning (int slot) noexcept;

        // Reads one slot's active generator, scaled by its level. Pure arithmetic.
        float slotSample (int slot) noexcept
        {
            // [T-CGUI-63] The stack is a SEPARATE arm, entered only at voices > 1 and not collapsed, so
            // the single-oscillator path below is byte-for-byte what it was (voices 1 = bit-exact).
            // [row 323] What SOUNDS is `shown`, which follows `stackLive` through a fade: a gain of
            // exactly 1 outside one, so both arms are untouched when nothing is crossing.
            // [T-CAL-23 G1] × the level match, exactly 1 unless `setStackLevelMatch` is on — and g·1 is g.
            if (slot < kNumMixSlots)
            {
                const auto i = static_cast<std::size_t> (slot);
                const float g = stackDips_[i].gain * stackMatch_[i];
                if (g != 1.0f)
                {
                    const float y = g * slotArm (slot);
                    slotSide_[i] *= g; // [S4] the side rides the same fade
                    return y;
                }
            }
            return slotArm (slot);
        }

        // The sounding arm of a slot, unfaded: its stack while shown, else the single generator.
        float slotArm (int slot) noexcept
        {
            Slot& s = slots_[slot];
            if (slot < kNumMixSlots && stackDips_[static_cast<std::size_t> (slot)].shown)
            {
                if (slot == 0)
                    syncSeedPending_ = true; // [D-M-0208] the synced copy did not run: re-seed it on return
                const float y = s.level * stackSample (slot, s, stacks_[static_cast<std::size_t> (slot)]);
                slotSide_[static_cast<std::size_t> (slot)] *= s.level; // [S4]
                return y;
            }
            return singleArm (slot);
        }

        // [T-CMP-37 L3] STAGING. A PD slot is staged in BEGIN only when its PLAIN generator — `s.pd.processSample ()`, the single arm's
        // unsynced PD read — is certain to run once this sample: the slot is in PD mode, its stack is not showing (`slotArm` would take
        // the stack path), and for OSC 1 also not synced (`slotSynced`) and not under FM (`fmActive ()`: the FM leg replaces the
        // generator read). With the cross-mod edge fade on (the default), OSC 1's read goes through `fadedXmodLeg`, which for a
        // non-Wavetable carrier takes the plain read unless the FM leg is wanted or still FADING OUT (`fmCarry`: it was live, the
        // stack is on its way out and the carrier is ready) — the same terms, from the same state, evaluated here. Anything looser
        // would advance a phasor nobody reads — `pdOrphans` counts exactly that, and a test holds it at 0. A stricter predicate
        // costs nothing but the lane: the unstaged slot runs the old path. Nothing between BEGIN and the read changes these inputs.
        void stagePd (int slot) noexcept
        {
            PdStage& st = pdStage_[static_cast<std::size_t> (slot)];
            st.staged = false;
            st.done = false;
            if constexpr (fork::kExact) // FIREFORK E12: no lane will read the staged steps
                if (!fork::Config::lanes)
                    return;
            if (slot < kNumMixSlots && slotUncoupledAndSilent (slot)) // FIREFORK: no PD generation for a silent, uncoupled slot
                return;
            const Slot& s = slots_[slot];
            if (s.mode != Mode::PhaseDistortion || stackDips_[static_cast<std::size_t> (slot)].shown)
                return;
            if (slot == 0)
            {
                if (syncActive() || fmActive())
                    return;
                // `fadedXmodLeg` (xmodFadeStep_ > 0): fmWant is false here, so `fm` is fmCarry — a leg that was live (a fresh core
                // resets the fade to fmWant = false) and a collapsing stack with a ready carrier. Then OSC 1 plays the FM leg.
                if (xmodFadeStep_ > 0.0f && !xmodFresh_ && fmFade_.live && stackDips_[0].dir < 0 && fmCarrierReady())
                    return;
            }
            const int factor = s.pd.oversampleFactor();
            if (factor < 2 || factor > PhaseDistortionOscillator::kMaxOsSteps)
                return;
            slots_[slot].pd.generateSteps (st.steps.data());
            st.staged = true;
        }

        // The PD single arm's plain read: a staged slot's decimated sample (the lane's, or its own decimator over the staged steps),
        // else the whole oscillator step as before.
        float pdPlain (int slot, Slot& s) noexcept
        {
            if (slot >= kNumMixSlots) // the SUB slot is never staged
                return s.pd.processSample();
            PdStage& st = pdStage_[static_cast<std::size_t> (slot)];
            if (!st.staged)
                return s.pd.processSample();
            st.staged = false;
            return st.done ? st.out : s.pd.decimateSteps (st.steps.data());
        }

        // The single generator of a slot, scaled by its level (it advances).
        float singleArm (int slot) noexcept
        {
            Slot& s = slots_[slot];
            const bool synced = slotSynced (slot);
            float g = 0.0f;
            switch (s.mode)
            {
                case Mode::Wavetable:
                    // [T-CMP-04 leaf 1] The synced path is a SEPARATE arm rather than a branch
                    // inside the ordinary one, so the un-synced hot path is byte-for-byte what it
                    // was and every committed golden holds (the T-CMP-15 leaf B constraint: a card
                    // that needs a second behaviour has no business moving a golden).
                    g = s.wtBound
                            ? (synced ? syncedSample (s.wt, [&s] (Real ph) noexcept { return readAtPhase (s.wt, ph); })
                                      : s.wt.processSample())
                            : 0.0f;
                    break;
                case Mode::PhaseDistortion:
                    // [T-CMP-04 leaf 5] The PD carrier syncs at the OS RATE, not the base rate,
                    // and takes NO blep — its own decimator band-limits a restart created inside
                    // the oversampled block. Different mechanism from the wavetable and pulse
                    // arms, and deliberately so: this is the one carrier where CATHODE §11's
                    // "oversample sync" is the right law, because the machinery is already in the
                    // path for the CZ warp.
                    g = synced ? syncedPdSample (s) : pdPlain (slot, s);
                    break;
                case Mode::Macro:
                    g = s.macro.processSample();
                    break;
                case Mode::Bitmask:
                    // [T-CMP-04 leaf 6] The LAST carrier, and the only one that needs BOTH laws —
                    // because its band-limiting is a per-note SWITCH. See syncedBitmaskSample.
                    g = synced ? syncedBitmaskSample (s) : s.bm.processSample();
                    break;
                case Mode::Pulse:
                    // The `live()` test is the UNBOUND guard, the WT arm's `wtBound` one generator
                    // over — an unbound pulse has no table to read. It is NOT a level test: the
                    // pulse has no level of its own, `s.level` below is the slot's.
                    // [T-CMP-04 leaf 4] The pulse syncs on the SAME mechanism as the wavetable,
                    // because it IS one: `PulseOscillator` is two non-advancing reads of one
                    // `WavetableOscillator`, so its slave is band-limited by the same M1-04 mip
                    // pyramid and its restart is the same sub-sample phase jump. `sampleAtPhase`
                    // is the pulse's own value-at-phase, so the step is sized off the PULSE and
                    // not off the saw underneath it.
                    g = s.pulse.live()
                            ? (synced ? syncedSample (s.pulse, [&s] (Real ph) noexcept { return s.pulse.sampleAtPhase (ph); })
                                      : s.pulse.processSample())
                            : 0.0f;
                    break;
            }
            return s.level * g;
        }

        // ---- [T-CGUI-63] OSC UNISON internals -------------------------------------------------------
        // Index 0 is the SINGLE VOICE as a member (mult 1) — it sounds only inside a count morph that crosses
        // one, and until it hands back to the generator; member k is at index 1 + k. `members` is how many
        // detuned members render (leaving ones included until the morph lands); the morph moves every entry
        // in [0, 1 + members) from its `From` to its `To` gain and multiplier.
        static constexpr int kStackEntries = kMaxOscUnison + 1;
        struct Stack
        {
            int voices = 1; // the REQUESTED count: the read-back and the collapse rules
            int members = 0; // detuned members rendered, 0 = none
            float detune = 0.0f;
            float blend = 1.0f;
            std::array<Wide, kStackEntries> mult { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
            std::array<float, kStackEntries> gain { 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
            std::array<Wide, kStackEntries> phase {};
            std::array<Wide, kStackEntries> multFrom {};
            std::array<Wide, kStackEntries> multTo {};
            std::array<float, kStackEntries> gainFrom {};
            std::array<float, kStackEntries> gainTo {};
            float morph = 1.0f; // [D-M-0539..0542] the count morph's progress, 0 -> 1; 1 = landed
            // [stereo voice S4] Each entry's pan where the morph began; it glides to `entryPan`'s target with
            // the gains, so a count change moves no member's position in one step. Settled, the law alone.
            std::array<float, kStackEntries> panFrom {};
        };

        // The pan entry x renders at now (0 = the single voice, centred): the law when settled; mid-morph a
        // glide from `panFrom` to the law at `voices` (a LEAVING member keeps its position), with the gains'
        // smoothstep. `voices` is the count the morph runs toward.
        [[nodiscard]] static float entryPan (const Stack& st, int x, int voices) noexcept
        {
            if (x == 0)
                return 0.0f;
            const int layout = voices > 1 ? voices : 0;
            const float to = x - 1 < layout ? unisonPan (x - 1, voices) : st.panFrom[static_cast<std::size_t> (x)];
            if (st.morph >= 1.0f)
                return to;
            const float w = st.morph;
            const float q = w * w * (3.0f - 2.0f * w);
            const float from = st.panFrom[static_cast<std::size_t> (x)];
            return from + (to - from) * q;
        }

        // The phase of the slot's own generator, in the convention its stack members keep (WT/Pulse: the next
        // read; PD/Bitmask: the last one) — so a member there reads what the generator would.
        [[nodiscard]] Wide generatorPhase (int slot) const noexcept
        {
            const Slot& s = slots_[slot];
            switch (s.mode)
            {
                case Mode::Wavetable:
                    return s.wt.phase();
                case Mode::Pulse:
                    return s.pulse.phase();
                case Mode::PhaseDistortion:
                    return s.pd.phase();
                case Mode::Bitmask:
                    return s.bm.phase();
                case Mode::Macro:
                    break;
            }
            return 0.0;
        }

        // May the stack path hold the single voice, and hand it back? Only where the slot's generator path
        // renders exactly what that member does: not Macro, and on OSC 1 no FM (live, or its leg still fading)
        // and no sync, both of which replace the generator's read. Ring post-processes either arm alike.
        [[nodiscard]] bool soloOk (int slot) const noexcept
        {
            return slots_[slot].mode != Mode::Macro && (slot != 0 || (!fmActive() && !fmFade_.live && !syncActive()));
        }

        // The count's member layout, nothing sounding: joining members at their seeds, the rest in place.
        void landLayout (int slot, int from) noexcept
        {
            Stack& st = stacks_[static_cast<std::size_t> (slot)];
            if (st.morph < 1.0f)
                landMorph (slot);
            for (int k = from; k < st.voices; ++k) // a JOINING member starts at its own seeded phase
                st.phase[static_cast<std::size_t> (k) + 1] = unisonStartPhase (slot, k);
            st.members = st.voices > 1 ? st.voices : 0;
            st.gain[0] = 0.0f;
            st.gainTo[0] = 0.0f;
            resolveUnisonMultipliers (st);
            resolveUnisonGains (st);
        }

        // [D-M-0539..0542] Starts the morph toward `voices` from wherever every member stands now (a morph
        // already under way included, so a reversed request turns it around). The members sounding are
        // re-matched to the new layout nearest pitch first; unmatched new members ARRIVE from gain 0 — at their
        // seed, or on the rise set when the single voice sounds alone — and unmatched old ones LEAVE to 0 at
        // their own pitch. Down to one, the single voice arrives on the stack's fundamental phase.
        void startMorph (int slot, int fromVoices) noexcept
        {
            Stack& st = stacks_[static_cast<std::size_t> (slot)];
            const int n = st.voices;
            const int layout = n > 1 ? n : 0;
            const int old = st.members;
            std::array<Wide, kMaxOscUnison> oPhase {}, oMult {}, lMult {};
            std::array<float, kMaxOscUnison> oGain {}, oPan {};
            bool audible = false;
            for (int i = 0; i < old; ++i)
            {
                const auto x = static_cast<std::size_t> (i) + 1;
                oPhase[static_cast<std::size_t> (i)] = st.phase[x];
                oMult[static_cast<std::size_t> (i)] = st.mult[x];
                oGain[static_cast<std::size_t> (i)] = st.gain[x];
                oPan[static_cast<std::size_t> (i)] = entryPan (st, i + 1, fromVoices); // [S4] where it sits now
                audible = audible || st.gain[x] != 0.0f;
            }
            layoutMultipliers (st.detune, n, lMult);
            // [S5R B31 F2] No old member sounds yet (a second push before a sample): none is matched or kept, so
            // every member of the new count arrives as if from the single voice — on ITS rise set, not a stale one.
            const int kept = audible ? old : 0;
            std::array<int, kMaxOscUnison> from {};
            from.fill (-1);
            std::array<bool, kMaxOscUnison> taken {};
            for (int pair = 0; pair < (kept < layout ? kept : layout); ++pair)
            {
                int bi = -1, bj = -1;
                Real best = 0.0;
                for (int j = 0; j < layout; ++j)
                    for (int i = 0; i < kept; ++i)
                    {
                        if (from[static_cast<std::size_t> (j)] >= 0 || taken[static_cast<std::size_t> (i)])
                            continue;
                        const Real dist = std::abs (oMult[static_cast<std::size_t> (i)] - lMult[static_cast<std::size_t> (j)]);
                        if (bj < 0 || dist < best)
                        {
                            best = dist;
                            bi = i;
                            bj = j;
                        }
                    }
                from[static_cast<std::size_t> (bj)] = bi;
                taken[static_cast<std::size_t> (bi)] = true;
            }
            const bool solo = !audible && st.gain[0] != 0.0f; // the single voice alone: a rise from it
            const Real lambda = solo && layout > 0 ? riseSpread (layout, st.blend) : 0.0;
            if (layout == 0 && st.gain[0] == 0.0f) // down to one, the single voice not yet sounding
                st.phase[0] = fundamentalPhase (oPhase.data(), oGain.data(), old, oPhase[0]);
            for (int j = 0; j < layout; ++j)
            {
                const auto x = static_cast<std::size_t> (j) + 1;
                const int i = from[static_cast<std::size_t> (j)];
                if (i >= 0)
                {
                    st.phase[x] = oPhase[static_cast<std::size_t> (i)];
                    st.gainFrom[x] = oGain[static_cast<std::size_t> (i)];
                    st.multFrom[x] = oMult[static_cast<std::size_t> (i)];
                    st.panFrom[x] = oPan[static_cast<std::size_t> (i)];
                }
                else
                {
                    const Real ph = solo ? st.phase[0] - lambda * riseUnit (j, layout) : unisonStartPhase (slot, j);
                    st.phase[x] = ph - std::floor (ph);
                    st.gainFrom[x] = 0.0f;
                    st.multFrom[x] = lMult[static_cast<std::size_t> (j)];
                    st.panFrom[x] = unisonPan (j, n); // arrives from silence: at its own place
                }
                st.gainTo[x] = unisonGainFor (j, n, st.blend);
                st.multTo[x] = lMult[static_cast<std::size_t> (j)];
            }
            int m = layout;
            for (int i = 0; i < kept; ++i)
                if (!taken[static_cast<std::size_t> (i)])
                {
                    const auto x = static_cast<std::size_t> (m++) + 1;
                    st.phase[x] = oPhase[static_cast<std::size_t> (i)];
                    st.gainFrom[x] = oGain[static_cast<std::size_t> (i)];
                    st.gainTo[x] = 0.0f;
                    st.multFrom[x] = oMult[static_cast<std::size_t> (i)];
                    st.multTo[x] = oMult[static_cast<std::size_t> (i)];
                    st.panFrom[x] = oPan[static_cast<std::size_t> (i)]; // leaves where it is
                }
            st.members = m;
            st.gainFrom[0] = st.gain[0];
            st.gainTo[0] = layout == 0 ? 1.0f : 0.0f;
            st.mult[0] = st.multFrom[0] = st.multTo[0] = 1.0;
            for (int x = 1; x <= m; ++x)
            {
                st.gain[static_cast<std::size_t> (x)] = st.gainFrom[static_cast<std::size_t> (x)];
                st.mult[static_cast<std::size_t> (x)] = st.multFrom[static_cast<std::size_t> (x)];
            }
            st.morph = 0.0f;
        }

        // One sample of the morph: every entry a smoothstep of the way from its `From` to its `To`, so the gains
        // have no kink at either end; lands exactly.
        void advanceMorph (int slot) noexcept
        {
            Stack& st = stacks_[static_cast<std::size_t> (slot)];
            st.morph += stackXfadeStep_;
            if (st.morph >= 1.0f)
            {
                landMorph (slot);
                return;
            }
            const float w = st.morph;
            const float q = w * w * (3.0f - 2.0f * w);
            for (std::size_t x = 0; x <= static_cast<std::size_t> (st.members); ++x)
            {
                st.gain[x] = st.gainFrom[x] + (st.gainTo[x] - st.gainFrom[x]) * q;
                st.mult[x] = st.multFrom[x] + (st.multTo[x] - st.multFrom[x]) * static_cast<Real> (q);
            }
        }

        // The morph's end: leaving members are gone, the rest sit on the count's laws exactly (the settled
        // stack's own arithmetic, bit for bit), and the single voice holds its last gain — 1 alone, else 0.
        void landMorph (int slot) noexcept
        {
            Stack& st = stacks_[static_cast<std::size_t> (slot)];
            st.morph = 1.0f;
            st.gain[0] = st.gainTo[0];
            st.members = st.voices > 1 ? st.voices : 0;
            resolveUnisonMultipliers (st);
            resolveUnisonGains (st);
        }

        // The stack path holds the single voice alone: it hands its phase to the slot's generator, which reads
        // there what the member read (WT/Pulse to the ulp, PD/Bitmask bit for bit) — the morph's last step.
        void handBack (int slot) noexcept
        {
            Slot& s = slots_[slot];
            Stack& st = stacks_[static_cast<std::size_t> (slot)];
            const Real p = st.phase[0];
            switch (s.mode)
            {
                case Mode::Wavetable:
                    s.wt.setPhase (p);
                    break;
                case Mode::Pulse:
                    s.pulse.setPhase (p);
                    break;
                case Mode::PhaseDistortion:
                    s.pd.setPhase (p);
                    break;
                case Mode::Bitmask:
                    s.bm.setPhase (p);
                    break;
                case Mode::Macro:
                    break;
            }
            st.gain[0] = 0.0f;
            st.gainTo[0] = 0.0f;
            stackDips_[static_cast<std::size_t> (slot)].shown = false;
        }

        // The rise set's unit spread for member k of n (`unisonRiseOffset`), and its λ for the level law at `blend`.
        [[nodiscard]] static Real riseUnit (int k, int n) noexcept
        {
            return (static_cast<Real> (k) - 0.5 * static_cast<Real> (n - 1)) / static_cast<Real> (n);
        }
        [[nodiscard]] static Real riseSpread (int n, float blend) noexcept
        {
            std::array<Real, kMaxOscUnison> g {};
            Real sum = 0.0;
            for (int k = 0; k < n; ++k)
            {
                g[static_cast<std::size_t> (k)] = static_cast<Real> (unisonGainFor (k, n, blend));
                sum += g[static_cast<std::size_t> (k)];
            }
            const auto fundamental = [&g, n] (Real lambda) {
                Real acc = 0.0;
                for (int k = 0; k < n; ++k)
                    acc += g[static_cast<std::size_t> (k)] * std::cos (2.0 * 3.14159265358979323846 * lambda * riseUnit (k, n));
                return acc;
            };
            if (sum <= 1.0 + 1.0e-9)
                return 0.0;
            Real lo = 0.0, hi = -1.0;
            for (int step = 1; step <= 64 * n; ++step)
            {
                const Real x = static_cast<Real> (step) / 32.0;
                if (fundamental (x) <= 1.0)
                {
                    hi = x;
                    break;
                }
                lo = x;
            }
            if (hi < 0.0)
                return lo; // unreachable by the argument at `unisonRiseOffset`
            for (int i = 0; i < 48; ++i)
            {
                const Real mid = 0.5 * (lo + hi);
                (fundamental (mid) > 1.0 ? lo : hi) = mid;
            }
            return 0.5 * (lo + hi);
        }

        [[nodiscard]] static bool memberOk (int slot, int member) noexcept
        {
            return slot >= 0 && slot < kNumMixSlots && member >= 0 && member < kMaxOscUnison;
        }

        // THE COLLAPSE RULES, read live: a stack sounds only on OSC 1/2 at voices > 1 and detune > 0,
        // off Macro, and while neither FM, ring nor sync is live (both slots are inside the cross pair).
        // [row 318] At detune 0 the members share one frequency, so their fixed seeded phases would sum
        // COHERENTLY — every partial combed anywhere from -inf to +3 dB, where the level law assumes
        // decorrelated members. Undetuned, the stack IS the single oscillator.
        [[nodiscard]] bool stackLive (int slot) const noexcept
        {
            if (slot < 0 || slot >= kNumMixSlots)
                return false;
            const Stack& st = stacks_[static_cast<std::size_t> (slot)];
            if (st.voices <= 1 || !(st.detune > 0.0f))
                return false;
            return slots_[slot].mode != Mode::Macro && !fmLive_ && !ringLive_ && !syncActive();
        }

        // [row 323] A COLLAPSE EDGE IS A FADE, NOT A SPLICE: when `stackLive` changes, the sounding arm
        // (stack or single generator) fades out over `kStackFadeSeconds`, the other takes over at the
        // silent point and fades in — the synth.fold.pos move shape. Two arms never run in one sample. A
        // request that reverses mid-fade turns the fade around. Nothing sounding (after reset) lands at once.
        // A VOICE-COUNT edge is not one of these: it is the member-gain morph (`setUnisonVoices`).
        struct StackDip
        {
            bool shown = false; // the arm sounding: the stack path (true) or the single generator
            float gain = 1.0f; // exactly 1 outside a fade
            int dir = 0; // -1 fading out, +1 fading in, 0 settled
            bool others = false; // `stackOthersLive` at the last sample
            bool otherCause = false; // it moved since the arms last settled: this edge is not detune's
        };
        static constexpr Real kStackFadeSeconds = 0.005;

        void followStackCollapse() noexcept
        {
            for (int slot = 0; slot < kNumMixSlots; ++slot)
            {
                StackDip& d = stackDips_[static_cast<std::size_t> (slot)];
                Stack& st = stacks_[static_cast<std::size_t> (slot)];
                // [D-M-0171(1)] Did a cause OTHER than detune move since the arms last settled?
                const bool others = stackOthersLive (slot);
                const bool othersMoved = others != d.others;
                d.others = others;
                if (stackFresh_)
                {
                    landLayout (slot, st.voices); // any morph lands, the single voice leaves the stack path
                    d = StackDip {};
                    d.shown = stackLive (slot);
                    d.others = others;
                    continue;
                }
                if (st.morph < 1.0f)
                    advanceMorph (slot);
                // [D-M-0539..0542] The stack path holds the single voice alone, settled: the morph's last step.
                if (d.shown && st.voices <= 1 && st.morph >= 1.0f && d.dir == 0 && soloOk (slot))
                    handBack (slot);
                // The stack path is wanted while a stack should sound, or while it carries the single voice down
                // from one — a count edge, so no dip, unless FM/sync/Macro takes the generator meanwhile.
                const bool want = stackLive (slot) || (d.shown && st.voices <= 1 && soloOk (slot));
                // A count edge's cause holds until its morph lands, so a collapse it runs into keeps unity.
                d.otherCause = d.otherCause || othersMoved;
                if (d.dir == 0)
                {
                    if (want == d.shown)
                    {
                        if (st.morph >= 1.0f)
                            d.otherCause = false;
                        continue;
                    }
                    d.dir = -1;
                }
                if (d.dir < 0 && want == d.shown)
                    d.dir = 1; // the request came back: fade the same arm back in
                else if (d.dir > 0 && want != d.shown)
                    d.dir = -1;
                d.gain += static_cast<float> (d.dir) * stackFadeStep_;
                if (d.dir < 0 && d.gain <= 0.0f)
                {
                    d.gain = 0.0f; // silent: the other arm takes over and fades in
                    float& match = stackMatch_[static_cast<std::size_t> (slot)];
                    if (stackMatchOn_ && !d.otherCause && (!stackMatchSawOnly_ || stackSawLike (slot)))
                    {
                        // [T-CAL-23 G1, D-M-0171(1)] the incoming arm starts at the outgoing arm's level —
                        // its CURRENT one, the match still gliding included (dsp-5: a second crossing
                        // inside the glide)
                        const Real r = stackLevelRatio (slot);
                        const Real m = d.shown ? static_cast<Real> (match) * r : static_cast<Real> (match) / r;
                        match = static_cast<float> (m < kStackMatchMin ? kStackMatchMin : (m > 1.0 / kStackMatchMin ? 1.0 / kStackMatchMin : m));
                    }
                    else
                        match = 1.0f; // a non-detune edge, or not a saw: the incoming arm at its own level, as with the match off
                    d.shown = want;
                    d.dir = 1;
                    landLayout (slot, st.voices); // silent: any morph lands; the single voice leaves the stack path
                }
                else if (d.dir > 0 && d.gain >= 1.0f)
                {
                    d.gain = 1.0f; // lands exactly
                    d.dir = 0;
                }
            }
            stackFresh_ = false;
            // [T-CAL-23 G1] Once the dip has landed, the match glides home and SNAPS to exactly 1 within
            // 1e-3 (a 0.009 dB step): in float the glide stalls near 4e-4 off unity, where δ·(1 − coef) is
            // under half an ulp. Held through the fade-in, so the switch itself is level.
            for (std::size_t i = 0; i < stackMatch_.size(); ++i)
                if (float& m = stackMatch_[i]; m != 1.0f && stackDips_[i].dir == 0)
                {
                    m = 1.0f + (m - 1.0f) * stackMatchCoef_;
                    if (std::abs (m - 1.0f) < 1.0e-3f)
                        m = 1.0f;
                }
        }

        // The phase, in turns, of a set of members' summed FUNDAMENTAL: arg Σ g_k e^(2πi φ_k). Every member reads
        // the same waveform, so a single voice at this phase has its fundamental in phase with theirs. At an edge only.
        [[nodiscard]] static Wide fundamentalPhase (const Wide* phase, const float* gain, int n, Real fallback) noexcept
        {
            Real re = 0.0, im = 0.0;
            for (int k = 0; k < n; ++k)
            {
                const Real a = 2.0 * 3.14159265358979323846 * phase[k];
                re += static_cast<Real> (gain[k]) * std::cos (a);
                im += static_cast<Real> (gain[k]) * std::sin (a);
            }
            if (re == 0.0 && im == 0.0)
                return fallback;
            const Real p = std::atan2 (im, re) / (2.0 * 3.14159265358979323846);
            return p < 0.0 ? p + 1.0 : p;
        }

        // ---- [T-CAL-23 G1] the A/B switches' internals ------------------------------------------------
        static constexpr Real kStackMatchMin = 0.25; // ±12 dB: the match never boosts or cuts past it

        // [D-M-0171(1)] `stackLive` with detune left out: voices > 1, off Macro, no FM, ring or sync. An
        // edge is DETUNE's — the only one the level match serves, the one the owner auditioned — when
        // this held still from the arms' last settle to the switch. Any other cause (Macro, FM, ring,
        // sync, the voice count; turning on OR off) changes the sound anyway, and keeps unity.
        [[nodiscard]] bool stackOthersLive (int slot) const noexcept
        {
            return stacks_[static_cast<std::size_t> (slot)].voices > 1 && slots_[slot].mode != Mode::Macro && !fmLive_
                   && !ringLive_ && !syncActive();
        }

        // The stack's rms over the single oscillator's at the members' CURRENT phases, for a BAND-LIMITED
        // SAW spectrum — amplitudes 1/h up to H, the last partial under Nyquist or the mip's top if lower.
        // Two such saws a phase τ apart correlate as w(τ) = Σ_{h≤H} cos(2πhτ)/h² / Σ_{h≤H} 1/h², so the
        // ratio is √(Σ_k Σ_l g_k g_l w(φ_k − φ_l)).
        // Truncating at H matters: a nearly cancelling stack keeps its power in the upper partials, which
        // a high note's table does not carry. At an EDGE only, never per sample: one cos per member pair
        // (≤ 28) and a Chebyshev recurrence over h. A saw is the model spectrum, so other waves are approximate.
        [[nodiscard]] Real stackLevelRatio (int slot) const noexcept
        {
            const Stack& st = stacks_[static_cast<std::size_t> (slot)];
            const Real f = slots_[slot].freqHz;
            const Real hMax = f > 0.0 ? 0.5 * sampleRate_ / f : 1.0;
            int harmonics = hMax < 1.0 ? 1 : (hMax > 1024.0 ? 1024 : static_cast<int> (hMax));
            // A table-read stack carries what its resolved mip keeps, which can be under Nyquist.
            const Slot& sl = slots_[slot];
            const WavetableOscillator* osc = sl.mode == Mode::Wavetable ? &sl.wt : (sl.mode == Mode::Pulse ? &sl.pulse.saw() : nullptr);
            if (osc != nullptr && osc->boundTable() != nullptr)
            {
                const int top = osc->boundTable()->mipTopHarmonic (osc->currentMip());
                harmonics = top < 1 ? 1 : (top < harmonics ? top : harmonics);
            }
            Real norm = 0.0;
            for (int h = 1; h <= harmonics; ++h)
                norm += 1.0 / (static_cast<Real> (h) * static_cast<Real> (h));
            Real acc = 0.0;
            const int first = st.gain[0] != 0.0f ? 0 : 1; // the entries the stack path renders (`stackSample`)
            const int end = 1 + st.members;
            for (int k = first; k < end; ++k)
            {
                const auto gk = static_cast<Real> (st.gain[static_cast<std::size_t> (k)]);
                acc += gk * gk;
                for (int l = k + 1; l < end; ++l)
                {
                    const Real c1 = std::cos (2.0 * 3.14159265358979323846 * (st.phase[static_cast<std::size_t> (k)] - st.phase[static_cast<std::size_t> (l)]));
                    Real prev = 1.0, cur = c1, w = 0.0;
                    for (int h = 1; h <= harmonics; ++h)
                    {
                        w += cur / (static_cast<Real> (h) * static_cast<Real> (h));
                        const Real next = 2.0 * c1 * cur - prev;
                        prev = cur;
                        cur = next;
                    }
                    acc += 2.0 * gk * static_cast<Real> (st.gain[static_cast<std::size_t> (l)]) * w / norm;
                }
            }
            return acc > 0.0 ? std::sqrt (acc) : 0.0;
        }

        // [D-M-0506] SAW-LIKE, the slot the ratio above models: a Wavetable or Pulse slot whose waveform as it reads
        // NOW (frame, position, mip, duty, saw mix) correlates with the core's own band-limited saw — the Pulse
        // table, the ShapeMorph saw `Basics` starts on — to |rho| >= kSawLikeMin against either of
        // the slot's two bracketing mips. PD and Bitmask are not tables, and an unbound slot or reference has
        // nothing to compare: not saw-like. 0.97, measured from 55 Hz to 8 kHz: a saw reads >= 0.984 (`Basics` 3 %
        // toward its square >= 0.983, 10 % 0.69); up to 4.2 kHz a sine reads <= 0.894, a square <= 0.883, `Basics`'
        // triangle 0.05. From ~6 kHz a mip keeps one or two harmonics and every wave passes (the model truncates to
        // the same harmonics). A phase-shifted saw reads as not saw-like and keeps unity. At an edge only, 256 reads.
        static constexpr Real kSawLikeMin = 0.97;
        static constexpr int kSawProbe = 256;
        [[nodiscard]] bool stackSawLike (int slot) const noexcept
        {
            const Slot& s = slots_[slot];
            const bool wt = s.mode == Mode::Wavetable;
            if (wt ? !s.wtBound : !(s.mode == Mode::Pulse && s.pulse.live()))
                return false;
            const WavetableOscillator& own = wt ? s.wt : s.pulse.saw();
            const Wavetable* ref = s.pulse.saw().boundTable();
            const Wavetable* table = own.boundTable();
            if (ref == nullptr || table == nullptr || ref->frameLength() != table->frameLength() || ref->frameLength() % kSawProbe != 0)
                return false;
            const auto stride = static_cast<std::size_t> (ref->frameLength() / kSawProbe);
            const int top = ref->numMips() - 1;
            const int j0 = own.currentMip() < top ? own.currentMip() : top;
            const Real* r0 = ref->frame (0, j0);
            const Real* r1 = ref->frame (0, j0 < top ? j0 + 1 : top);
            Real xx = 0.0, y0 = 0.0, y1 = 0.0, xy0 = 0.0, xy1 = 0.0;
            for (int k = 0; k < kSawProbe; ++k)
            {
                const Real ph = static_cast<Real> (k) / static_cast<Real> (kSawProbe);
                const auto x = static_cast<Real> (wt ? readAtPhase (s.wt, ph) : s.pulse.sampleAtPhase (ph));
                const Real a = r0[static_cast<std::size_t> (k) * stride];
                const Real b = r1[static_cast<std::size_t> (k) * stride];
                xx += x * x;
                y0 += a * a;
                y1 += b * b;
                xy0 += x * a;
                xy1 += x * b;
            }
            const Real floor2 = kSawLikeMin * kSawLikeMin * xx; // rho^2 >= k^2, squared so no sqrt
            return xx > 0.0 && (xy0 * xy0 >= floor2 * y0 || xy1 * xy1 >= floor2 * y1);
        }

        // One leg's crossfade state: its gain (0 = the plain leg alone), whether it is running, and how
        // many more samples it runs silent before the fade starts (the FIR priming).
        struct XmodFade
        {
            float gain = 0.0f;
            float depth = 0.0f; // the last LIVE depth, held while the leg fades out
            int prime = 0;
            bool live = false;
        };

        // Moves a fade one sample toward `want`; true while the leg still has to run.
        static void stepFade (XmodFade& f, bool want, float step) noexcept
        {
            if (f.prime > 0)
            {
                --f.prime;
                return;
            }
            f.gain += want ? step : -step;
            f.gain = f.gain < 0.0f ? 0.0f : (f.gain > 1.0f ? 1.0f : f.gain);
        }

        // [T-CAL-23 G1, D-M-0168(1)/D-M-0171(3)] slot 0's leg with the edge fade ON. FM crossfades only
        // on a single, unsynced, settled Wavetable carrier — the one whose FM leg is a separate
        // oversampled generator: the plain leg is the same table's non-advancing read at the current
        // phase (what `slotSample` would read), taken BEFORE `fmLeg` advances the phasor. Anywhere else
        // the FM choice is the shipped one, unfaded. The ring then crossfades over whatever s0 is.
        // [D-M-0505(b)] Both legs sound at the level match's gain, as `slotSample` does: an FM edge inside a
        // detune-0 match glide is the unmatched edge scaled by it — faded, never a jump from the match to 1.
        Real fadedXmodLeg (Real s1) noexcept
        {
            const float step = xmodFadeStep_;
            const auto s1f = static_cast<float> (s1);
            const bool fmWant = fmActive() && !stackDips_[0].shown;
            const bool crossfadable = slots_[0].mode == Mode::Wavetable && !syncActive() && !stackDips_[0].shown
                                      && stackDips_[0].gain == 1.0f;
            if (fmLive_)
                fmFade_.depth = fmDepth_;
            if (xmodFresh_)
                fmFade_ = { fmWant ? 1.0f : 0.0f, fmFade_.depth, 0, fmWant };

            Real s0 = 0.0;
            if (!crossfadable)
            {
                // [D-M-0505(a)] A UNISON STACK's FM edge is the row-323 dip, and the arm that fades is the one
                // that was SOUNDING. FM going dead re-forms the stack: the single arm fading out is the FM leg,
                // so it keeps sounding at its held depth until the dip is silent (not the plain generator, a
                // splice). FM going live collapses it: while the stack fades out, a Wavetable carrier's leg runs
                // unheard, so it fades in from a full FIR rather than an empty or stale one. PD and Bitmask
                // cannot: their stack shares the generator's one decimator (the row-323 residual, D-M-0168).
                const bool fmCarry = !fmWant && fmFade_.live && !stackDips_[0].shown && stackDips_[0].dir < 0 && fmCarrierReady();
                if (fmActive() && stackDips_[0].shown && slots_[0].mode == Mode::Wavetable)
                    (void) fmLeg (s1f, fmDepth_);
                const bool fm = fmWant || fmCarry;
                fmFade_ = { fm ? 1.0f : 0.0f, fmFade_.depth, 0, fm };
                s0 = fm ? static_cast<Real> (stackDips_[0].gain * stackMatch_[0]) * static_cast<Real> (fmLeg (s1f, fmWant ? fmDepth_ : fmFade_.depth))
                        : slotOrSilence (0);
            }
            else
            {
                if (fmWant && !fmFade_.live)
                {
                    fmA_.reset(); // a clean history, run silent for one FIR length before it is heard
                    fmB_.reset();
                    mPhase_.fill (0.0f);
                    fmFade_ = { 0.0f, fmFade_.depth, fmPrime_, true };
                }
                if (!fmWant && fmFade_.prime > 0) // went dead while still priming: never heard
                    fmFade_ = { 0.0f, fmFade_.depth, 0, false };
                if (fmFade_.live)
                {
                    Slot& s = slots_[0];
                    const Real plain = static_cast<Real> (s.level * s.wt.sampleAt (0.0));
                    const Real fm = static_cast<Real> (fmLeg (s1f, fmFade_.depth));
                    stepFade (fmFade_, fmWant, step);
                    s0 = static_cast<Real> (stackMatch_[0]) * (fmFade_.gain >= 1.0f ? fm : plain + (fm - plain) * static_cast<Real> (fmFade_.gain));
                    if (!fmWant && fmFade_.gain <= 0.0f)
                        fmFade_.live = false;
                }
                else
                    s0 = slotOrSilence (0);
            }

            if (ringLive_)
                ringFade_.depth = ringDepth_;
            if (xmodFresh_)
                ringFade_ = { ringLive_ ? 1.0f : 0.0f, ringFade_.depth, 0, ringLive_ };
            xmodFresh_ = false;
            if (ringLive_ && !ringFade_.live)
            {
                xmodA_.reset();
                xmodB_.reset();
                aPhase_.fill (0.0f);
                ringFade_ = { 0.0f, ringFade_.depth, ringPrime_, true };
            }
            if (!ringLive_ && ringFade_.prime > 0)
                ringFade_ = { 0.0f, ringFade_.depth, 0, false };
            if (ringFade_.live)
            {
                float* const side = sideForRing();
                const float side0 = side != nullptr ? *side : 0.0f;
                const auto ring = static_cast<Real> (ringLeg (static_cast<float> (s0), s1f, ringFade_.depth, side));
                stepFade (ringFade_, ringLive_, step);
                s0 = ringFade_.gain >= 1.0f ? ring : s0 + (ring - s0) * static_cast<Real> (ringFade_.gain);
                if (side != nullptr && ringFade_.gain < 1.0f) // [S4] the side crossfades into its ring as the mid does
                    *side = side0 + (*side - side0) * ringFade_.gain;
                if (!ringLive_ && ringFade_.gain <= 0.0f)
                    ringFade_.live = false;
            }
            return s0;
        }

        // The symmetric linear-cents spread as a geometric series: two exp2 per move rather than N. All
        // `kMaxOscUnison` read 1 at one voice or detune 0.
        static void layoutMultipliers (float detune, int n, std::array<Wide, kMaxOscUnison>& out) noexcept
        {
            if (n <= 1 || detune <= 0.0f)
            {
                out.fill (1.0);
                return;
            }
            const Real c = static_cast<Real> (detune) * kOscUnisonDetuneCents;
            const Real step = std::exp2 (c / (1200.0 * static_cast<Real> (n - 1)));
            Real m = std::exp2 (-c / 2400.0);
            for (int k = 0; k < n; ++k)
            {
                out[static_cast<std::size_t> (k)] = m;
                m *= step;
            }
        }

        // The count's laws: applied when settled (as they always were), else the morph's target for the new
        // layout's members only — the leaving ones, after them, keep theirs.
        static void resolveUnisonMultipliers (Stack& st) noexcept
        {
            std::array<Wide, kMaxOscUnison> l {};
            layoutMultipliers (st.detune, st.voices, l);
            if (st.morph >= 1.0f)
            {
                const int n = st.voices <= 1 || st.detune <= 0.0f ? kMaxOscUnison : st.voices;
                for (int k = 0; k < n; ++k)
                    st.mult[static_cast<std::size_t> (k) + 1] = l[static_cast<std::size_t> (k)];
                return;
            }
            for (int k = 0; k < (st.voices > 1 ? st.voices : 0); ++k)
                st.multTo[static_cast<std::size_t> (k) + 1] = l[static_cast<std::size_t> (k)];
        }

        static void resolveUnisonGains (Stack& st) noexcept
        {
            if (st.morph >= 1.0f)
            {
                for (int k = 0; k < st.voices; ++k)
                    st.gain[static_cast<std::size_t> (k) + 1] = unisonGainFor (k, st.voices, st.blend);
                return;
            }
            for (int k = 0; k < (st.voices > 1 ? st.voices : 0); ++k)
                st.gainTo[static_cast<std::size_t> (k) + 1] = unisonGainFor (k, st.voices, st.blend);
        }

        void seedUnisonPhases() noexcept
        {
            for (int slot = 0; slot < kNumMixSlots; ++slot)
                for (int k = 0; k < kMaxOscUnison; ++k)
                    stacks_[static_cast<std::size_t> (slot)].phase[static_cast<std::size_t> (k) + 1] = unisonStartPhase (slot, k);
        }

        // One sample of the stack path: every entry it renders — the members, and the single voice while it is
        // one of them. WT and Pulse members are non-advancing reads of the slot's own band-limited pyramid at
        // each member's phase (read, then advance — the WT convention); PD and Bitmask sum their members at
        // the OS rate through the slot's one decimator. The slot's own generator does not advance meanwhile.
        float stackSample (int slot, Slot& s, Stack& st) noexcept
        {
            const int first = st.gain[0] != 0.0f ? 0 : 1;
            const int n = 1 + st.members - first;
            if (n <= 0)
                return 0.0f;
            Wide* const phase = st.phase.data() + first;
            const Wide* const mult = st.mult.data() + first;
            const float* const gain = st.gain.data() + first;
            const auto si = static_cast<std::size_t> (slot);
            // [stereo voice S4] the members panned: a separate arm, so width 0 is untouched. [S5R 5] Not before
            // prepare(), which makes the side's decimators: until then the stack plays its mono arm.
            if (width_[si] > 0.0f && sideOs_ != nullptr)
                return stackSampleSide (slot, s, st, first, n);
            switch (s.mode)
            {
                case Mode::PhaseDistortion:
                    return s.pd.processSampleStack (phase, mult, gain, n);
                case Mode::Bitmask:
                    return s.bm.processSampleStack (phase, mult, gain, n);
                case Mode::Wavetable:
                case Mode::Pulse:
                {
                    const bool wt = s.mode == Mode::Wavetable;
                    if (wt ? !s.wtBound : !s.pulse.live())
                        return 0.0f;
                    const Wide inc = stackInc (s);
                    Real sum = 0.0;
                    for (int k = 0; k < n; ++k)
                    {
                        Wide& ph = phase[k];
                        const float v = wt ? readAtPhase (s.wt, ph) : s.pulse.sampleAtPhase (ph);
                        sum += static_cast<Real> (gain[k]) * static_cast<Real> (v);
                        ph += inc * mult[k];
                        ph -= std::floor (ph);
                    }
                    return static_cast<float> (sum);
                }
                case Mode::Macro:
                    break; // collapsed by `stackLive` — never reached
            }
            return 0.0f;
        }

        // [S5R 2] THE WIDTH GLIDES: a linear slew at the stack fade's own rate (`stackFadeStep_`, 5 ms full scale),
        // landing exactly — so a jump (a preset, an automation step, a stepped knob) moves the side over 5 ms, never in
        // one sample. A width that never moved does nothing here: 0 stays 0, bit for bit.
        void followWidth() noexcept
        {
            for (std::size_t i = 0; i < static_cast<std::size_t> (kNumMixSlots); ++i)
            {
                float& w = width_[i];
                const float t = widthTarget_[i];
                if (w == t)
                    continue;
                w = w < t ? (t - w <= stackFadeStep_ ? t : w + stackFadeStep_) : (w - t <= stackFadeStep_ ? t : w - stackFadeStep_);
            }
        }

        // [stereo voice S4] `stackSample` with the side: each entry's gain × its pan × the slot's width
        // weighs the same read into `slotSide_` (the mid is `stackSample`'s arithmetic, untouched).
        float stackSampleSide (int slot, Slot& s, Stack& st, int first, int n) noexcept
        {
            const auto si = static_cast<std::size_t> (slot);
            std::array<float, kStackEntries> sideGain {};
            for (int k = 0; k < n; ++k)
            {
                const int x = first + k;
                sideGain[static_cast<std::size_t> (k)] = st.gain[static_cast<std::size_t> (x)] * entryPan (st, x, st.voices) * width_[si];
            }
            Wide* const phase = st.phase.data() + first;
            const Wide* const mult = st.mult.data() + first;
            const float* const gain = st.gain.data() + first;
            if (!sidePrev_[si]) // the side starts again: no history from its last run
            {
                SideDecimators& d = *sideOs_;
                d.pd[si].reset();
                d.bmBase[si].reset();
                d.bmHigh[si].reset();
            }
            sideNow_[si] = true;
            float side = 0.0f;
            float mid = 0.0f;
            switch (s.mode)
            {
                case Mode::PhaseDistortion:
                    mid = s.pd.processSampleStackSide (phase, mult, gain, sideGain.data(), n, sideOs_->pd[si], side);
                    break;
                case Mode::Bitmask:
                    mid = s.bm.processSampleStackSide (phase, mult, gain, sideGain.data(), n, sideOs_->bmBase[si], sideOs_->bmHigh[si], side);
                    break;
                case Mode::Wavetable:
                case Mode::Pulse:
                {
                    const bool wt = s.mode == Mode::Wavetable;
                    if (wt ? !s.wtBound : !s.pulse.live())
                        break;
                    const Wide inc = stackInc (s);
                    Real sum = 0.0;
                    Real sd = 0.0;
                    for (int k = 0; k < n; ++k)
                    {
                        Wide& ph = phase[k];
                        const float v = wt ? readAtPhase (s.wt, ph) : s.pulse.sampleAtPhase (ph);
                        sum += static_cast<Real> (gain[k]) * static_cast<Real> (v);
                        sd += static_cast<Real> (sideGain[static_cast<std::size_t> (k)]) * static_cast<Real> (v);
                        ph += inc * mult[k];
                        ph -= std::floor (ph);
                    }
                    mid = static_cast<float> (sum);
                    side = static_cast<float> (sd);
                    break;
                }
                case Mode::Macro:
                    break; // collapsed by `stackLive` — never reached
            }
            slotSide_[si] = side;
            return mid;
        }

        // [D-M-0208] THE MASTER: OSC 2's cycle, advanced once per sample with `WavetableOscillator`'s
        // own arithmetic (read-then-advance, one conditional subtract). A wrap records WHERE in the
        // step it happened — `u` in (0,1] after this sample's read — for the slave to restart at.
        void advanceSyncMaster() noexcept
        {
            const Real prev = syncMasterPhase_;
            syncMasterRead_ = prev;
            syncMasterPhase_ += syncMasterInc_;
            syncCrossU_ = -1.0;
            if (syncMasterPhase_ >= 1.0)
            {
                syncMasterPhase_ -= 1.0;
                syncCrossU_ = (1.0 - prev) / syncMasterInc_; // a wrap implies inc > 0
            }
        }

        // THE SEED: where the synced copy stands at this sample's read had it been synced all along —
        // the time since OSC 2's last wrap (`read / masterInc` samples) at OSC 1's increment. Used on
        // engage, after reset/mode switch, and after FM or the stack held OSC 1, so the synced leg is a
        // function of OSC 2's phase and OSC 1's pitch alone: history-free, never re-seeded mid-cycle
        // from wherever OSC 1's free generator happens to stand.
        [[nodiscard]] Wide syncSeedPhase() const noexcept
        {
            if (!(syncMasterInc_ > 0.0))
                return 0.0;
            const Real p = syncSlaveInc_ * (syncMasterRead_ / syncMasterInc_);
            return p - std::floor (p);
        }

        // THE CROSSFADE: OSC 1's leg is `(1 − amt)·free + amt·synced`. At amt 1 the free read is not
        // mixed at all, so full sync is the synced copy exactly.
        [[nodiscard]] float syncBlend (float free, float synced) const noexcept
        {
            const auto d = static_cast<Real> (syncAmount_);
            return d >= 1.0 ? synced : static_cast<float> ((1.0 - d) * static_cast<Real> (free) + d * static_cast<Real> (synced));
        }

        // [D-M-0208, the T-CMP-04 leaf 1/4 mechanism] HARD SYNC at the BASE rate (Wavetable, Pulse, a
        // bypassed Bitmask): the free generator plays as always, and the SYNCED copy is a second phase
        // read non-advancingly through the same resolved mip/frame state, restarted when the master
        // wraps and band-limited by a MinBLEP sized off the waveform the output is made of — and, since
        // D-M-0207, a band-limited ramp (BLAMP) sized off its slope: the half a restart at a non-zero derivative jumps.
        //
        // ORDER MATTERS. `MinBlep::addStep`'s `frac` is measured from the LAST sample returned, so
        // this sample's correction is emitted BEFORE an event after it is queued.
        //
        // Hot path: the carrier's own read, one synced read, four more per restart, `floor`, no
        // allocation. The lambda inlines.
        //
        // `kSlope` false skips the BLAMP — for a carrier whose waveform STEPS inside the cycle (the bypassed
        // Bitmask): a one-sided difference straddling a step reads step / h, a spike (S5R B31 F1).
        template <bool kSlope = true, typename Carrier, typename ReadAt>
        float syncedSample (Carrier& carrier, ReadAt readAt) noexcept
        {
            if (syncSeedPending_)
            {
                syncSlavePhase_ = syncSeedPhase();
                syncBlep_.reset();
                syncSeedPending_ = false;
            }
            const float free = carrier.processSample();
            float synced = static_cast<float> (readAt (syncSlavePhase_)) + syncBlep_.processSample();

            Real next = syncSlavePhase_ + syncSlaveInc_;
            if (syncCrossU_ >= 0.0)
            {
                const Real u = syncCrossU_;
                // The copy's phase at the restart instant, on the trajectory it was on.
                Real pre = next - syncSlaveInc_ * (1.0 - u);
                pre -= std::floor (pre);
                const Real before = static_cast<Real> (readAt (pre));
                const Real after = static_cast<Real> (readAt (0.0));
                // By the next sample the copy has already run (1 − u) of a step past the restart.
                next = syncSlaveInc_ * (1.0 - u);
                if (!syncNaive_)
                {
                    syncBlep_.addStep (after - before, u);
                    // [D-M-0207 / T-CGUI-118] The SLOPE half (the BLAMP): the copy's derivative per sample, the one it
                    // takes at the restart less the one it leaves — one-sided reads of the waveform it plays, a hair
                    // either side, so neither straddles the restart point itself. Its first tap belongs to THIS
                    // sample, still in hand.
                    if (kSlope && !syncRampNaive_)
                    {
                        constexpr Real h = 1.0 / 65536.0;
                        const Real leave = (before - static_cast<Real> (readAt (pre - h))) / h;
                        const Real take = (static_cast<Real> (readAt (h)) - after) / h;
                        synced += static_cast<float> (syncBlep_.addRamp ((take - leave) * syncSlaveInc_, u));
                    }
                }
            }
            syncSlavePhase_ = next - std::floor (next);
            return syncBlend (free, synced);
        }

        // A non-advancing read at an ABSOLUTE phase, via `sampleAt`'s offset form. It exists so
        // the two reads above cannot drift from `processSample()`'s own arithmetic: `sampleAt`
        // shares `readAt` with it by construction (T-CMP-15 leaf B), so there is no second copy
        // of the table read to keep in step.
        [[nodiscard]] static float readAtPhase (const WavetableOscillator& osc, Real ph) noexcept
        {
            Real off = ph - osc.phase();
            off -= std::floor (off); // sampleAt's precondition is [0,1) and it refuses to clamp
            if (off >= 1.0)
                off = 0.0; // the floor above can leave exactly 1.0 for a negative-zero input
            return osc.sampleAt (off);
        }

        // [D-M-0208, the T-CMP-04 leaf 5 mechanism] The PD carrier syncs at the OS RATE and takes NO
        // blep: the restart is placed at the OS instant the master crossing maps to (`u · factor` OS
        // steps into this base sample) and the oscillator's own decimator band-limits it. The free and
        // synced phases are blended INSIDE the OS block, so both share the one decimator — linear, so
        // it equals blending the two decimated signals.
        float syncedPdSample (Slot& s) noexcept
        {
            if (syncSeedPending_)
            {
                syncSlavePhase_ = syncSeedPhase();
                syncSeedPending_ = false;
            }
            const Real restartAtOs = syncCrossU_ >= 0.0 ? syncCrossU_ * static_cast<Real> (s.pd.oversampleFactor()) : -1.0;
            const auto d = static_cast<Real> (syncAmount_);
            return s.pd.processSampleSyncBlend (syncSlavePhase_, restartAtOs, d >= 1.0 ? 0.0 : 1.0 - d, d);
        }

        // [T-CMP-04 leaf 6, carried over] The BITMASK carrier needs BOTH anti-alias laws, because
        // `maskClean` is a per-note switch on its band-limiting and the restart follows whichever side
        // is active:
        //
        //   BYPASSED    -> the base-rate restart with a MinBLEP (`syncedSample`); `sampleAt` is ABSOLUTE.
        //   OVERSAMPLED -> the OS-rate restart and NO blep, the PD mechanism.
        //
        // A carrier whose JOB is to alias (bypassed) cannot prove an anti-alias claim; the claims are
        // pinned on the Wavetable, Pulse and PD carriers (the T-CMP-04 battery's M10-M12 finding).
        float syncedBitmaskSample (Slot& s) noexcept
        {
            if (s.bm.oversamplingBypassed()) // no BLAMP: its waveform steps, and it aliases by design
                return syncedSample<false> (s.bm, [&s] (Real ph) noexcept { return s.bm.sampleAt (ph); });
            if (syncSeedPending_)
            {
                syncSlavePhase_ = syncSeedPhase();
                syncSeedPending_ = false;
            }
            const Real restartAtOs = syncCrossU_ >= 0.0 ? syncCrossU_ * static_cast<Real> (s.bm.activeOversampleFactor()) : -1.0;
            const auto d = static_cast<Real> (syncAmount_);
            return s.bm.processSampleSyncBlend (syncSlavePhase_, restartAtOs, d >= 1.0 ? 0.0 : 1.0 - d, d);
        }

        // [T-CMP-03 leaf 1] The RING leg: a multiply of two band-limited signals is NOT band-limited
        // (CATHODE.md §11 requires cross-mod be oversampled), and `Oversampler::processSample` takes
        // ONE input while a ring needs TWO at the OS rate. So the pair runs in LOCKSTEP, which is
        // composition rather than a new primitive (plan gate R1): both instances carry the same
        // polyphase design and each is fed one base sample per call, so their phase p is the same
        // instant by construction — nothing here aligns them by hand.
        //
        // A interpolates `a` and RECORDS each OS phase; its decimated return is DISCARDED (the one
        // wasted decimation, the named cost of not touching the shared primitive). B interpolates
        // `b` and, at each OS phase, mixes dry against product IN THE OVERSAMPLED DOMAIN — plan gate
        // R2a: mixing a base-rate dry leg with B's cascade-delayed output would comb-filter. B's
        // decimated return IS the leg.
        //
        // Hot path: two FIR cascades, no libm, no allocation (both lambdas capture by value/ref and
        // inline into the template).
        // [stereo voice S4] `side`, when given, is OSC 1's side and rings through the SAME law with the same
        // modulator OS steps — on twins of the pair (`SideDecimators::ringA/B`), so it is band-limited and
        // delayed as the mid is. The ring is linear in the carrier, so ring(mid ± side) = ring(mid) ± this.
        float ringLeg (float a, float b, float depth, float* side = nullptr) noexcept
        {
            int p = 0;
            const auto recordA = [this, &p] (float os) noexcept {
                aPhase_[static_cast<std::size_t> (p)] = os;
                ++p;
                return os;
            };
            if constexpr (fork::kExact) // FIREFORK E2: A's decimation was discarded; only its OS phases are used
                xmodA_.upsampleOnly (a, recordA);
            else
                (void) xmodA_.processSample (a, recordA);
            std::array<float, kMaxXmodOversample> sideUp {};
            std::array<float, kMaxXmodOversample> sideOs {};
            if (side != nullptr)
            {
                int ps = 0;
                const auto recordSide = [&sideUp, &ps] (float os) noexcept {
                    sideUp[static_cast<std::size_t> (ps++)] = os;
                    return os;
                };
                if constexpr (fork::kExact) // FIREFORK E2
                    sideOs_->ringA.upsampleOnly (*side, recordSide);
                else
                    (void) sideOs_->ringA.processSample (*side, recordSide);
            }

            int q = 0;
            const float d = depth; // [T-CAL-23 G1] the caller's: `ringDepth_`, or the held one in a fade-out
            const float out = xmodB_.processSample (b, [this, &q, d, side, &sideUp, &sideOs] (float os) noexcept {
                const auto k = static_cast<std::size_t> (q);
                const float av = aPhase_[k];
                ++q;
                if (side != nullptr)
                    sideOs[k] = (1.0f - d) * sideUp[k] + d * (sideUp[k] * os);
                return (1.0f - d) * av + d * (av * os);
            });
            if (side != nullptr)
            {
                int r = 0;
                *side = sideOs_->ringB.decimateSample ([&sideOs, &r]() noexcept { return sideOs[static_cast<std::size_t> (r++)]; });
            }
            return out;
        }

        // OSC 1's side for `ringLeg`, or null: only while a side was computed for it this sample. The side ring's
        // twins restart clean when it starts again (no history from its last run).
        float* sideForRing() noexcept
        {
            if (!sideNow_[0] || sideOs_ == nullptr)
                return nullptr;
            if (!sideRingPrev_)
            {
                sideOs_->ringA.reset();
                sideOs_->ringB.reset();
            }
            sideRingNow_ = true;
            return &slotSide_[0];
        }

        // [T-CMP-03 leaf 2a] Is the FM leg live AND does slot 0's mode have a carrier this leaf can
        // read? Both halves matter: `fmLive_` alone would take the path for a Macro slot, which has
        // no phase to offset at all.
        [[nodiscard]] bool fmActive() const noexcept { return fmLive_ && fmCarrierReady(); }

        // [D-M-0505(a)] The carrier half of `fmActive`, alone: a held-depth leg fading out needs a carrier, not a live depth.
        [[nodiscard]] bool fmCarrierReady() const noexcept
        {
            switch (slots_[0].mode)
            {
                case Mode::Wavetable:
                    return slots_[0].wtBound;
                // [leaves 2b/2c] Both are SELF-CONTAINED — no table to bind, so nothing can leave
                // them silent — and both carry their own oversampler, which is why `fmLeg` routes
                // them to their own offset overload rather than through this core's pair.
                case Mode::PhaseDistortion:
                case Mode::Bitmask:
                    return true;
                case Mode::Pulse:
                    return slots_[0].pulse.live(); // [leaf 2c] the UNBOUND guard, as in slotSample
                case Mode::Macro:
                    return true; // [leaf 2d] pitch, NOT phase — see fmLeg's Macro note
            }
            return false;
        }

        // [T-CMP-03 leaf 2a] The FM leg. Same lockstep idea as the ring (plan gate R1) with one
        // difference that changes its latency: the carrier is GENERATED inside B's shaper rather than
        // passed in, so B's input is a dummy 0 — a zero input upsamples to zeros and never touches the
        // interpolating FIR. The leg therefore carries `generatedLatencySamples()` (fractional) where
        // the ring leg carries the cascade's 2*M, which is why composing the two is its own test.
        //
        // A interpolates the MODULATOR and records each OS phase. B then reads the carrier
        // non-advancingly at that OS instant — `p * inc / factor` is where the carrier's phase would
        // be at OS phase p — plus the modulation. The carrier is advanced ONCE per base sample after
        // the pass, so its own clock is untouched.
        // [T-CAL-23 G1] `depth` is `fmDepth_` on the shipped path, the held last-live depth in a fade-out.
        float fmLeg (float modBase, float depth) noexcept
        {
            syncSeedPending_ = true; // [D-M-0208] FM owns OSC 1 and sync is inert; the copy re-seeds after
            int p = 0;
            const auto recordM = [this, &p] (float os) noexcept {
                mPhase_[static_cast<std::size_t> (p)] = os;
                ++p;
                return os;
            };
            Slot& s = slots_[0];
            if constexpr (fork::kExact) // FIREFORK E2: the decimation was discarded, and only the wavetable carrier reads mPhase_
            {
                if (s.mode == Mode::PhaseDistortion || s.mode == Mode::Bitmask || s.mode == Mode::Macro || s.mode == Mode::Pulse)
                    fmA_.pushInput (modBase);
                else
                    fmA_.upsampleOnly (modBase, recordM);
            }
            else
                (void) fmA_.processSample (modBase, recordM);

            // [T-CMP-03 leaf 2b] A PD carrier does NOT go through this core's pair. It owns its own
            // oversampler at its own factor and it must — the CZ warp is the nonlinearity that
            // oversampling exists to band-limit there, so wrapping it in a second cascade would
            // band-limit an already-band-limited signal and pay twice for it. The modulator is handed
            // over at base rate and held across PD's OS block; see that overload's note for why a ZOH
            // is the modulator rather than an approximation of it. A's pass above still ran, which is
            // what keeps the two legs' delay lines advancing in step when the ring is also live.
            const Real modTurns = static_cast<Real> (depth) * static_cast<Real> (modBase);

            if (s.mode == Mode::PhaseDistortion)
                return s.level * s.pd.processSample (modTurns);

            // [T-CMP-03 leaf 2c] Bitmask takes the same route and for the same reason — it owns the
            // oversampler that band-limits its own hard segment edges. It wants ABSOLUTE turns and
            // wraps them itself, so `modTurns` goes over unnormalised.
            if (s.mode == Mode::Bitmask)
                return s.level * s.bm.processSample (modTurns);

            // [T-CMP-03 leaf 2d] MACRO IS THE ONE CARRIER THAT IS NOT PHASE-MODULATED, and the code
            // says so rather than hiding it behind a shared name. There is no phase to offset — the
            // wrapper exposes none and most plaits engines have no phasor — so the slot drives PITCH
            // at the engine's own block rate. Owner-ruled 2026-08-20 with the limitation named: the
            // control is quantised to 12 samples (~4 kHz at 48 k), so a modulator above roughly
            // 2 kHz aliases in the control path. It is a usable sound and it is NOT FM; the FR entry
            // and the DEFERRED row state the mechanism, never the label.
            //
            // `kMacroFmSemitones` is an owner-tunable depth, the same shape as T-CORE-14's
            // `kPitchModSemitones`, not a spec constant.
            if (s.mode == Mode::Macro)
            {
                s.macro.setNoteOffsetSemitones (static_cast<float> (kMacroFmSemitones * modTurns));
                return s.level * s.macro.processSample();
            }

            // [T-CMP-03 leaf 2c] Pulse rides the WT mip pyramid through two reads of one saw, so it
            // inherits `sampleAt`'s [0,1) precondition and the caller — here — owes the wrap.
            if (s.mode == Mode::Pulse)
                return s.level * s.pulse.processSample (wrapOffsetTurns (modTurns));

            const int factor = fmB_.factor();
            const Wide incOs = fork::kApprox ? fork::divByFactor (sampleRate_ > 0.0 ? s.freqHz * invSampleRate_ : 0.0, factor) // FIREFORK N3
                                   : (sampleRate_ > 0.0 ? s.freqHz / sampleRate_ : 0.0) / static_cast<Real> (factor);
            const Real d = static_cast<Real> (depth);

            int q = 0;
            const auto readCarrier = [this, &s, &q, incOs, d]() noexcept {
                // `sampleAt`'s precondition is [0,1) and its note REFUSES to clamp on purpose, so the
                // caller owes the wrap. Two conditional subtracts are enough and no more: the ramp
                // term is under one cycle and the modulation term is bounded by d <= 1, so the sum
                // lies in (-1, 2).
                const Real off = static_cast<Real> (q) * incOs
                                   + d * static_cast<Real> (mPhase_[static_cast<std::size_t> (q)]);
                ++q;
                return s.wt.sampleAt (wrapOffsetTurns (off));
            };
            // FIREFORK E2: B's input is a constant 0 its shaper ignores; decimateSample is documented bit-identical without it
            const float leg = fork::kExact ? fmB_.decimateSample (readCarrier)
                                  : fmB_.processSample (0.0f, [&readCarrier] (float) noexcept { return readCarrier(); });

            // Advance the carrier's own clock exactly once for this base sample. The returned sample
            // is discarded deliberately — the leg above already read every instant that matters, and
            // re-using this one would read the UNMODULATED phase.
            (void) s.wt.processSample();
            return s.level * leg;
        }

        Wide sampleRate_ = 48000.0;
        Wide invSampleRate_ = 1.0 / 48000.0; // FIREFORK N3: 1 / sampleRate_
        Wide noteHz_ = 0.0;
        static inline bool lazyMacro_ = false; // FIREFORK
        float osc2SumGain_ = 1.0f; // [T-CGUI-80/83] see setOsc2ModOnly / setOsc2SumGain
        Slot slots_[kNumSlots];
        SubOscillator sub_; // [T-CMP-02] the fixed −1-octave sub, summed after the OSC 1/2 pair
        std::array<Stack, kNumMixSlots> stacks_ {}; // [T-CGUI-63] OSC 1 · OSC 2's unison stacks (SUB has none)
        // [stereo voice S4] The stacks' width, and this sample's side per slot (level, fade and env applied).
        // The side decimators (PD's, Bitmask's two) are twins of the slots' own, on the HEAP: one pointer
        // per voice, not six oversamplers (the eager-instance budget, EngineCore).
        struct SideDecimators
        {
            std::array<Oversampler, kNumMixSlots> pd;
            std::array<Oversampler, kNumMixSlots> bmBase;
            std::array<Oversampler, kNumMixSlots> bmHigh;
            Oversampler ringA; // OSC 1's side through the ring: twins of `xmodA_` / `xmodB_`
            Oversampler ringB;
        };
        std::unique_ptr<SideDecimators> sideOs_;
        std::array<float, kNumMixSlots> width_ { 0.0f, 0.0f }; // APPLIED, gliding to `widthTarget_`
        std::array<float, kNumMixSlots> widthTarget_ { 0.0f, 0.0f };
        std::array<float, kNumMixSlots> slotSide_ { 0.0f, 0.0f };
        std::array<bool, kNumMixSlots> sidePrev_ { false, false }; // a slot computed a side last sample
        std::array<bool, kNumMixSlots> sideNow_ { false, false };
        bool sideLive_ = false;
        bool sideRingPrev_ = false; // OSC 1's side rang last sample (`sideForRing`)
        bool sideRingNow_ = false;
        // [T-CMP-37 L3] A PD slot's staged OS-rate steps (see `stagePd`) and its lane result; OSC 1 and OSC 2 only.
        struct PdStage
        {
            std::array<float, PhaseDistortionOscillator::kMaxOsSteps> steps {};
            float out = 0.0f;
            bool staged = false; // BEGIN generated the steps; the single arm has not read them yet
            bool done = false; // a lane decimated them: `out` is the sample
        };
        std::array<PdStage, kNumMixSlots> pdStage_ {};
        std::array<lanes::OsLaneSlot*, kNumMixSlots> pdSeat_ {};
        std::uint64_t pdOrphans_ = 0;
        std::array<StackDip, kNumMixSlots> stackDips_ {}; // [row 323] see `followStackCollapse`
        float stackFadeStep_ = 1.0f / 240.0f; // per sample, set in prepare from kStackFadeSeconds
        float stackXfadeStep_ = 1.0f / 480.0f; // [D-M-0539..0542] the count morph, per sample, from kStackXfadeSeconds
        bool stackFresh_ = true;
        std::array<AdsrEnvelope, kNumSlots> oscEnv_ {}; // [T-CGUI-58 leaf 2] OSC 1 · OSC 2; [leaf 4] SUB's is [kSubSlot]
        std::array<bool, kNumSlots> oscEnvOn_ { false, false, false };
        SubMode subMode_ = SubMode::Sub; // [T-CGUI-57] Sub = today's sub
        int subOctave_ = kSubDefaultOctave;

        // [T-CMP-03 leaf 1] The cross-mod lockstep pair and its OS-phase scratch.
        Oversampler xmodA_;
        Oversampler xmodB_;
        std::array<float, kMaxXmodOversample> aPhase_ {};
        float syncAmount_ = 0.0f; // [D-M-0208] the free/synced crossfade; 0 = bypass
        bool syncNaive_ = false; // the keystone's falsifying control — see setSyncBandlimitBypassed
        bool syncRampNaive_ = false; // [D-M-0207] the BLAMP's — see setSyncSlopeBandlimitBypassed
        // [D-M-0208] The sync pair, one per core (= per voice): OSC 2's cycle and OSC 1's synced copy.
        Wide syncMasterPhase_ = 0.0;
        Real syncMasterRead_ = 0.0; // the master's phase at this sample's read (before the advance)
        Wide syncMasterInc_ = 0.0;
        Real syncCrossU_ = -1.0; // this sample's master wrap, `u` in (0,1] after the read; < 0 = none
        Wide syncSlavePhase_ = 0.0;
        Wide syncSlaveInc_ = 0.0;
        bool syncSeedPending_ = true; // the copy re-seeds (`syncSeedPhase`) before its next read
        MinBlep syncBlep_; // the base-rate restart's band-limited step (M1-19)
        float ringDepth_ = 0.0f;
        bool ringLive_ = false;

        // [T-CMP-03 leaf 2a] The FM leg's own pair and OS-phase scratch. Separate instances rather
        // than a shared pair: when both laws are live the FM leg feeds the ring leg, so one set of
        // delay lines cannot serve both.
        Oversampler fmA_;
        Oversampler fmB_;
        std::array<float, kMaxXmodOversample> mPhase_ {};
        float fmDepth_ = 0.0f;
        bool fmLive_ = false;

        // [T-CAL-23 G1] The A/B switches' state (see `setXmodEdgeFadeSeconds` / `setStackLevelMatch`).
        Real xmodFadeSeconds_ = kFmEdgeFadeSeconds;
        float xmodFadeStep_ = 0.0f; // per sample; 0 = the instant switch (the explicit OFF path)
        int fmPrime_ = 0; // samples the FM / ring leg runs silent from a clean history before its fade
        int ringPrime_ = 0;
        bool xmodFresh_ = true; // after reset/prepare: a live leg lands at once, as `stackFresh_`
        XmodFade fmFade_ {};
        XmodFade ringFade_ {};
        std::array<float, kNumMixSlots> stackMatch_ { 1.0f, 1.0f };
        float stackMatchCoef_ = 0.0f; // the glide's one-pole coefficient, set in prepare
        bool stackMatchOn_ = true;
        bool stackMatchSawOnly_ = true; // [D-M-0506]

        // Control-rate (and prepare): the fade's per-sample step and the two priming lengths, one FIR span.
        void resolveXmodFade() noexcept
        {
            const Real len = xmodFadeSeconds_ * sampleRate_;
            xmodFadeStep_ = xmodFadeSeconds_ > 0.0 ? static_cast<float> (1.0 / (len >= 1.0 ? len : 1.0)) : 0.0f;
            fmPrime_ = 2 * fmB_.latencySamples() + 1;
            ringPrime_ = 2 * xmodB_.latencySamples() + 1;
        }
    };
}
