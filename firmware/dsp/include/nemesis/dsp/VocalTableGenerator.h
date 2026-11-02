#pragma once

#include <nemesis/dsp/HarmonicFrame.h>
#include <nemesis/dsp/WavetableBank.h>
#include <nemesis/dsp/WavetableRecipe.h>

#include <array>
#include <cstdint>
#include <vector>

namespace nemesis::dsp
{
    // The VOCAL-category factory wavetable generator (T-WT-03; `family_overview.md` §2.3 +
    // ADR-027). Three banks — **Vox · Formant · Choir** — synthesised from pure code, because
    // ADR-027 rules shipped factory content SELF-GENERATED: no third-party wavetable audio is a
    // build input, and a vowel table in particular is the kind of content that would otherwise
    // arrive as somebody's sampled singer.
    //
    // ONE LAW SHAPE, THREE CHARACTERS. All three are a source-filter model built through
    // `HarmonicFrame`: a glottal-tilt source times a set of resonances, morphed across 32 frames.
    // What separates them is measurable rather than nominal, and each bank's own test says so:
    //   * Vox     — a VOWEL morph, /u/ → /a/ (closed to open). The loudest partial is never the
    //               fundamental, and it climbs as F1 opens.
    //   * Formant — ONE narrow resonance sweeping 400 → 3000 Hz over a nearly flat source, the
    //               "wah" figure. Half the width of Vox's formants, so the peak is sharper, and it
    //               travels four times as far: harmonic ~4 to harmonic ~27. Not a vowel — a filter
    //               figure, which is why it is the one bank here written in its own frequencies.
    //   * Choir   — Vox's vowel geometry sung by THREE voices whose formants sit ±6 % apart, and
    //               the only bank here with SPREAD PHASES. That is what makes it a choir rather
    //               than a loud vowel: identical magnitudes stacked in phase peak together and
    //               buzz, and the same magnitudes spread in phase smear. Measured as crest factor,
    //               which is exactly the quantity phase moves and the spectrum cannot see.
    //
    // THE VOWEL DATA IS THE FAMILY'S OWN, NOT A SECOND OPINION. F1/F2/F3 for /u/ and /a/ are the
    // Peterson-Barney rows `dsp::FormantFilter` already ships (M1-09x, `filter.md` §12.1 — "vowel
    // data is public"), interpolated linearly in Hz exactly as that filter interpolates them, with
    // its per-formant gain taper. Two engines in one family disagreeing about where /a/ is would be
    // the "format decided twice" failure applied to content, so `vowelFormantsHz` is public and the
    // test checks both endpoints AGAINST `FormantFilter`'s own accessors rather than against a
    // literal copied twice.
    //
    // THE GEOMETRY IS WRITTEN IN Hz AT A STATED REFERENCE (`HarmonicFrame::kReferenceHz`, 110 Hz).
    // A wavetable frame has no pitch of its own, so a table played an octave up carries its
    // formants up with it — the accepted behaviour of a wavetable vowel, and the reason CATHODE
    // §13's `formant` FILTER model exists for the pitch-independent version.
    //
    // Frame cap 32, §2.3's factory band (16–64) and the same owner pick BASIC took: one table costs
    // `frames × 11 × 2048 × 8` bytes, so 32 frames is 5.8 MB against 46 MB at the format ceiling.
    //
    // Deterministic — no RNG, no clock, no file input. The phase spread Choir needs is a
    // golden-ratio sequence, not a random one, precisely so two runs are bit-identical and §2.3's
    // stable bank id keeps meaning something.
    //
    // Build-time only: allocates and runs libm, never called at render time.
    class VocalTableGenerator
    {
    public:
        // §2.3's within-category order. The ordinal IS the catalogue's within-category slot, so
        // inserting a bank in the middle is a catalogue-visible change rather than a private one.
        enum class Bank : std::uint8_t {
            Vox = 0,
            Formant = 1,
            Choir = 2,
            // [T-WT-14 leaf 5] the 22 more the roster names — the first three ordinals are
            // UNCHANGED.
            AhEe = 3,
            OoAh = 4,
            EeOo = 5,
            VowelCircle = 6,
            Nasal = 7,
            SweepLow = 8,
            SweepHigh = 9,
            SweepNarrow = 10,
            SweepWide = 11,
            SweepDown = 12,
            Throat = 13,
            Breath = 14,
            Whisper = 15,
            Fricative = 16,
            BreathVowel = 17,
            ChoirWide = 18,
            ChoirMen = 19,
            ChoirHigh = 20,
            ChoirStack = 21,
            Talkbox = 22,
            ResoPeak = 23,
            DoubleFormant = 24
        };

        static constexpr int kBankCount = 25;
        static constexpr int kFrameLength = WavetableBank::kFrameLength;
        static constexpr int kFrameCount = 32;

        // The top harmonic any VOCAL law writes: 64 × 110 Hz ≈ 7 kHz, above which a vowel's
        // glottal tilt has nothing left to say. Stated as a constant because the distinctness
        // tests analyse exactly this many harmonics — a law that quietly wrote harmonic 65 would
        // be invisible to them.
        static constexpr int kMaxHarmonic = 64;

        // The three formant centres in Hz at vowel-morph position `p` in [0, 1]: 0 = /u/ (closed),
        // 1 = /a/ (open), linearly interpolated in Hz. PUBLIC because it is the transcription the
        // test verifies against `FormantFilter`'s own vowel rows — a private copy of shared family
        // data is a copy that drifts silently.
        [[nodiscard]] static std::array<double, 3> vowelFormantsHz (double p) noexcept;

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
