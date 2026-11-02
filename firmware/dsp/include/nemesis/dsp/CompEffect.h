#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/EnvelopeFollower.h>
#include <nemesis/dsp/FxEffect.h>
#include <nemesis/dsp/Svf.h>
#include <nemesis/dsp/Waveshaper.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

namespace nemesis::dsp
{
    // [D-M-0419] §0.8's shaper: the IDENTITY up to a knee, then a cubic SHOULDER that lands on full scale
    // with zero slope, then full scale. One half, for |x| = s and a shoulder DEPTH d in [0, 1]:
    //
    //     knee t = 1 − d,   width D = 3d/2,   v = (s − t)/D
    //     g(s) = s                     s ≤ t
    //          = t + D·(v − v³/3)      t < s < t + D
    //          = 1                     s ≥ t + D   (= 1 + d/2; t + 2D/3 = 1 exactly)
    //
    // `AdaaSoftClip`'s own cubic, moved onto the knee and scaled to the headroom above it, so it is C2 at
    // the knee and flat at the ceiling. Each half has its OWN depth, which is how an asymmetry (even
    // order) is carried with `f(0) = 0` — no offset to remove. First-order ADAA exactly as `AdaaSoftClip`
    // (the same divided difference, the same guard, the same midpoint fallback), over the closed-form
    // antiderivative below, so the hot path is pure arithmetic: no libm, no allocation.
    class ShoulderClip
    {
    public:
        struct Side
        {
            Real knee = 1.0; // t
            Real width = 0.0; // D
            Real invWidth = 0.0; // 1/D (0 when D is: the middle arm is then unreachable)
            Real top = 1.0; // t + D, where full scale is reached
            Real tailOffset = -0.5; // the antiderivative is `s + tailOffset` at and above `top`
        };

        [[nodiscard]] static Side sideFor (Real depth) noexcept
        {
            const Real d = depth < 0.0 ? 0.0 : (depth > 1.0 ? 1.0 : depth);
            Side s;
            s.knee = 1.0 - d;
            s.width = 1.5 * d;
            s.invWidth = s.width > 0.0 ? 1.0 / s.width : 0.0;
            s.top = s.knee + s.width;
            s.tailOffset = 0.5 * s.knee * s.knee + s.knee * s.width + s.width * s.width * (5.0 / 12.0) - s.top;
            return s;
        }

        // One half, s ≥ 0.
        [[nodiscard]] static Real half (Real s, const Side& h) noexcept
        {
            if (s <= h.knee)
                return s;
            if (s >= h.top)
                return 1.0;
            const Real v = (s - h.knee) * h.invWidth;
            return h.knee + h.width * (v - v * v * v / 3.0);
        }
        [[nodiscard]] static Real halfAntiderivative (Real s, const Side& h) noexcept
        {
            if (s <= h.knee)
                return 0.5 * s * s;
            if (s >= h.top)
                return s + h.tailOffset;
            const Real v = (s - h.knee) * h.invWidth;
            const Real v2 = v * v;
            return 0.5 * h.knee * h.knee + h.knee * (s - h.knee) + h.width * h.width * (0.5 * v2 - v2 * v2 / 12.0);
        }
        // The curve (odd in shape, each half its own side) and its antiderivative F1 (F1(0) = 0, even per side).
        [[nodiscard]] static Real shape (Real x, const Side& pos, const Side& neg) noexcept
        {
            return x < 0.0 ? -half (-x, neg) : half (x, pos);
        }
        [[nodiscard]] static Real antiderivative1 (Real x, const Side& pos, const Side& neg) noexcept
        {
            return x < 0.0 ? halfAntiderivative (-x, neg) : halfAntiderivative (x, pos);
        }

        // A new curve keeps the ADAA history (x[n−1]) and re-reads its antiderivative on the NEW curve, so
        // a live `type` edit is not a divided difference spanning two curves.
        void setSides (const Side& pos, const Side& neg) noexcept
        {
            pos_ = pos;
            neg_ = neg;
            f1x1_ = antiderivative1 (x1_, pos_, neg_);
        }
        void reset() noexcept
        {
            x1_ = 0.0;
            f1x1_ = 0.0;
        }
        float processSample (float x) noexcept
        {
            const Real x0 = static_cast<Real> (x);
            const Real diff = x0 - x1_;
            const Real f1x0 = antiderivative1 (x0, pos_, neg_);
            Real y;
            if (diff > kEps || diff < -kEps)
                y = (f1x0 - f1x1_) / diff;
            else
                y = shape (0.5 * (x0 + x1_), pos_, neg_);
            x1_ = x0;
            f1x1_ = f1x0;
            return static_cast<float> (y);
        }

    private:
        static constexpr Real kEps = 1.0e-5; // `AdaaSoftClip`'s guard, and for the same reason
        Side pos_, neg_;
        Real x1_ = 0.0;
        Real f1x1_ = 0.0;
    };

