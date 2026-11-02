#pragma once

// [T-PROTO-02] THE SCAN-ORDER LAW — one table, TWO inputs, and the second input is the finding.
//
// `family_overview.md` §2.3 (owner ruling 2026-08-03): ordering is a PARAMETER OVER ANY TABLE, not
// an attribute of some banks. A consumer exposes `Off · Binary · Gray · Centroid` — CATHODE's is
// `synth.osc{1,2}.scanOrder` — which permutes how POSITION traverses a table's frames.
//
// THE PART THAT IS EASY TO GET WRONG, AND THE REASON THIS IS A PUBLISHED LAW RATHER THAN THREE
// PERMUTATIONS AT THE CALL SITE: the answer depends on how the bank is ALREADY STORED. §2.3 says so
// in the same breath — "a recipe's ordering fixes how the frames are STORED, `scanOrder` permutes
// how a POSITION scan walks them" — and `WavetableRecipe::ordering` declares which of the three
// each bank took. All three are live on the shipped roster (177 Morph · 13 Brightness · 10 Gray — pinned by
// `core/tests/test_wavetable_ordering.cpp`).
//
// So `Gray` over a bank ALREADY laid out in Gray is the IDENTITY, not a Gray permutation.
// `MaskTableGenerator`'s own header states the consequence for the three COMPLEX banks, which are
// Gray-native (`renderFrame` puts mask `grayCode (f)` in slot `f`): "`Off` is already the musical
// traversal, `Gray` a no-op restatement of it, and Binary an explicit user choice." Reaching for
// `grayCode` unconditionally — which this card's own acceptance sketch said to do — gives those
// banks a DOUBLE Gray: a wrong permutation that is still a permutation, so every bijection test
// passes and nothing goes red.
//
//   requested \ stored |  Morph              Brightness              Gray
//   -------------------+---------------------------------------------------------
//   Off                |  i                  i                       i
//   Binary             |  i                  slotOfGeneration[i]     inverseGray(i)
//   Gray               |  grayCode(i)        slotOfGeneration[g(i)]  i
//   Centroid           |  slotByBrightness[i] i                      slotByBrightness[i]
//
// `Off` MEANS "AS STORED", so it coincides with a different one of the three on each kind of bank —
// with `Binary` on a Morph bank, with `Gray` on a COMPLEX one, with `Centroid` on a Brightness one.
// That is the model being correct, not a bug, and it is also exactly what a broken implementation
// looks like if you only ever probe ONE kind of bank. Hence the tests sweep all three.
//
// WIDTH: `log2 (frames)`, PER BANK (owner ruling 2026-08-28, amending this card's "4 bits, full
// stop"). The 2026-08-03 quantisation ruling protects `frames == 2^b` — every stop landing on a
// real frame rather than a blend — and that needs the width to FOLLOW the frame count, not to be
// pinned at 4. The shipped roster is 16 / 32 (the default) / 64, so b is 4, 5 or 6. What the
// original prohibition still binds is widening the permutation as a FIX for a bank whose frame
// count is not a power of two; that case falls back to the identity below.
//
// A header that holds a rule and no state — the `PageRegion.h` / `ShapeGlyph.h` / `EnumTileForm.h`
// precedent, included by whoever applies it. Nothing here allocates, locks or touches libm, so it
// is callable from the audio thread; the two lookup tables are built OFF it (see `ScanOrderTables`).

#include <nemesis/dsp/WavetableRecipe.h> // FrameOrdering — the STORED half of the table

#include <cstdint>

namespace nemesis::dsp
{
    // The param's roster, in registry order. The ordinal IS the contract: `synth.osc{1,2}.scanOrder`
    // is an Enum row whose labels are `{"Off", "Binary", "Gray", "Centroid"}`, and the apply row
    // casts its plain value straight to this. Spelled `= 0` upward so that pairing is visible here
    // rather than only in the registry (the T-CORE-23 rule — a positional currency that is not
    // written down is one nobody can check).
    enum class ScanOrder : std::uint8_t {
        Off = 0,
        Binary = 1,
        Gray = 2,
        Centroid = 3
    };

    inline constexpr int kScanOrderCount = 4;

    // ── The two bit laws, and they are INVERSES — proven, not named ────────────────────────────
    //
    // `grayCodeOf` is `MaskTableGenerator::grayCode` restated here rather than included, because
    // pulling that whole class in for one xor would make a leaf-level law depend on the mask
    // generator's entire surface. The copy is not left to agree by hope: the test asserts this
    // function equals `MaskTableGenerator::grayCode` over the full 9-bit space, so a change to
    // either goes red. `inverseGrayOf` is NEW here — it is what `Binary` over a Gray-native bank
    // needs, and nothing before this card had a reason to want it.
    [[nodiscard]] constexpr int grayCodeOf (int i) noexcept { return i ^ (i >> 1); }

