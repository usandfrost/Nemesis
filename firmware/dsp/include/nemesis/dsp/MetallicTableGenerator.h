#pragma once

#include <nemesis/dsp/HarmonicFrame.h>
#include <nemesis/dsp/WavetableBank.h>
#include <nemesis/dsp/WavetableRecipe.h>

#include <array>
#include <cstdint>
#include <vector>

namespace nemesis::dsp
{
    // The METALLIC-category factory wavetable generator (T-WT-03; `family_overview.md` §2.3 +
    // ADR-027). Three banks — **Metal · Bell · Additive** — synthesised from pure code, because
    // ADR-027 rules shipped factory content SELF-GENERATED: no third-party wavetable audio is a
    // build input.
    //
    // ONE KERNEL, THREE DEPARTURES FROM A SMOOTH TILT. All three are built through
    // `HarmonicFrame`'s additive sum (VOCAL's kernel, reused rather than copied), and what makes
    // them metallic is not brightness alone but that none of them is a smooth harmonic decay:
    //   * Metal    — a bright source (k^−0.3) through a COMB whose notch spacing narrows 23.7 → 4.3
    //                harmonics over the bank. Roughly half the analysed energy sits above harmonic
    //                16 on every frame, which no BASIC law manages, and the notch COUNT grows as
    //                the comb tightens. The spacing is deliberately NON-INTEGER, so notches fall
    //                between harmonics and the surviving amplitudes are irregular — an integer
    //                spacing of 2 would just be a square wave, i.e. BASIC's territory.
    //   * Bell     — a SPARSE partial set: `MetallicResonator`'s own inharmonic mode ratios
    //                ROUNDED TO INTEGERS, {1, 3, 5, 9, 11, 14}, at that class's own mode gains.
    //                Six partials out of 64, an irregular gap sequence (2 · 2 · 4 · 2 · 3 — not a
    //                comb and not odd-only), and the complement is EXACTLY zero. The bank morphs
    //                through a bell's decay: the upper modes fade, so the strike is six partials
    //                and the tail is three.
    //   * Additive — harmonic BUILDUP: frame f carries harmonics 1..N at a k^−0.5 tilt and nothing
    //                above, N = 2 → 64. The claim is the HARD BAND EDGE — harmonic N is present
    //                and N + 1 is exactly zero — which no continuous BASIC shape has anywhere
    //                inside the analysed band.
    //
    // THE ROUNDING IS THE PRICE OF A PERIODIC FRAME, AND IT IS STATED RATHER THAN HIDDEN. A real
    // bell is INHARMONIC (2.76, 5.40, …) and a single-cycle wavetable frame cannot be: every
    // partial must complete a whole number of cycles in the frame or the frame does not loop.
    // Rounding to {1, 3, 5, 9, 11, 14} keeps the SHAPE of the mode set — the irregular gaps that
    // make it read as struck metal rather than as a sawtooth's or a square's regular series — and
    // gives up the exact ratios. A truly inharmonic bell is `dsp::ModalResonator`'s job (M1-08x),
    // which is a resonator and not a table, and the family already ships it.
    //
    // THE PARTIAL SET AND ITS GAINS ARE THE FAMILY'S OWN DATA, NOT A SECOND OPINION.
    // `bellPartials()` / `bellPartialGain()` are public so the test can verify both against
    // `MetallicResonator`'s own `modeFreq` / `modeGain` accessors rather than against a literal
    // typed a second time — the same rule VOCAL's vowel rows follow (two engines in one family
    // disagreeing about where a bell's partials sit is "the format decided twice", applied to
    // content).
    //
    // Frame cap 32, §2.3's factory band (16–64) and the same owner pick BASIC and VOCAL took: one
    // table costs `frames × 11 × 2048 × 8` bytes, so 32 frames is 5.8 MB against 46 MB at the
    // format ceiling.
    //
    // Deterministic — no RNG, no clock, no file input, so §2.3's stable bank id keeps meaning
    // something. Build-time only: allocates and runs libm, never called at render time.
    class MetallicTableGenerator
    {
    public:
        // §2.3's within-category order. The ordinal IS the catalogue's within-category slot, so
        // inserting a bank in the middle is a catalogue-visible change rather than a private one.
        enum class Bank : std::uint8_t {
            Metal = 0,
            Bell = 1,
            Additive = 2,
            // [T-WT-14 leaf 5] the 32 more the roster names — the first three ordinals are
            // UNCHANGED.
            BellSoft = 3,
            Gong = 4,
            Plate = 5,
            Marimba = 6,
            HandBell = 7,
            Bar = 8,
            Glass = 9,
            Tube = 10,
            TubeLong = 11,
            GlassBright = 12,
            BarStack = 13,
            Ring = 14,
            RingWide = 15,
            RingClose = 16,
            RingDark = 17,
            RingThin = 18,
            FmBell = 19,
            FmGlass = 20,
            FmClang = 21,
            FmDeep = 22,
            FmTine = 23,
            FmSteel = 24,
            Cymbal = 25,
            Crash = 26,
            Shimmer = 27,
            DenseMetal = 28,
            String = 29,
            Pluck = 30,
            StringBright = 31,
            Wire = 32,
            Pianoish = 33,
            Slack = 34
        };

        static constexpr int kBankCount = 35;
        static constexpr int kFrameLength = WavetableBank::kFrameLength;
        static constexpr int kFrameCount = 32;

        // The top harmonic any METALLIC law writes: 64 × 110 Hz ≈ 7 kHz. Stated as a constant
        // because the distinctness tests analyse exactly this many harmonics — a law quietly
        // writing harmonic 65 would be invisible to them — and because Additive's top frame is
        // defined to reach it exactly.
        static constexpr int kMaxHarmonic = 64;

        static constexpr int kBellPartialCount = 6;

        // Bell's integer partial set: `MetallicResonator::kRatios` rounded. PUBLIC because it is a
        // transcription of shared family data and the test verifies it against that class's own
        // accessor; a private copy of shared data is a copy that drifts in silence.
        [[nodiscard]] static std::array<int, kBellPartialCount> bellPartials() noexcept;

        // Bell's mode gain for partial `i` — `MetallicResonator::modeGain`'s set, same reason.
        [[nodiscard]] static double bellPartialGain (int i) noexcept;

        // Additive's harmonic count at frame `frame`: **2 + 2 · frame**, i.e. 2 → 64 in steps of
        // two. LINEAR, and the linearity is a measured decision rather than a preference: a
        // geometric 2 → 64 ramp (the perceptually nicer one) rounds to the SAME count on the
        // first several frames, and two identical frames is not a morph — the frame-invariant
        // test forbids it, correctly.
        [[nodiscard]] static int additiveHarmonicCount (int frame) noexcept;

        // Metal's comb notch spacing in harmonics at frame `frame`: 23.7 → 4.3, geometrically (a
        // comb is heard in octaves of spacing, so a linear sweep would crawl then race). Never an
        // integer and never below ~4: a spacing of 2 is an odd-harmonic-only spectrum, and a
        // spacing at which the FIRST notch reaches harmonic 1 would notch out the fundamental,
        // which is the property that separates this category from VOCAL.
        [[nodiscard]] static double metalCombSpacing (int frame) noexcept;

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

        // The whole bank, contiguous, frame f at [f · kFrameLength, (f+1) · kFrameLength) — the
        // layout `Wavetable::build` consumes. Not RT-safe.
        [[nodiscard]] static std::vector<float> generate (Bank bank);
    };
} // namespace nemesis::dsp