    // [T-FX-11 leaf 1] COMP — the `COMP` instance type of the INSTRUMENT FX chain, and another
    // ADR-038 D6 "declared-and-unbuilt" gap: ten registry rows, a `tid`, seven unit curves and a
    // host handle pair all shipped, and `makeFxEffect` had no arm — so a patch could turn it on,
    // dial every row and hear nothing. This class closes that.
    //
    // `docs/specs/specs/dsp/compression.md` is the spec, and PART TWO is the binding half.
    // §0's four blocks, in the order this class runs them:
    //
    //     in ─┬──────────────────────────────── dry ─────────────────────────────┐
    //         │                                                                  │
    //         ├▶ [SIDECHAIN HP, per channel] ─▶ max(|L|,|R|) ─▶ [DETECTOR] ─▶ env │
    //         │        §0.6 / §6.1                  §0.7           §0.2          │
    //         │                                                     │            │
    //         │                            20·log10 ─▶ [GAIN COMPUTER] ─▶ gain_dB │
    //         │                                            §0.3                  │
    //         │                                                     │            │
    //         │                                       [BALLISTICS] ─▶ g[n]        │
    //         │                                            §0.4                  │
    //         │                                                     │            │
    //         └▶ × 10^((g[n] + makeup)/20) ─▶ [SATURATE] ─▶ wet ─────┴─▶ dry/wet ─┴─▶ out
    //                       §0.5                  §0.8                    §5
    //
    // ============================= THE PART-TWO DESIGN LAW ==============================
    // §9.1, and it is what makes this class finite: **with ten rows, everything in §0–§8 that
    // is not one of those ten is a per-type CONSTANT baked into the type, not a control.** That
    // is not a compromise — it IS what makes the four types distinct. A user who wants a
    // feedforward RMS-detecting fast comp selects VCA; one who wants a feedback peak-detecting
    // µs-attack comp selects FET. §7's "one engine, four voicings" is therefore realised as ONE
    // engine plus `kVoicing`, a four-row constant table, and the only thing `type` does is
    // select a row of it. Detector tap, peak-vs-RMS, saturation amount, stereo link,
    // auto-release, Compress/Limit and all-buttons are all cells of that table; not one of them
    // became an invented registry row (the same ruling T-FX-08 and T-FX-09 each took).
    // ====================================================================================
    //
    // WHAT EACH ROW MEANS ON THE WAY DOWN (`ParamRegistry.cpp`'s `comp.*` block and
    // `UnitCurves.cpp`'s matching curves are the authority for the unit; this class is the
    // authority for the conversion):
    //
    //   `on`      the CHAIN's bypass (a graph-node atomic) — ignored here, as in every sibling.
    //   `type`    ENUM ordinal (`kCompTypes`, 4 values) → `Type`. The index IS the patch.
    //   `thresh`  dB (`kThreshDb`, Linear 0 → −60 dB) → §0.3's `T`, straight through. THE
    //             DIRECTION IS COUNTER-INTUITIVE AND IS NOT A BUG (§9): the row's value RISES
    //             as the threshold FALLS, so v=0 ⇒ 0 dBFS ⇒ nothing compresses.
    //   `ratio`   :1 (`kRatio`, Exp 1 → 20, **+∞ at v=127**) → §0.3's `R`, through
    //             `ratioFor(type, …)` — which SNAPS on the types whose hardware has stops.
    //             An infinite ratio is a limiter and needs no special case: `Δ/R` is 0, so the
    //             curve pins the output at `T`.
    //   `attack`  ms (`kCompAtkMs`, Exp 0.05 → 100 ms) → the ballistics' attack τ, through
    //             `attackMsFor(type, …)`.
    //   `release` ms (`kCompRelMs`, Exp 5 → 2000 ms) → the release, through
    //             `releaseMsFor(type, …)` and the type's release LAW (see THE BALLISTICS).
    //   `makeup`  dB (`kLevelDb`, the Level curve: −∞ at v=0, 0 dB at v=100, +12 dB at v=127)
    //             → §0.5's makeup, added to `g[n]` in the dB domain so there is ONE exponential
    //             per sample rather than two. See the DEFECT note below.
    //   `schp`    Hz (`kHpCornerHz`, Exp 20 → 2000 Hz) → §0.6's sidechain high-pass, on the
    //             DETECTOR path only. See THE SIDECHAIN.
    //   `knee`    dB (`kKneeDb`, Linear 0 → 24 dB) → §0.3's `W`, through `kneeDbFor(type, …)`.
    //   `mix`     % (`kPercent`) → §5's parallel blend. `mix = 0` is bit-exact dry and
    //             `mix = 100` is bit-exact wet, both by construction (see `process`).
    //
    // **`comp.makeup`'s SHIPPED DEFAULT IS A DEFECT IN WAITING, and this class does NOT paper
    // over it.** v=24 on the `Level` curve reads **−24.79 dB**, so the first thing a user hears
    // on enabling COMP is a 25 dB drop — compression.md §15's OI-CMP-1, "a defect in waiting,
    // not a taste question", and the FLTR missing-output-comp class in the opposite direction.
    // The fix is a REGISTRY/curve change (re-law `makeup` linear-in-dB 0 → +24 dB) and it is an
    // OPEN OWNER FORK, so this class reads whatever dB the row hands it and reports it back
    // honestly. Nothing here needs to change when the ruling lands: only the number moves.
    //
    // ================================= THE BALLISTICS ===================================
    // §0.4, and §12's capabilities 4/5/6 — the half T-PRIM-02 deliberately left OUT of
    // `EnvelopeFollower` because only this consumer needs it ("a Comp-owned ballistics layer ON
    // the primitive, not a change to it", `DEFERRED.md`). Three release LAWS, selected per type:
    //
    //   `Exponential`  §0.4 verbatim — a branching (decoupled) one-pole on `gain_dB`, attack
    //                  when the target asks for MORE reduction, release when it asks for less.
    //   `ConstantRate` §12 capability 5 — recovery at a fixed **dB per second**, which a single
    //                  one-pole provably cannot express. The dbx-lineage VCA's published
    //                  ballistic (§11.4, grade A: the 160-series releases at ≈120 dB/s), and the
    //                  cheapest real character the reference type can have.
    //   `DualTc`       §12 capability 4 — TWO release poles plus a slow history state, blended
    //                  by recent gain reduction: the SSL `Auto` mode (§11.3) and the shape of
    //                  the LA-2A's "60 ms for 50 % release, 0.5 to 5 s for complete" (§11.2).
    //
    // **THE STATE LIVES HERE, IN ONE PLACE, AND THE PRIMITIVE PINS IT.** `EnvelopeFollower`'s
    // `GainDb` domain expresses `Exponential` exactly and nothing else — two of the three laws
    // need state a single one-pole does not have — so the smoothed gain is this class's own
    // `double`, and the α law is still the primitive's (`EnvelopeFollower::alphaFor`, the single
    // place `α = exp(−1/(τ·fs))` lives, called at CONTROL rate). What makes that deviation
    // falsifiable rather than a fork: at the `Exponential` policy this class's smoothed gain is
    // **BIT-IDENTICAL** to an `EnvelopeFollower` in `GainDb` domain configured the same way, and
    // a test asserts exactly that (the M1-09 hand-composed-reference keystone). So §0.4's own
    // branching law is proven to be the primitive's, and the two other laws are measured
    // deviations FROM it. The primitive is still doing its job in the shipped path — it is the
    // §0.2 level DETECTOR, in the `Level` domain, at the type's own detection law.
    //
    // WHERE THE DETECTOR'S OWN TIME CONSTANT COMES FROM, since no row reaches it: §0.4 says
    // "smoothing the GAIN vs smoothing the LEVEL before the curve gives different feels;
    // gain-smoothing is the common modern choice", so `attack`/`release` smooth the GAIN and the
    // detector carries only §0.2's own averaging window — a per-type constant
    // (`Voicing::detectorMs`), zero for a peak detector (instantaneous full-wave rectification)
    // and a real window for an RMS one. Those four numbers are owner-tunable defaults per the
    // M1-17f rule: §10 says "RMS (Blackmer lineage)" / "peak" / "RMS-ish, slow" / "RMS-leaning,
    // full-wave" and gives no figures, so the window is a pick, documented rather than asked.
    // ====================================================================================
    //
    // ================================== THE SIDECHAIN ===================================
    // [leaf 2] §0.6 / §6.1, and the distinction the spec is emphatic about: filtering the
    // DETECTOR path changes **what the compressor reacts to** while gain is still applied
    // **broadband**. That is not multiband, and the difference is measurable — a 40 Hz + 1 kHz
    // pair must come out attenuated by the SAME dB, which a band-gain implementation fails.
    //
    // It is nearly free here, and for a structural reason: `EnvelopeFollower`'s detector input
    // is an ARGUMENT (§12 capability 2), so the caller filters whatever it hands in. TWO
    // high-passes, one per channel, and they run BEFORE the §0.7 link — a link is a
    // rectification (`max(|L|,|R|)`) and rectifying before filtering would hand the filter a
    // signal whose spectrum is no longer the programme's.
    //
    // THE ROW'S BOTTOM RAIL IS OFF, BIT-EXACTLY, and §9 is the authority rather than this
    // class: "v=0 ⇒ 20 Hz ≈ **Off** (a 20 Hz HP passes everything musical)". So at
    // `schp = kSchpOffHz` the filters are not run at all and the detector sees the programme —
    // which also keeps every leaf-1 measurement exactly reproducible. The shipped DEFAULT is
    // v=20 ⇒ 41.3 Hz, i.e. the row already filters a little out of the box.
    // ====================================================================================
    //
    // ================================= THE SATURATION ===================================
    // [leaf 7] §0.8, "saturation is half the sound" (§7): VCA can be clean, FET/Opto/Bus add
    // gain-element and transformer harmonics. Per-type amount from `kVoicing`, applied AFTER the
    // makeup — which is not an arbitrary position but §2's own workflow model for the type that
    // cares most ("you drive harder with INPUT gain and set level with OUTPUT"): with no drive
    // row, `makeup` is the only row that can drive the colour, and after the makeup is where an
    // output transformer sits.
    //
    // **THE LAW — UNITY BELOW A KNEE, A SHOULDER TO FULL SCALE (D-M-0419, owner ruling 2026-10-08).**
    // For an amount `a` in (0, 1] (§10's per-type figure, `kAbiSaturation` under ABI):
    //
    //     k = 1 + kSatRange·a     depth d = 1 − 1/k     knee t = 1 − d = 1/k     width D = 3d/2
    //     y = x                          |x| ≤ t
    //     y = sgn(x)·(t + D·(v − v³/3))   t < |x| < t + D,   v = (|x| − t)/D
    //     y = sgn(x)                     |x| ≥ t + D = 1 + d/2
    //
    // (`ShoulderClip`, above.) `a = 0` is not in circuit at all, so it is the identity bit for bit. What
    // the law guarantees, each one a test: the gain is EXACTLY 1 below the knee, and the knee is never
    // under 1/3 (−9.5 dBFS) — VCA −1.6 · Bus −2.3 · FET −5.1 · Opto −5.9/−1.9 · ABI −8.0 dBFS — so a
    // passage at or under −12 dBFS is not narrowed at all; the ceiling is EXACTLY full scale for every
    // `a > 0`; `|y − x| ≤ 0.15·d` on |x| ≤ 1, so the law is continuous as the amount leaves 0 (the only
    // jump left is the ceiling itself, above full scale, which is its point); and the colour (H3 of a
    // full-scale sine) rises strictly with `a`. The knee sits at `1/k`, exactly where the first law's
    // curve met its clamp, so `kSatRange` keeps its meaning.
    // THE FIRST LAW, and why it went: `shape(k·x)/k` — `AdaaSoftClip`'s cubic with its ceiling at
    // `2/(3k)` (VCA −5.1, FET −8.6 dBFS) and a gain loss of `(k·x)²/3` at EVERY level. Its premise
    // ("only large signals soften") failed once the makeup lifted the programme near full scale: with
    // the gain computer pinned at 0 dB it narrowed a loud/quiet step by 1.9 (VCA) to 3.6 (FET) dB, and
    // amount 0⁺ capped at 2/3 while amount 0 was the identity. Retired on the owner's A/B (2026-10-08,
    // D-M-0526): the class carries this law only.
    //
    // **ASYMMETRY (Opto's even order, §10):** each half takes its own amount `a·(1 ± satAsym)`, capped
    // at 1 — the positive half kneeing earlier — so `f(0) = 0` and there is no offset to remove or to
    // prime the ADAA memory to (the first law's bias needed both).
    //
    // **NO OVERSAMPLER, and that is a stated deviation from §0.8's word "oversampled."**
    // `ShoulderClip` is a first-order ADAA shaper (`AdaaSoftClip`'s method), whose reason for existing is that
    // "consumers can drop the OS factor because the primitive carries the floor" (its own
    // header, M1-02) — and at the four amounts this class ships the alias floor is measured
    // against a naive shaper of the same curve rather than assumed (see `test_comp_effect.cpp`).
    // Two things fall out and both are wanted: the class's latency stays **0**, so §5's
    // make-or-break ("if the compressor has latency, delay the dry path by the same amount or
    // the blend combs") is satisfied by there being nothing to match; and the §14 cost note's
    // more expensive half never arrives. ponytail: the ceiling is a shaper amount well above
    // what §10's table asks for — an `Oversampler` around the shaper (plus a matched integer
    // delay on the DRY leg and a non-zero `latencySamples`) is the upgrade, and the alias-floor
    // case is the trigger that would justify it.
    //
    // ONE CHARACTER OF THE ADAA SHAPER WORTH NAMING, because a test found it rather than a
    // reading did: an ADAA `processSample` returns the AVERAGE of the curve over the sample
    // interval, which below the knee is `(x[n] + x[n−1])/2` — an inherent HALF-SAMPLE
    // average, i.e. a gentle high-frequency roll-off in the wet leg. THE FIGURE: a two-point average
    // has gain |cos(ω/2)| (ω = 2π·f/fs), so at fs = 48 kHz it is −0.30 dB at 4 kHz, −3.01 dB at
    // 12 kHz (cos(π/4)) and exactly −∞ at Nyquist; a small-signal sine measured through
    // `AdaaSoftClip` reads −0.30 / −3.01 dB there (−3.03 dB at 12 kHz at amplitude 0.1 under the first
    // law, whose cubic compressed a little; below the knee the shoulder adds nothing). It is a property of the ADAA shaper itself, so it applies to
    // EVERY voicing whose saturation is above 0 (`saturate`, CompEffect.cpp), not to Bus alone, and
    // it scales with the sample rate. That is the shaper's anti-aliasing mechanism
    // and not a misalignment: there is no latency to match, `latencySamples()` is 0, and the
    // blend stays exactly algebraic — which is why the `mix` case asserts `½·wet + ½·dry` sample
    // by sample instead of a frequency sweep, since at half a sample a sweep could not
    // discriminate a genuine one-sample error from this.
    // ====================================================================================
    //
    // TWO TRANSCENDENTALS PER SAMPLE, named rather than discovered: the gain computer is a
    // LOG-DOMAIN curve (§7: "compute the curve in dB; it's numerically clean and makes
    // soft-knee/ratio exact"), so a `log2` turns the detected level into dB and an `exp2` turns
    // the smoothed gain back into a multiplier. Both are per SAMPLE and neither has a
    // control-rate escape — the gain moves every sample, which is the whole point of a
    // compressor — so this is the same scoped, blessed deviation `CathodeModulatedVoice` takes
    // for a live cutoff destination and `BrokenConverter` for µ-law: bounded, deterministic,
    // allocation-free, no locks. ONE of each per sample for BOTH channels, because there is one
    // control signal (§0.7). ponytail: a decimated (every-N-sample) control rate is the obvious
    // saving and it would change the sound of a fast attack, so it wants a measurement rather
    // than a guess.
    //
    // WHAT IS NOT REACHABLE, and is therefore not built (§9.1's list, each one DEFERRED-ledgered
    // rather than invented as a row): stereo UNLINK (§0.7's "for special cases") · lookahead
    // (§0.5) · an EXTERNAL sidechain key input (§0.6) · auto-makeup · auto-release as a switch ·
    // peak-vs-RMS as a control · Compress/Limit · **§6.2 multiband and §6.3 single-band dynamic
    // EQ**, which need a crossover and a per-band row set that cannot fit inside ten rows and
    // are confirmed OUT of scope for this type (OI-CMP-6). §14's static-curve viz and live GR
    // meter are a surface this layer cannot draw; what it owes them is `gainReductionDb()`, and
    // that accessor exists.
    //
    // **FET AND OPTO ARE FINISHED, and both run FEEDBACK** (`kVoicing`'s `Topology::Feedback` — the
    // detector sees the processed output one sample late). compression.md §15's three rulings landed:
    // OI-CMP-3 gave every type its own attack/release span (`core::compAttackMs`/`compReleaseMs` —
    // the 1176's 20 µs and the LA-2A's 5 s tail are reachable), OI-CMP-4 put all-buttons-in at FET's
    // top ratio row (`isAllButtonsIn`), and OI-CMP-5 made Opto's attack/release CHARACTER knobs.
    //
    // RT-safety: `process`/`reset` allocate nothing (nothing here allocates at all — the two
    // SVFs, the follower and the two shapers are all fixed-size members), and neither does a
    // live edit of any row including `type`, which is a single table lookup. DUAL MONO in the
    // audio path and LINKED in the control path: nothing crosses channels except the one
    // control signal, which is §0.7's whole instruction.
    class CompEffect final : public FxEffect
    {
    public:
        // `comp.type`'s roster, in `ParamRegistry.cpp`'s `kCompTypes` STORAGE ORDER, which is
        // frozen: the ordinal is what a patch stores.
        enum class Type : std::uint8_t {
            Vca = 0,
            Fet,
            Opto,
            Bus
        };
        static constexpr int kNumTypes = 4;

