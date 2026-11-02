#include <nemesis/dsp/HarmonicTableGenerator.h>

#include <nemesis/dsp/WavetableKernels.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace nemesis::dsp
{
    namespace
    {
        // Organ's registration, indexed as `organDrawbars()` is. 8′ (harmonic 2) is the loudest
        // rank and 16′ (harmonic 1) sits just under it, which is how a drawbar organ is actually
        // balanced; the quints are the three that fade.
        //
        // SHARED LAW DATA, not recipe data: the `drawbars` kernel SELECTS this set by
        // `Registration::Classic` and reads it through the three public accessors below, so the
        // registration is stated once and both the tests and the kernel read the same one.
        constexpr std::array<int, HarmonicTableGenerator::kOrganDrawbarCount> kOrganSet { 1, 2, 3, 4, 6, 8, 12, 16 };
        constexpr std::array<double, HarmonicTableGenerator::kOrganDrawbarCount> kOrganGain {
            0.90, // 16′ sub
            1.00, // 8′ unison
            0.70, // 5⅓′ QUINT
            0.70, // 4′
            0.50, // 2⅔′ QUINT
            0.45, // 2′
            0.30, // 1⅗′ QUINT
            0.30 // 1′
        };
        constexpr std::array<bool, HarmonicTableGenerator::kOrganDrawbarCount> kOrganQuint {
            false,
            false,
            true,
            false,
            true,
            false,
            true,
            false
        };

        // Chord's just-intoned triad. 20 is the smallest base at which BOTH thirds are exact
        // integers (6/5 → 24, 5/4 → 25); the fifth (3/2 → 30) and the octave (2/1 → 40) follow.
        constexpr int kChordRoot = 20;
        constexpr int kChordMinorThird = 24;
        constexpr int kChordMajorThird = 25;
        constexpr int kChordFifth = 30;
        constexpr int kChordOctave = 40;

        // THE HARMONIC CATEGORY'S RECIPES (T-WT-11; ADR-041 decision 2).
        //
        //   * Harm  — an OCTAVE stack (1, 2, 4 … 64) whose per-partial tilt^j moves 0.5 → 1.0.
        //             0.5 is 6 dB per octave (a soft, hollow stack), 1.0 is flat (every octave
        //             equally loud, the brightest a pure octave stack can be).
        //   * Organ — the eight drawbars above, with the three QUINTS fading in 0.05 → 1.0. The
        //             floor is 0.05 rather than 0 so all eight ranks are PRESENT on every frame —
        //             the set is then a property of the bank, not of the frame you happened to
        //             analyse — while still being 26 dB down, i.e. inaudible as a rank.
        //   * Chord — the just triad, with an EQUAL-POWER crossfade between the two thirds. The
        //             gains are a voicing's balance (root carrying, octave lightest), not a
        //             spectral tilt.
        constexpr std::array<WavetableRecipe, static_cast<std::size_t> (HarmonicTableGenerator::kBankCount)> kRecipes {
            WavetableRecipe { "factory.harmonic.harm",
                "Harm",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::OctaveStack,
                kernel::OctaveStack::make (kernel::PartialSet::Octaves, 0.5, 1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.organ",
                "Organ",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Drawbars,
                kernel::Drawbars::make (kernel::Registration::Classic, 0.05, 1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.chord",
                "Chord",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::JustTriad,
                kernel::JustTriad::make (kernel::PartialSet::JustTriad,
                    1.00, // root — carries
                    0.72, // either third, equal-power crossfaded
                    0.85, // fifth
                    0.60), // octave — lightest
                FrameOrdering::Morph },

            // [T-WT-14 leaf 5] the 37 the roster names beyond the shipped three, transcribed from
            // `Tools/wavetable-proto/recipes.py`. `factory.harmonic.phaserish` carries a re-tune
            // (DEFERRED §B): its recipe was written against `comb`'s pre-T-WT-12b LINEAR spacing
            // interpolation, and the corrected geometric glide leaves it 0.29 travel against the
            // 0.30 floor — a wider `sp0`→`sp1` span is the fix, and it is the recipe that moves,
            // not the kernel.
            WavetableRecipe { "factory.harmonic.organjazz",
                "Organ Jazz",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Drawbars,
                kernel::Drawbars::makePair (kernel::Registration::Jazz, kernel::Registration::Reed),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.organgospel",
                "Organ Gospel",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Drawbars,
                kernel::Drawbars::makePair (kernel::Registration::Gospel, kernel::Registration::Full),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.organtibia",
                "Organ Tibia",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Drawbars,
                kernel::Drawbars::makePair (kernel::Registration::Tibia, kernel::Registration::Gospel, 0.6),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.organceleste",
                "Organ Celeste",
                WavetableCategory::Harmonic,
                64,
                WavetableKernel::Drawbars,
                kernel::Drawbars::makePair (kernel::Registration::Celeste, kernel::Registration::Gospel),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.pipe",
                "Pipe",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Drawbars,
                kernel::Drawbars::makePair (kernel::Registration::Principal, kernel::Registration::Flute),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.oddonly",
                "Odd Only",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::OddEven,
                kernel::OddEven::make (0.5, 1.0, 1.0, 0.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.evenonly",
                "Even Only",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::OddEven,
                kernel::OddEven::make (0.5, 0.0, 1.0, 0.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.oddevensweep",
                "Odd Even Sweep",
                WavetableCategory::Harmonic,
                64,
                WavetableKernel::OddEven,
                kernel::OddEven::make (0.0, 1.0, 1.0, 0.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.hollow",
                "Hollow",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::OddEven,
                kernel::OddEven::make (0.60, 1.0, 1.8, 1.1),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.oddbright",
                "Odd Bright",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::OddEven,
                kernel::OddEven::make (0.5, 1.0, 1.3, 1.2),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.evensoft",
                "Even Soft",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::OddEven,
                kernel::OddEven::make (0.5, 0.12, 1.6, 0.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.comb",
                "Comb",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Comb,
                kernel::Comb::make (31.3, 3.7, 1.0, 1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.notch",
                "Notch",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::NotchSweep,
                kernel::NotchSweep::make (2.0, 90.0, 0.7, 0.98, 1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.notchdown",
                "Notch Down",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::NotchSweep,
                kernel::NotchSweep::make (90.0, 2.0, 0.7, 0.98, 1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.notchnarrow",
                "Notch Narrow",
                WavetableCategory::Harmonic,
                64,
                WavetableKernel::NotchSweep,
                kernel::NotchSweep::make (2.0, 72.0, 0.55, 1.0, 1.0),
                FrameOrdering::Morph },
            // [T-WT-14 leaf 5 RE-TUNE, DEFERRED §B] `sp0`/`sp1` widened from the roster's original
            // 47.0/8.3 — the span the pre-T-WT-12b linear comb interpolation was tuned against —
            // to clear the 0.30 travel floor under the shipped geometric glide. See the measured
            // value in the comment; the law and every other parameter are unchanged from the roster.
            WavetableRecipe { "factory.harmonic.phaserish",
                "Phase Comb",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Comb,
                kernel::Comb::make (58.0, 6.0, 1.2, 0.7),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.combtight",
                "Comb Tight",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Comb,
                kernel::Comb::make (7.3, 2.1, 1.0, 1.4),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.major",
                "Major",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Chord,
                kernel::Chord::make (kernel::ChordVoicing::MajorToOctave, 12, 1.0, 0.45),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.minor",
                "Minor",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Chord,
                kernel::Chord::make (kernel::ChordVoicing::MinorToWide, 12, 1.0, 0.45),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.fifths",
                "Fifths",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Chord,
                kernel::Chord::make (kernel::ChordVoicing::Fifths, 12, 1.0, 0.5),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.sus",
                "Sus",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Chord,
                kernel::Chord::make (kernel::ChordVoicing::Sus, 12, 1.0, 0.45),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.seventh",
                "Seventh",
                WavetableCategory::Harmonic,
                64,
                WavetableKernel::Chord,
                kernel::Chord::make (kernel::ChordVoicing::Seventh, 12, 1.0, 0.5),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.octaves",
                "Octaves",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Chord,
                kernel::Chord::make (kernel::ChordVoicing::Octaves, 12, 1.0, 0.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.stretchup",
                "Stretch Up",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Stretch,
                kernel::Stretch::make (1.0, 1.35, 0.9, 24),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.stretchdown",
                "Stretch Down",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Stretch,
                kernel::Stretch::make (1.0, 0.72, 0.9, 24),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.stretchwide",
                "Stretch Wide",
                WavetableCategory::Harmonic,
                64,
                WavetableKernel::Stretch,
                kernel::Stretch::make (0.80, 1.30, 0.9, 32),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.stretchfine",
                "Stretch Fine",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Stretch,
                kernel::Stretch::make (1.0, 1.21, 1.0, 48),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.stretchdense",
                "Stretch Dense",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Stretch,
                kernel::Stretch::make (1.0, 1.18, 0.7, 56),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.tiltdark",
                "Tilt Dark",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::TiltSweep,
                kernel::TiltSweep::make (3.0, 1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.tiltbright",
                "Tilt Bright",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::TiltSweep,
                kernel::TiltSweep::make (1.0, 0.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.tiltfull",
                "Tilt Full",
                WavetableCategory::Harmonic,
                64,
                WavetableKernel::TiltSweep,
                kernel::TiltSweep::make (3.5, -0.2),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.shelf",
                "Shelf",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::TiltShelf,
                kernel::TiltShelf::make (2.0, 80.0, 0.6, 2.2),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.shelfdown",
                "Shelf Down",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::TiltShelf,
                kernel::TiltShelf::make (80.0, 2.0, 0.6, 2.2),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.walsh",
                "Walsh",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Walsh,
                kernel::Walsh::make (1.0, 32.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.walshwide",
                "Walsh Wide",
                WavetableCategory::Harmonic,
                64,
                WavetableKernel::Walsh,
                kernel::Walsh::make (1.0, 64.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.walshdown",
                "Walsh Down",
                WavetableCategory::Harmonic,
                HarmonicTableGenerator::kFrameCount,
                WavetableKernel::Walsh,
                kernel::Walsh::make (24.0, 2.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.harmonic.walshtight",
                "Walsh Tight",
                WavetableCategory::Harmonic,
                16,
                WavetableKernel::Walsh,
                kernel::Walsh::make (4.0, 16.0),
                FrameOrdering::Morph }
        };

    } // namespace

    const WavetableRecipe& HarmonicTableGenerator::recipeFor (Bank bank) noexcept
    {
        const auto i = static_cast<std::size_t> (bank);
        return kRecipes[i < kRecipes.size() ? i : 0];
    }

    std::array<int, HarmonicTableGenerator::kHarmPartialCount> HarmonicTableGenerator::harmPartials() noexcept
    {
        static_assert (1 << (kHarmPartialCount - 1) == kMaxHarmonic,
            "the octave stack must land exactly on the stated top harmonic");

        std::array<int, kHarmPartialCount> out {};
        for (int j = 0; j < kHarmPartialCount; ++j)
            out[static_cast<std::size_t> (j)] = 1 << j;
        return out;
    }

    double HarmonicTableGenerator::harmTilt (int frame) noexcept
    {
        // Delegated: the endpoints live in `Harm`'s recipe, so this accessor and the kernel read
        // one law instead of two copies of it.
        return WavetableKernels::tiltAt (recipeFor (Bank::Harm), frame);
    }

    std::array<int, HarmonicTableGenerator::kOrganDrawbarCount> HarmonicTableGenerator::organDrawbars() noexcept
    {
        return kOrganSet;
    }

    double HarmonicTableGenerator::organDrawbarGain (int i) noexcept
    {
        // Clamped rather than range-checked, like the other law accessors in this category: the
        // render loop indexes it from a drawbar counter.
        return kOrganGain[static_cast<std::size_t> (std::clamp (i, 0, kOrganDrawbarCount - 1))];
    }

    bool HarmonicTableGenerator::organIsQuint (int harmonic) noexcept
    {
        for (int i = 0; i < kOrganDrawbarCount; ++i)
            if (kOrganSet[static_cast<std::size_t> (i)] == harmonic)
                return kOrganQuint[static_cast<std::size_t> (i)];
        return false;
    }

    double HarmonicTableGenerator::organQuintFade (int frame) noexcept
    {
        return WavetableKernels::quintFadeAt (recipeFor (Bank::Organ), frame);
    }

    std::array<int, HarmonicTableGenerator::kChordPartialCount> HarmonicTableGenerator::chordPartials() noexcept
    {
        return { kChordRoot, kChordMinorThird, kChordMajorThird, kChordFifth, kChordOctave };
    }

    double HarmonicTableGenerator::chordThirdBlend (int frame) noexcept
    {
        return WavetableKernels::chordBlendAt (recipeFor (Bank::Chord), frame);
    }

    void HarmonicTableGenerator::renderFrame (Bank bank, int frame, float* out)
    {
        WavetableKernels::render (recipeFor (bank), frame, out);
    }

    std::vector<float> HarmonicTableGenerator::generate (Bank bank)
    {
        // [T-WT-14 leaf 5] READS THE RECIPE'S OWN FRAME COUNT, not the class-wide `kFrameCount`.
        // Every bank in the original three used the same count, so the two were interchangeable
        // and this loop never noticed; leaf 5 adds banks at 16 and 64 within categories whose
        // `kFrameCount` stays 32; bulk-filling at the class constant would silently truncate or
        // overrun those banks. `renderFrame`'s own clamp was already recipe-aware — this is the
        // one caller that was not.
        const int n = recipeFor (bank).frames;
        std::vector<float> frames (static_cast<std::size_t> (n) * static_cast<std::size_t> (kFrameLength));
        for (int f = 0; f < n; ++f)
            renderFrame (bank, f, frames.data() + static_cast<std::size_t> (f) * static_cast<std::size_t> (kFrameLength));
        return frames;
    }
} // namespace nemesis::dsp
