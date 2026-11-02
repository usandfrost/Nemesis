#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ForkConfig.h>
#include <nemesis/dsp/AcidFilter.h>
#include <nemesis/dsp/ClassicFilter.h>
#include <nemesis/dsp/CombFilter.h>
#include <nemesis/dsp/FilterDrive.h>
#include <nemesis/dsp/FilterResonance.h>
#include <nemesis/dsp/FormantFilter.h>
#include <nemesis/dsp/LadderFilter.h>
#include <nemesis/dsp/ReedFilter.h>
#include <nemesis/dsp/ResonanceCeiling.h>
#include <nemesis/dsp/ScreamFilter.h>
#include <nemesis/dsp/SelfOscLimiter.h>
#include <nemesis/dsp/SovietFilter.h>
#include <nemesis/dsp/StateFilter.h>
#include <nemesis/dsp/Svf.h>
#include <nemesis/dsp/Waveshaper.h>

#include <cstdint>

namespace nemesis::dsp
{
    // CathodeFilterBank — ONE complete CATHODE filter (CATHODE.md §3): the §3 drive (pre or post),
    // the selected core of the model zoo (TptSvf LP/BP/HP/Notch, Formant, Comb, Ladder, Scream, Acid,
    // Soviet, Classic, Reed, State), and the T-CMP-32 self-oscillation limiter, in that order.
    //
    // [T-CGUI-60 leaf 1] EXTRACTED VERBATIM from `CathodeVoiceStage`, where it was the stage's
    // `filterStage` and the members it read, so the stage can own TWO of them (the dual filter). A
    // pure refactor: every setter, latch, dispatch and the per-sample path moved unchanged, and every
    // committed cathode golden is bit-identical across the move. The stage forwards its historic
    // filter API to F1, so no caller changed.
    //
    // RT-safety: prepare() designs the oversamplers and allocates the comb ring (off the audio
    // thread); every setter is control-rate; process() is inline and allocation-free. No JUCE.
    class CathodeFilterBank
    {
    public:
        using CombType = CombFilter::Type;

        // The active filter core / response (CATHODE.md §3): the four TptSvf responses, the
        // parallel-band-pass FORMANT (vowel) bank (FormantFilter, M1-09fc), the tuned feedforward+
        // feedback COMB (CombFilter, M1-09fe), and the two §3/§13.1 MODEL-ZOO filters — the linear
        // 4-pole LADDER (LadderFilter, M1-09fg) as its two authentic taps Ladder24 (24 dB/oct) /
        // Ladder12 (12 dB/oct), the nonlinear Sallen-Key SCREAM (ScreamFilter, M1-09fh) as its two
        // MS-20 responses ScreamLp / ScreamHp, the nonlinear 303 diode-ladder ACID (AcidFilter,
        // M1-09fj) as one LP-only response Acid (18 dB/oct), the nonlinear asymmetric-OTA SOVIET
        // (SovietFilter, M1-16a) as its three Polivoks responses SovietLp / SovietBp / SovietHp
        // (12 dB/oct), the nonlinear compensated 4-pole OTA-cascade CLASSIC (ClassicFilter, M1-16c)
        // as its four Curtis responses ClassicLp24 / ClassicLp12 / ClassicBp / ClassicHp, and the nonlinear
        // symmetric-clip state-variable REED (ReedFilter, M1-16e) as its three Steiner responses ReedLp /
        // ReedBp / ReedHp (12 dB/oct; the three modes latch the summed-input blend presets {1,0.5,0}) plus
        // ReedMorph (the continuous blend, set via setReedBlend(); D-M-0569), and the LINEAR Oberheim-SEM STATE (StateFilter, M1-16h)
        // as its six SEM responses StateLp / StateBp / StateHp / StateNotch / StateAp (12 dB/oct) and
        // StateMorph (the continuous LP↔Notch↔HP morph, blend set via setStateMorph()). In every non-TptSvf
        // mode the TptSvf is bypassed and the signal is routed through the alternative core instead; those
        // cores are configured via their own setters (setVowel()/setFormantQ(); setCombHz()/setCombFeedback()/
        // setCombType(); setLadderCutoff(); setScreamCutoff()/setScreamDrive(); setAcidCutoff()/setAcidDrive();
        // setSovietCutoff()/setSovietDrive(); setClassicCutoff()/setClassicDrive(); setReedCutoff()/
        // setReedDrive()/setReedBlend(); setStateCutoff()/setStateMorph()) rather than the setCutoff() knobs.
        // [T-CMP-26] The §4 MOD-MATRIX REMAP OF CUTOFF/RESO ONTO EACH MODEL IS NO LONGER DEFERRED:
        // `setModelCutoffReso` routes both to whichever core is live, and `CathodeModulatedVoice`
        // reaches it from every filter push. ⚠ The sentence that stood here was a DEFERRAL RECORDED
        // ONLY IN THE CODE IT DEFERRED — it never reached `DEFERRED.md`, so no card that consulted
        // the ledger could see that `Dest::FiltCutoff`/`FiltReso` were silent on seven of eight
        // models. That is the §4a rule failing in the direction it exists to prevent, and it cost
        // the model zoo every cutoff route it should have had since the zoo landed.
        enum class FilterMode : std::uint8_t {
            LowPass,
            BandPass,
            HighPass,
            Notch,
            Formant,
            Comb,
            Ladder24,
            Ladder12,
            ScreamLp,
            ScreamHp,
            Acid,
            SovietLp,
            SovietBp,
            SovietHp,
            ClassicLp24,
            ClassicLp12,
            ClassicBp,
            ClassicHp,
            ReedLp,
            ReedBp,
            ReedHp,
            StateLp,
            StateBp,
            StateHp,
            StateNotch,
            StateAp,
            StateMorph,
            // [D-M-0569, owner 2026-10-10] Reed·Morph — the Reed's continuous summed-input blend
            // (setReedBlend) as a FOURTH Reed type. APPENDED, never reordered: nothing stores this
            // index (a project stores the registry's model/type ordinals), but the fused-map tests
            // transcribe the declaration order.
            ReedMorph
        };

        // Where the §3 filter drive sits relative to the filter (CATHODE.md §3 "pre or post"):
        // PreFilter saturates the signal going INTO the filter (classic overdrive-into-cutoff);
        // PostFilter saturates the selected filter OUTPUT. Irrelevant while drive == 0 (bypass).
        enum class DrivePlacement : std::uint8_t {
            PreFilter,
            PostFilter
        };

        // [T-CMP-32] SELF-OSCILLATION STAGING — the owner ruling of 2026-09-12, and the SHAPE is the
        // ruling, not the number. Measured on the shipped path (`DEFERRED.md`:633): Ladder at cutoff
        // plain 56, drive 0, resonance swept, peaks 0.332 / 0.289 / 0.323 / 1.226 / 2.716 at reso plain
        // 16 / 47 / 64 / 96 / 120 — flat to 64, then 8x in the last quarter, leaving the plugin at
        // +8.7 dBFS. Above ~plain 91 the filter is no longer EMPHASISING, it is GENERATING, and a
        // resonance makeup is the wrong shape of law for that: `FltrEffect::outputCompFor` returns
        // exactly 1.0 for this patch (Ladder's reso trim is deliberately 0.00, §0's Moog
        // resonance-loss carve-out), was wired in on 2026-09-08, rendered BIT-IDENTICAL, and the owner
        // ruled to revert rather than ship it inert.
        //
        // SO IT IS STAGING, ON THE T-CORE-15 CONVENTION: named once, at the node that mixes, and the
        // gain is a power of two so the multiply adds no rounding of its own (`:256-257`). What does
        // NOT transfer from `kResonStagingGain` is its magnitude or the sign of its problem — that is
        // -36 dB on a PARALLEL layer that is off by default; this is the MAIN path.
        //
        // ⚠ SMOOTHSTEP, NOT A SWITCH, AND THAT IS THE WHOLE RISK OF THE CARD. A boundary crossed in an
        // audio-rate path is a click (T-CMP-25/27: an LFO crossing a switch point clicks once per
        // cycle). `t*t*(3-2t)` has ZERO SLOPE AT BOTH ENDS, so neither the value nor its derivative
        // jumps at the onset or at the top — a crossfade would fix the value and leave the corner.
        // ⚠ AND THE THRESHOLD LIVES IN reso01, NOT IN THE KNOB'S PLAIN. `filter.md`'s
        // `FLT_OSC_THR = 120/127` is a raw FX control value; read as a CATHODE plain it puts onset at
        // reso01 0.994, already deep inside self-osc. Worked through `kResoQ` and `resonance01ForQ`,
        // reso01 0.92 is knob plain **94.59** on the T-CGUI-18 `ExpZero` noon-Q-1.5 law (it was 84.69
        // on the old Power law) — so plain 94 is BIT-EXACT and plain 95 is the first that moves
        // (computed from the curve, not re-measured). ⚠ [S5R round 2] AT SPREAD 0
        // ONLY, and that qualifier is load-bearing: `setPatchCutoff` hands each voice
        // `spreadResoQ (q, off, amt)` — a +-0.5-octave multiplier on Q — BEFORE `resonance01ForQ`
        // runs, so at full `synth.voice.spread` the onset moves to plain **86.1** (was 76.1), eight knob steps
        // lower, and it moves PER VOICE: in one chord some voices carry a ~30 % blend at a knob
        // setting where others carry none. That is defensible behaviour (spread moves the real Q, so
        // staging should follow it) but it is not "by construction". The goldens only read clean
        // because `synth.voice.spread` ships at 0.
        // ⚠ [S5R] The earlier note here said "plain ~88-91, where the peak crosses unity". That is
        // where the PEAK crosses, not where the onset sits; the two are 4-7 knob steps apart and the
        // onset is the lower one. The bit-exact claim rests on the onset plain, not on the crossing.
        //
        // [D-M-0511 / D-M-0373, B31] SUPERSEDED IN PART: the dry term is now bounded too — by
        // `ResonanceCeiling` relative to the filter's input (post-drive), in every mode and at every resonance (see
        // `process`), and the voice stage bounds the F1/F2 SUM the same way. The history below is kept
        // because its measurements are what the ceiling's numbers answer.
        // ⚠ WHAT "UNITY-CEILINGED" DOES AND DOES NOT MEAN. It ceilings the WET TERM at this stage.
        // The blend is `(1-b)*driven + b*limited`, so the DRY term is unbounded and the worst case is
        // MID-CROSSFADE, not at the top: measured stage peak ~1.9 around plain 88-96 (MEASURED ON THE
        // PRE-T-CGUI-18 POWER LAW; mapped onto the ExpZero law it is plain ~98-105, not re-measured) against 1.07 at
        // plain 120 (old law; ~122 now). What makes the PLUGIN output land under full scale is ~0.49 of downstream gain in
        // `CathodeOutputStage` (amp env, velocity, out-drive, `level_` up to +12 dB, pan, the 16-voice
        // sum). **So the plugin-level margin is patch-dependent** — a chord, a unison stack or a hotter
        // `synth.level` can put plain 88-98 (old law; ~98-106 now) back over. Bounding the SUM rather than the wet term is a
        // follow-on, not something this law claims.
        static constexpr float kSelfOscOnset = 0.92f;

