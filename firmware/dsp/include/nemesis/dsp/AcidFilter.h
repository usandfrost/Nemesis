#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/Oversampler.h>
#include <nemesis/dsp/Waveshaper.h>

#include <algorithm>

namespace nemesis::dsp
{
    // [T-CMP-31] The constants named below live in `resonance::ceiling` (FilterResonance.h),
    // not in this file's .cpp — they were lifted so a self-osc threshold law (CAT-4a) can read them.
    // AcidFilter — the "Acid" model-zoo filter (filter.md §6, CATHODE §13.1 `synth.filt.model += acid`):
    // a NONLINEAR 3-pole (18 dB/oct) zero-delay-feedback DIODE ladder (Roland TB-303 style) with a soft
    // clipper + drive in the resonance feedback loop — the clipper IS the squelch. LP-ONLY (the 303 is a
    // low-pass, ~18 dB/oct). The 303 bass-loss (the low end thins as resonance climbs — settled DC gain
    // = 1/(1+k)) is PRESERVED as character, not compensated. RT-safe, deterministic, no JUCE.
    //
    // Topology: three cascaded TPT/ZDF one-pole low-passes (each y_i = G*u_i + s_i*(1-G),
    // G = tan(pi*fc/fs')/(1+tan)) inside a global resonance feedback u = drive*x - k*phi(y3), resolved in
    // CLOSED FORM (the linear part is one exact solve, no iteration) — the 3-pole analog of the M1-09fg
    // LadderFilter. This is the public Zavalishin ("The Art of VA Filter Design") / Huovilainen-Valimaki
    // diode-ladder algorithm (same non-GPL lineage as the M1-01 TptSvf / M1-09fg LadderFilter — NOT a
    // port of the Open303 MIT engine the spec cites), so NO new ADR. resonance in [0,1] -> feedback
    // k = ceiling::kAcidMaxK*reso with ceiling::kAcidMaxK below the 3-pole self-oscillation threshold (k=8, where three one-poles
    // each hit -60 deg -> -180 deg with loop gain k*(1/2)^3 = 1), so the LINEAR core rings (bounded)
    // rather than blowing up, and the clip caps any large-signal loop.
    //
    // Nonlinearity resolution (filter.md §0.2 "one cheap fixed-point iteration"): the feedback saturator
    // phi(y3) = softclip(y3) is split as phi(y3) = y3 - psi, psi = y3 - softclip(y3) the "clipping excess"
    // (0 for small signals). The LINEAR loop (psi = 0) is solved EXACTLY zero-delay each sample; only psi
    // is seeded from the previous OS sample's y3 (a sub-OS-sample lag). So the small-signal response is the
    // EXACT linear 3-pole ladder (clean 18 dB/oct, resonant peak at the set cutoff, DC = 1/(1+k)), and only
    // the large-signal clipping is approximated. softclip = AdaaSoftClip::shape (reuse, bounded to +-2/3)
    // -> the injected feedback is bounded -> the ladder states stay bounded for any setting.
    //
    // OVERSAMPLED ON PURPOSE (analog family, filter.md §0.2/§13 — the MIRROR of the base-rate broken
    // converter whose aliasing IS the effect): the in-loop clipper makes harmonics above Nyquist that fold
    // back as inharmonic alias fizz, so the whole nonlinear core runs inside the M1-02b Oversampler (the
    // core-step is the stateful shaper lambda, like ScreamFilter/FilterDrive). factor 4 default; factor 1
    // is the naive base-rate baseline for the alias-floor keystone. g = tan(pi*fc/(factor*fs)) is computed
    // at the OS rate, so the analog cutoff is matched across factors.
    //
    // DEFERRED (stable §9/§10 names): the CathodeVoiceStage `synth.filt.model` fold-in as a selectable
    // FilterMode (a later card, mirroring M1-09fi); an asymmetric/tube diode variant (uses the signed-off
    // symmetric AdaaSoftClip now); the ENV-follower / accent / slide interplay + key-track (§4 mod matrix);
    // a perceptual (dB) drive curve / output makeup; self-oscillation pitch-tracking.
    //
    // RT-safety: prepare() designs the oversampler FIRs and allocates (off the audio thread). setCutoff()
    // resolves tan + the ZDF coefficients at control rate (.cpp). setDrive() is pure arithmetic.
    // processSample() is inline: the core-step lambda inlines through the Oversampler template (no
    // std::function, zero allocation) and the ZDF solve + clip are pure polynomial arithmetic. No JUCE.
    // Not thread-safe against prepare().
    class AcidFilter
    {
    public:
        // Oversampling factor for the nonlinear core (filter.md §13: 2x-4x; 4 gives a comfortable alias
        // floor, matching ScreamFilter/FilterDrive). 1 == no oversampling (the naive alias baseline).
        static constexpr int kDefaultOversample = 4;

        // Sets the sample rate, designs the oversampler FIRs for `oversampleFactor`, and clears state.
        // Allocates; call off the audio thread (like prepareToPlay). Call setCutoff() before processing.
        void prepare (Real sampleRate, int oversampleFactor = kDefaultOversample);