        // §0.4's release law — §12's capabilities 4 and 5 as a policy selector. See THE
        // BALLISTICS; `Exponential` is the one an `EnvelopeFollower` can express alone.
        enum class Release : std::uint8_t {
            Exponential,
            ConstantRate,
            DualTc
        };

        // §0.1's detection TOPOLOGY, and §10's defining choice per type. Feedforward is
        // predictable and modern (VCA, Bus); feedback is smooth, program-dependent and vintage
        // (FET, Opto) — the detector sees the processed OUTPUT one sample late, which is the form
        // `EnvelopeFollower`'s argument-taking contract exists for (§12 capability 2).
        enum class Topology : std::uint8_t {
            Feedforward,
            Feedback
        };

        void prepare (Real sampleRate, int maxBlockSamples) override;
        void reset() noexcept override;
        void process (float* left, float* right, int numSamples) noexcept override;

        void setAppliedUnit (std::string_view field, Real value) noexcept override;
        [[nodiscard]] std::optional<Real> appliedUnit (std::string_view field) const noexcept override;

        // Control-rate setters, in REAL UNITS (the T-CORE-06 rule: the plain 0–127 value and its
        // §22.8 curve stay on the core side of the façade — except the ENUM row, whose plain
        // ordinal IS the value). Each stores the row and re-resolves the derived set, the
        // `DelayEffect::refreshCores` idiom.
        void setType (int index) noexcept;
        void setThresholdDb (float db) noexcept;
        void setRatio (float ratio) noexcept;
        void setAttackMs (float ms) noexcept;
        void setReleaseMs (float ms) noexcept;
        void setMakeupDb (float db) noexcept;
        void setSidechainHpHz (float hz) noexcept;
        void setKneeDb (float db) noexcept;
        void setMixPercent (float percent) noexcept;
        // [D-M-0525] A DETECTOR-ONLY instance (MasterGlue's) takes the shaper out of circuit, so a feedback
        // voicing's tap reads the PRE-saturation reduced output and its gain does not move with §0.8's law.
        // Not a row: the insert always runs its shaper. Default on.
        void setSaturationEnabled (bool on) noexcept;

