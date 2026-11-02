#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/Oversampler.h>
#include <nemesis/dsp/Waveshaper.h>

#include <algorithm>
#include <cstdint>

namespace nemesis::dsp
{
    // [T-CMP-31] The constants named below live in `resonance::ceiling` (FilterResonance.h),
    // not in this file's .cpp — they were lifted so a self-osc threshold law (CAT-4a) can read them.
    // SovietFilter — the "Soviet" model-zoo filter (filter.md §1, Soviet<-Polivoks): a NONLINEAR
    // 2-pole OTA STATE-VARIABLE filter (12 dB/oct; LP/BP/HP) with a HARD, ASYMMETRIC saturator inside
    // the resonance feedback path — raw and biting, aggressive even at low resonance, the resonance
    // turning into screaming distortion; drive pushes the OTA into grit. The signature aggressive
    // UPSTREAM filter. RT-safe, deterministic, no JUCE.
    //
    // Topology: the shared TPT/ZDF state-variable core (Zavalishin, "The Art of VA Filter Design";
    // coefficient form after A. Simper / Cytomic — the same non-GPL lineage as the M1-01 TptSvf and
    // M1-09fh ScreamFilter, no new ADR). One frame yields lp = v2, bp = v1, hp = v0 - k*bp - lp. The
    // damping k = 1/Q is the resonance feedback; resonance in [0,1] maps k from ceiling::kDampMax (well damped)
    // down to ceiling::kSovietDampMin just above 0 (near self-oscillation but the LINEAR poles stay inside the unit
    // circle -> stable), and the in-loop clip then caps a max-reso ring (bounded, never blows up).
    //
    // The OTA aggression: the resonance feedback is SATURATED before it is subtracted, i.e. the
    // high-pass node is hp = v0 - k*saturate(bp) - lp with an ASYMMETRIC saturator (distinct from the
    // symmetric AdaaSoftClip used by Scream/Acid) — so the OTA injects EVEN harmonics + a DC term (a
    // symmetric clip makes only odd), which is the characteristic Polivoks bite.
    //
    // Nonlinearity resolution (filter.md §0 "one cheap fixed-point iteration where a nonlinearity sits
    // in the loop", the M1-09fh idiom): split saturate(bp) = bp - psi, psi = bp - saturate(bp) the
    // "clipping excess" (0 for small signals). The LINEAR loop (psi = 0) is solved EXACTLY zero-delay
    // each sample — the forcing term v3 = v0 - ic2eq simply gains a known +k*psi correction, so the
    // solve is the plain TPT SVF with v3 -> v3 + k*psi — and only psi is seeded from the previous OS
    // sample's bandpass (a sub-OS-sample lag). So the small-signal response is the EXACT linear OTA-SVF
    // (clean 12 dB/oct, resonant peak at the set cutoff) and only the large-signal clipping is
    // approximated. saturate is built on AdaaSoftClip::shape (bounded to +-2/3, C1 at 0, f'(0)=1) so
    // the injected feedback is bounded -> the states stay bounded for any setting.
    //
    // OVERSAMPLED ON PURPOSE (analog family, filter.md §0/§13 — the MIRROR of the base-rate broken
    // converter whose aliasing IS the effect): the in-loop clip makes harmonics above Nyquist that fold
    // back as inharmonic fizz, so the whole nonlinear core runs inside the M1-02b Oversampler (the
    // core-step is the stateful shaper lambda, like FilterDrive/ScreamFilter). factor 4 default; factor
    // 1 is the naive base-rate baseline for the alias-floor keystone. g = tan(pi*fc/(factor*fs)) is
    // computed at the OS rate, so the analog cutoff is matched across factors.
    //
    // DEFERRED (stable §9/§10 names): the CathodeVoiceStage `synth.filt.model` fold-in as a selectable
    // FilterMode (a later card, mirroring M1-09fi/M1-09fk); §11 sustained self-oscillation +
    // pitch-tracking / "playable as an oscillator" (osc injection — the primitive rings bounded at high
    // reso, does not sustain); the ENV-follower / LFO / key-track / §4 mod matrix; dual-filter routing;
    // a perceptual (dB) drive curve, output makeup/comp, a truly hard (square/tanh) saturator variant.
    //
    // RT-safety: prepare() designs the oversampler FIRs and allocates (off the audio thread). setCutoff()
    // resolves tan + the ZDF coefficients at control rate (.cpp). setDrive()/setType() are pure arithmetic.
    // processSample() is inline: the core-step lambda inlines through the Oversampler template (no
    // std::function, zero allocation) and the ZDF solve + clip are pure polynomial arithmetic. No JUCE.
    // Not thread-safe against prepare().
    class SovietFilter
    {
    public:
        // The three Polivoks responses (filter.md §1 "Types: LP · BP · HP", 2-pole 12 dB/oct).
        enum class Type : std::uint8_t {
            Lp,
            Bp,
            Hp
        };

