#pragma once

#include <nemesis/dsp/Real.h>
#include <cmath>
#include <cstdint>

namespace nemesis::dsp
{
    // EnvelopeFollower — the shared L1 level detector / envelope follower (T-PRIM-02).
    // `docs/specs/specs/dsp/compression.md` §0.2 (level detection), §0.4 (branching ballistics) and
    // §12 (the capability table this class is scoped by).
    //
    // ================================ THE API CONTRACT ==================================
    // **THE DETECTOR INPUT IS AN ARGUMENT. THIS CLASS NEVER OWNS THE AUDIO MULTIPLY.** That is
    // §12 capability 2, and it is the whole reason this is a primitive rather than a private stage
    // of the first consumer that needed one. `processSample (x)` takes the sample you want detection
    // performed ON and returns the smoothed envelope; applying anything to audio is the CALLER's job.
    //
    // Why it is non-negotiable: a FET/Opto-lineage compressor is a FEEDBACK design (§0.1) whose
    // detector sees the *processed output*, one sample late, while FLTR's auto-wah follower sees the
    // *input* directly. Both are this same class with a different tap:
    //
    //     feedforward (FLTR, Era, VCA/Bus):   env = f.processSample (in);        out = in * gain(env);
    //     feedback    (FET, Opto):            env = f.processSample (lastOut);   lastOut = in * gain(env);
    //
    // A follower that read an audio signal it owned could not express the second form AT ALL, and
    // retrofitting the argument later inverts the whole interface. Do not add a `process(buffer)`
    // convenience that closes the loop internally — that is the mistake this contract exists to
    // prevent. Sidechain filtering (§0.6) and stereo linking (§0.7) are free for the same reason:
    // the caller filters or sums whatever it hands in.
    // ====================================================================================
    //
    // SCOPE — §12's capability rows 1 · 2 · 3 · 7 · 8 · 10, which its unit-cost statement proves are
    // the WHOLE requirement for FLTR's `env`/`envatk`/`envrel` rows (filter.md §0) and for Era's
    // `comp` row (recording_era.md §11.4). Rows 4 (two release poles + slow history), 5 (constant
    // dB/s release), 6 (state-dependent τ — the T4 photocell memory) and 9 (oversampled / true-peak
    // detection) are deliberately NOT here: they are Comp-only, and they belong in a Comp-owned
    // ballistics layer built ON this class. Neither is the GAIN COMPUTER (§0.3's static curve) —
    // that is each consumer's own, because each consumer's curve is different.
    //
    // WHAT IT DOES, per sample:
    //   1. DETECT (`Detector`, capability 1) — `Peak` full-wave rectifies (|x|); `Rms` squares, and
    //      the smoother then averages the mean-square with `value()` taking the square root. Both are
    //      §0.2 verbatim.
    //   2. SMOOTH (capability 3) — a BRANCHING (decoupled) one-pole: `s = α·s + (1−α)·target`, with
    //      α chosen per sample by whether the follower is moving AWAY from rest (attack) or back
    //      toward it (release). No overshoot, and attack/release are independent.
    //   3. FLUSH (capability 8) — a state within `kDenormalFloor` of rest snaps exactly to rest, so a
    //      decay into silence lands on 0 rather than walking the subnormal range and stalling the FPU.
    //
    // `Domain` selects WHAT is being smoothed — §0.4's "where you smooth matters":
    //   * `Level`  — the argument is an audio sample. Rest is 0; a RISING detected level is the
    //     attack. This is FLTR's and Era's mode, and a feedforward comp's level detector.
    //   * `GainDb` — the argument is already a dB-domain GAIN (typically ≤ 0 dB, straight out of a
    //     §0.3 gain computer). It is NOT rectified (|−12 dB| would be a boost where a cut was asked
    //     for) and the branch INVERTS: rest is 0 dB, so a FALLING (more negative) target means more
    //     gain reduction and is therefore the ATTACK — §0.4's `if gain_dB < g[n−1]` exactly. The
    //     `Detector` setting is ignored in this domain (a dB gain is already a detected quantity).
    //
    // ONE DELIBERATE SIMPLIFICATION, stated rather than hidden: §0.2 draws the RMS averager with its
    // own α, separate from the ballistics. Here the branching attack/release one-pole IS the
    // mean-square averager — one filter, not two. That is what FLTR and Era want (their "attack" and
    // "release" ARE the averaging), and the branch decision is unaffected because comparing x² with
    // the mean-square state is monotone-equivalent to comparing |x| with the RMS. A consumer that
    // needs an RMS window independent of its ballistics cascades two instances (an `Rms` follower
    // with equal times into a second follower) — no change here.
    //
    // CONVENTION, and it differs from `AdsrEnvelope`/`PunchTransient` on purpose: `processSample()`
    // UPDATES first and returns the envelope INCLUDING the sample just handed in. A detector that
    // returned the previous value would add a sample of lag to every consumer and would put TWO
    // samples of delay in a feedback loop that is designed to have one.
    //
    // RT-safety (§2.3, capability 10): `prepare()` and the two time setters resolve the coefficients
    // — the only libm here is the `exp` in the .cpp, at CONTROL rate. `processSample()` is inline,
    // allocation-free, lock-free and branch-cheap (the `Rms` path's `sqrt` is a single arm64 `fsqrt`,
    // not a libm call — the sanctioned category BbdDelayLine already uses). No RNG, so a given
    // (params + input) renders bit-identically; `reset()` returns the state to exact rest and leaves
    // the parameters alone. Coefficients are settable per block with no click BY CONSTRUCTION: the
    // state is continuous across a coefficient change, so nothing is re-prepared and nothing jumps.
    // Computed in double, emitted as float. No JUCE. Not thread-safe against `prepare()`.
    class EnvelopeFollower
    {
    public:
        // §12 capability 1 — the detection law. Ignored when the domain is `GainDb`.
        enum class Detector : std::uint8_t {
            Peak, // full-wave rectify, |x| — transient-accurate (FET lineage, FLTR's auto-wah)
            Rms // mean-square one-pole → sqrt — loudness-like (dbx / SSL lineage)
        };