        // --- the LAWS, as pure statics -------------------------------------------------
        //
        // Exposed so a test pins `effective == law(row)` to the bit without a literal, and so a
        // reference implementation can be built from the same arithmetic (the
        // `FilterDrive::preGainFor` / `FltrEffect::resonance01ForQ` idiom).

        // §0.3's static curve, verbatim (Giannoulis/Massberg/Reiss — JAES 60(6):399–408, 2012,
        // cited at §11.5). Returns `gain_dB = out_dB − level_dB`, i.e. the REDUCTION, ≤ 0.
        //   * `kneeDb = 0` is a HARD knee: the soft-knee arm divides by `2W` and is skipped
        //     entirely, so there is no 0/0 to reason about at the threshold.
        //   * `ratio = ∞` pins the output at `T` (a limiter) by the curve's own arithmetic,
        //     since `Δ/R` is 0 — no special case.
        //   * `ratio < 1` clamps to 1: this row is downward compression, and an expansion law
        //     from a ratio knob a user turned the wrong way would be a surprise, not a feature.
        [[nodiscard]] static Real gainDbFor (Real levelDb, Real threshDb, Real ratio, Real kneeDb) noexcept;

        // §0.2's `level_dB = 20·log10(x_det + 1e-9)`, including the epsilon — so silence reads
        // −180 dB rather than −∞ and the curve above it never sees a non-finite input.
        [[nodiscard]] static Real levelDbFor (Real detected) noexcept;