        // ⚠ THE FIRST CUT OF THIS LAW WAS AN ATTENUATOR AND THE OWNER'S EARS REJECTED IT — read this
        // before changing the instrument back. A flat gain on the voice output scales the BASE SIGNAL
        // and the self-oscillation together, so at the top of the knob the played note vanished and
        // the tone went hollow. Measured RMS per bar, before vs that attempt:
        //     reso  64 : -22.7 dB -> -22.7 dB   (untouched, correct)
        //     reso  96 : -12.2 dB -> -15.5 dB
        //     reso 120 :  -6.7 dB -> -15.3 dB   <- the top TWO bars land at the same loudness,
        // i.e. the last quarter of the travel stopped doing anything. And the reason the UNSTAGED
        // version sounds level is that HARD CLIPPING IS LIMITING: it holds the peak at full scale
        // while the RMS keeps climbing. Replacing a limiter with an attenuator loses the loudness.
        // T-CORE-15 had already written the general form of this mistake down — "an honest
        // normalisation is signal-dependent, not one constant".
        //
        // SO THE INSTRUMENT IS A SOFT LIMITER, not a gain. `AdaaSoftClip` is the family's §0.3 cubic
        // soft clip (f(x) = x - x^3/3, clamped to +-2/3), first-order ADAA (DAFx-16). Scaled by
        // `kLimitDrive` = 2/3 in and its reciprocal out, it is UNITY-CEILINGED: a signal at 2.716
        // comes out at exactly 1.0, a signal at 0.3 comes out at 0.296 (-0.12 dB, near-transparent),
        // so the base signal survives and only the peaks are held.
        // [D-M-0372] ADAA-1 ALONE WAS NOT ENOUGH ONCE THE CUTOFF OPENS (-38 dB of in-band alias at
        // 4 kHz), so while the blend is nonzero the stage runs 2x oversampled — `SelfOscLimiter`
        // carries it, the crossing at the onset and the latency it reports (owner ruling 2026-10-06).
        // ⚠ THE CEILING IS NO LONGER EXACT WHILE ENGAGED: band-limiting a clipped wave overshoots it
        // (Gibbs). Measured with a 3.0 sine at the stage, blend 1, 48 kHz: peak 1.002 below 1 kHz,
        // 1.037 below 4 kHz, 1.093 below 8 kHz, 1.204 worst (~11 kHz); 1.32 at an input of 100.
        static constexpr float kLimitDrive = SelfOscLimiter::kLimitDrive;

        // The blend, not a switch: `dry + smooth * (limited - dry)`. At smooth = 0 the result is the
        // dry sample BIT-EXACTLY, and smoothstep has zero slope at both ends, so neither the value
        // nor its derivative jumps at the onset or at the top. This is T-CMP-27's crossfade-the-
        // predicate answer rather than a switch an LFO can click on once per cycle.
        [[nodiscard]] static constexpr float selfOscBlendFor (float reso01) noexcept
        {
            const float t = (reso01 - kSelfOscOnset) / (1.0f - kSelfOscOnset);
            if (!(t > 0.0f))
                return 0.0f; // BIT-EXACT below the onset, and NaN-safe: the negation catches NaN too
            const float c = t < 1.0f ? t : 1.0f;
            return c * c * (3.0f - 2.0f * c);
        }

        // Designs every core's oversampler and allocates the comb ring; primes a wide-open default
        // cutoff so a model mode is audible before its setter runs. Not RT-safe (allocates).
        void prepare (Real sampleRate,
            int screamOversample = ScreamFilter::kDefaultOversample,
            int acidOversample = AcidFilter::kDefaultOversample,
            int sovietOversample = SovietFilter::kDefaultOversample,
            int classicOversample = ClassicFilter::kDefaultOversample,
            int reedOversample = ReedFilter::kDefaultOversample)
        {
            // §3 filter drive: design its oversampler FIRs. Default drive 0 -> bit-exact bypass, so
            // the composed goldens are unchanged until `drive` is dialed. Sample-rate-agnostic.
            drive_.prepare (fork::Config::driveOversample); // FIREFORK N5 (upstream: the default 4)
            filter_.prepare (sampleRate);
            filter_.setCutoff (kDefaultCutoffHz, kDefaultQ); // TptSvf clamps below Nyquist
            // §3 Formant (Vowel) bank: resolve its band-pass coeffs against the rate. It is only
            // in-circuit when filterMode_ == Formant, so this does not touch the LP/BP/HP/notch path.
            if constexpr (!fork::kPanelFilters) // FIREFORK E21a
                formant_.prepare (sampleRate);
            // §3 Comb: allocate its delay ring buffer + resolve the (SR-dependent) delay. Only
            // in-circuit when filterMode_ == Comb, so this does not touch the LP/BP/HP/notch path.
            if constexpr (!fork::kPanelFilters)
                comb_.prepare (sampleRate);
            // [D-M-0570, owner 2026-10-09] FEEDFORWARD + FEEDBACK, as FLTR's comb (FltrEffect::kCombType): the
            // feedback path is what RINGS like a plucked string (CombFilter's kMaxFb 0.97 — it never
            // self-oscillates; ResonanceCeiling caps the level). setCombType still overrides it.
            if constexpr (!fork::kPanelFilters)
                comb_.setType (CombType::Both);
            // §3/§13.1 model zoo: the Ladder (linear until driven, T-CMP-23) and the oversampled
            // nonlinear Scream. Each is only
            // in-circuit in its own FilterMode, so this does not touch the LP/BP/HP/notch path. Prime a
            // wide-open default cutoff (resonance 0) so a model mode is audible before its setter runs.
            ladder_.prepare (sampleRate, fork::Config::driveOversample); // FIREFORK N5
            ladder_.setCutoff (kDefaultCutoffHz, 0.0f);
            if constexpr (!fork::kPanelFilters)
            {
                scream_.prepare (sampleRate, screamOversample); // designs the Scream OS FIRs
                scream_.setCutoff (kDefaultCutoffHz, 0.0f);
            }
            acid_.prepare (sampleRate, acidOversample); // designs the Acid OS FIRs
            acid_.setCutoff (kDefaultCutoffHz, 0.0f);
            soviet_.prepare (sampleRate, sovietOversample); // designs the Soviet OS FIRs
            soviet_.setCutoff (kDefaultCutoffHz, 0.0f);
            classic_.prepare (sampleRate, classicOversample); // designs the Classic OS FIRs
            classic_.setCutoff (kDefaultCutoffHz, 0.0f);
            if constexpr (!fork::kPanelFilters)
            {
                reed_.prepare (sampleRate, reedOversample); // designs the Reed OS FIRs
                reed_.setCutoff (kDefaultCutoffHz, 0.0f);
            }
            // §5/§13.1 State (Oberheim SEM): LINEAR — no oversampler, no OS param. Only in-circuit in a
            // State mode, so this does not touch the LP/BP/HP/notch path. Prime a wide-open default cutoff.
            if constexpr (!fork::kPanelFilters)
            {
                state_.prepare (sampleRate);
                state_.setCutoff (kDefaultCutoffHz, 0.0f);
            }
            // [D-M-0372] The self-osc limiter's 2x oversampler + the crossing length at this rate.
            selfOscLimiter_.prepare (sampleRate);
            dryCeiling_.prepare (sampleRate); // [D-M-0373] the dry term's ceiling detectors
            if constexpr (fork::kExact) // FIREFORK E16: every core's prepare cleared it; only the selected one can run next
                lazyDirty_ = lazyBitFor (filterMode_);
        }

