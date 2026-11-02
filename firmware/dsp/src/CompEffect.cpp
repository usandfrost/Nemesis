#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/CompEffect.h>

#include <cmath>
#include <limits>

namespace nemesis::dsp
{
    namespace
    {
        [[nodiscard]] Real clampD (Real v, Real lo, Real hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        // dB ↔ log2, so the hot path's two transcendentals are an `exp2`/`log2` pair rather
        // than a `pow`/`log10` one. Exact same law: 20·log10(x) == kDbPerLog2·log2(x).
        constexpr Real kDbPerLog2 = 6.020599913279624; // 20 / log2(10)
        constexpr Real kLog2PerDb = 0.1660964047443681; // log2(10) / 20

        constexpr Real kInf = std::numeric_limits<Real>::infinity();

        using Detector = EnvelopeFollower::Detector;
        using Release = CompEffect::Release;
        using Topology = CompEffect::Topology;

        // §10's four-type constant table — the whole of §9.1's design law as data. Every cell is
        // a value the TYPE fixes and no row can reach.
        //
        // THE DETECTOR WINDOWS AND THE SATURATION AMOUNTS ARE OWNER-TUNABLE DEFAULTS (M1-17f:
        // pick, document, don't ask). §10 gives the detection LAW per type and no window at all,
        // and gives saturation as a character word plus one figure per type; these are the
        // reading of that column.
        constexpr CompEffect::Voicing kVoicing[CompEffect::kNumTypes] = {
            // FIELD ORDER, once, because the rows below are one line each (a `ColumnLimit: 0`
            // house style packs a braced list up to the first trailing comment, so per-cell
            // comments would land against the wrong cell):
            //   detector · detectorMs · topology · release · autoAboveMs · autoFastMs ·
            //   attackMin/Max · releaseMin/Max · kneeMin/Max · attackMemory · saturation · satAsym
            //
            // Every attack/release span below is `core::compAttackMs`/`compReleaseMs`'s own
            // endpoint pair for that type — asserted, not assumed (`test_fx_seam.cpp`).
            //
            // VCA — §1 / §11.4, the dbx 160 lineage, and §10's REFERENCE implementation: the one
            // type that honours all five continuous rows LITERALLY, so the gain computer can be
            // tested in isolation against it. Feedforward, RMS (Blackmer), and its one sourced
            // deviation from the textbook is the release LAW, published in dB/s rather than as a τ
            // (§11.4, grade A). No AUTO, no memory, no even-order bias, and §10's "~0.1
            // (near-clean)" saturation.
            { Detector::Rms, 10.0, Topology::Feedforward, Release::ConstantRate, kInf, 0.0, 0.05, 100.0, 5.0, 2000.0, 0.0, 24.0, 0.0, 0.10, 0.0 },
            // FET — §2 / §11.1, the UREI/UA 1176LN. FEEDBACK topology (grade A: a FET as a
            // voltage-controlled resistor inside a feedback detector loop), instantaneous PEAK
            // detection, §11.1's grade-A 20 µs – 800 µs attack and 50 ms – 1.1 s release — the
            // range the owner's OI-CMP-3 ruling unlocked, and the type's single most-cited
            // characteristic — a forced hard-ish 0–6 dB knee (§10), and §10's "~0.4, strong — FET
            // + in/out transformers". All-buttons-in rides the `ratio` row's top position
            // (OI-CMP-4); see `kAbi*`.
            { Detector::Peak, 0.0, Topology::Feedback, Release::Exponential, kInf, 0.0, 0.02, 0.8, 50.0, 1100.0, 0.0, 6.0, 0.0, 0.40, 0.0 },
            // Opto — §3 / §11.2, the Teletronix LA-2A (T4B cell). FEEDBACK, slow RMS-ish
            // detection, and the type §7 singles out: "Opto is a MODEL, not a preset". Three cells
            // carry that. Its release law is UNCONDITIONALLY dual-TC — §11.2's grade-A spec line
            // "60 ms for 50 % release, 0.5 to 5 seconds for complete release", which is the
            // document's own load-bearing source and is quoted here as a 60 ms fast pole against
            // the row's 0.5–5 s tail. Its attack is the photocell's 5–20 ms optical lag with
            // MEMORY (attackMemory 1.5 → up to 2.5× slower when the cell has been driven hard and
            // long, §12 capability 6). Its knee is forced soft (6–24 dB). And its saturation
            // is ASYMMETRIC (the positive half knees earlier), because §10 asks for "tube/optical
            // warmth, even-order" and a symmetric clip makes no even harmonic at all.
            { Detector::Rms, 30.0, Topology::Feedback, Release::DualTc, 0.0, 60.0, 5.0, 20.0, 500.0, 5000.0, 6.0, 24.0, 1.5, 0.30, 0.6 },
            // Bus — §4 / §11.3, the SSL G-series, and the shipped DEFAULT type. Feedforward VCA
            // element, a 15 ms RMS-leaning detector, §11.3's grade-A stepped attack (0.1–30 ms)
            // and release (0.1–1.2 s), AUTO as the position past the last release stop with a
            // ~200 ms fast pole against the row's 1.2 s top, §10's "forced soft" 4–18 dB knee, and
            // §10's "~0.15 light bus colour".
            { Detector::Rms, 15.0, Topology::Feedforward, Release::Exponential, CompEffect::kBusAutoThresholdMs, 200.0, 0.1, 30.0, 100.0, 1200.0, 4.0, 18.0, 0.0, 0.15, 0.0 },
        };
    }

