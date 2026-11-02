#pragma once

#include <nemesis/dsp/Real.h>
#include <cstdint>

namespace nemesis::dsp
{
    // AdsrEnvelope — the shared voice-engine DAHDSR envelope generator (CATHODE §4 "Envelopes ×3,
    // one hard-wired to AMP"; §7 AMP env → drive/level → pan). A leading Delay stage, then Attack ·
    // Hold · Decay · Sustain · Release, gate-driven (gateOn at note-on, gateOff at note-off),
    // producing a 0..1 control value per sample. It is a pure GENERATOR: processSample() takes no
    // audio input and returns the envelope value; where it is applied (env · voice) and any velocity
    // scaling are voice/composition concerns (M1-10b), not this primitive. `synth.amp.{a,d,s,r}` is
    // this envelope with delay 0 AND hold 0.
    //
    // HOLD (T-SPEC-02a, the family DAHDSR convention — docs/dev/dsp-family-conventions.md §3): a flat
    // plateau held at the Attack peak (1.0) for hold·fs samples before Decay begins. **hold = 0 (the
    // default) is a BIT-EXACT no-op** — Attack hands straight to Decay exactly as the prior DADSR did,
    // so every committed golden + M1-10a keystone is byte-identical (no re-mint). PRISM/AETHER's
    // DAHDSR amp pages consume the Hold stage; the rest leave it at 0.
    //
    // Curve (analog character):
    //   * Attack  — EXPONENTIAL (analog RC) charge toward a target ABOVE the peak (kAttackTarget),
    //     stopped at 1.0: it decelerates into the peak, arriving at (T − 1)·ln(T/(T − 1)) ≈ 0.44 of a
    //     linear ramp's slope (T-CMP-30: a ramp that stops dead is a corner, audible at any length).
    //     A small slope step at the peak remains — measured, not claimed gone. The stage boundary is
    //     exact: a fresh note reaches 1.0 at sample ceil(attack·fs). A retrigger charges on from the
    //     current level, so the VALUE never steps (the slope does, as any retrigger's).
    //   * Hold    — held flat at 1.0 for hold·fs samples (hold 0 → skipped, bit-exact). Re-armed on
    //     each Attack peak, so a retrigger that re-reaches 1.0 gets a fresh hold.
    //   * Decay   — EXPONENTIAL (analog RC) approach toward the sustain level; it reaches −60 dB of
    //     its (1 − sustain) span at decay·fs samples, then snaps to sustain (a −120 dB epsilon,
    //     inaudible) and holds.
    //   * Sustain — held flat at the sustain level while gated.
    //   * Release — EXPONENTIAL (analog RC) decay of whatever level gateOff fired at, toward 0;
    //     it reaches −60 dB of that start level at release·fs samples, and the envelope frees
    //     itself (→ Idle, active() false, zero output) once it drops below a low floor.
    //
    // The DADSR family is realised from first principles (the §12.1 clean-path cites DaisySP `Adsr`
    // / STK `ADSR` only as MIT/permissive references), so it is license-clean — NO ADR.
    //
    // RT-safety (§11): the constructor, prepare() and the time setters resolve the control-rate
    // coefficients (the only libm — pow in the .cpp: up to three from the constructor or prepare(),
    // at most one per setter); processSample() is inline and pure arithmetic — no libm CALLS, no
    // allocation, no locks.
    // Deterministic: no RNG, so a given (params + gate sequence) renders bit-identically; reset()
    // returns it to Idle. Computed in double, emitted as float. No JUCE. Not thread-safe vs prepare().
    class AdsrEnvelope
    {
    public:
        // Resolves the coefficients for the default times at 48 kHz, so they always equal a full
        // recompute of the current params — the invariant the per-coefficient setters rely on.
        AdsrEnvelope() noexcept { updateCoefficients(); }

        // Sets the sample rate (used only to resolve control-rate coefficients), recomputes the
        // stage coefficients from the current times, and returns the envelope to Idle. Not RT-safe.
        void prepare (Real sampleRate) noexcept;

        // Returns the envelope to Idle (output 0). RT-safe. Does not change the parameters.
        void reset() noexcept
        {
            stage_ = Stage::Idle;
            level_ = 0.0;
            delayCounter_ = 0;
            holdCounter_ = 0;
        }

        // Pre-attack delay in ms (DAHDSR delay stage; 0 → no delay). Clamped ≥ 0. Control-rate.
        void setDelayMs (float ms) noexcept;

        // Attack time in ms (0 → effectively instant). Clamped ≥ 0. Control-rate.
        void setAttackMs (float ms) noexcept;

