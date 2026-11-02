#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ForkConfig.h>
#include <nemesis/dsp/Oversampler.h>
#include <nemesis/dsp/Waveshaper.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace nemesis::dsp
{
    // [T-CMP-31] The constants named below live in `resonance::ceiling` (FilterResonance.h),
    // not in this file's .cpp — they were lifted so a self-osc threshold law (CAT-4a) can read them.
    // ClassicFilter — the "Classic" model-zoo filter (filter.md §2, CATHODE §3 `synth.filt.model = classic`):
    // the "smooth American" 4-pole OTA cascade (Curtis/CEM-3320, Prophet-5 rev3 / Oberheim Xpander lineage).
    // Clean, round, musical; resonance stays clean nearly to self-oscillation. A NONLINEAR 4-pole
    // zero-delay-feedback cascade with a GENTLE soft-clip at each cell input + a RESONANCE-COMPENSATED
    // feedback so the low end stays full as resonance climbs — the DEFINING contrast with the Ladder/303
    // family (whose settled DC gain droops to 1/(1+k); the Classic's stays ~1). RT-safe, deterministic, no JUCE.
    //
    // Topology: four cascaded TPT/ZDF one-pole low-passes (each y_i = G*c_i + s_i*(1-G),
    // G = tan(pi*fc/(factor*fs))/(1+tan)) inside a global resonance feedback u = comp*x - k*y4, resolved in
    // CLOSED FORM (the linear part is one exact solve, no iteration) — the 4-pole M1-09fg LadderFilter core
    // with two additions: (1) RESONANCE COMPENSATION comp = (1+k) multiplies the loop input, so the linear
    // settled DC gain is 1 for ALL resonance (the Curtis "no bass-loss" signature — filter.md §0.4 "resonance
    // compensation keeps perceived level roughly constant unless the model is known for resonance-loss");
    // (2) a GENTLE soft-clip at each of the four cell inputs (the OTA transconductance nonlinearity —
    // "gentle tanh at the cell inputs", §2). This is the public Zavalishin ("The Art of VA Filter Design") /
    // Huovilainen VA cascade algorithm (same non-GPL lineage as the M1-01 TptSvf / M1-09fg LadderFilter /
    // M1-09fj AcidFilter), so NO new ADR. resonance in [0,1] -> feedback k = ceiling::kClassicMaxK*reso with ceiling::kClassicMaxK just under
    // the 4-pole self-oscillation threshold (k=4), so the LINEAR core rings (bounded — poles inside the unit
    // circle) rather than blowing up, and the clip caps any large-signal loop.
    //
    // Nonlinearity resolution (filter.md §0.2 "one cheap fixed-point iteration"): the AcidFilter/ScreamFilter
    // "clipping-excess-lagged" split is applied PER CELL. Each cell input c_i is soft-clipped
    // shape(c_i) = c_i - psi_i, psi_i = c_i - shape(c_i) the "clipping excess" (0 for small |c_i|), and each
    // psi_i is seeded from the PREVIOUS OS sample's cell input (a sub-OS-sample lag). The four psi's fold
    // known +offsets into the per-stage forcing terms (A_i = -G*psi_i + (1-G)*s_i), so the LINEAR loop
    // (all psi = 0) is solved EXACTLY zero-delay each sample: y4 = G^4*u + S with S = G^3*A1+G^2*A2+G*A3+A4,
    // and u = (comp*in - k*S)/(1 + k*G^4). So the small-signal response is the EXACT linear compensated
    // 4-pole cascade (clean 24 dB/oct LP24, 12 dB/oct LP12, DC gain 1, resonant peak at the set cutoff), and
    // only the large-signal clip is approximated. shape = AdaaSoftClip::shape (reuse, bounded to +-2/3, C1 at
    // 0, f'(0)=1 -> the "gentle tanh"); each cell input clipped to +-2/3 -> the cascade + feedback stay
    // bounded -> the loop self-limits (never blows up) at any setting.
    //
    // Multimode taps (mode-mixing on the cascade node — "multimode tap-outs of the cascade", §2): since the
    // serial cascade gives y_i = LP1^i(u), a summer over u and the taps realises LP24 = y4 (24 dB/oct),
    // LP12 = y2 (12 dB/oct), BP = 2*(y1 - y2) (a 12 dB bandpass, LP1*HP1), HP = u - 2*y1 + y2 = (1-LP1)^2*u
    // (a 12 dB highpass). A `type` SELECTOR chooses which tap the (single-output) oversampler emits (like
    // ScreamFilter's LP12/HP12 selector; the fold-in maps each type to a FilterMode value).
    //
    // OVERSAMPLED ON PURPOSE (analog family, filter.md §0.2/§13 — the MIRROR of the base-rate broken
    // converter whose aliasing IS the effect): the in-loop clip makes harmonics above Nyquist that fold back
    // as inharmonic alias fizz, so the whole nonlinear core runs inside the M1-02b Oversampler (the core-step
    // is the stateful shaper lambda, like AcidFilter/ScreamFilter/FilterDrive). factor 4 default; factor 1 is
    // the naive base-rate baseline for the alias-floor keystone. g = tan(pi*fc/(factor*fs)) is computed at
    // the OS rate, so the analog cutoff is matched across factors.
    //
    // DEFERRED (stable §9/§10 names): the CathodeVoiceStage `synth.filt.model` fold-in as selectable
    // FilterModes (a later card, mirroring M1-09fi/fk — LP24/LP12/BP/HP -> 4 FilterMode values + a mode-aware
    // classicLatencySamples()); §11 sustained self-oscillation + pitch-tracking (the primitive rings bounded,
    // does not sustain); the ENV-follower / LFO / key-track / §4 mod matrix; dual-filter routing (§0.4); a
    // perceptual (dB) drive curve / output makeup; per-cell independent thermal-voltage scaling or a tanh
    // (vs cubic) saturator variant; a 4-pole (24 dB) BP/HP variant (12 dB mode-mix taps for now).
    //
    // RT-safety: prepare() designs the oversampler FIRs and allocates (off the audio thread). setCutoff()
    // resolves tan + the ZDF coefficients at control rate (.cpp). setDrive()/setType() are pure arithmetic.
    // processSample() is inline: the core-step lambda inlines through the Oversampler template (no
    // std::function, zero allocation) and the ZDF solve + clip are pure polynomial arithmetic. No JUCE.
    // Not thread-safe against prepare().
    class ClassicFilter
    {
    public:
        // The four multimode responses (filter.md §2 "Types: LP24 · LP12 · BP · HP").
        enum class Type : std::uint8_t {
            Lp24,
            Lp12,
            Bp,
            Hp
        };

        // Oversampling factor for the nonlinear core (filter.md §13: 2x-4x; 4 gives a comfortable alias
        // floor, matching AcidFilter/ScreamFilter/FilterDrive). 1 == no oversampling (the naive baseline).
        static constexpr int kDefaultOversample = 4;

        // Sets the sample rate, designs the oversampler FIRs for `oversampleFactor`, and clears state.
        // Allocates; call off the audio thread (like prepareToPlay). Call setCutoff() before processing.
        void prepare (Real sampleRate, int oversampleFactor = kDefaultOversample);

        // Clears the 4 integrator states + the 4 feedback estimates + the oversampler FIR state, without
        // touching coefficients. RT-safe.
        void reset() noexcept;

        // Recomputes the ZDF coefficients for a cutoff (Hz) and resonance in [0,1]. cutoffHz is clamped
        // below the OS-rate Nyquist (tan bound); resonance maps to k = ceiling::kClassicMaxK*reso, approaching (but below)
        // the k=4 self-oscillation threshold. Control-rate (tan) — see the .cpp.
        void setCutoff (float cutoffHz, float resonance) noexcept;
        // FIREFORK E4: what setCutoff would compute, taken from a twin that just ran it with the same inputs.
        void copyCoefficientsFrom (const ClassicFilter& o) noexcept { g_ = o.g_; oneMinusG_ = o.oneMinusG_; g2_ = o.g2_; g3_ = o.g3_; g4_ = o.g4_; k_ = o.k_; compGain_ = o.compGain_; invComp_ = o.invComp_; gComp_ = o.gComp_; cachedRes_ = o.cachedRes_; cachedGStable_ = o.cachedGStable_; ++coefGen_; }

        // Drive amount in [0,1] (`synth.filt.drive`): a linear pre-gain that pushes the signal into the
        // in-loop clip (more drive -> more saturation). Uncompensated (output makeup is a later refinement).
        void setDrive (float amount) noexcept
        {
            driveGain_ = 1.0 + kDriveRange * static_cast<Real> (std::clamp (amount, 0.0f, 1.0f));
            boundMix_ = 0.0f;
            ++coefGen_;
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

        // Selects which multimode tap the oversampler emits. Zero-alloc, RT-safe (a hot-path branch).
        void setType (Type t) noexcept
        {
            type_ = t;
            ++coefGen_;
        }

        // Oversampling factor (1 == no oversampling).
        [[nodiscard]] int factor() const noexcept { return os_.factor(); }

        // [T-CMP-37 L2] WHAT THE LANE-WISE FILTER READS FROM A LIVE SCALAR CORE (ClassicFilterLanes.h): the coefficients exactly
        // as `setCutoff` / `setDrive` / `setLoopDrive` / `setType` left them — no tan, no divide re-derived on the other side — and
        // the eight states plus both FIR windows, in and out. The lanes are bit-identical to this class, so a voice may move
        // between the two at any sample and continue exactly.
        struct Coefficients
        {
            Real g, oneMinusG, g2, g3, k, compGain, invComp, gComp, drive;
            float boundMix;
            Type type;
        };
        [[nodiscard]] Coefficients coefficients() const noexcept
        {
            return { g_, oneMinusG_, g2_, g3_, k_, compGain_, invComp_, gComp_, driveGain_, boundMix_, type_ };
        }
        // [T-CMP-37 L3] Counts every call that can change `coefficients()` (setCutoff, setDrive, setLoopDrive, setType): a reader holding a
        // copy taken at generation G need not take another while this still reads G. Wraps; compared for equality only.
        [[nodiscard]] std::uint32_t coefficientsGeneration() const noexcept { return coefGen_; }
        // s1..s4, uEst, y1Est, y2Est, y3Est. STALE while this core is lane-resident (the lane holds the live state): pull it first.
        [[nodiscard]] std::array<Real, 8> state() const noexcept { return { s1_, s2_, s3_, s4_, uEst_, y1Est_, y2Est_, y3Est_ }; }
        void setState (const std::array<Real, 8>& s) noexcept
        {
            s1_ = s[0];
            s2_ = s[1];
            s3_ = s[2];
            s4_ = s[3];
            uEst_ = s[4];
            y1Est_ = s[5];
            y2Est_ = s[6];
            y3Est_ = s[7];
        }
        [[nodiscard]] Oversampler& oversampler() noexcept { return os_; }
        [[nodiscard]] const Oversampler& oversampler() const noexcept { return os_; }

        // Base-rate latency introduced by the oversampler group delay (0 at factor 1).
        [[nodiscard]] int latencySamples() const noexcept { return os_.latencySamples(); }

        // Resonance feedback k of the current coefficients (0 .. < 4). Exposed for tests/inspection.
        [[nodiscard]] Real feedbackK() const noexcept { return k_; }
        // [DM-R2-6] The TPT coefficient G the last `setCutoff` resolved (the stability cap included). For tests.
        [[nodiscard]] Real coefficientG() const noexcept { return g_; }

        // Processes one base-rate sample: the whole nonlinear 4-pole cascade core runs at OS rate inside
        // the oversampler (the lambda inlines through the template, no allocation). Hot path: pure arithmetic.
        float processSample (float x) noexcept
        {
            return os_.processSample (x, [this] (float u) noexcept { return coreStep (u); });
        }

    private:
        // Pre-gain span: drive 1 pushes a unit signal ~7x into the clip. Linear keeps "harmonics grow
        // monotonically with drive" a clean keystone and the header libm-free (matches Acid/Scream/FilterDrive).
        static constexpr Real kDriveRange = 6.0;

        // One OS-rate sample through the nonlinear compensated 4-pole cascade. The linear ZDF loop is solved
        // EXACTLY with the four clipping excesses `psi_i` seeded from the previous OS sample (the cheap
        // iteration). The four cell inputs are u, y1, y2, y3 (the input to each successive one-pole).
        float coreStep (float u) noexcept
        {
            const Real in = LoopDriveBound::apply (static_cast<Real> (u) * driveGain_, boundMix_); // [D-M-0571 S5R] see setLoopDrive

            // Clipping excess at each cell input, from the previous OS sample: psi_i = c_i - shape(c_i)
            // (0 for small |c_i|). Each folds a known +offset into its stage's forcing term.
            const Real psi1 = uEst_ - AdaaSoftClip::shape (uEst_);
            const Real psi2 = y1Est_ - AdaaSoftClip::shape (y1Est_);
            const Real psi3 = y2Est_ - AdaaSoftClip::shape (y2Est_);
            const Real psi4 = y3Est_ - AdaaSoftClip::shape (y3Est_);

            // Per-stage forcing (state + lagged clip offset): A_i = -G*psi_i + (1-G)*s_i.
            const Real a1 = oneMinusG_ * s1_ - g_ * psi1;
            const Real a2 = oneMinusG_ * s2_ - g_ * psi2;
            const Real a3 = oneMinusG_ * s3_ - g_ * psi3;
            const Real a4 = oneMinusG_ * s4_ - g_ * psi4;

            // Zero-delay closed form (compensated 4-pole): y4 = G^4*u + S, with the instantaneous forcing
            // response S = G^3*A1 + G^2*A2 + G*A3 + A4, and the resonance-compensated feedback node
            // u = (comp*in - k*S) * gComp, comp = 1+k, gComp = 1/(1 + k*G^4). The forward pass reproduces y4.
            const Real s = g3_ * a1 + g2_ * a2 + g_ * a3 + a4;
            const Real node = (compGain_ * in - k_ * s) * gComp_;

            // Forward pass through the four one-poles, each fed its SATURATED cell input (c_i - psi_i),
            // updating states (TPT: s := y + v). Reproduces the closed-form y4 (state-consistent).
            const Real c1 = node - psi1;
            const Real v1 = (c1 - s1_) * g_;
            const Real y1 = v1 + s1_;
            s1_ = y1 + v1;

            const Real c2 = y1 - psi2;
            const Real v2 = (c2 - s2_) * g_;
            const Real y2 = v2 + s2_;
            s2_ = y2 + v2;

            const Real c3 = y2 - psi3;
            const Real v3 = (c3 - s3_) * g_;
            const Real y3 = v3 + s3_;
            s3_ = y3 + v3;

            const Real c4 = y3 - psi4;
            const Real v4 = (c4 - s4_) * g_;
            const Real y4 = v4 + s4_;
            s4_ = y4 + v4;

            // Seed next OS sample's clipping excesses from this sample's cell inputs.
            uEst_ = node;
            y1Est_ = y1;
            y2Est_ = y2;
            y3Est_ = y3;

            // Multimode tap select (mode-mixing on the cascade node): LP24=y4, LP12=y2,
            // BP=2(y1-y2) (12 dB bandpass), HP=node-2y1+y2 = (1-LP1)^2 (12 dB highpass). The resonance
            // compensation (comp = 1+k) restores the LP DC gain to 1, but it also boosts the cascade node,
            // so the BP/HP mode-mix taps (which pass that node in their passband) inherit a (1+k) gain —
            // normalise them by invComp = 1/(1+k) so every response's passband sits at ~unity (the LP taps
            // keep the compensation, which IS their no-bass-loss signature). Shape is unaffected.
            Real out = y4;
            switch (type_)
            {
                case Type::Lp24:
                    out = y4;
                    break;
                case Type::Lp12:
                    out = y2;
                    break;
                case Type::Bp:
                    out = 2.0 * (y1 - y2) * invComp_;
                    break;
                case Type::Hp:
                    out = (node - 2.0 * y1 + y2) * invComp_;
                    break;
            }
            return static_cast<float> (out);
        }

        Oversampler os_;
        Type type_ = Type::Lp24;
        float boundMix_ = 0.0f; // [D-M-0571 S5R] setLoopDrive; beside type_, in its padding (eager budget)
        std::uint32_t coefGen_ = 0; // [T-CMP-37 L3] see coefficientsGeneration
        Real driveGain_ = 1.0;

        // Control-rate coefficients (setCutoff).
        Real g_ = 0.0; // TPT one-pole coefficient  G = tan(pi*fc/(factor*fs)) / (1 + tan)
        Real oneMinusG_ = 1.0; // 1 - G
        Real g2_ = 0.0, g3_ = 0.0, g4_ = 0.0; // G^2, G^3, G^4
        Real k_ = 0.0; // resonance feedback (0 .. < 4)
        Real compGain_ = 1.0; // resonance compensation 1 + k (input scale -> DC gain 1 for all reso)
        Real invComp_ = 1.0; // 1 / (1 + k) — passband normaliser for the BP/HP mode-mix taps
        Real cachedRes_ = -1.0, cachedGStable_ = 0.0; // FIREFORK E3b: the resonance-only terms, keyed on the clamped resonance
        Real gComp_ = 1.0; // 1 / (1 + k*G^4) — zero-delay loop divisor, precomputed control-rate

        // States: the four one-pole TPT integrators + the four previous-sample cell-input estimates.
        Real s1_ = 0.0, s2_ = 0.0, s3_ = 0.0, s4_ = 0.0;
        Real uEst_ = 0.0, y1Est_ = 0.0, y2Est_ = 0.0, y3Est_ = 0.0;

        Real sampleRate_ = 48000.0;
    };
}
