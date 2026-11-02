#include <nemesis/dsp/ChipTableGenerator.h>

#include <nemesis/dsp/WavetableKernels.h>

#include <array>
#include <cstddef>

namespace nemesis::dsp
{
    namespace
    {
        // PWM's duty walks DOWN from 34/64 so that all 32 frames get a distinct integer numerator
        // (34 … 3) and the widest frame is a hair over 50 % rather than exactly at it — a 50 %
        // pulse has no even harmonics at all, and starting there would make the first frame the one
        // place the partner's own harmonics stand alone.
        constexpr double kPwmTopSixtyFourths = 34.0;

        // THE CHIP CATEGORY'S RECIPES (T-WT-11; ADR-041 decision 2). Two of the three banks are
        // the SAME kernel as BASIC's `Analog` — and the recipe table is where that stops being
        // invisible. What actually separates them is three things, each now a parameter rather
        // than a different function: the duty LAW, the octave-up square PARTNER, and the
        // NORMALISATION.
        //
        //   * Chip  — the four duties a chip's pulse register actually has (12.5 / 25 / 50 / 75 %)
        //             as a STAIRCASE, not a sweep, over a partner rising 0.15 → 0.85.
        //   * PWM   — an integer-64ths duty ramp with a FIXED 0.35 partner: the width is what
        //             moves here.
        //   * 2-Op  — phase modulation at an integer 2:1 ratio, index 0 → 6. Phase, not frequency:
        //             the modulator displaces the carrier's PHASE, which is what every digital
        //             "FM" chip actually did and what keeps the frame exactly periodic.
        //
        // THESE THREE SHIPPED A DIFFERENT NORMALISATION SPELLING FROM `Analog` — multiply by the
        // reciprocal rather than divide — and the seam briefly carried a selector for it. The
        // battery refuted the premise: the two spellings differ by at most an ulp of double, which
        // the cast to float swallows, so there is one law and no selector. The note is in
        // `WavetableRecipe.h`, where the selector used to be.
        // [T-WT-14 leaf 5] BAKED FRAME ORDERS for the four brightness banks (`WavetableRecipe.h`'s
        // note on `frameOrder`) — measured from the shipped kernel, sorted by centroid with the
        // generation index as the tie-break; the drift-guard test recomputes and checks them.
        constexpr std::array<std::uint8_t, 32> kLfsrOrder { 0, 1, 3, 2, 4, 7, 10, 6, 5, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 29, 31 };
        constexpr std::array<std::uint8_t, 32> kLfsrShortOrder { 3, 4, 1, 11, 13, 14, 0, 7, 9, 12, 10, 8, 15, 5, 2, 6, 16, 17, 18, 30, 27, 21, 24, 31, 28, 26, 20, 29, 25, 19, 22, 23 };
        constexpr std::array<std::uint8_t, 32> kNoiseToneOrder { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
        constexpr std::array<std::uint8_t, 32> kChipNoiseOrder { 30, 29, 26, 25, 23, 28, 24, 22, 31, 21, 19, 20, 27, 13, 14, 12, 17, 18, 15, 16, 11, 6, 10, 7, 9, 5, 4, 3, 1, 8, 2, 0 };

        constexpr std::array<WavetableRecipe, static_cast<std::size_t> (ChipTableGenerator::kBankCount)> kRecipes {
            WavetableRecipe { "factory.chip.chip",
                "Chip",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Pulse,
                kernel::Pulse::make (kernel::DutyLaw::Staircase,
                    0.0, // `Sweep` only — unused here
                    0.0,
                    0.0, // `SixtyFourths` only — unused here
                    kernel::DutySet::ChipRegisters,
                    0.15, // the partner rises …
                    0.85,
                    ChipTableGenerator::kPartnerRatio),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.pwm",
                "PWM",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Pulse,
                kernel::Pulse::make (kernel::DutyLaw::SixtyFourths,
                    0.0,
                    0.0,
                    kPwmTopSixtyFourths,
                    kernel::DutySet::ChipRegisters,
                    ChipTableGenerator::kPwmPartnerLevel, // … and here it does not
                    ChipTableGenerator::kPwmPartnerLevel,
                    ChipTableGenerator::kPartnerRatio),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.2op",
                "2-Op",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.0,
                    ChipTableGenerator::kTwoOpMaxIndex,
                    static_cast<double> (ChipTableGenerator::kTwoOpRatio)),
                FrameOrdering::Morph },

            // [T-WT-14 leaf 5] the 27 the roster names beyond the shipped three, transcribed from
            // `Tools/wavetable-proto/recipes.py`. CHIP's own `fm2op` banks keep INTEGER ratios by
            // design (the roster's own note: "METALLIC uses non-integer r; CHIP keeps r integer,
            // which is what an OPL-era 2-op voice actually did"), so none of them takes the
            // non-integer DC correction `renderFm2Op` gained above — they render exactly as `2-Op`
            // always has.
            WavetableRecipe { "factory.chip.duty8",
                "Duty 8",
                WavetableCategory::Chip,
                16,
                WavetableKernel::Pulse,
                kernel::Pulse::make (kernel::DutyLaw::Sweep, 0.4375, 0.0625, 0.0, kernel::DutySet::ChipRegisters, 0.0, 0.0, 0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.duty25",
                "Duty 25",
                WavetableCategory::Chip,
                16,
                WavetableKernel::DoublePulse,
                kernel::DoublePulse::make (0.25, 0.0625, 0.5),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.fm1",
                "FM 1:1",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.0, 6.0, 1.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.fm3",
                "FM 1:3",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.0, 5.0, 3.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.fm4",
                "FM 1:4",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.0, 4.5, 4.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.fm7",
                "FM 1:7",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.0, 3.5, 7.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.fmwide",
                "FM Wide",
                WavetableCategory::Chip,
                64,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.0, 12.0, 2.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.fmoctave",
                "FM Octave",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Fm2Op,
                kernel::Fm2Op::make (0.0, 3.0, 8.0),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.feedback",
                "Feedback",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::FmFeedback,
                kernel::FmFeedback::make (0.0, 2.6, 12),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.feedbacksoft",
                "Feedback Soft",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::FmFeedback,
                kernel::FmFeedback::make (0.0, 1.2, 12),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.feedbackhard",
                "Feedback Hard",
                WavetableCategory::Chip,
                64,
                WavetableKernel::FmFeedback,
                kernel::FmFeedback::make (0.4, 4.0, 12),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.feedbackedge",
                "Feedback Edge",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::FmFeedback,
                kernel::FmFeedback::make (1.6, 3.2, 20),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.feedbackfine",
                "Feedback Fine",
                WavetableCategory::Chip,
                16,
                WavetableKernel::FmFeedback,
                kernel::FmFeedback::make (0.0, 0.9, 12),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.xor",
                "XOR",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Bitwise,
                kernel::Bitwise::make (kernel::BitwiseOp::Xor, 1.0, 8.0, 255, 8),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.and",
                "AND",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Bitwise,
                kernel::Bitwise::make (kernel::BitwiseOp::And, 1.0, 7.0, 255, 8),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.modulo",
                "Modulo",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Bitwise,
                kernel::Bitwise::make (kernel::BitwiseOp::Mod, 200.0, 9.0, 255, 4),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.modtight",
                "Mod Tight",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Bitwise,
                kernel::Bitwise::make (kernel::BitwiseOp::Mod, 61.0, 7.0, 255, 9),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.xorwide",
                "XOR Wide",
                WavetableCategory::Chip,
                64,
                WavetableKernel::Bitwise,
                kernel::Bitwise::make (kernel::BitwiseOp::Xor, 1.0, 11.0, 255, 32),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.mul",
                "Multiply",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Bitwise,
                kernel::Bitwise::make (kernel::BitwiseOp::Mul, 1.0, 13.0, 255, 8),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.fold",
                "Fold",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Fold,
                kernel::Fold::make (kernel::Shape::Sine, 1.0, 6.0, 1),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.folddeep",
                "Fold Deep",
                WavetableCategory::Chip,
                64,
                WavetableKernel::Fold,
                kernel::Fold::make (kernel::Shape::Sine, 1.0, 14.0, 1),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.foldtri",
                "Fold Tri",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Fold,
                kernel::Fold::make (kernel::Shape::Triangle, 1.0, 8.0, 1),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.foldstack",
                "Fold Stack",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::Fold,
                kernel::Fold::make (kernel::Shape::Sine, 1.5, 5.0, 3),
                FrameOrdering::Morph },
            WavetableRecipe { "factory.chip.lfsr",
                "LFSR",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::NoiseSpectrum,
                kernel::NoiseSpectrum::make (12345U, 0.6, 0.0, 0.35, 1.0),
                FrameOrdering::Brightness,
                kLfsrOrder.data() },
            WavetableRecipe { "factory.chip.lfsrshort",
                "LFSR Short",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::NoiseSpectrum,
                kernel::NoiseSpectrum::make (999U, 0.3, 0.1, 0.15, 0.6),
                FrameOrdering::Brightness,
                kLfsrShortOrder.data() },
            WavetableRecipe { "factory.chip.noisetone",
                "Noise Tone",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::NoiseSpectrum,
                kernel::NoiseSpectrum::make (606060U, 1.8, 0.2, 1.0, 1.0),
                FrameOrdering::Brightness,
                kNoiseToneOrder.data() },
            WavetableRecipe { "factory.chip.chipnoise",
                "Chip Noise",
                WavetableCategory::Chip,
                ChipTableGenerator::kFrameCount,
                WavetableKernel::NoiseSpectrum,
                kernel::NoiseSpectrum::make (424242U, 0.1, 0.5, 1.0, 0.25),
                FrameOrdering::Brightness,
                kChipNoiseOrder.data() }
        };

    } // namespace