    //--------------------------------------------------------------------------
    // The laws. See the header for WHY each one is shaped this way.
    //--------------------------------------------------------------------------

    const CompEffect::Voicing& CompEffect::voicingFor (const Type type) noexcept
    {
        const int i = static_cast<int> (type);
        return kVoicing[i >= 0 && i < kNumTypes ? i : 0];
    }

    Real CompEffect::levelDbFor (const Real detected) noexcept
    {
        // §0.2 verbatim, including its epsilon. `detected` is a LEVEL (the follower has already
        // taken the root under `Rms`), so it is ≥ 0 and the log is always defined.
        return kDbPerLog2 * std::log2 ((detected < 0.0 ? -detected : detected) + kDetectorEpsilon);
    }

    Real CompEffect::gainDbFor (const Real levelDb, const Real threshDb, const Real ratio, const Real kneeDb) noexcept
    {
        // Downward compression only: a ratio under 1:1 would be an EXPANDER, which is not what
        // this row is (§0.3's "upward expansion is the same curve with different R" is a
        // different product decision, and there is no row that asks for it).
        const Real r = ratio < kRatioMin ? kRatioMin : ratio;
        const Real w = kneeDb > 0.0 ? kneeDb : 0.0;
        const Real delta = levelDb - threshDb;

        // HARD KNEE is its own arm rather than a limit of the soft one: the soft arm divides by
        // 2W, so at W = 0 there would be a 0/0 exactly at the threshold.
        if (w <= 0.0)
            return delta <= 0.0 ? 0.0 : delta / r - delta;

        const Real twoDelta = 2.0 * delta;
        if (twoDelta < -w) // below the knee: no reduction at all
            return 0.0;
        if (twoDelta <= w) // inside the knee: the quadratic interpolation §0.3 writes
        {
            const Real t = delta + 0.5 * w;
            return (1.0 / r - 1.0) * t * t / (2.0 * w);
        }
        // above the knee: `out_dB = T + Δ/R`, so the reduction is what that costs
        return threshDb + delta / r - levelDb;
    }

    bool CompEffect::isAllButtonsIn (const Type type, const Real rowRatio) noexcept
    {
        // Owner ruling on OI-CMP-4(a): for the FET type ONLY, the `ratio` row's top position IS
        // all-buttons-in, and that position is where `kRatio` returns ∞. The other three types read
        // the same position as a plain LIMITER, which is what it means everywhere but a 1176.
        return type == Type::Fet && !std::isfinite (rowRatio);
    }