        // ---- Filter (§3; §9 synth.filt.{mode,cutoff,reso,drive}) ----
        // Selects the active filter core/response: one of the TptSvf's four responses (LP/BP/HP/
        // Notch), the alternative Formant / Comb core, or a model-zoo response (Ladder24/Ladder12,
        // ScreamLp/ScreamHp). Selecting a Scream response also latches the ScreamFilter's LP12/HP12
        // type (a trivial store) so the per-sample path never has to. Control-rate.
        void setFilterMode (FilterMode mode) noexcept
        {
            if constexpr (fork::kPanelFilters) // FIREFORK E21a: a model whose core is not built plays as the SVF low-pass (the panel cannot select one)
                if (mode == FilterMode::Formant || mode == FilterMode::Comb || (mode >= FilterMode::ScreamLp && mode <= FilterMode::ScreamHp)
                    || (mode >= FilterMode::ReedLp && mode <= FilterMode::StateMorph) || mode == FilterMode::ReedMorph)
                    mode = FilterMode::LowPass;
            ++modeWrites_;
            if constexpr (fork::kExact) // FIREFORK E6: only Ladder24 / Ladder12 read the ladder, one tap each
                ladder_.setUsedTap (mode != FilterMode::Ladder12);
            if constexpr (fork::kExact) // FIREFORK E5: entering Formant applies the deferred vowel
                if (vowelPending_ && mode == FilterMode::Formant)
                {
                    formant_.setVowel (pendingVowel_);
                    vowelPending_ = false;
                }
            if constexpr (fork::kExact) // FIREFORK E3a: entering an SVF mode applies the deferred cutoff
                if (svfPending_ && isSvfMode (mode))
                {
                    filter_.setCutoff (svfHz_, svfQ_);
                    svfPending_ = false;
                }
            filterMode_ = mode;
            if constexpr (fork::kExact) // FIREFORK E16: the core now selected may run, so its state may leave "freshly reset"
                lazyDirty_ = static_cast<std::uint8_t> (lazyDirty_ | lazyBitFor (mode));
            // A mode change moves where `synth.filt.drive` belongs: at Pre the Ladder (owner ruling 2026-09-07)
            // and, since D-M-0571 (owner 2026-10-09), Scream/Acid/Soviet/Classic/Reed take it in-loop. Re-route BEFORE the per-model latching below so a mode swap
            // cannot leave both the ladder and the FilterDrive holding the same amount.
            applyDriveRouting();
            if (mode == FilterMode::ScreamLp)
                scream_.setType (ScreamFilter::Type::Lp12);
            else if (mode == FilterMode::ScreamHp)
                scream_.setType (ScreamFilter::Type::Hp12);
            else if (mode == FilterMode::SovietLp)
                soviet_.setType (SovietFilter::Type::Lp);
            else if (mode == FilterMode::SovietBp)
                soviet_.setType (SovietFilter::Type::Bp);
            else if (mode == FilterMode::SovietHp)
                soviet_.setType (SovietFilter::Type::Hp);
            else if (mode == FilterMode::ClassicLp24)
                classic_.setType (ClassicFilter::Type::Lp24);
            else if (mode == FilterMode::ClassicLp12)
                classic_.setType (ClassicFilter::Type::Lp12);
            else if (mode == FilterMode::ClassicBp)
                classic_.setType (ClassicFilter::Type::Bp);
            else if (mode == FilterMode::ClassicHp)
                classic_.setType (ClassicFilter::Type::Hp);
            else if (mode == FilterMode::ReedLp)
                reed_.setType (ReedFilter::Type::Lp);
            else if (mode == FilterMode::ReedBp)
                reed_.setType (ReedFilter::Type::Bp);
            else if (mode == FilterMode::ReedHp)
                reed_.setType (ReedFilter::Type::Hp);
            else if (mode == FilterMode::ReedMorph)
                reed_.setBlend (reedBlend_); // [D-M-0569] the blend the morph last set, not a preset
            else if (mode == FilterMode::StateLp)
                state_.setType (StateFilter::Type::Lp);
            else if (mode == FilterMode::StateBp)
                state_.setType (StateFilter::Type::Bp);
            else if (mode == FilterMode::StateHp)
                state_.setType (StateFilter::Type::Hp);
            else if (mode == FilterMode::StateNotch)
                state_.setType (StateFilter::Type::Notch);
            else if (mode == FilterMode::StateAp)
                state_.setType (StateFilter::Type::Ap);
            else if (mode == FilterMode::StateMorph)
                state_.setType (StateFilter::Type::Morph);
        }
        // Cutoff in Hz (clamped below Nyquist by TptSvf) + resonance Q. Drives the LP/BP/HP/notch
        // responses; irrelevant in Formant mode (use setVowel/setFormantQ). Control-rate.
        void setCutoff (float cutoffHz, float q) noexcept
        {
            if constexpr (fork::kExact) // FIREFORK E3a: TptSvf::setCutoff is a pure function of (hz, q); defer it while out of circuit
            {
                svfHz_ = cutoffHz;
                svfQ_ = q;
                svfPending_ = !isSvfMode (filterMode_);
                if (svfPending_)
                    return;
            }
            filter_.setCutoff (cutoffHz, q);
        }
        // ---- Formant (Vowel) filter (§3 formant mode; §9 "cutoff = vowel-morph, reso = formant Q") ----
        // Vowel-morph position in [0,1] (A-E-I-O-U anchored at 0/0.25/0.5/0.75/1). Only in-circuit
        // while filterMode_ == Formant. Control-rate. (The mod-matrix/param remap of the cutoff knob
        // to this position is a §4 concern — deferred.)
        void setVowel (float position) noexcept
        {
            if constexpr (fork::kExact) // FIREFORK E5
                if (filterMode_ != FilterMode::Formant)
                {
                    vowelPending_ = true;
                    pendingVowel_ = position;
                    return;
                }
            formant_.setVowel (position);
            vowelPending_ = false;
        }
        // Shared formant Q (`reso`): higher → narrower, taller vowel peaks. Control-rate.
        void setFormantQ (float q) noexcept { formant_.setFormantQ (q); }
        // ---- Comb filter (§3 comb mode; §9 "cutoff = comb tuning, reso = feedback ±") ----
        // Comb tuning in Hz — the notch/peak spacing (D = fs/combHz). Only in-circuit while
        // filterMode_ == Comb. Control-rate. (The cutoff-knob → combHz remap is a §4 concern.)
        void setCombHz (float hz) noexcept { comb_.setCombHz (hz); }
        // Bipolar feedback macro (`reso`, the ± polarity) in [-1,1]: 0 = bit-exact bypass; magnitude
        // sets resonance, sign swaps peaks↔nulls. Clamped ±0.97 (rings, never self-oscillates).
        // Control-rate.
        void setCombFeedback (float g) noexcept
        {
            combKnobFeedback_ = g; // [T-CGUI-47] what the knob set, held while reso is unmodulated
            comb_.setFeedback (g);
        }
        // [T-CGUI-47] The voice's MODULATED cutoff and Q, in Comb mode only: cutoff drives the comb
        // pitch 1:1 (the knob's own retarget, Param Ref §2.3), and a MODULATED Q drives feedback by
        // the damping law every Comb takes (`resonance::resonance01ForQ` — FLTR's, T-CMP-29).
        // Unmodulated, the base feedback the patch set stands exactly. Owner rulings 2026-09-23.
        void setCombCutoffReso (float cutoffHz, float q, bool resoModulated) noexcept
        {
            // A COST guard, not a correctness one — and a LABELLED EQUIVALENT MUTANT (T-CGUI-47
            // battery): outside Comb the comb is out of circuit, and entering Comb re-sets both
            // values (`applyFilterFamily`), so deleting this changes no output. It spares the other
            // 26 modes a per-sample divide and a coefficient solve on every live cutoff route.
            if (filterMode_ != FilterMode::Comb)
                return;
            comb_.setCombHz (cutoffHz);
            comb_.setFeedback (resoModulated ? resonance::resonance01ForQ (q) : combKnobFeedback_);
        }
        // Which comb paths are active: Feedforward (FIR notches/peaks) | Feedback (resonant ring) |
        // Both. Control-rate.
        void setCombType (CombType type) noexcept { comb_.setType (type); }
        // ---- Ladder model (§3/§13.1 `synth.filt.model = ladder`; Moog 24 dB) ----
        // Ladder cutoff in Hz (clamped below Nyquist) + resonance in [0,1] (NOT the TptSvf q). Only
        // in-circuit while filterMode_ == Ladder24/Ladder12. Control-rate. (The cutoff/reso-knob →
        // ladder remap is a §4 mod-matrix concern — deferred.)
        void setLadderCutoff (float cutoffHz, float resonance) noexcept { ladder_.setCutoff (cutoffHz, resonance); }
        // ---- Scream model (§4/§13.1 `synth.filt.model = sk`; nonlinear MS-20 Sallen-Key) ----
        // Scream cutoff in Hz (clamped below the OS-rate Nyquist) + resonance in [0,1] (approaches the
        // K=3 self-osc threshold). Only in-circuit while filterMode_ == ScreamLp/ScreamHp. Control-rate.
        void setScreamCutoff (float cutoffHz, float resonance) noexcept { scream_.setCutoff (cutoffHz, resonance); }
        // The Scream's in-loop diode DRIVE in [0,1] (its "scream" — a pre-gain into the feedback
        // clipper). ROUTING OVERWRITES THIS (D-M-0571): every setDrive / setFilterMode / setDrivePlacement write sets the active core's drive — the routed amount at Pre, 0 at Post — so a value set here lasts only until the next one. Distinct from the §3 FilterDrive stage (setDrive) that sits pre/post the whole
        // filter. Control-rate.
        void setScreamDrive (float amount) noexcept { scream_.setDrive (amount); }
        // ---- Acid model (§13.1 `synth.filt.model = acid`; nonlinear Roland 303 diode ladder) ----
        // Acid cutoff in Hz (clamped below the OS-rate Nyquist) + resonance in [0,1] (the resonant peak
        // tracks the set cutoff; the 303 bass-loss is preserved). Only in-circuit while filterMode_ ==
        // Acid. Control-rate. (The cutoff/reso-knob → acid remap is a §4 mod-matrix concern — deferred.)
        void setAcidCutoff (float cutoffHz, float resonance) noexcept { acid_.setCutoff (cutoffHz, resonance); }
        // The Acid's in-loop diode DRIVE in [0,1] (the 303 "accent squelch" — a pre-gain into the
        // feedback clipper). ROUTING OVERWRITES THIS (D-M-0571): every setDrive / setFilterMode / setDrivePlacement write sets the active core's drive — the routed amount at Pre, 0 at Post — so a value set here lasts only until the next one. Distinct from the §3 FilterDrive stage (setDrive) that sits pre/post the
        // whole filter. Control-rate.
        void setAcidDrive (float amount) noexcept { acid_.setDrive (amount); }
        // ---- Soviet model (§3/§13.1 `synth.filt.model = soviet`; nonlinear asymmetric-OTA Polivoks) ----
        // Soviet cutoff in Hz (clamped below the OS-rate Nyquist) + resonance in [0,1] (maps the OTA
        // damping k = 1/Q from well-damped down to near self-oscillation; the resonant peak tracks the set
        // cutoff). Only in-circuit while filterMode_ == SovietLp/SovietBp/SovietHp. Control-rate. (The
        // cutoff/reso-knob → soviet remap is a §4 mod-matrix concern — deferred.)
        void setSovietCutoff (float cutoffHz, float resonance) noexcept { soviet_.setCutoff (cutoffHz, resonance); }
        // The Soviet's in-loop OTA DRIVE in [0,1] (the Polivoks bite — a pre-gain into the ASYMMETRIC
        // feedback clipper, injecting even harmonics + DC). ROUTING OVERWRITES THIS (D-M-0571): every setDrive / setFilterMode / setDrivePlacement write sets the active core's drive — the routed amount at Pre, 0 at Post — so a value set here lasts only until the next one. Distinct from the §3 FilterDrive stage
        // (setDrive) that sits pre/post the whole filter. Control-rate.
        void setSovietDrive (float amount) noexcept { soviet_.setDrive (amount); }
        // ---- Classic model (§3/§13.1 `synth.filt.model = classic`; nonlinear compensated 4-pole OTA cascade) ----
        // Classic cutoff in Hz (clamped below the OS-rate Nyquist) + resonance in [0,1] (approaches — but
        // stays below — the k=4 self-oscillation threshold; the resonance-compensated feedback keeps the LP
        // DC gain ~1, the Curtis "no bass-loss" signature). Only in-circuit while filterMode_ ==
        // ClassicLp24/ClassicLp12/ClassicBp/ClassicHp. Control-rate. (The cutoff/reso-knob → classic remap
        // is a §4 mod-matrix concern — deferred.)
        void setClassicCutoff (float cutoffHz, float resonance) noexcept { classic_.setCutoff (cutoffHz, resonance); }
        // The Classic's in-loop cell-input DRIVE in [0,1] (the gentle OTA soft-clip — a pre-gain into the
        // per-cell clipper). ROUTING OVERWRITES THIS (D-M-0571): every setDrive / setFilterMode / setDrivePlacement write sets the active core's drive — the routed amount at Pre, 0 at Post — so a value set here lasts only until the next one. Distinct from the §3 FilterDrive stage (setDrive) that sits pre/post the whole
        // filter. Control-rate.
        void setClassicDrive (float amount) noexcept { classic_.setDrive (amount); }
        // ---- Reed model (§7/§13.1 `synth.filt.model = reed`; nonlinear symmetric-clip Steiner-Parker SVF) ----
        // Reed cutoff in Hz (clamped below the OS-rate Nyquist) + resonance in [0,1] (maps the damping
        // k = 1/Q from well-damped down to a HIGHER-Q ceiling than Soviet — the "slightly unstable" Steiner
        // character, still clip-bounded; the resonant peak tracks the set cutoff). Only in-circuit while
        // filterMode_ == ReedLp/ReedBp/ReedHp. Control-rate. (The cutoff/reso-knob → reed remap is a §4
        // mod-matrix concern — deferred.)
        void setReedCutoff (float cutoffHz, float resonance) noexcept { reed_.setCutoff (cutoffHz, resonance); }
        // The Reed's in-loop SYMMETRIC DRIVE in [0,1] (the reedy grit — a pre-gain into the symmetric
        // feedback clipper, injecting ODD harmonics; the contrast with Soviet's asymmetric even-harmonic
        // bite). ROUTING OVERWRITES THIS (D-M-0571): every setDrive / setFilterMode / setDrivePlacement write sets the active core's drive — the routed amount at Pre, 0 at Post — so a value set here lasts only until the next one. Distinct from the §3 FilterDrive stage (setDrive) that sits pre/post the whole filter.
        // Control-rate.
        void setReedDrive (float amount) noexcept { reed_.setDrive (amount); }
        // The Reed's continuous "summed-input" blend morph (§7 "and the summed in-between"): blend in [0,1],
        // 0 → HP, 0.5 → BP, 1 → LP, triangular-summing the three simultaneous taps (bit-identical to the
        // matching Type preset at 0/0.5/1). [D-M-0569, owner 2026-10-10] HEARD ONLY IN ReedMorph: the
        // ReedLp/ReedBp/ReedHp types latch their preset and obey it — the morph is stored, never forwarded,
        // there — and entering ReedMorph re-applies the stored blend, so the call order does not matter.
        // Control-rate.
        void setReedBlend (float blend) noexcept
        {
            reedBlend_ = blend;
            if (filterMode_ == FilterMode::ReedMorph)
                reed_.setBlend (blend);
        }
        // ---- State model (§5/§13.1 `synth.filt.model = state`; LINEAR Oberheim-SEM state-variable) ----
        // State cutoff in Hz (clamped below Nyquist) + resonance in [0,1] (MILD/CLAMPED — maps the damping
        // k = 1/Q from well-damped down to a musical Q ~ 20 that always DECAYS, never sustains; the §11
        // "doesn't fully sing" character). Only in-circuit while filterMode_ == StateLp/Bp/Hp/Notch/Ap/Morph.
        // Control-rate. (The cutoff/reso-knob → state remap is a §4 mod-matrix concern — deferred.)
        void setStateCutoff (float cutoffHz, float resonance) noexcept { state_.setCutoff (cutoffHz, resonance); }
        // The State's continuous LP↔Notch↔HP morph (§5): blend in [0,1], 0 → HP, 0.5 → Notch, 1 → LP,
        // triangular-summing the lp/hp taps. Only heard in the StateMorph FilterMode; the discrete
        // StateLp/StateNotch/StateHp modes read the taps directly (bit-identical to the morph endpoints).
        // The SVF state is morph-independent, so the morph is exactly linear in the tap weights. Only
        // in-circuit while a State mode is selected. Control-rate.
        void setStateMorph (float blend) noexcept { state_.setMorph (blend); }