    // The reflected Gray code's inverse — the xor-PREFIX-SCAN `i = g ^ (g>>1) ^ (g>>2) ^ …`, every
    // term, written as the plain shift-by-one loop.
    //
    // NOT the doubling form, and the first cut of this function was exactly that mistake: `i ^= (g
    // >> shift)` with `shift` doubling gives `g ^ (g>>1) ^ (g>>2) ^ (g>>4) ^ …` and silently drops
    // `g>>3`, `g>>5` and the rest. The doubling trick is real but it accumulates into `i` (`i ^= i
    // >> 1; i ^= i >> 2; i ^= i >> 4;`), where each round has already folded in the terms the next
    // one needs — reading it off as a shorter loop over `g` is the trap. It first disagrees at
    // g = 8, i.e. inside the 4-bit COMPLEX space this card is built for, and the round-trip test
    // caught it on the first run.
    [[nodiscard]] constexpr int inverseGrayOf (int g) noexcept
    {
        int i = 0;
        for (int v = g; v != 0; v >>= 1)
            i ^= v;
        return i;
    }

    // `frames == 2^b`, the property the 2026-08-03 ruling actually protects. Zero and negatives are
    // NOT powers of two here — a degenerate bank must reach the identity fallback, not a shift by a
    // nonsense width.
    [[nodiscard]] constexpr bool isPowerOfTwoFrames (int frames) noexcept
    {
        return frames > 0 && (frames & (frames - 1)) == 0;
    }

    // ── The per-bank lookup tables ──────────────────────────────────────────────────────────────
    //
    // Both are PERMUTATIONS OVER SLOTS of length `count`, and both are OPTIONAL: a null table means
    // "this bank has not been measured", and every cell that would have read it falls back to the
    // identity. That is a real runtime state and not an error — a bank is measured when it loads,
    // and a caller holding a recipe it has not loaded yet still has to be able to ask.
    //
    // `slotOfGeneration` is the INVERSE of `WavetableRecipe::frameOrder`, which is already baked
    // onto every Brightness recipe (`WavetableKernels::clampFrame` resolves slot → generation with
    // it). So the Brightness column costs an inversion of a table that exists, never an FFT.
    // `slotByBrightness` is the measured one, and it is needed ONLY for `Centroid` on a Morph or
    // Gray bank — the Brightness column already IS that order.
    struct ScanOrderTables
    {
        const int* slotOfGeneration { nullptr }; // Brightness banks: inverse of recipe.frameOrder
        const int* slotByBrightness { nullptr }; // Morph / Gray banks: ascending measured centroid
        int count { 0 };
    };

    namespace detail
    {
        // A table read that refuses rather than trusting: null table, wrong length or an index off
        // the end all fall back to the identity. `count` is checked against `frames` because the
        // two come from different places (the recipe and the loaded bank) and a stale table is the
        // failure this guard exists for — not a hypothetical, since a bank swap replaces one and
        // not the other.
        [[nodiscard]] inline int lookupOr (const int* table, int count, int frames, int i) noexcept
        {
            if (table == nullptr || count != frames || i < 0 || i >= frames)
                return i;
            const int v = table[i];
            return (v >= 0 && v < frames) ? v : i;
        }
    } // namespace detail

