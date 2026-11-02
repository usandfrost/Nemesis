#include <nemesis/dsp/WavetableBank.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace nemesis::dsp
{
    std::int8_t WavetableBank::quantisePoint (double v) noexcept
    {
        const double clamped = std::clamp (v, -1.0, 1.0);
        return static_cast<std::int8_t> (std::lround (clamped * 127.0));
    }

    void WavetableBank::bakeEnvelope (const float* frame, std::int8_t* out) noexcept
    {
        // §2.3's `kChipPoints` points as `kChipPoints / 2` (min, max) PAIRS — see the header for why
        // the pair, and B-10 for the measurement that rejected every point-sampling alternative.
        constexpr int chunks = kChipPoints / 2;
        constexpr int chunkLen = kFrameLength / chunks;

        for (int c = 0; c < chunks; ++c)
        {
            // Seeded from the chunk's FIRST sample rather than from 0 — a ±0 seed would report an
            // excursion no sample reached for any chunk that sits entirely on one side of zero,
            // which is most of them for an asymmetric wave.
            const auto* first = frame + static_cast<std::ptrdiff_t> (c) * chunkLen;
            double lo = static_cast<double> (*first);
            double hi = lo;
            for (int j = 1; j < chunkLen; ++j)
            {
                const auto v = static_cast<double> (first[j]);
                lo = std::min (lo, v);
                hi = std::max (hi, v);
            }
            // MIN FIRST, then MAX. A fixed order (rather than the order the two extremes occur in)
            // keeps the law a pure function of the chunk's VALUE SET, so the polyline a consumer
            // draws is reproducible without the bake also publishing where in the chunk each
            // extreme fell — and it is the order the owner's B-10 ruling names.
            out[static_cast<std::size_t> (2 * c)] = quantisePoint (lo);
            out[static_cast<std::size_t> (2 * c + 1)] = quantisePoint (hi);
        }
    }

    std::vector<std::int8_t> WavetableBank::bakeChip (const RenderFrame& render, int chipFrame)
    {
        // ONE frame is rendered, never the bank: §2.3 has the catalogue baking previews for every
        // bank at first use while frames stay lazy, so a chip that cost a whole bank would make
        // the catalogue's own construction the expensive thing it exists to avoid.
        std::vector<float> frame (static_cast<std::size_t> (kFrameLength));
        render (chipFrame, frame.data());

        std::vector<std::int8_t> chip (static_cast<std::size_t> (kChipPoints));
        bakeEnvelope (frame.data(), chip.data());
        return chip;
    }

    std::vector<std::int8_t> WavetableBank::bakeWaterfall (const RenderFrame& render, int frameCount)
    {
        // The same law, per ROW — a waterfall row is one frame drawn at the chip's own width
        // (`kWaterfallPoints == kChipPoints`), which is what makes row 0 and the chip identical and
        // is asserted as such. Sharing `bakeEnvelope` is what keeps that true by construction: the
        // two bakes were separate copies of the stride law and drifting them apart would show up
        // three layers away as a pane whose top row disagrees with the slot chip it opened from.
        static_assert (kWaterfallPoints == kChipPoints,
            "the waterfall's row width IS the chip's width — bakeEnvelope bakes both");

        std::vector<float> frame (static_cast<std::size_t> (kFrameLength));
        std::vector<std::int8_t> fall (
            static_cast<std::size_t> (kWaterfallFrames) * static_cast<std::size_t> (kWaterfallPoints));

        for (int row = 0; row < kWaterfallFrames; ++row)
        {
            render (waterfallFrameFor (row, frameCount), frame.data());
            bakeEnvelope (frame.data(),
                fall.data() + static_cast<std::ptrdiff_t> (row) * static_cast<std::ptrdiff_t> (kWaterfallPoints));
        }
        return fall;
    }
} // namespace nemesis::dsp