        // §12 capability 3 — WHICH quantity the branching one-pole smooths. See the class note: the
        // two domains differ in rectification AND in which direction counts as the attack.
        enum class Domain : std::uint8_t {
            Level, // an audio sample; rest 0; RISING is the attack
            GainDb // a dB gain (≤ 0 dB); rest 0 dB; FALLING is the attack (§0.4)
        };

        // τ (ms) at `sampleRate` → the one-pole coefficient α = exp(−1/(τ·fs)) — §12 capability 7,
        // verbatim, and the single place the law lives (tests read this instead of a literal).
        // `ms <= 0` (and a non-positive rate) yield exactly 0 → the follower tracks its target in one
        // sample. The guard is load-bearing, not decorative: a NEGATIVE time would give α > 1, i.e. a
        // one-pole that grows without bound, and the release config is built with -Ofast, where
        // leaning on IEEE's exp(−1/0) = 0 for the zero case would not be safe. Control-rate (libm).
        [[nodiscard]] static Real alphaFor (float ms, Real sampleRate) noexcept;

        // Sets the sample rate, re-resolves both coefficients from the current times, and returns the
        // follower to rest. Not RT-safe (libm).
        void prepare (Real sampleRate) noexcept;

        // Returns the follower to exact rest (state 0 — which is silence in `Level` and unity gain in
        // `GainDb`). RT-safe. Does NOT change the parameters (the `AdsrEnvelope` contract).
        void reset() noexcept { state_ = 0.0; }

        // Detection law and smoothing domain. No libm → inline, RT-safe, and switching either mid
        // render is click-free (the state carries over; only its interpretation changes, which is the
        // caller's business).
        void setDetector (Detector d) noexcept { detector_ = d; }
        void setDomain (Domain d) noexcept { domain_ = d; }

        // Attack / release time constants in ms — the τ of `alphaFor`, i.e. the time to reach
        // 63.2 % of a step. Clamped at 0 (0 → instant, single-sample tracking). Settable per block
        // with no click. Control-rate (libm in the .cpp).
        void setAttackMs (float ms) noexcept;
        void setReleaseMs (float ms) noexcept;