        // [T-CMP-26] THE ONE DISPATCH. Cutoff + resonance to whichever core is actually in the
        // signal path, so a caller does not have to know which model is selected — the stage
        // already latched that in `setFilterMode`. Before this existed the knowledge lived in
        // `CathodeMachine::applyFilterFamily` ALONE, which is why the §4 mod matrix (which does
        // not have it) reached `setCutoff` and therefore the TptSvf on every model: on seven of
        // eight the matrix wrote into a filter nobody was listening to.
        //
        // `reso01` is the cores' own [0,1] axis, produced by `resonance::resonance01ForQ`. It is
        // NOT the knob's normalised position — those are different laws and the divergence was
        // the audible defect this card closed (`FilterResonance.h` carries the measurement).
        //
        // The four TptSvf responses take (hz, Q) instead and are NOT handled here: the svf is
        // routed through the engine's patch base so the M1-18i cutoff/reso SPREAD is honoured,
        // and short-circuiting that would silently drop spread. Comb and Formant have their own
        // arms: the Comb's cutoff is its TUNE (and its feedback takes this same damping law,
        // T-CMP-29), the Formant takes the Q itself as its bank Q. Callers keep their own arms for
        // those three; this handles exactly the seven model-zoo cores.
        //
        // [T-CGUI-60 leaf 2] `setCutoffQ` below is the WHOLE dispatch, for a bank with no legacy
        // caller to stay bit-exact with — F2. F1 keeps these three historic calls, each inert where
        // another is live, because its goldens ride on them.
        void setModelCutoffReso (float cutoffHz, float reso01) noexcept
        {
            // ⚠ [S5R 2026-09-08] THE OBSERVATION IS STAMPED PER ARM, NOT UP HERE, AND THAT IS THE
            // WHOLE POINT OF IT. The first cut wrote both members before the switch, so the
            // read-backs reported what was PASSED rather than what was DELIVERED — moving any
            // `case` label into the do-nothing group left the law test green, because the members
            // were stamped regardless. A read-back that cannot fail when the dispatch breaks is
            // not an observation of the dispatch; it only ever guarded the law. Stamping inside
            // each arm makes the two claims separable: the value arrived AND it arrived at a core.
            // [T-CMP-32] `sings` is passed PER ARM for the same reason the observation is stamped per
            // arm: State delivers reso like the others but is NOT osc-capable (owner ruling 8 — no
            // oversampler, no in-loop saturator, ceiling Q 20), so staging it would attenuate a core
            // that never generates. Deriving `sings` from `filterMode_` up here would silently stage
            // State the day someone moved a case label.
            const auto deliver = [&] (float hz, float r, bool sings) noexcept {
                appliedModelCutoffHz_ = hz;
                appliedModelReso01_ = r;
                selfOscBlend_ = sings ? selfOscBlendFor (r) : 0.0f;
            };
            switch (filterMode_)
            {
                case FilterMode::Ladder24:
                case FilterMode::Ladder12:
                    ladder_.setCutoff (cutoffHz, reso01);
                    deliver (cutoffHz, reso01, true);
                    break;
                case FilterMode::ScreamLp:
                case FilterMode::ScreamHp:
                    scream_.setCutoff (cutoffHz, reso01);
                    deliver (cutoffHz, reso01, true);
                    break;
                case FilterMode::Acid:
                    acid_.setCutoff (cutoffHz, reso01);
                    deliver (cutoffHz, reso01, true);
                    break;
                case FilterMode::SovietLp:
                case FilterMode::SovietBp:
                case FilterMode::SovietHp:
                    soviet_.setCutoff (cutoffHz, reso01);
                    deliver (cutoffHz, reso01, true);
                    break;
                case FilterMode::ClassicLp24:
                case FilterMode::ClassicLp12:
                case FilterMode::ClassicBp:
                case FilterMode::ClassicHp:
                    classic_.setCutoff (cutoffHz, reso01);
                    deliver (cutoffHz, reso01, true);
                    break;
                case FilterMode::ReedLp:
                case FilterMode::ReedBp:
                case FilterMode::ReedHp:
                case FilterMode::ReedMorph:
                    reed_.setCutoff (cutoffHz, reso01);
                    deliver (cutoffHz, reso01, true);
                    break;
                case FilterMode::StateLp:
                case FilterMode::StateBp:
                case FilterMode::StateHp:
                case FilterMode::StateNotch:
                case FilterMode::StateAp:
                case FilterMode::StateMorph:
                    state_.setCutoff (cutoffHz, reso01);
                    deliver (cutoffHz, reso01, false);
                    break;
                case FilterMode::LowPass:
                case FilterMode::BandPass:
                case FilterMode::HighPass:
                case FilterMode::Notch:
                case FilterMode::Formant:
                case FilterMode::Comb:
                    // Not this dispatch's models — see the note above. Deliberately not a
                    // `default:`, so adding a FilterMode fails to compile here rather than
                    // silently joining the do-nothing arm. (`-Werror=switch` on `_layer_srcs`,
                    // CMakeLists.txt:411, makes that a build failure and not a warning.)
                    //
                    // ⚠ NOTHING IS STAMPED HERE. The read-backs must not claim a core received a
                    // value in a mode where no core did: the first cut stamped them for all 27
                    // modes, so in these six they reported a number no filter had ever seen —
                    // a false green waiting for the next test author. `kNotDelivered` is what
                    // "this mode has no model core" reads as.
                    appliedModelCutoffHz_ = kNotDelivered;
                    appliedModelReso01_ = kNotDelivered;
                    // ⚠ [T-CMP-32, S5R fix-now] AND THE BLEND IS CLEARED, NOT MERELY LEFT ALONE.
                    // `deliver` is the only other writer, so without this a patch that had been at
                    // high resonance on an osc-capable core kept its blend when the user switched to
                    // SVF/Comb/Formant — and `reset()` did not clear it either, so the limiter stayed
                    // in circuit on the INIT PATCH permanently. Measured stuck transfer: 0.5 -> 0.4815
                    // with a third harmonic, 1.0 -> 0.8519. Every golden is an SVF render from a fresh
                    // processor, so not one of them could see it.
                    selfOscBlend_ = 0.0f;
                    break;
            }
        }

