#include <nemesis/dsp/VocalTableGenerator.h>

#include <nemesis/dsp/WavetableKernels.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace nemesis::dsp
{
    namespace
    {
        // THE VOCAL CATEGORY'S RECIPES (T-WT-11; ADR-041 decision 2). Two kernels, three banks —
        // `Vox` and `Choir` are the SAME law at different widths, floors and voice counts, which
        // is the fact the old one-function-per-bank shape hid and the recipe table states.
        //
        //   * Vox    — a vowel morph, /u/ → /a/. Source × filter: the glottal 1/k tilt times the
        //              vocal-tract response, at the vowel state the frame sits on.
        //   * Formant— ONE narrow resonance, 400 → 3000 Hz, at half Vox's width over a nearly FLAT
        //              1/√k source. The flatter source is what lets the peak travel: under a 1/k
        //              tilt harmonic 27 starts 27× down, so the fundamental would stay the loudest
        //              partial for the whole top half of the sweep and the bank's own claim ("the
        //              peak climbs") would be false while every frame still sounded plausible.
        //   * Choir  — Vox's vowel geometry sung by an ENSEMBLE: three formant sets ±6 % apart,
        //              each formant twice as WIDE, over a higher off-resonance floor, with
        //              Schroeder phases. The WIDTH is the bank — a detune-and-phases-only first
        //              version measured a centroid of 10.68 against Vox's 10.71, i.e. two banks
        //              that differed only in phase, which the ear would have heard as one wave
        //              under two names.
        // [T-WT-14 leaf 5] BAKED FRAME ORDERS for the three brightness banks (`WavetableRecipe.h`'s
        // note on `frameOrder`). `Breath`'s is the IDENTITY — its `noise_spectrum` generation order
        // is already brightness-monotone, so declaring the ordering costs it nothing, exactly as
        // T-WT-14 leaf 4's probe of the same recipe found. `Whisper` and `Fricative` are measured
        // from the shipped kernel (render in generation order, sort by centroid, generation index
        // as the tie-break — the drift-guard test below recomputes and checks these).
        constexpr std::array<std::uint8_t, 32> kBreathOrder { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
        constexpr std::array<std::uint8_t, 32> kWhisperOrder { 0, 1, 3, 2, 4, 6, 5, 7, 8, 9, 10, 11, 13, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
        constexpr std::array<std::uint8_t, 32> kFricativeOrder { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 14, 17, 16, 18, 19, 20, 21, 22, 23, 24, 25, 26, 28, 27, 29, 30, 31 };

        constexpr std::array<WavetableRecipe, static_cast<std::size_t> (VocalTableGenerator::kBankCount)> kRecipes {
            WavetableRecipe { "factory.vocal.vox",
                "Vox",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::Formants,
                kernel::Formants::make (kernel::VowelPair::UtoA,
                    1, // one tract
                    0.12, // off-resonance floor
                    1.0, // no smear
                    1.0, // no detune …
                    1.0,
                    0.0, // … and no side voices to weight
                    kernel::SourceLaw::OverK,
                    kernel::PhaseLaw::Zero),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.formant",
                "Formant",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::FormantSweep,
                kernel::FormantSweep::make (400.0,
                    3000.0,
                    0.02, // floor
                    0.11, // width in octaves — half Vox's
                    1.00, // the resonance's own weight over the floor
                    kernel::SourceLaw::OverSqrtK),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.choir",
                "Choir",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::Formants,
                kernel::Formants::make (kernel::VowelPair::UtoA,
                    3, // three voices
                    0.20, // more air between the formants than one voice has
                    2.0, // the ensemble smear — see the note above
                    0.94, // the two outer voices, stated as literals (see `Formants`)
                    1.06,
                    0.6, // their weight against the centre voice
                    kernel::SourceLaw::OverK,
                    kernel::PhaseLaw::Schroeder),
                FrameOrdering::Morph },

            // [T-WT-14 leaf 5] the 22 the roster names beyond the shipped three. None of these
            // name a `source`, so `_source_divisor`'s default applies — `SourceLaw::Free` at the
            // kernel's own default tilt (1.0 for `formants`, 0.8 for `formant_sweep`) unless the
            // recipe states one — the T-WT-12b defect this card's leaf 3 fixed.
            WavetableRecipe { "factory.vocal.ahee",
                "Ah Ee",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::Formants,
                kernel::Formants::make (kernel::VowelPair::AtoE,
                    1,
                    0.0,
                    1.0,
                    1.0,
                    1.0,
                    0.0,
                    kernel::SourceLaw::Free,
                    kernel::PhaseLaw::Zero,
                    0.0,
                    1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.ooah",
                "Oo Ah",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::Formants,
                kernel::Formants::make (kernel::VowelPair::OoToA,
                    1,
                    0.0,
                    1.0,
                    1.0,
                    1.0,
                    0.0,
                    kernel::SourceLaw::Free,
                    kernel::PhaseLaw::Zero,
                    0.0,
                    1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.eeoo",
                "Ee Oo",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::Formants,
                kernel::Formants::make (kernel::VowelPair::ItoU,
                    1,
                    0.0,
                    1.0,
                    1.0,
                    1.0,
                    0.0,
                    kernel::SourceLaw::Free,
                    kernel::PhaseLaw::Zero,
                    0.0,
                    1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.vowelcircle",
                "Vowel Circle",
                WavetableCategory::Vocal,
                64,
                WavetableKernel::Formants,
                kernel::Formants::make (kernel::VowelPair::AtoU,
                    1,
                    0.0,
                    1.0,
                    1.0,
                    1.0,
                    0.0,
                    kernel::SourceLaw::Free,
                    kernel::PhaseLaw::Zero,
                    0.26,
                    1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.nasal",
                "Nasal",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::Formants,
                kernel::Formants::make (kernel::VowelPair::ErToI,
                    1,
                    0.0,
                    1.0,
                    1.0,
                    1.0,
                    0.0,
                    kernel::SourceLaw::Free,
                    kernel::PhaseLaw::Zero,
                    0.18,
                    1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.sweeplow",
                "Sweep Low",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::FormantSweep,
                kernel::FormantSweep::make (200.0, 1200.0, 0.06, 0.22, 1.0, kernel::SourceLaw::Free, 0.8),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.sweephigh",
                "Sweep High",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::FormantSweep,
                kernel::FormantSweep::make (900.0, 5000.0, 0.06, 0.22, 1.0, kernel::SourceLaw::Free, 0.8),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.sweepnarrow",
                "Sweep Narrow",
                WavetableCategory::Vocal,
                64,
                WavetableKernel::FormantSweep,
                kernel::FormantSweep::make (300.0, 3400.0, 0.06, 0.12, 1.0, kernel::SourceLaw::Free, 0.8),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.sweepwide",
                "Sweep Wide",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::FormantSweep,
                kernel::FormantSweep::make (350.0, 2600.0, 0.06, 0.45, 1.0, kernel::SourceLaw::Free, 0.8),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.sweepdown",
                "Sweep Down",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::FormantSweep,
                kernel::FormantSweep::make (3200.0, 260.0, 0.06, 0.22, 1.0, kernel::SourceLaw::Free, 0.8),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.throat",
                "Throat",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::FormantSweep,
                kernel::FormantSweep::make (240.0, 900.0, 0.06, 0.30, 1.0, kernel::SourceLaw::Free, 1.2),
                FrameOrdering::Morph },

            // ─── brightness banks (T-WT-14 leaf 4's mechanism) ────────────────────────────────
            WavetableRecipe { "factory.vocal.breath",
                "Breath",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::NoiseSpectrum,
                kernel::NoiseSpectrum::make (90210U, 1.4, 0.3, 1.0, 1.0),
                FrameOrdering::Brightness,
                kBreathOrder.data() },
            WavetableRecipe { "factory.vocal.whisper",
                "Whisper",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::NoiseSpectrum,
                kernel::NoiseSpectrum::make (77345U, 0.9, 0.2, 0.5, 1.0),
                FrameOrdering::Brightness,
                kWhisperOrder.data() },
            WavetableRecipe { "factory.vocal.fricative",
                "Fricative",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::NoiseSpectrum,
                kernel::NoiseSpectrum::make (51001U, 0.2, -0.2, 0.8, 0.35),
                FrameOrdering::Brightness,
                kFricativeOrder.data() },

            WavetableRecipe { "factory.vocal.breathvowel",
                "Breath Vowel",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::Scramble,
                kernel::Scramble::make (33017U, 0.0, 1.0, 1.1, 0.35),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.choirwide",
                "Choir Wide",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::Formants,
                kernel::Formants::make (kernel::VowelPair::AtoO,
                    1,
                    0.0,
                    1.0,
                    1.0,
                    1.0,
                    0.0,
                    kernel::SourceLaw::Free,
                    kernel::PhaseLaw::Zero,
                    0.52,
                    1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.choirmen",
                "Choir Men",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::Formants,
                kernel::Formants::make (kernel::VowelPair::UtoEr,
                    1,
                    0.0,
                    1.0,
                    1.0,
                    1.0,
                    0.0,
                    kernel::SourceLaw::Free,
                    kernel::PhaseLaw::Zero,
                    0.40,
                    1.3),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.choirhigh",
                "Choir High",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::Formants,
                kernel::Formants::make (kernel::VowelPair::ItoE,
                    1,
                    0.0,
                    1.0,
                    1.0,
                    1.0,
                    0.0,
                    kernel::SourceLaw::Free,
                    kernel::PhaseLaw::Zero,
                    0.34,
                    0.7),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.choirstack",
                "Choir Stack",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::PhaseSpread,
                kernel::PhaseSpread::make (0.0, 2.8, 4, 1.2, 0.8, 3.5),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.talkbox",
                "Talkbox",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::FormantSweep,
                kernel::FormantSweep::make (420.0, 2400.0, 0.02, 0.09, 1.0, kernel::SourceLaw::Free, 0.8),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.resopeak",
                "Reso Peak",
                WavetableCategory::Vocal,
                64,
                WavetableKernel::FormantSweep,
                kernel::FormantSweep::make (600.0, 4200.0, 0.03, 0.07, 1.0, kernel::SourceLaw::Free, 0.5),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.vocal.doubleformant",
                "Double Formant",
                WavetableCategory::Vocal,
                VocalTableGenerator::kFrameCount,
                WavetableKernel::Formants,
                kernel::Formants::make (kernel::VowelPair::AeToOo,
                    1,
                    0.0,
                    1.0,
                    1.0,
                    1.0,
                    0.0,
                    kernel::SourceLaw::Free,
                    kernel::PhaseLaw::Zero,
                    0.15,
                    1.0),
                FrameOrdering::Morph }
        };

    } // namespace

    const WavetableRecipe& VocalTableGenerator::recipeFor (Bank bank) noexcept
    {
        const auto i = static_cast<std::size_t> (bank);
        return kRecipes[i < kRecipes.size() ? i : 0];
    }

    std::array<double, 3> VocalTableGenerator::vowelFormantsHz (double p) noexcept
    {
        // The Peterson-Barney rows `FormantFilter` ships (`filter.md` §12.1 — vowel data is
        // public), interpolated LINEARLY in Hz exactly as that filter interpolates them. /u/ at
        // p = 0 and /a/ at p = 1: F1 opens 300 → 730 Hz, which is what "closed to open" means and
        // what makes the loudest partial climb.
        //
        // STAYS HERE rather than moving into the kernel: this is SHARED FAMILY DATA, and the
        // recipe selects it by `VowelPair` rather than transcribing it, so the filter and the
        // wavetable cannot drift apart.
        constexpr double kU[3] { 300.0, 870.0, 2240.0 };
        constexpr double kA[3] { 730.0, 1090.0, 2440.0 };

        const double t = std::clamp (p, 0.0, 1.0);
        std::array<double, 3> out {};
        for (int i = 0; i < 3; ++i)
        {
            const auto j = static_cast<std::size_t> (i);
            out[j] = kU[j] + t * (kA[j] - kU[j]);
        }
        return out;
    }

    void VocalTableGenerator::renderFrame (Bank bank, int frame, float* out)
    {
        WavetableKernels::render (recipeFor (bank), frame, out);
    }

    std::vector<float> VocalTableGenerator::generate (Bank bank)
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
