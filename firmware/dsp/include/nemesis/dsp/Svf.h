#include <nemesis/dsp/Real.h>
#pragma once

namespace nemesis::dsp
{
    // Linear topology-preserving-transform (TPT / zero-delay-feedback) state-variable
    // filter core — the shared §0 primitive every FLTR model builds on, and the
    // "State"/SEM model (filter.md §5) in its linear region. Trapezoidal integrators
    // give exact cutoff under fast modulation; resonance is resolved zero-delay. One
    // frame yields LP/BP/HP/Notch simultaneously, so the model is swappable at runtime
    // without clicks. RT-safe, deterministic, no JUCE, no allocation.
    //
    // Topology: Zavalishin, "The Art of VA Filter Design"; coefficient form after
    // A. Simper (Cytomic), "Solving the continuous SVF equations using trapezoidal
    // integration". Nonlinear/oversampled models, drive, modulation and dual-filter
    // routing are separate cards (see filter.md §1-9); this is the linear core only.
    class TptSvf
    {
    public:
        struct Outputs
        {
            float lp = 0.0f;
            float bp = 0.0f;
            float hp = 0.0f;
            float notch = 0.0f;
        };

        // Sets the sample rate and clears the integrator state. Call before use and
        // whenever the sample rate changes.
        void prepare (Real sampleRate) noexcept;

        // Clears the integrator state (silences the filter) without touching coeffs.
        void reset() noexcept;

        // Recomputes the coefficients for a cutoff (Hz) and resonance (Q). cutoffHz is
        // clamped below Nyquist; q is clamped to a small positive minimum so the
        // damping term k = 1/q stays finite. Allocation-free.
        void setCutoff (float cutoffHz, float q) noexcept;
        // FIREFORK E4: what setCutoff would compute, taken from a twin that just ran it with the same inputs.
        void copyCoefficientsFrom (const TptSvf& o) noexcept { g_ = o.g_; k_ = o.k_; a1_ = o.a1_; a2_ = o.a2_; a3_ = o.a3_; }

        // Damping term k = 1/q used by the current coefficients — exposed so callers
        // can verify the TPT reconstruction identity  hp + k*bp + lp == input.
        Real dampingK() const noexcept { return k_; }

        // Processes one input sample, returning all four responses of one TPT frame.
        // Hot path: kept inline (pure arithmetic, no libm) so it inlines into callers.
        Outputs processSample (float x) noexcept
        {
            const Real v0 = static_cast<Real> (x);
            const Real v3 = v0 - ic2eq_;
            const Real v1 = a1_ * ic1eq_ + a2_ * v3;
            const Real v2 = ic2eq_ + a2_ * ic1eq_ + a3_ * v3;
            ic1eq_ = 2.0 * v1 - ic1eq_;
            ic2eq_ = 2.0 * v2 - ic2eq_;

            const Real lp = v2;
            const Real bp = v1;
            const Real hp = v0 - k_ * v1 - v2;
            const Real notch = v0 - k_ * v1; // == hp + lp

            return { static_cast<float> (lp),
                static_cast<float> (bp),
                static_cast<float> (hp),
                static_cast<float> (notch) };
        }

    private:
        Real sampleRate_ = 48000.0;
        Real g_ = 0.0; // tan(pi * fc / fs)
        Real k_ = 1.0; // 1 / q (damping)
        Real a1_ = 0.0, a2_ = 0.0, a3_ = 0.0; // resolved zero-delay coefficients
        Real ic1eq_ = 0.0, ic2eq_ = 0.0; // integrator states
    };
}