    Real CompEffect::ratioFor (const Type type, const Real rowRatio) noexcept
    {
        // ∞ is a position of the row (`kRatio` returns it at v = 127) and it means LIMIT on every
        // type — and on the FET it additionally means ABI (`isAllButtonsIn`), whose extra
        // character is applied in `refresh()` rather than here, because a RATIO is all this
        // function is allowed to answer. It survives every arm below untouched.
        if (!std::isfinite (rowRatio))
            return rowRatio;
        const Real r = rowRatio < kRatioMin ? kRatioMin : rowRatio;

        // §11.1/§11.3's stepped stops, both grade A: nearest in the LOG domain, because the row
        // itself is exponential and the stops are not evenly spaced — a linear nearest would put
        // 4:1 two thirds of the way from 2 to 10.
        const auto snap = [r] (const Real* stops, int count) {
            Real best = stops[0];
            Real bestDist = kInf;
            for (int i = 0; i < count; ++i)
            {
                const Real d = std::abs (std::log2 (r / stops[i]));
                if (d < bestDist)
                {
                    bestDist = d;
                    best = stops[i];
                }
            }
            return best;
        };

        switch (type)
        {
            case Type::Bus:
                // §4/§11.3's three (and "the stated ratios are asymptotic" — §0.3's soft knee is
                // what makes them so).
                return snap (kBusRatioStops, kNumBusRatioStops);
            case Type::Fet:
                // §11.1: four interlocking latching PUSH-BUTTONS, 4/8/12/20 — not a rotary
                // switch, so snapping is the literal hardware behaviour.
                return snap (kFetRatioStops, kNumFetRatioStops);
            case Type::Vca: // §1: continuous 1:1 … ∞, literal — this IS the reference type
            case Type::Opto: // §3: continuous and gentle; §11.2 refuses to publish a figure
                break;
        }
        return r;
    }

    Real CompEffect::kneeDbFor (const Type type, const Real rowKneeDb) noexcept
    {
        const Voicing& v = voicingFor (type);
        const Real t = clampD (rowKneeDb, 0.0, kKneeDbMax) / kKneeDbMax;
        // A rescale of the row's own travel into the type's window — and unlike the TIME rows,
        // this one is not an OI-CMP-3 problem: `knee` has no sourced hardware number to be
        // outside of (§10 gives a character, "hard-ish" / "soft"), so the window IS the law
        // rather than a stand-in for a range the row cannot reach.
        return v.kneeMinDb + (v.kneeMaxDb - v.kneeMinDb) * t;
    }

    Real CompEffect::attackMsFor (const Type type, const Real rowMs) noexcept
    {
        const Voicing& v = voicingFor (type);
        // A CLAMP, never a re-scale — see the header. Honest everywhere inside the type's own
        // span, and only ever disagrees with the display outside it.
        return clampD (rowMs, v.attackMinMs, v.attackMaxMs);
    }

    Real CompEffect::releaseMsFor (const Type type, const Real rowMs) noexcept
    {
        const Voicing& v = voicingFor (type);
        return clampD (rowMs, v.releaseMinMs, v.releaseMaxMs);
    }

    Real CompEffect::vcaReleaseDbPerSec (const Real rowMs) noexcept
    {
        // "The row is the time to recover `kVcaReleaseSpanDb`" — which puts §11.4's published
        // ≈120 dB/s at the row's own mid travel (v = 64 ⇒ ≈100 ms ⇒ 12 dB / 0.1 s).
        const Real ms = rowMs > 0.0 ? rowMs : kReleaseMsMin;
        return kVcaReleaseSpanDb * 1000.0 / ms;
    }

    ShoulderClip::Side CompEffect::saturationSideFor (const Real amount) noexcept
    {
        // THE SATURATION's law, one half: `k = 1 + kSatRange·a`, depth `d = 1 − 1/k` (written as
        // `r/(1 + r)` so it carries no cancellation as `a → 0`), knee `1/k`.
        const Real r = kSatRange * clampD (amount, 0.0, 1.0);
        return ShoulderClip::sideFor (r / (1.0 + r));
    }

    Real CompEffect::saturateFor (const Real x, const Real amount, const Real asym) noexcept
    {
        // `amount = 0` is the exact identity — not in circuit, rather than a curve that happens to
        // reproduce `x`.
        if (!(amount > 0.0))
            return x;
        const Real a = clampD (amount, 0.0, 1.0);
        const Real b = clampD (asym, -1.0, 1.0);
        return ShoulderClip::shape (x, saturationSideFor (a * (1.0 + b)), saturationSideFor (a * (1.0 - b)));
    }

