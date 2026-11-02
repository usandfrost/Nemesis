#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ForkConfig.h>
#include <nemesis/dsp/FastMath.h>
#include <nemesis/dsp/FilterResonance.h>

#include <nemesis/dsp/AdsrEnvelope.h>
#include <nemesis/dsp/BypassFade.h>
#include <nemesis/dsp/CathodeModMatrix.h>
#include <nemesis/dsp/CathodeOutputStage.h>
#include <nemesis/dsp/DeltaDecayEnvelope.h>
#include <nemesis/dsp/Lfo.h>
#include <nemesis/dsp/PulseOscillator.h>
#include <nemesis/dsp/PunchTransient.h>
#include <nemesis/dsp/StepModulator.h>
#include <nemesis/dsp/StepSlew.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace nemesis::dsp
{
    // CathodeModulatedVoice — the CATHODE §4 mod-matrix WIRE-IN (CATHODE.md §4 "sources (envs, LFOs,
    // S&H, step-mod, velocity, key, mod-wheel, the synth's own random/drift) → destinations … → mod
    // matrix → any destination"). The per-voice layer that OWNS + ticks the modulation sources, feeds
    // their per-sample values into the signed-off CathodeModMatrix evaluator (M1-17c), evaluate()s, and
    // APPLIES the summed per-destination offset to the voice's parameters — the piece the matrix + its
    // sources (Lfo M1-17a, StepModulator M1-17b, AdsrEnvelope M1-10a) were built to plug into.
    //
    // ARCHITECTURE (owner decision, 2026-07-15): a NEW per-voice wrapper composing a signed-off
    // CathodeOutputStage (M1-10b — the AMP env + tone path + drive + pan) with the extra §4 sources and
    // the matrix. It sits BETWEEN CathodeOutputStage and CathodeVoiceEngine; every signed-off class (and
    // its committed golden) is left UNTOUCHED. Swapping this into CathodeVoiceEngine is a follow-on card;
    // for now it is a standalone, independently-testable composition (as CathodeOutputStage was before
    // the engine consumed it).
    //
    // SOURCES OWNED (this card, M1-17d): Env2/Env3 (free DADSR mod envelopes, gated by the note alongside
    // the AMP env), Lfo1..Lfo3, StepMod, SampleHold (an Lfo locked to its SampleHold shape). Env1 is the
    // AMP envelope living inside CathodeOutputStage, read back through amp().value() (no extra state) — and
    // because AdsrEnvelope reads-then-advances, that value is exactly the AMP gain the stage applies THIS
    // sample (no lag). Velocity + KeyTrack are runtime scalars the caller latches; [T-MOD-01 leaf B] so are
    // the three DRIFTER ring values, but pushed once per SAMPLE from the engine-global `dsp::DrifterBank`
    // rather than per note (setDrifterValues — this class's first BROADCAST source). ConvDriftReadout
    // (ConverterDrift's instantaneous wander; named `Drift` before that leaf's rename) is LIVE since
    // T-CGUI-24 — scaled by the patch's drift amount — and so is ModWheel (`setModWheel`, T-CMP-21 leaf C). [T-CORE-14 leaf B] Env2/Env3 additionally carry a GLOBAL
    // SOURCE GAIN (`synth.env{2,3}.amt`, Param Ref:180 — setEnv2Amount): one bipolar multiply on the
    // value fed to the matrix, so it scales every route from that envelope and can invert it. Unity
    // by default, so a consumer that ignores it is unaffected.
    //
    // DESTINATIONS WIRED: the three original (M1-17d) — FiltCutoff (exponential octave remap onto the
    // default TptSvf cutoff, the tone path), Amp (linear, onto the output level), Pan (linear bipolar, onto
    // the equal-power pan) — PLUS the §2 broken-converter "alive grit" batch (M1-17f, CATHODE §4 "where a
    // lot of the alive grit lives"): ConvRate (exponential octave remap onto the native converter rate),
    // ConvBits (scaled-linear remap onto the storage word length), ConvLoopErr + ConvDrift (raw-additive,
    // Amp-style, onto the loop-seam / wow-flutter amounts) — PLUS the M1-17g batch: Pitch (exp2 semitone
    // remap onto the note Hz, fanned to osc + key-track + loop-err period), OscPos + PdAmount (raw-additive
    // onto both osc slots' WT morph / PD warp), FiltReso (multiplicative-octave exp2 remap onto the TptSvf Q,
    // combined with FiltCutoff into one setCutoff), FiltVowel (raw-additive onto the formant position) —
    // PLUS NoiseLevel (M1-18c, raw-additive onto the §5 texture layer's noise level, the M1-17g deferred
    // no-target dest unblocked by the M1-18b texture fold-in). The one remaining §4 destination Sync
    // (§1 hard-sync) has NO DSP target in the current voice path → it stays a deferred no-target dest and
    // lights up when its DSP lands.
    // [T-CORE-30 leaf D] ALSO OWNED, and it is not a modulation source or destination at all: the §2
    // shared PUNCH onset transient (`PunchTransient`, punch_transient.md). It lives here because this
    // is the per-VOICE layer — the burst fires with the voice's note-on, tracks its fundamental, and
    // sums into one of the voice's own two §2 nodes (`punch.mix`) — and because the primitive was
    // built complete and never instantiated by anything. Off at `punch.amount = 0`, which is a
    // bit-exact no-op rather than a quiet one (see processSample step 5).
    // reson.amount/tune + noise.type as destinations are likewise deferred. Each destination is applied ONLY
    // while it is LIVE (≥1 slot targets it): an un-live destination is never written per sample, so an
    // EMPTY matrix leaves the composed CathodeOutputStage bit-identical to a bare one (the no-op keystone).
    // When a destination goes live→un-live (its last slot cleared) its base is re-applied so the patch
    // value is restored. The ConvRate base is primed to the host rate in prepare() so an un-live ConvRate
    // sits at the reducer's bit-exact inc==1 bypass (the M1-09xe SRR gotcha).
    //
    // APPLY MATH is analytic (like the M1-17c matrix → NO golden): effective = clamp(base + remap(Σ v·d)).
    // Amp/Pan add the raw bipolar offset directly; FiltCutoff maps it to ±kCutoffModOctaves octaves
    // (base·2^(mod·octaves)). kCutoffModOctaves is an owner-TUNABLE default, not a spec constant. The
    // effective (post-mod) value each destination applies is exposed (effectiveCutoffHz()/effectiveLevel()/
    // effectivePan()) so a test pins base + remap(mod) to the bit (the FilterDrive::preGainFor /
    // CathodeOutputStage::velGain accessor idiom); cutoffForMod() is exposed static for identical operands.
    //
    // RT-safety (§11): prepare() prepares the output stage + every owned source (off the audio thread).
    // Setters resolve control-rate. processSample() ticks the sources (their inline hot paths), feeds +
    // evaluate()s the matrix (pure arithmetic), applies the live destinations and returns the output
    // stage's stereo sample. The libm on the hot path is std::exp2, and only while a destination that needs
    // it is live: the FiltCutoff/ConvRate/Pitch/FiltReso octave/semitone remaps, plus std::log2 per sample
    // on a ROUTED F2 cutoff (setF2Cutoff's step slew, + exp2 while it ramps; D-M-0490, measured at 14.4 ns
    // per voice-sample, perf-budgets.md §3h), plus the exp2/tan/cos·sin
    // the forwarded setters themselves do (BrokenConverter::setBitDepth for ConvBits, TptSvf::setCutoff for
    // FiltCutoff/FiltReso, SRR key-track for Pitch, FormantFilter::setVowel for FiltVowel) — a documented
    // scoped deviation (bounded, deterministic, no control-rate escape for smooth per-sample modulation;
    // precedented by the mono glide + μ-law grit). No allocation on any path.
    // Deterministic (§11): a given (patch + slots + note/gate) renders bit-identically; reset() reseeds
    // the LFO/S&H random streams (per-voice variation is wireable via each source's setSeed). No JUCE.
    // Not thread-safe against prepare().
    class CathodeModulatedVoice
    {
    public:
        using StereoSample = CathodeOutputStage::StereoSample;
        using Source = CathodeModMatrix::Source;
        using Dest = CathodeModMatrix::Dest;

        // [T-CORE-30 leaf D] Where the §2 punch burst joins the voice — `punch.mix`. The PRIMITIVE
        // deliberately does not own this ("where it sums is the voice's concern, not the
        // primitive's", PunchTransient.h), and the voice is what has both nodes:
        //   · PreFilter  — into the tone path's pre-filter node, so the burst passes through the
        //                  §3 filter and the §3 drive and inherits the voice's colour (subtler).
        //   · PostFilter — into the mix point downstream of the filter and the AMP envelope: the
        //                  cleanest, most consistent "DX7 click" (§2), and CATHODE's §5 default.
        // Index order is the spec's `{pre_filter, post_filter}` and it is the stored enum index.
        enum class PunchMix : std::uint8_t { PreFilter = 0,
            PostFilter = 1 };

        // [T-CORE-16] How many oscillator slots the two per-slot osc bases cover. DERIVED from the
        // tone path rather than restated, so the two cannot drift: the bases exist to feed
        // `CathodeVoiceStage::setPosition (slot, …)` / `setPdAmount (slot, …)`, and a base array
        // shorter or longer than that is a silent truncation.
        // [T-CGUI-57] The OSC 1/2 MIX pair (`kNumMixSlots`), NOT every slot: the SUB slot takes none of
        // the shared OscPos/PdAmount dests or the mask/PW hard-wires — its rows are generic dests.
        static constexpr std::size_t kNumOscSlots = static_cast<std::size_t> (CathodeOscCore::kNumMixSlots);

        // [T-CORE-14 leaf C] How many of the owned LFOs proper (Lfo1..Lfo3) — the three `lfoFor`
        // indexes. S&H and Step have their own sync rows since T-CGUI-19; see kTriggerableCount.
        static constexpr int kSyncableLfoCount = 3;
        // [T-CGUI-19] …and the sources a `Trig` sync can restart at note-on: the three LFOs, then the
        // S&H source (3) and the Step modulator (4), which gained `.sync` rows of their own.
        static constexpr int kTriggerableCount = 5;
        static constexpr int kSampleHoldTrigIndex = 3;
        static constexpr int kStepTrigIndex = 4;

        // Full-depth (mod = ±1) cutoff modulation spans ±this many octaves about the base cutoff. An
        // owner-tunable default (§4 has no spec constant); the analytic keystone pins the math, not the
        // musical value.
        static constexpr Real kCutoffModOctaves = 4.0;

        // [T-CGUI-83] THE CUTOFF STEP LIMITER (`StepSlew` on the FiltCutoff modulation, F1 and the F2
        // that follows it). A route moving the cutoff more than a quarter octave in ONE sample — a drawn
        // vertical step, a Square/S&H/Step edge — ramps there over 3 ms instead, because `TptSvf` keeps
        // its integrators across a coefficient jump and clicks (owner, 2026-10-01). Measured: the fastest
        // smooth movement in the dsp/app suites and the factory bank is 0.034 oct/sample (a filter-env
        // attack at 44.1 kHz; a full-depth 30 Hz sine is 0.017), so every smooth route passes BIT-EXACT.
        // The first millisecond of a note is exempt (see StepSlew): an instant filter envelope keeps its
        // snap. Pitch is NOT limited — a pitch step is phase-continuous and does not click.
        static constexpr Real kCutoffSlewStepOctaves = 0.25; // per sample
        static constexpr Real kCutoffSlewSeconds = 0.003;
        static constexpr Real kCutoffSlewOnsetSeconds = 0.001;
        // The threshold in the unit the limiter sees, the matrix MOD value (octaves / kCutoffModOctaves).
        [[nodiscard]] static constexpr float cutoffSlewThreshold() noexcept
        {
            return static_cast<float> (kCutoffSlewStepOctaves / kCutoffModOctaves);
        }

        // The modulated cutoff in Hz for a base cutoff + a bipolar matrix offset `mod`: an exponential
        // (octave) remap base·2^(mod·kCutoffModOctaves). Exposed static so a reference/test computes the
        // IDENTICAL operand (bit-identity); TptSvf clamps the result below Nyquist internally.
        [[nodiscard]] static float cutoffForMod (float baseCutoffHz, float mod) noexcept
        {
            return static_cast<float> (static_cast<Real> (baseCutoffHz)
                                       * fm::exp2 (static_cast<Real> (mod) * kCutoffModOctaves));
        }

        // ---- Converter "alive grit" batch (M1-17f, CATHODE §4) mod-depth defaults + remaps ----
        // The four §2 broken-converter destinations. ConvRate and ConvBits are WIDE-range params (Hz and
        // [1,24] bits) where a raw ±1 offset is musically negligible — like FiltCutoff they take a SCALED
        // remap; ConvLoopErr/ConvDrift are [0,1] params taking the raw-additive+clamp Amp remap inline.
        // Both depth constants are owner-tunable defaults, NOT spec constants (as with kCutoffModOctaves).

        // Full-depth (mod = ±1) converter-rate modulation spans ±this many octaves about the base rate —
        // owner decision (2026-07-15): an exponential octave curve mirroring FiltCutoff (rate is a
        // varispeed frequency, §2). A downward mod lowers the native rate → heavier aliasing/darkening.
        static constexpr Real kRateModOctaves = 4.0;
        // Full-depth (mod = ±1) bit-depth modulation spans ±this many bits about the base word length.
        static constexpr Real kBitsModRange = 8.0;

        // The modulated converter rate in Hz for a base rate + bipolar offset `mod`: base·2^(mod·
        // kRateModOctaves). Exposed static for bit-identical operands; the SampleRateReducer treats a rate
        // ≥ the host rate as a bit-exact bypass internally.
        [[nodiscard]] static float rateForMod (float baseRateHz, float mod) noexcept
        {
            return static_cast<float> (static_cast<Real> (baseRateHz)
                                       * fm::exp2 (static_cast<Real> (mod) * kRateModOctaves));
        }
        // The modulated word length in bits for a base + bipolar offset `mod`: base + mod·kBitsModRange (a
        // scaled LINEAR remap — bits is a linear quantity, unlike the octave-mapped rate/cutoff). Exposed
        // static for bit-identical operands; BrokenConverter clamps to [1, 24] internally (so, like
        // cutoffForMod, the returned value may sit outside the applied range).
        [[nodiscard]] static float bitsForMod (float baseBits, float mod) noexcept
        {
            return static_cast<float> (static_cast<Real> (baseBits)
                                       + static_cast<Real> (mod) * kBitsModRange);
        }

        // ---- Remaining wireable destinations (M1-17g) mod-depth defaults + remaps ----
        // Pitch and FiltReso are WIDE-range params (Hz freq / always-positive unbounded Q) taking SCALED
        // exp2 remaps like FiltCutoff; OscPos/PdAmount/FiltVowel are [0,1] params taking the raw-additive +
        // clamp01 Amp remap inline. Both depth constants are owner-tunable defaults, NOT spec constants.

        // ---- The Pitch depth LAW (owner-tunable feel constants, NOT spec values) ----
        //
        // [T-CORE-30 leaf B] Pitch modulation is NON-LINEAR in depth: small depths stay musical,
        // full depth reaches an octave. OWNER-RULED 2026-08-02 (T-CAT-01 Phase 3), replacing the
        // single linear constant below.
        //
        // THE HISTORY, because it is what fixes the two coefficients. [T-CORE-14 leaf A2] the
        // linear constant was 12.0 (±1 octave) — an untested placeholder that nothing had ever
        // heard, because no slot was routed until that card built the edge. Applied as written,
        // the §2.6 Init patch's seeded `L1→Pit +40` (0.635 of full depth) against a full-depth
        // ~1 Hz Lfo1 gave the DEFAULT patch a constant ±7.62 st siren, so it was retuned to 2.0 —
        // which fixed the Init and capped the whole matrix at ±2 st, ruling out the wide gestures
        // (S&H→Pitch stepped melodies, envelope drops) a mod matrix exists for. The two goals were
        // arithmetically incompatible for any single constant. A curve resolves them because it
        // does not have to answer the same way at both ends.
        //
        //     st(m) = kPitchModSlope·m + (kPitchModFull − kPitchModSlope)·m³,  m clamped to [−1, 1]
        //
        // ODD by construction, so it is sign-correct without a branch. Its slope at the origin is
        // EXACTLY kPitchModSlope = 2, i.e. a small dialed depth behaves as T-CORE-14 ruled — the
        // "musical" half is inherited from the shipped value rather than re-invented — while
        // m = ±1 gives ±12 st, an octave.
        //
        // NOTE that the Init patch's own siren cannot recur at ANY coefficients: T-CORE-14 ruled
        // every §2.6 Init DEPTH to 0, so the shipped patch modulates nothing. The constraint the
        // curve actually serves is the DIALED one — a user at a quarter depth wants vibrato, and
        // the same user at full depth wants an octave.
        //
        // THE CLAMP IS PART OF THE LAW. `CathodeModMatrix::evaluate` SUMS source×depth across all
        // 16 slots, so `mod` is not bounded by ±1: two slots at full depth reach ±2. An unbounded
        // cubic is then absurd (m = 4 → 648 st), and "full depth reaches an octave" only means
        // something once the domain is bounded. So Pitch SATURATES, exactly as the §1 [0,1]
        // destinations already do (OscPos/PdAmount via clamp01).
        // [T-CMP-21 leaf C] The NATIVE pitch-bend span, in semitones each way at full wheel.
        // OI-MIDI-6, owner-decided 2026-08-16: "pitch-bend NATIVE to voice pitch (±2 st)".
        // [T-CGUI-96] R69 made the span a per-track row (`synth.bendRange`, whole semitones 0..24), so ±2 is the
        // DEFAULT now and `bendSemitones_` the live value, set by `setBendRange`.
        static constexpr Real kDefaultBendSemitones = 2.0;
        static constexpr Real kMaxBendSemitones = 24.0;
        static constexpr Real kPitchModSlope = 2.0; // st per unit depth AT the origin
        static constexpr Real kPitchModFull = 12.0; // st at full depth (|m| ≥ 1) — an octave

        // The law itself, exposed so it can be pinned analytically at named anchors rather than
        // inferred from a rendered frequency (the M1-17c closed-form precedent).
        [[nodiscard]] static Real pitchModSemitones (Real mod) noexcept
        {
            const Real m = mod < -1.0 ? -1.0 : (mod > 1.0 ? 1.0 : mod);
            return kPitchModSlope * m + (kPitchModFull - kPitchModSlope) * m * m * m;
        }
        // Full-depth (mod = ±1) resonance modulation spans ±this many octaves about the base Q — owner
        // decision (2026-07-15): a multiplicative exp2 curve (Q is always positive / unbounded above).
        static constexpr Real kResoModOctaves = 2.0; // ×4 Q at full depth

        // The modulated note frequency in Hz for a base + bipolar offset `mod`:
        // base·2^(st(mod)/12), an equal-tempered (semitone) remap over the depth law above.
        // Exposed static for bit-identical operands (the cutoffForMod idiom). It CONSUMES
        // pitchModSemitones rather than restating the arithmetic, so there is one statement of
        // the law and the analytic anchors and the rendered pitch cannot disagree.
        [[nodiscard]] static float pitchForMod (float baseNoteHz, float mod) noexcept
        {
            return static_cast<float> (static_cast<Real> (baseNoteHz)
                                       * fm::exp2 (pitchModSemitones (static_cast<Real> (mod)) / 12.0));
        }
        // The modulated resonance Q for a base + bipolar offset `mod`: base·2^(mod·kResoModOctaves), a
        // multiplicative (octave) remap that stays strictly positive. Exposed static for bit-identical
        // operands; TptSvf clamps Q to a small positive minimum internally.
        [[nodiscard]] static float resoForMod (float baseQ, float mod) noexcept
        {
            return static_cast<float> (static_cast<Real> (baseQ)
                                       * fm::exp2 (static_cast<Real> (mod) * kResoModOctaves));
        }

        // Prepares the output stage and every owned source at this sample rate; locks the S&H source to
        // its SampleHold shape; seeds the effective values from the bases. Not RT-safe (allocates).
        void prepare (Wide sampleRate)
        {
            outputStage_.prepare (sampleRate);
            // FIREFORK E19: the limiters run once per control tick, so their ramp length (in samples) and step threshold scale with the rate (1 = unchanged)
            cutoffSlew_.prepare (sampleRate / kCtlRate, kCutoffSlewSeconds, kCutoffSlewOnsetSeconds); // [T-CGUI-83]
            cutoffSlew_.setThreshold (cutoffSlewThreshold() * kCtlRate);
            f2CutoffSlew_.prepare (sampleRate / kCtlRate, kCutoffSlewSeconds, kCutoffSlewOnsetSeconds); // [D-M-0158]
            f2CutoffSlew_.setThreshold (static_cast<float> (kCutoffSlewStepOctaves) * kCtlRate);
            env2_.prepare (sampleRate);
            env3_.prepare (sampleRate);
            for (auto& d : envDraw_) // [T-CGUI-61 leaf E6]
            {
                d.env.prepare (sampleRate);
                d.drop();
            }
            envXfLen_ = std::max (1, static_cast<int> (std::lround (sampleRate * DrawEnvelope::kDeclickSeconds))); // [row 304]
            envXfLeft_.fill (0);
            envLast_.fill (0.0f);
            lfo1_.prepare (sampleRate);
            lfo2_.prepare (sampleRate);
            lfo3_.prepare (sampleRate);
            step_.prepare (sampleRate);
            sh_.prepare (sampleRate);
            sh_.setShape (Lfo::Shape::SampleHold); // the dedicated §4 Sample & Hold source
            for (auto& dd : dd_)
                dd.prepare (sampleRate); // [T-MASK-03 leaf F1] re-resolves the decay coefficient
            punch_.prepare (sampleRate); // [T-CORE-30 leaf D] clears state + caches the sine table
            for (auto* f : { &envFade_, &lfoFade_, &driftFade_, &mtxFade_ })
                f->prepare (sampleRate); // [T-CGUI-08] the modulation-stage bypass fades, 10 ms
            // Prime the base converter rate to the HOST rate so an un-live ConvRate leaves the reducer at
            // its bit-exact inc==1 bypass (the M1-09xe gotcha: the SRR bypasses at device==host, NOT at a
            // fixed-Hz constructor default). Matches what CathodeVoiceStage::prepare set on the reducer.
            baseConverterRateHz_ = static_cast<float> (sampleRate);
            effectiveCutoffHz_ = baseCutoffHz_;
            effectiveLevel_ = baseLevel_;
            effectivePan_ = basePan_;
            effectiveConverterRateHz_ = baseConverterRateHz_;
            effectiveBitDepth_ = baseBitDepth_;
            effectiveLoopError_ = baseLoopError_;
            effectiveDrift_ = baseDrift_;
            effectiveNoteHz_ = static_cast<float> (baseNoteHz_);
            effectiveQ_ = baseQ_;
            effectiveOscPos_ = baseOscPos_;
            effectivePdAmount_ = basePdAmount_;
            // [T-MASK-03 leaf F1] The mask has no effective COPY to re-seed (see `baseMask_`), so the
            // base is PUSHED instead — otherwise a reset taken mid-transient would leave the
            // oscillator on whatever mask the envelope had reached, which the read-back would then
            // report truthfully and nobody asked for.
            for (std::size_t s = 0; s < kNumOscSlots; ++s)
                outputStage_.voice().setMask (static_cast<int> (s), baseMask_[s]);
            effectiveVowel_ = baseVowel_;
            effectiveNoiseLevel_ = baseNoiseLevel_;
            // [T-CMP-16 S5R-2] THE CONTROL-RATE PHASE IS PART OF A RESET, and the first version of
            // this card missed it: the phase was cleared only in `refreshLive`, so a STOLEN voice
            // (`reset()` then `gateOn()`) resumed mid-period and rendered up to 63 samples — 1.3 ms
            // at 48 kHz — of the PREVIOUS note's modulated value before the next control tick. Note
            // onset is the most audible moment a resonator's material has, and an audio-rate row has
            // no equivalent hole because it writes every sample. The `GenericRate` comment claimed
            // "the first sample after a reset carries the modulated value"; this is what makes that
            // sentence true rather than aspirational.
            genericControlPhase_ = 0;
            cutValid_ = false; ctlPhase_ = 0; // FIREFORK E17/E19 (the next sample is a control tick)
        }

        // Clears the output stage, every owned source (LFO/S&H streams RESEEDED, envelopes → Idle, step
        // → 0) and the matrix source/dest values; re-seeds the effective values from the bases. RT-safe.
        void reset() noexcept
        {
            outputStage_.reset();
            env2_.reset();
            env3_.reset();
            for (auto& d : envDraw_) // [T-CGUI-61 leaf E6]
            {
                d.env.reset();
                d.drop();
            }
            envXfLeft_.fill (0); // [row 304] nothing sounding to cross from
            envLast_.fill (0.0f);
            lfo1_.reset();
            lfo2_.reset();
            lfo3_.reset();
            step_.reset();
            sh_.reset();
            punch_.reset(); // [T-CORE-30 leaf D] disarm any burst + reseed its noise
            for (auto& dd : dd_)
                dd.reset(); // [T-MASK-03 leaf F1] a pending mask transient does not survive a reset
            matrix_.reset();
            // [T-CGUI-08] A bypass fade's GAIN is audio state and lands on its target; the TARGET
            // is a control parameter and survives (the `CathodeVoiceStage::reset` rule).
            for (auto* f : { &envFade_, &lfoFade_, &driftFade_, &mtxFade_ })
                f->settle();
            aftertouch_ = 0.0f; // [T-CMP-06] ditto for pressure
            // [T-CMP-21 leaf C] The two channel controls go home HERE and only here — a hard reset
            // IS "start again", while a note-on is not (see the members). Order matters: `bendRatio_`
            // is reset with them, and `effectiveNoteHz_` below is then the un-bent base.
            modWheel_ = 0.0f;
            channelPressure_ = 0.0f; // [T-CGUI-95] the engine re-applies its copy, as it does the wheel's
            bend_ = 0.0f;
            bendRatio_ = 1.0;
            organicCutoffMult_ = 1.0f; // [T-CMP-11] a hard reset owes no note a stale Organic offset
            pushValid_ = false; // FIREFORK E17: nothing has been pushed since this reset
            cutValid_ = false;
            cutoffSlew_.reset(); // [T-CGUI-83] no ramp survives a reset
            f2CutoffSlew_.reset(); // [D-M-0158]
            effectiveCutoffHz_ = baseCutoffHz_;
            effectiveLevel_ = baseLevel_;
            effectivePan_ = basePan_;
            effectiveConverterRateHz_ = baseConverterRateHz_;
            effectiveBitDepth_ = baseBitDepth_;
            effectiveLoopError_ = baseLoopError_;
            effectiveDrift_ = baseDrift_;
            effectiveNoteHz_ = static_cast<float> (baseNoteHz_);
            effectiveQ_ = baseQ_;
            effectiveOscPos_ = baseOscPos_;
            effectivePdAmount_ = basePdAmount_;
            // [T-MASK-03 leaf F1] The mask has no effective COPY to re-seed (see `baseMask_`), so the
            // base is PUSHED instead — otherwise a reset taken mid-transient would leave the
            // oscillator on whatever mask the envelope had reached, which the read-back would then
            // report truthfully and nobody asked for.
            for (std::size_t s = 0; s < kNumOscSlots; ++s)
                outputStage_.voice().setMask (static_cast<int> (s), baseMask_[s]);
            effectiveVowel_ = baseVowel_;
            effectiveNoiseLevel_ = baseNoiseLevel_;
            // [T-CMP-16 S5R-2] THE CONTROL-RATE PHASE IS PART OF A RESET, and the first version of
            // this card missed it: the phase was cleared only in `refreshLive`, so a STOLEN voice
            // (`reset()` then `gateOn()`) resumed mid-period and rendered up to 63 samples — 1.3 ms
            // at 48 kHz — of the PREVIOUS note's modulated value before the next control tick. Note
            // onset is the most audible moment a resonator's material has, and an audio-rate row has
            // no equivalent hole because it writes every sample. The `GenericRate` comment claimed
            // "the first sample after a reset carries the modulated value"; this is what makes that
            // sentence true rather than aspirational.
            genericControlPhase_ = 0;
            cutValid_ = false; ctlPhase_ = 0; // FIREFORK E17/E19
        }

        // ---- Note gate (fans out to the AMP env inside the output stage AND the free Env2/Env3) ----
        // [T-CGUI-68 S5R] A voice about to sound from ZERO GAIN (idle, or the steal-fade swap) owes its
        // GRIT dry leg this note's latency at once — there is nothing to glide from.
        void snapGritDryDelay() noexcept { outputStage_.voice().snapGritDryDelay(); }
        // `restrike` = a Mono-RT retrigger [T-CGUI-40, R11]: the AMP env strikes from zero (a 2 ms dip);
        // Env2/Env3 keep `gateOn`'s continue-from-level law — R11 names "the AMP ENVELOPE" only.
        void gateOn (bool restrike = false) noexcept
        {
            // [D-M-0173] A DRAW flip still pending lands BEFORE the gate — on a silent voice at once, so
            // the note starts on the new source alone (the drawn start, or the pure ADSR). A voice outside the engine's render span processes
            // no sample between a flip and its next note-on; at its first sample the flip would find the
            // AMP just gated and crossfade 5 ms out of the ADSRs instead.
            applyDrawEdges (!outputStage_.active());
            // [T-CGUI-68 leg 1] From idle the GRIT dry leg snaps to this note's latency (setNoteHz ran
            // first); a re-strike of a sounding voice keeps the glide, or its dry leg would step.
            // [T-CGUI-58 leaf 2] Read BEFORE the AMP env restarts: from silence, a per-osc release
            // that outlived the voice is cut; a Mono-RT retrigger restrikes them with AMP.
            outputStage_.voice().oscEnvGateOn (restrike, !outputStage_.active());
            if (!outputStage_.active())
            {
                snapGritDryDelay();
                outputStage_.voice().resetFilterState(); // [B34] a note from idle never inherits the last note's filter state
                outputStage_.voice().resetIdleRings(); // [B34, owner 2026-10-13] …nor its noise resonator's ring or HPF memory
            }
            if (restrike)
                outputStage_.restrike();
            else
                outputStage_.gateOn();
            env2_.gateOn();
            env3_.gateOn();
            // [T-CGUI-83] A note starts on its own cutoff, never mid-ramp. The onset window this opens
            // passes a re-gate's LFO Trig/Step-reset jump unramped into a filter that still holds state
            // (Mono-RT restrike, poly same-voice re-gate) — see StepSlew's ONSET note.
            cutoffSlew_.reset();
            f2CutoffSlew_.reset(); // [D-M-0158] F2's own route, likewise
            // [T-CGUI-61 leaf E6] …and their drawn twins, but ONLY while selected (the AMP stage's rule,
            // S5R): an unselected twin is never gated nor ticked, so DRAW Off is bit-exact, and
            // the On edge (`applyDrawEdges`) re-gates a held note from the drawn start.
            for (std::size_t i = 0; i < envDraw_.size(); ++i)
                if (envDrawOn_[i] || envXfLeft_[i] > 0)
                {
                    envDraw_[i].drop();
                    envDraw_[i].env.gateOn();
                }
            aftertouch_ = 0.0f; // [T-CMP-06] a fresh trigger owes no note the last occupant's pressure
            // [T-CMP-17 S5R-3] A NOTE-ON STARTS THE CONTROL PERIOD, so the first sample of a note
            // carries a freshly written value. Without this the phase is FREE-RUNNING — `processPoly`
            // ticks every voice every sample, idle ones included — so a new note began wherever the
            // counter happened to be, and up to 63 samples of it used the last tick's value.
            //
            // It bites the DELAY rows hardest and they are the reason this is not merely tidiness:
            // `AdsrEnvelope::gateOn` LATCHES `delayCounter_ = delaySamples_` and picks the
            // Delay-vs-Attack branch right here, so `synth.{amp,env2,env3}.del` would spend the
            // WHOLE NOTE on a value up to 63 samples stale rather than one sample stale.
            genericControlPhase_ = 0;
            cutValid_ = false; ctlPhase_ = 0; // FIREFORK E17/E19
            // [T-MASK-03 leaf D] A Bitmask slot's pitch-adaptive oversampling factor is ARMED by
            // `setFrequency` and committed HERE — note-on is the one moment the decimator is free to
            // change (leaf C2: a mid-note switch steps the output and jogs the group delay, i.e. a
            // click). `assignVoice` sets noteHz BEFORE gateOn, so the factor committed is the one
            // this note's pitch asks for. Phase is deliberately untouched: the slots are
            // free-running across notes and that stays a separate decision (T-CMP-10).
            //
            // Without this line the commit had no reachable caller in the instrument — the factor
            // was fixed inside prepare(), at frequency 0, so the shipped adaptive default never
            // escalated no matter how high you played. Cheap by construction: two flag writes and
            // two decimator clears per note-on, on slots nobody may even be using.
            outputStage_.voice().commitPendingMaskFactors();
            // [T-CGUI-122] …and STRIKE the percussive ALGO engines (Kick/Snare/Hat/String/Modal), which
            // otherwise drone (plaits' unpatched trigger). A no-op on every other mode and engine.
            outputStage_.voice().strikeMacros();
            // [T-MASK-03 leaf F1] …and FIRE the per-slot mask transients. This is the reference
            // instrument's signature gesture: the mask number snaps on every note and settles, so a
            // note has an attack in TIMBRE and not only in level. Unconditional and cheap (two stores
            // — the envelope decays from here whatever the depth is), because a depth of 0 already
            // makes it inaudible and a gate on the depth would put a branch on the note path to save
            // nothing. Fired AFTER the factor commit for no reason other than reading order; the two
            // are independent.
            for (auto& dd : dd_)
                dd.trigger();
            // [T-CORE-14 leaf C] `synth.lfo{1..3}.sync` = Trig — restart the flagged LFOs' phase at
            // note-on (Param Ref:194). See setLfoTrig; a Free LFO is never touched, which is what
            // keeps this a bit-exact no-op for every consumer that sets no sync mode.
            if (lfoTrig_[0])
                lfo1_.retrigger();
            if (lfoTrig_[1])
                lfo2_.retrigger();
            if (lfoTrig_[2])
                lfo3_.retrigger();
            // [T-CGUI-19, owner 2026-09-24] `synth.sh.sync` / `synth.step.sync` = Trig: the S&H cycle
            // and the step pattern restart at note-on too — S&H on its phase (its random stream runs
            // on, as an LFO's does), Step back to step 0.
            if (lfoTrig_[kSampleHoldTrigIndex])
                sh_.retrigger();
            if (lfoTrig_[kStepTrigIndex])
                step_.reset();
            // [T-CMP-01 leaf 2] `synth.lfo*.delay` — the Juno delayed-vibrato onset. Armed for
            // EVERY LFO, Trig or Free: the delay is a property of the NOTE, not of the sync mode.
            // It is a no-op at delay 0, so the "a Free LFO is never touched" law above still holds
            // for every patch that dials none, and it composes with the retrigger just above —
            // the phase is restarted, then HELD for the onset (see Lfo::processSample).
            lfo1_.armDelay();
            lfo2_.armDelay();
            lfo3_.armDelay();
            // [T-CORE-30 leaf D] The §2 onset transient fires SYNCHRONOUSLY with the voice's
            // note-on ("shares the Link-clock trigger; zero added latency"), which is why it
            // hangs off this gate rather than off a separate call the engine could forget to make.
            // `velocity01_` is already latched — `CathodeVoiceEngine::assignVoice` sets velocity
            // and noteHz BEFORE gateOn, and this class forwards both to the primitive as they
            // arrive, so the burst's gain and carrier are the ones this note asked for.
            //
            // With `punch.amount == 0` the primitive does NOT schedule its generator (the §1/§6
            // zero-cost promise), so an untouched punch leaves gateOn as cheap as it was and
            // processSample bit-exact — that is the property, not a hope: `active()` stays false
            // and the sum below is never reached.
            punch_.trigger (velocity01_);
        }
        void gateOff() noexcept
        {
            outputStage_.gateOff();
            outputStage_.voice().oscEnvGateOff(); // [T-CGUI-58 leaf 2]
            env2_.gateOff();
            env3_.gateOff();
            for (std::size_t i = 0; i < envDraw_.size(); ++i) // [T-CGUI-61 leaf E6] only while selected (or crossfading)
                if (envDrawOn_[i] || envXfLeft_[i] > 0)
                {
                    envDraw_[i].drop();
                    envDraw_[i].env.gateOff();
                }
            aftertouch_ = 0.0f; // [T-CMP-06] release returns the source to rest — no stuck modulation
        }
        // The voice is sounding while the AMP env runs (the free Env2/Env3 do not extend voice life —
        // the AMP env is the voice-reclaim edge, as in CathodeOutputStage).
        [[nodiscard]] bool active() const noexcept { return outputStage_.active(); }
        // [b39 E-tails] See CathodeOutputStage::silenceEnvelopes — the note ends, the phases run on.
        // [b39 review 1] EVERY envelope goes Idle, not only AMP: Env2/Env3 and their drawn twins (as `reset()` drops
        // them) and the per-osc envelopes. `gateOn` continues Env2/Env3 from their level, so a cut voice left at sustain
        // handed the next note an Env2 already at the top — an Env2→cutoff pluck played as a held-open filter.
        void silenceEnvelopes() noexcept
        {
            outputStage_.silenceEnvelopes();
            outputStage_.voice().silenceOscEnvelopes();
            env2_.reset();
            env3_.reset();
            for (auto& d : envDraw_)
            {
                d.env.reset();
                d.drop();
            }
            envXfLeft_.fill (0); // nothing sounding to cross from, as `reset()` lands it
            envLast_.fill (0.0f);
        }

        // ---- Runtime scalar sources ----
        // Note velocity in [0,1]: drives the output stage's amp gain AND feeds the Velocity mod source.
        void setVelocity (float velocity01) noexcept
        {
            outputStage_.setVelocity (velocity01);
            velocity01_ = clampUnit (velocity01);
        }
        // [T-CGUI-69 leg 3] The same, on a voice that is SOUNDING: the amp gain glides (see
        // CathodeOutputStage::glideVelocity); the Velocity matrix SOURCE still moves at once.
        void glideVelocity (float velocity01) noexcept
        {
            outputStage_.glideVelocity (velocity01);
            velocity01_ = clampUnit (velocity01);
        }
        // [T-CMP-06] Live poly aftertouch — CathodeVoiceEngine::aftertouch() calls this on every
        // voice currently holding the pressed note, so a note re-pressed harder mid-hold updates
        // its OWN matrix source without touching any other held voice's.
        void setAftertouch (float pressure01) noexcept { aftertouch_ = clampUnit (pressure01); }
        // [T-CMP-14] The read-back twin, and it earns its place rather than pairing a setter for
        // symmetry: T-CMP-14 has to prove that a MONOPHONIC engine applies pressure to the whole
        // stack, and the only alternative was to route Aftertouch -> Amp through the matrix and infer
        // it from a rendered level delta — an inference that also passes when the matrix is what is
        // broken. Reads the voice's own stored source value (T-CORE-16: read the instrument).
        [[nodiscard]] float aftertouch() const noexcept { return aftertouch_; }
        // [T-CGUI-95] CHANNEL pressure (R69): the channel-wide twin, so it lives like the wheel — NOT cleared by
        // gateOn/release. `Source::Aftertouch` reads max(poly, channel); at 0 that is the poly value, bit-exact.
        void setChannelPressure (float pressure01) noexcept { channelPressure_ = clampUnit (pressure01); }
        [[nodiscard]] float channelPressure() const noexcept { return channelPressure_; }
        // The played note in Hz → the tone path (osc pitch / key-track / loop-error period). Stores the
        // base AND forwards it; a live Pitch destination re-applies pitchForMod(base, mod) per sample.
        void setNoteHz (Wide hz) noexcept
        {
            baseNoteHz_ = hz;
            // [T-CMP-21 leaf C] Through `applyPitch`, so the CHANNEL BEND rides every write of the
            // note rather than being re-derived at three call sites (the one that would be forgotten
            // is `justCleared (Dest::Pitch)`, where a bent note would silently snap back to concert).
            applyPitch();
            keyTrack_ = keyTrackForNote (hz);
            // [T-CORE-30 leaf D] The pitched punch flavour (fm_click) tracks the voice fundamental,
            // shifted by `punch.tune`. Derived HERE, at the one place the note arrives, for the same
            // reason `keyTrack_` is — a punch carrier the caller had to remember to set separately is
            // a carrier that ends up at the primitive's standalone 220 Hz default for every note (the
            // Source::KeyTrack-fed-a-permanent-0 defect, one line up). The primitive applies it on the
            // next trigger(), so the mono-glide path's per-sample setNoteHz costs a clamp and a store.
            punch_.setNoteHz (static_cast<float> (hz));
        }

        // [T-CORE-14] Full-depth KeyTrack spans ±this many octaves about the reference note, which
        // is middle C. ±5 covers MIDI ~0..120, i.e. the playable keyboard, so a routed Key→Cut opens
        // the filter across the range instead of saturating within an octave.
        static constexpr Wide kKeyTrackRefHz = 261.6255653005986; // C4
        static constexpr Real kKeyTrackOctaves = 5.0;

        // The KeyTrack mod source value for a note: bipolar about the reference, 0 at middle C.
        [[nodiscard]] static float keyTrackForNote (Wide hz) noexcept
        {
            if (!(hz > 0.0))
                return 0.0f;
            const Real octaves = std::log2 (hz / kKeyTrackRefHz) / kKeyTrackOctaves;
            return static_cast<float> (octaves < -1.0 ? -1.0 : (octaves > 1.0 ? 1.0 : octaves));
        }

        // [T-CORE-14 leaf B] The §2.4 mod envelopes' GLOBAL SOURCE GAIN (`synth.env{2,3}.amt`,
        // Param Ref:180). The spec states the row as "Global source gain on EnvN as a matrix source"
        // and defers its "relationship to per-slot DEPTH" to the L2 wire-in; the wire-in RULED it
        // MULTIPLICATIVE — the gain scales the VALUE fed to `setSourceValue (Source::EnvN, …)`, so
        // one row scales every route from that envelope and composes with each slot's own depth
        // (env · amt · depth). Bipolar, clamped to [-1, 1]: a negative amount INVERTS the envelope,
        // which is what the row's −64…+63 range is for, and 0 mutes it everywhere.
        //
        // The DEFAULT IS UNITY, deliberately not the registry's 0. This class is a neutral router:
        // every existing consumer (the M1-17d cases, `CathodeVoiceEngine`, the sibling machines)
        // sets no amount, so a 0 default would have silently muted Env2/Env3 for all of them and
        // moved goldens for a parameter they never touch. The registry's 0 arrives from core.
        //
        // Why here and not in the matrix: the matrix is a PURE router (one value per source, Σ v·d),
        // and a per-source gain there would be a second depth law in the evaluator. The gain belongs
        // to the source, and the source is owned here. Control-rate.
        void setEnv2Amount (float amount) noexcept { env2Amount_ = clampUnit (amount); }
        void setEnv3Amount (float amount) noexcept { env3Amount_ = clampUnit (amount); }
        // Read-back (the `Lfo::rateHz()` / `AdsrEnvelope::attackMs()` bit-exact-pin idiom): the app
        // layer's BipolarPercent conversion is pinned against this rather than inferred from a
        // render, because ±1.0 vs a hand-rolled 63/64 = 0.984 is the right-setter/wrong-conversion
        // survivor class the generated sweep structurally cannot see.
        [[nodiscard]] float env2Amount() const noexcept { return env2Amount_; }
        [[nodiscard]] float env3Amount() const noexcept { return env3Amount_; }

        // [T-CORE-14 leaf C] The §2.5 LFO SYNC POLICY — `synth.lfo{1..3}.sync` = Trig (Param Ref:194,
        // "restart phase at note-on, per voice"). `lfoIndex` is 0..2 = Lfo1..Lfo3; out-of-range is
        // ignored. Free (the default) leaves the LFO free-running, which is why an unset voice
        // renders bit-identically to one built before this leaf.
        //
        // The policy lives here and the phase mechanism lives in `Lfo::retrigger` — the split the
        // primitive's own header asks for ("any host-clock sync [is a] higher-layer concern"), and
        // the reason the spec's "spread offsets apply to the retriggered phase" clause holds without
        // this class knowing anything about spread.
        //
        // [T-CGUI-19] FIVE SOURCES, not three: the LFOs (0..2), the S&H source (3) and the Step
        // modulator (4), since `synth.sh.sync` / `synth.step.sync` exist (owner 2026-09-23/24). Their
        // `Sync` value is a RATE question the machine answers (the division against the tempo), so
        // nothing at L1 represents it. Control-rate.
        // BATTERY NOTE — AN EQUIVALENT MUTANT, as it was at three slots: relaxing the bound to
        // `> kTriggerableCount` lets index 5 write into the padding after the 5-byte array (before
        // `env2Amount_`), which debug and asan both miss. Kept because that equivalence is a layout
        // accident — reorder one member and the same write corrupts a real field.
        void setLfoTrig (int lfoIndex, bool retriggerOnNoteOn) noexcept
        {
            if (lfoIndex < 0 || lfoIndex >= kTriggerableCount)
                return;
            lfoTrig_[static_cast<std::size_t> (lfoIndex)] = retriggerOnNoteOn;
        }
        [[nodiscard]] bool lfoTrig (int lfoIndex) const noexcept
        {
            if (lfoIndex < 0 || lfoIndex >= kTriggerableCount)
                return false;
            return lfoTrig_[static_cast<std::size_t> (lfoIndex)];
        }

        // The KeyTrack mod source value, bipolar about a reference note. `setNoteHz` derives this
        // for every note, so this setter exists only to override it.
        //
        // It used to be the ONLY way in, with the header saying "how it is derived from the note is
        // the caller's concern" — and no caller in shipped code ever concerned itself, so
        // `Source::KeyTrack` fed a permanent 0 and every routing from it was silently dead. Nothing
        // noticed because nothing routed a slot at all until [T-CORE-14]; the generated sweep found
        // it the moment `synth.filt.keytrack` became a real wire (reported as a row that "changed
        // NOTHING"). Deriving it at the one place the note arrives means the source cannot go dead
        // by omission again.
        void setKeyTrack (float value) noexcept { keyTrack_ = value; }

        // ── [T-CGUI-08] The four MODULATION stage bypasses ──────────────────────────────────────────
        // `synth.{env,lfo,drift,mtx}.bypass` — each fades its family out over 10 ms and keeps every
        // parameter (R15). ENV = Env2/Env3 (incl. the Env-amt hard-wire), never the AMP envelope;
        // LFO = LFO 1-3, Step and S&H, still ticking; DRIFT = the three drifter values; MTX = the
        // sixteen user slots, the six hard-wires kept. Patch state, not host params. See the gate in
        // `processSample`.
        void setEnvBypassed (bool bypassed) noexcept { envFade_.set (bypassed); }
        void setLfoBypassed (bool bypassed) noexcept { lfoFade_.set (bypassed); }
        void setDriftBypassed (bool bypassed) noexcept { driftFade_.set (bypassed); }
        void setMtxBypassed (bool bypassed) noexcept { mtxFade_.set (bypassed); }
        [[nodiscard]] bool envBypassed() const noexcept { return envFade_.bypassed(); }
        [[nodiscard]] bool lfoBypassed() const noexcept { return lfoFade_.bypassed(); }
        [[nodiscard]] bool driftBypassed() const noexcept { return driftFade_.bypassed(); }
        [[nodiscard]] bool mtxBypassed() const noexcept { return mtxFade_.bypassed(); }
        [[nodiscard]] float envBypassGain() const noexcept { return envFade_.gain(); }
        [[nodiscard]] float lfoBypassGain() const noexcept { return lfoFade_.gain(); }
        [[nodiscard]] float driftBypassGain() const noexcept { return driftFade_.gain(); }
        [[nodiscard]] float mtxBypassGain() const noexcept { return mtxFade_.gain(); }

        // [T-MOD-01 leaf B] The three DRIFTER ring values for THIS sample — the first BROADCAST
        // source this class carries, and the reason it takes three plain floats rather than a
        // `DrifterBank&` is the split this file already draws: a voice owns its voice-local sources
        // (envs, LFOs, S&H, step) and ticks them itself, while anything computed once for the whole
        // instrument arrives from outside. `dsp::DrifterBank` is engine-global — one instance, ticked
        // ONCE PER SAMPLE by `CathodeVoiceEngine::processSample`, its three outputs broadcast
        // identically into every voice — so a per-voice reference to it would invite exactly the bug
        // it must not have: a bank advanced once per VOICE per sample, i.e. at 8× or 16× its dialed
        // rate depending on the polyphony. Keeping the voice ignorant of the bank makes that
        // unexpressible here.
        //
        // Called every sample, BEFORE processSample; the values are stored and fed to the matrix in
        // step 2 alongside velocity/key-track (the other two latched scalars). A voice never told
        // holds 0, which is the same silent state Drifter1..3 had before this card.
        void setDrifterValues (float v0, float v1, float v2) noexcept
        {
            drifter_[0] = v0;
            drifter_[1] = v1;
            drifter_[2] = v2;
        }

        // ── [T-CMP-21 leaf C] The two CHANNEL-WIDE performance controls ──────────────────────────
        // Both are BROADCAST like the drifters and, unlike `setAftertouch`, both SURVIVE a note:
        // aftertouch is per-note pressure and is zeroed at `gateOn`/release, while a wheel a player
        // has parked at 40 % must still modulate the note they play next. That difference is the
        // whole reason these are not another `setAftertouch`.
        //
        // MOD WHEEL feeds `Source::ModWheel`, which has been a 0-valued placeholder since
        // T-MASK-03 leaf F1 — the picker offered "Wheel" and a route built on it did nothing.
        void setModWheel (float wheel01) noexcept { modWheel_ = clampUnit (wheel01); }
        // The read-back twin, for the `aftertouch()` reason: a MONO engine has to be shown feeding
        // the whole stack, and inferring it from a rendered level also passes when the matrix is
        // what is broken (T-CORE-16 — read the instrument).
        [[nodiscard]] float modWheel() const noexcept { return modWheel_; }

        // PITCH BEND is NOT a matrix source — there is no such `Source`, and the owner ruled it
        // NATIVE to voice pitch at ±2 st (OI-MIDI-6, 2026-08-16). `bend` is the normalised wheel,
        // -1 (full down) … +1 (full up); the semitone span is `bendSemitones_` (`setBendRange`).
        //
        // The RATIO is cached here, at control rate, and not recomputed per sample: this is the one
        // place an `exp2` would otherwise land on the hot path, which is the cost class T-CMP-18
        // exists to remove rather than add to. At rest the ratio is exactly 1.0 and the hot path's
        // multiply is therefore bit-exact — no golden moves until a wheel is touched.
        void setPitchBend (float bend) noexcept
        {
            bend_ = clampUnit (bend);
            bendRatio_ = fm::exp2 (static_cast<Real> (bend_) * bendSemitones_ / 12.0);
            applyPitch();
        }
        [[nodiscard]] float pitchBend() const noexcept { return bend_; }
        // [T-CGUI-96] The span, clamped 0..24. A HELD bend re-scales at once; at rest the ratio stays exp2(0) == 1.0, so
        // a range change moves nothing until the wheel does. Patch state, so a voice `reset()` leaves it alone.
        void setBendRange (Real semitones) noexcept
        {
            // [D-M-0118] A non-finite span is refused (`std::clamp` passes NaN) and the span is whole semitones.
            if (!std::isfinite (semitones))
                return;
            bendSemitones_ = std::clamp (std::round (semitones), 0.0, kMaxBendSemitones);
            setPitchBend (bend_);
        }
        [[nodiscard]] Real bendRange() const noexcept { return bendSemitones_; }
        // The base note as the BEND leaves it, before any Pitch-destination modulation.
        [[nodiscard]] Wide bentNoteHz() const noexcept { return baseNoteHz_ * bendRatio_; }
        // [T-CMP-07 leaf 6] The four MACROS, broadcast for the same reason the drifters are: a macro
        // is ONE machine-wide control, so a per-voice copy would be a second record that could
        // disagree with the one the apply table scales its rows by. Pushed from L5 whenever a macro
        // row applies — not per sample, because a macro only moves when the user moves it — and a
        // voice never told holds 0, which is the value that contributes nothing.
        void setMacroValues (float grit, float motion, float oscPos, float pdAmt) noexcept
        {
            macro_[0] = grit;
            macro_[1] = motion;
            macro_[2] = oscPos;
            macro_[3] = pdAmt;
        }

        // [T-CMP-14] The read-back twin, for the same reason as `aftertouch()` above: the engine
        // broadcasts drifter values over a RANGE that depends on the voice mode, and "did voice 3 get
        // fed" is a question about the broadcast, not about what the matrix then does with it.
        [[nodiscard]] float drifterValue (int i) const noexcept
        {
            return (i < 0 || i >= static_cast<int> (drifter_.size()))
                       ? 0.0f
                       : drifter_[static_cast<std::size_t> (i)];
        }

        // ---- Base (unmodulated) values for the wired destinations ----
        // Each stores the base AND forwards it to the output stage so the patch applies when the
        // destination is un-live; a live destination is re-applied (base + mod) per sample.
        // Base cutoff (Hz) + base resonance Q. Q is the FiltReso base; cutoff is the FiltCutoff base. Both
        // forward to the SAME voice().setCutoff; a live FiltCutoff/FiltReso re-applies its remap per sample.
        // [T-CMP-26] EVERY filter cutoff/resonance push goes through here, so the model zoo is
        // reached wherever the TptSvf is. Before this card the four call sites below wrote
        // `setCutoff` ALONE, which is `filter_.setCutoff` and nothing else — the TptSvf. On the
        // seven non-SVF models the matrix computed, the write landed, and the filter actually in
        // the signal path never saw it: LFO->cutoff, env->cutoff, velocity->cutoff and the
        // hard-wired keytrack were all silent on seven of eight models.
        //
        // BOTH calls are unconditional and that is deliberate rather than lazy: `setCutoff` writes
        // the TptSvf, which is BYPASSED in every non-SVF mode, and `setModelCutoffReso` has no arm
        // for the SVF/Comb/Formant modes. Each is inert exactly where the other is live, so no
        // caller has to know the mode and there is no second copy of the mode switch to drift.
        //
        // The Q is converted ONCE, here, by the same law the control-rate base uses — see
        // `FilterResonance.h`. If these two laws ever diverge again, routing an LFO at depth 0
        // would step the resonance, which is the shape that made this card's leaf A necessary.
        void pushFilterCutoffReso (float cutoffHz, float q) noexcept
        {
            if constexpr (fork::kExact) // FIREFORK E17: the same operands into the same model leave every coefficient where it is
            {
                const std::uint32_t mw = outputStage_.voice().filterModeWrites();
                const bool resoLive = destLive_[static_cast<std::size_t> (Dest::FiltReso)];
                NEMESIS_FORK_COUNT (pushCalls);
                if (pushValid_ && cutoffHz == pushHz_ && q == pushQ_ && mw == pushModeW_ && resoLive == pushResoLive_)
                {
                    NEMESIS_FORK_COUNT (pushSkips);
                    return;
                }
                pushValid_ = true;
                pushHz_ = cutoffHz;
                pushQ_ = q;
                pushModeW_ = mw;
                pushResoLive_ = resoLive;
            }
            outputStage_.voice().setCutoff (cutoffHz, q);
            float reso01;
            if constexpr (fork::kExact) // FIREFORK E3d: two divisions per push, the same value while Q stands still
            {
                if (!(q == pushedQ_))
                {
                    pushedQ_ = q;
                    pushedReso01_ = resonance::resonance01ForQ (q);
                }
                reso01 = pushedReso01_;
            }
            else
                reso01 = resonance::resonance01ForQ (q);
            outputStage_.voice().setModelCutoffReso (cutoffHz, reso01);
            // [T-CGUI-47] …and the Comb, the one mode neither call above reaches.
            outputStage_.voice().setCombCutoffReso (cutoffHz, q, destLive_[static_cast<std::size_t> (Dest::FiltReso)]);
        }

        void setBaseCutoff (float cutoffHz, float q) noexcept
        {
            baseCutoffHz_ = cutoffHz;
            baseQ_ = q;
            pushFilterCutoffReso (cutoffHz * organicCutoffMult_, q);
            if (!destLive_[static_cast<std::size_t> (Dest::FiltCutoff)])
                effectiveCutoffHz_ = cutoffHz * organicCutoffMult_;
            if (!destLive_[static_cast<std::size_t> (Dest::FiltReso)])
                effectiveQ_ = q;
        }

        // [T-CMP-11] Sets THIS note's Organic cutoff contribution (1.0 = none) and, only when it is
        // NOT the neutral 1.0, pushes the recomputed cutoff — the bug this shape exists to close:
        // setting only the member left the filter reading whatever cutoff the LAST setBaseCutoff/
        // matrix-recompute happened to leave, because neither of those paths runs again on a gate
        // that changes no other cutoff operand.
        //
        // GUARDED on `mult != 1.0f` for the SAME reason CathodeVoiceEngine::noteOnMono's stackGain_
        // write is: several tests configure a voice's cutoff DIRECTLY on the underlying
        // CathodeVoiceStage (`outputStage().voice().setCutoff(...)`, bypassing setBaseCutoff/
        // baseCutoffHz_ entirely — the KEYSTONE reference-matching pattern), so an unconditional
        // push here would compute `baseCutoffHz_ * 1.0` off baseCutoffHz_'s stale UNSET default
        // (20000 Hz) and silently overwrite whatever that direct call had set — a real regression
        // caught by the engine's own bit-identical-to-reference goldens.
        //
        // Mirrors setBaseCutoff's own not-live/live split so a route already modulating FiltCutoff
        // keeps doing so (its next per-sample recompute reads the freshly-stored multiplier on its
        // own). Called by CathodeVoiceEngine::assignVoice right before gateOn() — a fresh
        // per-note-trigger value, never accumulated across notes.
        void setOrganicCutoffMult (float mult) noexcept
        {
            organicCutoffMult_ = mult;
            if (outputStage_.voice().filter2On())
                pushF2(); // [T-CGUI-60] F2 carries this note's Organic offset too (no direct-config legacy to guard)
            if (mult == 1.0f)
                return;
            pushFilterCutoffReso (baseCutoffHz_ * organicCutoffMult_, effectiveQ_);
            if (!destLive_[static_cast<std::size_t> (Dest::FiltCutoff)])
                effectiveCutoffHz_ = baseCutoffHz_ * organicCutoffMult_;
        }
        // [T-CGUI-60] F2's cutoff (Hz) and Q — what its generic-destination writers deliver (the knob,
        // plus F2's OWN routes on `synth.filt2.{cutoff,reso}`). F1's cutoff MODULATION rides on top
        // (owner 2026-09-27: key-track and env amount are F1's rows and apply to both): every
        // `FiltCutoff` route — the key-track and env-amount hard-wires included — and this note's
        // Organic offset, each from F2's OWN base (the FLTR "each stage from its own base" law). F1's
        // per-voice cutoff SPREAD is a patch-base offset of F1's knob and does not reach F2.
        // [D-M-0158] F2's OWN route is step-limited like F1's modulation: a ROUTE write moving the cutoff
        // more than kCutoffSlewStepOctaves in one sample ramps there in octaves over kCutoffSlewSeconds;
        // a smaller move passes as the same float. A BASE write (the knob, a restore, a direct call)
        // lands at once and re-opens the onset window, so a route added later starts on its own value.
        void setF2Cutoff (float cutoffHz) noexcept
        {
            f2CutoffHz_ = cutoffHz;
            if (!genericRouteWrite_)
                f2CutoffSlew_.reset();
            else if (const float oct = f2CutoffSlew_.process (std::log2 (cutoffHz)); f2CutoffSlew_.ramping())
                f2CutoffHz_ = fm::exp2 (oct);
            pushF2();
        }
        void setF2Reso (float q) noexcept
        {
            f2Q_ = q;
            pushF2();
        }
        // Re-pushes F2 through its CURRENT mode's dispatch — owed after a mode change (a model swap
        // moves which core the cutoff reaches), and after `synth.filt2.on` turns F2 on.
        void refreshF2() noexcept { pushF2(); }
        [[nodiscard]] float effectiveF2CutoffHz() const noexcept { return f2EffectiveHz_; }
        [[nodiscard]] float f2Resonance() const noexcept { return f2Q_; }

        // Base output level, linear gain in [0, CathodeOutputStage::kMaxLevel] (§22.8 Levels — the
        // name carries no `01` because the domain is not 0..1; T-CORE-09).
        void setBaseLevel (float level) noexcept
        {
            baseLevel_ = level;
            outputStage_.setLevel (level);
            if (!destLive_[static_cast<std::size_t> (Dest::Amp)])
                effectiveLevel_ = level;
        }
        void setBasePan (float pan) noexcept
        {
            basePan_ = pan;
            outputStage_.setPan (pan);
            if (!destLive_[static_cast<std::size_t> (Dest::Pan)])
                effectivePan_ = pan;
        }
        // §2 broken-converter bases (M1-17f). Each stores the base AND forwards it to the tone path's
        // converter setter so the patch applies while the destination is un-live; a live destination is
        // re-applied (remap(base + mod)) per sample. Native converter rate in Hz (ConvRate).
        void setBaseConverterRate (float rateHz) noexcept
        {
            baseConverterRateHz_ = rateHz;
            outputStage_.voice().setConverterRate (rateHz);
            if (!destLive_[static_cast<std::size_t> (Dest::ConvRate)])
                effectiveConverterRateHz_ = rateHz;
        }
        // Storage word length in bits (ConvBits; BrokenConverter clamps [1, 24]).
        void setBaseBitDepth (float bits) noexcept
        {
            baseBitDepth_ = bits;
            outputStage_.voice().setBitDepth (bits);
            if (!destLive_[static_cast<std::size_t> (Dest::ConvBits)])
                effectiveBitDepth_ = bits;
        }
        // [T-CMP-04 leaf 2] HARD-SYNC amount in [0, 1] (Dest::Sync; 0 = bit-exact bypass).
        //
        // THIS IS THE VOICE'S BASE AND THE APP LAYER MUST WRITE IT, not `CathodeVoiceStage::
        // setSyncAmount` one layer down. T-CORE-14 leaf C: a live destination re-applies
        // `remap(base + mod)` from the base EVERY SAMPLE, so a knob that wrote past the base is
        // overwritten within one sample of anything being routed there — which is how the BITS
        // knob shipped completely dead while the apply-table sweep reported its row as changing
        // nothing. The two `synth.xmod.*` depths are exempt only because no cross-mod dest
        // exists; sync's does, so this row is squarely in the class.
        void setBaseSyncAmount (float amount) noexcept
        {
            baseSyncAmount_ = clamp01 (amount);
            outputStage_.voice().setSyncAmount (baseSyncAmount_);
            if (!destLive_[static_cast<std::size_t> (Dest::Sync)])
                effectiveSyncAmount_ = baseSyncAmount_;
        }

        // The amount the oscillator is ACTUALLY running at, after any live route — the read-back
        // twin every modulated quantity here carries, and the only way a test can say WHICH
        // value arrived rather than that the render moved.
        [[nodiscard]] float effectiveSyncAmount() const noexcept { return effectiveSyncAmount_; }

        // Loop-point error amount in [0, 1] (ConvLoopErr; 0 = bit-exact bypass).
        void setBaseLoopError (float amount) noexcept
        {
            baseLoopError_ = amount;
            outputStage_.voice().setLoopError (amount);
            if (!destLive_[static_cast<std::size_t> (Dest::ConvLoopErr)])
                effectiveLoopError_ = amount;
        }
        // Drift/jitter macro in [0, 1] (ConvDrift; 0 = bit-exact bypass).
        void setBaseDrift (float amount) noexcept
        {
            baseDrift_ = amount;
            outputStage_.voice().setDrift (amount);
            if (!destLive_[static_cast<std::size_t> (Dest::ConvDrift)])
                effectiveDrift_ = amount;
        }
        // §1 osc / §3 filter bases (M1-17g). WT morph position in [0, 1] (OscPos) for ONE osc slot.
        //
        // [T-CORE-16] PER SLOT, and it used to write BOTH. One destination modulates "the oscillator
        // position", but `synth.osc{1,2}.pos` are two independent params, so a single both-slots base
        // meant a LIVE OscPos re-applied one effective value to both oscillators every sample — which
        // silently collapsed the two params into one and left both knobs DEAD for any patch that
        // modulates the destination (T-CORE-14 leaf C found it by measurement: the generated sweep
        // reported `synth.conv.bits` changing nothing the moment StepMod was routed at it, and
        // enumerating the class found these two the only members needing an L1 change rather than a
        // one-line move). The DESTINATION stays single — `destLive_` and the matrix sum are per
        // destination, exactly as before. What split is the BASE, and with it the effective value and
        // the un-live restore, because a per-slot base restored to a shared value is the same defect
        // arriving three lines later.
        void setBaseOscPosition (int slot, float pos) noexcept
        {
            const auto s = slotIdx (slot);
            baseOscPos_[s] = pos;
            outputStage_.voice().setPosition (static_cast<int> (s), pos);
            if (!destLive_[static_cast<std::size_t> (Dest::OscPos)])
                effectiveOscPos_[s] = pos;
        }
        // PD warp depth in [0, 1] (PdAmount) for ONE osc slot — the twin of the above, same reason.
        void setBasePdAmount (int slot, float amount) noexcept
        {
            const auto s = slotIdx (slot);
            basePdAmount_[s] = amount;
            outputStage_.voice().setPdAmount (static_cast<int> (s), amount);
            if (!destLive_[static_cast<std::size_t> (Dest::PdAmount)])
                effectivePdAmount_[s] = amount;
        }
        // [T-MASK-03 leaf F1] The BASE mask for one slot (0..511 — `synth.osc{1,2}.mask`), and the
        // per-slot twin of `setBaseOscPosition` above for the same reason: the two masks move
        // independently, so a shared base would be the defect that rule was written about.
        void setBaseMask (int slot, int mask) noexcept
        {
            const auto s = slotIdx (slot);
            baseMask_[s] = mask;
            // Written to the oscillator unconditionally: with the dest live the next sample overwrites
            // it with base+mod anyway, and with the dest un-live this IS the value that sounds. No
            // conditional copy to keep in step — the oscillator is the record.
            outputStage_.voice().setMask (static_cast<int> (s), mask);
        }
        // [T-CMP-15 leaf D2] The BASE pulse width for one slot (`synth.osc{1,2}.pw`, a fraction of
        // the cycle spent high) — the per-slot twin of `setBaseMask` above, and per-slot for the
        // same reason: R2 gives the two widths their own dests precisely so they can move apart.
        void setBasePulseWidth (int slot, Real duty) noexcept
        {
            const auto s = slotIdx (slot);
            basePulseWidth_[s] = duty;
            // Unconditional, like the mask: with the dest live the next sample overwrites it with
            // base+mod, and with the dest un-live this IS the value that sounds. The oscillator is
            // the record; there is no effective copy to keep in step.
            outputStage_.voice().setPulseWidth (static_cast<int> (s), duty);
        }

        // The pulse SAW MIX for one slot. No dest, no base/effective split — nothing modulates it,
        // so it goes straight through and is stated here rather than left to be inferred.
        void setPulseSawMix (int slot, float mix) noexcept
        {
            outputStage_.voice().setPulseSawMix (static_cast<int> (slotIdx (slot)), mix);
        }

        // [T-PROTO-02 leaf 4] The scan order for one slot. No dest, no base/effective split —
        // nothing modulates it, so it goes straight through, exactly like the saw mix above.
        //
        // It is deliberately NOT a modulation destination, and that is a design statement rather
        // than an omission: the value selects WHICH PERMUTATION the position scan uses, so
        // sweeping it would re-map every frame under a moving position and produce a discontinuity
        // per step. The axis meant to be modulated is `pos` itself, which already is one
        // (`Dest::OscPos`, all slots since M1-17g).
        void setScanOrder (int slot, ScanOrder order) noexcept
        {
            outputStage_.voice().setScanOrder (static_cast<int> (slotIdx (slot)), order);
        }

        // [T-CMP-15 leaf D2] The PWM pair, and this is the one hard-wire on this mechanism whose
        // SOURCE varies. `filt.envAmt` is always Env2 and the two mask hard-wires are always their
        // own DeltaDecay, so `setSlot`'s src argument has been a constant on every reserved slot
        // until now — `synth.osc{1,2}.pwSrc` makes it a value the player picks. Both rows land in
        // one place because either one changing has to re-install the same slot: holding them apart
        // would mean a depth edit could resurrect a stale source, and vice versa.
        //
        // `Manual` (index 0) CLEARS the slot. So does depth 0. Both are the off-anchor, and both
        // clear rather than installing a zero-depth route — `applyMatrix`'s rule, since a live dest
        // re-pushes its base every sample and these rows default to off.
        // Takes a `Source`, NOT the registry's row index. The `synth.osc{1,2}.pwSrc` roster order is
        // an L2 fact, and L1 has no business knowing it — the app layer already owns exactly this
        // kind of translation (`oscModeFor`, `matrixSourceFor`), so the index→Source map lives there
        // with its siblings. `std::nullopt` is `Manual`: no route at all, which is why this is an
        // optional rather than a sentinel value inside the enum.
        void setPulseWidthModSource (int slot, std::optional<Source> source) noexcept
        {
            const auto s = slotIdx (slot);
            pwmSource_[s] = source;
            refreshPwmSlot (s);
        }
        void setPulseWidthModAmount (int slot, float amount01) noexcept
        {
            const auto s = slotIdx (slot);
            pwmAmount_[s] = amount01;
            refreshPwmSlot (s);
        }

        // The mask ENV depth for one slot, as a unit interval — installed as this slot's reserved
        // hard-wire routing (DeltaDecay{1,2} → Osc{1,2}Mask). 0 CLEARS the slot rather than installing
        // a zero-depth route, which is `applyMatrix`'s own rule: a live dest re-pushes its base every
        // sample, and this row defaults to 0.
        void setMaskEnvAmount (int slot, float amount01) noexcept
        {
            const auto s = slotIdx (slot);
            const int hardWire = s == 0 ? CathodeModMatrix::kMaskEnv1Slot : CathodeModMatrix::kMaskEnv2Slot;
            const auto source = s == 0 ? Source::DeltaDecay1 : Source::DeltaDecay2;
            const auto dest = s == 0 ? Dest::Osc1Mask : Dest::Osc2Mask;
            if (amount01 == 0.0f)
                clearSlot (hardWire);
            else
                setSlot (hardWire, source, dest, amount01);
        }
        // One slot's mask-transient DECAY time in ms (`synth.osc{1,2}.maskDecay`). Control-rate.
        void setMaskDecayMs (int slot, float ms) noexcept { dd_[slotIdx (slot)].setDecayMs (ms); }

        // ---- [D-M-0193] THE SUB's two hard-wires — the OSC 1/2 pair's shape on the third oscillator ----
        // `synth.sub.{maskEnv,maskDecay}` and `synth.sub.{pwSrc,pwm}`. The SUB's mask and width are
        // GENERIC destinations (see `CathodeModMatrix::kMaskEnvSubSlot`), so the app hands over their ids
        // once (`setSubHardWireDests`) and each hard-wire routes to the id a user route to the same row
        // uses: the two SUM in the evaluator, under the generic law (offset by depth x the row's span,
        // CLAMPED — where OSC 1/2's named mask dest folds through the 9-bit space). Off-states CLEAR the
        // slot (depth 0, Manual, or no dest bound), `applyMatrix`'s rule.
        void setSubHardWireDests (CathodeModMatrix::DestId maskDest, CathodeModMatrix::DestId pwDest) noexcept
        {
            subMaskDest_ = maskDest;
            subPwDest_ = pwDest;
            refreshSubHardWires();
        }
        void setSubMaskEnvAmount (float amount01) noexcept
        {
            subMaskEnvAmount_ = amount01;
            refreshSubHardWires();
        }
        void setSubMaskDecayMs (float ms) noexcept { dd_[kSubMaskEnv].setDecayMs (ms); }
        void setSubPulseWidthModSource (std::optional<Source> source) noexcept
        {
            subPwmSource_ = source;
            refreshSubHardWires();
        }
        void setSubPulseWidthModAmount (float amount01) noexcept
        {
            subPwmAmount_ = amount01;
            refreshSubHardWires();
        }
        [[nodiscard]] float subMaskEnvValue() const noexcept { return dd_[kSubMaskEnv].value(); }
        [[nodiscard]] float subMaskDecayMs() const noexcept { return dd_[kSubMaskEnv].decayMs(); }

        // Formant vowel-morph position in [0, 1] (FiltVowel; only audible in Formant filter mode).
        void setBaseVowel (float position) noexcept
        {
            baseVowel_ = position;
            outputStage_.voice().setVowel (position);
            if (!destLive_[static_cast<std::size_t> (Dest::FiltVowel)])
                effectiveVowel_ = position;
        }
        // §5 texture-layer base (M1-18c). Noise level in [0, 1] (NoiseLevel; 0 = the texture layer's
        // bit-exact bypass — the stage's primed-OFF default, matched by this base's default).
        void setBaseNoiseLevel (float level01) noexcept
        {
            baseNoiseLevel_ = level01;
            outputStage_.voice().setNoiseLevel (level01);
            if (!destLive_[static_cast<std::size_t> (Dest::NoiseLevel)])
                effectiveNoiseLevel_ = level01;
        }

        // ---- Sub-stage / source access (configure the rest of the patch + the source params) ----
        [[nodiscard]] CathodeOutputStage& outputStage() noexcept { return outputStage_; }
        [[nodiscard]] const CathodeOutputStage& outputStage() const noexcept { return outputStage_; }
        // Convenience delegators to the underlying output stage's tone path + AMP envelope (mirrors
        // CathodeOutputStage's own voice()/amp() — a modulated voice IS a voice with a tone path + AMP
        // env, so the ~30 tone setters + the AMP DADSR are configured directly rather than re-delegated).
        [[nodiscard]] CathodeVoiceStage& voice() noexcept { return outputStage_.voice(); }
        [[nodiscard]] const CathodeVoiceStage& voice() const noexcept { return outputStage_.voice(); }
        [[nodiscard]] AdsrEnvelope& amp() noexcept { return outputStage_.amp(); }
        [[nodiscard]] const AdsrEnvelope& amp() const noexcept { return outputStage_.amp(); }
        [[nodiscard]] AdsrEnvelope& env2() noexcept { return env2_; }
        [[nodiscard]] AdsrEnvelope& env3() noexcept { return env3_; }
        // [T-CGUI-61 leaf E6] DRAW mode for the two free mod envelopes (`synth.env{2,3}.draw`): `n` is
        // 2 or 3. On, the Env2/Env3 SOURCE is `drawEnv (n)` instead of `env2()`/`env3()` — the same
        // matrix source, the same `synth.env{2,3}.amt` gain and ENV bypass fade on top. Off: bit-exact.
        // Out-of-range `n` is a no-op / ENV 2's (the index-taking accessors' convention here).
        // [S5R2] ANY THREAD: only the request is stored; `applyDrawEdges()` applies it (see setAmpDraw).
        void setEnvDraw (int n, bool on) noexcept
        {
            if (n == 2 || n == 3)
                envDrawReq_[static_cast<std::size_t> (n - 2)].store (on, std::memory_order_relaxed); // [row 303]
            envDrawDirty_.store (true, std::memory_order_release); // FIREFORK E9
        }
        // `synth.env{2,3}.dtime` — ANY THREAD, request only; `applyDrawEdges()` applies it (S5R3).
        void setEnvDrawLengthMs (int n, Real ms) noexcept
        {
            if (n == 2 || n == 3)
                envDraw_[static_cast<std::size_t> (n - 2)].requestLengthMs (ms);
            envDrawDirty_.store (true, std::memory_order_release); // FIREFORK E9
        }
        // The REQUESTED mode (the patch state); the audio path follows it from its next sample.
        [[nodiscard]] bool envDraw (int n) const noexcept
        {
            return (n == 2 || n == 3) && envDrawReq_[static_cast<std::size_t> (n - 2)].load (std::memory_order_relaxed);
        }
        // AUDIO THREAD, at the top of `processSample` and `evaluateModulationNow` — every requested
        // DRAW flip and `.dtime` (AMP + both twins) lands here, before any source is read. The On EDGE
        // starts a twin clean (S5R): from its drawn start if the key is held NOW, silent Idle
        // otherwise — an Off twin was never gated. Steady state: six predictable not-taken branches
        // (a length and a flag per envelope), no writes; the output stage re-checks its two later in
        // the sample, so eight a sample in all.
        // [row 304] A flip while the voice SOUNDS crossfades the twin and its ADSR over the drawn
        // envelope's de-click time (the AMP stage's law); on a silent voice it lands at once.
        // `atSilentNoteOn` (from `gateOn`, AMP idle): nothing is heard to cross from, and the gate restarts
        // both sources, so every flip lands at once — a held drawn value or a releasing ADSR included.
        void applyDrawEdges (bool atSilentNoteOn = false) noexcept
        {
            outputStage_.applyDrawEdge();
            if constexpr (fork::kExact) // FIREFORK E9
            {
                if (!envDrawDirty_.load (std::memory_order_acquire))
                    return;
                envDrawDirty_.store (false, std::memory_order_relaxed);
            }
            for (std::size_t i = 0; i < envDraw_.size(); ++i)
            {
                envDraw_[i].applyLengthRequest();
                const bool req = envDrawReq_[i].load (std::memory_order_relaxed);
                if (req == envDrawOn_[i])
                    continue;
                envDrawOn_[i] = req;
                if (envXfLeft_[i] > 0)
                {
                    // [D-M-0173] A flip inside a running crossfade turns it round: the weight carries on
                    // (N - left) rather than restarting from the pure old source; the twin ticked through
                    // the fade, so it is still live and is not reset.
                    envXfLeft_[i] = envXfLen_ - envXfLeft_[i];
                    continue;
                }
                // [D-M-0173] Crossfade only while THIS envelope sounds — its ADSR running, its drawn twin
                // running, or a finished drawn one-shot still holding above 0 — never on the AMP's say.
                const bool heldDrawn = !envDrawOn_[i] && envLast_[i] > 0.0f;
                const bool sounding = !atSilentNoteOn && (modAdsr (i).active() || envDraw_[i].env.isActive() || heldDrawn);
                envXfLeft_[i] = sounding ? envXfLen_ : 0;
                if (!envDrawOn_[i])
                    continue;
                envDraw_[i].env.reset();
                envDraw_[i].drop();
                if (outputStage_.keyHeld())
                    envDraw_[i].env.gateOn();
            }
        }
        // True while ENV `n`'s DRAW flip crossfades (for tests).
        [[nodiscard]] bool envDrawCrossfading (int n) const noexcept { return envXfLeft_[n == 3 ? 1u : 0u] > 0; }
        [[nodiscard]] DrawEnvelope& drawEnv (int n) noexcept { return envDraw_[n == 3 ? 1u : 0u].env; }
        [[nodiscard]] const DrawEnvelope& drawEnv (int n) const noexcept { return envDraw_[n == 3 ? 1u : 0u].env; }
        [[nodiscard]] Lfo& lfo1() noexcept { return lfo1_; }
        [[nodiscard]] Lfo& lfo2() noexcept { return lfo2_; }
        [[nodiscard]] Lfo& lfo3() noexcept { return lfo3_; }
        [[nodiscard]] StepModulator& step() noexcept { return step_; }
        // The dedicated S&H source (an Lfo locked to SampleHold; configure its rate/depth/seed here).
        [[nodiscard]] Lfo& sampleHold() noexcept { return sh_; }
        // [T-CORE-30 leaf D] The §2 onset transient. Five of its six `punch.*` rows are the
        // primitive's own setters, reached through here exactly as the envelope/LFO rows reach
        // `env2()`/`lfo1()` — this class adds the two things the primitive cannot own: WHEN it fires
        // (gateOn) and WHERE it sums (setPunchMix).
        [[nodiscard]] PunchTransient& punch() noexcept { return punch_; }
        [[nodiscard]] const PunchTransient& punch() const noexcept { return punch_; }
        // `punch.mix` — which of the two §2 summing points the burst joins. Control-rate; takes
        // effect on the next sample (it selects a node, not a coefficient, so there is nothing to
        // recompute and nothing to glitch).
        void setPunchMix (PunchMix mix) noexcept { punchMix_ = mix; }
        [[nodiscard]] PunchMix punchMix() const noexcept { return punchMix_; }

        // [T-CMP-07 leaf 4a, WIDENED AT 4c] How a generic destination reaches the tone path. The
        // writer's conversion stays where it is — at L5, in the apply row, which is the only place
        // that knows the row's curve — and only the object it is handed changes.
        //
        // 4a took `CathodeVoiceStage&`, on the ground that it is "the object ~90 % of the per-voice
        // apply rows already write through". That figure came from FINDING 8's hand classification,
        // and FINDING 11 measured it against the real work-list: the stage reaches 25 of the 83
        // rows. The other 37 write to the VOICE — its own setters (`setBitmaskEnvAmount`,
        // `setPulseWidthMod`, `setHpfCutoff`, the LFO delays) or one of its members (`env2()`,
        // `env3()`, `punch()`, `step()`, the amp envelope, the output stage) — and no amount of
        // per-row cleverness reaches those from the stage, because the stage does not own them.
        //
        // THE VOICE IS THE WIDER OBJECT AND IT COSTS NOTHING. `outputStage().voice()` hands back the
        // stage, so a stage-targeting writer is one call longer and the other 37 become expressible;
        // and the apply loop below already runs INSIDE the voice, so it passes `*this` rather than
        // reaching through a member. A reference to the enclosing class is well-formed here — the
        // pointee is never completed at the point of the alias.
        using GenericWrite = void (*) (CathodeModulatedVoice&, float) noexcept;

        // [T-CMP-07 leaf 4c] HOW a generic destination's modulation combines with its base, because
        // ONE law is wrong for half the roster. FINDING 12: `Linear` is `base + mod * (hi - lo)`,
        // and `lo`/`hi` are also the CLAMP, so a wide-range row cannot narrow its span without
        // breaking its own reachable range — `synth.amp.d` is ExpZero 0–10000 ms against a 39 ms
        // default, so a 1 % route adds 100 ms and no depth setting is usable.
        //
        // This is not a new idea in this file. The BESPOKE destinations already split the same way
        // and say why at `kCutoffModOctaves`: "ConvRate and ConvBits are WIDE-range params (Hz and
        // [1,24] bits) where a raw ±1 offset is musically negligible — like FiltCutoff they take a
        // SCALED remap". `Octaves` is `cutoffForMod`'s law generalised — `base * 2^(mod * span)`,
        // still clamped to `[lo, hi]` — so an exponentially-curved row modulates in RATIO, which is
        // the domain its curve already put it in.
        enum class GenericLaw : std::uint8_t {
            Linear,
            Octaves,
            // [T-CMP-07 leaf 4c batch 3] `base * (1 + mod * span)`, clamped. For a LEVEL or a DEPTH
            // whose apply row GATES it on an on/off — `v = enabled ? value : 0` — the gate is
            // consumed BEFORE the base is formed, so the base already carries the answer and the
            // law must not undo it. Under `Linear` it does exactly that: base 0 plus
            // `mod * (hi - lo)` is non-zero, so an LFO routed at `synth.osc1.level` sounds while
            // oscillator 1 is switched OFF. Multiplying leaves zero at zero, which is the point.
            //
            // It is also the honest law for the quantity: modulating a depth should SCALE that
            // depth, not add a fixed amount to it — an LFO on `synth.lfo2.depth` at half depth
            // means "half as much LFO 2", which is a ratio. `Octaves` preserves zero too and is
            // far too coarse for a unit-interval control.
            Proportional
        };

        // [T-CMP-16 F3] AT WHAT RATE THE DESTINATION'S SETTER IS CALLED. Owner ruling 2026-08-31.
        //
        // The generic apply calls each destination's own setter once per sample per voice, and
        // several of those setters are documented "Control-rate" at their own declaration because
        // they REBUILD something: `synth.reson.*` runs `MetallicResonator::configureModes`, which is
        // six `ModalResonator::recomputeMode` calls, which is eighteen transcendentals. Measured on
        // the benchmark's EngineCore fixture — 4 SOUNDING voices (D-M-0497; it was labelled 16) —
        // routing the four resonator destinations took the engine from 13.66 us to 153.30 us per
        // 64-sample block — 1 % of the RT budget to 11.5 %, an eleven-fold cost the user reaches
        // with four matrix slots.
        //
        // WHY DECIMATION AND NOT A CHANGE-SKIP. The obvious mitigation — write only when the value
        // differs from the last one — saves nothing in the case that costs: under a moving LFO the
        // value changes every sample. A static route was already cheap. Only reducing the RATE helps.
        //
        // WHY IT COSTS NOTHING MUSICALLY. `Control` writes every `kGenericControlDecimation`
        // samples, which at 48 kHz is 750 Hz — far above any LFO, envelope or macro gesture that can
        // be routed here, and these destinations shape the resonator's MATERIAL (its brightness,
        // ring time, inharmonicity) rather than carrying a waveform. There is no musical gesture
        // that retunes a ring time at audio rate.
        //
        // PHASE 0 ALWAYS WRITES, so the first sample of a note — and the first after a reset —
        // carries the modulated value rather than a stale one. That holds because the phase is
        // cleared in every place a period can legitimately restart: `gateOn()` (a new note),
        // `refreshLive` (the live list changed), `reset()` and `prepare()`.
        //
        // TWO REVIEWS WERE NEEDED TO GET THAT SENTENCE TRUE. T-CMP-16 shipped it with only
        // `refreshLive` clearing the phase, which left a stale window at every onset; its S5R-2 added
        // `reset()` and described the gap as a voice STEAL. [T-CMP-17 S5R-3] That description named a
        // path that does not exist — `CathodeVoiceEngine::assignVoice` calls `gateOn()` and never
        // `reset()`, and `reset()` is an all-voices panic (stop, mode change, voice-count change).
        // The fix was right, the reason was wrong, and the ACTUAL onset path was still uncovered
        // until `gateOn()` cleared it too.
        enum class GenericRate : std::uint8_t { Audio,
            Control };

        // The spec's RT anchor block (64 samples @ 48 kHz), so a `Control` destination is written
        // once per block's worth of samples whatever block size the host actually uses.
        static constexpr int kGenericControlDecimation = 64;
        // FIREFORK E19: control rate of the named destinations (1 = every sample, today) and, for a voice the idle skip is ticking, of the idle voice
        static constexpr int kCtlRate = NEMESIS_FORK_CTL_RATE;
        static constexpr int kIdleCtlRate = NEMESIS_FORK_IDLE_CTL_RATE;

        // The shared default span for `Octaves`, matching `kCutoffModOctaves` and owner-tunable for
        // the same reason: the analytic keystone pins the MATH, not the musical value. A row states
        // its own only where four octaves is wrong for it.
        static constexpr float kGenericModOctaves = 4.0f;

        // ---- Mod-matrix routing (wrapped so the destination live-set stays in sync) ----
        // Route slot `slot`: src → dest at bipolar depth (clamped by the matrix). Out-of-range slot/src/
        // dest are ignored (matrix guard, mirrored here). Control-rate.
        //
        // [T-CMP-07 leaf 3] Bounded by the matrix's ID SPACE rather than by the named enum, and the
        // wrapper's own guard has to move with the matrix's or a routing accepted downstairs would
        // never reach the live-set bookkeeping up here — a silent half-routing, which is the failure
        // this wrapper exists to prevent. Every id accepted before is accepted now, unchanged.
        void setSlot (int slot, Source src, CathodeModMatrix::DestId dest, float depth) noexcept
        {
            if (slot < 0 || slot >= CathodeModMatrix::kMaxSlots)
                return;
            const int s = static_cast<int> (src);
            const int d = dest;
            // [D-M-0193] the matrix's own bound: the internal source only on a reserved slot
            const int sourceBound = slot >= CathodeModMatrix::kUserSlots ? CathodeModMatrix::kSourceStorage : CathodeModMatrix::kSourceCount;
            if (s < 0 || s >= sourceBound || d < 0 || d >= CathodeModMatrix::kDestCapacity)
                return;
            matrix_.setSlot (slot, src, dest, depth);
            retarget (slot, d);
        }
        // The NAMED form, unchanged for every caller that spells a destination out.
        void setSlot (int slot, Source src, Dest dest, float depth) noexcept
        {
            setSlot (slot, src, static_cast<CathodeModMatrix::DestId> (dest), depth);
        }
        void clearSlot (int slot) noexcept
        {
            if (slot < 0 || slot >= CathodeModMatrix::kMaxSlots)
                return;
            matrix_.clearSlot (slot);
            retarget (slot, -1);
        }
        void clearAllSlots() noexcept
        {
            matrix_.clearAllSlots();
            for (auto& t : slotDest_)
                t = -1;
            liveCount_.fill (0);
            refreshLive();
        }

        // ---- [T-CMP-07 leaf 4a] GENERIC destinations -------------------------------------------
        //
        // A destination with no bespoke law in this class. The eighteen named ones each got hand
        // -written apply and restore arms; the card's goal is that every CONTINUOUS registry row can
        // be a destination, and 69 of them are per-voice reachable — which is 69 arms nobody is going
        // to write. So a generic destination is a WRITER plus the range it clamps to, and the
        // per-sample law is the one every named sibling already follows: `clamp (base + mod * span)`.
        //
        // WHY L5 BINDS AND L1 ONLY STORES. The id space is L2's (`core::modDest*`, derived from the
        // registry); the SETTER is a dsp symbol. Only the app layer sees both, exactly as it does for
        // the eighteen named dests, so it resolves token → id → writer and hands the pair down. L1
        // therefore carries NO table of tokens and NO ordinal shared with another layer — which is
        // the whole of leaf 0's defect, and the one shape this card must not rebuild.
        //
        // Ids below `kDestCount` are REFUSED: those are the named destinations, and a generic writer
        // on one would apply twice per sample, once through each law.
        // [T-CMP-20 leaf F] THE DESTINATION TABLE IS SHARED, NOT PER-VOICE — and it always could
        // have been. Every field below DESCRIBES a destination: which writer, over what range, under
        // which law, at what rate. None of it is voice state, and `GenericWrite` takes the voice as
        // its first parameter precisely so ONE function pointer serves all of them. Yet
        // `CathodeMachine::bindGenericDests` looped `eachVoice` and wrote the identical tuple into
        // sixteen private copies, so each machine carried 15 redundant duplicates of a ~6 KB table —
        // 512 of the 784 bytes every capacity slot cost per instance, and the largest single term in
        // the eager-storage budget T-CMP-20 leaf E had to raise a ruling to widen.
        //
        // The voice now POINTS at one table the engine owns. Every READ site is unchanged —
        // `genericDest_[u]` still indexes — the write door moved to `CathodeVoiceEngine`, and the
        // refusal rule stays here as a static so there is exactly ONE copy of it; the engine and the
        // dsp tests both go through `writeGenericDest`.
        //
        // A NULL TABLE IS LEGAL and means "nothing bound". A bare voice constructed in a test renders
        // before anyone hands it a table, which was true before this change too — the difference is
        // that "unbound" used to be a table of null writers and is now also a null table, so every
        // reader guards the pointer.
        struct GenericDest
        {
            GenericWrite write = nullptr;
            float lo = 0.0f;
            float hi = 1.0f;
            GenericLaw law = GenericLaw::Linear;
            float span = kGenericModOctaves; // octaves, read only under `Octaves`
            GenericRate rate = GenericRate::Audio; // [T-CMP-16 F3] see `GenericRate`
        };

        using GenericDestTable = std::array<GenericDest, static_cast<std::size_t> (CathodeModMatrix::kDestCapacity)>;

        // Ids below `kDestCount` are REFUSED: those are the named destinations, and a generic writer
        // on one would apply twice per sample, once through each law. Returns whether it took, which
        // the old void signature could not report.
        static bool writeGenericDest (GenericDestTable& table,
            CathodeModMatrix::DestId id,
            GenericWrite write,
            float lo,
            float hi,
            GenericLaw law = GenericLaw::Linear,
            float span = kGenericModOctaves,
            GenericRate rate = GenericRate::Audio) noexcept
        {
            if (id < CathodeModMatrix::kDestCount || id >= CathodeModMatrix::kDestCapacity)
                return false;
            auto& g = table[static_cast<std::size_t> (id)];
            g.write = write;
            g.lo = lo;
            g.hi = hi;
            g.law = law;
            g.span = span;
            g.rate = rate;
            return true;
        }

        // Point this voice at the engine-owned table. Re-derives the live set, because that set is
        // per-voice bookkeeping OVER a now-shared description — the table says what a destination
        // is, `genericLive_` says which of them this voice currently has a route to.
        void setGenericDestTable (const GenericDestTable* table) noexcept
        {
            genericDest_ = (table != nullptr) ? table->data() : nullptr;
            refreshLive();
        }

        // The value the apply table last wrote for this destination — the base the modulation offsets
        // from, and the value the restore arm puts back. It WRITES THROUGH immediately, so a bound
        // generic destination behaves exactly like the direct `eachStage` call it replaces while
        // nothing routes to it.
        void setGenericBase (CathodeModMatrix::DestId id, float value) noexcept
        {
            if (id < CathodeModMatrix::kDestCount || id >= CathodeModMatrix::kDestCapacity)
                return;
            const auto u = static_cast<std::size_t> (id);
            genericBase_[u] = value;
            if (genericDest_ != nullptr && genericDest_[u].write != nullptr && !destLive_[u])
                genericDest_[u].write (*this, value);
        }

        // Read-back, for the same reason every other effective* accessor exists: a test cannot
        // otherwise tell "the modulation was applied" from "the base happened to be that value".
        [[nodiscard]] float genericBase (CathodeModMatrix::DestId id) const noexcept
        {
            if (id < 0 || id >= CathodeModMatrix::kDestCapacity)
                return 0.0f;
            return genericBase_[static_cast<std::size_t> (id)];
        }
        // [T-CGUI-46 leaf 2] Changes whenever the live set is re-derived (a route set or cleared, a
        // table bound) — the engine-wide kind's cheap "did routing move" test, one compare a sample.
        [[nodiscard]] std::uint32_t liveEpoch() const noexcept { return liveEpoch_; }
        [[nodiscard]] bool genericDestBound (CathodeModMatrix::DestId id) const noexcept
        {
            if (id < 0 || id >= CathodeModMatrix::kDestCapacity || genericDest_ == nullptr)
                return false;
            return genericDest_[static_cast<std::size_t> (id)].write != nullptr;
        }

        // [T-CORE-14] Read-only view of the routing table, for pinning the core→dsp edge against an
        // independent transcription of §2.6's Init patch. Const — the wrapper owns the live-set
        // bookkeeping, so writes go through setSlot/clearSlot or `destLive` goes stale.
        [[nodiscard]] const CathodeModMatrix& matrix() const noexcept { return matrix_; }

        // ---- Effective (post-modulation) values the hot path applies (metering / test anchors) ----
        [[nodiscard]] float effectiveCutoffHz() const noexcept { return effectiveCutoffHz_; }
        [[nodiscard]] float effectiveLevel() const noexcept { return effectiveLevel_; }
        [[nodiscard]] float effectivePan() const noexcept { return effectivePan_; }
        [[nodiscard]] float effectiveConverterRateHz() const noexcept { return effectiveConverterRateHz_; }
        [[nodiscard]] float effectiveBitDepth() const noexcept { return effectiveBitDepth_; }
        [[nodiscard]] float effectiveLoopError() const noexcept { return effectiveLoopError_; }
        [[nodiscard]] float effectiveDrift() const noexcept { return effectiveDrift_; }
        // [T-CORE-16] PER SLOT, and the shape change is the point rather than a side effect: a
        // single-valued accessor cannot express "one destination offset, two effective positions",
        // which is exactly the property the per-slot base exists to provide. So the read-back
        // surface splits with the setter (and every caller now has to say WHICH slot it means).
        [[nodiscard]] float effectiveOscPosition (int slot) const noexcept { return effectiveOscPos_[slotIdx (slot)]; }
        [[nodiscard]] float effectivePdAmount (int slot) const noexcept { return effectivePdAmount_[slotIdx (slot)]; }
        // [T-MASK-03 leaf F1] The mask the slot is ACTUALLY building its waveform from — read off the
        // OSCILLATOR, not from a copy of what this class last computed (see `baseMask_`: a copy made
        // the fold unfalsifiable). The base is what the row set; this is what is heard.
        // [T-CMP-15 leaf D2] The APPLIED pulse width for one slot, read from the OSCILLATOR and not
        // from a copy this class keeps — the `effectiveMask` rule below, for the same reason: a copy
        // is a second record that can agree with the test while the sound disagrees.
        [[nodiscard]] Real effectivePulseWidth (int slot) const noexcept
        {
            return outputStage_.voice().pulseWidth (slot);
        }

        [[nodiscard]] int effectiveMask (int slot) const noexcept
        {
            return outputStage_.voice().mask (slotIdx (slot) == 0 ? 0 : 1);
        }
        // The slot's mask transient, for the same reason the LFO/step depth read-backs exist.
        [[nodiscard]] float maskEnvValue (int slot) const noexcept { return dd_[slotIdx (slot)].value(); }
        [[nodiscard]] float maskDecayMs (int slot) const noexcept { return dd_[slotIdx (slot)].decayMs(); }
        [[nodiscard]] float effectiveNoteHz() const noexcept { return effectiveNoteHz_; }
        [[nodiscard]] float effectiveResonance() const noexcept { return effectiveQ_; }
        [[nodiscard]] float effectiveVowel() const noexcept { return effectiveVowel_; }
        [[nodiscard]] float effectiveNoiseLevel() const noexcept { return effectiveNoiseLevel_; }
        [[nodiscard]] bool destLive (Dest dest) const noexcept
        {
            return destLive_[static_cast<std::size_t> (dest)];
        }
        // [T-CMP-07 leaf 3] The same question about an id the enum does not name — bounds-checked,
        // because an id from L2 has not been through an enum's type system on the way here.
        [[nodiscard]] bool destLive (CathodeModMatrix::DestId dest) const noexcept
        {
            if (dest < 0 || dest >= CathodeModMatrix::kDestCapacity)
                return false;
            return destLive_[static_cast<std::size_t> (dest)];
        }
        // Total reported through-path latency (delegated; alignment/PDC is a voice/engine concern).
        [[nodiscard]] int latencySamples() const noexcept { return outputStage_.latencySamples(); }

        // [T-CGUI-76 S5R2] Re-evaluates the matrix from every source's CURRENT value, ticking none — the
        // engine's note-on latch reads a voice triggered on this very sample, before it has rendered one,
        // whose matrix otherwise still holds its PREVIOUS note. One matrix sum; RT-safe, zero-alloc.
        void evaluateModulationNow() noexcept
        {
            applyDrawEdges(); // [S5R3] land a flip requested for THIS sample before any source is read
            const auto peek = [] (const BypassFade& f) noexcept { return f.inCircuit() ? 1.0f : f.gain(); };
            feedSources ({ outputStage_.ampEnvValue(), peekModEnv (0), peekModEnv (1), lfo1_.nextValue(), lfo2_.nextValue(), lfo3_.nextValue(), step_.value(), sh_.nextValue(), dd_[0].value(), dd_[1].value(), dd_[2].value(), peek (envFade_), peek (lfoFade_), peek (driftFade_), peek (mtxFade_) });
            matrix_.evaluate();
        }
        // [T-CGUI-79 A4] The same, for a note latched behind a steal-fade: its VELOCITY and KEY stand in
        // for the note this voice still sounds, for this one evaluation only (both restored after).
        void evaluateModulationFor (float velocity01, Wide noteHz) noexcept
        {
            const float velocity = velocity01_;
            const float key = keyTrack_;
            velocity01_ = clampUnit (velocity01);
            keyTrack_ = keyTrackForNote (noteHz);
            evaluateModulationNow();
            velocity01_ = velocity;
            keyTrack_ = key;
        }

        // [D-M-0574] The engine's IDLE SKIP calls this INSTEAD of processSample() for a voice that is not sounding and
        // has held exact silence: the WHOLE modulation half of a sample (`modulateSample` — sources ticked and fed, the
        // matrix evaluated, every live destination written, a routed LFO rate included) and nothing of the audio half.
        // So a skipped voice's free LFOs, S&H and Env2/3 stay in lockstep with a rendering voice's, and the matrix and
        // destination read-backs stay current. What it saves is the punch and the whole output stage.
        void tickIdleSources() noexcept
        {
            if constexpr (kIdleCtlRate != kCtlRate) // FIREFORK E19: a silent voice needs its sources ticking, not its destinations every sample
            {
                idleTick_ = true;
                modulateSample();
                idleTick_ = false;
            }
            else
                modulateSample();
        }

        // Produces one per-voice STEREO sample: tick the sources → feed + evaluate the matrix → apply the
        // live destinations → the output stage. Hot path: the primitives' inline hot paths + the matrix
        // sum; the libm is exp2 for a LIVE cutoff remap, plus log2 (+ exp2 while ramping) for a ROUTED F2
        // cutoff (setF2Cutoff; D-M-0490, 14.4 ns per voice-sample, perf-budgets.md §3h) — scoped deviations.
        // An empty matrix touches no destination → bit-identical to a bare CathodeOutputStage.
        StereoSample processSample() noexcept
        {
            processSampleBegin();
            processSamplePre();
            processSampleMid (outputStage_.voice().f1CoreScalar());
            return processSamplePost (outputStage_.voice().f2CoreScalar());
        }

        // [T-CMP-37 L2] The same sample in two halves around F1's core, for the engine's batched Classic core: PRE is the
        // modulation half and the punch and the tone path up to the core's input; POST takes the core's sample (the scalar's, or
        // a lane-wise one bit-identical to it) and runs the rest. `processSample` is pre → the scalar core → post.
        // [T-CMP-37 L3] BEGIN is the modulation half, the punch and the oscillator core's prologue and PD generators (so the engine
        // can run the PD decimators for four voices at once); PRE the tone path from there up to F1's core input.
        void processSampleBegin() noexcept
        {
            modulateSample();
            renderBegin();
        }
        void processSamplePre() noexcept { outputStage_.processSamplePre (punchPreAmp_); }
        // [T-CMP-37 L3] …and in three phases (F2's core is the second cut): MID takes F1's core sample, POST F2's.
        void processSampleMid (float f1Core) noexcept { outputStage_.processSampleMid (f1Core); }
        StereoSample processSamplePost (float f2Core) noexcept { return outputStage_.processSamplePost (f2Core, punchPostAmp_); }
        // [T-CMP-37 L3] POST in two halves around the output drive (CathodeOutputStage::processSampleAmp): AMP, then TAIL on the drive's sample.
        void processSampleAmp (float f2Core) noexcept { outputStage_.processSampleAmp (f2Core, punchPostAmp_); }
        StereoSample processSampleTail (float driven) noexcept { return outputStage_.processSampleTail (driven); }

    private:
        // Steps 0-4 of a sample: everything the matrix touches. Shared by processSample and the idle tick.
        void modulateSample() noexcept
        {
            applyDrawEdges(); // [S5R2] a requested DRAW flip lands here, on the audio thread
            // FIREFORK E19: the sources below tick every sample; the matrix sum and the named destinations run on tick 0 of each control period
            bool ctlTick = true;
            if constexpr (kCtlRate > 1 || kIdleCtlRate > 1)
            {
                const int period = idleTick_ ? kIdleCtlRate : kCtlRate;
                ctlTick = (ctlPhase_ == 0);
                ctlPhase_ = (ctlPhase_ + 1 >= period) ? 0 : ctlPhase_ + 1;
            }
            // 1. Tick the owned dynamic sources (read-then-advance).
            // [T-CGUI-61 leaf E6] A DRAW-mode envelope is the source INSTEAD of its ADSR; [row 304] the ADSR
            // still ticks beside it, so a flip Off lands on the ADSR the note would be on.
            const float e2 = tickModEnv (0);
            // FIREFORK E21b: a source no active slot reads does not advance (the panel routes LFO1, Env2, key tracking, wheel and aftertouch only)
            const float e3 = (fork::kPanelSources && !matrix_.reads (Source::Env3)) ? 0.0f : tickModEnv (1);
            // FIREFORK E8: an LFO whose value no active slot reads (or nothing reads: the idle skip) only advances.
            const bool fed = !(fork::Config::idleMatrixSkip && matrix_.idle());
            const auto tickLfo = [fed, ctlTick, this] (Lfo& lfo, Source src) noexcept {
                if (fed && ctlTick && matrix_.reads (src))
                    return lfo.processSample();
                lfo.tick();
                return 0.0f;
            };
            const float l1 = tickLfo (lfo1_, Source::Lfo1);
            const float l2 = (fork::kPanelSources && !matrix_.reads (Source::Lfo2)) ? 0.0f : tickLfo (lfo2_, Source::Lfo2);
            const float l3 = (fork::kPanelSources && !matrix_.reads (Source::Lfo3)) ? 0.0f : tickLfo (lfo3_, Source::Lfo3);
            const float st = (fork::kPanelSources && !matrix_.reads (Source::StepMod)) ? 0.0f : step_.processSample();
            const float sh = (fork::kPanelSources && !matrix_.reads (Source::SampleHold)) ? 0.0f : tickLfo (sh_, Source::SampleHold);
            // Env1 = the AMP env value the output stage is about to apply this sample (no lag) — the
            // ADSR's `value()`, or [T-CGUI-61 leaf E6] the drawn AMP envelope's cached tick.
            const float e1 = outputStage_.ampEnvValue();

            // 2. Feed the matrix source values.
            // [T-CGUI-24] Every source is fed below; none is left at 0 (ModWheel since T-CMP-21 leaf C,
            // ConvDriftReadout since T-CGUI-24).
            // Env2/Env3 carry their §2.4 GLOBAL SOURCE GAIN here (`synth.env{2,3}.amt`, Param
            // Ref:180 — see setEnv2Amount): one multiply, so every route from that envelope is
            // scaled and the matrix stays a pure router. Env1 has no such row — it is the AMP
            // envelope, whose level is the voice's own output, not a mod-source amount.
            //
            // [T-CGUI-08] The four MODULATION stage bypasses (Param Ref §2.13, owner 2026-09-23) gate
            // their sources HERE, at the one door every route and hard-wire reads them through. ENV
            // takes Env2/Env3 — and with them the Env-amt hard-wire, which reads Env2 — but never
            // Env1, the AMP envelope. LFO takes all five strip instances. Each source keeps ticking,
            // so an un-bypassed LFO comes back in phase. At gain 1 each multiply is exact.
            const float envGain = fadeGain (envFade_);
            const float lfoGain = fadeGain (lfoFade_);
            const float driftGain = fadeGain (driftFade_);
            const float mtxGain = fadeGain (mtxFade_);
            const float dd1 = (fork::kPanelSources && !matrix_.reads (Source::DeltaDecay1)) ? 0.0f : dd_[0].processSample();
            const float dd2 = (fork::kPanelSources && !matrix_.reads (Source::DeltaDecay2)) ? 0.0f : dd_[1].processSample();
            const float dd3 = (fork::kPanelSources && !matrix_.reads (static_cast<Source> (CathodeModMatrix::kSubMaskEnvSource))) ? 0.0f
                                                                                                                            : dd_[2].processSample(); // [D-M-0193] the SUB's
            // FIREFORK: with no active slot the matrix has nothing to sum (see CathodeModMatrix::idle). The sources above and
            // the fades keep ticking, so nothing downstream can tell.
            if (ctlTick && !(fork::Config::idleMatrixSkip && matrix_.idle()))
            {
                feedSources ({ e1, e2, e3, l1, l2, l3, st, sh, dd1, dd2, dd3, envGain, lfoGain, driftGain, mtxGain });

                // 3. Sum every destination's modulation.
                matrix_.evaluate();
            }

            // 4. Apply the live wired destinations (un-live ones are never touched → no-op bit-exact).
            if (ctlTick) // FIREFORK E19 (closed before the generic destinations)
            {
            // §1 oscillator dests (M1-17g): Pitch (exp2 semitone remap, fans to osc + key-track + loop-err
            // via the voice's setNoteHz), OscPos/PdAmount ([0,1] Amp-style, both slots).
            if (destLive_[static_cast<std::size_t> (Dest::Pitch)])
            {
                // [T-CMP-21 leaf C] `bentNoteHz()` rather than `baseNoteHz_`: the channel bend and a
                // Pitch route COMPOSE — bending a vibrato'd note bends the vibrato with it, which is
                // what a player expects and what a wheel that only moved the un-modulated base would
                // not do. One multiply on the hot path (the ratio is cached at control rate), so the
                // libm count is unchanged.
                effectiveNoteHz_ = pitchForMod (static_cast<float> (bentNoteHz()), matrix_.modulation (Dest::Pitch));
                outputStage_.voice().setNoteHz (static_cast<Real> (effectiveNoteHz_));
            }
            // [T-CORE-16] ONE destination offset, applied to each slot's OWN base — and the clamp
            // is inside the loop, so one slot can saturate while the other stays interior. That
            // asymmetry is not decoration: it is the only observable difference between this and a
            // both-slots base whose single clamped value is written twice.
            if (destLive_[static_cast<std::size_t> (Dest::OscPos)])
            {
                const auto mod = matrix_.modulation (Dest::OscPos);
                for (std::size_t s = 0; s < kNumOscSlots; ++s)
                {
                    effectiveOscPos_[s] = clamp01 (baseOscPos_[s] + mod);
                    outputStage_.voice().setPosition (static_cast<int> (s), effectiveOscPos_[s]);
                }
            }
            if (destLive_[static_cast<std::size_t> (Dest::PdAmount)])
            {
                const auto mod = matrix_.modulation (Dest::PdAmount);
                for (std::size_t s = 0; s < kNumOscSlots; ++s)
                {
                    effectivePdAmount_[s] = clamp01 (basePdAmount_[s] + mod);
                    outputStage_.voice().setPdAmount (static_cast<int> (s), effectivePdAmount_[s]);
                }
            }
            // [T-MASK-03 leaf F1] THE TWO MASK DESTS, per slot and independent. The modulation is a
            // unit-interval offset like every other dest's, scaled here by the mask SPACE (512) so a
            // depth of 100 % sweeps the whole range — the conversion belongs at the destination
            // because only the destination knows what its units are (the `pitchForMod` /
            // `cutoffForMod` precedent).
            //
            // AND IT FOLDS RATHER THAN CLAMPING, which is the one place this dest differs from every
            // other one in this loop. Its siblings `clamp01`, because a position or a warp depth past
            // its rail is not a value. A mask is a 9-bit NUMBER whose top wraps to its bottom by
            // construction (leaf B's `mask & 511`), so an overshooting transient continues through
            // the space instead of sticking — the behaviour leaf E measured on the push path and
            // stated as ruled. The fold happens at the oscillator; passing the un-folded int keeps ONE
            // home for that law.
            for (std::size_t s = 0; s < kNumOscSlots; ++s)
            {
                if (!destLive_[maskDestIndex (s)])
                    continue;
                const auto dest = s == 0 ? Dest::Osc1Mask : Dest::Osc2Mask;
                const Real mod = matrix_.modulation (dest) * static_cast<Real> (BitmaskOscillator::kMaskSpace);
                outputStage_.voice().setMask (static_cast<int> (s), baseMask_[s] + static_cast<int> (mod));
            }
            // [T-CMP-15 leaf D2] The pulse WIDTH, and it CLAMPS where the mask above FOLDS. That is
            // not an inconsistency, it is the difference between the two spaces: a 9-bit mask is
            // cyclic, so an overshooting transient continues through it, whereas duty 0 and duty 1
            // are both SILENCE — a folding width would chop where the player asked it to breathe.
            // Param Ref states the rails; `PulseOscillator::setDuty` owns them, so passing the
            // un-clamped value keeps ONE home for that law, exactly as the mask does for its fold.
            for (std::size_t s = 0; s < kNumOscSlots; ++s)
            {
                if (!destLive_[pwDestIndex (s)])
                    continue;
                const auto dest = s == 0 ? Dest::Osc1Pw : Dest::Osc2Pw;
                outputStage_.voice().setPulseWidth (static_cast<int> (s), basePulseWidth_[s] + matrix_.modulation (dest));
            }
            // §3 filter cutoff + resonance share one voice().setCutoff — compute each effective (its remap
            // when live, else the held base) and issue ONE combined call if either is live. With only
            // FiltCutoff live, effectiveQ_ == baseQ_ → setCutoff(effCutoff, baseQ) → bit-exact vs M1-17d.
            {
                const bool cutoffLive = destLive_[static_cast<std::size_t> (Dest::FiltCutoff)];
                const bool resoLive = destLive_[static_cast<std::size_t> (Dest::FiltReso)];
                if (cutoffLive || resoLive)
                {
                    // [T-CGUI-83] Through the step limiter: a smooth sum passes as the same float.
                    if (cutoffLive)
                    {
                        const float cutMod = cutoffSlew_.process (matrix_.modulation (Dest::FiltCutoff));
                        NEMESIS_FORK_COUNT (cutCalls);
                        if constexpr (fork::kExact) // FIREFORK E17: the same operands give the same Hz, so skip the exp2
                        {
                            if (!(cutValid_ && cutMod == cutModLast_ && baseCutoffHz_ == cutBaseLast_ && organicCutoffMult_ == cutOrgLast_))
                            {
                                cutValid_ = true;
                                cutModLast_ = cutMod;
                                cutBaseLast_ = baseCutoffHz_;
                                cutOrgLast_ = organicCutoffMult_;
                                effectiveCutoffHz_ = cutoffForMod (baseCutoffHz_, cutMod) * organicCutoffMult_;
                            }
                            else
                                NEMESIS_FORK_COUNT (expSkips);
                        }
                        else
                            effectiveCutoffHz_ = cutoffForMod (baseCutoffHz_, cutMod) * organicCutoffMult_;
                    }
                    if (resoLive)
                        effectiveQ_ = resoForMod (baseQ_, matrix_.modulation (Dest::FiltReso));
                    pushFilterCutoffReso (effectiveCutoffHz_, effectiveQ_);
                    // [T-CGUI-60] F2 follows F1's cutoff modulation, from its own base (see setF2Cutoff).
                    if (cutoffLive && outputStage_.voice().filter2On())
                        pushF2();
                }
            }
            if (destLive_[static_cast<std::size_t> (Dest::FiltVowel)])
            {
                effectiveVowel_ = clamp01 (baseVowel_ + matrix_.modulation (Dest::FiltVowel));
                outputStage_.voice().setVowel (effectiveVowel_);
            }
            if (destLive_[static_cast<std::size_t> (Dest::Amp)])
            {
                // T-CORE-09: the level domain is the output stage's §22.8 [0, kMaxLevel], so the
                // modulated result takes clampLevel — NOT the clamp01 its six sibling destinations
                // share (OscPos / PdAmount / FiltVowel / ConvLoopErr / ConvDrift / NoiseLevel are
                // all genuinely 0..1, and widening the shared helper would silently give them a
                // 4× ceiling). This was the THIRD clamp on the level path.
                //
                // [T-CORE-14 leaf A2] Amp modulation SCALES the base; it does not add to it. Every
                // other destination here is an offset, and Amp read the same way until the Init
                // patch was actually routed — at which point `Vel→Amp +40` (a seeded §2.6 slot,
                // velocity being unipolar) put a permanent +0.635 floor under the level and
                // `synth.level` = 0 rendered at 0.635 gain. Param Ref:272 states that row as
                // "−∞ → 0 dB at 100 → +12 dB", so level 0 is SILENCE and an additive Amp dest
                // contradicts it. The special-case fix (force 0 when the base is 0) would be a
                // constant hiding a defect — T-CORE-11's rule — so the law changes instead: a VCA
                // scales, which is also what makes `Vel→Amp` mean velocity SENSITIVITY rather than
                // a velocity-sized DC offset. `1 + mod` floors at 0 because mod is bipolar and a
                // negative scale would invert the waveform rather than quieten it.
                const auto ampScale = 1.0f + matrix_.modulation (Dest::Amp);
                effectiveLevel_ = clampLevel (baseLevel_ * (ampScale < 0.0f ? 0.0f : ampScale));
                outputStage_.setLevel (effectiveLevel_);
            }
            if (destLive_[static_cast<std::size_t> (Dest::Pan)])
            {
                effectivePan_ = clampUnit (basePan_ + matrix_.modulation (Dest::Pan));
                outputStage_.setPan (effectivePan_);
            }
            // §2 converter "alive grit" batch (M1-17f). WIDE-range params (rate/bits) take the scaled
            // remaps (rate = exp2 octave — the scoped per-sample-exp2 deviation, blessed for cutoff;
            // bits = scaled linear, but BrokenConverter::setBitDepth itself does an exp2 → the same
            // deviation while ConvBits is live). LoopErr/Drift take the raw Amp-style clamp01(base + mod).
            if (destLive_[static_cast<std::size_t> (Dest::ConvRate)])
            {
                effectiveConverterRateHz_ = rateForMod (baseConverterRateHz_, matrix_.modulation (Dest::ConvRate));
                outputStage_.voice().setConverterRate (effectiveConverterRateHz_);
            }
            if (destLive_[static_cast<std::size_t> (Dest::ConvBits)])
            {
                effectiveBitDepth_ = bitsForMod (baseBitDepth_, matrix_.modulation (Dest::ConvBits));
                outputStage_.voice().setBitDepth (effectiveBitDepth_);
            }
            // [T-CMP-04 leaf 2] Sync: a [0,1] amount, so the Amp-style `clamp01(base + mod)` the
            // loop-error row below uses — not an octave remap. The amount is the OSC 2-master
            // sync's free/synced CROSSFADE weight (D-M-0208), so it saturates at full sync.
            if (destLive_[static_cast<std::size_t> (Dest::Sync)])
            {
                effectiveSyncAmount_ = clamp01 (baseSyncAmount_ + matrix_.modulation (Dest::Sync));
                outputStage_.voice().setSyncAmount (effectiveSyncAmount_);
            }
            if (destLive_[static_cast<std::size_t> (Dest::ConvLoopErr)])
            {
                effectiveLoopError_ = clamp01 (baseLoopError_ + matrix_.modulation (Dest::ConvLoopErr));
                outputStage_.voice().setLoopError (effectiveLoopError_);
            }
            if (destLive_[static_cast<std::size_t> (Dest::ConvDrift)])
            {
                effectiveDrift_ = clamp01 (baseDrift_ + matrix_.modulation (Dest::ConvDrift));
                outputStage_.voice().setDrift (effectiveDrift_);
            }
            // §5 texture layer (M1-18c). NoiseLevel is a [0,1] param → the raw Amp-style clamp01(base +
            // mod); the forwarded setNoiseLevel recomputes the stage's textureLive_ gate (a float compare
            // — pure arith, no libm; a modulated level of exactly 0 re-enters the bit-exact texture bypass).
            if (destLive_[static_cast<std::size_t> (Dest::NoiseLevel)])
            {
                effectiveNoiseLevel_ = clamp01 (baseNoiseLevel_ + matrix_.modulation (Dest::NoiseLevel));
                outputStage_.voice().setNoiseLevel (effectiveNoiseLevel_);
            }

            } // FIREFORK E19
            // [T-CMP-07 leaf 4a] The GENERIC destinations, and the loop is over the LIVE ones only —
            // never over the id space. With nothing routed generically `genericLiveCount_` is 0 and
            // this costs one compare and writes nothing, which is what keeps the goldens bit-exact
            // (the same "un-live destinations are never touched" property the named block above has).
            //
            // ONE LAW, and it is the one the named siblings already follow: offset the base by the
            // modulation scaled into the destination's OWN span, then clamp to that span. The span is
            // the setter's range rather than the registry's control range because the base arriving
            // here has already been through the row's curve — `setGenericBase` stores what the apply
            // row wrote, in the setter's units, so mixing in a control-domain span would add two
            // different quantities.
            // [T-CMP-16 F3] The control-rate phase, sampled BEFORE the advance so phase 0 writes and
            // the period is exactly `kGenericControlDecimation`.
            //
            // [S5R-4] AND THE UPDATE INSTANTS DO SHIFT WHEN A ROUTE IS ADDED OR REMOVED — an earlier
            // comment here claimed the opposite. `refreshLive` resets the phase to 0, deliberately,
            // so a new route takes effect on its very first sample rather than up to 63 late. The
            // `genericLiveCount_ > 0` guard below is therefore belt-and-braces rather than load-
            // bearing: the phase can only be non-zero while something is live already.
            const bool controlTick = (genericControlPhase_ == 0);
            if (genericLiveCount_ > 0)
                genericControlPhase_ = (genericControlPhase_ + 1) % kGenericControlDecimation;

            genericIterating_ = true; // [S5R B31 dsp2 F4] see `refreshLive`
            for (int i = 0; i < genericLiveCount_; ++i)
            {
                const auto id = genericLive_[static_cast<std::size_t> (i)];
                const auto& g = genericDest_[static_cast<std::size_t> (id)];
                if (g.rate == GenericRate::Control && !controlTick)
                    continue;
                const float base = genericBase_[static_cast<std::size_t> (id)];
                const float mod = matrix_.modulation (id);
                // [T-CMP-07 leaf 4c] The three laws. `Octaves` is `cutoffForMod`'s remap, and the
                // `exp2` is the same scoped libm deviation this class already documents for the live
                // cutoff remap — it runs only for a ROUTED exponential destination, never for an
                // idle one, because the loop walks the live list.
                //
                // [T-CMP-16 F3] THAT WAS TRUE OF THE LAW AND FALSE OF THE PATH, AND THE PATH IS WHAT
                // COSTS. `g.write` is the destination's OWN setter, every one of them documented
                // "Control-rate" at its own declaration, and several call libm on the way:
                //   synth.reson.{tune,decay,struct,color} -> 6 x ModalResonator::recomputeMode
                //                                         -> 6 x (pow + cos + sin) = EIGHTEEN
                //   the 15 envelope TIMES -> AdsrEnvelope::update{Attack,Decay,Release} -> 1 x pow (T-CGUI-81)
                //   synth.osc{1,2}.maskDecay -> DeltaDecayEnvelope::setDecayMs -> pow
                //   synth.hpf                -> TptSvf::setCutoff             -> tan
                //   punch.tune               -> an explicit exp2, on a LINEAR row
                // So the `Octaves` arm is not the deviation to watch; the WRITE is.
                //
                // MEASURED, 2026-08-31, `bench: generic modulation destinations` — 4 SOUNDING voices
                // (D-M-0497: the fixture's voices 4 + unison 4 was labelled "16"), 64 samples, mean
                // of 100 samples, against a 1333.3 us block budget (16 voices: not measured):
                //   nothing routed              13.50 us   (the floor)
                //   4 x cheap linear, no libm   17.80 us   +32 %   ~1.0 ns per write
                //   4 x envelope time, 2 x pow  29.70 us   +120 %  ~4.0 ns per write
                //   4 x resonator, 18 calls    153.30 us   +1022 % ~34 ns per write
                // The per-write costs track the transcendental count (34 / 4.0 ~ 8.5, 18 / 2 = 9),
                // which is what rules out "the resonator switched on" as the explanation.
                //
                // THE OBVIOUS MITIGATION IS THE WRONG ONE: skipping the write when `v` repeats saves
                // nothing under the case that costs, because a moving LFO changes `v` every sample,
                // and a static route was already cheap. Only reducing the RATE helps, which is what
                // `GenericRate::Control` above does. Owner ruling 2026-08-31; re-measured after:
                //   4 x resonator, control-rate  17.70 us   +31 %   (from +1022 %) — what a
                //                                                    plain float store costs
                // [T-CMP-17, owner ruling 2026-08-31] THE FIFTEEN ENVELOPE TIMES JOINED THEM, and
                // they were the worst MEASURED case: 29.70 us -> 15.40 us, +120 % -> +15 %. They are
                // now the cheapest routed arm, below even the plain-float-store rows, because
                // decimation saves the loop's write as well as the setter's `pow` calls (two when
                // measured; one per setter since T-CGUI-81, figures above not re-taken).
                //
                // [S5R-2] WHAT IS STILL AUDIO-RATE IS MORE THAN THIS CARD FIRST CLAIMED, and the
                // claim "no expensive class is left on the generic path" was WRONG — it is retracted
                // here rather than quietly dropped. Unmeasured, and T-CMP-18 carries them:
                //   `synth.osc{1,2}.{oct,semi,fine}` — SIX rows, a bigger class than the resonator's
                //      four. Each reaches `CathodeOscCore::updateTuning` -> `exp2` -> `updateFrequency`,
                //      which re-pushes the frequency into FIVE sub-generators per write.
                //   `punch.decay` — `exp` + `log`, the same count as an envelope time, and it was not
                //      on the list at all.
                //   `punch.tune` — THREE calls, not the one claimed: the row's own `exp2` plus
                //      `recomputeClickCoef`'s `pow` and `exp`.
                //   `synth.osc{1,2}.maskDecay` (`pow`) · `synth.hpf` (`tan`) — one call each.
                float v = base + mod * (g.hi - g.lo); // GenericLaw::Linear
                if (g.law == GenericLaw::Octaves)
                    v = static_cast<float> (static_cast<Real> (base)
                                            * fm::exp2 (static_cast<Real> (mod) * static_cast<Real> (g.span)));
                else if (g.law == GenericLaw::Proportional)
                    v = base * (1.0f + mod * g.span);
                genericRouteWrite_ = true; // [D-M-0158] tells a writer this is the route, not the base
                g.write (*this, v < g.lo ? g.lo : (v > g.hi ? g.hi : v));
                genericRouteWrite_ = false;
            }
            genericIterating_ = false;
            if (liveRefreshOwed_)
            {
                liveRefreshOwed_ = false;
                refreshLive();
            }
        }

        // Steps 5-6: the punch and the voice itself — the audio half the idle skip saves. [T-CMP-37 L2] Cut at F1's core:
        // `renderBegin` is the punch and the stage's BEGIN half (the post-amp injection waits in `punchPostAmp_`, the pre-filter one
        // in `punchPreAmp_` for PRE).
        void renderBegin() noexcept
        {
            // 5. [T-CORE-30 leaf D] The §2 onset transient, evaluated ONCE per sample and routed to
            // one of the two §2 summing points. `active()` is false whenever no burst is decaying —
            // which includes every voice whose `punch.amount` is 0, because trigger() refuses to
            // schedule at 0 amount — so an untouched punch costs one bool test and BOTH injections
            // stay literal zero, which the two stages' guards turn into a bit-exact no-op.
            //
            // The voice sample handed to processSample is the PREVIOUS mix-point sample, read back
            // from the output stage. Only the ringmod_zing flavour reads it (§4.5); see
            // `CathodeOutputStage::mixPointSample` for why a one-sample reference is the right
            // answer here rather than a callback threaded through two stages.
            //
            // BATTERY SURVIVOR, CLASSIFIED (c) — unprovable by construction, and PREDICTED before the
            // run rather than discovered by it. Relaxing this to `if (true)` cannot change any output:
            // `PunchTransient::processSample` early-returns exactly 0.0f while inactive, and both
            // injection sites below receive that literal zero. So the check is a COST guard — it skips
            // a call, a switch and an envelope multiply per sample per voice — and NOT the mechanism
            // that makes the OFF state bit-exact (that is the exact-zero return). Same category as
            // T-UI-56's already-shown `showOverlay` early-out: real work avoided, no observable
            // behaviour to assert. Kept and labelled; the honest test of the OFF state is the
            // bit-exactness case, which passes with or without this line.
            float punchOut = 0.0f;
            if (punch_.active())
                punchOut = punch_.processSample (outputStage_.mixPointSample());
            const bool punchPre = punchMix_ == PunchMix::PreFilter;

            // 6. Render the voice.
            punchPostAmp_ = punchPre ? 0.0f : punchOut;
            punchPreAmp_ = punchPre ? punchOut : 0.0f;
            outputStage_.processSampleBegin();
        }

    private:
        // [T-CMP-21 leaf C] THE ONE PLACE THE TONE PATH'S NOTE IS WRITTEN, so the channel bend
        // cannot be forgotten at a call site. Three callers: a new note (`setNoteHz`), a wheel move
        // (`setPitchBend`) and the frame a Pitch routing is cleared (`justCleared`). The live-Pitch
        // per-sample branch does NOT come through here — it composes `bentNoteHz()` with the
        // matrix's modulation inline, because that path must not re-read `destLive_`.
        //
        // At rest `bendRatio_` is exactly 1.0, so `baseNoteHz_ * bendRatio_` is bit-identical to
        // `baseNoteHz_` and every golden holds until a wheel is touched.
        void applyPitch() noexcept
        {
            const Wide hz = bentNoteHz();
            outputStage_.voice().setNoteHz (hz);
            if (!destLive_[static_cast<std::size_t> (Dest::Pitch)])
                effectiveNoteHz_ = static_cast<float> (hz);
        }

        static float clamp01 (float v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
        // The Amp destination's own ceiling (§22.8 make-up region, T-CORE-09) — deferred to the
        // output stage so every level clamp in the voice path reads one constant.
        static float clampLevel (float v) noexcept
        {
            return v < 0.0f ? 0.0f
                            : (v > CathodeOutputStage::kMaxLevel ? CathodeOutputStage::kMaxLevel : v);
        }
        static float clampUnit (float v) noexcept { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

        // [T-CORE-16] Osc-slot index guard for the two per-slot bases. CLAMPS to the nearest slot,
        // which is `CathodeOscCore::idx`'s convention deliberately mirrored rather than re-decided:
        // the bases forward straight to that class, so a wrapper that IGNORED an out-of-range slot
        // while the tone path clamped it would make the base and the oscillator disagree.
        static std::size_t slotIdx (int slot) noexcept
        {
            if (slot < 0)
                return 0;
            return static_cast<std::size_t> (slot) >= kNumOscSlots ? kNumOscSlots - 1
                                                                   : static_cast<std::size_t> (slot);
        }

        // Update the live-count when slot `slot` changes its target to `newDest` (−1 = cleared). Restores
        // the base of any wired destination that just went un-live so the patch value is re-applied.
        void retarget (int slot, int newDest) noexcept
        {
            const auto s = static_cast<std::size_t> (slot);
            const int old = slotDest_[s];
            // [S5R B31 dsp2 F4] Same target: the live set cannot have moved (it is a function of the counts), so
            // a depth-only edit re-derives nothing — no epoch bump, no control-phase reset. A routed hard-wire
            // DEPTH (`synth.sub.maskEnv`, `synth.osc{1,2}.{pwm,maskEnv}`) re-installs its slot every sample.
            if (old == newDest)
                return;
            if (old >= 0)
                --liveCount_[static_cast<std::size_t> (old)];
            if (newDest >= 0)
                ++liveCount_[static_cast<std::size_t> (newDest)];
            slotDest_[s] = newDest;
            refreshLive();
        }

        // Recompute the wired destinations' live flags from the counts, then restore the base of any wired
        // destination that JUST went live→un-live (its last slot was cleared) so the patch value returns.
        // The restore is TRANSITION-gated (not "restore every un-live dest"): a dest whose parameter the
        // caller configured DIRECTLY on the tone path (not via setBase*) and never routed must NOT be
        // clobbered to this wrapper's default base — only a genuine clear of a previously-live dest restores.
        void refreshLive() noexcept
        {
            // [S5R B31 dsp2 F4] Never under the generic loop's feet: a writer there (a hard-wire depth crossing 0)
            // would rebuild `genericLive_` mid-iteration and skip or repeat a destination. Deferred to the
            // loop's end, the same sample.
            if (genericIterating_)
            {
                liveRefreshOwed_ = true;
                return;
            }
            ++liveEpoch_; // [T-CGUI-46 leaf 2] the engine-wide kind re-reads the live set on a change
            const auto wasLive = destLive_;
            for (int d = 0; d < CathodeModMatrix::kDestCapacity; ++d)
            {
                const auto u = static_cast<std::size_t> (d);
                destLive_[u] = liveCount_[u] > 0;
            }
            const auto justCleared = [&] (Dest dd) noexcept {
                const auto u = static_cast<std::size_t> (dd);
                return wasLive[u] && !destLive_[u];
            };

            // [T-CMP-07 leaf 4a] The GENERIC half, and both arms of it live here because both are
            // the same bookkeeping the named dests get twenty lines below — a live-set to walk, and
            // a base to put back when the last route to a destination goes away.
            //
            // FINDING 1 (iii) IS THE RESTORE, and the card names it as the surface where a generic
            // destination fails first: miss it and a dest un-routed mid-note keeps its last modulated
            // value forever, silently. It is TRANSITION-gated exactly like the named arms — restoring
            // every un-live generic dest every time would clobber a parameter the apply table set
            // directly and nothing ever routed.
            genericLiveCount_ = 0;
            genericControlPhase_ = 0; // [T-CMP-16 F3] phase 0 writes, so a rebuilt live list is current
            cutValid_ = false; ctlPhase_ = 0; // FIREFORK E17/E19
            // [T-CMP-20 leaf F] No table bound yet — nothing generic can be live, so the loop is
            // skipped. It is a GUARD AROUND THE LOOP and must never become an early `return`: the
            // NAMED destinations' restore arms are below, and an unbound voice still owes them.
            // The first cut of this leaf used `return` and the suite caught it — `effectiveMask (0)`
            // held 228 where the test wanted its base of 100, because clearing a mask route stopped
            // restoring the base for every bare voice in `dsp/tests`.
            if (genericDest_ != nullptr)
            {
                for (int d = CathodeModMatrix::kDestCount; d < CathodeModMatrix::kDestCapacity; ++d)
                {
                    const auto u = static_cast<std::size_t> (d);
                    if (genericDest_[u].write == nullptr)
                        continue;
                    if (destLive_[u])
                    {
                        // Bounded by kMaxSlots: at most that many destinations can be targeted at
                        // once, so the list cannot overflow however wide the id space grows.
                        if (genericLiveCount_ < CathodeModMatrix::kMaxSlots)
                            genericLive_[static_cast<std::size_t> (genericLiveCount_++)] = d;
                    }
                    else if (wasLive[u])
                    {
                        genericDest_[u].write (*this, genericBase_[u]);
                    }
                }
            }
            // §1 osc dests (M1-17g).
            if (justCleared (Dest::Pitch))
            {
                applyPitch(); // [T-CMP-21 leaf C] …to the BENT base, not to concert pitch
            }
            // [T-CORE-16] Each slot restores to its OWN base. Restoring both from one value here
            // would reproduce the whole defect at the moment a routing is cleared — the least
            // visible half of this card, because it is three lines from the apply and looks like
            // the same edit.
            if (justCleared (Dest::OscPos))
            {
                effectiveOscPos_ = baseOscPos_;
                for (std::size_t s = 0; s < kNumOscSlots; ++s)
                    outputStage_.voice().setPosition (static_cast<int> (s), baseOscPos_[s]);
            }
            if (justCleared (Dest::PdAmount))
            {
                effectivePdAmount_ = basePdAmount_;
                for (std::size_t s = 0; s < kNumOscSlots; ++s)
                    outputStage_.voice().setPdAmount (static_cast<int> (s), basePdAmount_[s]);
            }
            // [T-MASK-03 leaf F1] Each mask restores to its OWN base, and only its own dest's clearing
            // restores it — the two dests are separate, so clearing osc1's must leave osc2's live
            // modulation alone.
            for (std::size_t s = 0; s < kNumOscSlots; ++s)
            {
                const auto dest = s == 0 ? Dest::Osc1Mask : Dest::Osc2Mask;
                if (justCleared (dest))
                    outputStage_.voice().setMask (static_cast<int> (s), baseMask_[s]);
            }
            // [T-CMP-15 leaf D2] Each width restores to its OWN base, and only its own dest's
            // clearing restores it — the two dests are separate, so clearing osc1's must leave
            // osc2's live modulation alone.
            for (std::size_t s = 0; s < kNumOscSlots; ++s)
            {
                const auto dest = s == 0 ? Dest::Osc1Pw : Dest::Osc2Pw;
                if (justCleared (dest))
                    outputStage_.voice().setPulseWidth (static_cast<int> (s), basePulseWidth_[s]);
            }
            // §3 filter cutoff + resonance share setCutoff: restore each just-cleared one to its base, then
            // push one combined call if EITHER just cleared (the other keeps its last effective).
            if (justCleared (Dest::FiltCutoff) || justCleared (Dest::FiltReso))
            {
                if (justCleared (Dest::FiltCutoff))
                {
                    effectiveCutoffHz_ = baseCutoffHz_ * organicCutoffMult_;
                    cutoffSlew_.reset(); // [T-CGUI-83] a route re-added later starts on its own value
                }
                if (justCleared (Dest::FiltReso))
                    effectiveQ_ = baseQ_;
                pushFilterCutoffReso (effectiveCutoffHz_, effectiveQ_);
                if (justCleared (Dest::FiltCutoff))
                    pushF2(); // [T-CGUI-60] F2 drops F1's cleared modulation too
            }
            if (justCleared (Dest::FiltVowel))
            {
                effectiveVowel_ = baseVowel_;
                outputStage_.voice().setVowel (baseVowel_);
            }
            if (justCleared (Dest::Amp))
            {
                effectiveLevel_ = baseLevel_;
                outputStage_.setLevel (baseLevel_);
            }
            if (justCleared (Dest::Pan))
            {
                effectivePan_ = basePan_;
                outputStage_.setPan (basePan_);
            }
            // §2 converter batch (M1-17f).
            if (justCleared (Dest::ConvRate))
            {
                effectiveConverterRateHz_ = baseConverterRateHz_;
                outputStage_.voice().setConverterRate (baseConverterRateHz_);
            }
            if (justCleared (Dest::ConvBits))
            {
                effectiveBitDepth_ = baseBitDepth_;
                outputStage_.voice().setBitDepth (baseBitDepth_);
            }
            if (justCleared (Dest::ConvLoopErr))
            {
                effectiveLoopError_ = baseLoopError_;
                outputStage_.voice().setLoopError (baseLoopError_);
            }
            if (justCleared (Dest::ConvDrift))
            {
                effectiveDrift_ = baseDrift_;
                outputStage_.voice().setDrift (baseDrift_);
            }
            // §5 texture layer (M1-18c).
            if (justCleared (Dest::NoiseLevel))
            {
                effectiveNoiseLevel_ = baseNoiseLevel_;
                outputStage_.voice().setNoiseLevel (baseNoiseLevel_);
            }
        }

        CathodeOutputStage outputStage_; // owns Env1 (AMP), the tone path, output drive + pan
        float punchPostAmp_ = 0.0f; // [T-CMP-37 L2] the punch's post-amp injection, PRE → POST of one sample
        float punchPreAmp_ = 0.0f; // [T-CMP-37 L3] …and the pre-filter one, BEGIN → PRE
        AdsrEnvelope env2_;
        AdsrEnvelope env3_;
        // [T-CGUI-61 leaf E6] Env2/Env3's DRAW-mode twins ([0] = ENV 2, [1] = ENV 3) and their switches.
        // [row 304] ENV 2 / ENV 3's source value, read before the sample (the matrix peek) and ticked in
        // it. Through a DRAW crossfade, sample k of N weighs the NEW source k/N: the first sample has
        // left the old one, the last lands on the new one exactly. The ADSR ticks in DRAW mode too.
        [[nodiscard]] AdsrEnvelope& modAdsr (std::size_t i) noexcept { return i == 0 ? env2_ : env3_; }
        [[nodiscard]] float modEnvBlend (std::size_t i, float adsr, float drawn) const noexcept
        {
            const float w = static_cast<float> (envXfLen_ - envXfLeft_[i] + 1) / static_cast<float> (envXfLen_);
            return envDrawOn_[i] ? adsr + w * (drawn - adsr) : drawn + w * (adsr - drawn);
        }
        [[nodiscard]] float peekModEnv (std::size_t i) noexcept
        {
            if (envXfLeft_[i] > 0)
                return modEnvBlend (i, modAdsr (i).value(), envDraw_[i].peek());
            return envDrawOn_[i] ? envDraw_[i].peek() : modAdsr (i).value();
        }
        float tickModEnv (std::size_t i) noexcept
        {
            const float a = modAdsr (i).processSample();
            if (envXfLeft_[i] > 0)
            {
                envLast_[i] = modEnvBlend (i, a, envDraw_[i].take());
                --envXfLeft_[i];
            }
            else
                envLast_[i] = envDrawOn_[i] ? envDraw_[i].take() : a;
            return envLast_[i];
        }

        std::array<DrawEnvelopeTap, 2> envDraw_ {};
        std::array<bool, 2> envDrawOn_ {}; // APPLIED — audio thread only (`applyDrawEdges`)
        std::atomic<bool> envDrawDirty_ { true }; // FIREFORK E9
        std::array<std::atomic<bool>, 2> envDrawReq_ {}; // REQUESTED — `setEnvDraw`, any thread (row 303)
        std::array<int, 2> envXfLeft_ {}; // [row 304] crossfade samples left, per twin
        std::array<float, 2> envLast_ {}; // [D-M-0173] each ENV's last emitted value (>= 0)
        int envXfLen_ = 240;
        // [T-MASK-03 leaf F1] The per-slot mask BASE. An INT because the mask is a NUMBER and not a
        // continuum: the modulation is resolved to a whole mask before it reaches the oscillator.
        //
        // THERE IS NO `effectiveMask_` COPY, and its absence is a battery finding rather than an
        // omission. The first version kept one beside `effectiveOscPos_`/`effectivePdAmount_` and
        // computed the 9-bit fold into it — so a mutation that made the OSCILLATOR write clamp instead
        // of fold SURVIVED: the copy still folded, the read-back still reported the folded value, and
        // the instrument was the only thing that was wrong. That is the T-CORE-14 rule stated exactly
        // ("a bookkeeping copy of what I applied agrees with itself even when the DSP is wrong"), so
        // the copy is gone and `effectiveMask` asks the oscillator. Its siblings keep their copies
        // legitimately: a position or a warp depth is a float the tone path does not store in the form
        // the accessor owes, whereas the mask IS stored, exactly, by the thing that uses it.
        int baseMask_[kNumOscSlots] {};
        // [T-CMP-15 leaf D2] Default 0.5 — the SQUARE, matching `PulseOscillator`'s own default, so
        // a voice that is never told still agrees with the primitive rather than starting at duty 0
        // (silence) the way a zero-initialised member would.
        Real basePulseWidth_[kNumOscSlots] { PulseOscillator::kSquareDuty, PulseOscillator::kSquareDuty };
        std::optional<Source> pwmSource_[kNumOscSlots] {}; // nullopt = Manual = no route
        float pwmAmount_[kNumOscSlots] {}; // `synth.osc{1,2}.pwm` depth in [0,1]; 0 = no route

        // Which `Dest` a slot's mask is, as an index into `destLive_` — the two dests are per slot
        // (see the Dest roster note), so every site that asks "is my mask being modulated" needs this.
        [[nodiscard]] static std::size_t maskDestIndex (std::size_t slot) noexcept
        {
            return static_cast<std::size_t> (slot == 0 ? Dest::Osc1Mask : Dest::Osc2Mask);
        }
        // [T-CMP-15 leaf D2] Installs or clears this slot's PWM hard-wire from the two rows that
        // feed it. Called by BOTH setters, because either one changing has to re-install the same
        // slot — a depth edit must not resurrect a stale source and a source edit must not resurrect
        // a stale depth. Clears on EITHER off-state (Manual, or depth 0) rather than installing a
        // zero-depth route, which is `applyMatrix`'s rule: a live dest re-pushes its base every
        // sample, and both rows default to off.
        void refreshPwmSlot (std::size_t slot) noexcept
        {
            const int hardWire = slot == 0 ? CathodeModMatrix::kPwm1Slot : CathodeModMatrix::kPwm2Slot;
            const auto dest = slot == 0 ? Dest::Osc1Pw : Dest::Osc2Pw;
            // Written as a POSITIVE check with the value bound in the condition, not as
            // `if (!has_value() || depth == 0) clear; else setSlot(*opt, …)`. The negative form is
            // correct and `clang-tidy` still reds it: `bugprone-unchecked-optional-access` does not
            // follow the `||` short-circuit back to the deref in the else-branch. The house note in
            // `lessons/bootstrap-build-gate-layers.md` records the same checker failing to see a
            // Catch2 `REQUIRE(opt.has_value())`, so this is that limitation one form over — restated
            // rather than suppressed, because the positive form is also the clearer read.
            if (const auto source = pwmSource_[slot]; source.has_value() && pwmAmount_[slot] != 0.0f)
                setSlot (hardWire, *source, dest, pwmAmount_[slot]);
            else
                clearSlot (hardWire);
        }

        // [D-M-0193] Installs or clears both SUB hard-wires from their rows (either row changing re-installs
        // its slot, the `refreshPwmSlot` rule). A dest id the app never bound (-1) clears too.
        void refreshSubHardWires() noexcept
        {
            if (subMaskDest_ >= 0 && subMaskEnvAmount_ != 0.0f)
                setSlot (CathodeModMatrix::kMaskEnvSubSlot, static_cast<Source> (CathodeModMatrix::kSubMaskEnvSource), subMaskDest_, subMaskEnvAmount_);
            else
                clearSlot (CathodeModMatrix::kMaskEnvSubSlot);
            if (const auto source = subPwmSource_; source.has_value() && subPwDest_ >= 0 && subPwmAmount_ != 0.0f)
                setSlot (CathodeModMatrix::kPwmSubSlot, *source, subPwDest_, subPwmAmount_);
            else
                clearSlot (CathodeModMatrix::kPwmSubSlot);
        }
        static constexpr std::size_t kSubMaskEnv = kNumOscSlots; // dd_[2]
        CathodeModMatrix::DestId subMaskDest_ = -1;
        CathodeModMatrix::DestId subPwDest_ = -1;
        float subMaskEnvAmount_ = 0.0f;
        std::optional<Source> subPwmSource_ {};
        float subPwmAmount_ = 0.0f;

        [[nodiscard]] static constexpr std::size_t pwDestIndex (std::size_t slot) noexcept
        {
            return static_cast<std::size_t> (slot == 0 ? Dest::Osc1Pw : Dest::Osc2Pw);
        }

        // [T-MASK-03 leaf F1] One per-note TRANSIENT envelope per oscillator slot — the reference
        // instrument's dedicated mask envelopes, realised as ordinary matrix SOURCES (Source::DeltaDecay
        // {1,2}) so they can drive anything, plus a reserved hard-wire each so the MASK ENV knob always
        // works. Indexed by slot: dd_[0] is osc1's.
        // [D-M-0193] …and dd_[2] is the SUB's, fed to the matrix's INTERNAL source
        // (`CathodeModMatrix::kSubMaskEnvSource`) for its MASK ENV hard-wire.
        DeltaDecayEnvelope dd_[kNumOscSlots + 1];
        Lfo lfo1_;
        Lfo lfo2_;
        Lfo lfo3_;
        StepModulator step_;
        Lfo sh_; // dedicated Sample & Hold (locked to Lfo::Shape::SampleHold in prepare)
        PunchTransient punch_; // [T-CORE-30 leaf D] the §2 onset transient (off at amount 0)
        CathodeModMatrix matrix_;

        // [T-CORE-30 leaf D] `punch.mix`. PostFilter is the primitive's §2 default AND CATHODE's §5
        // per-engine choice, so the class default and the registry default agree — unlike
        // `env{2,3}Amount_` below, where they deliberately do not.
        PunchMix punchMix_ = PunchMix::PostFilter;

        // Runtime scalar sources.
        float velocity01_ = 1.0f;
        float keyTrack_ = 0.0f;
        // [T-CMP-06] Polyphonic aftertouch — the per-NOTE pressure the matrix's Aftertouch source
        // reads (Source::Aftertouch, T-MASK-03 leaf F1's placeholder). Zeroed at both gateOn() and
        // gateOff() — the "release/note-off returns the source to rest with no stuck modulation"
        // acceptance condition — and set live by CathodeVoiceEngine::aftertouch() while held.
        float aftertouch_ = 0.0f;
        // [T-CMP-21 leaf C] The two CHANNEL-WIDE controls. Deliberately NOT zeroed by gateOn(),
        // release() or the per-note reset paths that clear `aftertouch_` three lines up: a wheel is
        // the player's hand, not a property of a note, and a bend that snapped to centre on every
        // note-on would make a held bend unplayable. `reset()` — the whole-engine one — is where
        // they go home, because that IS "start again".
        float modWheel_ = 0.0f;
        float channelPressure_ = 0.0f; // [T-CGUI-95] channel-wide like the wheel; see `setChannelPressure`
        float bend_ = 0.0f;
        Real bendSemitones_ = kDefaultBendSemitones; // [T-CGUI-96] the span; see `setBendRange`
        // exp2(bend·bendSemitones_/12), cached at control rate. EXACTLY 1.0 at rest, so the hot
        // path's multiply is bit-exact and no golden moves until a wheel is touched.
        Real bendRatio_ = 1.0;
        // [T-MOD-01 leaf B] The engine-global DRIFTER ring's three values for this sample — pushed in
        // (setDrifterValues), never generated here. 0 until an engine broadcasts, which is bit-exactly
        // the state these sources were in before the card.
        std::array<float, 3> drifter_ {};

        // [T-CGUI-08] The modulation-stage bypass fades — see `setEnvBypassed`.
        BypassFade envFade_;
        BypassFade lfoFade_;
        BypassFade driftFade_;
        BypassFade mtxFade_;
        // [T-CGUI-76 S5R2] One sample's source values — ticked (`processSample`) or peeked
        // (`evaluateModulationNow`) — and the four bypass gains that gate them.
        struct SourceTaps
        {
            float e1, e2, e3, l1, l2, l3, st, sh, dd1, dd2, dd3, envGain, lfoGain, driftGain, mtxGain;
        };

        // Step 2 of `processSample`: feed the matrix every source value. Reads no generator's state.
        void feedSources (const SourceTaps& t) noexcept
        {
            matrix_.setUserSlotGain (t.mtxGain);
            matrix_.setSourceValue (Source::Env1, t.e1);
            matrix_.setSourceValue (Source::Env2, t.e2 * env2Amount_ * t.envGain);
            matrix_.setSourceValue (Source::Env3, t.e3 * env3Amount_ * t.envGain);
            matrix_.setSourceValue (Source::Lfo1, t.l1 * t.lfoGain);
            matrix_.setSourceValue (Source::Lfo2, t.l2 * t.lfoGain);
            matrix_.setSourceValue (Source::Lfo3, t.l3 * t.lfoGain);
            matrix_.setSourceValue (Source::StepMod, t.st * t.lfoGain);
            matrix_.setSourceValue (Source::SampleHold, t.sh * t.lfoGain);
            matrix_.setSourceValue (Source::Velocity, velocity01_);
            matrix_.setSourceValue (Source::KeyTrack, keyTrack_);
            // [T-CMP-06] Live — was T-MASK-03 leaf F1's 0-valued placeholder.
            // [T-CGUI-95] …merged with CHANNEL pressure as max(poly, channel). Only while channel pressure is up: at rest
            // the source is the poly value untouched — `clampUnit` is BIPOLAR, so a direct-dsp negative poly pressure
            // is a pinned behaviour a bare max would floor at 0.
            matrix_.setSourceValue (Source::Aftertouch, channelPressure_ > 0.0f ? std::max (aftertouch_, channelPressure_) : aftertouch_);
            // [T-CMP-21 leaf C] …and so is the WHEEL, on the same evidence: the picker has offered
            // "Wheel" since the matrix shipped and a route built on it modulated nothing, because
            // the app had no MIDI CC dispatch at all. Fed unconditionally, like the drifters — a
            // source that only reached the matrix while routed would jump when a route was added.
            matrix_.setSourceValue (Source::ModWheel, modWheel_);
            // [T-CGUI-24, owner 2026-09-25] …and the converter's own wander, read from the GRIT stage's
            // ConverterDrift (last sample's value — the stage runs after the matrix). Already scaled by
            // the patch's drift amount, so a patch with no drift routes nothing. Fed unconditionally.
            matrix_.setSourceValue (Source::ConvDriftReadout, outputStage_.voice().driftReadout());
            // [T-MASK-03 leaf F1] The two mask transients, ticked read-then-advance like every source
            // above. They are fed UNCONDITIONALLY even when nothing routes them: a source that only
            // advanced while routed would jump when a route was added mid-note, and two multiplies is
            // not a cost worth a branch on the audio path.
            matrix_.setSourceValue (Source::DeltaDecay1, t.dd1);
            matrix_.setSourceValue (Source::DeltaDecay2, t.dd2);
            matrix_.setSourceValue (static_cast<Source> (CathodeModMatrix::kSubMaskEnvSource), t.dd3); // [D-M-0193]
            // [T-MOD-01 leaf B] The three BROADCAST drifter values — stored, never ticked here (see
            // setDrifterValues). Fed unconditionally for the same reason the mask transients are: a
            // source that only reached the matrix while routed would jump when a route was added.
            matrix_.setSourceValue (Source::Drifter1, drifter_[0] * t.driftGain);
            matrix_.setSourceValue (Source::Drifter2, drifter_[1] * t.driftGain);
            matrix_.setSourceValue (Source::Drifter3, drifter_[2] * t.driftGain);
            // [T-CMP-07 leaf 6] The four macros, stored and never generated here (see
            // `setMacroValues`). Fed unconditionally alongside the drifters and for the same
            // reason — a source that only reached the matrix while routed would jump the moment a
            // route was added mid-note.
            matrix_.setSourceValue (Source::MacroGrit, macro_[0]);
            matrix_.setSourceValue (Source::MacroMotion, macro_[1]);
            matrix_.setSourceValue (Source::MacroOscPos, macro_[2]);
            matrix_.setSourceValue (Source::MacroPdAmt, macro_[3]);
        }

        // One sample of a fade: exactly 1 while in circuit (no tick), else the ticked gain.
        static float fadeGain (BypassFade& f) noexcept
        {
            if (f.inCircuit())
                return 1.0f;
            f.tick();
            return f.gain();
        }

        // Per-source note-on retrigger policy (`synth.{lfo1..3,sh,step}.sync` = Trig — see setLfoTrig). All
        // Free by default, so gateOn() touches no phase until a sync row says otherwise.
        std::array<bool, kTriggerableCount> lfoTrig_ {};

        // The §2.4 mod envelopes' global source gains (see setEnv2Amount). UNITY, not 0 — the
        // registry default arrives from core, and this class must stay behaviour-neutral for the
        // consumers that never set it.
        float env2Amount_ = 1.0f;
        float env3Amount_ = 1.0f;

        // Base (unmodulated) values for the wired destinations.
        float baseCutoffHz_ = 20000.0f; // wide open (TptSvf clamps below Nyquist)
        float baseQ_ = 0.70710678f; // Butterworth
        float baseLevel_ = 1.0f;
        float basePan_ = 0.0f;
        float baseConverterRateHz_ = 48000.0f; // primed to the host rate in prepare() (inc==1 bypass)
        float baseBitDepth_ = 8.0f; // BrokenConverter always-on default (2^(8-1) levels)
        float baseSyncAmount_ = 0.0f; // [T-CMP-04 leaf 2]
        float effectiveSyncAmount_ = 0.0f;
        float baseLoopError_ = 0.0f; // bypass
        float baseDrift_ = 0.0f; // bypass
        Wide baseNoteHz_ = 0.0; // Pitch base (fanned to osc / key-track / loop-err)
        // [T-CORE-16] PER SLOT — `synth.osc{1,2}.{pos,pdAmount}` are two independent params each,
        // and one shared base made both DEAD under a live OscPos/PdAmount routing.
        std::array<float, kNumOscSlots> baseOscPos_ {}; // OscPos base, per osc slot
        std::array<float, kNumOscSlots> basePdAmount_ {}; // PdAmount base, per osc slot
        float baseVowel_ = 0.0f; // FiltVowel base
        float baseNoiseLevel_ = 0.0f; // NoiseLevel base (0 == the stage's primed-OFF texture bypass)

        // [T-CMP-11] Organic's per-note cutoff contribution — a per-voice-trigger constant multiplier
        // set ONCE via setOrganicCutoffMult() (called by CathodeVoiceEngine::assignVoice, right before
        // gateOn()) and folded into EVERY place effectiveCutoffHz_ is derived from baseCutoffHz_, so a
        // mid-note knob move (setBaseCutoff) or a live matrix FiltCutoff destination both keep it for
        // the rest of this note's life. 1.0 = no contribution (bit-exact — Organic off/amount 0/default).
        float organicCutoffMult_ = 1.0f;
        float pushedQ_ = -1.0f, pushedReso01_ = 0.0f; // FIREFORK E3d (Q is always > 0, so -1 never matches)

        // [T-CGUI-83] The cutoff step limiter (see kCutoffSlewStepOctaves), on the FiltCutoff MOD value.
        // Ticked only while FiltCutoff is live; disarmed by a note-on, a reset and the route clearing.
        StepSlew cutoffSlew_;
        // The cutoff modulation as applied: the limiter's output mid-ramp, else the matrix sum.
        [[nodiscard]] float appliedCutoffMod() const noexcept
        {
            return cutoffSlew_.ramping() ? cutoffSlew_.value() : matrix_.modulation (Dest::FiltCutoff);
        }

        // [D-M-0158] F2's own-route limiter, in log2 Hz; ticked only by route writes (`genericRouteWrite_`).
        StepSlew f2CutoffSlew_;
        bool genericRouteWrite_ = false; // true only inside the generic loop's write
        // [T-CGUI-60] F2's delivered base (knob + own route) and the effective cutoff last pushed.
        float f2CutoffHz_ = 1360.0f;
        float f2Q_ = 1.0f;
        float f2EffectiveHz_ = 1360.0f;
        // F2 = its own base × F1's FiltCutoff modulation (live) × this note's Organic offset. The exp2
        // is only paid while F1's cutoff is routed; the push only while F2 is on or being configured.
        void pushF2() noexcept
        {
            const float mult = destLive_[static_cast<std::size_t> (Dest::FiltCutoff)]
                                   ? cutoffForMod (1.0f, appliedCutoffMod()) * organicCutoffMult_
                                   : organicCutoffMult_;
            f2EffectiveHz_ = f2CutoffHz_ * mult;
            outputStage_.voice().setFilter2CutoffReso (f2EffectiveHz_, f2Q_);
        }

        // Effective (post-mod) values the hot path last applied (metering / test anchors).
        float effectiveCutoffHz_ = 20000.0f;
        float effectiveLevel_ = 1.0f;
        float effectivePan_ = 0.0f;
        float effectiveConverterRateHz_ = 48000.0f;
        float effectiveBitDepth_ = 8.0f;
        float effectiveLoopError_ = 0.0f;
        float effectiveDrift_ = 0.0f;
        float effectiveNoteHz_ = 0.0f;
        float effectiveQ_ = 0.70710678f; // FiltReso (Butterworth default, mirrors baseQ_)
        std::array<float, kNumOscSlots> effectiveOscPos_ {};
        std::array<float, kNumOscSlots> effectivePdAmount_ {};
        float effectiveVowel_ = 0.0f;
        float effectiveNoiseLevel_ = 0.0f;

        // Destination live-set bookkeeping (which wired dests have ≥1 slot targeting them).
        //
        // The inactive marker is −1 and 0 is a REAL dest (OscPos), so this array cannot be spelled as
        // a brace list: one written shorter than the array value-initialises the tail to 0, which
        // would report those slots as targeting OscPos from construction and then decrement
        // `liveCount_[OscPos]` below zero on their first write. It was a literal 16 entries until
        // [T-CORE-14] grew kMaxSlots to 18 for the reserved hard-wires — so the list is computed,
        // and the ceiling has one home.
        static constexpr std::array<int, CathodeModMatrix::kMaxSlots> makeInactiveSlotDests() noexcept
        {
            std::array<int, CathodeModMatrix::kMaxSlots> a {};
            for (auto& d : a)
                d = -1;
            return a;
        }
        std::array<int, CathodeModMatrix::kMaxSlots> slotDest_ { makeInactiveSlotDests() }; // per-slot target dest, −1 = inactive
        // [T-CMP-07 leaf 3] Sized by the ID SPACE, not the named enum — an id leaf 4 routes
        // generically needs its live-count and its flag exactly as a named one does.
        // A count is at most kMaxSlots, so a byte holds it: `int` cost 576 B per voice (B31 budget).
        static_assert (CathodeModMatrix::kMaxSlots <= 255, "liveCount_ counts slots in a byte");
        std::array<std::uint8_t, static_cast<std::size_t> (CathodeModMatrix::kDestCapacity)> liveCount_ {}; // slots targeting each dest
        std::array<bool, static_cast<std::size_t> (CathodeModMatrix::kDestCapacity)> destLive_ {}; // liveCount > 0
        // [T-CMP-07 leaf 4a] The generic destinations: what to write, the range to clamp into, and
        // the base the apply table last supplied. `genericLive_` is the compact list the hot loop
        // walks — rebuilt in `refreshLive`, for the same reason the matrix keeps its touched list:
        // a per-sample loop must scale with what is ROUTED, never with the id space.
        // [T-CMP-07 leaf 6] grit · motion · oscPos · pdAmt, in `Source` order. 0 until L5 pushes,
        // which is bit-exactly the silence these four had before this leaf.
        std::array<float, 4> macro_ {};
        const GenericDest* genericDest_ = nullptr; // [T-CMP-20 leaf F] borrowed from the engine; may be null
        std::array<float, static_cast<std::size_t> (CathodeModMatrix::kDestCapacity)> genericBase_ {};
        std::array<CathodeModMatrix::DestId, static_cast<std::size_t> (CathodeModMatrix::kMaxSlots)> genericLive_ {};
        int genericLiveCount_ = 0;
        std::uint32_t liveEpoch_ = 0; // [T-CGUI-46 leaf 2] bumped by every `refreshLive`
        // [T-CMP-16 F3] Where in the control-rate period this voice is. PER VOICE and not shared,
        // because voices start at different times and a shared phase would make the update instant
        // depend on which voice happened to be rendered first. 0 means "write this sample".
        int genericControlPhase_ = 0;
        bool idleTick_ = false; // FIREFORK E19: the call comes from tickIdleSources
        int ctlPhase_ = 0; // FIREFORK E19: position inside the control period (0 = this sample is a control tick)
        bool pushValid_ = false; // FIREFORK E17: what the last push delivered
        bool pushResoLive_ = false;
        bool cutValid_ = false; // FIREFORK E17: the operands of the last effectiveCutoffHz_
        float cutModLast_ = 0.0f, cutBaseLast_ = 0.0f, cutOrgLast_ = 0.0f;
        float pushHz_ = 0.0f, pushQ_ = 0.0f;
        std::uint32_t pushModeW_ = 0;
        bool genericIterating_ = false; // [S5R B31 dsp2 F4] the generic loop is walking `genericLive_`
        bool liveRefreshOwed_ = false; // [S5R B31 dsp2 F4] a `refreshLive` deferred to that loop's end
#if NEMESIS_FORK_VOICE_PAD > 0
        // FIREFORK E18: pure padding, never read or written. See ForkConfig.h.
        alignas (8) std::uint8_t forkStridePad_[NEMESIS_FORK_VOICE_PAD] {};
#endif
    };
}