        // Oversampling factor for the nonlinear core (filter.md §13: 2x-4x; 4 gives a comfortable alias
        // floor, matching ScreamFilter/FilterDrive). 1 == no oversampling (the naive alias baseline).
        static constexpr int kDefaultOversample = 4;

        // Sets the sample rate, designs the oversampler FIRs for `oversampleFactor`, and clears state.
        // Allocates; call off the audio thread (like prepareToPlay). Call setCutoff() before processing.
        void prepare (Real sampleRate, int oversampleFactor = kDefaultOversample);

        // Clears the 2 integrator states + the feedback estimate + the oversampler FIR state, without
        // touching coefficients. RT-safe.
        void reset() noexcept;

        // Recomputes the ZDF coefficients for a cutoff (Hz) and resonance in [0,1]. cutoffHz is clamped
        // below the OS-rate Nyquist (tan bound); resonance maps to the damping k = 1/Q from ceiling::kDampMax
        // (well damped) down to ceiling::kSovietDampMin (near self-oscillation). Control-rate (tan) — see the .cpp.
        void setCutoff (float cutoffHz, float resonance) noexcept;
        // FIREFORK E4: what setCutoff would compute, taken from a twin that just ran it with the same inputs.
        void copyCoefficientsFrom (const SovietFilter& o) noexcept { k_ = o.k_; a1_ = o.a1_; a2_ = o.a2_; a3_ = o.a3_; }

        // Drive amount in [0,1] (`synth.filt.drive`): a linear pre-gain that pushes the signal into the
        // in-loop clip (more drive -> more grit). Uncompensated (output makeup is a later refinement).
        void setDrive (float amount) noexcept
        {
            driveGain_ = 1.0 + kDriveRange * static_cast<Real> (std::clamp (amount, 0.0f, 1.0f));
            boundMix_ = 0.0f;
        }

        // [D-M-0571 S5R] The CathodeFilterBank's IN-LOOP drive (`synth.filt.drive` at Pre): the same pre-gain, but
        // the driven input is bounded by the house soft clip (AdaaSoftClip::shape, +-2/3) before it enters the
        // loop — as the Ladder's per-stage saturators bound its driven signal — so the drive saturates instead of
        // running the core up to 7x its input. The bound FADES IN over the first quarter of the drive (LoopDriveBound),
        // so no drive step jumps. Amount 0 is setDrive(0) exactly. setDrive clears the bound.
        void setLoopDrive (float amount) noexcept
        {
            setDrive (amount);
            boundMix_ = LoopDriveBound::mixFor (driveGain_);
        }
        // The pre-gain the drive resolved to (1 at drive 0), for the bank's ceiling reference.
        [[nodiscard]] Real driveGain() const noexcept { return driveGain_; }

        // Selects the LP/BP/HP response. Zero-alloc, RT-safe (a hot-path branch).
        void setType (Type t) noexcept { type_ = t; }

        // Oversampling factor (1 == no oversampling).
        [[nodiscard]] int factor() const noexcept { return os_.factor(); }

        // Base-rate latency introduced by the oversampler group delay (0 at factor 1).
        [[nodiscard]] int latencySamples() const noexcept { return os_.latencySamples(); }