    //--------------------------------------------------------------------------
    // Lifecycle
    //--------------------------------------------------------------------------

    void CompEffect::prepare (const Real sampleRate, const int maxBlockSamples)
    {
        (void) maxBlockSamples; // nothing here is block-sized; nothing here allocates at all

        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (Channel& c : ch_)
            c.schp.prepare (sampleRate_);
        // §0.2's detector, in the LEVEL domain — the argument is an audio sample. Stated here
        // rather than left to the primitive's default, because the DOMAIN is a decision: the
        // §0.4 dB-gain smoothing is this class's own (see THE BALLISTICS), so the primitive's
        // `GainDb` domain is not what this instance is for.
        detector_.setDomain (EnvelopeFollower::Domain::Level);
        detector_.prepare (sampleRate_);

        // AFTER the members' own prepare: `prepare` re-resolves the follower's coefficients from
        // its current times and returns it to rest, and the SVFs clear their state, so every
        // stored row has to be re-offered at the new rate.
        refresh();
        // AFTER `refresh()`, which is where the bias and the pre-gain are resolved.
        primeShapers();
        smoothedGainDb_ = 0.0;
        autoFastDb_ = 0.0;
        autoSlowDb_ = 0.0;
        autoHistoryDb_ = 0.0;
        targetGainDb_ = 0.0;
        meterGrDb_.store (0.0f, std::memory_order_relaxed); // [D-M-0312] the published copy rests with the state it mirrors
        activeAttackCoef_ = attackCoef_;
        lastWetL_ = 0.0f;
        lastWetR_ = 0.0f;
        prepared_ = true;
    }

    void CompEffect::reset() noexcept
    {
        // AUDIO thread (the chain's §DSP D6 hard-switch flush), so alloc-free and noexcept.
        // EVERY state, not only the detector: a ballistics state still holding 12 dB of
        // reduction would duck the first block after a chain reorder, which is the same
        // stuck-tail class `reset()` exists to close off.
        for (Channel& c : ch_)
            c.schp.reset();
        primeShapers();
        detector_.reset();
        smoothedGainDb_ = 0.0;
        autoFastDb_ = 0.0;
        autoSlowDb_ = 0.0;
        autoHistoryDb_ = 0.0;
        targetGainDb_ = 0.0;
        meterGrDb_.store (0.0f, std::memory_order_relaxed); // [D-M-0312] the published copy rests with the state it mirrors
        activeAttackCoef_ = attackCoef_;
        // …and §0.1's feedback tap, which is state too: a loop still holding the last loud sample
        // would compute the first gain of the next passage from the previous one.
        lastWetL_ = 0.0f;
        lastWetR_ = 0.0f;
    }

    //--------------------------------------------------------------------------
    // The audio path
    //--------------------------------------------------------------------------

