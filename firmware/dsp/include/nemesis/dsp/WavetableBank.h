#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace nemesis::dsp
{
    // The shared FACTORY-BANK seam (T-WT-03 leaf A; `family_overview.md` §2.3 + ADR-027).
    //
    // WHAT IT IS FOR. §2.3's catalogue holds banks from five categories plus, later, imported sets
    // — and until this leaf the two baked-preview formats were `static` members of
    // `BasicTableGenerator` keyed by ITS bank enum, so the catalogue's entry was typed to that one
    // generator and a second category had nowhere to go. This class is the generalisation, and it
    // is deliberately the SMALLEST one that works: a bank is anything that can render one
    // single-cycle frame, and the previews are baked from that renderer.
    //
    // A `RenderFrame` IS THE WHOLE CONTRACT — no base class, no virtual, no registry of
    // generators. Each category generator stays a free-standing class with its own `Bank` enum and
    // its own laws (they share nothing but the frame length), and the catalogue binds one to a
    // bank by capturing it. That keeps the ownership rule §2.3 asks for: the generators stay in
    // dsp, the catalogue's identity data stays in core, and `WavetableCatalogue.h` still names no
    // dsp type at all.
    //
    // BUILD-TIME ONLY, never the audio thread: a renderer allocates and runs libm (and, for
    // MASK's Centroid ordering, an FFT), and the bakes below call it up to
    // `kWaterfallFrames + 1` times.
    class WavetableBank
    {
    public:
        // The family-locked single-cycle length (OI-AE-10 / `PRISM.md` §1). Spelled once here
        // because every category generator and the catalogue's own frame buffer need it, and three
        // copies of 2048 is three chances for one of them to age.
        static constexpr int kFrameLength = 2048;

        // §2.3's two baked-preview formats, fixed in the CONTRACT rather than by whichever
        // producer got written first — the generators and the T-WT-05 importer are different
        // cards, and a format decided twice is a format reconciled later. `core` re-declares these
        // as `kWavetableChipPoints` / `kWavetableWaterfall*` so a catalogue consumer does not
        // compile a dsp header; the catalogue's tests hold the two spellings together.
        static constexpr int kChipPoints = 64; // 64-point int8 polyline of one frame (32 min/max pairs)
        static constexpr int kWaterfallFrames = 16; // 16 rows …
        static constexpr int kWaterfallPoints = 64; // … × 64 points

        // Writes `kFrameLength` samples of one single-cycle frame. The frame index is the
        // generator's own — a generator clamps it, because the callers below and the L4 pane drive
        // this with computed indices.
        using RenderFrame = std::function<void (int frame, float* out)>;

        // The one QUANTISATION law both previews use: a value clamped to ±1 and scaled to int8.
        // Exposed so the importer (T-WT-05) has a definition to match rather than two examples to
        // infer from — and so the CLAMP is testable on its boundary. Without it a generator bug of
        // ±2 would WRAP (2 · 127 = 254 → −2 as int8) and draw as an inverted spike.
        [[nodiscard]] static std::int8_t quantisePoint (double v) noexcept;

        // THE SAMPLING LAW BOTH PREVIEWS FILL THEIR POINTS WITH: a MIN/MAX ENVELOPE. `frame` is
        // `kFrameLength` samples; `out` receives `kChipPoints` of them as `kChipPoints / 2` chunks
        // of `kFrameLength / (kChipPoints / 2)` samples, each chunk emitting its (min, max) pair.
        //
        // [B-10, owner ruling 2026-08-16.] Until then this was `frame[k · 32]` — every 32nd sample,
        // a decimator with NO band limit, so its effective Nyquist was harmonic 32 and any frame
        // whose content sat above that aliased. THIRTEEN of the 200 shipped banks did: `chip.and`'s
        // frame 0 has period exactly 32 samples, so the stride sampled one identical phase every
        // time and the chip flattened to a CONSTANT while the audio was perfect (peak 1.0, 2047
        // zero crossings). The obvious repair — band-limit before decimating, i.e. a box-mean over
        // each chunk — was MEASURED FIRST and is worse: a 32-sample box has its transfer zeros on
        // harmonics 64, 128, 192 …, exactly where those frames live, so it integrates `chip.and`'s
        // period away just as the stride sampled one phase of it. Box-mean fixed 3 of the 13, left
        // 10, and BROKE 5 banks that drew correctly (the chaotic maps: a broadband frame carries
        // almost no energy below harmonic 32, so averaging it converges to the frame mean). Net
        // 13 → 15 flat of 200.
        //
        // THE GENERALISATION IS WHY THE ENVELOPE IS RIGHT RATHER THAN MERELY BETTER: no
        // band-limiting law can draw a waveform whose content is all above the band, and that is
        // the whole class the thirteen belong to. An envelope answers a different question — not
        // "what is the curve at x" but "how far does it travel near x" — so it has no Nyquist to be
        // above. Measured 0 of 200 flat, including the four nothing else fixed.
        //
        // THE COST, stated because it is real: 32 abscissae rather than 64, and a smooth wave draws
        // as a thin band rather than a line. What is NOT paid is a format change — this is still
        // §2.3's 64 `int8` points, so `WavePreview`, the browser and the DS `canvas.segment` path
        // are untouched (the rejected alternative, a 2×64 envelope, would have moved all three).
        static void bakeEnvelope (const float* frame, std::int8_t* out) noexcept;

        // Which frame waterfall row `row` summarises, in a bank of `frameCount` frames. The rows
        // SPAN the bank inclusively — row 0 is frame 0 and the last row is the last frame — so a
        // pane cannot show the first 16 frames of a 32-frame bank and look plausible. Total: an
        // out-of-range row answers with the nearest endpoint.
        //
        // THE EMPTY-BANK GUARD IS `<= 0`, AND THE BATTERY IS WHY. It prevents `frameCount == 0`
        // answering `−1` on the last rows, i.e. a NEGATIVE frame index handed to a renderer — not a
        // division by zero (the divisor below is the constant ROW count). It was first written
        // `<= 1`, and the mutation to `< 1` SURVIVED: a ONE-frame bank needs no guard at all,
        // because `row · 0 / 15` is 0 and `frameCount − 1` is 0, so the arithmetic already answers
        // frame 0 on every row. That made the `1` decorative — a guard covering a case it was not
        // needed for, which reads as protection it does not provide (T-CORE-02). Narrowed to the
        // case that IS load-bearing, where the mutation to `< 0` dies on the last row of an empty
        // bank. Only the T-WT-05 importer can produce a bank that small (§2.3's factory floor is 16).
        [[nodiscard]] static constexpr int waterfallFrameFor (int row, int frameCount) noexcept
        {
            if (row <= 0 || frameCount <= 0)
                return 0;
            if (row >= kWaterfallFrames - 1)
                return frameCount - 1;
            return row * (frameCount - 1) / (kWaterfallFrames - 1);
        }

        // §2.3's chip preview: `kChipPoints` points of frame `chipFrame`, the representative frame
        // (`synth.osc{1,2}.pos` rests on frame 0, so that is what a slot chip should show).
        [[nodiscard]] static std::vector<std::int8_t> bakeChip (const RenderFrame& render, int chipFrame);

        // §2.3's waterfall preview: `kWaterfallFrames` rows × `kWaterfallPoints` points, the rows
        // spanning a `frameCount`-frame bank per `waterfallFrameFor`.
        [[nodiscard]] static std::vector<std::int8_t> bakeWaterfall (const RenderFrame& render, int frameCount);
    };
} // namespace nemesis::dsp
