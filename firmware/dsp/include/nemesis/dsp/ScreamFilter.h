#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ForkConfig.h>
#include <nemesis/dsp/Oversampler.h>
#include <nemesis/dsp/Waveshaper.h>

#include <algorithm>
#include <cstdint>

namespace nemesis::dsp
{
    // [T-CMP-31] The constants named below live in `resonance::ceiling` (FilterResonance.h),
    // not in this file's .cpp — they were lifted so a self-osc threshold law (CAT-4a) can read them.
    // ScreamFilter — the "Scream" model-zoo filter (filter.md §4, CATHODE §13.1 `synth.filt.model += sk`):
    // a NONLINEAR 2-pole zero-delay-feedback Sallen-Key (Korg MS-20 style) with a soft diode clipper in
    // the resonance feedback loop — clean until resonance/drive climb, then aggressive/distorting, and
    // self-limiting into a bounded ring at max resonance ("controlled chaos"). Two authentic MS-20 modes
    // are selectable: LP12 and HP12 (a `type` selector, unlike the Ladder's simultaneous taps). RT-safe,
    // deterministic, no JUCE.
    //
    // Topology: the 2-pole Sallen-Key transfer function H(s) = K*w0^2 / (s^2 + (3-K)*w0*s + w0^2), with
    // Q = 1/(3-K), realised from two TPT/ZDF one-pole integrators over the two capacitor-voltage states
    // (VC1, VC2). This is the public Zavalishin ("The Art of VA Filter Design") / D'Angelo-Valimaki
    // Sallen-Key algorithm — the same non-GPL lineage as the M1-01 TptSvf and M1-09fg LadderFilter (no
    // new ADR). resonance in [0,1] -> K = 1 + ceiling::kScreamMaxR*reso with ceiling::kScreamMaxR just under 2, so K approaches the
    // K=3 (Q->inf) self-oscillation threshold but stays under it (poles inside the unit circle -> the
    // LINEAR core is stable), and the diode clip in the feedback caps the loop so even a max-reso strike
    // self-limits (bounded), never blows up.
    //
    // Nonlinearity resolution (filter.md §0.2 "one cheap fixed-point iteration where a nonlinearity sits
    // in the loop"): the feedback saturator is split as phi(VC2) = VC2 - psi, psi = VC2 - softclip(VC2)
    // the "clipping excess" (0 for small signals). The LINEAR part (psi = 0) is solved EXACTLY zero-delay
    // each sample; only psi is seeded from the previous OS sample's VC2 (a sub-OS-sample lag). So the
    // small-signal response is the EXACT linear Sallen-Key (clean 12 dB/oct, resonant peak at the set
    // cutoff), and only the large-signal clipping is approximated. softclip = AdaaSoftClip::shape (reuse,
    // bounded to +-2/3) -> the injected feedback is bounded -> VC1/VC2 stay bounded for any setting.
    //
    // OVERSAMPLED ON PURPOSE (analog family, filter.md §0.2/§13 — the MIRROR of the base-rate broken
    // converter whose aliasing IS the effect): the in-loop clipper makes harmonics above Nyquist that
    // fold back as inharmonic alias fizz, so the whole nonlinear core runs inside the M1-02b Oversampler
    // (the core-step is the stateful shaper lambda, like FilterDrive). factor 4 default; factor 1 is the
    // naive base-rate baseline for the alias-floor keystone. g = tan(pi*fc/(factor*fs)) is computed at
    // the OS rate, so the analog cutoff is matched across factors.
    //
    // DEFERRED (stable §9/§10 names): dual-filter routing (series/parallel/stereo); the ENV-follower /
    // LFO / key-track modulation sources; self-oscillation PITCH-tracking + "playable as an oscillator"
    // (§11); a perceptual (dB) drive curve, asymmetric/tube diode variants, output makeup/comp; the
    // CathodeVoiceStage `synth.filt.model` fold-in (a later card, mirroring FormantFilter->M1-09fd).
    //
    // RT-safety: prepare() designs the oversampler FIRs and allocates (off the audio thread). setCutoff()
    // resolves tan + the ZDF coefficients at control rate (.cpp). setDrive()/setType() are pure arithmetic.
    // processSample() is inline: the core-step lambda inlines through the Oversampler template (no
    // std::function, zero allocation) and the ZDF solve + clip are pure polynomial arithmetic. No JUCE.
    // Not thread-safe against prepare().
    class ScreamFilter
    {
    public:
        // The two MS-20 modes (filter.md §4 "Types: LP12 · HP12").
        enum class Type : std::uint8_t {
            Lp12,
            Hp12
        };

        // Oversampling factor for the nonlinear core (filter.md §13: 2x-4x; 4 gives a comfortable alias
        // floor, matching FilterDrive/PD). 1 == no oversampling (the naive alias baseline).
        static constexpr int kDefaultOversample = 4;

        // Sets the sample rate, designs the oversampler FIRs for `oversampleFactor`, and clears state.
        // Allocates; call off the audio thread (like prepareToPlay). Call setCutoff() before processing.
        void prepare (Real sampleRate, int oversampleFactor = kDefaultOversample);

        // Clears the 2 integrator states + the feedback estimate + the oversampler FIR state, without
        // touching coefficients. RT-safe.
        void reset() noexcept;