    Real CompEffect::ballistics (const Real targetDb) noexcept
    {
        // "Attack" is movement AWAY from rest, i.e. a target asking for MORE reduction — §0.4's
        // `if gain_dB < g[n−1]` exactly, in all three laws.
        const bool attacking = targetDb < smoothedGainDb_;

        // §12 CAPABILITY 6 — the T4 photocell's MEMORY, and the reason Opto is a model rather
        // than a preset (§7). The attack τ is a function of how hard AND how long the cell has
        // been driven, so the coefficient is INTERPOLATED between the row's own α and the
        // memory-lengthened one by the history state — no transcendental per sample, which is
        // what §12 row 6 asks for in as many words. On the three types with no memory
        // `attackCoefSlow_ == attackCoef_`, so this is a no-op by construction rather than a
        // branch, and the read-back still reports the coefficient actually used.
        const Real memory = clampD (autoHistoryDb_ / kOptoMemoryFullDb, 0.0, 1.0);
        activeAttackCoef_ = attackCoef_ + memory * (attackCoefSlow_ - attackCoef_);

        switch (policy_)
        {
            case Release::Exponential:
            {
                // §0.4 verbatim. BIT-IDENTICAL to `EnvelopeFollower` in `GainDb` domain at the
                // same times — including the rest snap below, which is why the floor is a copy
                // of the primitive's rather than a rounder number.
                const Real a = attacking ? activeAttackCoef_ : releaseCoef_;
                smoothedGainDb_ = a * smoothedGainDb_ + (1.0 - a) * targetDb;
                break;
            }
            case Release::ConstantRate:
            {
                // §12 capability 5. The ATTACK is still a one-pole (§10: "Attack law: constant
                // τ" for this type); only the recovery is a fixed slope in dB, which is the
                // thing a one-pole cannot be.
                if (attacking)
                {
                    smoothedGainDb_ = activeAttackCoef_ * smoothedGainDb_ + (1.0 - activeAttackCoef_) * targetDb;
                }
                else
                {
                    const Real stepped = smoothedGainDb_ + releaseDbPerSample_;
                    smoothedGainDb_ = stepped > targetDb ? targetDb : stepped;
                }
                break;
            }
            case Release::DualTc:
            {
                // §12 capability 4: the reduction in force is SPLIT between a fast and a slow
                // pole, and the split is set by how much reduction has recently been applied — so
                // the recovery is a sum of two exponentials whose knee moves with programme
                // history. §11.3's "~0.1–0.3 s fast pole plus a ~1.2–3 s slow pole blended by
                // recent GR", as our choice.
                if (attacking)
                {
                    const Real total = activeAttackCoef_ * smoothedGainDb_ + (1.0 - activeAttackCoef_) * targetDb;
                    // THE SPLIT IS OF THE WHOLE REDUCTION, NOT OF ITS EXCESS OVER THE TARGET, and
                    // that distinction is the difference between working and not: in a sustained
                    // passage the attack pole SETTLES ON the target, so the excess is ~0 and a
                    // split of it would leave both poles empty — the release would then be
                    // instant, which is exactly what the first version of this measured (a
                    // 1.5 s tail that arrived at exactly 0 dB). Re-split every attack sample, so
                    // a passage that keeps driving harder keeps moving reduction into the slow
                    // pole, which is what lengthens the tail.
                    const Real share = kAutoMaxSlowShare * clampD (autoHistoryDb_ / kAutoFullShareDb, 0.0, 1.0);
                    autoSlowDb_ = share * total;
                    autoFastDb_ = total - autoSlowDb_;
                    smoothedGainDb_ = total;
                }
                else
                {
                    // The split FREEZES and each pole decays toward REST on its own: the fast one
                    // is gone within its own few τ, leaving the slow tail — the knee C-CMP-4
                    // measures. Never past what the curve still asks for, though: a partial
                    // release stops at the target rather than releasing through it and having to
                    // re-attack on the next sample.
                    autoFastDb_ *= autoFastCoef_;
                    autoSlowDb_ *= autoSlowCoef_;
                    const Real decayed = autoFastDb_ + autoSlowDb_;
                    smoothedGainDb_ = decayed < targetDb ? decayed : targetDb;
                }
                break;
            }
        }

        // "How much / how long GR has been applied" — the history state itself, a slow one-pole on
        // the reduction in force. It drives §12 capability 4's blend AND capability 6's τ, so it is
        // updated on EVERY policy and in both branches: a tail that has been long is exactly what
        // should still be remembered, and the photocell's memory is not a property of the release
        // law (a `Release::DualTc`-only update was where the first version had it, which left
        // Opto's attack memory reading the history the release had already spent).
        autoHistoryDb_ = autoHistCoef_ * autoHistoryDb_ + (1.0 - autoHistCoef_) * -smoothedGainDb_;

        // §12 capability 8, in the dB-gain domain: a state this close to unity ARRIVES at
        // exactly 0 dB instead of walking the subnormal range. The primitive's own snap, so the
        // `Exponential` law stays bit-identical to it.
        if (smoothedGainDb_ < kRestFloorDb && smoothedGainDb_ > -kRestFloorDb)
            smoothedGainDb_ = 0.0;

        return smoothedGainDb_;
    }

    void CompEffect::primeShapers() noexcept
    {
        // §0.8's shaper is an ADAA — it returns the AVERAGE of the curve over the sample interval,
        // so its one-sample memory has to START at the operating point or the first sample reads
        // HALF a step. The law's operating point is 0 at any asymmetry, so a reset IS the prime.
        for (Channel& c : ch_)
            c.sat.reset();
    }

