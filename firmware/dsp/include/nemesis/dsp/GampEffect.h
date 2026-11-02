#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/EnvelopeFollower.h>
#include <nemesis/dsp/FxEffect.h>
#include <nemesis/dsp/OnePole.h>
#include <nemesis/dsp/Oversampler.h>
#include <nemesis/dsp/Svf.h>
#include <nemesis/dsp/Waveshaper.h>

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace nemesis::dsp
{
    // [T-FX-08 leaf 1] GAMP — the "AMP" instance type of the INSTRUMENT FX chain, and the last
    // of the ADR-038 D6 "declared-and-unbuilt" gaps that looks fully shipped from every angle
    // except the audio one: eleven registry rows, a `tid`, a host handle pair, unit curves —
    // and `makeFxEffect` had no arm, so a patch could turn it on, dial every row and hear
    // nothing change.
    //
    // `docs/specs/specs/dsp/amp_modelling.md` is the authority and it is a FULL spec (347
    // lines, third in the series with the delay/reverb specs, same "build §0 once, each
    // voicing is a CONFIGURATION of the same chain" shape). §0.1's chain, which is the
    // structure of this class and of the card's leaf plan:
    //
    //   in → [input HP] → [PREAMP: N × (HP couple → +bias → soft clip → plate LP)]   ← leaf 1
    //      → [tone stack: bass/mid/treble]                                           ← leaf 2
    //      → [power amp: push-pull clip + rail SAG] → [presence]                      ← leaf 4
    //      → [cabinet + mic]                                                          ← leaf 3
    //      → [DC block] → [per-model makeup] → level → dry/wet → out
    //
    // THE ONE OWNER RULING THAT SHAPES ALL OF IT (2026-08-10): the cab/mic stage is
    // ALGORITHMIC — a fixed EQ curve per combination, §0.6's second bullet ("analog speaker
    // sim (no IR)") — and NOT convolution. GAMP is the cheap/classic amp-sim type; T-FX-06
    // (NAM, neural capture) is the future home for faithfully-modelled real-amp fidelity, so
    // this type carries none of that weight and needs no IR assets and no licence diligence.
    // The preamp is the other half of the ruling: a per-model MULTI-STAGE cascade, not one
    // shared curve.
    //
    // THIS CLASS OWNS NO NEW NONLINEARITY AND NO NEW FILTER. §0.3 ranks its transfer-function
    // options and the cheapest tier is "DC offset → soft-clip → remove DC"; `AdaaSoftClip`
    // (M1-02) is that soft clip, already Tier-A proven and already the pick Tape's record head
    // and T-CMP-08c's Analog leaf made for the same fork. `Oversampler` (M1-02b) is §0.2's
    // up/down cascade. What lives HERE is the DISPATCH, the per-model CONFIGURATION table, the
    // unit conversions and the stage ORDER — which is why the card's policy is FOCUSED.
    //
    // WHAT EACH ROW MEANS ON THE WAY DOWN (`ParamRegistry.cpp`'s `gamp.*` block and
    // `UnitCurves.cpp`'s matching curve rows are the authority for the unit; this class is the
    // authority for the conversion):
    //
    //   `model`    ENUM ordinal (`kAmpModels`, 6 values) → `Model`. The index IS the patch,
    //              and it selects a whole row of `kVoicing` — §8's "voicing selection summary"
    //              as a table.
    //   `gain`     dB (`kDriveDb`, 0–36) → the cascade's input pre-gain, through
    //              `preGainFor` — PER MODEL, because §8's gain structures differ by ~20 dB
    //              across the six and one shared curve would make Clean a fuzz box or
    //              Hi-gain a clean channel.
    //   `level`    dB (`kLevelDb`, −∞…+12, unity at plain 100) → `levelGainFor`, the §1.3–§6.3
    //              "Master" row every voicing table has.
    //   `mix`      % (`kPercent`) → the wet/dry blend. `mix = 0` is the input bit-exactly,
    //              DELAYED by `latencySamples()` (T-FX-24's dry line — see `process`).
    //   `bass`     % (`kPercent`) → [leaf 2] §0.4's tone stack, one section each, through
    //   `mid`      `bandGainForPercent`: 0 % is a full cut, 50 % is EXACTLY unity and 100 % a
    //   `treble`   full boost, over a per-model dB range. The CENTRES are per model too — §0.4's
    //              Fender-scoop versus Marshall-mid-forward voicings, `kTone`.
    //   `cab`      ENUM ordinal (`kCabTypes`, 6 values) → [leaf 3] §0.6's speaker sim, a biquad
    //              cascade per cabinet. `Direct` is a bit-exact BYPASS.
    //   `mic`      ENUM ordinal (`kMicTypes`, 3 values) → [leaf 3] a MODIFIER on the cabinet
    //              rather than a second table: see THE CABINET below.
    //   `presence` % (`kPercent`) → [leaf 2] §0.5's presence, which is NOT a fourth tone band:
    //              it is the HF shelf that emerges from reducing negative feedback around the
    //              power amp, so it sits at BASE rate after the power-amp position rather than
    //              inside the tone stack. On Acoustic it is §6.3's "air / sparkle" shelf, which
    //              is the same filter at a much higher corner.
    //
    // `on` is the CHAIN's bypass (a graph-node atomic) and is ignored here exactly as in every
    // sibling effect; every other row of the block is wired.
    //
    // ============================= THE PREAMP CASCADE =============================
    // §0.3's tube gain stage, at the cheapest fidelity tier §0.3 itself ranks first:
    //
    //   stage i:  HP couple (the coupling cap) → ×stage gain → +grid bias → soft clip
    //             → plate-load LP (Miller) → (the next stage's HP removes the injected DC)
    //
    // THREE THINGS IN THAT LINE ARE THE SPEC'S OWN EMPHASIS, not decoration.
    //   * THE BIAS IS THE ASYMMETRY. §0.3: "apply a DC offset, pass through tanh/soft-clip,
    //     remove DC. Asymmetry → EVEN harmonics (the tube warmth)." `AdaaSoftClip::shape` is
    //     odd, so it makes only odd harmonics on its own; the offset is what buys the even
    //     ones, and §0.7's DC blocker is what keeps the offset itself out of the output.
    //   * THE INTER-STAGE HP IS THE TIGHTNESS. §0.3: "inter-stage HP filtering between every
    //     stage … the most important tone-sculpting lever in high-gain design", and §4.2 makes
    //     it the whole answer to Hi-gain's "flub" problem — lows removed BEFORE they are
    //     distorted, not after. That is why the corner climbs 12 Hz → 220 Hz across the table.
    //   * THE WHOLE CASCADE RUNS OVERSAMPLED, at the model's own factor (§0.2's table: 2–4×
    //     clean, 4× crunch, 4–8× lead, 8–16× hi-gain, and Acoustic needs none at all because
    //     §6.2 says the chain is essentially linear). ONE up/down cascade wraps the entire
    //     nonlinear core rather than one per stage — the M1-09fh `ScreamFilter` idiom, a
    //     stateful shaper lambda captured by reference so the template inlines with no
    //     `std::function` allocation.
    //
    // ACOUSTIC HAS ZERO STAGES, and that is §8's own row ("Acoustic | none (linear)") plus
    // §6's opening sentence ("NOT a distortion model"). So the arithmetic that makes every
    // other model an amp is skipped entirely: no clip, no oversampler, no group delay. Its
    // character is the linear chain leaf 5 gives it.
    //
    // ============================== THE TONE STACK ================================
    // [leaf 2] §0.4, and the two decisions it forces.
    //
    // WHERE IT SITS: "position the stack AFTER the first gain stage(s) (where real amps put it)
    // so it shapes what feeds later gain — this matters for how the distortion sounds, not just
    // the final EQ." So the stack runs between stage 1 and stage 2, INSIDE the oversampled
    // region, and the later stages distort what it left behind. That is also why its corners
    // are resolved at `factor · fs` like the cascade's own.
    // ponytail: it costs three `TptSvf` frames per OS sample rather than three per base sample —
    // 24 frames a sample on Hi-gain at 8×. The alternative is a second up/down FIR cascade to
    // get back to base rate and up again (§0.2 allows EQ at base rate), which is more
    // arithmetic than the three filters it would save AND puts the stack after all the gain,
    // where §0.4 says it does not belong. If the cost ever matters, the honest measurement is
    // the T-SIB-01 tracer's.
    //
    // WHAT A SECTION IS: §0.4 asks for the real passive RC network ("not three independent
    // bands … the controls interact"). [D-M-0305] The five amp voicings now HAVE it — see
    // `ToneNetwork`: the rows drive Yeh's interactive stack, relative to its own noon. The
    // sections below remain for Acoustic (whose EQ is active, not a passive stack) and, on the
    // amps, carry only the noon `midOffsetDb` bell. One shelf or bell per row, built out of
    // the signed-off `TptSvf`'s simultaneous taps:
    //
    //   low shelf   y = x + (A−1)·lp     (DC gain EXACTLY A, HF gain 1)
    //   bell        y = x + k·(A−1)·bp   (gain at the centre EXACTLY A, k = 1/Q)
    //   high shelf  y = x + (A−1)·hp     (HF gain A, DC gain 1)
    //
    // Two properties make that worth the honesty: each band's gain is EXACT where the claim is
    // made (so the tests pin numbers rather than directions), and A = 1 at the row's noon makes
    // every section bit-exactly transparent — `x + 0·tap` is `x`, so a neutral stack cannot
    // perturb anything. What the sections alone do NOT reproduce is the INTERACTION (a real
    // Fender stack's treble control moves the mid scoop) — the network does, on the amps. The
    // per-model noon voicing stays here: `kTone`'s `midOffsetDb` is §0.4's Fender-scoop versus
    // Marshall-mid-forward as data, and §3.2's lead "hump" and §4.3's "mid scoop" as a sign.
    // ==============================================================================
    //
    // ==================== THE TWO VOICINGS THAT ARE NOT A CASCADE ==================
    // [leaf 5] §5 and §6 do not describe a differently-configured amp; they describe a different
    // TOPOLOGY, and both are structural rather than a table row.
    //
    // BASS — §5.1's BAND SPLIT, which §5.2 calls "*the* bass-distortion technique": "distorting
    // the full-range signal turns the low end to mush and intermodulation. Split at ~150–500 Hz,
    // distort only the highs, keep lows clean, recombine." So on this one voicing the preamp
    // cascade is fed the HIGH band only and the low band is added back afterwards, with §5.1's
    // tone stack and power amp downstream of the recombine (which is also why the stack cannot
    // simply stay where §0.4 put it for the other five).
    //   The split is an LR4 CROSSOVER — two cascaded Butterworth sections per half, so each half
    //   is 24 dB/oct and the two sum to an allpass (flat in MAGNITUDE, phase rotated). It is NOT
    //   the complementary `x − lp` subtraction that looks cheaper and reconstructs bit-exactly:
    //   that ISOLATES NOTHING, because near the corner a low-pass's PHASE dominates its
    //   magnitude. At 60 Hz against a cascaded 250 Hz low-pass the magnitude is 0.997 and the
    //   phase −38°, so `|1 − 0.997∠−38°|` is 0.65 — the fundamental is two thirds present in the
    //   "high" band, takes §5.3's 20 dB of high-band gain and clips. Measured 0.69 THD at 60 Hz,
    //   the arithmetic and the render agreeing; two cascaded high-passes leave it 49 dB down.
    //   `gain` is applied to the HIGH BAND only, which is §5.3's own wording for that row.
    //   `kBassXoverHz` is §5.3's own default (250 Hz, mid-range of its 100–600 Hz row) and has
    //   NO REGISTRY ROW — nor do §5.3's low/high blend and DI blend. Fixed constants, ledgered:
    //   `mix` already offers a parallel clean path, which is most of what a DI blend is for.
    //
    // ACOUSTIC — §6.1's linear preamp, and §6's opening sentence is the whole design: "NOT a
    // distortion model … the job is transparency, fullness, and feedback control, turning a
    // quacky piezo pickup into a natural acoustic tone." It has no gain stages (leaf 1) and no
    // power amp (leaf 4); what it has instead is §6.2's two corrective sections —
    //   * PIEZO DE-QUACK: "under-saddle piezos sound thin and quacky with a harsh 2–5 kHz spike
    //     and weak lows. A corrective EQ (cut the spike, gentle low shelf) … is the core
    //     transformation." So: a cut bell in the middle of that band, plus a low shelf.
    //   * BODY RESONANCE: "model the main air/top resonances ~90–250 Hz to reintroduce the
    //     dimensionality a pickup misses. This is the closest thing here to a cab, but it's a
    //     guitar body, not a speaker."
    // §6.3's "air / sparkle" row is NOT a new section — it is `presence`, which leaf 2 already
    // put at 8 kHz for this voicing. §6.3's anti-feedback notches and phase rotation default to
    // OFF in the spec's own table and have no row either, so they are not built (ledgered): a
    // notch nobody can tune is a hole in the response, and the feedback loop it exists to break
    // does not exist inside a DAW.
    // ==============================================================================
    //
    // ============================= THE POWER AMP AND SAG ===========================
    // [leaf 4] §0.5, and §7's second row is why it exists at all: "distortion is DISTRIBUTED —
    // spread clipping across preamp + power amp + transformer + speaker LP. A single big
    // waveshaper into a cab IR always sounds stiff and fizzy." So the last thing inside the
    // oversampled region is a SECOND nonlinearity, and it is a different one:
    //
    //   rail = 1 − sag · env(|previous output|)          the B+ droop
    //   y    = rail · clip(x · powerDrive / rail)        a class-AB pair inside that rail
    //
    // SYMMETRIC, unlike the preamp's stages: a push-pull pair clips both halves alike, so there
    // is no bias, no injected DC and nothing to prime — silence in is exactly silence out by
    // construction.
    //
    // THE SAG IS THE WHOLE POINT, and §0.5 says what it is: "model the B+ rail as a voltage that
    // droops when signal current is high, recovering over an RC time (~10–100 ms). Under hard
    // playing the amp compresses then blooms back — the dynamic touch response players chase."
    // Dividing the clip's input by the rail as well as scaling its output by it is what makes the
    // droop a HEADROOM change rather than a volume change: the threshold comes down, so a
    // constant loud input gets progressively more compressed over the attack and lets go over the
    // release. That is a claim a memoryless clipper cannot satisfy at all, which is exactly why
    // the test measures the DROOP WITHIN a constant burst rather than a static curve.
    //
    // `dsp::EnvelopeFollower` (T-PRIM-02) is the detector, unchanged — and this is the FEEDBACK
    // tap of its §12-capability-2 contract, the half `FltrEffect` did not use: the detector is
    // handed the previous sample's own OUTPUT, because a rail droops with the current the amp is
    // actually delivering. ONE follower and ONE rail for both channels, linked on the max of the
    // two (compression.md §0.7: "derive ONE control signal from both channels … independent
    // per-channel gain shifts the image"), which is also the cheaper arrangement.
    //
    // PER MODEL, and this is the field where §8's voicings disagree most sharply: §2.2 makes
    // Crunch's sag "prominent and fast-ish" (it is where the touch response comes from), §3.2
    // gives Lead "heavy, slow sag" for singing sustain, and §4.2 voices Hi-gain "tight (fast/no
    // sag) — modern hi-gain prefers definition over bloom". Acoustic has no power amp at all,
    // like it has no preamp (§6: "not a distortion model"), so the whole stage is skipped for the
    // one voicing with no gain stages.
    //
    // [D-M-0304] §0.5's other two power-amp colours, per model and with no registry row (the
    // §0.1 "as built" rule: a per-voicing control the knob set lacks is a constant):
    //
    //   v = x · powerDrive / rail
    //   v −= xoverDepth · v · (1 − (v/w)²)³   for |v| < w          CROSSOVER, the AB edge
    //   z = rail · clip(v)
    //   φ = n · k · bp_{fF, Q=½}(z)                                the core FLUX
    //   y = z − e(φ),  e(φ) = φ − S(g·φ)/g,  S(u) = u − u⁹/9 (±8/9 for |u| ≥ 1)  TRANSFORMER
    //
    // THE TRANSFORMER. Core flux is the time-integral of the winding voltage, so for the same
    // amplitude a lower note drives the core harder (|φ| ∝ 1/f) — that is what makes the
    // saturation LF-only. `φ` is the band-pass at `kXfmrFluxHz` (Q = ½, two coincident poles):
    // above the corner it IS the leaky integral (−6 dB/oct), below it the push-pull pair's DC
    // cancellation and the primary's own inductance take it back down, so a DC offset from an
    // asymmetric preamp cannot park the core in saturation. `n` normalises it so a sine at
    // `kXfmrRefHz` (a bass's low E) reads its own amplitude. When the core saturates, the
    // magnetising current the source impedance has to supply grows faster than linear and the
    // voltage it drops subtracts from the output: `e(φ)`, the part of the flux the core could not
    // carry. `S` is a NINTH-order knee (C1 at |u| = 1) rather than the cubic the clip uses,
    // because the knee has to be ABSENT below high level — `e ∝ φ⁹` is −50 dB at half the knee,
    // which is what keeps §5.2's clean low band clean at ordinary levels (`[gamp][split]`) — and
    // present at it: low-end compression plus odd harmonics of the low notes, §5.2's SVT "growl".
    // Feed-forward (the flux of the voltage the pair delivers, not of the output), so there is no
    // loop to keep stable. `g = xfmrDrive` is 1/(the low-E amplitude at the knee); 0 skips the
    // stage bit-exactly.
    //
    // THE CROSSOVER. A push-pull pair biased class AB hands over between its two halves around
    // zero, where the summed transconductance dips: the transfer's SLOPE at the origin is
    // `1 − xoverDepth`, and it rejoins the straight line at `|v| = kXoverWidth` (grid units, where
    // the clip's knee is 1). The bump is a polynomial with a triple root at the edge, so the curve
    // is C2 there and its harmonics fall off fast — it runs inside the oversampled region like
    // everything else here, and through first-order ADAA (its antiderivative is a polynomial too),
    // because a LOUD note crosses the band in a fraction of an OS sample and the bump would
    // otherwise land as a broadband spike. LEVEL-DEPENDENT by construction: a quiet note spends its whole cycle
    // inside the band and a loud one a sliver of it, so it is the "edge" on a decaying clean
    // note and nothing on a driven one. 0 skips it bit-exactly.
    //
    // `setOutputStage(false, false)` is the A/B seam (render/test only, no host param): it forces
    // both to 0, which is the pre-D-M-0304 power amp bit for bit (pinned).
    // ==============================================================================
    //
    // ============================ THE CABINET AND THE MIC ==========================
    // [leaf 3] §0.6, and the owner's ruling: ALGORITHMIC, not convolution. §0.6 offers exactly
    // two constructions and the second one is what this is — "analog speaker sim (no IR):
    // cascade of biquads — LF resonance peak ~80–120 Hz, broad presence peak ~1.5–4 kHz, steep
    // LP ~4–6 kHz (the speaker's natural top limit — kills fizz), one or two notches for
    // cone-breakup dips". Four sections, in that order, over the same signed-off `TptSvf` the
    // tone stack uses, at BASE rate (§0.2: "cab convolution and most EQ can run at base rate").
    // §7's own table is the reason this stage matters more than its size suggests: "most amp
    // tone is the speaker+mic … a bad/absent cab makes the best preamp sound like a fuzz pedal."
    //
    // THE MIC IS A MODIFIER, NOT AN 18-ROW TABLE. Six cabs × three mics is eighteen responses,
    // and writing eighteen of them would be eighteen chances to transcribe one wrong for no new
    // degrees of freedom: a microphone does the same three things whatever it is pointed at — it
    // has its own presence peak, its own top-end reach and its own low-end weight. So `kMic`
    // carries a bell, a MULTIPLIER on the cab's LP corner and a low shelf, and the eighteen
    // combinations are the product of the two tables. The distinctness claim is then asserted
    // over all of them rather than assumed from the table's shape.
    //
    // `Direct` IS A BIT-EXACT BYPASS, AND THE MIC GOES WITH IT. §0.6's roster ends in a DI, which
    // is by definition a signal that never met a speaker — so the whole stage early-returns, and
    // that makes "a DI has no microphone" an EXACT claim (the three `Direct` combinations are
    // bit-identical) rather than a nearly-inaudible one. It is also the anchor that keeps a
    // cabinet from being something the class cannot be measured without. [D-M-0513] A switch to or
    // from it on a sounding note CROSSFADES the stage over `kGlideMs` (`gCabMix_`, the box's share),
    // fading out on the last real box (`voicedCab_`); settled on `Direct` it early-returns again.
    // ==============================================================================
    //
    // A LEVEL MAKEUP PER MODEL, PICKED UP FRONT. T-FX-09 shipped leaf 1 without one and
    // measured 8× the dry RMS on a driven setting, then spent leaf 1b fitting the stage it
    // should have had; the lesson is cheaper the second time. Here the soft clip BOUNDS the
    // cascade (each stage's output is inside ±2/3 by construction), so the risk is the
    // opposite one — a hot model sitting well ABOVE a quiet one — and `kVoicing[].makeupDb` is
    // the measured per-model trim that lands all six within a few dB of each other at their
    // shipped rows. It is NOT gain-invariance: turning the gain row up on a clean channel is
    // SUPPOSED to get louder, and `level` is the master that answers that.
    // ==============================================================================
    //
    // DUAL MONO, and that is the whole stereo story: every stage is mono, so this holds two
    // identically-configured channel chains and runs one per channel (`FltrEffect`'s and
    // `PhaserEffect`'s answer to the same shape). NOTHING EVER CROSSES CHANNELS.
    //
    // RT-safety: `process`/`reset` allocate nothing — every model's `Oversampler` is built in
    // `prepare` (one per model per channel, because a live `model` edit must not be the moment
    // one is designed), and a live `model`/`gain`/`level`/`mix` edit only recomputes
    // coefficients. Latency is the ACTIVE model's oversampler group delay — see
    // `latencySamples`.
    class GampEffect final : public FxEffect
    {
    public:
        // `gamp.model`'s roster, in `ParamRegistry.cpp`'s `kAmpModels` STORAGE ORDER, which is
        // frozen: the ordinal is what a patch stores.
        enum class Model : std::uint8_t {
            Clean = 0,
            Crunch,
            Lead,
            HiGain,
            Bass,
            Acoustic
        };
        static constexpr int kNumModels = 6;

        // §0.3's cascade, as data — one row per `Model`, in the same storage order. Every
        // number is an OWNER-TUNABLE DEFAULT in the M1-17f sense (pick, document, don't ask):
        // amp_modelling.md gives the SHAPE of each field (§0.2's OS table, §0.3's 2–30 Hz
        // couplings and 10–20 kHz plate roll-off, §8's stage counts) and a range rather than a
        // figure, so these are the picks, and `dsp/tests/test_gamp_effect.cpp` pins the
        // PROPERTIES they exist for (more stages ⇒ more harmonic density; a higher inter-stage
        // corner ⇒ less low-frequency intermodulation) rather than the numbers themselves.
        struct Voicing
        {
            int stages; // §8's "gain stages" column. 0 = linear (Acoustic).
            int oversample; // §0.2's factor. 1 = none (Acoustic).
            Real gainOffsetDb; // the `gain` row, offset per model — the gain STRUCTURE.
            Real stageGainDb; // inter-stage gain, applied at every stage's input.
            Real bias; // §0.3's grid-bias offset → asymmetry → even harmonics.
            Real interHpHz; // §0.3's coupling HP → §4.2's "tightness".
            Real stageLpHz; // §0.3's plate-load LP (Miller capacitance).
            Real makeupDb; // the measured per-model output trim (see the note above).
            // [leaf 4] §0.5's power amp. `stages == 0` skips the whole stage, so these are
            // unread on the linear voicing.
            Real powerDriveDb; // how hard the class-AB pair is pushed
            Real sagAmount; // how far the B+ rail droops at full detected level, ∈ [0, 1)
            Real sagAttackMs; // …and its RC, §0.5's 10–100 ms
            Real sagReleaseMs;
            // [D-M-0304] §0.5's output transformer — 1/(the core flux at saturation) — and the
            // class-AB crossover's slope dip at zero. Both 0 = that colour is not in the chain.
            Real xfmrDrive;
            Real xoverDepth;
        };

        [[nodiscard]] static const Voicing& voicingFor (Model m) noexcept;

        // [leaf 2] §0.4's stack and §0.5's presence, as data — one row per `Model`, same storage
        // order and the same owner-tunable-default status as `Voicing` (§0.4 names the
        // topologies and their character, not figures).
        struct Tone
        {
            Real bassHz; // the low shelf's corner
            Real midHz; // the bell's centre — §0.4's Fender/Marshall/bass voicings
            Real midQ; // its width; wider on the clean voicings, tighter on the gain ones
            Real trebleHz; // the high shelf's corner
            Real presenceHz; // §0.5's NFB shelf — §6.3's "air" on Acoustic, far higher
            Real rangeDb; // ± the travel each row has about noon
            Real midOffsetDb; // the BASELINE mid tilt: §1's Fender scoop, §3.2's lead hump,
                // §4.3's modern mid-scoop. 0 = a mid-forward Marshall stack.
        };

        [[nodiscard]] static const Tone& toneFor (Model m) noexcept;

        // [D-M-0305] §0.4's PASSIVE NETWORK — the bass/mid/treble rows on the five amp voicings. Yeh & Smith's
        // tone-stack transfer function ("Discretization of the '59 Fender Bassman Tone Stack", DAFx-06): one
        // third-order H(s; l, m, t) whose every coefficient depends on all three pots, so the controls INTERACT
        // (treble up lowers the low mids, bass up scoops the mids) — what three independent sections cannot do.
        // It runs RELATIVE TO ITS OWN NOON, H(l, m, t) / H(noon), so noon is still bit-exactly transparent and the
        // per-model voicing at noon (`kTone`'s `midOffsetDb` bell, the makeup fit) is unchanged. The shared zero
        // at DC cancels, which is what makes the ratio stable to realise: a fixed SVF at the noon zeros, an SVF at
        // two of the stack's three (real, RC) poles and a one-pole at the third, each written `x + c·tap` so a
        // correction is exactly 0 at noon. The component values are each topology's published circuit (the
        // owner-tunable-default status of every table here). Acoustic is not a passive stack (§6.3's active EQ)
        // and keeps the three independent sections; `toneNetworkFor` answers nullptr there.
        struct ToneNetwork
        {
            Real c1, c2, c3; // F: treble, bass, mid caps
            Real r1, r2, r3, r4; // Ω: treble pot, bass pot, mid pot, slope resistor
        };
        [[nodiscard]] static const ToneNetwork* toneNetworkFor (Model m) noexcept;
        // The pot positions the rows drive, ∈ [0, 1]: the bass pot is AUDIO taper (a real stack's), the mid and
        // treble linear. Both clamp at the rows' rails; the bass taper never reaches 0 (it stays off the
        // degenerate network where the cubic's leading term vanishes).
        [[nodiscard]] static Real bassPot (Real percent) noexcept;
        [[nodiscard]] static Real linearPot (Real percent) noexcept;
        // The analytic (s-domain) network response at `hz`, relative to the same model's noon — the reference
        // the tests measure the discretised stack against. Exactly 1 on Acoustic and at noon.
        [[nodiscard]] static std::complex<Real> toneNetworkResponse (Model m, Real bassPercent, Real midPercent, Real treblePercent, Real hz) noexcept;

        // [leaf 3] `gamp.cab`'s roster, in `ParamRegistry.cpp`'s `kCabTypes` STORAGE ORDER.
        enum class Cabinet : std::uint8_t {
            Quad12 = 0, // 4×12, closed back
            Dual12, // 2×12, open back
            Single12,
            Single10,
            Octal10, // 8×10, the bass cab
            Direct // no speaker at all — a bit-exact bypass
        };
        static constexpr int kNumCabs = 6;

        // …and `gamp.mic`'s, same rule (`kMicTypes`).
        enum class Microphone : std::uint8_t {
            Dynamic = 0,
            Condenser,
            Ribbon
        };
        static constexpr int kNumMics = 3;

        // §0.6's four sections, as data — one row per `Cabinet`. Ranges are §0.6's own; which
        // figure inside them belongs to which cabinet is the pick (the M1-17f rule again), and
        // the tests pin the ORDERINGS §0.6 states rather than the numbers.
        struct Cab
        {
            Real lfHz, lfDb, lfQ; // the LF resonance peak (§0.6's 80–120 Hz)
            Real presHz, presDb, presQ; // the broad presence peak (1.5–4 kHz)
            Real lpHz; // the steep top limit (4–6 kHz) — 24 dB/oct
            Real notchHz, notchDb, notchQ; // one cone-breakup dip
        };

        // …and the microphone, as the three things a mic does to whatever it is pointed at.
        struct Mic
        {
            Real bellHz, bellDb, bellQ; // its own presence/air/body peak
            Real lpScale; // how far its top reaches, RELATIVE to the cab's
            Real lowShelfDb; // proximity / weight, at `kMicShelfHz`
        };

        [[nodiscard]] static const Cab& cabFor (Cabinet c) noexcept;
        [[nodiscard]] static const Mic& micFor (Microphone m) noexcept;

        void prepare (Real sampleRate, int maxBlockSamples) override;
        // …and the same thing with §0.2's factor FORCED, which is what the alias-floor
        // keystone needs: `factorOverride = 1` renders the model's own cascade at base rate,
        // the naive reference the oversampled path is measured against (the M1-02b/M1-09fh
        // "factor 1 is the naive baseline" convention). 0 = each model's own factor, which is
        // what the `FxEffect` override above passes.
        void prepare (Real sampleRate, int maxBlockSamples, int factorOverride);
        void reset() noexcept override;
        void process (float* left, float* right, int numSamples) noexcept override;

        // The ACTIVE model's oversampler group delay (0 for Acoustic, which has no
        // oversampler at all). The chain reads this at graph-PUBLISH time for parallel-path
        // alignment (`FxChain.cpp`'s `g.latency`), so it must report what is actually running
        // rather than a constant.
        // ponytail: a `gamp.model` edit AFTER publish therefore leaves a stale alignment,
        // bounded by the OS group delay (16 samples at every factor > 1) — the same ceiling
        // `FltrEffect` names, and republishing on a param edit would mean rebuilding the graph
        // for a knob turn.
        [[nodiscard]] int latencySamples() const noexcept override;

        void setAppliedUnit (std::string_view field, Real value) noexcept override;
        [[nodiscard]] std::optional<Real> appliedUnit (std::string_view field) const noexcept override;

        // Control-rate setters, in REAL UNITS (the T-CORE-06 rule: the plain 0–127 value and
        // its §22.8 curve stay on the core side of the façade — except for the ENUM row, whose
        // plain ordinal IS the value).
        void setModel (int index) noexcept;
        void setGainDb (float db) noexcept;
        // [leaf 2] The tone stack. 50 % is bit-exactly transparent on all four.
        void setBassPercent (float percent) noexcept;
        void setMidPercent (float percent) noexcept;
        void setTreblePercent (float percent) noexcept;
        void setPresencePercent (float percent) noexcept;
        // [leaf 3] §0.6's speaker sim. `Direct` bypasses the whole stage, mic included.
        // (§0.5's power amp and its sag have no registry row at all — they are per-model
        // constants, `kVoicing`'s last four columns. See the DEFERRED ledger.)
        void setCab (int index) noexcept;
        void setMic (int index) noexcept;
        void setLevelDb (float db) noexcept;
        void setMixPercent (float percent) noexcept;
        // FIREFORK F1: process only the left channel and copy it to the right (the caller feeds the mono sum): half the cost
        void setMono (bool on) noexcept { mono_ = on; }
        // [D-M-0304] THE A/B SEAM — render/test only, no host param. `false` forces that colour's
        // per-model amount to 0, i.e. the power amp as it was before D-M-0304, bit for bit.
        // Both default ON: the shipped models carry their `kVoicing` amounts.
        void setOutputStage (bool transformer, bool crossover) noexcept;

        // --- the CONVERSION LAWS, as pure statics ---------------------------------
        //
        // Exposed so a test pins `effective == law(row)` to the bit rather than against a
        // literal (the `FilterDrive::preGainFor` idiom). All of them are total: they clamp
        // rather than fold, because the message-thread edit path has already clamped to the
        // registry range and anything past a rail is a caller bug.

        // (model, `gain` dB) → the LINEAR pre-gain into the cascade's first stage:
        // `10^((db + kVoicing[model].gainOffsetDb)/20)`. PER MODEL because §8's gain
        // structures are what separate the voicings — at the row's shipped default (15.31 dB)
        // the offset puts Clean 1.3 dB into a single stage (§1.2's "gain stages run well below
        // clipping") and Hi-gain 15.3 dB into four (§4's "enormous gain").
        [[nodiscard]] static float preGainFor (Model m, Real gainDb) noexcept;

        // The per-stage inter-stage gain, likewise linear and likewise per model — §0.3's
        // "stage cascade" (2 stages = crunch, 3–5 = lead/hi-gain) is a claim about GAIN as
        // much as about count, so the two travel together.
        [[nodiscard]] static float stageGainFor (Model m) noexcept;

        // dB → linear, the `level` row and the per-model makeup both. The row's own curve
        // reaches −∞ dB at plain 0 (a `CurveKind::Level` row: `40·log10(v/100)`), so this must
        // answer EXACTLY 0 for it rather than a denormal — hence the floor, and hence a level
        // of 0 is real silence.
        [[nodiscard]] static float gainForDb (Real db) noexcept;

        // [leaf 2] (% , ± range in dB) → the LINEAR gain a tone section applies:
        // `10^(((percent/50 − 1)·range + offset)/20)`. NOON IS EXACTLY 1.0f when the offset is
        // zero, and that is load-bearing rather than tidy — a section is `x + (A−1)·tap`, so an
        // A of exactly 1 makes it bit-exactly transparent and a neutral stack cannot perturb a
        // level or a bit-exact claim anywhere else in the class.
        [[nodiscard]] static float bandGainForPercent (Real percent, Real rangeDb, Real offsetDb = 0.0) noexcept;

        // The row RAILS this class converts against. A SECOND COPY of `UnitCurves.cpp`'s own
        // (`gamp.gain` = `kDriveDb` 0–36 · `gamp.level` = `kLevelDb` …+12 · `gamp.mix` =
        // `kPercent` 0–100), because `dsp` sits below `core` and cannot read the registry the
        // way `CathodeMachine::normalised01` does — so `app/tests/test_fx_seam.cpp` asserts
        // them against the real curve's ends instead and the copy cannot drift silently.
        static constexpr Real kGainDbMax = 36.0;
        static constexpr Real kLevelDbMax = 12.0;
        static constexpr Real kMixPercentMax = 100.0;
        // [leaf 2] `gamp.bass`/`mid`/`treble`/`presence` are all `kPercent` 0–100, and noon —
        // the transparent position — is the middle of that, not a separate constant to drift.
        static constexpr Real kTonePercentMax = 100.0;
        static constexpr Real kToneNoonPercent = 50.0;
        // [leaf 5] §5.3's crossover default (its row is 100–600 Hz and this is the middle), and
        // §6.2's two corrective sections. None of the five has a registry row — see THE TWO
        // VOICINGS THAT ARE NOT A CASCADE, and the DEFERRED ledger.
        static constexpr Real kBassXoverHz = 250.0;
        static constexpr Real kPiezoHz = 3200.0; // the middle of §6.2's "harsh 2–5 kHz spike"
        static constexpr Real kPiezoDb = -6.0;
        static constexpr Real kPiezoQ = 1.0;
        static constexpr Real kPiezoShelfHz = 200.0; // §6.2's "gentle low shelf" to restore lows
        static constexpr Real kPiezoShelfDb = 3.0;
        static constexpr Real kBodyHz = 120.0; // §6.2's "main air/top resonances ~90–250 Hz"
        static constexpr Real kBodyDb = 4.0;
        static constexpr Real kBodyQ = 1.2;
        // [D-M-0304] §0.5's two power-amp colours. The flux corner: above it the core flux is the
        // integral of the voltage (|φ| ∝ 1/f), so a bass low E drives the core ~3.5× harder than
        // a 165 Hz note at the same level. The reference the flux is normalised at — a bass's low
        // E — and the crossover band, in the clip's grid units (knee = 1).
        static constexpr Real kXfmrFluxHz = 15.0;
        static constexpr Real kXfmrRefHz = 41.2;
        static constexpr Real kXoverWidth = 0.005;

        // [leaf 3] The mic's low shelf corner — one frequency for all three, because what
        // separates them on this axis is HOW MUCH weight they add, not where.
        static constexpr Real kMicShelfHz = 150.0;
        // Below this the `level` row IS silence (its curve's own −∞ at plain 0, floored).
        static constexpr Real kSilenceDb = -120.0;

        // --- read-backs, in FORCE (tests, and a surface that wants the truth) --------
        [[nodiscard]] Model model() const noexcept { return model_; }
        [[nodiscard]] Real appliedPreGain() const noexcept { return static_cast<Real> (preGain_); }
        [[nodiscard]] Real appliedMix() const noexcept { return static_cast<Real> (wetGain_); }
        // [leaf 2] The four tone gains IN FORCE — the mid's includes the model's baseline tilt,
        // which is the whole point of reading it here rather than recomputing the law.
        [[nodiscard]] Real appliedBassGain() const noexcept { return static_cast<Real> (bassGain_); }
        [[nodiscard]] Real appliedMidGain() const noexcept { return static_cast<Real> (midGain_); }
        [[nodiscard]] Real appliedTrebleGain() const noexcept { return static_cast<Real> (trebleGain_); }
        [[nodiscard]] Real appliedPresenceGain() const noexcept { return static_cast<Real> (presenceGain_); }
        // [leaf 3] The two enum rows, and the LP corner actually in force — which is the one
        // place the cab and the mic MULTIPLY, so it is worth being able to read rather than
        // recompute. Read off the filter, not off a mirror of the product.
        [[nodiscard]] Cabinet cab() const noexcept { return cab_; }
        [[nodiscard]] Microphone mic() const noexcept { return mic_; }
        [[nodiscard]] bool cabBypassed() const noexcept { return cab_ == Cabinet::Direct; }
        // [D-M-0562] True while a cab/mic corner glide is still counting down. `reset()` lands it,
        // so a test that proves a glide LANDED must read this before any reset.
        [[nodiscard]] bool cornerGlideActive() const noexcept { return cornerLeft_ > 0; }
        // [leaf 4] The B+ rail in force, and the detector behind it — read off the RUNNING
        // follower rather than mirrored, so a model whose sag times never reached the primitive
        // reads back wrong here (the T-MASK-03 rule).
        [[nodiscard]] Real appliedRail() const noexcept { return static_cast<Real> (rail_); }
        [[nodiscard]] float sagEnvelope() const noexcept { return sag_.value(); }
        [[nodiscard]] float appliedSagAttackMs() const noexcept { return sag_.attackMs(); }
        [[nodiscard]] float appliedSagReleaseMs() const noexcept { return sag_.releaseMs(); }
        // [D-M-0304] The two amounts IN FORCE — the members the hot path reads, so a seam or a
        // model whose amount never reached them reads back wrong.
        [[nodiscard]] Real appliedTransformerDrive() const noexcept { return static_cast<Real> (xfmrDrive_); }
        [[nodiscard]] Real appliedCrossoverDepth() const noexcept { return static_cast<Real> (xoverDepth_); }
        // The oversampling factor ACTUALLY RUNNING on the active model — read off the
        // oversampler rather than off the table, so a model whose factor never reached its
        // `prepare` reads back wrong here (the T-MASK-03 "read the instrument, not the layer's
        // own copy" rule; a factor mirrored in a member would agree with itself).
        [[nodiscard]] int appliedOversample() const noexcept
        {
            return ch_[0].os[static_cast<int> (model_)].factor();
        }

    private:
        // §0.3's cascade for ONE model, on ONE channel. Sized for the deepest voicing; a
        // shallower model simply runs fewer of them (`stages`).
        static constexpr int kMaxStages = 4;

        struct Channel
        {
            // One per MODEL, because `Oversampler::prepare` designs coefficients and
            // allocates: a live `model` edit must not be the moment one is built (§2.3, and
            // the same trade `FltrEffect` takes for its nine cores).
            Oversampler os[kNumModels];
            AdaaSoftClip clip[kMaxStages];
            OnePole couple[kMaxStages]; // the HP before each stage
            OnePole plate[kMaxStages]; // the LP after each stage
            OnePole dcBlock; // §0.7, at BASE rate, after the whole core
            // [leaf 4] §0.5's class-AB pair, at OS rate and last inside the nonlinear core.
            AdaaSoftClip power;
            // [leaf 2] §0.4's three sections, at OS rate between stage 1 and stage 2.
            TptSvf bassSvf;
            TptSvf midSvf;
            TptSvf trebleSvf;
            // [D-M-0305] the network ratio's sections (see `ToneNetwork`): the noon zeros, two poles, one pole.
            TptSvf netZero;
            TptSvf netPole;
            Real netLp = 0.0;
            // …and §0.5's presence, at BASE rate after the power-amp position.
            TptSvf presenceSvf;
            // [leaf 3] §0.6's cascade, at BASE rate: the cab's three peaks/dips and its 24 dB/oct
            // top limit (two cascaded 2-pole sections), plus the mic's own bell and shelf.
            TptSvf cabLf;
            TptSvf cabPres;
            TptSvf cabNotch;
            TptSvf cabLp1;
            TptSvf cabLp2;
            TptSvf micBell;
            TptSvf micShelf;
            // [leaf 5] §5.1's band split (ONE frame: `lp` is the low band and `x − lp` its exact
            // complement) and §6.2's piezo/body sections.
            TptSvf xoverLo1;
            TptSvf xoverLo2;
            TptSvf xoverHi1;
            TptSvf xoverHi2;
            TptSvf piezoBell;
            TptSvf piezoShelf;
            TptSvf bodyBell;
            // [D-M-0304] the transformer's core flux — a unity-peak band-pass at OS rate.
            TptSvf xfmrFlux;
            // [D-M-0304] the crossover's ADAA memory: v[n−1] and its antiderivative.
            Real xoverX1 = 0.0;
            Real xoverG1 = 0.0;
        };

        // Re-resolves every coefficient from the stored rows. The `DelayEffect`/`FltrEffect`
        // `refreshCores` idiom: every edit re-pushes everything, which costs nothing on a
        // message-thread path and is what lets the per-sample path have no configuration
        // behind its model branch.
        void refresh() noexcept;
        void updateDryAlignment() noexcept;
        // Puts every shaper's one-sample memory at the stage's idle BIAS point — see the
        // definition; without it a cold cascade emits a one-sample spike out of silence.
        void primeClips() noexcept;
        // The whole nonlinear core for one OS sample, on one channel — the stage loop.
        [[nodiscard]] float core (Channel& c, float u) noexcept;
        // [leaf 5] ONE §0.3 gain stage — its own function since §5.1's split path drives the same
        // stages over a different signal.
        [[nodiscard]] float stage (Channel& c, int s, float x) noexcept;
        // [leaf 2] §0.4's three sections, in one place — called from inside the stage loop.
        [[nodiscard]] float tone (Channel& c, float x) noexcept;
        // [D-M-0305] Resolves the network ratio at pot positions (l, m, t) — at refresh, and once per base sample
        // while a pot glides. No allocation, no lock: a cubic's roots, two `tan`s.
        void resolveNetwork (float l, float m, float t) noexcept;
        // [leaf 3] §0.6's speaker+mic cascade, at base rate. Early-returns on a settled `Direct`.
        [[nodiscard]] float cabinet (Channel& c, float x) noexcept;
        // [leaf 4] §0.5's power amp inside the current B+ rail.
        [[nodiscard]] float powerAmp (Channel& c, float x) noexcept;
        // [D-M-0304] §0.5's crossover DEVIATION for one OS sample (ADAA), subtracted in `powerAmp`.
        [[nodiscard]] float crossover (Channel& c, float v) noexcept;
        // [leaf 5] §6.2's two corrective sections, for the linear voicing only.
        [[nodiscard]] float acousticVoicing (Channel& c, float x) noexcept;

        Channel ch_[2]; // L, R — see the DUAL MONO note

        bool prepared_ = false;
        bool mono_ = false; // FIREFORK F1
        Real sampleRate_ = 48000.0;
        int factorOverride_ = 0; // 0 = each model's own §0.2 factor

        // The rows, in their own §22.8 units. The defaults are `ParamRegistry.cpp`'s own
        // (`gamp` plain 1 / 54 / 84 / 100 through their curves — Crunch, 15.307 dB of gain,
        // −3.029 dB of level, 78.74 % wet), so an instance somehow rendered before its first
        // param push already sounds like the one the registry describes.
        Model model_ = Model::Crunch;
        Real gainDb_ = 15.30708661417323;
        Real levelDb_ = -3.0288285575247342;
        Real mixPercent_ = 78.74015748031496;
        // [leaf 2] …and the tone rows, same rule (`gamp` plain 62 / 52 / 60 / 46 through
        // `kPercent`). NOTE that none of them is noon: the shipped patch is a mild treble-and-
        // bass lift over a cut mid and presence, which is what a real amp's knobs sit at — so
        // the tone stack is NOT transparent at the registry defaults, and the per-model makeup
        // is fitted at NOON rather than here (see `kVoicing`).
        Real bassPercent_ = 48.818897637795274;
        Real midPercent_ = 40.94488188976378;
        Real treblePercent_ = 47.24409448818898;
        Real presencePercent_ = 36.22047244094488;
        // [leaf 3] …and the two enum rows, whose defaults are the registry's plain 0 both: a
        // 4×12 through a dynamic mic, which is the sound the type's own default patch describes.
        Cabinet cab_ = Cabinet::Quad12;
        Microphone mic_ = Microphone::Dynamic;
        // [D-M-0513] The last real box (never `Direct`): the one the stage's coefficients hold, so a
        // switch to `Direct` fades out on the cabinet that was sounding.
        Cabinet voicedCab_ = Cabinet::Quad12;

        // Resolved control-rate from the rows above.
        float preGain_ = 1.0f;
        // [leaf 5] The pre-gain as the INPUT sees it: `preGain_` for five voicings, exactly 1 for
        // Bass, whose `gain` row is applied to the high band after the split (§5.3).
        float inputGain_ = 1.0f;
        float stageGain_ = 1.0f;
        Real bias_ = 0.0;
        // `AdaaSoftClip::shape(bias_)` — §0.3's "remove DC", subtracted at each stage so a
        // silent input is a silent output EXACTLY rather than a decaying thump (see `core`).
        float biasOffset_ = 0.0f;
        // [leaf 2] The four section gains, and the bell's `k` (its width enters its own law).
        // Exactly 1.0f at noon, which is what makes a neutral section `x + 0·tap`.
        float bassGain_ = 1.0f;
        float midGain_ = 1.0f;
        float midK_ = 1.0f;
        float trebleGain_ = 1.0f;
        float presenceGain_ = 1.0f;
        // [leaf 3] The cab/mic section gains and their bells' `k`, resolved control-rate exactly
        // like the tone stack's — same `x + k·(A−1)·bp` and `x + (A−1)·lp` forms.
        float cabLfGain_ = 1.0f, cabLfK_ = 1.0f;
        float cabPresGain_ = 1.0f, cabPresK_ = 1.0f;
        float cabNotchGain_ = 1.0f, cabNotchK_ = 1.0f;
        float micBellGain_ = 1.0f, micBellK_ = 1.0f;
        float micShelfGain_ = 1.0f;
        // [leaf 4] §0.5. ONE follower and ONE rail for both channels (§0.7's stereo link), fed
        // the max of the two channels' PREVIOUS outputs — the feedback tap.
        EnvelopeFollower sag_;
        float lastOut_[2] = { 0.0f, 0.0f };
        float rail_ = 1.0f;
        float powerDrive_ = 1.0f;
        float sagAmount_ = 0.0f;
        // [D-M-0304] the seam's two switches, and the amounts they gate — resolved in `refresh`.
        bool xfmrOn_ = true;
        bool xoverOn_ = true;
        float xfmrDrive_ = 0.0f; // g; 0 = no transformer stage at all
        float xfmrInvDrive_ = 0.0f; // 1/g, so the hot path does not divide
        float xfmrK_ = 2.0f; // the flux band-pass's own k, which makes `k·bp` unity at its peak
        float xoverDepth_ = 0.0f; // 0 = no crossover stage at all
        // The rates the filters were last PREPARED at — `prepare` clears state, so `refresh` only
        // calls it when one of these moves (see `refresh`). 0 = never, which `prepare()` forces.
        Real osPreparedRate_ = 0.0;
        Real basePreparedRate_ = 0.0;

        // THE GLIDES: a row edit on a sounding note moves each hot-path gain linearly over
        // `kGlideMs` and lands EXACTLY on its target, instead of stepping (a click). `fresh_` is
        // true from `prepare`/`reset` until the first block — nothing sounds, so edits snap.
        static constexpr Real kGlideMs = 10.0;
        struct Glide
        {
            float cur = 0.0f;
            float target = 0.0f;
            float step = 0.0f;
            int left = 0;
            void snap (float t) noexcept
            {
                cur = t;
                target = t;
                left = 0;
            }
            void moveTo (float t, int n) noexcept
            {
                if (t == target)
                    return; // an unchanged re-push (a host re-sending every row) is no glide
                target = t;
                left = n;
                step = (t - cur) / static_cast<float> (n);
            }
            bool advance() noexcept
            {
                if (left == 0)
                    return false;
                --left;
                cur = left == 0 ? target : cur + step;
                return true;
            }
        };
        Glide gInput_, gPre_, gBass_, gMid_, gTreble_, gPresence_;
        Glide gCabLf_, gCabPres_, gCabNotch_, gMicBell_, gMicShelf_, gWet_, gDry_;
        Glide gCabMix_; // [D-M-0513] the cab stage's share: 1 a box, 0 `Direct`, between them mid-switch
        // [D-M-0305] the three POT POSITIONS on a network voicing — glided, so a knob turn re-resolves the stack
        // per base sample instead of stepping it; landed, the coefficients are the target's bit for bit.
        Glide gNetBass_, gNetMid_, gNetTreble_;
        bool netOn_ = false;
        // The network ratio as the hot path applies it: S1 `c0·x + c1·bp + c2·lp` over the noon zeros, S2
        // `x + d1·bp + d2·lp` over two poles, S3 `x + e·lp` over the third, then the leading-term ratio. Every
        // correction is EXACTLY 0 (and c0, gain exactly 1) at noon.
        float netC0_ = 1.0f, netC1_ = 0.0f, netC2_ = 0.0f, netD1_ = 0.0f, netD2_ = 0.0f, netE_ = 0.0f, netGain_ = 1.0f;
        Real netG_ = 0.0; // the one-pole's TPT `g / (1 + g)`
        Real netOsRate_ = 48000.0;
        // …and the model's noon, which every correction is taken against.
        Real netB1n_ = 1.0, netB2n_ = 0.0, netB3n_ = 1.0, netA3n_ = 1.0, netRhoN_ = 1.0, netAlphaN_ = 0.0, netBetaN_ = 1.0;
        float netNoonL_ = 0.0f;
        bool gliding_ = false;
        bool fresh_ = true;
        Model glideModel_ = Model::Crunch; // the model the glides were last resolved for
        [[nodiscard]] std::array<Glide*, 14> glides() noexcept
        {
            return { &gInput_, &gPre_, &gBass_, &gMid_, &gTreble_, &gPresence_, &gCabLf_, &gCabPres_, &gCabNotch_, &gMicBell_, &gMicShelf_, &gWet_, &gDry_, &gCabMix_ };
        }
        void advanceGlides() noexcept;
        // [D-M-0513] …and the cab/mic CORNERS, which a cab or mic edit moves as well as their gains: a
        // stepped (Hz, Q) kinks the waveform. The five sections' corners glide together over `kGlideMs`
        // — Hz geometrically, Q linearly — and land on the table's floats exactly (a landed glide is the
        // old `setCutoff` bit for bit). A re-target mid-glide starts from the corners IN FORCE, so a
        // reversal turns round where it is. Applied once per base sample while `cornerLeft_ > 0`: an
        // `exp` + a `tan` per section for the glide's 10 ms only — no allocation, no lock.
        struct Corner
        {
            float hz = 1000.0f;
            float q = 0.70710678f;
            bool operator== (const Corner& o) const noexcept { return hz == o.hz && q == o.q; }
        };
        static constexpr int kCorners = 5; // cabLf · cabPres · cabNotch · cabLp (both sections) · micBell
        using Corners = std::array<Corner, kCorners>;
        Corners cornerFrom_ {}, cornerTo_ {};
        std::array<Real, kCorners> cornerLnRatio_ {}; // ln(to.hz / from.hz)
        int cornerLeft_ = 0, cornerLen_ = 1;
        [[nodiscard]] Corners cornersInForce() const noexcept;
        void applyCorners (const Corners& k) noexcept;
        // [leaf 5] §5.1/§6.2's fixed sections, resolved control-rate like every other gain here.
        float piezoBellGain_ = 1.0f, piezoBellK_ = 1.0f;
        float piezoShelfGain_ = 1.0f;
        float bodyGain_ = 1.0f, bodyK_ = 1.0f;

        // The floor the rail cannot droop past: a real supply sags, it does not collapse, and the
        // clip's input is DIVIDED by this so it must stay well away from zero.
        static constexpr float kMinRail = 0.25f;
        // `level` × the model's own makeup, folded into ONE coefficient — the wet leg's whole
        // output arithmetic, so neither costs a second multiply.
        float outGain_ = 1.0f;
        // The blend, held as the PAIR rather than as one number so the hot path is
        // `wet·a + dry·b`: at mix 0 that is `wet·0 + dry·1` and at mix 100 `wet·1 + dry·0`,
        // both EXACT in IEEE-754 whatever the compiler does with the contraction — which is
        // what makes the dry anchor bit-exact without a bypass branch, and keeps the cascade
        // running through it so re-opening the mix is not a cold start. Never derive one from
        // the other at the point of use; the pair IS the guarantee (T-FX-09 leaf 1b's one
        // named trap: a makeup folded into `wetGain_` leaks dry signal back in at full mix,
        // which is why the makeup lives in `outGain_` above and not here).
        float wetGain_ = 0.7874016f;
        float dryGain_ = 0.2125984f;
        // `wetGain_ · outGain_`, i.e. the whole wet leg in one coefficient, so `level` and the
        // makeup cost the hot path nothing. Both anchors survive the fold: `mix = 0` makes
        // `wetGain_` exactly 0 so this is exactly 0, and `mix = 100` makes it exactly 1 so this
        // IS `outGain_`.
        float wetOut_ = 0.7874016f;

        // [T-FX-24 / owner ruling 2026-09-08] THE DRY LEG, DELAYED TO THIS EFFECT'S OWN REPORTED
        // LATENCY — FLTR's `dryLine_` pattern (T-CMP-24), so a partial mix is a mix and not a comb.
        // The latency is two-valued, {0, 16} (`Oversampler`'s `2 * kHalfTapsPerPhase` at any factor
        // >= 2; Acoustic alone runs at factor 1), so the capacity is 17, allocated in `prepare` and
        // never again. At latency 0 the read index IS the write index: bit-exact to the old path.
        // ⚠ A length change does NOT clear the line (FLTR's comment says why: a clear is a dropout).
        static constexpr int kDryDelayCapacity = 17;
        std::array<std::vector<float>, 2> dryLine_ {};
        std::atomic<int> dryLen_ { 0 }; // written on the message thread, read per sample (FLTR S5R f4)
        int dryWrite_ = 0; // AUDIO THREAD ONLY
    };
}
