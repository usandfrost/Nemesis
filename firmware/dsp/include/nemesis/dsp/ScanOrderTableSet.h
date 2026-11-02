#pragma once

// [T-PROTO-02 leaf 2] THE TWO PERMUTATION TABLES `ScanOrder.h`'s law reads, and where they come
// from — which is the whole of this card's cost question.
//
// `scanOrderSlot` is a pure O(1) table read, callable from the audio thread. Two of its twelve
// cells need per-bank data, and the two are NOT the same kind of thing:
//
//   * `slotOfGeneration` — the INVERSE of `WavetableRecipe::frameOrder`, the slot→generation table
//     already BAKED onto every `FrameOrdering::Brightness` recipe since T-WT-14 (see
//     `WavetableKernels::clampFrame`, which resolves a slot with it, and the suite case that
//     recomputes it live and requires a match). Inverting an existing table is arithmetic — no
//     FFT, no frames read at all.
//   * `slotByBrightness` — the MEASURED one: ascending spectral centroid over the bank's own
//     frames. Needed only for `Centroid` on a `Morph` or `Gray` bank, because a `Brightness` bank's
//     slots ARE that order already. `scanOrderNeedsCentroidTable` is the predicate; this class asks
//     it rather than restating "Centroid unless Brightness", which is the kind of duplicated
//     condition that drifts.
//
// THE PLAN GATE HAD THIS MORE EXPENSIVE THAN IT IS, and the correction is recorded on the card
// (R3): it implied all three non-identity columns owed a measurement. Only one does, and only on
// two of the three stored orderings.
//
// NOT RT-SAFE — `build` allocates and, when a centroid table is owed, runs an FFT per frame
// (`MaskTableGenerator::spectralCentroid`). It belongs where `Wavetable::build` already belongs:
// off the audio thread, once per bank load. Reading the built tables through `view()` is what the
// hot path does, and that is a pointer hand-off with no work in it.

#include <nemesis/dsp/ScanOrder.h>
#include <nemesis/dsp/WavetableRecipe.h>

#include <cstdint>
#include <vector>

namespace nemesis::dsp
{
    class ScanOrderTableSet
    {
    public:
        // Builds whatever THIS bank owes and nothing else.
        //
        // `frameOrder` is the recipe's baked slot→generation table, or nullptr for a bank that has
        // none; it is read only when `stored == FrameOrdering::Brightness`. `samples` is the bank's
        // source frames laid out contiguously, exactly as `Wavetable::build` takes them — read only
        // during this call, and only when a centroid measurement is owed.
        //
        // Safe to call again: it REPLACES, so a bank swap cannot leave half of the previous bank's
        // tables in place (the `AdvancedGrid::generate` rule — replace, never append).
        void build (FrameOrdering stored,
            const std::uint8_t* frameOrder,
            const float* samples,
            int numFrames,
            int frameLength);

        // Drops both tables and returns to the "unmeasured" state, where every cell that would read
        // one falls back to the identity. What an unbound oscillator holds.
        void clear() noexcept;

        // The hot-path view. A table this bank does not owe comes back null, which the law already
        // treats as "unmeasured → identity" — so a Brightness bank asking for `Centroid` and a
        // Morph bank that has not been measured take the SAME path, and neither is an error.
        [[nodiscard]] ScanOrderTables view() const noexcept;

        [[nodiscard]] FrameOrdering ordering() const noexcept { return ordering_; }
        [[nodiscard]] int frameCount() const noexcept { return frames_; }

        // Whether this set actually measured centroids. Published for the tests and for a caller
        // that wants to know whether a `Centroid` request on this bank will do anything — a
        // question with a real answer (a Brightness bank never measures and never needs to).
        [[nodiscard]] bool hasCentroidTable() const noexcept { return !slotByBrightness_.empty(); }
        [[nodiscard]] bool hasGenerationTable() const noexcept { return !slotOfGeneration_.empty(); }

    private:
        FrameOrdering ordering_ { FrameOrdering::Morph };
        int frames_ { 0 };
        std::vector<int> slotOfGeneration_;
        std::vector<int> slotByBrightness_;
    };
} // namespace nemesis::dsp