    const WavetableRecipe& ChipTableGenerator::recipeFor (Bank bank) noexcept
    {
        const auto i = static_cast<std::size_t> (bank);
        return kRecipes[i < kRecipes.size() ? i : 0];
    }

    double ChipTableGenerator::chipDuty (int frame) noexcept
    {
        static_assert (kFrameCount % kChipDutyCount == 0,
            "the duty staircase must divide the bank evenly, or one step gets fewer frames");
        return WavetableKernels::dutyAt (recipeFor (Bank::Chip), frame);
    }

    double ChipTableGenerator::chipPartnerLevel (int frame) noexcept
    {
        return WavetableKernels::partnerLevelAt (recipeFor (Bank::Chip), frame);
    }

    int ChipTableGenerator::pwmDutySixtyFourths (int frame) noexcept
    {
        static_assert (static_cast<int> (34) - (kFrameCount - 1) >= 1,
            "the duty sweep must stay above zero width");
        return WavetableKernels::dutySixtyFourthsAt (recipeFor (Bank::Pwm), frame);
    }

    double ChipTableGenerator::pwmDuty (int frame) noexcept
    {
        static_assert (kPwmDutyGrid == 64, "the SixtyFourths duty law divides by exactly 64");
        return WavetableKernels::dutyAt (recipeFor (Bank::Pwm), frame);
    }

    double ChipTableGenerator::twoOpIndex (int frame) noexcept
    {
        return WavetableKernels::fmIndexAt (recipeFor (Bank::TwoOp), frame);
    }

    void ChipTableGenerator::renderFrame (Bank bank, int frame, float* out)
    {
        WavetableKernels::render (recipeFor (bank), frame, out);
    }

    std::vector<float> ChipTableGenerator::generate (Bank bank)
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