        // [T-CGUI-60 leaf 2] ONE destination per mode: the TptSvf takes (hz, Q); the seven model cores
        // take `resonance01ForQ (q)` (which stages the self-osc limiter); the Comb takes the cutoff as
        // its tune and the same damping law of Q as feedback (T-CMP-29: FLTR's comb law); the
        // Formant takes Q as its bank Q (the vowel is shared — `setVowel`). The model call runs in
        // EVERY mode, so a switch away from an osc-capable core clears its blend.
        void setCutoffQ (float cutoffHz, float q) noexcept
        {
            setModelCutoffReso (cutoffHz, resonance::resonance01ForQ (q));
            switch (filterMode_)
            {
                case FilterMode::LowPass:
                case FilterMode::BandPass:
                case FilterMode::HighPass:
                case FilterMode::Notch:
                    filter_.setCutoff (cutoffHz, q);
                    break;
                case FilterMode::Comb:
                    comb_.setCombHz (cutoffHz);
                    comb_.setFeedback (resonance::resonance01ForQ (q));
                    break;
                case FilterMode::Formant:
                    formant_.setFormantQ (q);
                    break;
                case FilterMode::Ladder24:
                case FilterMode::Ladder12:
                case FilterMode::ScreamLp:
                case FilterMode::ScreamHp:
                case FilterMode::Acid:
                case FilterMode::SovietLp:
                case FilterMode::SovietBp:
                case FilterMode::SovietHp:
                case FilterMode::ClassicLp24:
                case FilterMode::ClassicLp12:
                case FilterMode::ClassicBp:
                case FilterMode::ClassicHp:
                case FilterMode::ReedLp:
                case FilterMode::ReedBp:
                case FilterMode::ReedHp:
                case FilterMode::ReedMorph:
                case FilterMode::StateLp:
                case FilterMode::StateBp:
                case FilterMode::StateHp:
                case FilterMode::StateNotch:
                case FilterMode::StateAp:
                case FilterMode::StateMorph:
                    break; // delivered by setModelCutoffReso above
            }
        }

        // [T-CMP-26] What the ACTIVE model core was last given, for tests that must observe the
        // seam rather than restate it. ⚠ These report what the destination HOLDS: they can prove
        // a value arrived and that it followed the right law, but they cannot tell "written once"
        // from "tracking" (`lessons/process-battery.md`, THE STORED VALUE AND THE EVENT). Any
        // claim that MODULATION reaches a core owes a rendered-audio or counting probe, not these.
        [[nodiscard]] float appliedModelResonance01() const noexcept { return appliedModelReso01_; }
        [[nodiscard]] float appliedModelCutoffHz() const noexcept { return appliedModelCutoffHz_; }
        // [T-CGUI-72] The derived self-osc blend (`selfOscBlend_`), so a test can prove it survives
        // the `applyAll(); reset();` boot order that once cleared it (T-CMP-32 S5R round 2).
        [[nodiscard]] float selfOscBlend() const noexcept { return selfOscBlend_; }
        // [D-M-0372] True while the self-osc limiter's oversampler runs (engaged or crossing) — the
        // cost read-back: below the onset it must be false, because there the stage is bit-exact.
        [[nodiscard]] bool selfOscOversampling() const noexcept { return selfOscLimiter_.oversampling(); }
        // [D-M-0373] The gain `ResonanceCeiling` applied to the core's output on the last sample — exactly 1
        // while the core lifts its input by under the ceiling's ratio.
        [[nodiscard]] float dryCeilingGain() const noexcept { return dryCeilingGain_; }
        // [D-M-0511] |the filter's input| on the last sample when a PreFilter drive sits in front of the core
        // (what the drive passed), else 0 — the stage's F1+F2 ceiling counts it in its reference.
        [[nodiscard]] float preDriveLevel() const noexcept { return preDriveLevel_; }
        // [T-CMP-29] The Comb's feedback as the CORE holds it (`kNotDelivered` stays the model-zoo
        // read-backs' business: the Comb is not one of the seven).
        [[nodiscard]] Real appliedCombFeedback() const noexcept { return comb_.feedback(); }
        // Filter drive amount in [0,1] (`synth.filt.drive`): 0 = bit-exact bypass; higher = more
        // oversampled soft-clip saturation (FilterDrive, M1-09fa). Control-rate.
        // `synth.filt.drive`. OWNER RULING 2026-09-07 (T-CMP-23): in a LADDER mode with drivePos = Pre,
        // "Pre" means IN-LOOP — the drive goes into the ladder's own feedback path (filter.md §3, "tanh
        // per stage … drive = the overdriven-Moog growl") instead of the separate pre-filter FilterDrive
        // stage, and the FilterDrive is held at 0 so the signal is not driven twice. [D-M-0571, owner
        // 2026-10-09] The same holds for every model with an in-loop drive — Scream, Acid, Soviet, Classic,
        // Reed: at Pre the amount is the active core's own BOUNDED loop drive (setLoopDrive — its driven input
        // soft-clipped inside its oversampler, as the Ladder's stages bound theirs [S5R]), and the oversampled
        // FilterDrive (and its latency) is out. Post is unchanged; the linear SVF/State and the Comb/Formant keep the
        // FilterDrive at both placements. Re-applied on a mode or placement change (see applyDriveRouting)
        // because either can move where the same amount belongs.
        void setDrive (float amount) noexcept
        {
            driveAmount_ = amount;
            applyDriveRouting();
        }
        // Whether the drive saturates the filter INPUT (PreFilter) or its selected OUTPUT (PostFilter),
        // per CATHODE.md §3 "pre or post". Control-rate; irrelevant while drive == 0. Default PreFilter.
        void setDrivePlacement (DrivePlacement placement) noexcept
        {
            drivePlacement_ = placement;
            applyDriveRouting();
        }