        // Parameter read-back (the `Lfo::rateHz()` / `AdsrEnvelope` idiom — for tests, engine
        // introspection and metering). The coefficient accessors are what pin capability 7 without a
        // second copy of the law.
        [[nodiscard]] Detector detector() const noexcept { return detector_; }
        [[nodiscard]] Domain domain() const noexcept { return domain_; }
        [[nodiscard]] float attackMs() const noexcept { return attackMs_; }
        [[nodiscard]] float releaseMs() const noexcept { return releaseMs_; }
        [[nodiscard]] Real attackCoefficient() const noexcept { return attackCoef_; }
        [[nodiscard]] Real releaseCoefficient() const noexcept { return releaseCoef_; }

        // The current envelope, without advancing — a LEVEL in `Level` (rms-rooted when the detector
        // is `Rms`), a dB gain in `GainDb`.
        [[nodiscard]] float value() const noexcept { return static_cast<float> (currentValue()); }

        // |smoother state|, in the smoothing domain and in double — the instrument the §12
        // capability-8 denormal claim is measured with (a subnormal double casts to 0.0f, so
        // `value()` alone cannot see it). Note this is the MEAN-SQUARE under `Detector::Rms`, i.e.
        // `value()` squared; everywhere else it is `|value()|`. The `DelayCore::loopStateMagnitude`
        // precedent.
        [[nodiscard]] Real stateMagnitude() const noexcept { return state_ < 0.0 ? -state_ : state_; }

        // Detects on `detectorInput` — WHICH signal that is, is the caller's decision and the whole
        // point of this class (see the API contract above) — advances the smoother by one sample and
        // returns the envelope INCLUDING this sample. Hot path: inline, no libm CALLS, no allocation,
        // no locks.
        float processSample (float detectorInput) noexcept
        {
            const Real in = static_cast<Real> (detectorInput);

            // 1. Detect (capability 1). A dB gain is already a detected quantity — rectifying or
            //    squaring it would turn a cut into a boost.
            Real target = in;
            if (domain_ == Domain::Level)
                target = detector_ == Detector::Rms ? in * in : (in < 0.0 ? -in : in);

            // 2. Branching (decoupled) attack/release (capability 3). "Attack" is movement AWAY from
            //    rest: a rising level, or a FALLING (more negative) dB gain — §0.4's `gain_dB < g[n−1]`.
            const bool attacking = domain_ == Domain::GainDb ? target < state_ : target > state_;
            const Real alpha = attacking ? attackCoef_ : releaseCoef_;
            state_ = alpha * state_ + (1.0 - alpha) * target;

            // 3. Denormal flush (capability 8): snap a state within the floor to exact rest, so a
            //    decay into silence ARRIVES at 0 instead of walking the subnormal range. The floor is
            //    ~600 dB down (~300 dB in the mean-square domain), far under any audible tail.
            if (state_ < kDenormalFloor && state_ > -kDenormalFloor)
                state_ = 0.0;

            return static_cast<float> (currentValue());
        }

    private:
        // Rest-snap threshold — comfortably above the smallest normal double (~2.2e-308) so the state
        // can never become subnormal, and comfortably below anything audible.
        static constexpr Real kDenormalFloor = 1.0e-30;

        // The smoother state read out in its domain's units. `state_` is a mean-square under
        // `Detector::Rms`, so that is the one path with a root; it can never be negative there
        // (targets are squares, α ∈ [0, 1), rest is 0), which is why there is no guard on the sqrt.
        [[nodiscard]] Real currentValue() const noexcept
        {
            if (domain_ == Domain::Level && detector_ == Detector::Rms)
                return std::sqrt (state_);
            return state_;
        }

        Real sampleRate_ = 48000.0;

        // Parameters (control-rate).
        Detector detector_ = Detector::Peak;
        Domain domain_ = Domain::Level;
        float attackMs_ = 5.0f;
        float releaseMs_ = 50.0f;

        // Control-rate coefficients — α = exp(−1/(τ·fs)), resolved by the setters / prepare().
        Real attackCoef_ = 0.0;
        Real releaseCoef_ = 0.0;

        // The single piece of per-sample state: the smoothed quantity, in the current domain.
        Real state_ = 0.0;
    };
}
