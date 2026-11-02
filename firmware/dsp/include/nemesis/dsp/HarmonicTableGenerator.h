#pragma once

#include <nemesis/dsp/HarmonicFrame.h>
#include <nemesis/dsp/WavetableBank.h>
#include <nemesis/dsp/WavetableRecipe.h>

#include <array>
#include <cstdint>
#include <vector>

namespace nemesis::dsp
{
    // The HARMONIC-category factory wavetable generator (T-WT-03; `family_overview.md` §2.3 +
    // ADR-027). Three banks — **Harm · Organ · Chord** — synthesised from pure code, because
    // ADR-027 rules shipped factory content SELF-GENERATED.
    //
    // WHAT MAKES THE CATEGORY ONE CATEGORY, MEASURABLY. Every partial any law here writes is a
    // 5-LIMIT ratio to the frame's fundamental — an index of the form 2^a · 3^b · 5^c, i.e. a
    // musical interval (octave, fifth, third) rather than an arbitrary harmonic. That is the
    // property METALLIC lacks: Bell's mode set contains 11 and 14, Metal's comb passes every
    // harmonic including 7 and 11, and a partial at 11× the fundamental beats against the stack
    // instead of fusing with it. So "HARMONIC" is a claim about WHICH harmonics carry energy, it is
    // checked in both directions against METALLIC (the neighbour §2.3 puts below it), and the CHIP
    // boundary above belongs to that leaf from its own side.
    //
    // THE THREE LAWS, and each uses a DIFFERENT morph mechanism on purpose — three banks that all
    // brighten by tilting one exponent would be one bank at three settings:
    //   * Harm  — the powers of two, {1, 2, 4, 8, 16, 32, 64}: an octave stack. The SET NEVER
    //             MOVES and the BALANCE does — mode j is scaled by tilt^j with tilt opening
    //             0.5 → 1.0, so the bank travels from a 6-dB-per-octave taper to a flat stack and
    //             the centroid climbs 3.5 → 18.1. (Contrast METALLIC's Additive, where the balance
    //             is fixed and the SET grows: same audible direction, opposite mechanism.)
    //   * Organ — the drawbar set {1, 2, 3, 4, 6, 8, 12, 16}, the octave ranks 16′/8′/4′/2′/1′ plus
    //             the QUINT ranks at 3, 6 and 12. The morph pulls the quints out: they fade in from
    //             0.05 to full while the octave balance stays exactly where it is, so the quint
    //             energy share climbs 0.0008 → 0.24. That is what makes it an organ rather than a
    //             brighter Harm — the quints are the ranks Harm structurally cannot have.
    //   * Chord — a just-intoned TRIAD in one frame: root 20 · third 24 → 25 · fifth 30 · octave 40,
    //             so 24/20 = 6/5 is a minor third and 25/20 = 5/4 is a major one. The morph is the
    //             THIRD, crossfading MINOR → MAJOR at equal power (the third's own energy is
    //             constant across the bank; a linear crossfade would dip 3 dB at the midpoint,
    //             which is audible as the chord's colour going hollow halfway through).
    //
    // WHAT A CHORD TABLE COSTS, STATED RATHER THAN DISCOVERED. Chord's lowest partial is harmonic
    // 20, so its implied fundamental — the frame's own period — sits **log2(20) ≈ 4.3 octaves
    // BELOW the chord's root**. Played at C1 the triad sounds around E5. Both halves of that are
    // deliberate: an integer partial set is the only kind a single-cycle frame can hold (see the
    // rounding note in `MetallicTableGenerator`), and 20 is the smallest base at which a just minor
    // third (6/5) and a just major third (5/4) are BOTH exact integers — 24 and 25. A smaller base
    // would need a rounded third, which is the one interval a chord cannot afford to detune.
    //
    // Frame cap 32, §2.3's factory band (16–64), the same owner pick the other categories took.
    // Deterministic — no RNG, no clock, no file input. Build-time only: allocates and runs libm,
    // never called at render time.
    class HarmonicTableGenerator
    {
    public:
        // §2.3's within-category order. The ordinal IS the catalogue's within-category slot.
        enum class Bank : std::uint8_t {
            Harm = 0,
            Organ = 1,
            Chord = 2,
            // [T-WT-14 leaf 5] the 37 more the roster names — the first three ordinals are
            // UNCHANGED.
            OrganJazz = 3,
            OrganGospel = 4,
            OrganTibia = 5,
            OrganCeleste = 6,
            Pipe = 7,
            OddOnly = 8,
            EvenOnly = 9,
            OddEvenSweep = 10,
            Hollow = 11,
            OddBright = 12,
            EvenSoft = 13,
            Comb = 14,
            Notch = 15,
            NotchDown = 16,
            NotchNarrow = 17,
            Phaserish = 18,
            CombTight = 19,
            Major = 20,
            Minor = 21,
            Fifths = 22,
            Sus = 23,
            Seventh = 24,
            Octaves = 25,
            StretchUp = 26,
            StretchDown = 27,
            StretchWide = 28,
            StretchFine = 29,
            StretchDense = 30,
            TiltDark = 31,
            TiltBright = 32,
            TiltFull = 33,
            Shelf = 34,
            ShelfDown = 35,
            Walsh = 36,
            WalshWide = 37,
            WalshDown = 38,
            WalshTight = 39
        };