        [[nodiscard]] FilterMode filterMode() const noexcept { return filterMode_; }
        // [T-CGUI-77] How many `setFilterMode` calls reached this bank — the EVENT, for a probe that
        // must tell "not walked" from "walked with the same mode" (a held mode cannot).
        [[nodiscard]] std::uint32_t filterModeWrites() const noexcept { return modeWrites_; }
        [[nodiscard]] DrivePlacement drivePlacement() const noexcept { return drivePlacement_; }
        // The oversampler group delay the active drive adds on the through-path (0 when drive == 0).
        // Alignment/PDC across a live-modulated drive is a voice/engine concern (M1-08b free-running
        // stance) — the stage only reports it. The osc/converter stages are generators/base-rate and
        // add no reported through-path latency.
        [[nodiscard]] int driveLatencySamples() const noexcept { return drive_.latencySamples(); }
        // The oversampler group delay the LADDER adds — nonzero ONLY while a Ladder mode is selected AND
        // the drive is both in-loop (drivePos = Pre) and above zero. Owner ruling 2026-09-07: an undriven
        // Ladder patch stays latency-identical to the pre-growl core, so this is 0 there, exactly as the
        // §latency contract above promised unconditionally before the growl existed.
        [[nodiscard]] int ladderLatencySamples() const noexcept
        {
            return (filterMode_ == FilterMode::Ladder24 || filterMode_ == FilterMode::Ladder12)
                       ? ladder_.latencySamples()
                       : 0;
        }
        // The oversampler group delay the active Scream model adds on the through-path (its OS is only
        // running while a Scream mode is selected; 0 in every other mode. ⚠ This sentence used to end
        // "the Ladder is linear and adds none", which T-CMP-23 made false — the Ladder's own contribution
        // is reported by ladderLatencySamples() and is nonzero while it is driven in-loop. Adds none). Reported, like driveLatencySamples(), for the caller to align (M1-08b stance). Total
        // through-path latency = driveLatencySamples() + screamLatencySamples().
        [[nodiscard]] int screamLatencySamples() const noexcept
        {
            return (filterMode_ == FilterMode::ScreamLp || filterMode_ == FilterMode::ScreamHp)
                       ? scream_.latencySamples()
                       : 0;
        }
        // The oversampler group delay the active Acid model adds on the through-path (its OS is only
        // running while the Acid mode is selected; 0 in every other mode). Reported, like
        // screamLatencySamples(), for the caller to align (M1-08b stance). Only one model core is active
        // at a time, so at most one of scream/acid latency is nonzero.
        [[nodiscard]] int acidLatencySamples() const noexcept
        {
            return (filterMode_ == FilterMode::Acid) ? acid_.latencySamples() : 0;
        }
        // The oversampler group delay the active Soviet model adds on the through-path (its OS is only
        // running while a Soviet mode is selected; 0 in every other mode). Reported, like
        // screamLatencySamples()/acidLatencySamples(), for the caller to align (M1-08b stance). Only one
        // model core is active at a time, so at most one of scream/acid/soviet latency is nonzero.
        [[nodiscard]] int sovietLatencySamples() const noexcept
        {
            return (filterMode_ == FilterMode::SovietLp || filterMode_ == FilterMode::SovietBp
                       || filterMode_ == FilterMode::SovietHp)
                       ? soviet_.latencySamples()
                       : 0;
        }
        // The oversampler group delay the active Classic model adds on the through-path (its OS is only
        // running while a Classic mode is selected; 0 in every other mode). Reported, like
        // sovietLatencySamples(), for the caller to align (M1-08b stance). Only one model core is active at
        // a time, so at most one of scream/acid/soviet/classic latency is nonzero.
        [[nodiscard]] int classicLatencySamples() const noexcept
        {
            return (filterMode_ == FilterMode::ClassicLp24 || filterMode_ == FilterMode::ClassicLp12
                       || filterMode_ == FilterMode::ClassicBp || filterMode_ == FilterMode::ClassicHp)
                       ? classic_.latencySamples()
                       : 0;
        }
        // The oversampler group delay the active Reed model adds on the through-path (its OS is only running
        // while a Reed mode is selected; 0 in every other mode). Reported, like classicLatencySamples(), for
        // the caller to align (M1-08b stance). Only one model core is active at a time, so at most one of
        // scream/acid/soviet/classic/reed latency is nonzero.
        [[nodiscard]] int reedLatencySamples() const noexcept
        {
            return isReedMode() ? reed_.latencySamples()
                                : 0;
        }
        // The State model is LINEAR (no oversampler, no in-loop clip — filter.md §5/§13), so it adds NO
        // through-path group delay in any mode. This accessor is ALWAYS 0; it exists for symmetry with the
        // oversampled-model accessors and to positively document that State (like the Ladder) is latency-free
        // — so, unlike the OS cores, it is deliberately NOT summed into CathodeOutputStage::latencySamples().
        [[nodiscard]] int stateLatencySamples() const noexcept { return 0; }
        // [D-M-0372] The self-osc limiter's 2x group delay: nonzero ONLY while the blend is (every
        // patch below the onset stays latency-identical). REPORTED, like every in-loop oversampler
        // here, as the TARGET path's — across a crossing the heard lag glides (`SelfOscLimiter`).
        [[nodiscard]] int selfOscLatencySamples() const noexcept { return selfOscLimiter_.latencySamples (selfOscBlend_); }
        // The whole bank's through-path group delay: the drive's plus the active core's (only one
        // core is ever active, and the FilterDrive is at 0 — no latency — whenever a core takes the drive
        // in-loop, D-M-0571), plus the self-osc limiter's, which sits after both.
        [[nodiscard]] int latencySamples() const noexcept
        {
            return driveLatencySamples() + ladderLatencySamples() + screamLatencySamples()
                   + acidLatencySamples() + sovietLatencySamples() + classicLatencySamples()
                   + reedLatencySamples() + selfOscLatencySamples();
        }

        // The FILTER stage: [drive if PreFilter] → the selected core → [drive if PostFilter] → the
        // self-osc limiter. One function so the `synth.filt.bypass` blend in `processSample` has
        // exactly one wet value to fade against (T-CGUI-08).
        float process (float preFilter) noexcept
        {
            const float toFilter = processPre (preFilter);
            return processPost (toFilter, processCore (toFilter));
        }

        // [T-CMP-37 L2] `process` IN THREE HALVES, so the engine can run the Classic core for several voices at once
        // (ClassicFilterLanes) between a voice's pre and post halves: `process` is exactly this composition, and the
        // lane-wise path replaces ONLY the middle call, with a sample the scalar core is bit-identical to. Every
        // statement below is the one `process` held, cut at statement boundaries (the compiler fuses within one).
        //
        // PRE: §3 filter drive, PreFilter placement — saturate the filter INPUT, the whole summed node, texture
        // included (owner decision) — bypass at drive 0. Returns what the core is fed.
        [[nodiscard]] float processPre (float preFilter) noexcept
        {
            return (drivePlacement_ == DrivePlacement::PreFilter) ? drive_.processSample (preFilter) : preFilter;
        }

        // The mode the Classic core serves (the one a lane can stand in for).
        [[nodiscard]] bool classicCoreActive() const noexcept
        {
            return filterMode_ == FilterMode::ClassicLp24 || filterMode_ == FilterMode::ClassicLp12
                   || filterMode_ == FilterMode::ClassicBp || filterMode_ == FilterMode::ClassicHp;
        }
        // The scalar Classic core itself, for the lane-wise path to read coefficients from and to take state in and out of.
        // STALE while the voice is lane-resident (its state and FIR windows live in the lane): pull it back first (pullClassicFromLane).
        [[nodiscard]] ClassicFilter& classicCore() noexcept
        {
            if constexpr (fork::kExact) // FIREFORK E16: the lanes run this core by reference, whatever the mode says
                lazyDirty_ = static_cast<std::uint8_t> (lazyDirty_ | kLzClassic);
            return classic_;
        }
        [[nodiscard]] const ClassicFilter& classicCore() const noexcept { return classic_; }

