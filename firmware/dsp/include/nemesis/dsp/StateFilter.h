#pragma once

#include <nemesis/dsp/Real.h>
#include <algorithm>
#include <cstdint>

namespace nemesis::dsp
{
    // [T-CMP-31] The constants named below live in `resonance::ceiling` (FilterResonance.h),
    // not in this file's .cpp — they were lifted so a self-osc threshold law (CAT-4a) can read them.
    // StateFilter — the "State" model-zoo filter (filter.md §5, State<-Oberheim SEM): a LINEAR 2-pole
    // (12 dB/oct) state-variable MULTIMODE (LP12 · BP12 · HP12 · Notch · AP) with the famous continuous
    // LP<->Notch<->HP MORPH. Smooth, creamy, "never harsh" — the linear counterpart to the driven
    // Soviet/Classic/Reed models. RT-safe, deterministic, no JUCE.
    //
    // Topology: the shared TPT/ZDF state-variable core (Zavalishin, "The Art of VA Filter Design";
    // coefficient form after A. Simper / Cytomic — the same non-GPL lineage as the M1-01 TptSvf and the
    // M1-16e ReedFilter, no new ADR). One frame yields lp = v2, bp = v1, hp = v0 - k*bp - lp, and the two
    // mixed responses notch = lp + hp and ap = notch - k*bp (the standard 2nd-order allpass v0 - 2k*bp,
    // flat magnitude / phase-only). The damping k = 1/Q is the resonance feedback.
    //
    // LINEAR ON PURPOSE (filter.md §5 "no oversampling needed in the linear region", §13 "SEM linear =
    // cheapest, no OS"): unlike Soviet/Classic/Reed there is NO in-loop saturator and NO oversampler —
    // the SEM stays clean and creamy. So processSample is a single base-rate TPT frame, and latency is 0.
    //
    // The SEM morph (§5 "continuous LP->Notch->HP", §11 "the blend is exposed as the Type when set to
    // Morph"): the output is a continuous blend of the lp/hp taps, blend in [0,1] mapping 0 -> HP, 0.5 ->
    // Notch (= lp + hp), 1 -> LP, with weights wLp = clamp(2*blend, 0, 1), wHp = clamp(2*(1-blend), 0, 1).
    // Because the SVF STATE does not depend on the blend, the output is exactly linear in the weights, so
    // Type{Lp,Notch,Hp} == setMorph{1.0,0.5,0.0} BIT-EXACT (same path) and blend 0.25 == mean(blend0,
    // blend0.5) to float precision (triangular continuity) — the same free-keystone structure as Reed.
    //
    // MILD / CLAMPED resonance (filter.md §11 "State is mild/clamped — musical, doesn't fully sing"):
    // resonance in [0,1] maps k from ceiling::kDampMax (well damped, Q = 0.5) down to ceiling::kStateDampMin, which is kept
    // deliberately LARGER than Reed/Soviet's so the max-reso ring is a musical, clearly-DECAYING high-Q
    // resonance rather than a sustained tone. The LINEAR poles stay inside the unit circle for k > 0, so
    // it never blows up and always decays — the "doesn't fully sing" character.
    //
    // DEFERRED (stable §9/§10 names): the CathodeVoiceStage `synth.filt.model = state` fold-in as
    // selectable FilterModes + the morph wire-in (a later card, mirroring M1-16f); §11 TRUE sustained
    // self-oscillation (osc injection); ENV-follower / LFO / key-track / §4 mod matrix; dual-filter
    // routing (e.g. SEM HP into a Moog LP, §0.4).
    //
    // RT-safety: setCutoff() resolves tan + the ZDF coefficients at control rate (.cpp). setType()/
    // setMorph() are pure arithmetic. processSample() is inline: the ZDF solve + tap mix are pure
    // polynomial arithmetic, zero allocation, no libm. No JUCE. Not thread-safe against prepare().
    class StateFilter
    {
    public:
        // The five SEM responses (filter.md §5/§114 "State LP/BP/HP/Notch/AP·12") plus Morph, the
        // continuous LP<->Notch<->HP blend. Morph presets: Lp = blend 1.0, Notch = 0.5, Hp = 0.0.
        enum class Type : std::uint8_t {
            Lp,
            Bp,
            Hp,
            Notch,
            Ap,
            Morph
        };

        // Sets the sample rate and clears the integrator state. Call before use and whenever the sample
        // rate changes. No allocation (linear core, no oversampler). Call setCutoff() before processing.
        void prepare (Real sampleRate) noexcept;