        // Peak-hold time in ms (DAHDSR hold stage; 0 → BIT-EXACT no-op, Attack→Decay directly).
        // Clamped ≥ 0. Control-rate.
        void setHoldMs (float ms) noexcept;

        // Decay time in ms — the −60 dB time of the (1 − sustain) span (0 → instant). Clamped ≥ 0.
        // Control-rate.
        void setDecayMs (float ms) noexcept;

        // Sustain level in [0, 1] (`synth.amp.s`). No libm → inline. Control-rate.
        void setSustain (float level) noexcept { sustain_ = clamp01 (level); }

        // Release time in ms — the −60 dB time from the gateOff level (0 → instant). Clamped ≥ 0.
        // Control-rate.
        void setReleaseMs (float ms) noexcept;

        // Parameter read-back (the Lfo::rateHz() bit-exact-pin idiom — trivial, for tests/engine
        // introspection; the §6 spread fan-out pins the applied per-voice times against these).
        [[nodiscard]] float attackMs() const noexcept { return attackMs_; }
        [[nodiscard]] float holdMs() const noexcept { return holdMs_; }
        [[nodiscard]] float decayMs() const noexcept { return decayMs_; }
        [[nodiscard]] float releaseMs() const noexcept { return releaseMs_; }
        [[nodiscard]] float sustain() const noexcept { return static_cast<float> (sustain_); }

        // The resolved control-rate coefficients, for tests: a setter recomputes only its own, and
        // [T-CGUI-81] pins every one bit-equal to what a full recompute (prepare) would produce.
        struct Coefficients
        {
            int delaySamples, holdSamples, dipSamples;
            Real attackK, decayCoef, releaseCoef;
        };
        [[nodiscard]] Coefficients coefficients() const noexcept
        {
            return { delaySamples_, holdSamples_, dipSamples_, attackK_, decayCoef_, releaseCoef_ };
        }

        // Note-on: (re)start the envelope. From a fresh note it rises from 0; a retrigger continues
        // from the current level (click-free). With a delay set, it holds the current level for the
        // delay, then attacks. Control-rate.
        void gateOn() noexcept
        {
            if (delaySamples_ > 0)
            {
                stage_ = Stage::Delay;
                delayCounter_ = delaySamples_;
            }
            else
            {
                stage_ = Stage::Attack;
            }
        }

        // [T-CGUI-40, R11/R63] A RE-STRIKE FROM ZERO — the Mono-RT retrigger. It ramps LINEARLY from
        // the current level to silence over 2 ms, then runs exactly what `gateOn` would (Delay or
        // Attack). Owner 2026-09-24: a literal jump to 0 is a step (a click on every retrigger); the
        // dip is the shortest fall that is not. From Idle there is nothing to dip: it is `gateOn`.
        // A separate method because `gateOn`'s continue-from-level law is what five engines rely on.
        void restrike() noexcept
        {
            if (stage_ == Stage::Idle || level_ <= 0.0)
            {
                level_ = 0.0;
                gateOn();
                return;
            }
            stage_ = Stage::Dip;
            dipCounter_ = dipSamples_;
            dipDec_ = level_ / static_cast<Real> (dipSamples_);
        }

        // Note-off: enter the Release stage from the current level (from any active stage). No-op
        // when already Idle. Control-rate.
        void gateOff() noexcept
        {
            if (stage_ != Stage::Idle)
                stage_ = Stage::Release;
        }

        // True while the envelope is running (any stage but Idle). For tests / voice bookkeeping.
        [[nodiscard]] bool active() const noexcept { return stage_ != Stage::Idle; }
        // [T-CGUI-69 leg 2] True during a restrike's dip — its falling edge is the moment the note is
        // silent and about to speak, which is when per-note state may jump without a click.
        [[nodiscard]] bool dipping() const noexcept { return stage_ == Stage::Dip; }

        // The current envelope value (without advancing). For tests / metering.
        [[nodiscard]] float value() const noexcept { return static_cast<float> (level_); }