        // The `ratio` row → the ratio actually in force. §10 gives two of the four types
        // hardware STOPS (Bus 2/4/10 — SSL, grade A; FET 4/8/12/20 — the 1176's four latching
        // buttons, grade A) and the other two a continuous law, so this SNAPS to the nearest
        // stop where the hardware has them and passes the row straight through where it does
        // not. An infinite row value stays infinite on every type.
        [[nodiscard]] static Real ratioFor (Type type, Real rowRatio) noexcept;

        // The `knee` row → §0.3's `W`. §10 forces a knee CHARACTER per type by rescaling the
        // row's 0–24 dB travel into the type's own window (VCA literal · Bus 4–18 soft ·
        // FET 0–6 hard-ish · Opto 6–24 soft), which is the design law at work: the knee is a
        // control, and how MUCH of a knee the type will accept is the type's.
        [[nodiscard]] static Real kneeDbFor (Type type, Real rowKneeDb) noexcept;

        // The `attack` / `release` rows → the times actually in force. **These CLAMP; they never
        // RE-SCALE.** That boundary is the whole of compression.md §15's OI-CMP-3: a clamp is
        // honest for every position inside the type's range and only ever disagrees with the
        // display outside it (the built `DelayEffect::kAnalogMaxTimeMs` precedent — the seam's
        // `core::toUnit` is type-blind, so a per-type range can only live here), whereas a
        // re-scale would make the displayed number wrong across the WHOLE travel. Types whose
        // hardware range is not CONTAINED by the row's own therefore cannot be served by a
        // clamp at all, and that is exactly why FET and Opto wait on an owner ruling.
        [[nodiscard]] static Real attackMsFor (Type type, Real rowMs) noexcept;
        [[nodiscard]] static Real releaseMsFor (Type type, Real rowMs) noexcept;

        // The `release` row → §12 capability 5's recovery RATE, in dB per second, for the type
        // whose ballistic is published that way. Read the row as "the time to recover
        // `kVcaReleaseSpanDb` of gain reduction", which puts ≈120 dB/s at the row's own mid
        // travel — §11.4's grade-A figure for the dbx 160 lineage, arrived at from the row
        // rather than pasted next to it.
        [[nodiscard]] static Real vcaReleaseDbPerSec (Real rowMs) noexcept;

        // §0.8's shape, memoryless — THE LAW (see THE SATURATION), and the reference the stateful
        // ADAA in the audio path is measured against: `amount = 0` returns `x` EXACTLY (an early
        // return), any amount > 0 is the identity below its knee and holds full scale above its
        // shoulder. `asym` is §10's even-order asymmetry (Opto's "tube/optical warmth"): the halves
        // take amounts `amount·(1 ± asym)`, so `f(0) = 0` at any asymmetry.
        [[nodiscard]] static Real saturateFor (Real x, Real amount, Real asym = 0.0) noexcept;
        // …and the two halves that law hands `ShoulderClip`, so the audio path and the static cannot
        // disagree about the curve.
        [[nodiscard]] static ShoulderClip::Side saturationSideFor (Real amount) noexcept;

        // The `ratio` row → ABI, for the FET type only (owner ruling on OI-CMP-4). True exactly
        // where the row's own top position is, which the `kRatio` curve reports as ∞.
        [[nodiscard]] static bool isAllButtonsIn (Type type, Real rowRatio) noexcept;

        // ONE row of §10's four-type constant table. Every cell is a value the TYPE fixes and
        // the user cannot reach — §9.1's design law as data.
        struct Voicing
        {
            EnvelopeFollower::Detector detector; // §0.2 / §10's "Detection" row
            Real detectorMs; // §0.2's own averaging window; 0 = instantaneous peak
            Topology topology; // §0.1 / §10's "Detector tap" row
            Release release; // §0.4 / §10's "Release law" row, below `autoAboveMs`
            // §4's "Release … 0.1–1.2 s **+ AUTO**": a `release` ROW at or above this selects
            // `DualTc`, because AUTO is the position past the last stop rather than a switch
            // (there is no row for a switch). Infinite on the types that have no AUTO position;
            // `Release::DualTc` in `release` makes the law unconditional (Opto's is).
            Real autoAboveMs;
            // `DualTc`'s FAST pole. The SLOW one is the release ROW itself, which is why this is
            // the only extra constant the law needs: Opto's fast stage is §11.2's grade-A "60 ms
            // for 50 % release" and its tail IS the row (0.5–5 s), and Bus's Auto sits on a
            // ~200 ms fast pole against the row's 1.2 s top — inside §11.3's own "~0.1–0.3 s
            // fast plus ~1.2–3 s slow" band. Ignored unless the policy is `DualTc`.
            Real autoFastMs;
            // The type's own attack/release span, from `core::compAttackMs`/`compReleaseMs` (the
            // owner ruling on OI-CMP-3 gave every type its real hardware range). Held here as a
            // CLAMP — a safety net at the same rails, not a second conversion; `test_fx_seam.cpp`
            // asserts these against the core function's own endpoints so the copy cannot drift.
            Real attackMinMs, attackMaxMs;
            Real releaseMinMs, releaseMaxMs;
            Real kneeMinDb, kneeMaxDb; // §10's per-type knee window
            // §12 capability 6 — how much the photocell's MEMORY lengthens the attack: the
            // effective τ runs from `attack` to `attack·(1 + attackMemory)` as recent gain
            // reduction builds. 0 on the three types whose gain element has no memory, which is
            // what makes "Opto is a model, not a preset" (§7) a number rather than an adjective.
            Real attackMemory;
            Real saturation; // §0.8 / §10's "Saturation" row
            // …and its ASYMMETRY (D-M-0419). §10 asks Opto for "tube/optical
            // warmth, EVEN-order", and a symmetric odd curve cannot make an even harmonic at all — so
            // the halves knee at amounts `saturation·(1 ± satAsym)`. Opto's 0.6 is the first law's
            // 0.18 bias read as a share of its 0.30 amount. 0 elsewhere: §10 gives the other three no
            // even-order character.
            Real satAsym;
        };
        [[nodiscard]] static const Voicing& voicingFor (Type type) noexcept;