        // CORE: the selected model, fed `toFilter`.
        [[nodiscard]] float processCore (float toFilter) noexcept
        {
            if constexpr (fork::kExact) // FIREFORK E10: one jump table instead of the if-chain; each arm is the chain's call
            {
                switch (filterMode_)
                {
                    case FilterMode::LowPass: return filter_.processSample (toFilter).lp;
                    case FilterMode::BandPass: return filter_.processSample (toFilter).bp;
                    case FilterMode::HighPass: return filter_.processSample (toFilter).hp;
                    case FilterMode::Notch: return filter_.processSample (toFilter).notch;
                    case FilterMode::Formant: return formant_.processSample (toFilter);
                    case FilterMode::Comb: return comb_.processSample (toFilter);
                    case FilterMode::Ladder24: return ladder_.processSample (toFilter).lp24;
                    case FilterMode::Ladder12: return ladder_.processSample (toFilter).lp12;
                    case FilterMode::ScreamLp: case FilterMode::ScreamHp: return scream_.processSample (toFilter);
                    case FilterMode::Acid: return acid_.processSample (toFilter);
                    case FilterMode::SovietLp: case FilterMode::SovietBp: case FilterMode::SovietHp: return soviet_.processSample (toFilter);
                    case FilterMode::ClassicLp24: case FilterMode::ClassicLp12: case FilterMode::ClassicBp: case FilterMode::ClassicHp:
                        return classic_.processSample (toFilter);
                    case FilterMode::ReedLp: case FilterMode::ReedBp: case FilterMode::ReedHp: case FilterMode::ReedMorph:
                        return reed_.processSample (toFilter);
                    case FilterMode::StateLp: case FilterMode::StateBp: case FilterMode::StateHp: case FilterMode::StateNotch:
                    case FilterMode::StateAp: case FilterMode::StateMorph: return state_.processSample (toFilter);
                }
                return 0.0f;
            }
            float out = 0.0f;
            if (filterMode_ == FilterMode::Formant)
            {
                // Formant mode: route through the parallel band-pass vowel bank; the TptSvf is
                // bypassed (only the active filter core advances, like the osc-slot mode branch).
                out = formant_.processSample (toFilter);
            }
            else if (filterMode_ == FilterMode::Comb)
            {
                // Comb mode: route through the tuned feedforward+feedback comb; the TptSvf is
                // bypassed (only the active filter core advances).
                out = comb_.processSample (toFilter);
            }
            else if (filterMode_ == FilterMode::Ladder24 || filterMode_ == FilterMode::Ladder12)
            {
                // Ladder model: route through the linear 4-pole ladder; the TptSvf is bypassed. Both
                // taps fall out of one pass — select the 24 dB (lp24) or 12 dB (lp12) tap.
                const LadderFilter::Outputs l = ladder_.processSample (toFilter);
                out = (filterMode_ == FilterMode::Ladder24) ? l.lp24 : l.lp12;
            }
            else if (filterMode_ == FilterMode::ScreamLp || filterMode_ == FilterMode::ScreamHp)
            {
                // Scream model: route through the nonlinear (oversampled) Sallen-Key; the TptSvf is
                // bypassed. The LP12/HP12 response was latched in setFilterMode().
                out = scream_.processSample (toFilter);
            }
            else if (filterMode_ == FilterMode::Acid)
            {
                // Acid model: route through the nonlinear (oversampled) 303 diode ladder; the TptSvf is
                // bypassed. LP-only (the 303 is low-pass), so there is a single response.
                out = acid_.processSample (toFilter);
            }
            else if (filterMode_ == FilterMode::SovietLp || filterMode_ == FilterMode::SovietBp
                     || filterMode_ == FilterMode::SovietHp)
            {
                // Soviet model: route through the nonlinear (oversampled) asymmetric-OTA state-variable
                // filter; the TptSvf is bypassed. The LP/BP/HP response was latched in setFilterMode().
                out = soviet_.processSample (toFilter);
            }
            else if (filterMode_ == FilterMode::ClassicLp24 || filterMode_ == FilterMode::ClassicLp12
                     || filterMode_ == FilterMode::ClassicBp || filterMode_ == FilterMode::ClassicHp)
            {
                // Classic model: route through the nonlinear (oversampled) compensated 4-pole OTA cascade;
                // the TptSvf is bypassed. The LP24/LP12/BP/HP response was latched in setFilterMode().
                out = classic_.processSample (toFilter);
            }
            else if (isReedMode())
            {
                // Reed model: route through the nonlinear (oversampled) symmetric-clip state-variable filter;
                // the TptSvf is bypassed. The LP/BP/HP summed-input blend was latched in setFilterMode()
                // (ReedMorph: the continuous blend setReedBlend() set).
                out = reed_.processSample (toFilter);
            }
            else if (filterMode_ == FilterMode::StateLp || filterMode_ == FilterMode::StateBp
                     || filterMode_ == FilterMode::StateHp || filterMode_ == FilterMode::StateNotch
                     || filterMode_ == FilterMode::StateAp || filterMode_ == FilterMode::StateMorph)
            {
                // State model: route through the LINEAR (no-OS) Oberheim-SEM state-variable filter; the
                // TptSvf is bypassed. The Lp/Bp/Hp/Notch/Ap/Morph response was latched in setFilterMode()
                // (the StateMorph blend can be morphed continuously via setStateMorph()).
                out = state_.processSample (toFilter);
            }
            else
            {
                const TptSvf::Outputs f = filter_.processSample (toFilter);
                switch (filterMode_)
                {
                    case FilterMode::LowPass:
                        out = f.lp;
                        break;
                    case FilterMode::BandPass:
                        out = f.bp;
                        break;
                    case FilterMode::HighPass:
                        out = f.hp;
                        break;
                    case FilterMode::Notch:
                        out = f.notch;
                        break;
                    case FilterMode::Formant:
                    case FilterMode::Comb:
                    case FilterMode::Ladder24:
                    case FilterMode::Ladder12:
                    case FilterMode::ScreamLp:
                    case FilterMode::ScreamHp:
                    case FilterMode::Acid:
                    case FilterMode::SovietLp:
                    case FilterMode::SovietBp:
                    case FilterMode::SovietHp:
                    case FilterMode::ClassicLp24:
                    case FilterMode::ClassicLp12:
                    case FilterMode::ClassicBp:
                    case FilterMode::ClassicHp:
                    case FilterMode::ReedLp:
                    case FilterMode::ReedBp:
                    case FilterMode::ReedHp:
                    case FilterMode::StateLp:
                    case FilterMode::StateBp:
                    case FilterMode::StateHp:
                    case FilterMode::StateNotch:
                    case FilterMode::StateAp:
                    case FilterMode::StateMorph:
                    case FilterMode::ReedMorph:
                        break; // handled by the outer branches; unreachable here
                }
            }
            return out;
        }

        // POST: [drive if PostFilter] → the self-osc limiter. `toFilter` is what the core was fed, `out` what it returned.
        [[nodiscard]] float processPost (float toFilter, float out) noexcept
        {
            // PostFilter placement: saturate the selected filter OUTPUT (bypass at drive 0).
            const float driven = (drivePlacement_ == DrivePlacement::PostFilter) ? drive_.processSample (out) : out;
            // [T-CMP-32] The self-osc limiter is LAST, after the drive, and deliberately so: the
            // ruling is about LEVEL, and limiting before the saturator would change how much a driven
            // patch clips — a character change nobody asked for.
            // [row 759] Invariant: the limiter's output is that of a limiter run on EVERY sample. It
            // only RUNS inside the region (running it dry would waveshape every voice of every patch);
            // outside, the one input it would have seen last is kept, and re-entry primes the ADAA
            // history with it — so x[n-1] is the previous sample, not the last in-region one.
            // [D-M-0372] Blend 0 and settled: `driven` bit for bit, no oversampler. Engaged: 2x.
            // [D-M-0373] …and what it crossfades FROM is bounded first: the core's output under
            // `ResonanceCeiling` relative to the FILTER's input — `toFilter`, post-drive [S5R B31 dsp2 F5]: the
            // pre-drive node would clamp a PreFilter drive's own gain on a quiet input and hold nothing on a
            // loud one the drive compresses — a GAIN, exactly 1 (bit-exact) while the
            // core lifts its input by under 2x. The T-CMP-32 blend `(1-b)*dry + b*limited` ceilinged only
            // its second term, so a core running at 30-150 below the onset fell to the clip's ~1 within
            // the last few knob steps (a cliff) and sat ~3x above the top of the knob mid-travel. With the
            // dry term already held, the crossfade walks from the held level to the clip's along the
            // smoothstep itself — the whole travel above the onset, no cliff — and nothing in the bound
            // depends on the resonance (the rejected B29 law's ceiling did, and jumped).
            // [D-M-0571 S5R] …and an IN-LOOP drive is referenced the same way: by what it passes into the loop,
            // shape(toFilter·g) — the bounded driven input — not the undriven node, which would clamp the drive's
            // own gain (ScreamHp at drive 1 on a 0.3 saw read gain 0.35: a pumping limiter). Drive 0: toFilter. The bound
            // fades in with the drive exactly as the core's does (LoopDriveBound, the one law for both).
            const float ceilingRef = loopDriveGain_ != 1.0
                                         ? static_cast<float> (LoopDriveBound::apply (static_cast<Real> (toFilter) * loopDriveGain_, loopBoundMix_))
                                         : toFilter;
            preDriveLevel_ = (drivePlacement_ == DrivePlacement::PreFilter && drive_.engaged()) ? std::abs (toFilter)
                             : loopDriveGain_ != 1.0                                            ? std::abs (ceilingRef)
                                                                                                : 0.0f;
            dryCeilingGain_ = dryCeiling_.gainFor (ceilingRef, std::abs (driven));
            return selfOscLimiter_.process (dryCeilingGain_ == 1.0f ? driven : driven * dryCeilingGain_, selfOscBlend_);
        }

        // [T-CGUI-08] Clears everything `filterStage` owns, the audio state `reset()` clears for the
        // filter, drive and limiter. NOT the HPF: it is not part of the FILTER bypass (owner 2026-09-23).
        void reset() noexcept
        {
            drive_.reset();
            // [D-M-0372] Nothing sounds after a reset, so an engaged limiter lands on its 2x path at once.
            selfOscLimiter_.reset (selfOscBlend_ != 0.0f);
            dryCeiling_.reset(); // [D-M-0373]
            dryCeilingGain_ = 1.0f;
            preDriveLevel_ = 0.0f;
            filter_.reset();
            if constexpr (!fork::kPanelFilters) // FIREFORK E21a
            {
                formant_.reset();
                state_.reset();
            }
            if constexpr (fork::kExact)
            {
                // FIREFORK E16: lazy reset. A core that was not selected since the last reset has not run, so its state is
                // still exactly what a reset leaves; clearing it again writes the same bytes. `lazyDirty_` holds the cores
                // that were selected (setFilterMode) or are selected now; those are cleared, the rest are skipped.
                const std::uint8_t d = lazyDirty_;
                if (d & kLzComb) { comb_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);
                if (d & kLzLadder) { ladder_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);
                if (d & kLzScream) { scream_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);
                if (d & kLzAcid) { acid_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);
                if (d & kLzSoviet) { soviet_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);
                if (d & kLzClassic) { classic_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);
                if (d & kLzReed) { reed_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);
                lazyDirty_ = lazyBitFor (filterMode_); // the selected core runs next
            }
            else
            {
                comb_.reset();
                ladder_.reset();
                scream_.reset();
                acid_.reset();
                soviet_.reset();
                classic_.reset();
                reed_.reset();
            }
        }

    public:
        // [S5R 2026-09-08] "no model core received this" — distinct from every legal cutoff and
        // every legal [0,1] resonance, so a test can assert ABSENCE as well as a value.
        static constexpr float kNotDelivered = -1.0f;

