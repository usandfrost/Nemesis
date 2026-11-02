#pragma once

#include <nemesis/dsp/WavetableBank.h>

#include <vector>

namespace nemesis::dsp
{
    // The ADDITIVE frame kernel shared by the factory categories that are stated in the frequency
    // domain (T-WT-03; `family_overview.md` §2.3 + ADR-027). VOCAL, METALLIC and HARMONIC are all
    // "a set of harmonic amplitudes, morphed across 32 frames" — the categories differ in the
    // amplitude LAW, not in how a frame is built from one — so the sum lives here and each
    // generator ships only its law.
    //
    // WRITTEN ONCE FOR THREE CATEGORIES RATHER THAN EXTRACTED LATER: the alternative was three
    // copies of the same loop, which is where a DC convention or a normalisation factor drifts
    // apart between banks that are then compared against each other by the distinctness tests.
    // (CHIP is deliberately NOT a consumer — its laws are time-domain by nature, and forcing a
    // pulse through a harmonic sum would either band-limit away the thing that makes it a chip
    // wave or need hundreds of harmonics to fake a corner.)
    //
    // ZERO MEAN IS BY CONSTRUCTION, NOT BY CORRECTION. Every harmonic k ≥ 1 has exactly zero mean
    // over one full cycle at any phase, so a sum of them does too — for ANY amplitude law a
    // category invents. That is why this kernel takes harmonics and not an arbitrary shape: a DC
    // offset in an oscillator frame survives the mip band-limiter (which zeroes bins ABOVE the top
    // harmonic and never touches bin 0) and thumps on every note.
    //
    // Build-time only: allocates, runs libm, never called at render time.
    class HarmonicFrame
    {
    public:
        static constexpr int kFrameLength = WavetableBank::kFrameLength;

        // Nyquist for a 2048-sample frame is harmonic 1024. The cap is well below it because the
        // factory laws are stated over the audible first ~64 harmonics of a 110 Hz reference and
        // the mip pyramid band-limits what remains; a law needing more than this is a law that
        // wants a longer frame.
        static constexpr int kMaxHarmonics = 256;

        // Renders `out[i] = Σ_k amps[k-1] · sin(2π·k·i/N + phases[k-1])` over one cycle, then
        // PEAK-NORMALISES to unity (§2.3 / ADR-006's per-frame normalize). `phases` may be empty,
        // meaning every harmonic starts at zero phase. Amplitudes past `kMaxHarmonics` are ignored.
        //
        // The normalisation is what makes a bank's frames comparable to each other: a morph that
        // also changed loudness would read as "brighter" to the ear whatever its spectrum did.
        static void renderAdditive (const std::vector<double>& amps,
            const std::vector<double>& phases,
            float* out);

        // DEPOSIT `gain` AT A FRACTIONAL HARMONIC POSITION, split across the two neighbouring
        // bins in proportion (T-WT-13; ADR-041). Written once, here, because five kernels need it
        // — `stretch`, `ringmod`, `modal`, `chord` and anything else placing a partial that is not
        // an integer multiple of the fundamental — and five copies of it is five chances to round
        // differently.
        //
        // ROUNDING TO THE NEAREST BIN IS THE DEFECT THIS EXISTS TO AVOID, and it is a real one the
        // reference implementation records: a ratio that moves smoothly across a bank lands on the
        // SAME integer bin for many consecutive frames, so the bank stops morphing and reads as a
        // run of duplicate frames — which the T-WT-12 validator fails, correctly, at more than
        // frames/3. Splitting keeps the motion continuous and is also the more honest statement of
        // a non-integer partial: a single-cycle table cannot hold one, so it holds the two
        // neighbours that interpolate to it.
        //
        // A position outside [1, kMaxHarmonics] is DROPPED rather than clamped or reflected. That
        // matters for `ringmod`, whose difference partials legitimately run off the bottom of the
        // series: clamping would pile them onto harmonic 1 and invent a fundamental the law does
        // not have.
        static void addBin (std::vector<double>& amps, double position, double gain);

        // A RESONANCE bump in harmonic space: the gain a formant at `centreHz` with width
        // `widthOctaves` applies to a harmonic at `freqHz`. A Gaussian in LOG frequency, so a
        // formant keeps its shape as it sweeps — a linear-frequency bump would be narrow low down
        // and vague up top, which is the opposite of how a resonance behaves.
        [[nodiscard]] static double resonance (double freqHz, double centreHz, double widthOctaves) noexcept;

        // The reference fundamental the factory formant/chord geometries are written at. A
        // wavetable frame has no pitch of its own — harmonic k is k× whatever the oscillator plays
        // — so a law expressed in Hz needs a stated reference, and a table played an octave up
        // carries its formants up with it. That is the accepted behaviour of a wavetable vowel;
        // pitch-independent formants are what CATHODE §13's `formant` FILTER model is for.
        static constexpr double kReferenceHz = 110.0; // A2
    };
} // namespace nemesis::dsp
