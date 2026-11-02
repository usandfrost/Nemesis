#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ForkConfig.h>
#include <nemesis/dsp/BrokenConverter.h>
#include <nemesis/dsp/BypassFade.h>
#include <nemesis/dsp/CathodeFilterBank.h>
#include <nemesis/dsp/CathodeOscCore.h>
#include <nemesis/dsp/ClassicFilterLanes.h>
#include <nemesis/dsp/ConverterDrift.h>
#include <nemesis/dsp/FilterResonance.h>
#include <nemesis/dsp/LoopError.h>
#include <nemesis/dsp/MacroOscillator.h>
#include <nemesis/dsp/MetallicResonator.h>
#include <nemesis/dsp/NoiseSource.h>
#include <nemesis/dsp/PhaseDistortionOscillator.h>
#include <nemesis/dsp/ResonanceCeiling.h>
#include <nemesis/dsp/SampleRateReducer.h>
#include <nemesis/dsp/Svf.h>
#include <nemesis/dsp/Wavefolder.h>
#include <nemesis/dsp/Waveshaper.h>
#include <nemesis/dsp/WavetableOscillator.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace nemesis::dsp
{
    // CathodeVoiceStage — CATHODE's per-voice TONE PATH (CATHODE.md §0 signal flow, §2–3): the
    // oscillator mix, then the always-on "broken converter" chain, then the multimode filter —
    // i.e. everything UPSTREAM of the AMP/env/output stage (that tail, plus bounce-parity, is
    // M1-10). This is a pure COMPOSITION of already-signed-off primitives, wired in the spec-exact
    // §0 order `mix → var-rate · bits · compand · ZOH · loop-err · drift → filter`:
    //
    //     CathodeOscCore (§1, M1-08b)                       osc mix
    //       → SampleRateReducer.capture (§2, M1-09xa)       key-tracked variable rate
    //           ↳ BrokenConverter (§2, M1-05)               bit-reduction + μ-law companding,
    //             applied AT CAPTURE via the M1-09xd hook (spec-exact: quantise happens BETWEEN
    //             capture and reconstruction, so ZOH holds / linear ramps between QUANTISED samples)
    //       → SampleRateReducer.reconstruct (ZOH | linear)  imperfect reconstruction / imaging
    //       → LoopError (§2, M1-09xc)                       periodic sub-sample loop-seam at f0
    //       → ConverterDrift (§2, M1-09xb)                  seeded wow/flutter/jitter + level wobble
    //       → FilterDrive (§3, M1-09fa) [if PreFilter]       oversampled soft-clip saturate (pre)
    //       → TptSvf (§3, M1-01)                            multimode LP/BP/HP/notch  (filter core)
    //         OR FormantFilter (§3, M1-09fc) [Formant mode] parallel-BP vowel bank  (alt filter core)
    //         OR CombFilter (§3, M1-09fe) [Comb mode]       tuned FF+FB comb        (alt filter core)
    //         OR LadderFilter (§3/§13.1, M1-09fg) [Ladder*] linear Moog 24 dB ladder (model-zoo core)
    //         OR ScreamFilter (§4/§13.1, M1-09fh) [Scream*] nonlinear MS-20 SK (oversampled model core)
    //         OR AcidFilter (§13.1, M1-09fj) [Acid]         nonlinear 303 diode ladder (oversampled model)
    //         OR SovietFilter (§1/§13.1, M1-16a) [Soviet*]  nonlinear asymmetric-OTA SVF (oversampled model)
    //         OR ClassicFilter (§2/§13.1, M1-16c) [Classic*] nonlinear compensated 4-pole cascade (OS model)
    //         OR ReedFilter (§7/§13.1, M1-16e) [Reed*]      nonlinear symmetric-clip SVF + summed-input (OS model)
    //         OR StateFilter (§5/§13.1, M1-16h) [State*]    LINEAR SEM SVF + LP↔Notch↔HP morph (no-OS model)
    //       → FilterDrive (§3, M1-09fa) [if PostFilter]      oversampled soft-clip saturate (post)
    //       → stage output (pre-amp)
    //
    // The per-sample DSP is golden-tested inside each primitive; the genuinely-new logic here is
    // only the serial wiring (incl. the capture-time interleave), parameter routing, note fan-out
    // and the filter-mode output selection. The correctness anchor is therefore FAITHFUL ROUTING —
    // the stage is bit-identical to a hand-composed chain of the same standalone primitives
    // configured identically (see the acceptance keystone) — NOT the golden (a §4.6 drift-guard on
    // our chosen composition).
    //
    // DEFAULTS ARE BIT-EXACT BYPASS for every §2 sibling except the always-on bits/compand grit:
    // the rate reducer is primed with device rate == host (inc == 1 → passes through, applying only
    // the capture-time quantiser), loopErr = 0 and drift = 0 early-return. So an untouched converter
    // chain reproduces the M1-09 `osc → grit → filter` render bit-for-bit and the committed
    // `cathode_grit_filter` golden is unchanged. There is no bit-exact "grit bypass" (a B-bit
    // quantiser is never identity), so the clean-tone claim stays spectral, not bit-exact.
    //
    // THE CONVERTER IS AT BASE RATE ON PURPOSE (§2, §11): its aliasing/quantisation/imaging IS the
    // effect, so — unlike the PD phase-map or a filter drive — it is NOT oversampled.
    //
    // The §3 filter DRIVE (oversampled soft-clip saturate, M1-09fa FilterDrive) is folded in with a
    // selectable pre/post-filter placement (§3 "pre or post"); `drive = 0` is a bit-exact bypass so
    // the composed goldens are unchanged until it is dialed. Because the saturator is oversampled it
    // adds latency when active — driveLatencySamples() reports it for the caller to align (§11).
    //
    // The §3 FORMANT (Vowel, M1-09fc FormantFilter) and COMB (M1-09fe CombFilter) modes are folded
    // in as selectable FilterModes: when filterMode_ == Formant/Comb the signal is routed through the
    // parallel band-pass vowel bank / the tuned feedforward+feedback comb instead of the TptSvf (only
    // the active filter core advances). The default mode is LowPass, so the LP/BP/HP/notch path — and
    // every committed stage golden/keystone — is unchanged. The vowel position + formant Q are set via
    // setVowel()/setFormantQ(); the comb tuning + bipolar feedback + type via setCombHz()/
    // setCombFeedback()/setCombType(). Both are linear, NOT oversampled (filter.md §0.2/§8–9).
    //
    // The §3/§13.1 MODEL-ZOO filters (`synth.filt.model`) are folded in the same way: the LINEAR 4-pole
    // LADDER (M1-09fg LadderFilter, Moog 24 dB) as two FilterModes selecting its two authentic taps —
    // Ladder24 (24 dB/oct) / Ladder12 (12 dB/oct) — and the NONLINEAR Sallen-Key SCREAM (M1-09fh
    // ScreamFilter, MS-20) as its two MS-20 responses — ScreamLp / ScreamHp — and the nonlinear 303
    // diode-ladder ACID (M1-09fj AcidFilter, Roland TB-303) as one LP-only response — Acid. Their cutoff
    // + resonance (resonance in [0,1], NOT the TptSvf's q) are set via setLadderCutoff()/setScreamCutoff()/
    // setAcidCutoff(); the Scream's and Acid's in-loop diode drive via setScreamDrive()/setAcidDrive().
    // [D-M-0571, owner 2026-10-09] At drivePos = Pre, `setDrive` IS that in-loop drive for every one of these
    // cores (Scream, Acid, Soviet, Classic, Reed — as for the Ladder since 2026-09-07): the oversampled §3
    // FilterDrive is held at 0 and its latency drops out; at Post the FilterDrive runs after the core. The nonlinear asymmetric-OTA SOVIET (M1-16a SovietFilter,
    // Polivoks) is folded in the same way as its three Polivoks responses — SovietLp / SovietBp / SovietHp
    // (12 dB/oct LP·BP·HP) — with cutoff + resonance via setSovietCutoff() and its in-loop OTA drive (the
    // asymmetric "bite", distinct from the §3 FilterDrive stage) via setSovietDrive(). The nonlinear
    // compensated 4-pole OTA-cascade CLASSIC (M1-16c ClassicFilter, Curtis/CEM-3320 — the "smooth American"
    // default) is folded in the same way as its four responses — ClassicLp24 / ClassicLp12 / ClassicBp /
    // ClassicHp — with cutoff + resonance via setClassicCutoff() and its in-loop cell-input drive via
    // setClassicDrive(). The nonlinear symmetric-clip state-variable REED (M1-16e ReedFilter, Steiner-Parker/
    // Synthacon — the "other character": vocal, reedy, slightly unstable) is folded in the same way as its
    // three Steiner responses — ReedLp / ReedBp / ReedHp (12 dB/oct LP·BP·HP) — with cutoff + resonance via
    // setReedCutoff() and its in-loop SYMMETRIC (odd-harmonic) drive via setReedDrive(). Reed additionally
    // exposes the defining Steiner CONTINUOUS "summed-input" blend morph via setReedBlend() (blend∈[0,1],
    // 0→HP, 0.5→BP, 1→LP): the three ReedLp/ReedBp/ReedHp modes latch the preset blends {1.0,0.5,0.0} and
    // OBEY them, and the fourth, ReedMorph, plays the blend setReedBlend() set (D-M-0569; the SVF state is
    // blend-independent, so the morph is exactly linear in the tap weights). The LINEAR Oberheim-SEM STATE (M1-16h StateFilter — the
    // "smooth, creamy, never harsh" model, the CONTRAST with the driven cores) is folded in the same way as
    // its six SEM responses — StateLp / StateBp / StateHp / StateNotch / StateAp (12 dB/oct) and StateMorph,
    // the continuous LP↔Notch↔HP morph — with cutoff + resonance (mild/clamped, §11) via setStateCutoff();
    // the StateMorph response's blend via setStateMorph() (blend∈[0,1], 0→HP, 0.5→Notch, 1→LP): the discrete
    // StateLp/StateNotch/StateHp modes read the taps directly, and StateMorph triangular-sums the lp/hp taps
    // (the SVF state is morph-independent, so the morph is exactly linear in the tap weights and its endpoints
    // are bit-exact the discrete taps). The STATE is LINEAR (stateLatencySamples() is ALWAYS 0); the Scream, the
    // Acid, the Soviet, the Classic and the Reed are OVERSAMPLED (filter.md §1/§2/§4/§6/§7/§13 — the in-loop
    // clipper needs it), so in one of those modes the through-path gains the OS group delay reported by
    // screamLatencySamples()/acidLatencySamples()/sovietLatencySamples()/classicLatencySamples()/
    // reedLatencySamples() (0 in every other mode).
    //
    // ⚠ THE LADDER IS NO LONGER UNCONDITIONALLY LINEAR (T-CMP-23). filter.md §3's in-loop "growl" makes it
    // an oversampled nonlinear core WHILE the drive is in-loop and above zero — i.e. a Ladder mode with
    // drivePos = Pre and `synth.filt.drive` > 0, which is what owner ruling 2026-09-07 made "Pre" mean
    // here. ladderLatencySamples() reports that and is 0 otherwise, so an UNDRIVEN Ladder patch is still
    // bit-exact and latency-identical to the pre-growl core. The consequence worth knowing: a Ladder
    // patch's reported latency now CHANGES when the player turns drive off zero.
    //
    // Only one model core is ever active (mode-exclusive), so total through-path latency is
    // driveLatencySamples() + ladderLatencySamples() + screamLatencySamples() + acidLatencySamples()
    // + sovietLatencySamples() + classicLatencySamples() + reedLatencySamples(); alignment/PDC is a
    // voice/engine concern. (driveLatencySamples() is 0 whenever the ladder holds the drive — the two
    // are mutually exclusive by applyDriveRouting.)
    //
    // The §5 NOISE / TEXTURE layer (M1-18a NoiseSource + MetallicResonator) is folded in as a per-voice
    // texture source mixed PRE-FILTER (§0): the noise generator + a metallic resonator bank (excited by
    // a mix-weighted blend of the raw osc mix + the noise) sum into the converter output — a clean
    // parallel path that does NOT pass through the grit — before the pre-filter drive (the default
    // `NoiseRoute::Parallel`; `Through` feeds the direct noise INTO the converter, D-M-0509). Set via
    // setNoiseType/setNoiseLevel/setNoiseSeed + setResonTune/setResonAmount, plus the §5 EXTENDED NOISE
    // controls (M1-18k): setResonDecay/setResonStruct/setResonColor (pass-throughs to the resonator's
    // shape setters, each neutral at 1.0 == the built table) and setResonMix (the osc↔noise excitation
    // BALANCE, neutral 0.5 == the built `oscMix + noiseOut` bit-exact). Noise + resonator default OFF
    // (noise level 0, resonator amount 0), guarded by a `textureLive_` flag, so an untouched texture is
    // a bit-exact bypass (the drifted signal is passed through unchanged and the noise RNG does not
    // advance) — and the DEFAULT mix (0.5) leaves the M1-18b excitation bit-exact — so the committed
    // goldens/keystones are unchanged until it is dialed. The resonator's own contribution is scaled at
    // the sum by `kResonStagingGain` (T-CORE-15), which is what makes `setResonAmount`'s [0,1] a wet
    // LEVEL rather than a raw gain on a high-Q bank — see the constant for why it is STAGING, not
    // normalisation.
    //
    // DEFERRED: the remaining §3 filter extras — key-track/
    // env-amt modulation, and the drive.md §6.3 drive refinements (character/asymmetry/compression/
    // tilt/mix/level); the §4 mod-matrix remap of the cutoff/reso knobs to the vowel position / the
    // comb tuning+feedback; the §4 NoiseLevel / reson.amount mod destinations (now unblocked by this
    // texture layer); the §10 LFO→comb-tuning rhythmic FX; the "Voice" (male/female/child) formant-set
    // shift. The §4 mod matrix, §6 voice-alloc/per-voice spread (incl. per-voice noise seed) and §7
    // AMP/env/output are later milestones.
    //
    // RT-safety: prepare() builds/allocates the composed primitives' tables and modulation buffers
    // (off the audio thread); setters resolve their coefficients at control rate; processSample()
    // is inline and calls only the primitives' inline hot paths (the capture quantiser is a lambda
    // that inlines with no std::function allocation — the M1-09xd hook). The linear grit path is
    // pure arithmetic; the companded grit path calls RT-safe libm (log1p/expm1 — owner-accepted
    // M1-05 deviation, non-allocating). Deterministic given the drift seed (§11) → bit-identical
    // re-render; reset() reseeds drift. No JUCE. Not thread-safe against prepare().
    class CathodeVoiceStage
    {
    public:
        // [stereo voice S1] One per-voice sample as an L/R PAIR, before the AMP envelope and pan. A mono
        // voice has `left == right` on every sample (one value, copied); the Stereo filter route (S2) splits
        // it — see `stereoActive`. `CathodeOutputStage::StereoSample` is this type.
        struct StereoSample
        {
            float left = 0.0f;
            float right = 0.0f;
        };

        using OscMode = CathodeOscCore::Mode;
        using Interp = SampleRateReducer::Interp;
        using CombType = CombFilter::Type;
        using NoiseType = NoiseSource::Type;

        // [T-CGUI-60 leaf 1] THE FILTER LIVES IN `CathodeFilterBank` NOW — the stage owns F1 and
        // forwards its historic filter API to it unchanged, so every caller and every golden is the
        // same. The mode roster, the drive placement and the self-osc staging law are the bank's.
        using FilterMode = CathodeFilterBank::FilterMode;
        using DrivePlacement = CathodeFilterBank::DrivePlacement;

        // [T-CGUI-60 leaf 2] `synth.filt.route` — how F2 joins F1 while `synth.filt2.on` is On (owner
        // rulings 2026-09-27). Series: F2(F1(x)). Parallel: g1·F1(x) + g2·F2(x), full gain at the
        // centre (the FLTR sum), the SHORTER branch delayed so the two sum in phase. Stereo (stereo voice
        // S2, D-M-0194): F1 LEFT, F2 RIGHT — the Parallel branches, aligned the same way, kept apart
        // (L = g1·F1(x), R = g2·F2(x); the balance is an L/R level) and carried to the pan as a pair.
        // STORAGE ORDER IS THE REGISTRY'S (kFiltRoutes), LOCKED.
        enum class FilterRoute : std::uint8_t {
            Series,
            Parallel,
            Stereo
        };
        // [T-CGUI-64] `synth.fold.pos` — WHERE the wavefolder sits (owner 2026-09-27). Pre: after the
        // HPF, before the converter (GRIT chews the fold). Post: after the whole FILTER stage (F1/F2
        // routing and its bypass blend), before the amp. STORAGE ORDER IS THE REGISTRY'S, LOCKED.
        enum class FoldPosition : std::uint8_t {
            Pre,
            Post
        };
        // [D-M-0509] `synth.noise.route` — WHERE the §5 noise joins the voice (owner ruling 2026-10-07,
        // noise_resonator.md §2.1). Parallel (the default): the clean path AROUND the converter, summed at
        // the pre-filter node — the built sound, bit for bit. Through: injected INTO the converter after
        // HPF/FOLD Pre, so GRIT decimates and crushes the hiss. STORAGE ORDER IS THE REGISTRY'S, LOCKED.
        enum class NoiseRoute : std::uint8_t {
            Parallel,
            Through
        };
        // `synth.filt.balance` in [-1, 1], Parallel and Stereo (an L/R tilt there) — a MIXER BALANCE, not a crossfade: the
        // centre keeps both at unity (1.0f·x is exact), turning toward one end fades the OTHER out,
        // and the ends are one filter alone. Exposed so the keystone applies the same gains.
        struct BalanceGains
        {
            float f1;
            float f2;
        };
        [[nodiscard]] static constexpr BalanceGains balanceGains (float balance) noexcept
        {
            const float b = balance < -1.0f ? -1.0f : (balance > 1.0f ? 1.0f : balance);
            return { b <= 0.0f ? 1.0f : 1.0f - b, b >= 0.0f ? 1.0f : 1.0f + b };
        }
        static constexpr float kSelfOscOnset = CathodeFilterBank::kSelfOscOnset;
        static constexpr float kLimitDrive = CathodeFilterBank::kLimitDrive;
        [[nodiscard]] static constexpr float selfOscBlendFor (float reso01) noexcept
        {
            return CathodeFilterBank::selfOscBlendFor (reso01);
        }

        static constexpr int kNumSlots = CathodeOscCore::kNumSlots;

        // The §5 metallic resonator's STAGING GAIN at the texture sum — the level at which the
        // resonator bank is mixed into the voice (CATHODE.md §5, Param Ref §2.7; T-CORE-15).
        //
        // IT IS A STAGING CHOICE, NOT A NORMALISATION, and the distinction is the whole point.
        // `MetallicResonator` is an honest RAW-GAIN primitive: `processSample` returns
        // `amount * bank`, so `amount` is an output gain on a high-Q modal bank whose own gain
        // varies with tune / decay / struct / color. No single constant can make `amount = 1`
        // unity for every one of those, so this one does not claim to — it sets a sane default
        // HEADROOM so the control's travel is musical. Claiming normalisation here would be the
        // T-CORE-09 "a 0..1-shaped NAME invites a 0..1 clamp" trap in reverse.
        //
        // The value: 1/64. Measured at T-CORE-15 on a quiet-osc + noise-bed probe with the grit
        // transparent and the filter wide open, the bank's contribution at `amount = 1.0` peaked at
        // 42–54× the dry converter path's own peak across tune 176 Hz – 7.9 kHz (≈51× at the
        // Param Ref §2.7 default v = 64 ≈ 650 Hz). Dividing by 64 puts 100 % at ≈0.8× the dry
        // path's level — a dry-LEVEL WET MIX with a few dB of margin — and keeps the loudest thing
        // the layer does (a `tune` sweep across a ringing bank, ≈3.6× the static peak) inside full
        // scale. A power of two, so the multiply adds no rounding of its own.
        //
        // Before it, `synth.reson.amount` was usable over roughly its first 3 % of travel and
        // clipped over the other 97 % (peak 24.8× full scale at plain 100, 260× while sweeping
        // `tune`) — the T-CORE-13 measurement that carded this. The mix decision lives HERE, at
        // the layer that does the mixing: not in the primitive (an honest normalisation is
        // signal-dependent, not one constant) and not in the app's apply-table conversion (a fudge
        // hiding an L1 level structure, and T-CORE-11's re-derive rule would apply) — the two
        // alternatives the owner ruled out on 2026-07-28.
        //
        // `amount = 0` is UNAFFECTED: the resonator early-returns 0 and `textureLive_` gates the
        // whole block, so the bit-exact bypass is a property of those two and not of this gain.
        static constexpr float kResonStagingGain = 1.0f / 64.0f;

        // The two excitation weights the §5 resonator MIX (synth.reson.mix) applies to the osc mix
        // and the noise output before they drive the metallic resonator (CATHODE.md §5, Param Ref §2.7).
        struct ResonExcite
        {
            float wOsc;
            float wNoise;
        };

        // Resonator excitation BALANCE (CATHODE.md §5 "resonator excitation balance"): `mix` in [0,1]
        // balances the osc↔noise drive of the metallic resonator. It is a MIXER BALANCE, NOT a
        // crossfade: the CENTRE (0.5) keeps BOTH sources at unity so the excitation is the built
        // `oscMix + noiseOut` BIT-EXACT (wOsc == wNoise == 1.0f; 1.0f·x is exact in IEEE-754), and
        // turning toward one side fades the OTHER out while the near side stays at unity — all-osc
        // (mix 0) = pitched metallic ring, all-noise (mix 1) = atmospheric clang, neither boosted.
        // Exposed static so the faithfulness keystone applies the SAME weights (the preGainFor idiom).
        static ResonExcite resonExciteWeights (float mix) noexcept
        {
            const float m = mix < 0.0f ? 0.0f : (mix > 1.0f ? 1.0f : mix);
            const float wOsc = m <= 0.5f ? 1.0f : 2.0f * (1.0f - m);
            const float wNoise = m >= 0.5f ? 1.0f : 2.0f * m;
            return { wOsc, wNoise };
        }

        // Prepares the oscillator core (WT rate + PD sine table/decimator) and the filter. Sets a
        // wide-open default cutoff so the stage is audible before setCutoff() is called. Not
        // RT-safe (allocates) — call like prepareToPlay. The grit is memoryless (no prepare).
        void prepare (Wide sampleRate,
            int pdOversample = fork::Config::pdOversample, // FIREFORK: upstream 4
            int screamOversample = fork::Config::filterOversample,
            int acidOversample = fork::Config::filterOversample,
            int sovietOversample = fork::Config::filterOversample,
            int classicOversample = fork::Config::classicOversample > 0 ? fork::Config::classicOversample : fork::Config::filterOversample,
            int reedOversample = fork::Config::filterOversample)
        {
            osc_.prepare (sampleRate, pdOversample);
            srr_.prepare (sampleRate);
            // Default the converter rate to the host rate so the reducer is a bit-exact bypass
            // (inc == 1) until `rate`/`track` are dialed — the always-on grit still runs at capture.
            srr_.setDeviceRateHz (static_cast<float> (sampleRate));
            loopErr_.prepare (sampleRate);
            drift_.prepare (sampleRate);
            // §3 filter (drive · core · self-osc limiter): F1 — see CathodeFilterBank::prepare.
            f1_.prepare (sampleRate, screamOversample, acidOversample, sovietOversample, classicOversample, reedOversample);
            if (laneSlot_ != nullptr)
                laneSlot_->drop(); // [T-CMP-37 L2] prepare cleared the scalar
            f2_.prepare (sampleRate, screamOversample, acidOversample, sovietOversample, classicOversample, reedOversample);
            if (laneSlot2_ != nullptr)
                laneSlot2_->drop(); // [T-CMP-37 L3] prepare cleared F2's scalar core too
            // [stereo voice S3] The right chain's twins, prepared alike so starting it copies into storage
            // that is already the right size (no allocation on the audio thread). On the HEAP: a voice
            // carries one pointer for it, not two more banks (the eager-instance budget, EngineCore).
            if (!fork::Config::stereoTwin) // FIREFORK P3: no twin, so width and the Stereo route play mono
                right_.reset();
            else
            {
            if (right_ == nullptr)
                right_ = std::make_unique<RightChain>();
            right_->f1.prepare (sampleRate, screamOversample, acidOversample, sovietOversample, classicOversample, reedOversample);
            right_->f2.prepare (sampleRate, screamOversample, acidOversample, sovietOversample, classicOversample, reedOversample);
            right_->hpf.prepare (sampleRate);
            right_->srr.prepare (sampleRate);
            right_->loopErr.prepare (sampleRate);
            right_->drift.prepare (sampleRate);
            right_->reson.prepare (sampleRate);
            right_->noiseSide.prepare (sampleRate);
            // [S5R 6] What `noise_` was given before the side stream existed: its type, and its seed salted.
            right_->noiseSide.setType (noiseType_);
            right_->noiseSide.setSeed (noiseSeed_ ^ kNoiseSideSalt);
            right_->noiseSide.setLevel (0.0f); // as noise_ is primed below
            } // FIREFORK P3
            rightRun_ = false;
            rightMix_ = 0.0f;
            rightFadeStep_ = static_cast<float> (1.0 / std::max (static_cast<Wide> (1.0), 0.010 * sampleRate));
            noiseWidthStep_ = static_cast<float> (1.0 / std::max (static_cast<Wide> (1.0), kWidthGlideSeconds * sampleRate)); // [S5R 2]
            hpf_.prepare (sampleRate);
            // §5 texture layer (noise + metallic resonator, M1-18a). Both primitives default to their
            // "on" state, so PRIME THEM OFF here (noise level 0, resonator amount 0) → textureLive_ stays
            // false → the texture block never runs → the drifted converter output is passed through
            // bit-for-bit (the no-op keystone + the committed grit/filter golden are unchanged).
            noise_.prepare (sampleRate);
            noise_.setLevel (0.0f);
            reson_.prepare (sampleRate);
            reson_.setAmount (0.0f);
            textureLive_ = false;
            // [T-CGUI-08] The stage bypass fades: 10 ms, the machine's own control-ramp time.
            textureFade_.prepare (sampleRate);
            gritFade_.prepare (sampleRate);
            routeFade_.prepare (sampleRate); // [D-M-0512] the noise route's crossfade, seated on the route
            routeFade_.set (noiseRoute_ == NoiseRoute::Parallel);
            routeFade_.settle();
            // [T-CGUI-09] The GRIT dry ring: power-of-two, masked (the FX aligner's shape), capped at
            // `kGritDryLimitMs` — the `+ 1` holds the sample written beside the oldest one read.
            gritDryLimit_ = std::ceil (kGritDryLimitMs * 0.001 * sampleRate);
            std::size_t cap = 2;
            while (static_cast<Real> (cap) < gritDryLimit_ + 2.0)
                cap <<= 1;
            gritDry_.assign (cap, 0.0f);
            if (right_ != nullptr) // FIREFORK P3
                right_->gritDry.assign (cap, 0.0f); // [stereo voice S3] sized alike: the start copies, never grows
            gritDryMask_ = cap - 1;
            gritDryPos_ = 0;
            gritDelaySlew_ = 1.0 - std::exp (-1.0 / (kGritDelaySlewSeconds * sampleRate));
            filterFade_.prepare (sampleRate);
            foldFade_.prepare (sampleRate); // [T-CGUI-64] the Pre/Post switch fade, 10 ms
            f2Fade_.prepare (sampleRate); // [row 317] F2's on/route crossfade, 10 ms
            ceiling_.prepare (sampleRate); // [D-M-0511 / D-M-0373] the stage ceiling's detectors
            ceilingTailRise_ = static_cast<float> (1.0 / (kCeilingTailSeconds * sampleRate)); // [S5R B31 dsp2 F1]
            balanceRampLen_ = std::max (1, static_cast<int> (sampleRate * 0.010));
            settleFilter2Topology();
        }

        // Clears the oscillator phase(s), the converter-chain state (reducer hold, loop-error ring,
        // drift buffer + RESEEDED modulators), the filter-drive state (ADAA memory + oversampler FIR),
        // the filter integrator state AND all model cores (the Ladder integrators; the Scream and Acid
        // integrators + feedback estimate + OS FIR). The grit has no state. RT-safe.
        void reset() noexcept
        {
            oscTap_ = 0.0f;
            textureTap_ = 0.0f;
            osc_.reset();
            srr_.reset();
            loopErr_.reset();
            drift_.reset();
            resetFilterStage(); // F1 + F2: the drive, the self-osc limiter, every core, the aligners
            // ⚠ [S5R round 2] `selfOscBlend_` IS DELIBERATELY *NOT* CLEARED HERE, and round 1 got this
            // wrong in the other direction. It is a DERIVED CONTROL-RATE PARAMETER, like `filterMode_`
            // and `appliedModelReso01_`, none of which this function touches — `reset()` clears AUDIO
            // STATE. `CathodeMachine::prepareToPlay` runs `applyAll(); engine.reset();` IN THAT ORDER,
            // so clearing it here wiped the value `applyAll` had just computed and left the limiter
            // OUT OF CIRCUIT on every session open — the pre-card +8.7 dBFS, i.e. exactly the defect
            // this card exists to remove. `CathodeVoiceEngine::reset` documents the same hazard class
            // for LFO phase and the channel controls (T-CMP-21 leaf C).
            // The stale-blend bug round 1 found is closed by the do-nothing arm alone: every mode
            // change goes through `applyFilterFamily`, which ALWAYS ends in `setPatchCutoff` — no
            // change-gate, all `kMaxVoices` — so that arm always runs.
            hpf_.reset();
            // §5 texture: reseed the noise stream (deterministic re-render) and clear the resonator ring.
            noise_.reset();
            if (right_ != nullptr)
                right_->noiseSide.reset(); // [S5] the side stream re-renders too
            reson_.reset();
            // [T-CGUI-08] The fade GAIN is audio state and snaps to its target; the bypass TARGET is a
            // control parameter and survives (the `selfOscBlend_` rule above).
            textureFade_.settle();
            gritFade_.settle();
            filterFade_.settle();
            routeFade_.settle(); // [D-M-0512]
            std::fill (gritDry_.begin(), gritDry_.end(), 0.0f); // [T-CGUI-09] the dry leg is audio state
            gritDryPos_ = 0;
            gritDryDelay_ = gritLatencySamples(); // a fresh voice starts aligned
            refreshTextureLive();
            // [T-CGUI-64] The fold's ADAA memory is audio state; a pending Pre/Post move lands at once.
            fold_.reset();
            foldR_.reset();
            settleFoldMove();
            stereoHold_ = 0; // [stereo voice S2] nothing has sounded: the right chain starts again from the left
            split_ = false;
            rightRun_ = false; // [stereo voice S3] …and so does the upstream one (a copy, at its next start)
            noiseWidth_ = noiseWidthTarget_; // [S5R 2] nothing sounds: the noise width lands
            rightMix_ = 0.0f;
        }

        // ---- Oscillator core (delegated; §1 osc, §8 tuning, §9 synth.osc*) ----
        void setOscMode (int slot, OscMode mode) noexcept { osc_.setMode (slot, mode); }
        // [T-CGUI-58 leaf 2] the per-oscillator amp envelopes (see `CathodeOscCore::oscEnv`).
        [[nodiscard]] AdsrEnvelope& oscEnv (int slot) noexcept { return osc_.oscEnv (slot); }
        void setOscEnvOn (int slot, bool on) noexcept { osc_.setOscEnvOn (slot, on); }
        [[nodiscard]] bool oscEnvOn (int slot) const noexcept { return osc_.oscEnvOn (slot); }
        void oscEnvGateOn (bool restrike, bool fromSilence) noexcept
        {
            osc_.oscEnvGateOn (restrike, fromSilence);
            if (fromSilence)
            {
                noiseWidth_ = noiseWidthTarget_; // [S5R 2] the voice is silent: the noise width lands
                settleFadesFromSilence();
            }
        }
        // [D-M-0574 review 2] …and so does every STAGE fade still under way, with its landing action: a note from
        // silence has nothing to crossfade from. Without this an idle-SKIPPED voice (which ticks no fade) opened its
        // next note on a fade frozen mid-way — an un-bypassed FILTER faded in over 10 ms under the attack — where a
        // rendering idle voice had long landed it. Settled, every line is a no-op.
        void settleFadesFromSilence() noexcept
        {
            filterFade_.settle(); // parked: `resetFilterState` (the same gate) already cleared the stage
            gritFade_.settle();
            routeFade_.settle();
            if (textureFade_.fading())
            {
                textureFade_.settle();
                if (textureFade_.parked()) // the landed-out action: no stale ring on a later un-bypass
                {
                    reson_.reset();
                    if (right_ != nullptr)
                        right_->reson.reset();
                }
                refreshTextureLive();
            }
            if (foldFade_.fading())
                settleFoldMove();
        }
        void oscEnvGateOff() noexcept { osc_.oscEnvGateOff(); }
        void silenceOscEnvelopes() noexcept { osc_.silenceOscEnvelopes(); } // [b39 review 1] every per-osc env to Idle
        void setWavetable (int slot, const Wavetable& wt) noexcept { osc_.setWavetable (slot, wt); }
        void setPosition (int slot, float pos) noexcept { osc_.setPosition (slot, pos); }
        void setPdShape (int slot, float shape) noexcept { osc_.setPdShape (slot, shape); }
        void setPdAmount (int slot, float amount) noexcept { osc_.setPdAmount (slot, amount); }
        // [T-CGUI-10 leaf B] `synth.osc{1,2}.pdType` — forwarded to the slot's PD oscillator.
        void setPdType (int slot, PhaseDistortionOscillator::PdType type) noexcept { osc_.setPdType (slot, type); }
        // [T-CMP-02] The SUB (`synth.sub.{wave,level}`) — a fourth generator, not a third slot, so
        // it takes no `slot` argument. It sums into the osc mix, which is what puts it inside
        // `synth.hpf` and before the broken converter (plan gate R2); see `processSample` below.
        // [T-CMP-15 leaf C] The PULSE mode's pass-throughs, the sub's three lines one generator over.
        // The table is bound for BOTH slots at once (the core's setter loops) because it is one
        // shared saw pyramid, unlike a WT slot's per-slot bank; the two knobs are per slot, because
        // the whole point of R2's two dests is osc1 and osc2 breathing independently.
        void setPulseWavetable (const Wavetable& wt) noexcept { osc_.setPulseWavetable (wt); }
        void setPulseWidth (int slot, Real duty) noexcept { osc_.setPulseWidth (slot, duty); }
        void setPulseSawMix (int slot, float mix) noexcept { osc_.setPulseSawMix (slot, mix); }
        void setScanOrder (int slot, ScanOrder order) noexcept { osc_.setScanOrder (slot, order); } // [T-PROTO-02 leaf 4]

        void setSubWavetable (const Wavetable& wt) noexcept { osc_.setSubWavetable (wt); }
        void setSubWave (SubOscillator::Wave wave) noexcept { osc_.setSubWave (wave); }
        void setSubLevel (float level) noexcept { osc_.setSubLevel (level); }
        void setSubMode (CathodeOscCore::SubMode mode) noexcept { osc_.setSubMode (mode); } // [T-CGUI-57]
        void setSubOctave (int octaves) noexcept { osc_.setSubOctave (octaves); } // [T-CGUI-57]
        // [T-MASK-03 leaf D] The BITMASK mode's two controls (ADR-037): the mask NUMBER (0..511,
        // wrapped at the primitive) and ruling 1's per-slot clean/dirty switch.
        void setMask (int slot, int mask) noexcept { osc_.setMask (slot, mask); }
        void setMaskOversamplingBypassed (int slot, bool bypassed) noexcept
        {
            osc_.setMaskOversamplingBypassed (slot, bypassed);
        }
        // [T-MASK-03 leaf D] The note-on hook — commits each Bitmask slot's armed pitch-adaptive
        // factor and touches nothing else (no phase reset; the slots stay free-running). Called from
        // `CathodeModulatedVoice::gateOn`, on the audio thread.
        void commitPendingMaskFactors() noexcept { osc_.commitPendingMaskFactors(); }
        void strikeMacros() noexcept { osc_.strikeMacros(); } // [T-CGUI-122] see CathodeOscCore
        void setMacroEngine (int slot, MacroOscillator::Engine engine) noexcept { osc_.setMacroEngine (slot, engine); }
        void setMacroSeed (int slot, std::uint32_t seed) noexcept { osc_.setMacroSeed (slot, seed); } // [T-CGUI-44]
        void setMacroHarmonics (int slot, float value) noexcept { osc_.setMacroHarmonics (slot, value); }
        void setMacroTimbre (int slot, float value) noexcept { osc_.setMacroTimbre (slot, value); }
        void setMacroMorph (int slot, float value) noexcept { osc_.setMacroMorph (slot, value); }
        void setOctave (int slot, int octaves) noexcept { osc_.setOctave (slot, octaves); }
        void setSemitones (int slot, int semis) noexcept { osc_.setSemitones (slot, semis); }
        void setFine (int slot, Wide cents) noexcept { osc_.setFine (slot, cents); }
        void setLevel (int slot, float level) noexcept { osc_.setLevel (slot, level); }
        // [T-CGUI-63] OSC UNISON — `CathodeOscCore`'s per-slot stack (OSC 1/2 only; SUB ignores it).
        void setUnisonVoices (int slot, int voices) noexcept { osc_.setUnisonVoices (slot, voices); }
        void setUnisonDetune (int slot, float amount) noexcept { osc_.setUnisonDetune (slot, amount); }
        void setUnisonBlend (int slot, float blend) noexcept { osc_.setUnisonBlend (slot, blend); }
        // [stereo voice S4] `synth.osc{1,2}.uni.width` — the stack's members spread L/R (0 = mono, bit for bit).
        void setUnisonWidth (int slot, float width) noexcept { osc_.setUnisonWidth (slot, width); }
        [[nodiscard]] float unisonWidth (int slot) const noexcept { return osc_.unisonWidth (slot); }
        // The played note in Hz — fanned out to the oscillator PITCH, the reducer's KEY-TRACK, and
        // the loop-error PERIOD simultaneously (§0/§2/§8). Control-rate.
        void setNoteHz (Wide hz) noexcept
        {
            osc_.setNoteHz (hz);
            srr_.setNoteHz (static_cast<float> (hz));
            loopErr_.setNoteHz (static_cast<float> (hz));
            editRight ([hz] (RightChain& r) noexcept {
                r.srr.setNoteHz (static_cast<float> (hz));
                r.loopErr.setNoteHz (static_cast<float> (hz));
            });
        }
        // [T-CGUI-80] `synth.osc2.modOnly` — OSC 2 leaves the sum, still modulates (see the core).
        void setOsc2ModOnly (bool on) noexcept { osc_.setOsc2ModOnly (on); }
        void setOsc2SumGain (float gain) noexcept { osc_.setOsc2SumGain (gain); } // [T-CGUI-83] the faded form

        // [T-CMP-03 leaf 1] `synth.xmod.ring` — the osc2 -> osc1 ring depth. 0 is a bit-exact
        // bypass (the core skips the path entirely), so this is safe to push unconditionally.
        // [T-CMP-04 leaf 2] Hard-sync amount, 0 = bypass. Unlike the two cross-mod depths beside
        // it, this row HAS a matrix destination (`Dest::Sync`), so the app layer must write the
        // VOICE's base and not this forwarder — see CathodeModulatedVoice::setBaseSyncAmount.
        void setSyncAmount (float amount) noexcept { osc_.setSyncAmount (amount); }
        void setXmodRing (float depth) noexcept { osc_.setXmodRing (depth); }
        [[nodiscard]] float ringLegGain() const noexcept { return osc_.ringLegGain(); } // [D-M-0574 review 3]
        [[nodiscard]] float fmLegGain() const noexcept { return osc_.fmLegGain(); }
        [[nodiscard]] float xmodRing() const noexcept { return osc_.xmodRing(); }

        // [T-CMP-03 leaf 2e] `synth.xmod.fm`. 0 is a bit-exact bypass, so this is safe to push
        // unconditionally, exactly like the ring above.
        void setXmodFm (float depth) noexcept { osc_.setXmodFm (depth); }
        [[nodiscard]] float xmodFm() const noexcept { return osc_.xmodFm(); }

        // ---- Broken converter (§2; §9 synth.conv.{rate,track,bits,compand,interp,loopErr,drift}) ----
        // Native converter rate in Hz (`rate`); at or above the host rate the reducer is a bit-exact
        // bypass (applying only the capture-time grit). Control-rate.
        void setConverterRate (float rateHz) noexcept
        {
            srr_.setDeviceRateHz (rateHz);
            editRight ([rateHz] (RightChain& r) noexcept { r.srr.setDeviceRateHz (rateHz); });
        }
        // Key-tracking amount in [0,1] (`track`): 0 = pitch-independent rate, 1 = full varispeed
        // (octave up doubles the effective rate). Control-rate.
        void setKeyTrack (float amount) noexcept
        {
            srr_.setKeyTrack (amount);
            editRight ([amount] (RightChain& r) noexcept { r.srr.setKeyTrack (amount); });
        }
        // Storage word length in bits (`bits`, fractional allowed); control-rate, clamped to [1, 24].
        void setBitDepth (float bits) noexcept { grit_.setBitDepth (bits); }
        // μ-law companding amount in [0,1] (`compand`; 0 = linear grain, 1 = full μ=255). Control-rate.
        void setCompanding (float amount) noexcept { grit_.setCompanding (amount); }
        // Reconstruction mode (`interp`): ZeroOrderHold (staircase imaging) | Linear (softer).
        void setInterpolation (Interp mode) noexcept
        {
            srr_.setInterpolation (mode);
            editRight ([mode] (RightChain& r) noexcept { r.srr.setInterpolation (mode); });
        }
        // Loop-point error amount in [0,1] (`loopErr`); 0 = bit-exact bypass. Control-rate.
        void setLoopError (float amount) noexcept
        {
            loopErr_.setLoopErr (amount);
            editRight ([amount] (RightChain& r) noexcept { r.loopErr.setLoopErr (amount); });
        }
        // Drift/jitter macro in [0,1] (`drift`); 0 = bit-exact bypass. Control-rate.
        void setDrift (float amount) noexcept
        {
            drift_.setDrift (amount);
            editRight ([amount] (RightChain& r) noexcept { r.drift.setDrift (amount); });
        }
        // Base RNG seed for the (deterministic) drift modulators — drives §6 per-voice variation.
        // Reseeds and clears drift state.
        void setDriftSeed (std::uint32_t seed) noexcept
        {
            drift_.setSeed (seed);
            editRight ([seed] (RightChain& r) noexcept { r.drift.setSeed (seed); });
        }

        // ---- Filter (§3; §9 synth.filt.*) — forwarded to F1 (CathodeFilterBank, T-CGUI-60) ----
        void setFilterMode (FilterMode mode) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setFilterMode (mode); });
        }
        [[nodiscard]] std::uint32_t filterModeWrites() const noexcept { return f1_.filterModeWrites(); } // FIREFORK E17
        void setCutoff (float cutoffHz, float q) noexcept
        {
            editFilter1Coefficients ([&] (CathodeFilterBank& b) noexcept { b.setCutoff (cutoffHz, q); });
        }

        // [T-CMP-01] `synth.hpf`: a FIXED, NON-RESONANT series high-pass. `cutoffHz <= 0` is the
        // row's v = 0 off-state and is a bit-exact bypass (see processSample). Q is fixed at
        // Butterworth — the flattest response with no peak anywhere, which is what "non-resonant"
        // requires — and is its OWN constant rather than a reuse of kDefaultQ, so retuning the
        // main filter's default cannot silently put a resonant bump in the HPF.
        void setHpf (float cutoffHz) noexcept
        {
            hpfLive_ = cutoffHz > 0.0f;
            if (hpfLive_)
            {
                hpf_.setCutoff (cutoffHz, kHpfQ);
                editRight ([cutoffHz] (RightChain& r) noexcept { r.hpf.setCutoff (cutoffHz, kHpfQ); });
            }
        }

        // Exposed for the keystone case, the same way resonExciteWeights() is: a test that
        // hand-composes this chain must configure its own TptSvf identically or it is proving
        // nothing about the shipped one.
        [[nodiscard]] static constexpr float hpfQ() noexcept { return kHpfQ; }

        // [T-CGUI-64] `synth.fold.amount` in [0, 1] (`specs/dsp/wavefolder.md` §0.3) — a matrix
        // destination, so this runs per sample while routed: a clamp and a store. 0 is a bit-exact
        // bypass: `foldLive_` is false and the fold is not called at either position — and its ADAA
        // memory is forgotten, so re-entry is re-primed (§0) rather than divided against a stale u[n−1].
        void setFoldAmount (float amount) noexcept
        {
            foldAmount_ = std::clamp (amount, 0.0f, 1.0f);
            foldLive_ = foldAmount_ > 0.0f;
            if (!foldLive_)
            {
                fold_.reset();
                foldR_.reset();
                settleFoldMove(); // silent: a pending move has nothing to fade
            }
        }
        [[nodiscard]] float foldAmount() const noexcept { return foldAmount_; }
        // [T-CGUI-64] `synth.fold.pos`. While the fold is audible a move FADES: the wet mix runs out
        // over 10 ms at the old position, the stage moves and re-primes, and the mix runs back in — the
        // dry signal is continuous throughout, so a mid-note switch is a dip in fold, never a step.
        // `fade = false` moves at once — for a SILENT voice (the machine passes `active()`), whose
        // pending move would otherwise wait for a sample it may never render.
        void setFoldPosition (FoldPosition pos, bool fade = true) noexcept
        {
            foldTarget_ = pos;
            if (!foldLive_ || !fade)
            {
                settleFoldMove();
                return;
            }
            foldFade_.set (foldTarget_ != foldPos_); // out toward a move, back in if it is cancelled
        }
        // Where the fold runs NOW (a fading move reports the old position until it lands).
        [[nodiscard]] FoldPosition foldPosition() const noexcept { return foldPos_; }
        [[nodiscard]] float foldMoveGain() const noexcept { return foldFade_.gain(); }
        // The vowel is F1's row and applies to BOTH filters (owner 2026-09-27) — F2 reads it in Formant.
        void setVowel (float position) noexcept
        {
            editFilter1 ([position] (CathodeFilterBank& b) noexcept { b.setVowel (position); });
            editFilter2 ([position] (CathodeFilterBank& b) noexcept { b.setVowel (position); });
        }
        void setFormantQ (float q) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setFormantQ (q); });
        }
        void setCombHz (float hz) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setCombHz (hz); });
        }
        void setCombFeedback (float g) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setCombFeedback (g); });
        }
        void setCombCutoffReso (float cutoffHz, float q, bool resoModulated) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setCombCutoffReso (cutoffHz, q, resoModulated); });
        }
        void setCombType (CombType type) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setCombType (type); });
        }
        void setLadderCutoff (float cutoffHz, float resonance) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setLadderCutoff (cutoffHz, resonance); });
        }
        void setScreamCutoff (float cutoffHz, float resonance) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setScreamCutoff (cutoffHz, resonance); });
        }
        void setScreamDrive (float amount) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setScreamDrive (amount); });
        }
        void setAcidCutoff (float cutoffHz, float resonance) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setAcidCutoff (cutoffHz, resonance); });
        }
        void setAcidDrive (float amount) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setAcidDrive (amount); });
        }
        void setSovietCutoff (float cutoffHz, float resonance) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setSovietCutoff (cutoffHz, resonance); });
        }
        void setSovietDrive (float amount) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setSovietDrive (amount); });
        }
        void setClassicCutoff (float cutoffHz, float resonance) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setClassicCutoff (cutoffHz, resonance); });
        }
        void setClassicDrive (float amount) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setClassicDrive (amount); });
        }
        void setReedCutoff (float cutoffHz, float resonance) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setReedCutoff (cutoffHz, resonance); });
        }
        void setReedDrive (float amount) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setReedDrive (amount); });
        }
        void setReedBlend (float blend) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setReedBlend (blend); });
        }
        void setStateCutoff (float cutoffHz, float resonance) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setStateCutoff (cutoffHz, resonance); });
        }
        void setStateMorph (float blend) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setStateMorph (blend); });
        }
        // [T-CMP-26] THE ONE DISPATCH of cutoff + resonance to the live model core — see the bank.
        void setModelCutoffReso (float cutoffHz, float reso01) noexcept
        {
            editFilter1Coefficients ([&] (CathodeFilterBank& b) noexcept { b.setModelCutoffReso (cutoffHz, reso01); });
        }
        [[nodiscard]] float appliedModelResonance01() const noexcept { return f1_.appliedModelResonance01(); }
        [[nodiscard]] float appliedModelCutoffHz() const noexcept { return f1_.appliedModelCutoffHz(); }
        [[nodiscard]] float selfOscBlend() const noexcept { return f1_.selfOscBlend(); }
        void setDrive (float amount) noexcept
        {
            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setDrive (amount); });
        }
        // The drive position is F1's row and applies to BOTH filters (owner 2026-09-27).
        void setDrivePlacement (DrivePlacement placement) noexcept
        {
            editFilter1 ([placement] (CathodeFilterBank& b) noexcept { b.setDrivePlacement (placement); });
            editFilter2 ([placement] (CathodeFilterBank& b) noexcept { b.setDrivePlacement (placement); });
        }

        // ---- [T-CGUI-60 leaf 2] F2 — the second filter (synth.filt2.*, synth.filt.{route,balance}) ----
        // OPT-IN (owner ruling 3, 2026-09-07): while Off F2 is never processed, so the stage is the
        // single-filter stage bit for bit and costs nothing. F2 owns its model·type·cutoff·reso·drive;
        // the vowel, the morph and the drive position are F1's rows and reach both (the morph is
        // set per bank, after its mode latch, by the caller).
        // [stereo voice S3] THE FORWARDING SEAM: every write to a bank goes through `editFilter{1,2}`, so it
        // reaches the RIGHT channel's twin too while that chain runs (`rightChainRunning`) — any bank setter,
        // present or future, without a forwarder per setter. While the right chain is idle only the left is
        // written: the twin takes the whole left bank, settings and state, when it starts.
        template <typename Fn>
        void editFilter1 (Fn&& fn) noexcept
        {
            fn (f1_);
            if (rightRun_)
            {
                fn (right_->f1);
                if constexpr (fork::kExact)
                    right_->f1.refreshCombRingFrom (f1_); // FIREFORK B5
            }
        }
        template <typename Fn>
        void editFilter2 (Fn&& fn) noexcept
        {
            fn (f2_);
            if (rightRun_)
            {
                fn (right_->f2);
                if constexpr (fork::kExact)
                    right_->f2.refreshCombRingFrom (f2_); // FIREFORK B5
            }
        }

        // FIREFORK E4: editFilter* for the cutoff setters: the right twin copies the coefficients the left bank just computed.
        template <typename Fn>
        void editFilter1Coefficients (Fn&& fn) noexcept
        {
            fn (f1_);
            if (rightRun_)
            {
                if (fork::kExact && f1_.coefficientsCopyable())
                    right_->f1.copyCoefficientsFrom (f1_);
                else
                    fn (right_->f1);
            }
        }
        template <typename Fn>
        void editFilter2Coefficients (Fn&& fn) noexcept
        {
            fn (f2_);
            if (rightRun_)
            {
                if (fork::kExact && f2_.coefficientsCopyable())
                    right_->f2.copyCoefficientsFrom (f2_);
                else
                    fn (right_->f2);
            }
        }

    private:
        struct RightChain;
        // [stereo voice S3] The upstream twins' half of the seam: a setter writes its left stage, then this.
        template <typename Fn>
        void editRight (Fn&& fn) noexcept
        {
            if (rightRun_)
                fn (*right_);
        }

    public:
        // The LEFT banks — read-backs, and writes for a stage whose right chain never runs (tests, the
        // FILTER figure's private stage). A write here does not reach the right twin: use `editFilter*`.
        [[nodiscard]] CathodeFilterBank& filter1() noexcept { return f1_; }
        [[nodiscard]] const CathodeFilterBank& filter1() const noexcept { return f1_; }
        [[nodiscard]] CathodeFilterBank& filter2() noexcept { return f2_; }
        [[nodiscard]] const CathodeFilterBank& filter2() const noexcept { return f2_; }
        // [stereo voice S3] A TEST SEAM, not a host param: runs the right chain on every sample, fed what the
        // left is fed, so a test can hold the two chains side by side (L == R, bit for bit, unless a write
        // missed one of them). Off (the default) the right chain runs only while the source is stereo.
        void forceRightChain (bool on) noexcept { forceRight_ = on; }
        [[nodiscard]] bool rightChainRunning() const noexcept { return rightRun_; }
        // [row 317] F2's on/route are REQUESTS: the sounding topology follows them through a 10 ms
        // crossfade on F2's contribution — out of the old topology to F1 alone, switch, in to the new one
        // (the synth.fold.pos move shape), so F2 never has to serve two inputs in one sample. At the
        // switch F2 and its aligner start CLEAN [T-CGUI-77]: what they hold was written for another
        // signal, or not at all. F1's aligner is NOT cleared: F1 always runs, so it is kept warm for one
        // store a sample (`filterStage`).
        // [T-CGUI-79 A3] ANY THREAD, request only: the setters store an atomic; `applyFilter2Edge` latches it
        // at the top of the next `filterStage`, and the clean-up runs there on the audio thread that owns F2's
        // state. The getters report the request (the patch state).
        void setFilter2On (bool on) noexcept
        {
            f2OnReq_.store (on, std::memory_order_relaxed);
            filterReqDirty_.store (true, std::memory_order_release); // FIREFORK E9
        }
        [[nodiscard]] bool filter2On() const noexcept { return f2OnReq_.load (std::memory_order_relaxed); }
        void setFilterRoute (FilterRoute route) noexcept
        {
            routeReq_.store (route, std::memory_order_relaxed);
            filterReqDirty_.store (true, std::memory_order_release); // FIREFORK E9
        }
        [[nodiscard]] FilterRoute filterRoute() const noexcept { return routeReq_.load (std::memory_order_relaxed); }
        // [row 317] The balance gains RAMP linearly to their target over 10 ms; a settled ramp holds the
        // target gains exactly.
        // [T-CGUI-79 S5R-7] ANY THREAD, request only, like F2's on/route: `synth.filt.balance` is a generic
        // destination whose base write-through runs on the message thread while the audio thread ticks the
        // ramp. The setter stores ONE atomic; `applyFilter2Edge` latches it and computes the target and step
        // on the audio thread, from the gains as they ARE. The getter reports the request.
        // [T-CGUI-81] A NaN request stores the CENTRE: NaN passes the clamp, and `latchBalanceRequest`'s
        // "same as latched" compare is never true for it, so it would restart the ramp every sample.
        void setFilterBalance (float balance) noexcept
        {
            const float b = std::isnan (balance) ? 0.0f : balance;
            balanceReq_.store (b < -1.0f ? -1.0f : (b > 1.0f ? 1.0f : b), std::memory_order_relaxed);
            filterReqDirty_.store (true, std::memory_order_release); // FIREFORK E9
        }
        [[nodiscard]] float filterBalance() const noexcept { return balanceReq_.load (std::memory_order_relaxed); }
        // [T-CGUI-79 S5R-7] The gains applied this sample — a test read-back, on the thread that renders.
        [[nodiscard]] BalanceGains appliedBalanceGains() const noexcept { return gains_; }
        // F2's cutoff + Q through the bank's whole-dispatch (see `CathodeFilterBank::setCutoffQ`).
        void setFilter2CutoffReso (float cutoffHz, float q) noexcept
        {
            editFilter2Coefficients ([&] (CathodeFilterBank& b) noexcept { b.setCutoffQ (cutoffHz, q); });
        }
        void setFilter2Drive (float amount) noexcept
        {
            editFilter2 ([&] (CathodeFilterBank& b) noexcept { b.setDrive (amount); });
        }
        // The whole FILTER stage's through-path latency: F1 alone while F2 is Off (the historic sum,
        // bit for bit), both in Series, the LONGER of the two in Parallel (the shorter is aligned).
        [[nodiscard]] int filterLatencySamples() const noexcept
        {
            const int l1 = f1_.latencySamples();
            if (!filter2On())
                return l1;
            const int l2 = f2_.latencySamples();
            return filterRoute() == FilterRoute::Series ? l1 + l2 : std::max (l1, l2);
        }

        // ---- §5 Noise / texture layer (§9 synth.noise.{type,level}, synth.reson.{tune,amount}) ----
        // A per-voice texture source mixed PRE-FILTER (CATHODE.md §0/§5): the noise generator + a
        // metallic resonator bank excited by the osc mix + the noise. Summed into the converter output
        // (a clean parallel path, NOT through the grit) before the pre-filter drive. Both primitives
        // default OFF (noise level 0, resonator amount 0) so the layer is a bit-exact bypass until
        // dialed — `textureLive_` gates the per-sample block, so an untouched texture never perturbs
        // the drifted signal (and never advances the noise RNG). Control-rate.
        void setNoiseType (NoiseType type) noexcept
        {
            noise_.setType (type);
            noiseType_ = type; // [S5R 6] kept for a side stream prepare() has yet to make
            if (right_ != nullptr)
                right_->noiseSide.setType (type); // [S5] the side stream is the same colour
        }
        // Linear noise level [0,1]; 0 (with resonator amount 0) → the texture layer bypasses bit-exact.
        void setNoiseLevel (float level) noexcept
        {
            noise_.setLevel (level);
            if (right_ != nullptr)
                right_->noiseSide.setLevel (level); // [S5]
            refreshTextureLive();
        }
        // [T-CGUI-08] `synth.noise.bypass` — routes the WHOLE texture layer out (noise AND resonator,
        // which rings from the osc mix too, so `level 0` alone is not a bypass) with a linear fade
        // over `textureFadeLen_`, keeping every parameter. Once faded out the resonator is cleared,
        // so an un-bypass does not resume a stale ring, and the block stops running: bit-exact with
        // a cleared layer. Not bypassed and not fading, the sum is the built expression bit-for-bit.
        void setTextureBypassed (bool bypassed) noexcept
        {
            if (textureFade_.set (bypassed))
                refreshTextureLive();
        }
        // [D-M-0509] The noise ROUTE (see `NoiseRoute`). Through moves only the DIRECT noise term: the
        // resonator still rings from `oscMix + noiseOut` and sums at the pre-filter node either way, and
        // the texture fade scales the injected noise as it scales the summed one. The GRIT stage owns
        // what it is fed, so with GRIT bypassed (or amount 0) the noise rides its latency-matched DRY
        // leg — clean, i.e. Through degrades to Parallel (delayed by the chain's latency). [D-M-0512] A
        // switch while the noise sounds CROSSFADES over 10 ms (`BypassFade`), equal-gain: the injected share
        // `w` and the summed share `1 − w` — an instant switch left a hole (or a doubling) one converter
        // latency long. `fade = false` (a SILENT voice: the machine passes `active()`) or a layer that is not
        // live switches at once. Settled, either route is its own expression bit for bit. Control-rate.
        void setNoiseRoute (NoiseRoute route, bool fade = true) noexcept
        {
            noiseRoute_ = route;
            routeFade_.set (route == NoiseRoute::Parallel); // "bypassed" = Parallel: the gain IS the Through share
            if (!fade || !textureLive_)
                routeFade_.settle();
        }
        [[nodiscard]] NoiseRoute noiseRoute() const noexcept { return noiseRoute_; }
        // The Through share in force: 0 Parallel, 1 Through, between them mid-switch (D-M-0512).
        [[nodiscard]] float noiseRouteMix() const noexcept { return routeFade_.gain(); }
        [[nodiscard]] bool textureBypassed() const noexcept { return textureFade_.bypassed(); }
        [[nodiscard]] float textureGain() const noexcept { return textureFade_.gain(); }
        // [T-CGUI-08] `synth.grit.bypass` — routes the broken-converter chain (rate reducer, bits,
        // loop error, drift) out: the output fades from the chain's to its own input (post-HPF — the
        // HPF is not part of GRIT, owner 2026-09-23) and every parameter is kept. ⚠ THE CHAIN KEEPS
        // RUNNING WHILE BYPASSED, unlike NOISE and FILTER, which park: its drift line delays ~62
        // samples, so a cleared line would read silence on un-bypass and then step in mid-fade — a
        // click. Kept warm, the un-bypass fades between two live signals. [T-CGUI-09] The dry leg it
        // fades to is the LATENCY-MATCHED one `synth.grit.amount` blends against (see gritDryLeg), so
        // the fade no longer combs mid-way when the chain runs late. The texture layer sums in AFTER
        // this, so a GRIT bypass never touches noise or the resonator.
        void setGritBypassed (bool bypassed) noexcept { gritFade_.set (bypassed); }
        // [T-CGUI-09] `synth.grit.amount` (R40) — a LINEAR dry/wet blend of the converter chain
        // against its own input DELAYED TO THE CHAIN'S NOMINAL LATENCY: 1 = the chain alone (the
        // shipped sound, bit for bit), 0 = the clean leg alone, 0.5 = PARALLEL GRIT. An undelayed dry
        // leg would comb at every partial mix — invisible at 0 and 1, audible in between.
        void setGritAmount (float amount) noexcept { gritAmount_ = std::clamp (amount, 0.0f, 1.0f); }
        [[nodiscard]] float gritAmount() const noexcept { return gritAmount_; }
        // The chain's nominal latency in samples (capped at 50 ms) — the TARGET the dry leg glides to.
        [[nodiscard]] Real gritLatencySamples() const noexcept
        {
            return std::min (srr_.nominalDelaySamples() + loopErr_.nominalDelaySamples() + drift_.nominalDelaySamples(), gritDryLimit_);
        }
        // …and where the dry leg actually reads now: that target, glided (see processSample).
        [[nodiscard]] Real gritDryDelaySamples() const noexcept { return gritDryDelay_; }
        // [T-CGUI-68 leg 1] A note from IDLE has nothing to glide from — its amp starts at zero — so
        // the dry leg lands on this note's (key-tracked) latency at once. The voice calls it at gate-on.
        void snapGritDryDelay() noexcept { gritDryDelay_ = gritLatencySamples(); }
        // [B34] A note from IDLE starts both filter stages from silence (`resetFilterStage`: the cores, drive, self-osc
        // limiter, resonance ceilings, aligners). An idle voice keeps running its tone path through the silence, so its
        // filters sat in whatever state the LAST note's tuning (or, never played, the base tuning) left: a ceiling-held
        // Comb ("Comb Snap") opened 0.17 on a never-played voice and 0.58 on a reused one, x3.4 at equal velocity, and a
        // loaded patch's note opened on the previous patch's filter ring (T-CGUI-81). Silent voice: clearing cannot click.
        void resetFilterState() noexcept { resetFilterStage(); }
        // [B34, owner 2026-10-13] …and by the same rule the NOISE RESONATOR's ring and the HPF's memory: an idle voice
        // drives them on from its free-running osc (or freezes them mid-ring when the engine skips it), so a reused
        // voice's note opened on the last note's state — Comb Snap's defect class. Not in `resetFilterStage`: neither
        // is part of the FILTER bypass (the HPF by owner ruling 2026-09-23), which also calls that.
        void resetIdleRings() noexcept
        {
            for (const bool right : { false, true })
            {
                resetResonRing (right);
                resetHpfState (right);
            }
        }
        // …one at a time, left or the right chain's twin (a no-op without one): the tests that pin WHICH state a
        // gate keeps clear exactly one.
        void resetResonRing (bool right) noexcept
        {
            if (!right)
                reson_.reset();
            else if (right_ != nullptr)
                right_->reson.reset();
        }
        void resetHpfState (bool right) noexcept
        {
            if (!right)
                hpf_.reset();
            else if (right_ != nullptr)
                right_->hpf.reset();
        }
        [[nodiscard]] bool gritBypassed() const noexcept { return gritFade_.bypassed(); }
        [[nodiscard]] float gritGain() const noexcept { return gritFade_.gain(); }
        // [T-CGUI-08] `synth.filt.bypass` — routes the whole filter stage out: pre-drive, the model
        // core, post-drive and the self-osc limiter, from the pre-filter node (texture and aux
        // included) to the stage output. Every parameter is kept. Once faded out the stage stops
        // running and its state is cleared, so an un-bypass never resumes a stale ring. The dry leg
        // is NOT latency-matched: an oversampled core runs 16-32 samples late, and over a 10 ms fade
        // that is a brief comb, not a click (the card accepts that engaging moves latency and phase).
        void setFilterBypassed (bool bypassed) noexcept { filterFade_.set (bypassed); }
        // [T-CGUI-71] The FILTER stage ALONE — `filterStage` exactly as `processSample` runs it — for
        // `core::FilterPreview`'s impulse measurement on its own, private stage. Never called on a
        // voice that is sounding.
        // Mono: a split pair (Stereo) folds to L + R — the two filters as routed, the Parallel sum.
        float processFilterOnly (float x) noexcept
        {
            const StereoSample y = filterStageWhole (x, x);
            return split_ ? y.left + y.right : y.left;
        }
        // [stereo voice S2] The same, as the L/R pair the voice carries.
        StereoSample processFilterOnlyStereo (float x) noexcept { return filterStageWhole (x, x); }
        // [stereo voice S2] Whether the RIGHT channel ran its own chain for the last sample: from the first
        // split sample (Stereo sounding) until `kStereoHoldSamples` after the last one. The hold feeds the
        // right chain the left's signal until its state has converged, so the downstream copies (the
        // Post fold here, the output drive in `CathodeOutputStage`) drop out bit-exact, without a step.
        // Outside it the pair is one mono value (L == R) and only the left chain runs.
        [[nodiscard]] bool stereoActive() const noexcept { return stereoHold_ > 0; }
        // [D-M-0511 / D-M-0373] The gain `ResonanceCeiling` applied to the last stage sample's F1/F2 SUM — 1
        // while the stage is under its ceiling or F2 is silent (each bank's own: `filter{1,2}().dryCeilingGain`).
        [[nodiscard]] float filterCeilingGain() const noexcept { return ceilingGain_; }
        // Longer than any right-chain state: the output drive's FIR history (2·16 base samples) and the
        // fold's one-sample ADAA memory.
        static constexpr int kStereoHoldSamples = 256;
        [[nodiscard]] bool filterBypassed() const noexcept { return filterFade_.bypassed(); }
        [[nodiscard]] float filterGain() const noexcept { return filterFade_.gain(); }
        [[nodiscard]] bool textureLive() const noexcept { return textureLive_; } // the block runs
        // Base RNG seed for the (deterministic) noise stream — drives §6 per-voice variation. Reseeds.
        void setNoiseSeed (std::uint32_t seed) noexcept
        {
            noise_.setSeed (seed);
            noiseSeed_ = seed; // [S5R 6] kept for a side stream prepare() has yet to make
            if (right_ != nullptr)
                right_->noiseSide.setSeed (seed ^ kNoiseSideSalt); // [S5] decorrelated, and as reproducible
        }
        // [stereo voice S5, D-M-0188] `synth.noise.width` in [0, 1]: the noise as MID + SIDE — left = n + w·n₂,
        // right = n − w·n₂, n₂ a second, decorrelated stream of the same type and level — so the mono sum is
        // the mono noise and width 1 makes the two ears independent. 0 (the default) is the mono noise bit for
        // bit: the second stream never runs. It reaches every place the noise goes (the Through injection, the
        // resonator's excitation, the pre-filter sum), each channel its own.
        // [S5R 2] The applied width GLIDES to the request (5 ms full scale, linear, landing exactly), like the unison
        // width; a silent voice (reset, a note from silence) lands at once.
        void setNoiseWidth (float width) noexcept { noiseWidthTarget_ = width < 0.0f ? 0.0f : (width > 1.0f ? 1.0f : width); }
        [[nodiscard]] float noiseWidth() const noexcept { return noiseWidthTarget_; }
        [[nodiscard]] float appliedNoiseWidth() const noexcept { return noiseWidth_; }
        // Metallic-resonator base frequency in Hz (mode i at tune·ratio[i]); control-rate (retunable
        // while ringing).
        void setResonTune (float baseHz) noexcept
        {
            reson_.setTune (baseHz);
            editRight ([baseHz] (RightChain& r) noexcept { r.reson.setTune (baseHz); });
        }
        // Metallic-resonator wet mix [0,1] — a WET LEVEL against the dry converter path, which is what
        // `kResonStagingGain` at the sum site makes true (the primitive's own `amount` is a raw gain on
        // the bank, ≈51× at the default tune). 0 (with noise level 0) → the texture layer bypasses
        // bit-exact (the resonator early-returns without advancing its ring). Control-rate.
        void setResonAmount (float amount) noexcept
        {
            reson_.setAmount (amount);
            editRight ([amount] (RightChain& r) noexcept { r.reson.setAmount (amount); });
            refreshTextureLive();
        }
        // ---- §5 extended NOISE shape controls (§9 synth.reson.{decay,struct,color,mix}; M1-18k) ----
        // Pass-throughs to the MetallicResonator shape setters (M1-18k slice a). Each is NEUTRAL at
        // 1.0 == the built table bit-exact, so an unshaped resonator is the M1-18a/b build. Control-rate
        // (the resonator recomputes its mode coefficients). The registry's 0-127 → engineering-unit map
        // (Param Ref §2.7) is an app-wiring concern — these take the DSP-native shape values directly.
        void setResonDecay (float scale) noexcept // global ring-time scale
        {
            reson_.setDecayScale (scale);
            editRight ([scale] (RightChain& r) noexcept { r.reson.setDecayScale (scale); });
        }
        void setResonStruct (float structure) noexcept // partial-spacing stretch
        {
            reson_.setStructure (structure);
            editRight ([structure] (RightChain& r) noexcept { r.reson.setStructure (structure); });
        }
        void setResonColor (float color) noexcept // spectral tilt of the mode gains
        {
            reson_.setColor (color);
            editRight ([color] (RightChain& r) noexcept { r.reson.setColor (color); });
        }
        // Resonator excitation BALANCE `mix` in [0,1] (§9 synth.reson.mix): balances the osc↔noise drive
        // of the metallic resonator. Neutral centre 0.5 (the default) keeps the built `oscMix + noiseOut`
        // excitation BIT-EXACT; 0 = osc only (pitched ring), 1 = noise only (atmospheric clang). The weights
        // are resolved control-rate here; the hot path is two muls + an add. Does NOT touch textureLive_
        // (mix only reshapes the resonator's input — noise.level/reson.amount gate the layer). Control-rate.
        void setResonMix (float mix) noexcept
        {
            resonMix_ = mix < 0.0f ? 0.0f : (mix > 1.0f ? 1.0f : mix);
            resonExcite_ = resonExciteWeights (resonMix_);
        }

        // [T-CORE-18] The §2 converter's control values, in the units their setters take.
        // The four the GRIT depth-scaler owns (`converterRateHz` · `bitDepth` · `loopError` ·
        // `drift`) and the two it must leave alone (`converterKeyTrack` · `companding`; the
        // third excluded row, `conv.interp`, is `converterInterp`).
        [[nodiscard]] float converterRateHz() const noexcept { return static_cast<float> (srr_.deviceRateHz()); }
        [[nodiscard]] float converterKeyTrack() const noexcept { return static_cast<float> (srr_.keyTrack()); }
        [[nodiscard]] SampleRateReducer::Interp converterInterp() const noexcept { return srr_.interpolation(); }
        [[nodiscard]] float bitDepth() const noexcept { return grit_.bitDepth(); }
        [[nodiscard]] float companding() const noexcept { return grit_.companding(); }
        [[nodiscard]] float loopError() const noexcept { return loopErr_.loopErr(); }
        [[nodiscard]] float drift() const noexcept { return drift_.drift(); }
        [[nodiscard]] float driftReadout() const noexcept { return drift_.readout(); } // [T-CGUI-24] Source::ConvDriftReadout

        [[nodiscard]] NoiseType noiseType() const noexcept { return noise_.type(); }
        [[nodiscard]] float noiseLevel() const noexcept { return noise_.level(); }
        [[nodiscard]] float resonTune() const noexcept { return reson_.tune(); }
        [[nodiscard]] float resonAmount() const noexcept { return reson_.amount(); }
        [[nodiscard]] float resonDecay() const noexcept { return reson_.decayScale(); }
        [[nodiscard]] float resonStruct() const noexcept { return reson_.structure(); }
        [[nodiscard]] float resonColor() const noexcept { return reson_.color(); }
        [[nodiscard]] float resonMix() const noexcept { return resonMix_; }

        [[nodiscard]] FilterMode filterMode() const noexcept { return f1_.filterMode(); }
        [[nodiscard]] DrivePlacement drivePlacement() const noexcept { return f1_.drivePlacement(); }
        // F1's per-part through-path latencies (see CathodeFilterBank for each one's contract).
        [[nodiscard]] int driveLatencySamples() const noexcept { return f1_.driveLatencySamples(); }
        [[nodiscard]] int ladderLatencySamples() const noexcept { return f1_.ladderLatencySamples(); }
        [[nodiscard]] int screamLatencySamples() const noexcept { return f1_.screamLatencySamples(); }
        [[nodiscard]] int acidLatencySamples() const noexcept { return f1_.acidLatencySamples(); }
        [[nodiscard]] int sovietLatencySamples() const noexcept { return f1_.sovietLatencySamples(); }
        [[nodiscard]] int classicLatencySamples() const noexcept { return f1_.classicLatencySamples(); }
        [[nodiscard]] int reedLatencySamples() const noexcept { return f1_.reedLatencySamples(); }
        [[nodiscard]] int stateLatencySamples() const noexcept { return f1_.stateLatencySamples(); }

        [[nodiscard]] OscMode oscMode (int slot) const noexcept { return osc_.mode (slot); }
        // [T-CGUI-63] The stack's applied state: requested count, count sounding after the collapse
        // rules (Macro / FM / ring / sync), detune and blend.
        [[nodiscard]] int unisonVoices (int slot) const noexcept { return osc_.unisonVoices (slot); }
        [[nodiscard]] int unisonActiveVoices (int slot) const noexcept { return osc_.unisonActiveVoices (slot); }
        [[nodiscard]] float unisonDetune (int slot) const noexcept { return osc_.unisonDetune (slot); }
        [[nodiscard]] float unisonBlend (int slot) const noexcept { return osc_.unisonBlend (slot); }
        // [D-M-0171(1)] The stack/single level match (`CathodeOscCore::setStackLevelMatch`, ON by default): the
        // A/B switch and the gain it holds — a test seam, not a host param.
        void setStackLevelMatch (bool on) noexcept { osc_.setStackLevelMatch (on); }
        [[nodiscard]] float stackLevelMatchGain (int slot) const noexcept { return osc_.stackLevelMatchGain (slot); }
        [[nodiscard]] float position (int slot) const noexcept { return osc_.position (slot); }
        // [T-CORE-16] The PdAmount twin of `position` above — the per-slot leg of the §2.9 PD AMT
        // rider's read-back, so the family's arithmetic is pinnable on BOTH oscillator destinations.
        [[nodiscard]] float pdAmount (int slot) const noexcept { return osc_.pdAmount (slot); }
        [[nodiscard]] PhaseDistortionOscillator::PdType pdType (int slot) const noexcept { return osc_.pdType (slot); }
        // [T-MASK-03 leaf D] The bitmask twins of the pair above — read at the oscillator, so a
        // slot that never received the value cannot report it.
        [[nodiscard]] int mask (int slot) const noexcept { return osc_.mask (slot); }
        [[nodiscard]] bool maskOversamplingBypassed (int slot) const noexcept
        {
            return osc_.maskOversamplingBypassed (slot);
        }
        [[nodiscard]] int maskOversampleFactor (int slot) const noexcept
        {
            return osc_.maskOversampleFactor (slot);
        }
        [[nodiscard]] Wide phase (int slot) const noexcept { return osc_.phase (slot); }
        // [T-WT-02 leaf C] The `setWavetable` twin: which table a slot is bound to, read at the
        // oscillator rather than inferred from whoever last called the setter.
        [[nodiscard]] const Wavetable* boundTable (int slot) const noexcept { return osc_.boundTable (slot); }
        // [T-CMP-02] The sub's read-backs, read at the oscillator for the reason T-CORE-14 gives:
        // a bookkeeping copy one layer up stays correct while a missed push leaves the sound wrong.
        [[nodiscard]] float subLevel() const noexcept { return osc_.subLevel(); }
        [[nodiscard]] CathodeOscCore::SubMode subMode() const noexcept { return osc_.subMode(); } // [T-CGUI-57]
        [[nodiscard]] int subOctave() const noexcept { return osc_.subOctave(); } // [T-CGUI-57]
        // [T-CMP-15 leaf C] Read-backs reach the VOICE, not a cache one layer up (T-CORE-14's rule).
        [[nodiscard]] Real pulseWidth (int slot) const noexcept { return osc_.pulseWidth (slot); }
        [[nodiscard]] float pulseSawMix (int slot) const noexcept { return osc_.pulseSawMix (slot); }
        [[nodiscard]] bool pulseLive (int slot) const noexcept { return osc_.pulseLive (slot); }
        [[nodiscard]] SubOscillator::Wave subWave() const noexcept { return osc_.subWave(); }
        [[nodiscard]] Wide subFrequencyHz() const noexcept { return osc_.subFrequencyHz(); }
        [[nodiscard]] const Wavetable* subBoundTable() const noexcept { return osc_.subBoundTable(); }

        // [T-CGUI-15] THE LIVE-TRACE TAPS — the two signals the OSC and NOISE vizzes draw (Function
        // Reference §"THE VISUALISATION CONTRACT"), as produced by the LAST `processSample`. OSC is the
        // PRE-HPF osc mix (owner, 2026-09-18); NOISE is the texture node `noiseOut + resonOut`, exactly
        // 0.0f while the layer is gated off. Stores beside the signal path, never on it: every golden
        // stays bit-exact. Read on the audio thread; `reset()` zeroes both.
        [[nodiscard]] float oscTap() const noexcept { return oscTap_; }
        [[nodiscard]] float textureTap() const noexcept { return textureTap_; }

        // Produces one per-voice sample in the spec-exact §0 order: osc mix → [HPF] → [FOLD Pre] → SRR capture (with
        // bits/compand applied AT CAPTURE via the M1-09xd hook) → SRR reconstruct → loop-error →
        // drift → [+ §5 texture: noise + resonator(mix-weighted oscMix↔noise), when live] → [+ auxPreFilter]
        // → [drive if PreFilter] → filter (TptSvf mode, or the FormantFilter bank in Formant mode)
        // → [drive if PostFilter] → [FOLD Post] (T-CGUI-64).
        // Hot path: the primitives' inline reads + a mode branch; the capture quantiser and the drive
        // shaper lambdas inline (no std::function allocation).
        //
        // [T-CORE-30 leaf D] `auxPreFilter` is an EXTERNALLY-generated signal summed at the same
        // pre-filter node the §5 texture uses, i.e. it goes through the drive and the filter. Its one
        // caller is the punch transient's `pre_filter` mix option (punch_transient.md §2), which is a
        // per-VOICE concern owned a layer up (`CathodeModulatedVoice`) — this stage only provides the
        // node, because the node is inside it and nothing else can reach it. Guarded by a `!= 0.0f`
        // test for the same reason the texture layer is guarded by `textureLive_`: adding a literal
        // 0.0f would flip a −0.0f sign bit, so a caller that passes nothing is bit-exact.
        StereoSample processStereo (float auxPreFilter = 0.0f) noexcept
        {
            processStereoBegin();
            processStereoPre (auxPreFilter);
            processStereoMid (f1CoreScalar());
            return processStereoPost (f2CoreScalar());
        }

        // [T-CMP-37 L2/L3] `processStereo` IN THREE PHASES around the two filter cores, so the engine can batch each Classic
        // core across voices. PRE runs everything up to F1's core input; MID takes F1's core output and runs F1's post half, the
        // F2 request follower and F2's pre half (the drive in front of F2's core); POST takes F2's core output and runs the rest.
        // `processStereo` is exactly pre → `f1CoreScalar()` → mid → `f2CoreScalar()` → post, so there is one code path and the
        // lane-wise cores replace only the middle calls. Between PRE and MID only F1's core may be advanced, between MID and
        // POST only F2's (the voice's other state waits for the next phase).
        void processStereoPre (float auxPreFilter = 0.0f) noexcept { chainPre (auxPreFilter); }
        // [T-CMP-37 L3] …and BEGIN before PRE: the oscillator core's prologue and its PD generators (CathodeOscCore::beginSample), so the
        // engine can run the PD decimators for four voices at once between BEGIN and PRE.
        void processStereoBegin() noexcept { osc_.beginSample(); }
        [[nodiscard]] bool oscPdStaged (int slot) const noexcept { return osc_.pdStaged (slot); }
        [[nodiscard]] const float* oscPdSteps (int slot) const noexcept { return osc_.pdSteps (slot); }
        void setOscPdResult (int slot, float decimated) noexcept { osc_.setPdLaneResult (slot, decimated); }
        [[nodiscard]] PhaseDistortionOscillator& oscPd (int slot) noexcept { return osc_.pdOscillator (slot); }
        [[nodiscard]] int oscPdFactor (int slot) const noexcept { return osc_.pdOscillatorFactor (slot); }
        void setOscPdLaneSlot (int slot, lanes::OsLaneSlot* seat) noexcept { osc_.setPdLaneSlot (slot, seat); }
        void pullOscPd (int slot) noexcept { osc_.pullPd (slot); }
        [[nodiscard]] std::uint64_t oscPdOrphans() const noexcept { return osc_.pdOrphans(); }
        void processStereoMid (float f1Core) noexcept { chainMid (f1Core); }
        // Whether F2's core is asked for a sample this time (F2 sounding, behind a stage that ran) and what it is fed.
        [[nodiscard]] bool f2CoreRuns() const noexcept { return mid_.f2Runs; }
        [[nodiscard]] float f2CoreInput() const noexcept { return mid_.f2In; }
        // F2's scalar core sample (0 when F2 does not run, which POST then ignores).
        [[nodiscard]] float f2CoreScalar() noexcept { return mid_.f2Runs ? f2_.processCore (mid_.f2In) : 0.0f; }
        // F2's Classic core can stand in a lane for this sample: F2 runs, in a Classic mode, at the lanes' design, with no right
        // chain (the twin `right_->f2` is a scalar copy; it stays scalar).
        [[nodiscard]] bool classicLane2Ready (const lanes::ClassicLanes4& batch) const noexcept
        {
            return mid_.f2Runs && !pre_.right && f2_.classicCoreActive() && batch.matches (f2_.classicCore());
        }
        // F2's seat in the engine's second lane batch (null = scalar only), and its hand-back, as F1's.
        void setClassicLaneSlot2 (lanes::ClassicLaneSlot* slot) noexcept { laneSlot2_ = slot; }
        void pullClassic2FromLane() noexcept
        {
            if (laneSlot2_ != nullptr)
                laneSlot2_->pull (f2_.classicCore());
        }
        // Whether F1's core is asked for a sample this time (the FILTER stage is not parked) and what it is fed.
        [[nodiscard]] bool f1CoreRuns() const noexcept { return pre_.runsF1; }
        [[nodiscard]] float f1CoreInput() const noexcept { return pre_.toFilter; }
        // The scalar core's sample (0 when F1 does not run, which POST then ignores).
        [[nodiscard]] float f1CoreScalar() noexcept { return pre_.runsF1 ? f1_.processCore (pre_.toFilter) : 0.0f; }
        // A lane-wise Classic core can stand in for F1's this sample: F1 runs, in a Classic mode, at the lanes' design, with no
        // right chain (its F1 twin is a scalar copy; L3) — a stage that does not qualify just runs `f1CoreScalar()`.
        [[nodiscard]] bool classicLaneReady (const lanes::ClassicLanes4& batch) const noexcept
        {
            return pre_.runsF1 && !pre_.right && f1_.classicCoreActive() && batch.matches (f1_.classicCore());
        }
        // The lane seat the engine assigns this stage (null = scalar only: a test's or a benchmark's private stage).
        void setClassicLaneSlot (lanes::ClassicLaneSlot* slot) noexcept { laneSlot_ = slot; }
        [[nodiscard]] lanes::ClassicLaneSlot* classicLaneSlot() const noexcept { return laneSlot_; }
        // The scalar F1 Classic core holds the true state again (a no-op while it is not lane-resident). Call before
        // anything reads or copies it.
        void pullClassicFromLane() noexcept
        {
            if (laneSlot_ != nullptr)
                laneSlot_->pull (f1_.classicCore());
        }

        StereoSample processStereoPost (float f2Core) noexcept
        {
            const StereoSample out = chainPost (f2Core);
            // [stereo voice S2] The right chain runs from the first split sample, starting from the left's
            // state (until now both heard one signal), and through the hold after the last.
            // [stereo voice S3] …and while the upstream right chain runs (a stereo source). The Post fold's
            // twin starts here; at Pre it started with that chain (`startRightChain`) and has already run.
            if (split_ || rightRun_)
            {
                if (stereoHold_ == 0 && foldPos_ == FoldPosition::Post)
                    foldR_ = fold_;
                stereoHold_ = kStereoHoldSamples;
            }
            else if (stereoHold_ > 0)
                --stereoHold_;
            // [T-CGUI-64] FOLD Post: after the whole FILTER stage (F1/F2 and the bypass blend), before
            // the amp. Guarded, so an off fold returns the stage output bit for bit.
            if (foldLive_ && foldPos_ == FoldPosition::Post)
                return foldPair (out, stereoHold_ > 0);
            return out;
        }

        // [stereo voice S1] The MONO view of `processStereo`, for the callers that read one sample (tests).
        // [D-M-0556] ½(L + R): exact while the voice is mono (L == R: L + L and ×½ are both exact), and a
        // split pair is heard WHOLE — F1 and F2 — at the level a mono voice has (a Stereo voice with
        // F1 == F2 is the mono voice; the output stage's pan law holds the same rule).
        float processSample (float auxPreFilter = 0.0f) noexcept
        {
            const StereoSample y = processStereo (auxPreFilter);
            return 0.5f * (y.left + y.right);
        }

    private:
        // [T-CMP-37 L2] What PRE leaves for POST: the pre-filter nodes, whether the right chain ran, whether F1's core runs,
        // and what it is fed.
        struct PreStash
        {
            float preFilter = 0.0f;
            float preFilterR = 0.0f;
            float toFilter = 0.0f;
            bool right = false;
            bool runsF1 = false;
        };
        PreStash pre_;

        // [T-CMP-37 L3] What MID leaves for POST: whether the filter stage finished in MID (F2 off and settled: the single-filter
        // stage), its result then, whether F2's core runs and what it is fed, and the nodes POST's F2 branches read.
        struct MidStash
        {
            float preR = 0.0f;
            float a = 0.0f; // F1's post-half output
            float aR = 0.0f;
            float f2In = 0.0f; // what F2's core is fed (after its pre half)
            StereoSample settled {}; // the result when `done`
            bool right = false;
            bool ran = false; // MID ran the filter stage (F1 not parked)
            bool done = false; // F2 off and settled: `settled` is the stage output
            bool f2Runs = false;
        };
        MidStash mid_;

        // The §0 chain up to F1's core input — the first half of what `processStereo` runs before the Post fold.
        void chainPre (float auxPreFilter) noexcept
        {
            // [stereo voice S4] The oscillators as MID and SIDE: left = mid + side, right = mid − side.
            const CathodeOscCore::StereoOut osc = osc_.finishSample(); // [T-CMP-37 L3] BEGIN ran the prologue and the PD generators
            const float oscMix = osc.mid;
            // [stereo voice S3] Decided before any twin runs: the right chain starts (a copy of the left) or
            // fades out here. Idle, every `…R` value below IS its left value and no twin runs.
            const bool side = osc_.sideLive();
            if (noiseWidth_ != noiseWidthTarget_) // [S5R 2] the glide
            {
                const float gap = noiseWidthTarget_ - noiseWidth_;
                noiseWidth_ = std::abs (gap) <= noiseWidthStep_ ? noiseWidthTarget_ : noiseWidth_ + (gap > 0.0f ? noiseWidthStep_ : -noiseWidthStep_);
            }
            const bool noiseWide = noiseWidth_ > 0.0f && textureLive_ && noise_.level() > 0.0f; // [S5]
            followRightChain (forceRight_ || side || noiseWide);
            const bool right = rightRun_;
            const bool spread = right && side; // a side the right chain can carry (else the voice stays mono)
            const float oscL = spread ? oscMix + osc.side : oscMix;
            const float oscR = spread ? oscMix - osc.side : oscMix;
            oscTap_ = oscMix; // [T-CGUI-15] the OSC live trace, PRE-HPF
            textureTap_ = 0.0f; // [T-CGUI-15] overwritten below only while the layer is live
            // [T-CMP-01] `synth.hpf` — the Juno series high-pass, PRE-CONVERTER (owner ruling,
            // 2026-08-18; CATHODE.md:72 amended). It shapes what the broken converter then chews
            // on, which is the audible point of putting it first. Guarded rather than computed:
            // at v = 0 the filter neither runs NOR ADVANCES, so an off row is bit-exact against
            // every shipped golden AND cannot colour the next note through leftover state.
            //
            // WHAT IT DOES NOT TOUCH: noise and the resonator sum in at the PRE-FILTER node below,
            // i.e. after the converter, so they are not high-passed (R4, ruled). The SUB (T-CMP-02)
            // joins `oscMix` above and IS — that is plan gate R2, and it is why `oscMix` and not
            // some later node is where the sub sums.
            const float hpfOut = hpfLive_ ? hpf_.processSample (oscL).hp : oscL;
            const float hpfOutR = !right ? hpfOut : (hpfLive_ ? right_->hpf.processSample (oscR).hp : oscR);
            // [T-CGUI-64] FOLD Pre: after the HPF, BEFORE the converter — GRIT chews the fold, and the
            // GRIT dry leg below is the folded signal (the fold is upstream of the whole GRIT stage).
            StereoSample src { hpfOut, hpfOutR };
            if (foldLive_ && foldPos_ == FoldPosition::Pre)
                src = foldPair (src, right);
            const float source = src.left;
            const float sourceR = src.right;
            // §5 texture: the noise is drawn HERE, before the converter, so a Through route can feed it
            // in (D-M-0509). Same one draw per sample under `textureLive_`, so the RNG order is the
            // built one. The fade TICKS here too (once, as before) so the injection rides this sample's
            // gain; `textureInCircuit` is read BEFORE the tick — the sum below keeps the built rounding.
            float noiseOut = 0.0f;
            float noiseSide = 0.0f;
            const bool noiseSpread = right && noiseWide; // [S5] a side the right chain can carry
            const bool textureInCircuit = textureFade_.inCircuit();
            bool textureLandedOut = false;
            if (textureLive_)
            {
                noiseOut = noise_.processSample();
                if (noiseSpread)
                    noiseSide = noiseWidth_ * right_->noiseSide.processSample();
                if (!textureInCircuit)
                    textureLandedOut = textureFade_.tick();
            }
            // [stereo voice S5] Mid + side; narrow, the left IS the mono noise (no add: a −0.0f stays −0.0f).
            const float noiseL = noiseSpread ? noiseOut + noiseSide : noiseOut;
            const float noiseR = noiseSpread ? noiseOut - noiseSide : noiseOut;
            // [D-M-0512] Mid-switch the noise is SPLIT, `w` injected and `1 − w` summed (`routeW`, the
            // Through share, ticked here so this sample rides it). Settled, the two built expressions.
            const bool routeFading = routeFade_.fading();
            float routeW = 0.0f;
            if (routeFading)
            {
                routeFade_.tick();
                routeW = routeFade_.gain();
            }
            const bool blend = textureLive_ && routeFading;
            const bool through = textureLive_ && !routeFading && noiseRoute_ == NoiseRoute::Through;
            // In circuit the gain is exactly 1.0f (1.0f·x is exact). The GRIT dry leg below is written
            // from this node, so GRIT bypass/amount act on the injected noise as on the osc.
            const auto inject = [&] (float x, float n) noexcept {
                return through ? x + textureFade_.gain() * n : (blend ? x + (textureFade_.gain() * routeW) * n : x);
            };
            const float converterIn = inject (source, noiseL);
            const auto quantise = [this] (float v) noexcept { return grit_.processSample (v); };
            const float converted = srr_.processSample (converterIn, quantise);
            const float looped = loopErr_.processSample (converted);
            const float drifted = drift_.processSample (looped);
            float converterInR = converterIn;
            float driftedR = drifted;
            if (right)
            {
                RightChain& r = *right_;
                converterInR = inject (sourceR, noiseR);
                driftedR = r.drift.processSample (r.loopErr.processSample (r.srr.processSample (converterInR, quantise)));
                r.gritDry[gritDryPos_] = converterInR;
            }
            // [T-CGUI-09 / T-CGUI-08] The GRIT blends. The dry ring is written EVERY sample, so any
            // blend begins warm. At amount 1 and in circuit, `gritted` IS `drifted` — the built chain
            // bit for bit; otherwise the wet is `dry + amount·(chain − dry)` against the latency-
            // matched dry leg, and the bypass fades that to the dry leg itself (parked: dry alone).
            gritDry_[gritDryPos_] = converterIn;
            // The dry leg's delay GLIDES to the chain's latency (a slow one-pole, snapped when it
            // arrives): a control that steps the latency — drift or loop error crossing 0, RECON, a
            // key-tracked note — must not step the dry leg (a click), and a rate LFO must not sweep it
            // (a vibrato the wet chain does not have). Off-path while the blend is unused.
            {
                const Real target = gritLatencySamples();
                const Real gap = target - gritDryDelay_;
                gritDryDelay_ = std::abs (gap) < 1.0e-3 ? target : gritDryDelay_ + gritDelaySlew_ * gap; // snap within 1/1000 sample
            }
            float gritted = drifted;
            float grittedR = driftedR;
            if (!gritFade_.inCircuit() || gritAmount_ < 1.0f)
            {
                const auto wetOf = [this] (float dry, float chain) noexcept {
                    return gritAmount_ >= 1.0f ? chain : dry + gritAmount_ * (chain - dry);
                };
                const float dry = gritDryLeg (gritDry_);
                const float wet = wetOf (dry, drifted);
                const float dryR = right ? gritDryLeg (right_->gritDry) : dry;
                const float wetR = right ? wetOf (dryR, driftedR) : wet;
                if (gritFade_.inCircuit())
                {
                    gritted = wet;
                    grittedR = wetR;
                }
                else
                {
                    gritFade_.tick();
                    const auto faded = [this] (float d, float w) noexcept {
                        return gritFade_.parked() ? d : d + gritFade_.gain() * (w - d);
                    };
                    gritted = faded (dry, wet);
                    grittedR = right ? faded (dryR, wetR) : gritted;
                }
            }
            gritDryPos_ = (gritDryPos_ + 1) & gritDryMask_;
            // §5 texture layer: sum the noise + metallic resonator into the converter output, PRE-FILTER
            // (CATHODE.md §0). The resonator is excited by the raw osc mix + the noise (owner decision).
            // Guarded by textureLive_ so a bypassed layer passes `gritted` through bit-for-bit (adding
            // 0.0f would flip a -0.0f sign bit) and never advances the noise RNG. `noiseOut` was drawn
            // above the converter; on the Through route its direct term is already inside `gritted`.
            float preFilter = gritted;
            float preFilterR = grittedR;
            if (textureLive_)
            {
                // §5 `mix` excitation balance: the resonator is driven by the mix-weighted osc↔noise
                // node (neutral 0.5 → wOsc == wNoise == 1.0f → the built `oscMix + noiseOut` bit-exact).
                // The direct `noiseOut` term in the texture node is unaffected by mix.
                //
                // [T-CMP-02] `oscMix` NOW CARRIES THE SUB, so a live sub also excites the resonator.
                // That is a consequence of summing the sub into the osc mix rather than a second
                // decision: §5 says the resonator is excited by "the raw osc mix", and the sub is
                // part of the osc mix. It is stated here because it is the one place the sub reaches
                // that is not on the R2 path, and a reader chasing "why did the resonator change"
                // should find the answer at the node rather than deduce it.
                const float excite = resonExcite_.wOsc * oscL + resonExcite_.wNoise * noiseL;
                // …and the §5 STAGING gain (kResonStagingGain) is applied HERE, where the layer
                // mixes: `amount` is a raw gain on the bank, so the level structure belongs to the
                // sum, not to the primitive. Named once at the destination (T-CORE-09).
                const float resonOut = kResonStagingGain * reson_.processSample (excite);
                const float resonOutR = !right ? resonOut
                                               : kResonStagingGain
                                                     * right_->reson.processSample (resonExcite_.wOsc * oscR + resonExcite_.wNoise * noiseR);
                if (textureInCircuit) // not fading, not bypassed: gain is exactly 1
                {
                    const auto sum = [&] (float g, float n, float ro) noexcept {
                        if (blend)
                            return g + (1.0f - routeW) * n + ro;
                        return through ? g + ro : g + n + ro;
                    };
                    preFilter = sum (gritted, noiseL, resonOut);
                    preFilterR = right ? sum (grittedR, noiseR, resonOutR) : preFilter;
                    // [T-CGUI-15] The NOISE live trace. Its own add, off the path: `preFilter` above
                    // is `(gritted + noiseOut) + resonOut`, so this sum is not a subexpression of it.
                    textureTap_ = noiseOut + resonOut;
                }
                else
                {
                    // [T-CGUI-08] Fading. The step direction follows the target; the last step lands
                    // EXACTLY on it, and a finished fade-out clears the ring and parks the layer.
                    if (textureLandedOut)
                    {
                        reson_.reset();
                        if (right)
                            right_->reson.reset();
                    }
                    const auto sum = [&] (float g, float n, float ro) noexcept {
                        if (blend)
                            return g + textureFade_.gain() * ((1.0f - routeW) * n + ro);
                        return g + (through ? textureFade_.gain() * ro : textureFade_.gain() * (n + ro));
                    };
                    textureTap_ = textureFade_.gain() * (noiseOut + resonOut);
                    preFilter = sum (gritted, noiseL, resonOut);
                    preFilterR = right ? sum (grittedR, noiseR, resonOutR) : preFilter;
                    if (textureFade_.parked())
                        refreshTextureLive();
                }
            }
            // [T-CORE-30 leaf D] The external pre-filter injection (see the header note). BATTERY
            // SURVIVOR, CLASSIFIED (c) — the twin of the post-amp guard in `CathodeOutputStage`, same
            // reasoning and same verdict: removing it kills nothing, because adding a literal 0.0f is
            // the identity except on −0.0f, where the sign flip is invisible to float `==` and to a
            // PCM encode. Kept because that equivalence is a property of the current test CORPUS
            // rather than of the code, and labelled so the next reader does not mistake it for the
            // mechanism that makes an absent injection bit-exact.
            if (auxPreFilter != 0.0f)
            {
                preFilter += auxPreFilter;
                preFilterR += auxPreFilter;
            }
            if (!right)
                preFilterR = preFilter;
            else
                preFilterR = rightBlend (preFilterR, preFilter); // [S3] a fading right chain leaves toward the left
            // [T-CGUI-08] The FILTER bypass blend, from the pre-filter node to the stage output. In
            // circuit the built stage runs untouched; parked it does not run at all — both bit-exact.
            // [D-M-0511 / D-M-0373] The stage output is held under `ResonanceCeiling` — the SUM of F1 and
            // F2 as routed, relative to the pre-filter node. A gain of exactly 1 under it (bit-exact).
            // [T-CMP-37 L2] The stage runs unless it is parked; its F1-side prologue (`filterStage`'s first lines) and the
            // drive that feeds the core are PRE, the core and everything after it are POST.
            pre_.preFilter = preFilter;
            pre_.preFilterR = preFilterR;
            pre_.right = right;
            pre_.runsF1 = !filterFade_.parked();
            if (pre_.runsF1)
            {
                split_ = false;
                applyFilter2Edge();
                pre_.toFilter = f1_.processPre (preFilter);
            }
        }

        // [T-CMP-37 L3] The FILTER stage from F1's core output up to F2's core input: runs when PRE ran the stage (F1 not parked).
        void chainMid (float f1Core) noexcept
        {
            mid_.ran = false;
            mid_.f2Runs = false;
            mid_.done = false;
            if (pre_.runsF1)
                filterStageMid (pre_.preFilter, pre_.preFilterR, pre_.right, pre_.toFilter, f1Core);
        }

        // The rest of the §0 chain up to and including the FILTER stage — `processStereo` without the Post fold.
        StereoSample chainPost (float f2Core) noexcept
        {
            const float preFilter = pre_.preFilter;
            const float preFilterR = pre_.preFilterR;
            if (filterFade_.inCircuit())
                return ceilinged (filterStageEnd (f2Core), preFilter, preFilterR);
            if (filterFade_.parked())
            {
                split_ = false; // the filter does not run: one mono signal
                return { preFilter, preFilterR };
            }
            const StereoSample wet = ceilinged (filterStageEnd (f2Core), preFilter, preFilterR);
            if (filterFade_.tick())
            {
                resetFilterStage(); // faded out: clear the ring, so an un-bypass starts clean
                split_ = false;
                return { preFilter, preFilterR };
            }
            const float g = filterFade_.gain();
            return { preFilter + g * (wet.left - preFilter), preFilterR + g * (wet.right - preFilterR) };
        }

        // [D-M-0511 / D-M-0373] The filter stage's output under the level-dependent ceiling (see
        // `ResonanceCeiling`): one LINKED gain for the pair, so a Stereo split keeps its image. Exactly
        // the input pair while the gain is 1. Not on `processFilterOnly` — the FILTER figure measures
        // the filters' linear response, and a ceiling is a level law, not part of it.
        // Each BANK already holds its own core (`CathodeFilterBank::dryCeilingGain`), so with F2 silent this
        // stage's gain is computed (its detectors stay warm) but not applied: a second rounding of the same
        // bound would only move the single-filter stage by ulps. It acts on the SUM — F2 sounding — AND on the
        // tail after F2 stops [S5R B31 dsp2 F1]: the detectors still hold the sum's peak there, and dropping the
        // gain to 1 in one sample was a ~6 dB step. The tail RISES from the held gain to 1 — no slower than
        // `kCeilingTailSeconds` full scale, faster if the detectors release first — then lets go for good. A
        // rise faster than the detectors is safe: F1 alone is held by its own bank under the same law (and a
        // steady tone's detector can stall up to 3 % high, so waiting on them alone might never reach 1).
        // [D-M-0511, F5's law one level up] The reference is the louder of the stage input and what each
        // sounding bank's PreFilter drive passed: a quiet node driven up by the banks is not held relative to
        // the quiet node (that clamped the drives' own gain away). A drive's output is bounded (~2/3), so this
        // only ever lifts a floor-bound ceiling, by at most ~7 %; without a Pre drive it is the stage input.
        StereoSample ceilinged (StereoSample y, float in, float inR) noexcept
        {
            float ref = std::max (std::abs (in), std::abs (inR)); // [S3] mono: inR == in, the built ref
            ref = std::max (ref, f1_.preDriveLevel());
            if (f2Sounding_)
                ref = std::max (ref, f2_.preDriveLevel());
            const float g = ceiling_.gainFor (ref, std::max (std::abs (y.left), std::abs (y.right)));
            if (f2Sounding_)
            {
                ceilingTail_ = true;
                // [S5R B31 final F6] F2 back on inside the detectors' hold + release: they still hold the old sum, so
                // taking their gain at once stepped from the risen tail (or 1) to ~0.5 in one sample. The gain walks
                // down at the tail's own rate until it meets theirs, then follows them again.
                const float down = ceilingGain_ - ceilingTailRise_;
                if (ceilingCatch_ && g < down)
                    ceilingGain_ = down;
                else
                {
                    ceilingGain_ = g;
                    ceilingCatch_ = false;
                }
            }
            else if (ceilingTail_)
            {
                const float up = ceilingGain_ + ceilingTailRise_;
                ceilingGain_ = g > up ? g : up;
                if (ceilingGain_ >= 1.0f)
                {
                    ceilingGain_ = 1.0f;
                    ceilingTail_ = false;
                }
            }
            else
                ceilingGain_ = 1.0f;
            if (!f2Sounding_)
                ceilingCatch_ = true; // F2's next entry walks down from wherever the gain is then
            if (ceilingGain_ == 1.0f)
                return y;
            return { y.left * ceilingGain_, y.right * ceilingGain_ };
        }

        // [T-CGUI-64] One folded sample at the CURRENT position, the move fade applied. The fade lands
        // on 0 → the stage moves, forgets its ADAA memory (the new position's signal is unrelated),
        // and fades back in.
        float foldSample (float x) noexcept
        {
            if (foldFade_.inCircuit())
                return fold_.processSample (x, foldAmount_);
            const bool landedOut = foldFade_.tick();
            const float y = fold_.processSample (x, foldAmount_, foldFade_.gain());
            if (landedOut)
            {
                foldPos_ = foldTarget_;
                fold_.reset();
                foldR_.reset();
                foldFade_.set (false);
            }
            return y;
        }

        // [stereo voice S2] FOLD over the pair: `foldSample` on the left, and on the right its own fold
        // (`foldR_`) under the same move fade while the right channel runs; otherwise the left's value.
        // [stereo voice S3] At either position: Pre while the upstream right chain runs, Post while the
        // pair is held stereo.
        StereoSample foldPair (StereoSample in, bool right) noexcept
        {
            if (!right)
            {
                const float y = foldSample (in.left);
                return { y, y };
            }
            if (foldFade_.inCircuit())
                return { fold_.processSample (in.left, foldAmount_), foldR_.processSample (in.right, foldAmount_) };
            const bool landedOut = foldFade_.tick();
            const float g = foldFade_.gain();
            const StereoSample y { fold_.processSample (in.left, foldAmount_, g), foldR_.processSample (in.right, foldAmount_, g) };
            if (landedOut)
            {
                foldPos_ = foldTarget_;
                fold_.reset();
                foldR_.reset();
                foldFade_.set (false);
            }
            return y;
        }

        // Lands a pending move at once (a silent fold, or `reset`). A real move re-primes the ADAA
        // memory: the new position's signal is unrelated to the last u[n−1] (the faded path's rule).
        void settleFoldMove() noexcept
        {
            if (foldPos_ != foldTarget_)
            {
                fold_.reset();
                foldR_.reset();
            }
            foldPos_ = foldTarget_;
            foldFade_.set (false);
            foldFade_.settle();
        }

        // [T-CGUI-09] The dry leg: the ring read `gritDryDelay_` (the latency, glided) behind the sample just written,
        // linearly interpolated (the drift and loop-error lines' own read law).
        [[nodiscard]] float gritDryLeg (const std::vector<float>& ring) const noexcept
        {
            const Real d = gritDryDelay_;
            const auto whole = static_cast<std::size_t> (d);
            const auto frac = static_cast<float> (d - static_cast<Real> (whole));
            const float a = ring[(gritDryPos_ - whole) & gritDryMask_];
            if (frac == 0.0f)
                return a;
            const float b = ring[(gritDryPos_ - whole - 1) & gritDryMask_];
            return a + frac * (b - a);
        }

        // The FILTER stage — F1 (T-CGUI-60 leaf 1: the body is `CathodeFilterBank::process`). One
        // function so the `synth.filt.bypass` blend in `processSample` has exactly one wet value to
        // fade against (T-CGUI-08). [stereo voice S1] Returned as an L/R pair; mono, so L == R.
        // [stereo voice S3] `preR` is the right channel's pre-filter node and `right` whether its twins run
        // (`f1R_`/`f2R_`, each fed as its left twin is); idle, `preR` must be `preL` and the pair is the
        // mono stage bit for bit. On the Stereo route F2 hears the RIGHT input (left source -> F1, right -> F2).
        // [T-CMP-37 L2] The stage as one call (the FILTER figure's private stage): the PRE lines, F1's scalar core, the MID lines,
        // F2's scalar core, then the rest.
        StereoSample filterStageWhole (float preL, float preR) noexcept
        {
            split_ = false;
            applyFilter2Edge();
            const float toFilter = f1_.processPre (preL);
            filterStageMid (preL, preR, false, toFilter, f1_.processCore (toFilter));
            return filterStageEnd (f2CoreScalar());
        }

        // [T-CMP-37 L2] Its first lines (`split_ = false`, the F2 edge, F1's drive) ran in PRE; `toFilter` is what F1's core was
        // fed and `f1Core` its sample.
        // [T-CMP-37 L3] CUT AT F2'S CORE. MID is everything up to the one place the stage hands a signal to F2: F1's post half,
        // its aligner write, the F2 request follower and the settled single-filter early return (F2 off), then F2's pre half (its
        // drive) on the signal F2 is fed. END takes F2's core sample, runs F2's post half and the rest. Nothing between F2's pre
        // half and its post half touches F2: the cut is at statement boundaries, the compiler fuses within a statement only.
        void filterStageMid (float preL, float preR, bool right, float toFilter, float f1Core) noexcept
        {
            // [T-CGUI-77 f2-parallel-zero-gap] F1's aligner is written on EVERY sample F1 runs — which
            // is every sample — so entering Parallel with F1 as the delayed branch reads F1's own real
            // past rather than a zero-filled ring (a `lag`-sample dropout: a click). One store.
            const float a = f1_.processPost (toFilter, f1Core);
            alignA_[alignPos_] = a;
            float aR = a;
            if (right)
            {
                aR = right_->f1.process (preR);
                right_->alignA[alignPos_] = aR;
            }
            if (filterFresh_) // nothing has sounded since a reset: a request lands at once, no crossfade
            {
                settleFilter2Topology();
                filterFresh_ = false;
            }
            followFilter2Request();
            mid_.ran = true;
            mid_.preR = preR;
            mid_.right = right;
            mid_.a = a;
            mid_.aR = aR;
            mid_.done = false;
            mid_.f2Runs = false;
            if (!f2Sounding_ && lagA_ == 0.0)
            {
                if (balanceLeft_ > 0) // nothing to hear the ramp through: land it
                {
                    gains_ = gainsTarget_;
                    balanceLeft_ = 0;
                }
                alignPos_ = (alignPos_ + 1) & kAlignMask;
                mid_.settled = { a, right ? rightBlend (aR, a) : a }; // F2 off and settled: the single-filter stage, bit for bit
                mid_.done = true;
                return;
            }
            if (f2Sounding_)
            {
                // Series: F2 hears F1's output. Parallel / Stereo: both from the same node (Stereo: F2 the right input).
                mid_.f2Runs = true;
                mid_.f2In = f2_.processPre (f2Parallel_ ? (f2Stereo_ ? preR : preL) : a);
            }
        }

        StereoSample filterStageEnd (float f2Core) noexcept
        {
            if (mid_.done)
                return mid_.settled;
            const float preR = mid_.preR;
            const bool right = mid_.right;
            const float a = mid_.a;
            const float aR = mid_.aR;
            // F1 ALONE, read `lagA_` behind: the reference every F2 topology crossfades against, so a
            // topology change passes through it rather than through silence.
            const float f1 = lagA_ == 0.0 ? a : alignRead (alignA_.data(), lagA_);
            const float f1R = !right ? f1 : rightBlend (lagA_ == 0.0 ? aR : alignRead (right_->alignA.data(), lagA_), f1);
            float out = f1;
            float outR = f1R;
            if (f2Sounding_ && !f2Parallel_)
            {
                const float wet = f2_.processPost (mid_.f2In, f2Core);
                out = f2Fade_.inCircuit() ? wet : f1 + f2Fade_.gain() * (wet - f1);
                outR = out;
                if (right)
                {
                    const float wetR = right_->f2.process (aR);
                    outR = rightBlend (f2Fade_.inCircuit() ? wetR : f1R + f2Fade_.gain() * (wetR - f1R), out);
                }
            }
            else if (f2Sounding_)
            {
                // Parallel and Stereo: both from the same node, the earlier branch delayed by the latency
                // difference so the two stay in phase. F2's ring is written every such sample (cleared at
                // the switch). A latency change GLIDES the read distances.
                const float b = f2_.processPost (mid_.f2In, f2Core);
                alignB_[alignPos_] = b;
                const float db = lagB_ == 0.0 ? b : alignRead (alignB_.data(), lagB_);
                if (f2Stereo_)
                {
                    // [stereo voice S2] F1 left, F2 right — kept apart; each fades from F1 alone, so the
                    // right channel leaves the left (and comes back to it) as a crossfade.
                    const float wetL = gains_.f1 * f1;
                    const float wetR = gains_.f2 * db;
                    const bool in = f2Fade_.inCircuit();
                    out = in ? wetL : f1 + f2Fade_.gain() * (wetL - f1);
                    outR = in ? wetR : f1R + f2Fade_.gain() * (wetR - f1R);
                    split_ = true;
                }
                else
                {
                    const float wet = gains_.f1 * f1 + gains_.f2 * db;
                    out = f2Fade_.inCircuit() ? wet : f1 + f2Fade_.gain() * (wet - f1);
                    outR = out;
                    if (right)
                    {
                        const float bR = right_->f2.process (preR);
                        right_->alignB[alignPos_] = bR;
                        const float dbR = lagB_ == 0.0 ? bR : alignRead (right_->alignB.data(), lagB_);
                        const float wetR = gains_.f1 * f1R + gains_.f2 * dbR;
                        outR = rightBlend (f2Fade_.inCircuit() ? wetR : f1R + f2Fade_.gain() * (wetR - f1R), out);
                    }
                }
            }
            if (balanceLeft_ > 0)
                tickBalance();
            if (f2Fade_.fading() && f2Fade_.tick())
                switchFilter2Topology(); // F2's contribution has faded out: the requested topology takes over
            glideAlignLags();
            alignPos_ = (alignPos_ + 1) & kAlignMask;
            return { out, outR };
        }

        // A read `lag` samples behind the one just written, linearly interpolated (the GRIT dry leg's law).
        [[nodiscard]] float alignRead (const float* ring, Real lag) const noexcept
        {
            const auto whole = static_cast<std::size_t> (lag);
            const auto frac = static_cast<float> (lag - static_cast<Real> (whole));
            const float x0 = ring[(alignPos_ - whole) & kAlignMask];
            if (frac == 0.0f)
                return x0;
            const float x1 = ring[(alignPos_ - whole - 1) & kAlignMask];
            return x0 + frac * (x1 - x0);
        }

        // Per sample: a request that differs from the sounding topology fades F2's contribution out (or,
        // with F2 silent already, switches at once); a request back to the sounding one fades it back in.
        void followFilter2Request() noexcept
        {
            const bool wantParallel = route_ != FilterRoute::Series;
            const bool wantStereo = route_ == FilterRoute::Stereo; // [stereo voice S2] its own topology
            const bool same = f2On_ == f2Sounding_ && (!f2On_ || (wantParallel == f2Parallel_ && wantStereo == f2Stereo_));
            if (same)
            {
                if (f2Sounding_ && f2Fade_.bypassed())
                    f2Fade_.set (false);
                return;
            }
            if (!f2Sounding_ || f2Fade_.parked())
                switchFilter2Topology();
            else
                f2Fade_.set (true);
        }

        // F2's contribution is silent: the requested topology takes over, F2 and its aligner clean.
        void switchFilter2Topology() noexcept
        {
            f2Sounding_ = f2On_;
            f2Parallel_ = route_ != FilterRoute::Series;
            f2Stereo_ = route_ == FilterRoute::Stereo;
            if (!f2Sounding_)
                return;
            resetFilter2Path();
            lagB_ = alignTargetB();
            f2Fade_.set (false); // from 0: fade in
        }

        // The aligners' TARGET read distances: in Parallel the shorter branch waits for the longer; F1
        // alone (F2 off or in Series) is not delayed.
        [[nodiscard]] Real alignTargetA() const noexcept
        {
            if (!f2Sounding_ || !f2Parallel_)
                return 0.0;
            const int diff = f2_.latencySamples() - f1_.latencySamples();
            return static_cast<Real> (std::min (diff > 0 ? diff : 0, static_cast<int> (kAlignMask) - 1));
        }
        [[nodiscard]] Real alignTargetB() const noexcept
        {
            const int diff = f1_.latencySamples() - f2_.latencySamples();
            return static_cast<Real> (std::min (diff > 0 ? diff : 0, static_cast<int> (kAlignMask) - 1));
        }

        // Moves each read distance toward its target by at most kAlignGlide samples per sample (a brief
        // varispeed, ~1 semitone at most) and lands on it exactly.
        void glideAlignLags() noexcept
        {
            const auto glide = [] (Real& lag, Real target) noexcept {
                if (lag == target)
                    return;
                const Real gap = target - lag;
                lag = std::abs (gap) <= kAlignGlide ? target : lag + (gap > 0.0 ? kAlignGlide : -kAlignGlide);
            };
            glide (lagA_, alignTargetA());
            if (f2Sounding_ && f2Parallel_)
                glide (lagB_, alignTargetB());
        }

        void tickBalance() noexcept
        {
            if (--balanceLeft_ == 0)
                gains_ = gainsTarget_; // lands exactly
            else
                gains_ = { gains_.f1 + balanceStep_.f1, gains_.f2 + balanceStep_.f2 };
        }

        // The whole F2 topology at its request, with nothing in flight (reset / prepare).
        void settleFilter2Topology() noexcept
        {
            latchBalanceRequest(); // [T-CGUI-79 S5R-7] a settle lands on the REQUESTED balance
            f2Sounding_ = f2On_;
            f2Parallel_ = route_ != FilterRoute::Series;
            f2Stereo_ = route_ == FilterRoute::Stereo;
            f2Fade_.set (!f2Sounding_);
            f2Fade_.settle();
            lagA_ = alignTargetA();
            lagB_ = alignTargetB();
            gains_ = gainsTarget_;
            balanceLeft_ = 0;
            filterFresh_ = true;
        }

        // [T-CGUI-08] Clears everything `filterStage` owns. NOT the HPF: it is not part of the FILTER
        // bypass (owner 2026-09-23).
        void resetFilterStage() noexcept
        {
            f1_.reset();
            if (laneSlot_ != nullptr)
                laneSlot_->drop(); // [T-CMP-37 L2] the scalar was just zeroed: a lane's copy of the old state is dead
            alignA_.fill (0.0f); // F1's own past goes with F1
            if (right_ != nullptr) // [stereo voice S3] the right twins with their left ones
            {
                right_->f1.reset();
                right_->alignA.fill (0.0f);
            }
            alignPos_ = 0;
            resetFilter2Path();
            settleFilter2Topology(); // [row 317] nothing is sounding to crossfade from
            ceiling_.reset(); // [D-M-0511] the detectors are audio state
            ceilingGain_ = 1.0f;
            ceilingTail_ = false;
            ceilingCatch_ = false;
        }

        // [T-CGUI-79 A3] AUDIO THREAD: latches the requested F2 On/route, so every reader of `f2On_`/`route_`
        // sees one value per sample. The clean-up for a new topology is NOT here: the row-317 follower
        // (`followFilter2Request`) does it at the switch sample, after F2's contribution has faded out.
        // Steady state: two loads, one compare.
        void applyFilter2Edge() noexcept
        {
            if constexpr (fork::kExact) // FIREFORK E9
            {
                if (!filterReqDirty_.load (std::memory_order_acquire))
                    return;
                filterReqDirty_.store (false, std::memory_order_relaxed);
            }
            f2On_ = f2OnReq_.load (std::memory_order_relaxed);
            route_ = routeReq_.load (std::memory_order_relaxed);
            latchBalanceRequest();
        }

        // [T-CGUI-79 S5R-7] AUDIO THREAD: a balance request that moved restarts the 10 ms ramp from the gains
        // applied NOW. Steady state: one load, one compare.
        void latchBalanceRequest() noexcept
        {
            const float b = balanceReq_.load (std::memory_order_relaxed);
            // NOLINTNEXTLINE(clang-diagnostic-float-equal) — "is this the request I last latched", exactly
            if (b == balance_)
                return;
            balance_ = b;
            gainsTarget_ = balanceGains (b);
            balanceLeft_ = balanceRampLen_;
            balanceStep_ = { (gainsTarget_.f1 - gains_.f1) / static_cast<float> (balanceRampLen_),
                (gainsTarget_.f2 - gains_.f2) / static_cast<float> (balanceRampLen_) };
        }

        // F2's cores and FIR plus F2's Parallel aligner — everything only F2's topologies write. NOT
        // `alignA_`: F1 keeps it current on every sample, and it is F1's real past (see `filterStage`).
        // [stereo voice S3] Per sample, before any twin runs: the right chain runs while it is wanted, and
        // STARTS as a copy of the left — settings and state, which until this sample heard the same signal.
        void followRightChain (bool want) noexcept
        {
            want = want && right_ != nullptr;
            if (want)
            {
                if (!rightRun_)
                {
                    rightRun_ = startRightChain();
                    rightMix_ = 1.0f; // a copy of the left: no step to fade
                }
                else
                    rightMix_ = std::min (1.0f, rightMix_ + rightFadeStep_); // back from a fade-out under way
                return;
            }
            if (!rightRun_)
                return;
            // Not wanted: the right channel crossfades to the left's (`rightBlend`) — its chain's state never
            // has to converge on the left's — and the chain stops once it is the left's exactly.
            rightMix_ -= rightFadeStep_;
            if (rightMix_ <= 0.0f)
            {
                rightMix_ = 0.0f;
                rightRun_ = false;
            }
        }

        // The right channel's value while its chain runs: its own (`rightMix_` 1, exactly), crossfading to the
        // left's as the chain stops (0, exactly the left).
        [[nodiscard]] float rightBlend (float r, float l) const noexcept
        {
            if (rightMix_ >= 1.0f)
                return r;
            return rightMix_ <= 0.0f ? l : l + rightMix_ * (r - l);
        }

        // Every right twin takes its left twin whole — memberwise, so a member a bank gains later is copied
        // too. No allocation: each twin was prepared alike, so every buffer copies into storage of its own
        // size (pinned by the [stereo-chain][rt] case). The copy assignments are not DECLARED non-throwing,
        // so a throw is caught rather than reaching `noexcept`; it cannot happen, and if it did the voice
        // would simply stay mono (false: the right chain does not start).
        bool startRightChain() noexcept
        {
#if __cpp_exceptions // FIREFORK: the firmware has exceptions off; the copy cannot throw there
            try
#endif
            {
                RightChain& r = *right_;
                pullClassicFromLane(); // [T-CMP-37 L2] the twin copies F1's CURRENT state, which may live in a lane
                pullClassic2FromLane(); // [T-CMP-37 L3] …and F2's
                r.hpf = hpf_;
                r.srr = srr_;
                r.loopErr = loopErr_;
                r.drift = drift_; // the same seeded wow and flutter: the two channels drift together
                r.gritDry = gritDry_;
                r.reson = reson_;
                if constexpr (fork::kExact) // FIREFORK B5
                {
                    r.f1.assignForTwin (f1_);
                    r.f2.assignForTwin (f2_);
                }
                else
                {
                    r.f1 = f1_;
                    r.f2 = f2_;
                }
                r.alignA = alignA_;
                r.alignB = alignB_;
                if (foldPos_ == FoldPosition::Pre || stereoHold_ == 0)
                    foldR_ = fold_; // at Post while the pair is held, the twin is already running
                return true;
            }
#if __cpp_exceptions
            catch (...) // NOLINT(bugprone-empty-catch) — the alternative is std::terminate on the audio thread
            {
                return false;
            }
#endif
        }

        void resetFilter2Path() noexcept
        {
            f2_.reset();
            if (laneSlot2_ != nullptr)
                laneSlot2_->drop(); // [T-CMP-37 L3] F2's scalar was just zeroed: a lane's copy of the old state is dead
            alignB_.fill (0.0f);
            if (right_ != nullptr) // [stereo voice S3]
            {
                right_->f2.reset();
                right_->alignB.fill (0.0f);
            }
        }

    public:
        static constexpr float kNotDelivered = CathodeFilterBank::kNotDelivered;

    private:
        static constexpr float kHpfQ = 0.70710678f; // [T-CMP-01] Butterworth: non-resonant, no peak

        // The §5 texture layer is "live" (and its per-sample block runs) only when the noise or the
        // resonator is audible. Recomputed control-rate from the primitives' clamped params in the
        // level/amount setters, so a cleared layer (both 0) restores the bit-exact bypass.
        void refreshTextureLive() noexcept
        {
            // [T-CGUI-08] …and not while a finished bypass has parked it (a fade still runs the block).
            // A SILENT layer cannot fade — the block does not run — so a pending fade settles here at
            // once. Otherwise a bypass tapped at level 0 would hold 10 ms of full-gain texture until a
            // later level raise released it from a stage the LED calls bypassed (S5R).
            const bool audible = noise_.level() > 0.0f || reson_.amount() > 0.0f;
            if (!audible && textureFade_.fading())
            {
                textureFade_.settle();
                if (textureFade_.bypassed())
                {
                    reson_.reset();
                    editRight ([] (RightChain& r) noexcept { r.reson.reset(); });
                }
            }
            textureLive_ = !textureFade_.parked() && audible;
        }

        CathodeOscCore osc_;
        SampleRateReducer srr_;
        BrokenConverter grit_;
        LoopError loopErr_;
        ConverterDrift drift_;
        TptSvf hpf_; // [T-CMP-01] the pre-converter series high-pass
        bool hpfLive_ = false;
        NoiseSource noise_;
        MetallicResonator reson_;
        lanes::ClassicLaneSlot* laneSlot_ = nullptr; // [T-CMP-37 L2] F1's seat in the engine's lane batch (null: scalar only)
        lanes::ClassicLaneSlot* laneSlot2_ = nullptr; // [T-CMP-37 L3] F2's seat in the engine's second batch
        CathodeFilterBank f1_; // [T-CGUI-60] F1 — the §3 filter (drive · core · self-osc limiter)
        CathodeFilterBank f2_; // [T-CGUI-60] F2 — the same bank, processed only while it sounds
        // [stereo voice S3] THE RIGHT CHAIN — a twin of every stateful stage the left runs, run only while
        // `rightRun_` (see `followRightChain`); each starts as a copy of its left twin. Heap, made by prepare.
        struct RightChain
        {
            TptSvf hpf;
            SampleRateReducer srr;
            LoopError loopErr;
            ConverterDrift drift;
            std::vector<float> gritDry; // the GRIT dry leg's ring (`gritDryPos_` and the delay are shared)
            MetallicResonator reson;
            NoiseSource noiseSide; // [S5] the noise's second stream (NOT a copy of the left: its own seed)
            CathodeFilterBank f1;
            CathodeFilterBank f2;
            std::array<float, 256> alignA {}; // the Parallel aligners' twins (`kAlignMask + 1`)
            std::array<float, 256> alignB {};
        };
        std::unique_ptr<RightChain> right_;
        bool rightRun_ = false; // the right chain ran for the last sample (and `editFilter*` writes it)
        float rightMix_ = 0.0f; // the right channel's share of its own chain (see `rightBlend`)
        float noiseWidth_ = 0.0f; // [stereo voice S5] see setNoiseWidth — APPLIED, gliding to the target
        float noiseWidthTarget_ = 0.0f;
        float noiseWidthStep_ = 1.0f / 240.0f; // per sample (prepare)
        static constexpr Wide kWidthGlideSeconds = 0.005; // = CathodeOscCore's stack fade, the unison width's glide
        static constexpr std::uint32_t kNoiseDefaultSeed = 0x1u; // NoiseSource's own default seed
        std::uint32_t noiseSeed_ = kNoiseDefaultSeed; // [S5R 6] the last setNoiseSeed / setNoiseType, for prepare()
        NoiseType noiseType_ = NoiseType::White;
        static constexpr std::uint32_t kNoiseSideSalt = 0x5A17E5EDu; // the side stream's seed = the noise's ^ this
        float rightFadeStep_ = 1.0f / 480.0f; // 10 ms out (prepare)
        bool forceRight_ = false; // `forceRightChain` — the test seam
        bool f2On_ = false; // latched REQUEST — audio thread only (`applyFilter2Edge`)
        FilterRoute route_ = FilterRoute::Series; // latched REQUEST — audio thread only
        std::atomic<bool> filterReqDirty_ { true }; // FIREFORK E9
        std::atomic<bool> f2OnReq_ { false }; // REQUESTED — `setFilter2On`, any thread
        std::atomic<FilterRoute> routeReq_ { FilterRoute::Series }; // REQUESTED — `setFilterRoute`, any thread
        // [row 317] What SOUNDS, following the latched request through `f2Fade_` (bypassed = F2's
        // contribution faded out to F1 alone), and the balance ramp.
        bool f2Sounding_ = false;
        bool f2Parallel_ = false;
        bool f2Stereo_ = false; // [stereo voice S2] the sounding topology is Stereo (F1 left / F2 right)
        bool split_ = false; // [stereo voice S2] the last `filterStage` sample's L and R are two signals
        bool ceilingTail_ = false; // [S5R B31 dsp2 F1] applying the stage ceiling: F2 sounds, or its held tail has not let go
        bool ceilingCatch_ = false; // [S5R B31 final F6] F2 (re-)entered: the gain walks DOWN to the detectors' until it meets it
        ResonanceCeiling ceiling_; // [D-M-0511 / D-M-0373] the filter stage's level-dependent ceiling
        float ceilingGain_ = 1.0f; // the gain it applied to the last sample (a read-back)
        float ceilingTailRise_ = 1.0f / 960.0f; // [S5R B31 dsp2 F1] the tail's minimum rise per sample (prepare)
        int stereoHold_ = 0; // [stereo voice S2] see stereoActive()
        BypassFade f2Fade_;
        float balance_ = 0.0f; // latched REQUEST — audio thread only (`latchBalanceRequest`)
        std::atomic<float> balanceReq_ { 0.0f }; // REQUESTED — `setFilterBalance`, any thread [S5R-7]
        BalanceGains gains_ { 1.0f, 1.0f }; // applied this sample
        BalanceGains gainsTarget_ { 1.0f, 1.0f };
        BalanceGains balanceStep_ { 0.0f, 0.0f };
        int balanceLeft_ = 0;
        int balanceRampLen_ = 480;
        // The aligners' read distances (samples), gliding to `alignTarget*` — see `glideAlignLags`.
        static constexpr Wide kAlignGlide = 1.0 / 16.0;
        Real lagA_ = 0.0;
        Real lagB_ = 0.0;
        bool filterFresh_ = true; // no sample through `filterStage` since the last settle
        // The Parallel aligners — a power of two above any bank's latency (drive + an OS core is tens
        // of samples; the read distance is clamped to the mask all the same). Fixed-size: no allocation.
        static constexpr std::size_t kAlignMask = 255;
        std::array<float, kAlignMask + 1> alignA_ {};
        std::array<float, kAlignMask + 1> alignB_ {};
        static_assert (std::tuple_size_v<decltype (RightChain::alignA)> == kAlignMask + 1, "the right twins' aligners");
        std::size_t alignPos_ = 0;
        bool textureLive_ = false;
        float oscTap_ = 0.0f; // [T-CGUI-15] see oscTap()
        float textureTap_ = 0.0f; // [T-CGUI-15] see textureTap()
        // [T-CGUI-08] the stage bypass fades — see `setTextureBypassed`, `setGritBypassed`,
        // `setFilterBypassed`
        BypassFade textureFade_;
        BypassFade gritFade_;
        // [T-CGUI-09] `synth.grit.amount` and the latency-matched dry leg — see setGritAmount.
        static constexpr Real kGritDryLimitMs = 50.0;
        static constexpr Real kGritDelaySlewSeconds = 0.2; // the dry-delay glide (see processSample)
        static constexpr Real kCeilingTailSeconds = 0.02; // [S5R B31 dsp2 F1] 0 -> 1 at the slowest: 0.5 -> 1 in 10 ms
        Real gritDryDelay_ = 0.0;
        Real gritDelaySlew_ = 1.0;
        float gritAmount_ = 1.0f;
        std::vector<float> gritDry_ = std::vector<float> (2, 0.0f); // safe before prepare
        std::size_t gritDryMask_ = 1;
        std::size_t gritDryPos_ = 0;
        Real gritDryLimit_ = 0.0;
        BypassFade filterFade_;
        // [T-CGUI-64] the wavefolder (`synth.fold.*`) — see setFoldAmount / setFoldPosition
        Wavefolder fold_;
        Wavefolder foldR_; // [stereo voice S2] the right channel's Post fold — see stereoActive()
        float foldAmount_ = 0.0f;
        bool foldLive_ = false;
        FoldPosition foldPos_ = FoldPosition::Pre;
        FoldPosition foldTarget_ = FoldPosition::Pre;
        NoiseRoute noiseRoute_ = NoiseRoute::Parallel; // [D-M-0509] see setNoiseRoute
        BypassFade routeFade_; // [D-M-0512] the route crossfade; gain = the Through share (seated by prepare)
        BypassFade foldFade_; // the Pre/Post move fade (bypassed = faded out for a move)
        // §5 resonator excitation balance (synth.reson.mix): default balanced (0.5) → both weights 1.0
        // → the excitation is the built `oscMix + noiseOut` bit-exact (the M1-18b build preserved).
        float resonMix_ = 0.5f;
        ResonExcite resonExcite_ { 1.0f, 1.0f };
    };
}