        // Recomputes the ZDF coefficients for a cutoff (Hz) and resonance in [0,1]. cutoffHz is clamped
        // below the OS-rate Nyquist (tan bound); resonance maps to K = 1 + ceiling::kScreamMaxR*reso, approaching the
        // K=3 self-oscillation threshold. Control-rate (tan) — see the .cpp.
        void setCutoff (float cutoffHz, float resonance) noexcept;
        // FIREFORK E4: what setCutoff would compute, taken from a twin that just ran it with the same inputs.
        void copyCoefficientsFrom (const ScreamFilter& o) noexcept { g_ = o.g_; r_ = o.r_; gr_ = o.gr_; twoGR_ = o.twoGR_; oneMinusGR_ = o.oneMinusGR_; gOnePlus2r_ = o.gOnePlus2r_; onePlus2g_ = o.onePlus2g_; invDet_ = o.invDet_; damp_ = o.damp_; cachedR_ = o.cachedR_; cachedGLimit_ = o.cachedGLimit_; }

        // Drive amount in [0,1] (`synth.filt.drive`): a linear pre-gain that pushes the signal into the
        // in-loop clipper (more drive -> more scream). Uncompensated (output makeup is a later refinement).
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

        // Selects the LP12 or HP12 response. Zero-alloc, RT-safe (a hot-path branch).
        void setType (Type t) noexcept { type_ = t; }

        // Oversampling factor (1 == no oversampling).
        [[nodiscard]] int factor() const noexcept { return os_.factor(); }

        // Base-rate latency introduced by the oversampler group delay (0 at factor 1).
        [[nodiscard]] int latencySamples() const noexcept { return os_.latencySamples(); }

        // Resonance feedback gain K (1 .. ~3) of the current coefficients. Exposed for tests/inspection.
        [[nodiscard]] Real resonanceK() const noexcept { return 1.0 + r_; }
        // [DM-R2-6] The TPT coefficient g the last `setCutoff` resolved (the stability cap included). For tests.
        [[nodiscard]] Real coefficientG() const noexcept { return g_; }

        // Processes one base-rate sample: the whole nonlinear Sallen-Key core runs at OS rate inside the
        // oversampler (the lambda inlines through the template, no allocation). Hot path: pure arithmetic.
        float processSample (float x) noexcept
        {
            return os_.processSample (x, [this] (float u) noexcept { return coreStep (u); });
        }

    private:
        // Pre-gain span: drive 1 pushes a unit signal ~7x into the clipper. Linear keeps "harmonics grow
        // monotonically with drive" a clean keystone and the header libm-free (matches FilterDrive).
        static constexpr Real kDriveRange = 6.0;

        // One OS-rate sample through the nonlinear Sallen-Key. The linear ZDF loop is solved EXACTLY with
        // the clipping excess `psi` seeded from the previous OS sample (the cheap fixed-point iteration).
        float coreStep (float u) noexcept
        {
            const Real in = LoopDriveBound::apply (static_cast<Real> (u) * driveGain_, boundMix_); // [D-M-0571 S5R] see setLoopDrive

            // Clipping excess of the previous output: psi = VC2 - softclip(VC2) (0 for small |VC2|).
            const Real psi = vc2Est_ - static_cast<Real> (AdaaSoftClip::shape (vc2Est_));

            // Solve the 2x2 ZDF system (VC1, VC2) with psi as a known forcing term. Derived from the two
            // TPT integrators VC1 = g*f1 + s1, VC2 = g*f2 + s2 with the Sallen-Key node relations
            // f1 = in - 2*VC1 - (1+2r)*VC2 + 2r*psi, f2 = VC1 + r*VC2 - r*psi; det = (1+g)^2 - g*r.
            const Real b1 = g_ * in + twoGR_ * psi + s1_;
            const Real b2 = -gr_ * psi + s2_;
            const Real vc1 = (b1 * oneMinusGR_ - gOnePlus2r_ * b2) * invDet_;
            const Real vc2 = (onePlus2g_ * b2 + g_ * b1) * invDet_;
            const Real f2 = vc1 + r_ * vc2 - r_ * psi; // the "bandpass" node (s/w0)*VC2

            // Update the TPT integrator states (s := 2*x - s) and store the feedback estimate.
            s1_ = 2.0 * vc1 - s1_;
            s2_ = 2.0 * vc2 - s2_;
            vc2Est_ = vc2;

            // LP12 = VC2 (DC gain 1); HP12 = in - (3-K)*f2 - VC2 (DC 0, HF 1).
            const Real out = (type_ == Type::Lp12) ? vc2 : (in - damp_ * f2 - vc2);
            return static_cast<float> (out);
        }

        Oversampler os_;
        Type type_ = Type::Lp12;
        float boundMix_ = 0.0f; // [D-M-0571 S5R] setLoopDrive; beside type_, in its padding (eager budget)
        Real driveGain_ = 1.0;

        // Control-rate coefficients (setCutoff).
        Real g_ = 0.0; // TPT integrator coeff, g = tan(pi*fc/(factor*fs))
        Real r_ = 0.0; // resonance, r = K - 1 in [0, ~2)
        Real cachedR_ = -1.0, cachedGLimit_ = 0.0; // FIREFORK E3c
        Real gr_ = 0.0; // g*r
        Real twoGR_ = 0.0; // 2*g*r
        Real oneMinusGR_ = 1.0; // 1 - g*r
        Real gOnePlus2r_ = 0.0; // g*(1 + 2r)
        Real onePlus2g_ = 1.0; // 1 + 2g
        Real invDet_ = 1.0; // 1 / ((1+g)^2 - g*r)
        Real damp_ = 2.0; // 3 - K = 2 - r (HP tap)

        // States: the two capacitor-voltage TPT integrators + the previous-sample feedback estimate.
        Real s1_ = 0.0, s2_ = 0.0, vc2Est_ = 0.0;

        Real sampleRate_ = 48000.0;
    };
}
