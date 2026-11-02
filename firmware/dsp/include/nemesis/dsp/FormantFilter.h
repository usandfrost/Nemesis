#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/Svf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace nemesis::dsp
{
    // FormantFilter — CATHODE's §3 "Formant mode" / the FLTR §9 "Vowel" model: a small bank of
    // parallel BAND-PASS resonators tuned to the vowel formant frequencies, summed to one mono
    // output, with a single VOWEL-MORPH position sweeping A-E-I-O-U and a global FORMANT Q. A
    // "talk-box" colour on any source (filter.md §9).
    //
    // Pure COMPOSITION of the already-signed-off TptSvf (M1-01) in band-pass mode: for each of the
    // kNumFormants formants, one TptSvf is set to the (morph-interpolated) formant frequency at the
    // shared Q, and their band-pass outputs are summed with fixed per-formant gains. The genuinely-
    // new surface is only the vowel formant table, the morph interpolation, the per-formant gain
    // sum, and the wiring — the band-pass DSP itself is already golden-tested. LINEAR: NO
    // oversampling (filter.md §0.2/§9 — the linear models need none; only the in-loop-saturation
    // models are oversampled).
    //
    // There is NO bit-exact "bypass" (a formant filter always colours the signal), so — like the
    // grit stage (M1-09) — the correctness anchor is the BIT-EXACT composition: the bank ==
    // Σ formantGain(i) · standaloneTptSvf_i(BP) with identical operands. `formantHz(i)` and
    // `formantGain(i)` expose the exact operands so a test can reconstruct that sum bit-for-bit.
    //
    // Vowel morph: `setVowel(p)`, p in [0,1], places the timbre along the five vowels anchored at
    // {0, 0.25, 0.5, 0.75, 1.0} = {A, E, I, O, U}; each formant centre frequency is LINEARLY
    // interpolated between the two bracketing vowels (filter.md §9 "cutoff = vowel-morph position").
    // `setFormantQ(q)` is the shared formant sharpness (filter.md §9 "reso = formant Q").
    //
    // DEFERRED (stable names): the FOLD-IN into CathodeVoiceStage as a selectable filter MODE (a
    // later card, mirroring FilterDrive→M1-09fb); the optional "Voice" (male/female/child) formant-
    // set shift (§9); per-formant published BANDWIDTHS / perceptual gain calibration (a global Q +
    // fixed per-formant gains here — the golden guards the chosen values); a 4th/5th formant; the
    // COMB mode (a separate fractional-delay-feedback primitive); the other filter extras; §4 mod
    // matrix wire-in of the vowel position. Vowel does NOT self-oscillate (filter.md §11).
    //
    // RT-safety: prepare() sets the sample rate and re-resolves coefficients (calls into TptSvf,
    // off the audio thread like prepareToPlay). setVowel()/setFormantQ() resolve the coefficients at
    // CONTROL rate — the morph interpolation is pure arithmetic and the tan() lives inside TptSvf's
    // .cpp, so this class needs no .cpp of its own. processSample() is inline: kNumFormants band-pass
    // frames plus a gain-weighted sum in double. No allocation, no locks, no RNG → bit-identical
    // re-render. No JUCE. Not thread-safe against prepare().
    class FormantFilter
    {
    public:
        // Three formants (F1/F2/F3): the vowel-identity pair plus one for naturalness (filter.md §9
        // "3-5 parallel band-pass resonators"). A lightweight bank; a 4th/5th formant is a later add.
        static constexpr int kNumFormants = 3;

        // The five morph vowels A-E-I-O-U (filter.md §9).
        static constexpr int kNumVowels = 5;

        // Sets the sample rate (used by the band-passes), clears state, and re-resolves the current
        // vowel/Q coefficients against the new rate. Not RT-safe — call like prepareToPlay.
        void prepare (Real sampleRate) noexcept
        {
            for (auto& svf : svf_)
                svf.prepare (sampleRate);
            updateCoeffs();
        }

        // Clears every band-pass integrator (silences the bank) without changing the vowel/Q. RT-safe.
        void reset() noexcept
        {
            for (auto& svf : svf_)
                svf.reset();
        }

        // Vowel-morph position in [0,1]: 0 = A, 0.25 = E, 0.5 = I, 0.75 = O, 1 = U; each formant
        // frequency is linearly interpolated between the two bracketing vowels. Control-rate.
        void setVowel (float position) noexcept
        {
            const Real p = static_cast<Real> (std::clamp (position, 0.0f, 1.0f));
            const Real scaled = p * static_cast<Real> (kNumVowels - 1);
            int lo = static_cast<int> (std::floor (scaled));
            if (lo > kNumVowels - 2)
                lo = kNumVowels - 2; // p == 1 → interpolate O→U with frac 1
            const Real frac = scaled - static_cast<Real> (lo);

            const auto loIdx = static_cast<std::size_t> (lo);
            const auto& a = kVowelHz[loIdx];
            const auto& b = kVowelHz[loIdx + 1];
            for (int i = 0; i < kNumFormants; ++i)
            {
                const Real fi = static_cast<Real> (a[static_cast<std::size_t> (i)])
                                  + frac
                                        * (static_cast<Real> (b[static_cast<std::size_t> (i)])
                                            - static_cast<Real> (a[static_cast<std::size_t> (i)]));
                freqHz_[static_cast<std::size_t> (i)] = static_cast<float> (fi);
            }
            updateCoeffs();
        }

        // Shared formant Q (`reso`): higher → narrower, taller formant peaks. Clamped to a small
        // positive minimum so the band-pass damping term stays finite. Control-rate.
        void setFormantQ (float q) noexcept
        {
            q_ = std::max (q, kMinQ);
            updateCoeffs();
        }

        // The current (morph-interpolated) centre frequency of formant `i` in Hz — the exact operand
        // fed to the i-th band-pass. Out-of-range → 0.
        [[nodiscard]] float formantHz (int i) const noexcept
        {
            return (i >= 0 && i < kNumFormants) ? freqHz_[static_cast<std::size_t> (i)] : 0.0f;
        }

        // The fixed linear output gain of formant `i` (F1 loudest; higher formants tapered). Out-of-
        // range → 0. Exposed so a test can reconstruct the summed output bit-for-bit.
        [[nodiscard]] static constexpr float formantGain (int i) noexcept
        {
            return (i >= 0 && i < kNumFormants) ? kGain[static_cast<std::size_t> (i)] : 0.0f;
        }

        // Processes one sample: sums the gain-weighted band-pass outputs of every formant. Hot path —
        // pure arithmetic (the band-pass frames are libm-free), no allocation.
        float processSample (float x) noexcept
        {
            Real acc = 0.0;
            for (int i = 0; i < kNumFormants; ++i)
                acc += static_cast<Real> (kGain[static_cast<std::size_t> (i)])
                       * static_cast<Real> (svf_[static_cast<std::size_t> (i)].processSample (x).bp);
            return static_cast<float> (acc);
        }

    private:
        // Average-male vowel formant frequencies (Hz), F1/F2/F3 — public data (Peterson-Barney
        // lineage; filter.md §12.1 "vowel data is public"). Rows A, E, I, O, U.
        static constexpr std::array<std::array<float, kNumFormants>, kNumVowels> kVowelHz { {
            { { 730.0f, 1090.0f, 2440.0f } }, // A
            { { 530.0f, 1840.0f, 2480.0f } }, // E
            { { 270.0f, 2290.0f, 3010.0f } }, // I
            { { 570.0f, 840.0f, 2410.0f } }, // O
            { { 300.0f, 870.0f, 2240.0f } }, // U
        } };

        // Fixed per-formant output gains: F1 at unity, F2/F3 tapered (~ -4 / -8 dB). A perceptual
        // per-vowel calibration is a later refinement (non-goal); the golden guards these values.
        static constexpr std::array<float, kNumFormants> kGain { 1.0f, 0.63f, 0.4f };

        static constexpr float kMinQ = 0.05f;

        // Re-resolves each band-pass to its current formant frequency at the shared Q. Control-rate;
        // the tan() lives inside TptSvf::setCutoff.
        void updateCoeffs() noexcept
        {
            for (int i = 0; i < kNumFormants; ++i)
                svf_[static_cast<std::size_t> (i)].setCutoff (freqHz_[static_cast<std::size_t> (i)], q_);
        }

        std::array<TptSvf, kNumFormants> svf_ {};
        std::array<float, kNumFormants> freqHz_ { kVowelHz[0][0], kVowelHz[0][1], kVowelHz[0][2] }; // default A
        float q_ = 6.0f;
    };
}