    // THE LAW. Maps a SCAN position to the bank's own SLOT index — the index
    // `WavetableKernels::clampFrame` then resolves to a generation frame, which is why nothing here
    // touches `frameOrder` directly for anything but its inverse.
    //
    // `scanIndex` is clamped into [0, frames). A non-power-of-two `frames` returns the identity for
    // every ordering: the permutations are bit laws over a 2^b space and there is no honest answer
    // outside one. **No such bank ships today** — the roster is 16 / 32 / 64 — so that arm is a
    // GUARD rather than a tested claim about content, and it is written here because T-WT-05's
    // import can bring one and the alternative is a shift by a fractional width. (The precedent is
    // `MaskTableGenerator::layoutMasks`, which falls back to Binary on an unaligned mask slice for
    // the same reason.)
    [[nodiscard]] inline int scanOrderSlot (ScanOrder requested,
        FrameOrdering stored,
        int frames,
        int scanIndex,
        const ScanOrderTables& tables) noexcept
    {
        if (frames <= 0)
            return 0;
        const int i = scanIndex < 0 ? 0 : (scanIndex >= frames ? frames - 1 : scanIndex);

        // `Off` is the EXACT identity and is answered before the power-of-two guard, because "as
        // stored" is meaningful on a bank of any size — the guard is about the bit laws, not about
        // the param.
        //
        // [leaf 6] A LABELLED BATTERY SURVIVOR, CLASSIFIED (c) UNPROVABLE BY CONSTRUCTION — and the
        // classification is EQUIVALENT MUTANT rather than a coverage hole, which is a different
        // animal from a weak test (T-UI-56's rule: say which of the two a pass is).
        //
        // Deleting this early-out cannot change an answer. Every `Off` arm of every row below also
        // returns `i`, and the power-of-two guard it jumps ahead of returns `i` too — so all three
        // paths agree for `Off` on every bank, by construction rather than by luck. What the line
        // buys is a STATEMENT of precedence: "as stored" is meaningful on a 12-frame bank, and
        // answering it here says so where a reader is looking, instead of leaving them to notice
        // that the fallback happens to be the same value.
        //
        // Kept and labelled rather than deleted, which is the opposite call to T-CORE-02's
        // dead-guard rule and for its stated exception: this is not a guard that fails to protect
        // anything, it is a guard whose protection is IDENTICAL to its neighbour's. Deleting it
        // would make the reachable behaviour depend on the bit guard for a param the bit guard has
        // nothing to do with. Re-run this mutation every round; if a future arm makes `Off` mean
        // something else on some row, this stops being equivalent and MUST go red.
        if (requested == ScanOrder::Off)
            return i;
        if (!isPowerOfTwoFrames (frames))
            return i;

        switch (stored)
        {
            case FrameOrdering::Morph:
                // Generation order IS the raw index order here, so `Binary` is the identity and
                // only `Gray` permutes. `Centroid` needs the measured table.
                switch (requested)
                {
                    case ScanOrder::Binary:
                        return i;
                    case ScanOrder::Gray:
                        return grayCodeOf (i);
                    case ScanOrder::Centroid:
                        return detail::lookupOr (tables.slotByBrightness, tables.count, frames, i);
                    case ScanOrder::Off:
                    default:
                        return i;
                }

            case FrameOrdering::Brightness:
                // Slots are ALREADY ascending in measured brightness, so `Centroid` is the identity
                // and the other two have to undo the sort first — `slotOfGeneration` is that undo.
                switch (requested)
                {
                    case ScanOrder::Binary:
                        return detail::lookupOr (tables.slotOfGeneration, tables.count, frames, i);
                    case ScanOrder::Gray:
                        return detail::lookupOr (tables.slotOfGeneration, tables.count, frames, grayCodeOf (i));
                    // `Centroid` shares the identity arm with `Off` rather than spelling its own,
                    // and the two labels together ARE the statement: on a Brightness bank the slots
                    // are already ascending in measured centroid, so "as stored" and "by brightness"
                    // are the same traversal. Written as one arm because two identical consecutive
                    // arms is a `bugprone-branch-clone` red, and the honest fix is to say they are
                    // the same rather than to make them look accidentally alike.
                    case ScanOrder::Centroid:
                    case ScanOrder::Off:
                    default:
                        return i;
                }

            case FrameOrdering::Gray:
                // Slot f already holds mask `grayCode (f)`, so `Gray` is the identity and `Binary`
                // is the INVERSE — the cell this card exists to get right.
                switch (requested)
                {
                    case ScanOrder::Binary:
                        return inverseGrayOf (i);
                    case ScanOrder::Gray:
                        return i;
                    case ScanOrder::Centroid:
                        return detail::lookupOr (tables.slotByBrightness, tables.count, frames, i);
                    case ScanOrder::Off:
                    default:
                        return i;
                }
        }
        return i;
    }

    // [T-CGUI-79 G2] THE POSITION LAW, ONCE: a [0,1] position over `numFrames` frames → the two
    // bracketing SLOTS (through the scan order, BOTH endpoints) and the blend fraction between them.
    // `WavetableOscillator::resolvePosition` (the sound) and `core::wavetableChipAtPosition` (the WT
    // figure) both call it, so the drawn frame cannot drift from the sounding one.
    //
    // `position01` is the caller's, already clamped: negatives are not re-clamped here (the float
    // → int truncation is the oscillator's original arithmetic, kept bit-exact). `Off` returns the
    // raw bracket without reading `tables`.
    struct ScanFrames
    {
        int f0 { 0 };
        int f1 { 0 };
        double frac { 0.0 };
    };

    [[nodiscard]] inline ScanFrames resolveScanFrames (double position01,
        int numFrames,
        ScanOrder requested,
        FrameOrdering stored,
        const ScanOrderTables& tables) noexcept
    {
        if (numFrames <= 0)
            return {};
        const double fp = position01 * static_cast<double> (numFrames - 1);
        int f0 = static_cast<int> (fp);
        if (f0 > numFrames - 1)
            f0 = numFrames - 1;
        ScanFrames r;
        r.frac = fp - static_cast<double> (f0);
        const int f1 = f0 + 1 < numFrames ? f0 + 1 : f0;
        if (requested == ScanOrder::Off)
        {
            r.f0 = f0;
            r.f1 = f1;
            return r;
        }
        r.f0 = scanOrderSlot (requested, stored, numFrames, f0, tables);
        r.f1 = scanOrderSlot (requested, stored, numFrames, f1, tables);
        return r;
    }

    // Whether a (requested, stored) pair needs `slotByBrightness` — i.e. whether the bank owes a
    // centroid MEASUREMENT at load. Published so leaf 2's loader asks the law rather than
    // re-deriving "Centroid, unless the bank is already Brightness-sorted", which is the kind of
    // restated condition that drifts.
    [[nodiscard]] constexpr bool scanOrderNeedsCentroidTable (ScanOrder requested, FrameOrdering stored) noexcept
    {
        return requested == ScanOrder::Centroid && stored != FrameOrdering::Brightness;
    }
} // namespace nemesis::dsp