        // The stepped hardware ratio stops `ratioFor` snaps to, both grade A. §4/§11.3's SSL
        // G-series three, and §11.1's four interlocking 1176 push-buttons — which are BUTTONS and
        // not a rotary switch, so "snap" is the literal behaviour and not a convenience.
        static constexpr Real kBusRatioStops[] = { 2.0, 4.0, 10.0 };
        static constexpr int kNumBusRatioStops = 3;
        static constexpr Real kFetRatioStops[] = { 4.0, 8.0, 12.0, 20.0 };
        static constexpr int kNumFetRatioStops = 4;

        // The row RAILS this class converts against. They are `UnitCurves.cpp`'s own and
        // therefore a SECOND COPY of them, so `app/tests/test_fx_seam.cpp` asserts every one
        // against the real curve's ends — the copy cannot drift silently (the `FltrEffect`
        // convention; a layer below `core`, the registry is not available).
        static constexpr Real kThreshDbMin = -60.0; // `kThreshDb`'s hi endpoint (v = 127)
        static constexpr Real kThreshDbMax = 0.0; // …and its lo (v = 0): nothing compresses
        static constexpr Real kRatioMin = 1.0;
        static constexpr Real kRatioMax = 20.0; // …and +∞ at v = 127, by the Ratio law
        static constexpr Real kAttackMsMin = 0.05;
        static constexpr Real kAttackMsMax = 100.0;
        static constexpr Real kReleaseMsMin = 5.0;
        static constexpr Real kReleaseMsMax = 2000.0;
        // `comp.makeup`'s own `kMakeupDb` curve, Linear 0 → +24 dB (owner ruling on OI-CMP-1). Its
        // BOTTOM is unity and not silence, which is what a makeup gain is; the row it used to
        // share, `kLevelDb`, reaches −∞ dB at v = 0 and made this row read −24.79 dB at its
        // shipped default.
        static constexpr Real kMakeupDbMax = 24.0;
        static constexpr Real kSchpHzMin = 20.0;
        static constexpr Real kSchpHzMax = 2000.0;
        static constexpr Real kKneeDbMax = 24.0;
        static constexpr Real kMixPercentMax = 100.0;

        // §9's reading of `kHpCornerHz`'s bottom rail: "v=0 ⇒ 20 Hz ≈ Off". At or below this
        // the sidechain filters are bypassed, bit-exactly.
        static constexpr Real kSchpOffHz = kSchpHzMin;
        // The sidechain high-pass's Q — a 2-pole Butterworth corner (§6.1 asks for a high-pass
        // and no order; one `TptSvf` frame is 12 dB/oct, which is the standard bus-comp filter).
        static constexpr Real kSchpQ = 0.7071067811865476;

        // A floor under the `makeup` dB before it enters the exponent. The shipped row can no
        // longer produce a non-finite value (its curve bottoms at 0 dB since the OI-CMP-1 ruling),
        // so this guards the SETTER rather than the row: `setMakeupDb` is public and a −∞ must not
        // reach `exp2` on a RELEASE build, where `CMakeLists.txt` gives the plugin target `-Ofast`
        // and leaning on IEEE's `exp2(−∞) = 0` is unspecified (the T-PRIM-02 lesson). −240 dB is
        // silence by any measure and is finite.
        static constexpr Real kMakeupDbFloor = -240.0;

        // §0.2's own epsilon, so silence reads −180 dB rather than −∞ and the log-domain curve
        // never sees a non-finite input. The spec writes it into the law: `20·log10(x + 1e-9)`.
        static constexpr Real kDetectorEpsilon = 1.0e-9;

        // §12 capability 8's rest snap, in the dB-gain domain: a smoothed gain this close to
        // unity ARRIVES at exactly 0 dB rather than walking the subnormal range. The same value
        // `EnvelopeFollower` uses privately — a second copy, and deliberately so, because it is
        // what makes this class's `Exponential` law BIT-IDENTICAL to the primitive's. The
        // bit-identity test is the guard: change either and it reds.
        static constexpr Real kRestFloorDb = 1.0e-30;

        // §12 capability 5's normalisation: the `release` row is the time to recover this many
        // dB. An owner-tunable default per the M1-17f rule, and not a free pick — it is the
        // number that puts §11.4's published ≈120 dB/s at the row's own mid travel (v = 64 ⇒
        // ≈100 ms ⇒ 120 dB/s).
        static constexpr Real kVcaReleaseSpanDb = 12.0;

        // §0.8's range: `k = 1 + kSatRange·amount`, and the knee sits at `1/k` (see THE SATURATION).
        // Picked small on purpose — §0.8's word is "gentle": §10's amounts (0.1 … 0.4) keep the knee
        // above −5.2 dBFS, and the ADAA shaper carries the alias floor without an oversampler.
        static constexpr Real kSatRange = 2.0;

        // `DualTc` (§12 capability 4). The HISTORY state that blends the two poles — "how much /
        // how long GR has been applied" — plus how many dB of sustained reduction reaches full
        // slow share. 3 dB is §4's own glue target ("2–4 dB of gain reduction"). Owner-tunable
        // defaults, because §11.3 is explicit that SSL never published the constants ("commonly
        // modelled as a ~0.1–0.3 s fast pole plus a ~1.2–3 s slow pole blended by recent GR —
        // state that as OUR choice, not as SSL's number"). The two POLES are per type
        // (`Voicing::autoFastMs` and the release row itself), where the sources are.
        static constexpr Real kAutoHistoryMs = 500.0;
        static constexpr Real kAutoFullShareDb = 3.0;
        // …and the largest fraction of the reduction the SLOW pole may carry. Below 1 on
        // purpose: at 1 the fast pole would have nothing to do at full history and the knee
        // C-CMP-4 measures would vanish exactly when the mode is working hardest.
        static constexpr Real kAutoMaxSlowShare = 0.75;

        // §4's "Release … 0.1–1.2 s **+ AUTO**": AUTO is the position PAST the last stop, so a
        // `release` row at the top of Bus's OWN range (`core::compReleaseMs(127, Bus)`) selects
        // the dual-TC law and every position below it is the stepped exponential.
        static constexpr Real kBusAutoThresholdMs = 1200.0;