        // Clears the two TPT integrator states (silences the filter) without touching coefficients.
        void reset() noexcept;

        // Recomputes the ZDF coefficients for a cutoff (Hz) and resonance in [0,1]. cutoffHz is clamped
        // below Nyquist (tan bound); resonance maps to the damping k = 1/Q from ceiling::kDampMax (well damped)
        // down to ceiling::kStateDampMin (mild/clamped high-Q ring). Control-rate (tan) — see the .cpp.
        void setCutoff (float cutoffHz, float resonance) noexcept;
        // FIREFORK E4: what setCutoff would compute, taken from a twin that just ran it with the same inputs.
        void copyCoefficientsFrom (const StateFilter& o) noexcept { k_ = o.k_; a1_ = o.a1_; a2_ = o.a2_; a3_ = o.a3_; }

        // Selects the response. Lp/Bp/Hp/Notch/Ap read the matching tap; Morph reads the continuous blend
        // set by setMorph(). Zero-alloc, RT-safe.
        void setType (Type t) noexcept { type_ = t; }

        // The continuous LP<->Notch<->HP morph (§5): blend in [0,1], 0 -> HP, 0.5 -> Notch, 1 -> LP.
        // Only heard when the Type is Morph (§11 "exposed as the Type when set to Morph"). Clamped.
        // Zero-alloc, RT-safe.
        void setMorph (float blend) noexcept
        {
            const Real b = static_cast<Real> (std::clamp (blend, 0.0f, 1.0f));
            wLp_ = std::min (2.0 * b, 1.0); // 0..0.5 -> 0..1, then held at 1
            wHp_ = std::min (2.0 * (1.0 - b), 1.0); // 0.5..1 -> 1..0, held at 1 below 0.5
        }

        // Damping term k = 1/Q of the current coefficients — exposed so callers can verify the TPT
        // reconstruction identity  hp + k*bp + lp == input.
        [[nodiscard]] Real dampingK() const noexcept { return k_; }

        // Processes one input sample, returning the selected response. Hot path: inline, pure arithmetic,
        // no libm, no allocation. The state update is tap-independent (driven by v3 = in - s2), so the
        // same frame yields every response.
        float processSample (float x) noexcept
        {
            const Real v0 = static_cast<Real> (x);

            // Cytomic resolved zero-delay SVF solve.
            const Real v3 = v0 - s2_;
            const Real v1 = a1_ * s1_ + a2_ * v3; // bandpass (bp)
            const Real v2 = s2_ + a2_ * s1_ + a3_ * v3; // lowpass (lp)
            s1_ = 2.0 * v1 - s1_;
            s2_ = 2.0 * v2 - s2_;

            const Real lp = v2;
            const Real bp = v1;
            const Real hp = v0 - k_ * v1 - v2;
            const Real notch = lp + hp; // == v0 - k*bp (SVF notch)
            const Real ap = notch - k_ * bp; // == v0 - 2k*bp (2nd-order allpass)

            Real out = 0.0;
            switch (type_)
            {
                case Type::Lp:
                    out = lp;
                    break;
                case Type::Bp:
                    out = bp;
                    break;
                case Type::Hp:
                    out = hp;
                    break;
                case Type::Notch:
                    out = notch;
                    break;
                case Type::Ap:
                    out = ap;
                    break;
                case Type::Morph:
                    // wLp*lp + wHp*hp: morph 1.0 -> lp (==Lp), 0.0 -> hp (==Hp), 0.5 -> lp+hp (==Notch),
                    // all bit-exact because these multiplies/adds reproduce the discrete-tap expressions.
                    out = wLp_ * lp + wHp_ * hp;
                    break;
            }
            return static_cast<float> (out);
        }

    private:
        Type type_ = Type::Lp;

        // Morph weights (control-rate, setMorph). Default = LP (blend 1.0 -> wLp 1, wHp 0).
        Real wLp_ = 1.0, wHp_ = 0.0;

        // Control-rate coefficients (setCutoff). a1/a2/a3 are the resolved zero-delay Cytomic SVF coeffs.
        Real k_ = 2.0; // damping = 1/Q
        Real a1_ = 1.0, a2_ = 0.0, a3_ = 0.0;

        // The two TPT integrator states.
        Real s1_ = 0.0, s2_ = 0.0;

        Real sampleRate_ = 48000.0;
    };
}
