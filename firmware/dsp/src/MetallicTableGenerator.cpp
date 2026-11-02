#include <nemesis/dsp/MetallicTableGenerator.h>

#include <nemesis/dsp/MetallicResonator.h>
#include <nemesis/dsp/WavetableKernels.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace nemesis::dsp
{
    namespace
    {
        // THE METALLIC CATEGORY'S RECIPES (T-WT-11; ADR-041 decision 2). Three banks, three
        // kernels — and the three kernels are the ones the roster reuses hardest downstream
        // (`comb` for 9 banks, `modal` for 13), which is why this category is where the seam pays
        // for itself first.
        //
        //   * Metal    — |cos(πk/spacing)|^0.6 over a k^−0.3 source, the spacing sweeping 23.7 →
        //                4.3 GEOMETRICALLY. Non-integer spacing on purpose: an integer spacing
        //                lands every notch exactly on a harmonic (spacing 2 zeroes the odd
        //                harmonics, fundamental included), which is a filter; a non-integer one
        //                leaves every harmonic present at an irregular level, which is a resonant
        //                body. The −0.3 source tilt is deliberately brighter than VOCAL's 1/k and
        //                BASIC's saw: the bank's headline claim is that half its energy sits above
        //                harmonic 16, and a 1/k source cannot get there under any comb.
        //   * Bell     — `MetallicResonator`'s own inharmonic mode set, mode i scaled by tilt^i so
        //                mode 0 never moves and the set thins from the top down. The tilt floor is
        //                0.55 rather than 0 because a bank that ends on a pure sine has stopped
        //                being a bell — at 0.55 the sixth mode is still 0.0075 of the fundamental
        //                while the significant-partial count has fallen 6 → 3.
        //   * Additive — a stack whose PARTIAL COUNT moves, 2 + 2f, at a fixed k^−0.5 tilt: growth,
        //                not brightening. The tilt sits between a flat stack (an impulse — one buzz
        //                at every N) and a saw's 1/k (which would make the top frames a plain
        //                band-limited saw, i.e. BASIC's law).
        // [T-WT-14 leaf 5] `Crash`'s BAKED FRAME ORDER (`WavetableRecipe.h`'s note on `frameOrder`)
        // — measured from the shipped kernel, sorted by centroid with the generation index as the
        // tie-break; the drift-guard test recomputes and checks it.
        constexpr std::array<std::uint8_t, 32> kCrashOrder { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };

        constexpr std::array<WavetableRecipe, static_cast<std::size_t> (MetallicTableGenerator::kBankCount)> kRecipes {
            WavetableRecipe { "factory.metallic.metal",
                "Metal",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Comb,
                kernel::Comb::make (23.7, 4.3, 0.3, 0.6),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.bell",
                "Bell",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Metallic, 0.55),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.additive",
                "Additive",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::HarmonicCount,
                kernel::HarmonicCount::make (2.0, 2.0, 0.5),
                FrameOrdering::Morph },

            // [T-WT-14 leaf 5] the 32 the roster names beyond the shipped three, transcribed from
            // `Tools/wavetable-proto/recipes.py`. The eight non-`Metallic` mode sets take the
            // kernel's own default gain law (1/(i+1)) — none of the roster's modal banks below
            // override it, only `Bell`'s shipped row does, and that row already reads
            // `MetallicResonator`'s own gains through `ModeSet::Metallic`.
            WavetableRecipe { "factory.metallic.bellsoft",
                "Bell Soft",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Bell, 0.75, 4.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.gong",
                "Gong",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Gong, 0.55, 8.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.plate",
                "Plate",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Plate, 0.55, 12.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.marimba",
                "Marimba",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Marimba, 0.55, 6.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.handbell",
                "Hand Bell",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Bell, 0.40, 9.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.bar",
                "Bar",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Bar, 0.55, 5.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.glass",
                "Glass",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Glass, 0.55, 7.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.tube",
                "Tube",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Tube, 0.55, 4.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.tubelong",
                "Tube Long",
                WavetableCategory::Metallic,
                64,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Tube, 0.70, 9.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.glassbright",
                "Glass Bright",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Glass, 0.80, 14.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.barstack",
                "Bar Stack",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Bar, 0.30, 10.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.ring",
                "Ring",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Ringmod,
                kernel::Ringmod::make (1.0, 13.0, 0.8, 32),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.ringwide",
                "Ring Wide",
                WavetableCategory::Metallic,
                64,
                WavetableKernel::Ringmod,
                kernel::Ringmod::make (2.0, 31.0, 0.8, 32),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.ringclose",
                "Ring Close",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Ringmod,
                kernel::Ringmod::make (1.0, 5.0, 0.8, 32),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.ringdark",
                "Ring Dark",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Ringmod,
                kernel::Ringmod::make (3.0, 17.0, 1.4, 32),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.ringthin",
                "Ring Thin",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Ringmod,
                kernel::Ringmod::make (7.0, 23.0, 0.4, 12),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.fmbell",
                "FM Bell",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.0, 5.0, 3.47),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.fmglass",
                "FM Glass",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.0, 7.0, 5.19),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.fmclang",
                "FM Clang",
                WavetableCategory::Metallic,
                64,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.5, 9.0, 7.83),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.fmdeep",
                "FM Deep",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.0, 6.0, 1.41),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.fmtine",
                "FM Tine",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.0, 3.2, 2.05),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.fmsteel",
                "FM Steel",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.2, 4.4, 11.3),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.cymbal",
                "Cymbal",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Modal,
                kernel::Modal::make (kernel::ModeSet::Cymbal, 0.50, 16.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.crash",
                "Crash",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::NoiseSpectrum,
                kernel::NoiseSpectrum::make (40411U, 0.4, -0.1, 1.0, 1.0),
                FrameOrdering::Brightness,
                kCrashOrder.data() },
            WavetableRecipe { "factory.metallic.shimmer",
                "Shimmer",
                WavetableCategory::Metallic,
                64,
                WavetableKernel::Comb,
                kernel::Comb::make (71.0, 11.5, 0.15, 0.6),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.densemetal",
                "Dense Metal",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Comb,
                kernel::Comb::make (21.0, 2.1, 0.1, 0.35, 0.55),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.string",
                "String",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Comb,
                kernel::Comb::make (9.3, 3.1, 0.9, 1.2),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.pluck",
                "Pluck",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Comb,
                kernel::Comb::make (5.7, 2.3, 1.1, 1.6),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.stringbright",
                "String Bright",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Comb,
                kernel::Comb::make (29.0, 4.1, 0.85, 0.9, 0.5),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.wire",
                "Wire",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Stretch,
                kernel::Stretch::make (1.0, 1.22, 0.9, 28),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.pianoish",
                "Stiff String",
                WavetableCategory::Metallic,
                64,
                WavetableKernel::Stretch,
                kernel::Stretch::make (1.0, 1.17, 1.1, 40),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.metallic.slack",
                "Slack String",
                WavetableCategory::Metallic,
                MetallicTableGenerator::kFrameCount,
                WavetableKernel::Stretch,
                kernel::Stretch::make (1.0, 0.86, 0.8, 32),
                FrameOrdering::Morph }
        };

    } // namespace

    const WavetableRecipe& MetallicTableGenerator::recipeFor (Bank bank) noexcept
    {
        const auto i = static_cast<std::size_t> (bank);
        return kRecipes[i < kRecipes.size() ? i : 0];
    }

    std::array<int, MetallicTableGenerator::kBellPartialCount> MetallicTableGenerator::bellPartials() noexcept
    {
        // `MetallicResonator`'s own inharmonic mode ratios, read through its own accessor and
        // ROUNDED to integers: {1, 2.76, 5.40, 8.93, 11.34, 13.87} → {1, 3, 5, 9, 11, 14}. Read
        // rather than re-typed, because a private copy of shared family data is a copy that drifts
        // in silence — and the rounding is the price of a frame that loops (see the header).
        //
        // STAYS HERE rather than becoming recipe data, for exactly that reason: the `modal`
        // kernel SELECTS this set by `ModeSet::Metallic` and reads it through this accessor, so
        // the static_assert below still binds and a mode added to the resonator is still a partial
        // owed here.
        static_assert (kBellPartialCount == MetallicResonator::kNumModes,
            "Bell IS the resonator's mode set; a mode added there is a partial owed here");

        std::array<int, kBellPartialCount> out {};
        for (int i = 0; i < kBellPartialCount; ++i)
            out[static_cast<std::size_t> (i)] =
                static_cast<int> (std::lround (static_cast<double> (MetallicResonator::modeFreq (1.0f, i))));
        return out;
    }

    double MetallicTableGenerator::bellPartialGain (int i) noexcept
    {
        // Clamped rather than range-checked: the render loop indexes this from a mode counter, and
        // §2.3's other law accessors are total in the same way.
        const int m = std::clamp (i, 0, kBellPartialCount - 1);
        return static_cast<double> (MetallicResonator::modeGain (m));
    }

    int MetallicTableGenerator::additiveHarmonicCount (int frame) noexcept
    {
        // 2 + 2f: 2 harmonics at frame 0, kMaxHarmonic at the last frame, every frame distinct.
        // The arithmetic now lives in the recipe (`HarmonicCount::make (2, 2, …)`); this accessor
        // DELEGATES rather than restating it, so the law the tests read and the law the kernel
        // renders cannot be two different laws.
        static_assert (2 + 2 * (kFrameCount - 1) == kMaxHarmonic,
            "the buildup must land exactly on the stated top harmonic");
        return WavetableKernels::harmonicCountAt (recipeFor (Bank::Additive), frame);
    }

    double MetallicTableGenerator::metalCombSpacing (int frame) noexcept
    {
        // Geometric, because a comb is heard in octaves of spacing: a linear sweep from 23.7 to
        // 4.3 would spend half the bank between 14 and 24 harmonics, where the comb barely changes
        // character, and race through the tight end where it changes fastest. Delegated for the
        // same reason as `additiveHarmonicCount` above.
        return WavetableKernels::combSpacingAt (recipeFor (Bank::Metal), frame);
    }

    void MetallicTableGenerator::renderFrame (Bank bank, int frame, float* out)
    {
        WavetableKernels::render (recipeFor (bank), frame, out);
    }

    std::vector<float> MetallicTableGenerator::generate (Bank bank)
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
