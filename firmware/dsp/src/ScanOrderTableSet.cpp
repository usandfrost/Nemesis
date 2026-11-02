#include <nemesis/dsp/ScanOrderTableSet.h>

#include <nemesis/dsp/MaskTableGenerator.h>

#include <algorithm>
#include <numeric>

namespace nemesis::dsp
{
    void ScanOrderTableSet::clear() noexcept
    {
        ordering_ = FrameOrdering::Morph;
        frames_ = 0;
        slotOfGeneration_.clear();
        slotByBrightness_.clear();
    }

    void ScanOrderTableSet::build (FrameOrdering stored,
        const std::uint8_t* frameOrder,
        const float* samples,
        int numFrames,
        int frameLength)
    {
        clear(); // REPLACE, never append — a bank swap must not leave the previous bank's tables
        if (numFrames <= 0)
            return;

        ordering_ = stored;
        frames_ = numFrames;

        // ── The INVERSE of the baked slot→generation table ──────────────────────────────────────
        //
        // Only a Brightness bank has one, and only its `Binary`/`Gray` cells read it. Built by
        // walking slots and writing each generation index's slot — `inv[gen[slot]] = slot` — which
        // is the inversion and also the check: `frameOrder` is a permutation by contract, so any
        // entry out of range or repeated leaves a hole, and a holed table is REFUSED WHOLE rather
        // than published half-right. A partial permutation would silently send some scan positions
        // to slot 0 and read as a bank with a stuck frame.
        if (stored == FrameOrdering::Brightness && frameOrder != nullptr)
        {
            constexpr int kUnset = -1;
            std::vector<int> inv (static_cast<std::size_t> (numFrames), kUnset);
            bool ok = true;
            for (int slot = 0; slot < numFrames && ok; ++slot)
            {
                const int gen = static_cast<int> (frameOrder[static_cast<std::size_t> (slot)]);
                if (gen < 0 || gen >= numFrames || inv[static_cast<std::size_t> (gen)] != kUnset)
                    ok = false;
                else
                    inv[static_cast<std::size_t> (gen)] = slot;
            }
            if (ok)
                slotOfGeneration_ = std::move (inv);
        }

        // ── The MEASURED ascending-centroid order ───────────────────────────────────────────────
        //
        // Owed only where the law says it is (`scanOrderNeedsCentroidTable`), so a Brightness bank
        // measures NOTHING — its slots already are this order, and that is most of what makes the
        // feature affordable. The predicate is asked rather than restated.
        if (!scanOrderNeedsCentroidTable (ScanOrder::Centroid, stored))
            return;
        if (samples == nullptr || frameLength <= 0)
            return;

        std::vector<double> centroid (static_cast<std::size_t> (numFrames), 0.0);
        for (int f = 0; f < numFrames; ++f)
            centroid[static_cast<std::size_t> (f)] = MaskTableGenerator::spectralCentroid (
                samples + static_cast<std::size_t> (f) * static_cast<std::size_t> (frameLength), frameLength);

        std::vector<int> order (static_cast<std::size_t> (numFrames));
        std::iota (order.begin(), order.end(), 0);
        // DETERMINISTIC TIE-BREAK ON THE SLOT INDEX. Two frames with an equal centroid are ordinary
        // — `Weave` is a pure phase permutation, so its frames share a magnitude spectrum EXACTLY
        // and every one of its centroids ties — and a bank whose scan order depended on the sort's
        // internal pivot choices would render differently between builds while every test passed.
        //
        // [T-PROTO-02 leaf 6] `stable_sort` ON TOP OF A TOTAL COMPARATOR, DELIBERATELY REDUNDANT —
        // and this comment is the record of a restructure that was TRIED, RUN TWICE, AND REVERTED,
        // because the reasoning behind it was falsified rather than merely unproven.
        //
        // THE ATTEMPT. The battery's design pass read this as two INDEPENDENTLY SUFFICIENT guards
        // (stability alone gives determinism; a total comparator alone gives determinism), which is
        // the accidental-redundancy shape T-B26 says to restructure rather than label. So it became
        // `sort` + the comparator as the single enforcement point, on the prediction that the tie
        // arm would then be killable. It was not: the mutation `return a < b;` -> `return false;`
        // SURVIVED round 1 and survived round 2 against a re-aimed probe.
        //
        // WHY IT CANNOT BE KILLED, which is the finding and is not about the probe. With the tie
        // arm intact the comparator is TOTAL, so exactly ONE permutation satisfies it and every
        // correct sort must produce that one. With the tie arm broken the comparator is STILL a
        // valid strict weak ordering — it just admits MANY valid answers, and the library picks one
        // by an unspecified rule. So the property the tie arm actually buys is not "this order" but
        // "this order INDEPENDENTLY OF THE SORT IMPLEMENTATION" — and a test that runs one
        // implementation cannot observe an implementation-independence claim. Round 1 blamed the
        // arity (libc++ insertion-sorts small ranges and is stable in fact) and round 2 blamed the
        // input shape (an ALL-ties bank cannot discriminate, since every permutation is validly
        // sorted); both were true and neither was the reason. This is T-B19's "label the surviving
        // mutation rather than adding a probe that only looks like one", reached the long way.
        //
        // AND THAT IS WHY THE REDUNDANCY STAYS. T-B26's rule is to restructure when the redundancy
        // is accidental — but its whole payoff is a mutation that goes red, and here no arrangement
        // of these two clauses produces one. With the redundancy unkillable either way, T-B19's
        // rule governs instead: keep the conjunction and DOCUMENT why it is not an oversight, or
        // someone restores it. Concretely: if a later card breaks the tie arm, `stable_sort` still
        // holds the order and nothing goes wrong; under `sort` the determinism would die silently
        // and no test in this repo would notice. Defence in depth beats minimalism exactly when the
        // battery has proved it cannot adjudicate.
        //
        // Two frames with an equal centroid are ordinary, not pathological: `Weave` is a pure phase
        // permutation, so its frames share a magnitude spectrum EXACTLY and every centroid ties. A
        // bank whose scan order depended on the sort's internal pivot choices would render
        // differently between builds while every test here passed.
        std::stable_sort (order.begin(), order.end(), [&centroid] (int a, int b) {
            const double ca = centroid[static_cast<std::size_t> (a)];
            const double cb = centroid[static_cast<std::size_t> (b)];
            if (ca < cb)
                return true;
            if (cb < ca)
                return false;
            return a < b;
        });
        slotByBrightness_ = std::move (order);
    }

    ScanOrderTables ScanOrderTableSet::view() const noexcept
    {
        ScanOrderTables t {};
        t.slotOfGeneration = slotOfGeneration_.empty() ? nullptr : slotOfGeneration_.data();
        t.slotByBrightness = slotByBrightness_.empty() ? nullptr : slotByBrightness_.data();
        t.count = frames_;
        return t;
    }
} // namespace nemesis::dsp