        // Clears the 3 integrator states + the feedback estimate + the oversampler FIR state, without
        // touching coefficients. RT-safe.
        void reset() noexcept;

        // Recomputes the ZDF coefficients for a cutoff (Hz) and resonance in [0,1]. cutoffHz is clamped
        // below the OS-rate Nyquist (tan bound); resonance maps to k = ceiling::kAcidMaxK*reso, approaching (but below)
        // the k=8 self-oscillation threshold. Control-rate (tan) — see the .cpp.
        void setCutoff (float cutoffHz, float resonance) noexcept;
        // FIREFORK E4: what setCutoff would compute, taken from a twin that just ran it with the same inputs.
        void copyCoefficientsFrom (const AcidFilter& o) noexcept { g_ = o.g_; oneMinusG_ = o.oneMinusG_; g2_ = o.g2_; g3_ = o.g3_; k_ = o.k_; gComp_ = o.gComp_; }

        // Drive amount in [0,1] (`synth.filt.drive`): a linear pre-gain that pushes the signal into the
        // in-loop clipper (more drive -> more squelch). Uncompensated (output makeup is a later refinement).
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

        // Oversampling factor (1 == no oversampling).
        [[nodiscard]] int factor() const noexcept { return os_.factor(); }

        // Base-rate latency introduced by the oversampler group delay (0 at factor 1).
        [[nodiscard]] int latencySamples() const noexcept { return os_.latencySamples(); }

        // Resonance feedback k of the current coefficients (0 .. < 8). Exposed for tests/inspection.
        [[nodiscard]] Real feedbackK() const noexcept { return k_; }

        // Processes one base-rate sample: the whole nonlinear 3-pole diode-ladder core runs at OS rate
        // inside the oversampler (the lambda inlines through the template, no allocation). LP-only output.
        float processSample (float x) noexcept
        {
            return os_.processSample (x, [this] (float u) noexcept { return coreStep (u); });
        }

    private:
        // Pre-gain span: drive 1 pushes a unit signal ~7x into the clipper. Linear keeps "harmonics grow
        // monotonically with drive" a clean keystone and the header libm-free (matches Scream/FilterDrive).
        static constexpr Real kDriveRange = 6.0;

        // One OS-rate sample through the nonlinear 3-pole diode ladder. The linear ZDF loop is solved
        // EXACTLY with the clipping excess `psi` seeded from the previous OS sample (the cheap iteration).
        float coreStep (float u) noexcept
        {
            const Real in = LoopDriveBound::apply (static_cast<Real> (u) * driveGain_, boundMix_); // [D-M-0571 S5R] see setLoopDrive

            // Clipping excess of the previous feedback output: psi = y3 - softclip(y3) (0 for small |y3|).
            // The feedback is  k*softclip(y3) = k*(y3 - psi), so the loop input gains a known +k*psi term.
            const Real psi = y3Est_ - AdaaSoftClip::shape (y3Est_);
            const Real inEff = in + k_ * psi;

            // Zero-delay closed form (3-pole analog of the LadderFilter): with S the instantaneous state
            // response,  y3 = (G^3*inEff + S) * gComp,  gComp = 1/(1 + k*G^3),  and the feedback node
            // u = inEff - k*y3. The forward pass below reproduces y3 (state-consistent).
            const Real s = oneMinusG_ * (g2_ * s1_ + g_ * s2_ + s3_);
            const Real y3 = (g3_ * inEff + s) * gComp_;
            const Real node = inEff - k_ * y3;

            // Forward pass through the three one-poles, updating states (TPT: s_new = 2v + s).
            const Real v1 = (node - s1_) * g_;
            const Real y1 = v1 + s1_;
            s1_ = y1 + v1;

            const Real v2 = (y1 - s2_) * g_;
            const Real y2 = v2 + s2_;
            s2_ = y2 + v2;

            const Real v3 = (y2 - s3_) * g_;
            const Real y3b = v3 + s3_; // == y3 (the closed form), now state-consistent
            s3_ = y3b + v3;

            y3Est_ = y3b;
            return static_cast<float> (y3b); // LP18 (the 303 is LP-only, ~18 dB/oct)
        }

        Oversampler os_;
        Real driveGain_ = 1.0;
        float boundMix_ = 0.0f; // [D-M-0571 S5R] setLoopDrive

        // Control-rate coefficients (setCutoff).
        Real g_ = 0.0; // TPT one-pole coefficient  G = tan(pi*fc/(factor*fs)) / (1 + tan)
        Real oneMinusG_ = 1.0; // 1 - G
        Real g2_ = 0.0, g3_ = 0.0; // G^2, G^3
        Real k_ = 0.0; // resonance feedback (0 .. < 8)
        Real gComp_ = 1.0; // 1 / (1 + k*G^3) — zero-delay loop divisor, precomputed control-rate

        // States: the three one-pole TPT integrators + the previous-sample feedback estimate.
        Real s1_ = 0.0, s2_ = 0.0, s3_ = 0.0, y3Est_ = 0.0;

        Real sampleRate_ = 48000.0;
    };
}