        // ALL-BUTTONS-IN (§2, and §11.1 grade B — undocumented on the hardware, modelled by every
        // commercial 1176). Owner ruling 2026-08-11 on compression.md §15's OI-CMP-4, option (a):
        // **for the FET type, `ratio` at its top position IS ABI**, because the built `kRatio`
        // curve already returns ∞ there and "∞:1 with a broken knee" is a fair reading of what
        // jamming all four latches does. Zero new rows, zero 499-lock cost. §11.1's description is
        // "a very high, NONLINEAR effective ratio, a distorted and non-monotonic knee, a
        // lagged/less-controlled attack, pronounced distortion, and a GR meter that rests off
        // zero", and §2 asks for "an aggressive curve + extra saturation + altered attack". These
        // four constants are that, one per clause:
        static constexpr Real kAbiAttackLag = 8.0; // ×τ — the "lagged, less-controlled attack"
        static constexpr Real kAbiSaturation = 0.75; // "pronounced distortion"
        static constexpr Real kAbiRestingGrDb = 2.0; // "a GR meter that rests off zero"
        static constexpr Real kAbiKneeDb = 10.0; // the "distorted, lagging knee" as a width
        // WHAT ABI DELIBERATELY IS NOT: §11.1's "NON-MONOTONIC knee" is not modelled. A knee that
        // reduces LESS as the input gets louder is a real reported behaviour of the reconfigured
        // resistor network, and it is also the one clause with no published shape at all (grade B,
        // "the effective ratio is not a published number") — inventing a fold there would be
        // guessing at the character rather than modelling it. Ledgered.

        // §12 capability 6 — the T4 photocell's MEMORY, as the two numbers it needs. The
        // effective attack τ runs from the row's own to `×(1 + attackMemory)` as recent reduction
        // builds toward this many dB. Owner-tunable defaults: §11.2 is grade B on the mechanism
        // ("a cell driven hard AND long recovers slower than one hit by a short transient") and
        // publishes no number, which the spec says out loud.
        static constexpr Real kOptoMemoryFullDb = 8.0;

        // --- read-backs, in FORCE ------------------------------------------------------
        //
        // `appliedUnit` answers with the ROW as dialed (what a surface must show); these answer
        // with what is actually running, which is the only claim a render cannot make.
        [[nodiscard]] Type type() const noexcept { return type_; }
        [[nodiscard]] Real appliedRatio() const noexcept { return ratio_; }
        [[nodiscard]] Real appliedKneeDb() const noexcept { return knee_; }
        [[nodiscard]] Real appliedAttackMs() const noexcept { return attack_; }
        [[nodiscard]] Real appliedReleaseMs() const noexcept { return release_; }
        [[nodiscard]] Release releasePolicy() const noexcept { return policy_; }
        [[nodiscard]] Topology topology() const noexcept { return voicingFor (type_).topology; }
        // Whether the FET's `ratio` row is at its ABI position (owner ruling on OI-CMP-4).
        [[nodiscard]] bool allButtonsIn() const noexcept { return abi_; }
        // The attack coefficient the LAST sample actually used — the instrument for §12
        // capability 6, because the photocell's τ is a per-sample quantity and a row read-back
        // cannot see it. Equal to `alphaFor(appliedAttackMs())` on the three types with no memory.
        [[nodiscard]] Real attackCoefficient() const noexcept { return activeAttackCoef_; }
        // …and the history state that drives both capability 4's blend and capability 6's τ: how
        // much reduction has RECENTLY been applied, in dB.
        [[nodiscard]] Real historyDb() const noexcept { return autoHistoryDb_; }
        [[nodiscard]] Real appliedMix() const noexcept { return static_cast<Real> (wetGain_); }
        // §0.8's amount IN FORCE — which is the TABLE's on three types and `kAbiSaturation` on a
        // FET with all buttons in, so this reads the applied value rather than the table (the
        // T-MASK-03 rule: a copy of a fact is a second opinion, and second opinions agree with
        // themselves — this one reported the table while ABI was overriding it).
        [[nodiscard]] Real appliedSaturation() const noexcept { return satAmount_; }
        [[nodiscard]] Real appliedSaturationAsymmetry() const noexcept { return satAsym_; }

        // §14's GR meter, and the instrument rather than a bookkeeping copy of it: this is the
        // dB the hot path last turned into a multiplier, negated so it reads as REDUCTION
        // (positive = quieter). A row that never reached the ballistics reads back wrong here,
        // which `appliedUnit` alone cannot see (the T-MASK-03 rule).
        [[nodiscard]] Real gainReductionDb() const noexcept { return -smoothedGainDb_; }
        // [D-M-0312] THE GUI'S COPY of that, safe from ANY thread: a block-end snapshot of the reduction the
        // multiplier actually carries (`gainReductionDb()` PLUS ABI's resting offset, so the all-buttons-in
        // meter rests at 2 dB as §2/§11.1 say it does) in dB, positive = quieter. One relaxed atomic store per
        // BLOCK on the audio thread and one relaxed load on the reader - no lock, no allocation, and a torn
        // read is impossible on a float. `gainReductionDb()` stays the tests' instrument: it reads a plain
        // double the audio thread is writing, which a message-thread caller must not.
        [[nodiscard]] float meterGainReductionDb() const noexcept { return meterGrDb_.load (std::memory_order_relaxed); }
        // …and the detector, read off the RUNNING follower for the same reason.
        [[nodiscard]] float detectorLevel() const noexcept { return detector_.value(); }
        [[nodiscard]] Real appliedDetectorMs() const noexcept
        {
            return static_cast<Real> (detector_.attackMs());
        }
        // The dB the gain computer is asking for THIS instant, before the ballistics smooth it —
        // the input half of the pair `gainReductionDb()` is the output half of, so a test can
        // separate "the curve is wrong" from "the smoothing is wrong".
        [[nodiscard]] Real targetGainDb() const noexcept { return targetGainDb_; }
        // Whether the sidechain filters are in circuit at all (the §9 "20 Hz ≈ Off" reading).
        [[nodiscard]] bool sidechainActive() const noexcept { return schpHz_ > kSchpOffHz; }

    private:
        // Everything one channel owns. The audio path is DUAL MONO; only the control signal is
        // shared (§0.7).
        struct Channel
        {
            TptSvf schp; // §0.6's detector high-pass — see THE SIDECHAIN
            ShoulderClip sat; // §0.8's shaper, stateful (one-sample ADAA memory)
        };