        // Damping term k = 1/Q of the current coefficients. Exposed for tests/inspection.
        [[nodiscard]] Real dampingK() const noexcept { return k_; }

        // The ASYMMETRIC OTA saturator, exposed for tests/reuse. Built on the symmetric cubic soft clip
        // (AdaaSoftClip::shape, bounded +-2/3) with a DIFFERENT knee per polarity, so it is C1 at 0 with
        // f'(0) = 1 (small-signal identity -> psi -> 0) yet asymmetric (saturate(a)+saturate(-a) != 0),
        // injecting even harmonics. Positive knee 1.0, negative knee kNegKnee (< 1 -> clips harder).
        static Real saturate (Real x) noexcept
        {
            constexpr Real kNegKnee = 0.6;
            return x >= 0.0 ? AdaaSoftClip::shape (x)
                            : kNegKnee * AdaaSoftClip::shape (fork::kApprox ? x * (1.0 / kNegKnee) : x / kNegKnee); // FIREFORK N2
        }

        // Processes one base-rate sample: the whole nonlinear OTA state-variable core runs at OS rate
        // inside the oversampler (the lambda inlines through the template). Hot path: pure arithmetic.
        float processSample (float x) noexcept
        {
            return os_.processSample (x, [this] (float u) noexcept { return coreStep (u); });
        }

    private:
        // Pre-gain span: drive 1 pushes a unit signal ~7x into the clip. Linear keeps "harmonics grow
        // monotonically with drive" a clean keystone and the header libm-free (matches ScreamFilter).
        static constexpr Real kDriveRange = 6.0;

        // One OS-rate sample through the nonlinear OTA state-variable filter. The linear ZDF loop is
        // solved EXACTLY with the clipping excess `psi` seeded from the previous OS sample.
        float coreStep (float u) noexcept
        {
            const Real in = LoopDriveBound::apply (static_cast<Real> (u) * driveGain_, boundMix_); // [D-M-0571 S5R] see setLoopDrive

            // Clipping excess of the previous bandpass: psi = bp - saturate(bp) (0 for small |bp|).
            const Real psi = bpEst_ - saturate (bpEst_);

            // Solve the TPT SVF with the saturated feedback. hp = in - k*(bp - psi) - lp folds the +k*psi
            // correction into the forcing term: bp = a1*s1 + a2*(v3 + k*psi), v3 = in - s2 (the linear
            // Cytomic solve with v3 shifted). psi is a known term (previous OS sample) -> no iteration.
            const Real v3 = in - s2_;
            const Real w = v3 + k_ * psi;
            const Real v1 = a1_ * s1_ + a2_ * w; // bandpass (bp)
            const Real v2 = s2_ + a2_ * s1_ + a3_ * w; // lowpass (lp)

            // Update the TPT integrator states (s := 2*x - s) and store the feedback estimate.
            s1_ = 2.0 * v1 - s1_;
            s2_ = 2.0 * v2 - s2_;
            bpEst_ = v1;

            // hp = in - k*saturate(bp) - lp, self-consistent with the loop (saturate(bp) ~ v1 - psi).
            const Real hp = in - k_ * (v1 - psi) - v2;
            const Real out = (type_ == Type::Lp) ? v2 : (type_ == Type::Bp) ? v1
                                                                              : hp;
            return static_cast<float> (out);
        }

        Oversampler os_;
        Type type_ = Type::Lp;
        float boundMix_ = 0.0f; // [D-M-0571 S5R] setLoopDrive; beside type_, in its padding (eager budget)
        Real driveGain_ = 1.0;

        // Control-rate coefficients (setCutoff). a1/a2/a3 are the resolved zero-delay Cytomic SVF coeffs.
        Real k_ = 2.0; // damping = 1/Q
        Real a1_ = 1.0, a2_ = 0.0, a3_ = 0.0;

        // States: the two TPT integrators + the previous-OS-sample bandpass estimate (for psi).
        Real s1_ = 0.0, s2_ = 0.0, bpEst_ = 0.0;

        Real sampleRate_ = 48000.0;
    };
}