    float CompEffect::saturate (Channel& c, const float x) noexcept
    {
        // §0.8, with the STATEFUL (ADAA) shaper in place of `saturateFor`'s memoryless one. No gain
        // in, none out: the law is already unity below its knee and full scale above its shoulder.
        // The two agree everywhere the ADAA's one-sample average is not the point, which is what
        // makes the static a legitimate reference for the alias-floor comparison.
        return c.sat.processSample (x);
    }

    void CompEffect::process (float* left, float* right, const int numSamples) noexcept
    {
        if (left == nullptr || right == nullptr || numSamples <= 0 || !prepared_)
            return;

        // Resolved once per block: §9's reading of the row's bottom rail as OFF, so at
        // `schp = 20 Hz` the two filters are not run and the detector sees the programme.
        const bool sidechain = schpHz_ > kSchpOffHz;
        // §0.1's topology, and it is ONE branch per block rather than per sample: which SIGNAL the
        // detector is handed. §7 calls the one-sample feedback loop "cheap and worth it", and this
        // is the whole of its cost — the primitive takes its detector input as an argument
        // (§12 capability 2), so a feedback design is a different tap and not a different class.
        const bool feedback = voicingFor (type_).topology == Topology::Feedback;

        for (int n = 0; n < numSamples; ++n)
        {
            const float xl = left[n];
            const float xr = right[n];

            // FEEDFORWARD sees the input; FEEDBACK sees the previous sample's REDUCED OUTPUT —
            // §0.1's "gain applied this sample is computed from last sample's reduced output". That
            // one-sample lag is what softens the effective curve, and C-CMP-5 measures it.
            const float detL = feedback ? lastWetL_ : xl;
            const float detR = feedback ? lastWetR_ : xr;

            // §0.6 FIRST, then §0.7: filter each channel, THEN link. A link is a
            // rectification, and rectifying before filtering would hand the filter a signal
            // whose spectrum is no longer the programme's.
            const float dl = sidechain ? ch_[0].schp.processSample (detL).hp : detL;
            const float dr = sidechain ? ch_[1].schp.processSample (detR).hp : detR;
            const float al = dl < 0.0f ? -dl : dl;
            const float ar = dr < 0.0f ? -dr : dr;
            // ONE control signal for both channels — §0.7's whole instruction, and what keeps
            // the stereo image still. `max` over a sum (which cancels out-of-phase content and
            // reads 2× on correlated content) and over an average (which halves a hard-panned
            // transient, the very transient a compressor should catch).
            const float env = detector_.processSample (al > ar ? al : ar);

            // §0.2 → §0.3 → §0.4: level in dB, the static curve, then the ballistics.
            targetGainDb_ = gainDbFor (levelDbFor (static_cast<Real> (env)), threshDb_, ratio_, knee_);
            const Real g = ballistics (targetGainDb_);

            // §0.5, with the makeup folded into the same exponent so there is ONE `exp2` per
            // sample for both channels rather than a `pow` per channel. `restingGrDb_` is ABI's
            // "GR meter that rests off zero" and is exactly 0 everywhere else.
            const auto gain = static_cast<float> (std::exp2 ((g - restingGrDb_) * kLog2PerDb + makeupExponent_));

            // §0.8, after the makeup — see THE SATURATION for why that is the 1176's own
            // workflow rather than an arbitrary position.
            float wl = xl * gain;
            float wr = xr * gain;
            if (satActive_)
            {
                wl = saturate (ch_[0], wl);
                wr = saturate (ch_[1], wr);
            }

            // §0.1's feedback TAP, stored before the blend: what a feedback detector sees is the
            // compressor's own reduced output, not the dry/wet mix — the mix is a §5 wrapper
            // AROUND the compressor and is not inside its loop. Written on every sample whatever
            // the topology, so a live `type` edit into a feedback voicing is not a cold start.
            lastWetL_ = wl;
            lastWetR_ = wr;

            // §5's parallel blend. At `mix = 0` this is `wet·0 + dry·1`, exactly the input.
            left[n] = wetGain_ * wl + dryGain_ * xl;
            right[n] = wetGain_ * wr + dryGain_ * xr;
        }
        // [D-M-0312] The GUI's GR meter: one snapshot per block, AFTER the loop so it is the value the last sample
        // used (the same instant `gainReductionDb()` reads), plus ABI's resting offset the multiplier also carries.
        meterGrDb_.store (static_cast<float> (-smoothedGainDb_ + restingGrDb_), std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------------
    // Control rate
    //--------------------------------------------------------------------------

    void CompEffect::refresh() noexcept
    {
        const Voicing& v = voicingFor (type_);

        // ALL-BUTTONS-IN first, because three other cells depend on it (OI-CMP-4).
        abi_ = isAllButtonsIn (type_, rowRatio_);
        restingGrDb_ = abi_ ? kAbiRestingGrDb : 0.0;

        ratio_ = ratioFor (type_, rowRatio_);
        // ABI's knee is the "distorted, lagging" one §11.1 describes — a fixed WIDTH that
        // overrides the type's own window, because the reconfigured resistor network is not the
        // knob's knee any more.
        knee_ = abi_ ? kAbiKneeDb : kneeDbFor (type_, rowKneeDb_);
        // …and its attack is "lagged / less controlled", which is a MULTIPLE of the row's τ rather
        // than a fixed time: the knob still does something, it is just no longer in charge.
        attack_ = attackMsFor (type_, rowAttackMs_) * (abi_ ? kAbiAttackLag : 1.0);
        release_ = releaseMsFor (type_, rowReleaseMs_);
        // §4's AUTO is a POSITION of the release row, not a switch — see `Voicing::autoAboveMs`.
        // Read from the ROW rather than from the clamped time, because the clamp's own ceiling is
        // the AUTO stop itself and comparing the clamped value would make every position AUTO.
        policy_ = rowReleaseMs_ >= v.autoAboveMs ? Release::DualTc : v.release;

        // The α law is the PRIMITIVE's, called at control rate — the single place
        // `α = exp(−1/(τ·fs))` lives, for every pole in this class.
        attackCoef_ = EnvelopeFollower::alphaFor (static_cast<float> (attack_), sampleRate_);
        // §12 capability 6's second coefficient: the same τ lengthened by the photocell's memory.
        // EQUAL to the first when the type has no memory, which is what makes the per-sample
        // interpolation a no-op there without a branch.
        attackCoefSlow_ = EnvelopeFollower::alphaFor (
            static_cast<float> (attack_ * (1.0 + v.attackMemory)), sampleRate_);
        activeAttackCoef_ = attackCoef_;
        releaseCoef_ = EnvelopeFollower::alphaFor (static_cast<float> (release_), sampleRate_);
        // `DualTc`'s two poles: the type's own FAST stage, and the release ROW as the slow tail —
        // which is why Opto's dual stage is §11.2's grade-A pair (60 ms against 0.5–5 s) and Bus's
        // AUTO is §11.3's (~200 ms against the row's 1.2 s top) with no third constant.
        autoFastCoef_ = EnvelopeFollower::alphaFor (static_cast<float> (v.autoFastMs), sampleRate_);
        autoSlowCoef_ = releaseCoef_;
        autoHistCoef_ = EnvelopeFollower::alphaFor (static_cast<float> (kAutoHistoryMs), sampleRate_);
        releaseDbPerSample_ = vcaReleaseDbPerSec (release_) / sampleRate_;

        // §0.2's detection law and its own averaging window, both per type. Equal attack and
        // release make the branching one-pole a plain averager, which is what a detection
        // WINDOW is; the ballistics that shape the FEEL are on the gain, per §0.4.
        detector_.setDetector (v.detector);
        detector_.setAttackMs (static_cast<float> (v.detectorMs));
        detector_.setReleaseMs (static_cast<float> (v.detectorMs));

        // The makeup, floored and pre-multiplied so the hot path adds it inside the exponent.
        makeupExponent_ = (makeupDb_ > kMakeupDbFloor ? makeupDb_ : kMakeupDbFloor) * kLog2PerDb;

        // §0.8's amount, and ABI's "pronounced distortion" overriding it (§2's "extra saturation").
        satAmount_ = clampD (abi_ ? kAbiSaturation : v.saturation, 0.0, 1.0);
        satAsym_ = v.satAsym;
        satActive_ = satEnabled_ && satAmount_ > 0.0;
        // THE SATURATION's law, both halves, handed to the stateful shapers. `setSides` keeps their
        // ADAA history and re-reads it on the new curve, so a live `type` edit needs no re-prime.
        {
            const Real b = clampD (satAsym_, -1.0, 1.0);
            const ShoulderClip::Side pos = saturationSideFor (satAmount_ * (1.0 + b));
            const ShoulderClip::Side neg = saturationSideFor (satAmount_ * (1.0 - b));
            for (Channel& c : ch_)
                c.sat.setSides (pos, neg);
        }
        for (Channel& c : ch_)
            c.schp.setCutoff (static_cast<float> (clampD (schpHz_, kSchpHzMin, kSchpHzMax)),
                static_cast<float> (kSchpQ));

        // The blend as the PAIR — see the header for why nothing may be folded into `wetGain_`.
        const auto wet = static_cast<float> (clampD (mixPercent_, 0.0, kMixPercentMax) / kMixPercentMax);
        wetGain_ = wet;
        dryGain_ = 1.0f - wet;
    }

    void CompEffect::setType (const int index) noexcept
    {
        type_ = static_cast<Type> (index >= 0 && index < kNumTypes ? index : 0);
        refresh();
    }

    void CompEffect::setThresholdDb (const float db) noexcept
    {
        threshDb_ = static_cast<Real> (db);
        refresh();
    }

    void CompEffect::setRatio (const float ratio) noexcept
    {
        rowRatio_ = static_cast<Real> (ratio);
        refresh();
    }

    void CompEffect::setAttackMs (const float ms) noexcept
    {
        rowAttackMs_ = static_cast<Real> (ms);
        refresh();
    }

    void CompEffect::setReleaseMs (const float ms) noexcept
    {
        rowReleaseMs_ = static_cast<Real> (ms);
        refresh();
    }

    void CompEffect::setMakeupDb (const float db) noexcept
    {
        makeupDb_ = static_cast<Real> (db);
        refresh();
    }

    void CompEffect::setSidechainHpHz (const float hz) noexcept
    {
        schpHz_ = static_cast<Real> (hz);
        refresh();
    }

    void CompEffect::setKneeDb (const float db) noexcept
    {
        rowKneeDb_ = static_cast<Real> (db);
        refresh();
    }

    void CompEffect::setMixPercent (const float percent) noexcept
    {
        mixPercent_ = static_cast<Real> (percent);
        refresh();
    }

    void CompEffect::setSaturationEnabled (const bool on) noexcept
    {
        satEnabled_ = on;
        refresh();
    }

    void CompEffect::setAppliedUnit (const std::string_view field, const Real value) noexcept
    {
        const auto f = static_cast<float> (value);
        if (field == "type")
            setType (static_cast<int> (value)); // ENUM: the plain ordinal (T-CORE-06)
        else if (field == "thresh")
            setThresholdDb (f);
        else if (field == "ratio")
            setRatio (f);
        else if (field == "attack")
            setAttackMs (f);
        else if (field == "release")
            setReleaseMs (f);
        else if (field == "makeup")
            setMakeupDb (f);
        else if (field == "schp")
            setSidechainHpHz (f);
        else if (field == "knee")
            setKneeDb (f);
        else if (field == "mix")
            setMixPercent (f);
        // Anything else — `on`, which is the CHAIN's bypass (a graph-node atomic), exactly as in
        // every sibling effect — is ignored, which is what keeps an unhandled row inert rather
        // than half-wired.
    }

    std::optional<Real> CompEffect::appliedUnit (const std::string_view field) const noexcept
    {
        // The ROW as dialed, which is what a surface has to show. What is actually IN FORCE
        // (after the per-type clamp, snap and knee window) is the `applied*` accessors' job —
        // the two-regime split `PhaserEffect` and `FltrEffect` already document.
        if (field == "type")
            return static_cast<Real> (type_);
        if (field == "thresh")
            return threshDb_;
        if (field == "ratio")
            return rowRatio_;
        if (field == "attack")
            return rowAttackMs_;
        if (field == "release")
            return rowReleaseMs_;
        if (field == "makeup")
            return makeupDb_;
        if (field == "schp")
            return schpHz_;
        if (field == "knee")
            return rowKneeDb_;
        if (field == "mix")
            return mixPercent_;
        return std::nullopt;
    }
}