        // Re-resolves everything derived from the rows: the per-type voicing, the clamped times,
        // the α coefficients, the ratio/knee laws, the filter corners and the mix pair. Runs on
        // every setter — a message-thread path, so the cost is free (the `refreshCores` idiom).
        void refresh() noexcept;

        // §0.4's smoothing, one sample, in the dB domain. Returns `g[n]`. The three laws live
        // here and nowhere else; `Exponential` is bit-identical to `EnvelopeFollower` in
        // `GainDb` domain (see THE BALLISTICS, and the test that pins it).
        [[nodiscard]] Real ballistics (Real targetDb) noexcept;

        // §0.8 with the channel's own STATEFUL (ADAA) shaper — the audio path's counterpart to the
        // memoryless `saturateFor`. See the .cpp.
        [[nodiscard]] float saturate (Channel& c, float x) noexcept;

        // Clears the shapers. The law's operating point is 0 (`f(0) = 0` at any asymmetry), so its
        // memory starts there.
        void primeShapers() noexcept;

        Channel ch_[2];
        // §0.2's detector — ONE, because there is ONE control signal (§0.7). `Level` domain:
        // what it is handed is the linked, sidechain-filtered programme.
        EnvelopeFollower detector_;

        Real sampleRate_ = 48000.0;
        bool prepared_ = false;

        // The rows, in their own §22.8 units. The defaults are `ParamRegistry.cpp`'s own
        // (`comp` plain 3 / 74 / 40 / 30 / 50 / 24 / 20 / 40 / 100 through their curves), so an
        // instance somehow rendered before its first param push already behaves like the one the
        // registry describes rather than like a bare engine.
        Type type_ = Type::Bus;
        Real threshDb_ = -34.960629921259844;
        Real rowRatio_ = 2.569053964534461;
        Real rowAttackMs_ = 0.3011208789710753;
        Real rowReleaseMs_ = 52.893593322748245;
        Real makeupDb_ = -24.79155033153576; // …the −24.79 dB defect in waiting, unedited
        Real schpHz_ = 41.303858629592554;
        Real rowKneeDb_ = 7.559055118110236;
        Real mixPercent_ = 78.74015748031496;

        // The derived set, all resolved by `refresh()`.
        Real ratio_ = 2.0; // after `ratioFor` (Bus snaps 2/4/10)
        Real knee_ = 6.0; // after `kneeDbFor`
        Real attack_ = 0.3011208789710753; // after `attackMsFor` (a CLAMP, never a re-scale)
        Real release_ = 100.0; // after `releaseMsFor`
        Release policy_ = Release::Exponential;
        bool abi_ = false; // the FET's `ratio` row at its ABI position (OI-CMP-4)
        Real restingGrDb_ = 0.0; // ABI's "GR meter that rests off zero"; 0 otherwise
        Real attackCoef_ = 0.0; // α = exp(−1/(τ·fs)), from `EnvelopeFollower::alphaFor`
        // §12 capability 6 — the α at the MEMORY-LENGTHENED attack τ. The per-sample coefficient
        // is INTERPOLATED between `attackCoef_` and this one by the history, which is what makes
        // a state-dependent time constant cost no transcendental per sample (§12 row 6 asks for
        // exactly that). Equal to `attackCoef_` on the types with no memory, so the interpolation
        // is then a no-op by construction rather than by a branch.
        Real attackCoefSlow_ = 0.0;
        Real activeAttackCoef_ = 0.0; // …and the one the last sample used (the read-back)
        Real releaseCoef_ = 0.0;
        Real autoFastCoef_ = 0.0; // `DualTc`'s fast pole; the SLOW one is the release row
        Real autoSlowCoef_ = 0.0;
        Real autoHistCoef_ = 0.0;
        Real releaseDbPerSample_ = 0.0; // `ConstantRate`'s step
        Real makeupExponent_ = 0.0; // the makeup, pre-multiplied for `exp2`
        Real satAmount_ = 0.0;
        Real satAsym_ = 0.0; // §10's even-order asymmetry; 0 on three of the four types
        // …and whether the shaper is in circuit at all, so `amount = 0` is a bit-exact
        // pass-through exactly as `saturateFor` defines it. No shipped type reaches 0 (§10's
        // lightest is VCA's 0.10) — the branch exists so the static IS the reference at every
        // amount rather than at every amount but one.
        bool satActive_ = false;
        bool satEnabled_ = true; // `setSaturationEnabled` — false only on a detector-only instance

        // §0.4's smoothed gain in dB — the ONE piece of ballistics state, and what
        // `gainReductionDb()` reports. Rest is 0 dB (unity, no reduction).
        Real smoothedGainDb_ = 0.0;
        // [D-M-0312] `meterGainReductionDb()`'s storage - written once per block by `process`, zeroed by `reset`.
        std::atomic<float> meterGrDb_ { 0.0f };
        // …and `DualTc`'s extra state: the excess over the target, split between a fast and a
        // slow pole, plus the history that sets the split (§12 capability 4).
        Real autoFastDb_ = 0.0;
        Real autoSlowDb_ = 0.0;
        Real autoHistoryDb_ = 0.0;
        // The curve's current answer, before smoothing — `targetGainDb()`'s instrument.
        Real targetGainDb_ = 0.0;

        // §0.1's FEEDBACK tap: the previous sample's REDUCED output, per channel, which is what a
        // feedback-topology detector sees ("implement as a one-sample loop: gain applied this
        // sample is computed from last sample's reduced output"). Written on every sample
        // regardless of topology — one store, and it keeps the tap from being a cold start when a
        // live `type` edit switches a feedforward voicing to a feedback one.
        float lastWetL_ = 0.0f;
        float lastWetR_ = 0.0f;

        // The blend, resolved control-rate and held as the PAIR rather than as one number, so
        // the hot path is `wet·a + dry·b`: at mix 0 that is `wet·0 + dry·1` and at mix 100
        // `wet·1 + dry·0`, both EXACT in IEEE-754 whatever the compiler does with the
        // contraction — which is what makes the dry anchor bit-exact with no bypass branch, and
        // keeps the detector running through a closed mix so re-opening it is not a cold start
        // (§14's own note, and the `FltrEffect` precedent). NOTHING may be folded into
        // `wetGain_`: `dryGain_` is `1 − wetGain_`, so a gain hidden in the wet coefficient
        // would leak dry signal back in at full mix (the FLTR leaf-1b trap).
        float wetGain_ = 0.7874016f;
        float dryGain_ = 0.2125984f;
    };
}
