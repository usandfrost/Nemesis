#pragma once

#include <nemesis/dsp/Real.h>
#include <array>
#include <cmath>
#include <cstdint>

namespace nemesis::dsp
{
    // ModalResonator — the neutral shared §0 modal bank (DROSS §2.1 "spectral DNA"; the
    // resonator half of punch `exciter_ping` §4.3). A small bank of independent 2nd-order
    // modes, each a damped sinusoid {frequency, T60 decay, output gain}; the bank is excited
    // by an input signal (an impulse/pulse from an exciter the CALLER owns) and returns the
    // summed mono output. Struck by an impulse, mode i produces gain·r^n·sin(nω): a decaying
    // sinusoid at its set frequency whose amplitude reaches −60 dB after exactly `decayMs`.
    //
    // Deliberately NEUTRAL: it resonates whatever modes are set. Material recipes, `inharm`
    // stretch, density, per-mode pan, resonator coupling and the junk nonlinearities (DROSS
    // §2.1/§2.4/§3) are DROSS-engine concerns (milestone M11) built ON this primitive — they
    // are NOT baked in here. The exciter (pulse shape / hardness / strike) is likewise the
    // caller's concern; this class is the linear resonator only.
    //
    // Realisation: each mode is a COUPLED-FORM (rotation×decay) resonator — state (u, v)
    // advanced by u' = rc·u − rs·v + x, v' = rs·u + rc·v, output += gain·v, where
    // rc = r·cos ω, rs = r·sin ω. This gives EXACT modal frequency (no bilinear warping),
    // low quantisation noise, and unconditional stability for r < 1 (poles at radius r inside
    // the unit circle). Chosen over a Direct-Form biquad for its robustness at high Q and its
    // frequency-independent output amplitude.
    //
    // RT-safety: prepare()/setMode() resolve every transcendental (cos/sin/pow for the mode
    // coefficients) at CONTROL rate; processSample() is inline and pure arithmetic — no libm
    // calls, no allocation, no locks. Fixed-size storage (kMaxModes), so no heap. Deterministic:
    // no randomness → a given mode set + input renders bit-identically. Computed in double,
    // emitted as float. No JUCE. Not thread-safe vs prepare().
    class ModalResonator
    {
    public:
        // A small bank — enough for a ping / toy / bar / short metallic object. Dense clangs
        // (many close modes) are a DROSS-engine composition, not this lightweight primitive.
        static constexpr int kMaxModes = 8;

        // Sets the sample rate (used only to resolve control-rate coefficients) and clears all
        // state. Leaves the configured modes intact (re-resolving them against the new rate).
        // Not RT-safe — call like prepareToPlay.
        void prepare (Wide sampleRate) noexcept;

        // Clears the resonator state (all modes fall silent) without changing the mode set.
        // RT-safe.
        void reset() noexcept;

        // Number of active modes, clamped to [0, kMaxModes]. Modes at index >= count are not
        // processed. Control-rate.
        void setModeCount (int count) noexcept
        {
            count_ = count < 0 ? 0 : (count > kMaxModes ? kMaxModes : count);
        }

        // Configures mode `i` (ignored if out of range): resonant frequency in Hz (clamped to
        // (0, 0.49·fs]), T60 decay in ms (time to −60 dB, clamped > 0), and linear output gain.
        // Recomputes the rotation/decay coefficients (cos/sin/pow) — control-rate. Does NOT
        // clear the mode's state, so it can be retuned while ringing.
        void setMode (int i, float freqHz, float decayMs, float gain) noexcept;

        // Number of active modes.
        [[nodiscard]] int modeCount() const noexcept { return count_; }

        // Processes one sample: drives the input through every active mode and returns the
        // summed contribution. Hot path — pure arithmetic, no libm / alloc / locks.
        float processSample (float input) noexcept
        {
            const Real x = static_cast<Real> (input);
            Real acc = 0.0;
            for (int i = 0; i < count_; ++i)
            {
                const Real u = u_[static_cast<std::size_t> (i)];
                const Real v = v_[static_cast<std::size_t> (i)];
                const Real un = rc_[static_cast<std::size_t> (i)] * u - rs_[static_cast<std::size_t> (i)] * v + x;
                const Real vn = rs_[static_cast<std::size_t> (i)] * u + rc_[static_cast<std::size_t> (i)] * v;
                u_[static_cast<std::size_t> (i)] = un;
                v_[static_cast<std::size_t> (i)] = vn;
                acc += gain_[static_cast<std::size_t> (i)] * vn;
            }
            return static_cast<float> (acc);
        }

    private:
        // Pole radius is clamped just below 1 so a very long decay can never sit ON the unit
        // circle (marginal stability / unbounded growth).
        static constexpr Real kMaxRadius = 0.999999;

        Wide sampleRate_ = 48000.0;
        int count_ = 0;

        // Per-mode configuration (kept so prepare() can re-resolve against a new sample rate).
        std::array<float, kMaxModes> freqHz_ {};
        std::array<float, kMaxModes> decayMs_ {};

        // Per-mode control-rate coefficients: rc = r·cos ω, rs = r·sin ω, plus the output gain.
        std::array<Real, kMaxModes> rc_ {};
        std::array<Real, kMaxModes> rs_ {};
        std::array<Real, kMaxModes> gain_ {};

        // Per-mode running state (the rotating, decaying phasor).
        std::array<Real, kMaxModes> u_ {};
        std::array<Real, kMaxModes> v_ {};

        // Resolves rc_/rs_ for mode i from freqHz_[i]/decayMs_[i] + sampleRate_ (libm). Control-rate.
        void recomputeMode (int i) noexcept;
    };
}