        // Advances one sample and returns the envelope value for THIS sample (the value is read
        // first, then the state advances — as PunchTransient). Hot path: pure arithmetic, no libm
        // calls / allocation / locks.
        float processSample() noexcept
        {
            const Real out = level_;
            switch (stage_)
            {
                case Stage::Idle:
                    return 0.0f;

                case Stage::Dip:
                    // [T-CGUI-40] The restrike's linear fall to silence; at 0, what gateOn would do.
                    level_ -= dipDec_;
                    if (--dipCounter_ <= 0 || level_ <= 0.0)
                    {
                        level_ = 0.0;
                        gateOn();
                    }
                    break;

                case Stage::Delay:
                    // Hold the current level; count the delay down, then attack.
                    if (--delayCounter_ <= 0)
                        stage_ = Stage::Attack;
                    break;

                case Stage::Attack:
                    // RC charge toward kAttackTarget; at the peak (1.0, within a rounding epsilon so
                    // the sample is the designed one), enter Hold if armed, else Decay directly.
                    // hold 0 → holdSamples_ 0 → straight to Decay.
                    level_ += (kAttackTarget - level_) * attackK_;
                    if (level_ >= 1.0 - kPeakEps)
                    {
                        level_ = 1.0;
                        if (holdSamples_ > 0)
                        {
                            stage_ = Stage::Hold;
                            holdCounter_ = holdSamples_;
                        }
                        else
                        {
                            stage_ = Stage::Decay;
                        }
                    }
                    break;

                case Stage::Hold:
                    // Flat plateau at the peak; count down, then Decay.
                    level_ = 1.0;
                    if (--holdCounter_ <= 0)
                        stage_ = Stage::Decay;
                    break;

                case Stage::Decay:
                {
                    // Exponential (RC) approach toward sustain; snap + hold once within epsilon.
                    Real gap = level_ - sustain_;
                    gap *= decayCoef_;
                    level_ = sustain_ + gap;
                    if (gap <= kSnapEps && gap >= -kSnapEps)
                    {
                        level_ = sustain_;
                        stage_ = Stage::Sustain;
                    }
                    break;
                }

                case Stage::Sustain:
                    level_ = sustain_; // held flat
                    break;

                case Stage::Release:
                    // Exponential (RC) decay toward 0; free the envelope at the floor.
                    level_ *= releaseCoef_;
                    if (level_ <= kEndFloor)
                    {
                        level_ = 0.0;
                        stage_ = Stage::Idle;
                    }
                    break;
            }

            return static_cast<float> (out);
        }

    private:
        enum class Stage : std::uint8_t { Idle,
            Delay,
            Attack,
            Hold,
            Decay,
            Sustain,
            Release,
            Dip };

        // Decay→Sustain snap threshold (−120 dB of full scale — inaudible; avoids the asymptote
        // never quite reaching sustain) and the Release→Idle deactivation floor. The floor sits an
        // order of magnitude below the −60 dB keystone point so the release keystone stays
        // measurable in-stage before the envelope frees itself.
        static constexpr Real kSnapEps = 1.0e-6;
        // [T-CMP-30] The attack's RC target: above the peak so the charge crosses 1.0 in finite time,
        // near it so the arrival slope is small. 1.3 is the analog-emulation convention (a 0.3 target
        // ratio). kPeakEps absorbs the recursion's rounding so the peak lands on ceil(attack·fs).
        static constexpr Real kAttackTarget = 1.3;
        static constexpr Real kPeakEps = 1.0e-9;
        static constexpr Real kRestrikeDipMs = 2.0; // [T-CGUI-40] owner 2026-09-24: a 1-2 ms dip
        static constexpr Real kEndFloor = 1.0e-5;

        static float clamp01 (float v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

        void updateCoefficients() noexcept; // all of them, from times + fs (prepare / construction)
        // [T-CGUI-81] One coefficient each — a setter pays one pow at most, never three.
        void updateDelay() noexcept;
        void updateHold() noexcept;
        void updateAttack() noexcept;
        void updateDecay() noexcept;
        void updateRelease() noexcept;

        Real sampleRate_ = 48000.0;

        // Parameters (control-rate). Sustain kept in double so a snapped Sustain output is exact.
        float delayMs_ = 0.0f;
        float attackMs_ = 5.0f;
        float holdMs_ = 0.0f; // DAHDSR hold (default 0 → bit-exact no-op)
        float decayMs_ = 50.0f;
        float releaseMs_ = 50.0f;
        Real sustain_ = 0.7;

        // Control-rate coefficients (all by updateCoefficients(); each setter by its own update*()).
        int delaySamples_ = 0;
        int holdSamples_ = 0; // 0 → Attack hands straight to Decay (bit-exact)
        int dipSamples_ = 96; // [T-CGUI-40] the restrike's 2 ms fall (≥ 1)
        Real attackK_ = 0.0; // [T-CMP-30] per-sample RC charge fraction, 1 − (1 − 1/T)^(1/attack-samples)
        Real decayCoef_ = 0.0; // per-sample gap multiplier (10^(-3/decay-samples))
        Real releaseCoef_ = 0.0; // per-sample level multiplier (10^(-3/release-samples))

        // Per-sample state.
        Stage stage_ = Stage::Idle;
        Real level_ = 0.0; // current envelope value
        int delayCounter_ = 0;
        int holdCounter_ = 0;
        int dipCounter_ = 0;
        Real dipDec_ = 0.0; // per-sample fall of the current dip
    };
}