        static constexpr int kBankCount = 40;
        static constexpr int kFrameLength = WavetableBank::kFrameLength;
        static constexpr int kFrameCount = 32;

        // The top harmonic any HARMONIC law writes — Harm's seventh octave. Stated as a constant
        // because the distinctness tests analyse exactly this many harmonics.
        static constexpr int kMaxHarmonic = 64;

        static constexpr int kHarmPartialCount = 7; // 2^0 … 2^6
        static constexpr int kOrganDrawbarCount = 8;
        static constexpr int kChordPartialCount = 5; // root · both thirds · fifth · octave

        // Harm's octave stack: {1, 2, 4, 8, 16, 32, 64}. PUBLIC so the test pins the set at its
        // source rather than inferring it from a spectrum — the doubling is the law.
        [[nodiscard]] static std::array<int, kHarmPartialCount> harmPartials() noexcept;

        // Harm's per-octave tilt at frame `frame`: 0.5 (a 6-dB-per-octave taper) → 1.0 (flat).
        // Octave j is scaled by tilt^j, so mode 0 never moves and the stack opens from the top.
        [[nodiscard]] static double harmTilt (int frame) noexcept;

        // Organ's drawbar set: {1, 2, 3, 4, 6, 8, 12, 16}. The three QUINT ranks (3, 6, 12) are the
        // ones Harm cannot have, and `organIsQuint` names them so the test measures the same set
        // the law fades rather than a second transcription of it.
        [[nodiscard]] static std::array<int, kOrganDrawbarCount> organDrawbars() noexcept;
        [[nodiscard]] static double organDrawbarGain (int i) noexcept;
        [[nodiscard]] static bool organIsQuint (int harmonic) noexcept;

        // How far the quint drawbars are pulled out at frame `frame`: 0.05 → 1.0. Never zero, so
        // every drawbar is PRESENT on every frame and the set is a fact about the bank rather than
        // about which frame you looked at.
        [[nodiscard]] static double organQuintFade (int frame) noexcept;

        // Chord's partial set: {20, 24, 25, 30, 40} — root, minor third, major third, fifth,
        // octave. Both thirds are members: mid-bank both sound, which is what a crossfade between
        // two integer partials means.
        [[nodiscard]] static std::array<int, kChordPartialCount> chordPartials() noexcept;

        // The third's position at frame `frame`: 0 = MINOR (all of the third's energy at harmonic
        // 24), 1 = MAJOR (all at 25). The crossfade itself is equal-power (see the header note).
        [[nodiscard]] static double chordThirdBlend (int frame) noexcept;

        // Renders `bank`'s frame `frame` into `out` (kFrameLength floats). `frame` is clamped into
        // [0, kFrameCount). Deterministic. Not RT-safe (allocates, libm). This is the
        // `WavetableBank::RenderFrame` the catalogue captures.
        static void renderFrame (Bank bank, int frame, float* out);
        // THE RECIPE BEHIND `bank` (T-WT-12). Public so the ADR-041 validator battery walks the
        // SHIPPED recipes rather than a transcription of them: the DC tolerance a bank owes is
        // chosen by its kernel's DOMAIN, so a table of banks-to-kernels kept in a test file would
        // go stale at T-WT-14 and silently WEAKEN the check (a spectral bank held to the
        // time-domain's 1/N passes with 2000x the offset it owes). Reading the recipe is also what
        // lets one loop over `kBankCount` cover a roster that grows to 200 with no edit there.
        [[nodiscard]] static const WavetableRecipe& recipeFor (Bank bank) noexcept;

        // The whole bank, contiguous, frame f at [f · kFrameLength, (f+1) · kFrameLength).
        [[nodiscard]] static std::vector<float> generate (Bank bank);
    };
} // namespace nemesis::dsp