    private:
        static constexpr float kDefaultCutoffHz = 20000.0f; // wide open (TptSvf clamps below Nyquist)
        static constexpr float kDefaultQ = 0.70710678f; // Butterworth
        FilterDrive drive_;
        TptSvf filter_;
        FormantFilter formant_;
        CombFilter comb_;
        LadderFilter ladder_;
        ScreamFilter scream_;
        AcidFilter acid_;
        SovietFilter soviet_;
        ClassicFilter classic_;
        ReedFilter reed_;
        StateFilter state_;
        FilterMode filterMode_ = FilterMode::LowPass;
        std::uint32_t modeWrites_ = 0; // [T-CGUI-77] see filterModeWrites
        float reedBlend_ = 0.0f; // [D-M-0569] the morph's Reed blend, heard only in ReedMorph (0 = HP, the row default)
        float combKnobFeedback_ = 0.0f; // [T-CGUI-47] the knob's comb feedback (CombFilter's own default is 0)
        // [T-CMP-26] Last values DELIVERED to a model core by `setModelCutoffReso`. Seeded to the
        // sentinel, not to a plausible number: before the first push nothing has been delivered,
        // and an observation that reads 0.0 or 20 kHz there cannot be told from a real write.
        // [T-CMP-32] Control-rate, derived in `deliver`. 0.0f means the limiter is bypassed entirely,
        // so a non-osc-capable core and a resonance below the onset are both BIT-EXACT.
        float selfOscBlend_ = 0.0f;
        SelfOscLimiter selfOscLimiter_; // [D-M-0372] the T-CMP-32 stage, 2x while engaged
        ResonanceCeiling dryCeiling_; // [D-M-0373] bounds the core's output before the self-osc crossfade
        float dryCeilingGain_ = 1.0f; // the gain it applied to the last sample (a read-back)
        float preDriveLevel_ = 0.0f; // [D-M-0511] see `preDriveLevel`
        float appliedModelReso01_ = kNotDelivered;
        float appliedModelCutoffHz_ = kNotDelivered;
        // Sends `driveAmount_` to whichever stage owns it under the current mode + placement, and ZEROES
        // the other, so the two can never both be driving. At Pre, a model with an in-loop drive takes it
        // in its own loop — the Ladder (owner ruling 2026-09-07) and Scream/Acid/Soviet/Classic/Reed
        // (D-M-0571, owner 2026-10-09); the SVF, State, Comb and Formant use the FilterDrive as before.
        // Only the ACTIVE model core is written: entering its mode re-routes, so a core out of circuit
        // never needs it. Called on a drive, mode or placement change — all three move the answer.
        void applyDriveRouting() noexcept
        {
            const bool ladderMode = (filterMode_ == FilterMode::Ladder24 || filterMode_ == FilterMode::Ladder12);
            const bool screamMode = (filterMode_ == FilterMode::ScreamLp || filterMode_ == FilterMode::ScreamHp);
            const bool acidMode = filterMode_ == FilterMode::Acid;
            const bool sovietMode = (filterMode_ == FilterMode::SovietLp || filterMode_ == FilterMode::SovietBp
                                     || filterMode_ == FilterMode::SovietHp);
            const bool classicMode = (filterMode_ == FilterMode::ClassicLp24 || filterMode_ == FilterMode::ClassicLp12
                                      || filterMode_ == FilterMode::ClassicBp || filterMode_ == FilterMode::ClassicHp);
            const bool reedMode = isReedMode();
            const bool loopModel = ladderMode || screamMode || acidMode || sovietMode || classicMode || reedMode;
            const bool inLoop = loopModel && drivePlacement_ == DrivePlacement::PreFilter;
            const float core = inLoop ? driveAmount_ : 0.0f;

            // [D-M-0571 S5R] The five take it as a BOUNDED loop drive (setLoopDrive), as the Ladder's saturators
            // bound its own; `loopDriveGain_` keeps the pre-gain for the ceiling reference in `process`.
            ladder_.setDrive (ladderMode ? core : 0.0f);
            loopDriveGain_ = 1.0;
            if (screamMode)
            {
                scream_.setLoopDrive (core);
                loopDriveGain_ = scream_.driveGain();
            }
            else if (acidMode)
            {
                acid_.setLoopDrive (core);
                loopDriveGain_ = acid_.driveGain();
            }
            else if (sovietMode)
            {
                soviet_.setLoopDrive (core);
                loopDriveGain_ = soviet_.driveGain();
            }
            else if (classicMode)
            {
                classic_.setLoopDrive (core);
                loopDriveGain_ = classic_.driveGain();
            }
            else if (reedMode)
            {
                reed_.setLoopDrive (core);
                loopDriveGain_ = reed_.driveGain();
            }
            loopBoundMix_ = LoopDriveBound::mixFor (loopDriveGain_);
            drive_.setDrive (inLoop ? 0.0f : driveAmount_);
        }

    public:
        // FIREFORK B5: `*this = o` for the stereo twin, except the Comb ring, which is taken when Comb becomes the mode.
        void assignForTwin (const CathodeFilterBank& o) noexcept
        {
            if (o.filterMode_ == FilterMode::Comb)
            {
                *this = o;
                combRingStale_ = false;
                return;
            }
            std::vector<Real> mine, theirs;
            comb_.swapRing (mine);
            const_cast<CombFilter&> (o.comb_).swapRing (theirs); // audio thread only; restored below
            *this = o; // the comb's ring is empty on both sides here: nothing copied, nothing allocated
            const_cast<CombFilter&> (o.comb_).swapRing (theirs);
            comb_.swapRing (mine);
            combRingStale_ = true;
        }
        // FIREFORK B5: called after every edit while the twin runs: Comb just became the mode, so take the ring now.
        void refreshCombRingFrom (const CathodeFilterBank& o) noexcept
        {
            if (combRingStale_ && filterMode_ == FilterMode::Comb)
            {
                comb_.copyRingFrom (o.comb_);
                combRingStale_ = false;
            }
        }
        bool combRingStale_ = false;

        // FIREFORK E4: everything the cutoff setters (setCutoff, setModelCutoffReso, setCutoffQ) leave in this bank, copied from a
        // bank in the same mode that just received the same call. Not for Comb or Formant (see coefficientsCopyable).
        [[nodiscard]] bool coefficientsCopyable() const noexcept { return filterMode_ != FilterMode::Comb && filterMode_ != FilterMode::Formant; }
        void copyCoefficientsFrom (const CathodeFilterBank& o) noexcept
        {
            filter_.copyCoefficientsFrom (o.filter_);
            svfHz_ = o.svfHz_;
            svfQ_ = o.svfQ_;
            svfPending_ = o.svfPending_;
            appliedModelCutoffHz_ = o.appliedModelCutoffHz_;
            appliedModelReso01_ = o.appliedModelReso01_;
            selfOscBlend_ = o.selfOscBlend_;
            switch (filterMode_)
            {
                case FilterMode::Ladder24: case FilterMode::Ladder12: ladder_.copyCoefficientsFrom (o.ladder_); break;
                case FilterMode::ScreamLp: case FilterMode::ScreamHp: scream_.copyCoefficientsFrom (o.scream_); break;
                case FilterMode::Acid: acid_.copyCoefficientsFrom (o.acid_); break;
                case FilterMode::SovietLp: case FilterMode::SovietBp: case FilterMode::SovietHp: soviet_.copyCoefficientsFrom (o.soviet_); break;
                case FilterMode::ClassicLp24: case FilterMode::ClassicLp12: case FilterMode::ClassicBp: case FilterMode::ClassicHp:
                    classic_.copyCoefficientsFrom (o.classic_); break;
                case FilterMode::ReedLp: case FilterMode::ReedBp: case FilterMode::ReedHp: case FilterMode::ReedMorph: reed_.copyCoefficientsFrom (o.reed_); break;
                case FilterMode::StateLp: case FilterMode::StateBp: case FilterMode::StateHp: case FilterMode::StateNotch: case FilterMode::StateAp:
                case FilterMode::StateMorph: state_.copyCoefficientsFrom (o.state_); break;
                default: break;
            }
        }

    private:
        // FIREFORK E16: the cores whose reset is worth skipping (a comb ring, oversampler histories) and the model each is selected by.
        static constexpr std::uint8_t kLzComb = 1, kLzLadder = 2, kLzScream = 4, kLzAcid = 8, kLzSoviet = 16, kLzClassic = 32, kLzReed = 64;
        static constexpr std::uint8_t lazyBitFor (FilterMode m) noexcept
        {
            switch (m)
            {
                case FilterMode::Comb: return kLzComb;
                case FilterMode::Ladder24: case FilterMode::Ladder12: return kLzLadder;
                case FilterMode::ScreamLp: case FilterMode::ScreamHp: return kLzScream;
                case FilterMode::Acid: return kLzAcid;
                case FilterMode::SovietLp: case FilterMode::SovietBp: case FilterMode::SovietHp: return kLzSoviet;
                case FilterMode::ClassicLp24: case FilterMode::ClassicLp12: case FilterMode::ClassicBp: case FilterMode::ClassicHp: return kLzClassic;
                case FilterMode::ReedLp: case FilterMode::ReedBp: case FilterMode::ReedHp: case FilterMode::ReedMorph: return kLzReed;
                default: return 0;
            }
        }
    public:
        // bits of the cores that may hold state: selected since the last reset (read by the tests)
        std::uint8_t lazyDirty_ = 0;
    private:
        // FIREFORK E3a
        static constexpr bool isSvfMode (FilterMode m) noexcept
        {
            return m == FilterMode::LowPass || m == FilterMode::BandPass || m == FilterMode::HighPass || m == FilterMode::Notch;
        }
        float svfHz_ = 0.0f, svfQ_ = 0.0f;
        float pendingVowel_ = 0.0f; // FIREFORK E5
        bool vowelPending_ = false;
        bool svfPending_ = false;

        [[nodiscard]] bool isReedMode() const noexcept
        {
            return filterMode_ == FilterMode::ReedLp || filterMode_ == FilterMode::ReedBp
                   || filterMode_ == FilterMode::ReedHp || filterMode_ == FilterMode::ReedMorph;
        }

        float driveAmount_ = 0.0f;
        float loopBoundMix_ = 0.0f; // [D-M-0571 S5R] LoopDriveBound::mixFor (loopDriveGain_); beside driveAmount_, in its padding
        Real loopDriveGain_ = 1.0; // [D-M-0571 S5R] the active core's routed in-loop pre-gain (1 = none; never the Ladder's)
        DrivePlacement drivePlacement_ = DrivePlacement::PreFilter;
    };
}
