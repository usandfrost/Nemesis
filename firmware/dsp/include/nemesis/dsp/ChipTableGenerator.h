#pragma once

#include <nemesis/dsp/WavetableBank.h>
#include <nemesis/dsp/WavetableRecipe.h>

#include <cstdint>
#include <vector>

namespace nemesis::dsp
{
    // The CHIP-category factory wavetable generator (T-WT-03; `family_overview.md` §2.3 +
    // ADR-027). Three banks — **Chip · PWM · 2-Op** — synthesised from pure code.
    //
    // THE FIRST CATEGORY WRITTEN IN THE TIME DOMAIN, AND THAT CHANGES WHAT "ALIAS-FREE" MEANS.
    // VOCAL, METALLIC and HARMONIC are additive laws with a stated top harmonic, so their frames are
    // band-limited BY CONSTRUCTION and their tests assert exactly that. A chip wave is a hard-edged
    // pulse: forcing it through a harmonic sum would either band-limit away the corner that makes it
    // a chip wave or need hundreds of harmonics to fake one (which is why `HarmonicFrame`'s own
    // header says CHIP is deliberately not a consumer). So these frames carry harmonics to Nyquist
    // by design, and "alias-free" here means what it means for BASIC's pulse: **the mip pyramid does
    // its job** — the M1-04 keystone, re-run per bank (`tests/harness/AliasFloor.h`).
    //
    // ZERO MEAN IS STILL BY CONSTRUCTION, JUST BY DIFFERENT ARITHMETIC. An additive law gets it free;
    // a pulse does not — a 12.5 % duty pulse of ±1 sits at −0.75 mean, and a DC offset in an
    // oscillator frame survives the pyramid (which zeroes bins ABOVE the mip's top harmonic and
    // never touches bin 0) and thumps on every note. So the two levels are chosen from the duty
    // itself — high = 1 − d, low = −d over exactly d·N samples — rather than subtracted afterwards.
    //
    // THE THREE LAWS, and the first two differ in WHICH QUANTITY MOVES, not in how bright they get:
    //   * Chip  — the duty is a STAIRCASE of the four values a chip's duty register actually has,
    //             {1/8, 1/4, 1/2, 3/4}, eight frames each; what travels across the bank is the
    //             BALANCE against an octave-up square partner, whose level sweeps 0.15 → 0.85.
    //             Four sample levels, never more.
    //   * PWM   — the mirror: the partner's level is FIXED and the WIDTH travels, sweeping down a
    //             1/64 grid (34/64 → 3/64, a distinct duty on every frame). That is what makes it
    //             more than BASIC's `Analog` at lower resolution: Analog is one pulse with two
    //             sample levels and a continuous duty, this is two voices with four levels and a
    //             quantised one.
    //   * 2-Op  — two operators, phase modulation at ratio 2, index 0 → 6. Frame 0 is a PURE SINE
    //             (index 0 is the carrier alone), and because the ratio is 2 the sidebands land at
    //             1 + 2n, i.e. the spectrum is ODD-harmonic only — a structural claim its test
    //             asserts as an exact zero rather than a threshold.
    //
    // THE PARTNER IS WHY THE FIRST TWO BANKS ARE NOT ONE BANK. Both are "a pulse plus an octave
    // square", and the handoff sketch for PWM said "a rising octave square". Read as a rising
    // PITCH — partner at 2×, then 4×, then 8× — it groups the bank into three blocks inside which
    // nothing varies, so frames come out IDENTICAL and the frame-invariant test refuses them
    // (correctly). Read as a rising LEVEL it is Chip's own mechanism. Resolved by giving each bank
    // one moving quantity and fixing the other, which is a sharper contrast than either sketch.
    //
    // Frame cap 32, §2.3's factory band. Deterministic — no RNG (a chip's noise channel is an LFSR
    // and would be legitimate here, but no bank needs one). Build-time only: allocates, runs libm.
    class ChipTableGenerator
    {
    public:
        // §2.3's within-category order. The ordinal IS the catalogue's within-category slot.
        enum class Bank : std::uint8_t {
            Chip = 0,
            Pwm = 1,
            TwoOp = 2,
            // [T-WT-14 leaf 5] the 27 more the roster names — the first three ordinals are
            // UNCHANGED.
            Duty8 = 3,
            Duty25 = 4,
            Fm1 = 5,
            Fm3 = 6,
            Fm4 = 7,
            Fm7 = 8,
            FmWide = 9,
            FmOctave = 10,
            Feedback = 11,
            FeedbackSoft = 12,
            FeedbackHard = 13,
            FeedbackEdge = 14,
            FeedbackFine = 15,
            Xor = 16,
            And = 17,
            Modulo = 18,
            ModTight = 19,
            XorWide = 20,
            Multiply = 21,
            Fold = 22,
            FoldDeep = 23,
            FoldTri = 24,
            FoldStack = 25,
            Lfsr = 26,
            LfsrShort = 27,
            NoiseTone = 28,
            ChipNoise = 29
        };

        static constexpr int kBankCount = 30;
        static constexpr int kFrameLength = WavetableBank::kFrameLength;
        static constexpr int kFrameCount = 32;

        static constexpr int kChipDutyCount = 4; // the register set, not a taste
        static constexpr int kPwmDutyGrid = 64; // duties are exact 1/64ths
        static constexpr int kPartnerRatio = 2; // the octave-up square, both pulse banks
        static constexpr int kTwoOpRatio = 2; // modulator : carrier, hence odd harmonics only
        static constexpr double kTwoOpMaxIndex = 6.0;

        // Chip's duty at frame `frame`: one of {0.125, 0.25, 0.5, 0.75}, eight frames each.
        // PUBLIC so the test pins the staircase at its source — that it takes exactly four distinct
        // values across the bank is the bank's identity, and a spectrum states it only indirectly.
        [[nodiscard]] static double chipDuty (int frame) noexcept;

        // Chip's partner level at frame `frame`: 0.15 → 0.85. Never zero, so the partner is present
        // on every frame and "four sample levels" is a fact about the bank.
        [[nodiscard]] static double chipPartnerLevel (int frame) noexcept;

        // PWM's duty at frame `frame`: (34 − frame)/64, i.e. 0.531 → 0.047 with a distinct value on
        // every frame. `pwmDutySixtyFourths` is the integer, so the GRID is testable exactly rather
        // than through a floating-point remainder.
        [[nodiscard]] static int pwmDutySixtyFourths (int frame) noexcept;
        [[nodiscard]] static double pwmDuty (int frame) noexcept;
        static constexpr double kPwmPartnerLevel = 0.35; // FIXED — the width is what moves here

        // 2-Op's modulation index at frame `frame`: 0 → kTwoOpMaxIndex, linearly.
        [[nodiscard]] static double twoOpIndex (int frame) noexcept;

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
