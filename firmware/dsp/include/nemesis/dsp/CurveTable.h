#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nemesis::dsp
{
    // [T-CGUI-61 leaf E1] Breakpoint cap and table resolution for a DRAWN curve.
    inline constexpr std::size_t kMaxCurvePoints = 32;
    inline constexpr std::size_t kCurveTableSize = 1024;
    // [T-CGUI-82] Every point can be a knot, so the knot list can never overflow (static_assert below).
    inline constexpr std::size_t kCurveKnotCapacity = kMaxCurvePoints;

    // One breakpoint of a drawn curve — the `core::AutoPoint` semantics in unit form: `pos` 0–1,
    // `val` 0–1 (core's is 0–127), `curve` −100 … +100 bending the segment STARTING at this point.
    struct CurvePoint
    {
        double pos = 0.0;
        double val = 0.0;
        int curve = 0;
    };

    // The Draw easing law, COPIED bit-for-bit from `core::autoEase` (core/src/AutoLane.cpp), which
    // this layer cannot include: `bt = c≥0 ? t^(1+3c) : 1−(1−t)^(1−3c)`, `c = clamp(curve,±100)/100`,
    // both endpoints held EXACTLY for every curve, guarded before the pow. Calls libm — NOT RT.
    // A later leaf owes the cross-layer test that pins this against `core::autoEase`.
    [[nodiscard]] double curveEase (double t, int curve) noexcept;

    // CurveTable — a drawn curve BAKED to a fixed table so the audio thread never evaluates the pow.
    // bake() is message-thread only (libm); the readers are RT-safe: no allocation, no libm, a
    // linear interpolation between kCurveTableSize + 1 samples (a guard sample at pos 1).
    //
    // Sample i holds the curve at x = i / kCurveTableSize (exact: the size is a power of two), by
    // the same segment rule as core's `drawUnit`: the segment is the LAST point with pos ≤ x, and
    // `val = v0 + (v1 − v0)·ease((x − p0)/(p1 − p0), curve0)`. So a breakpoint whose pos is on the
    // grid reads back its val EXACTLY; one off the grid reads the interpolation of its neighbours.
    // A default-constructed or never-successfully-baked table reads 0 everywhere.
    //
    // [T-CGUI-82] DISCONTINUITY-AWARE: a segment NARROWER THAN ONE CELL (a tie, or the 1-ULP pair the
    // model makes of a vertical drag) cannot be represented by the grid, so bake() records both its
    // points as KNOTS (exact pos, val). A cell holding a knot reads piecewise-linearly through
    // (cell start, sample) → its knots → (cell end, sample): a step is an instant jump at its exact
    // pos at any read rate (left of it the earlier point's val, from it on the later one's). A cell
    // holding no knot reads the plain interpolation, bit for bit — and a curve with no narrow
    // segment has no knot at all. The samples themselves are unchanged by this.
    class CurveTable
    {
    public:
        // Bakes `count` points. REFUSES (returns false, table and baked() unchanged) unless
        // 2 ≤ count ≤ kMaxCurvePoints, points[0].pos == 0, points[count−1].pos == 1, pos is
        // non-decreasing, and every val lies in [0, 1] — a >32-point list is refused, never
        // truncated, so the curve that plays is always the whole curve that was drawn. Equal
        // neighbouring positions are a vertical STEP (the later point wins at that x). NOT RT.
        bool bake (const CurvePoint* points, std::size_t count) noexcept;

        [[nodiscard]] bool baked() const noexcept { return baked_; }

        // CYCLIC reader (the LFO): `phase01` is wrapped into [0, 1) — the value at 1 is the value
        // at 0. A non-finite or astronomically large phase (|p| ≥ 1e15) reads phase 0. RT-safe.
        [[nodiscard]] double valueAt (double phase01) const noexcept
        {
            double p = phase01;
            if (!(p >= 0.0 && p < 1.0)) // the LFO's own phase never enters: it is already in [0, 1)
            {
                if (!(p > -kWrapLimit && p < kWrapLimit)) // NaN fails both
                    return table_[0];
                p -= static_cast<double> (static_cast<long long> (p)); // fractional part, no libm
                if (p < 0.0)
                    p += 1.0;
                if (p >= 1.0) // −tiny + 1.0 rounds up to 1.0
                    p = 0.0;
            }
            return read (p);
        }

        // NON-CYCLIC reader (the drawn envelope and its loop markers): `pos01` is CLAMPED to
        // [0, 1], so pos 1 reads the last point's val exactly. NaN reads pos 0. RT-safe.
        [[nodiscard]] double valueAtPos (double pos01) const noexcept
        {
            return read (pos01 > 0.0 ? (pos01 < 1.0 ? pos01 : 1.0) : 0.0); // NaN fails `> 0` → 0
        }

        // The raw samples, for tests and a figure that wants the baked shape.
        [[nodiscard]] const std::array<double, kCurveTableSize + 1>& samples() const noexcept { return table_; }

        // [T-CGUI-82] How many knots the last successful bake recorded (0 for a curve with no
        // sub-cell segment). Two tables can share every sample and still differ in a knot, so
        // "plays the same" is operator==, never a samples() compare.
        [[nodiscard]] std::size_t knotCount() const noexcept { return knotCount_; }
        [[nodiscard]] bool operator== (const CurveTable& other) const noexcept;
        [[nodiscard]] bool operator!= (const CurveTable& other) const noexcept { return !(*this == other); }

    private:
        // The double → long long truncation in valueAt is defined well inside this bound; a phase
        // this large carries no meaningful cycle position (and NaN none at all), so it reads 0.
        static constexpr double kWrapLimit = 1.0e15;

        // Linear interpolation at p ∈ [0, 1]. Only p == 1 lands on the guard sample itself, which
        // has no right-hand neighbour to blend with. A knot cell takes readKnotCell instead.
        [[nodiscard]] double read (double p) const noexcept
        {
            const double x = p * static_cast<double> (kCurveTableSize);
            const auto i = static_cast<std::size_t> (x);
            if (i >= kCurveTableSize)
                return table_[kCurveTableSize];
            const double f = x - static_cast<double> (i);
            if (((knotCells_[i >> 6U] >> (i & 63U)) & 1U) != 0U)
                return readKnotCell (i, f);
            return table_[i] + (table_[i + 1] - table_[i]) * f;
        }

        // Cell i holds the knots whose pos·N lies in (i, i + 1]; `f` ∈ [0, 1) is the read's offset in
        // the cell. Every denominator is > 0: a knot is taken as the left end only when u ≤ f < 1,
        // and the next one is used only when it lies past f. Positions in cell units are exact (N is
        // a power of two). Divisions only — RT-safe.
        [[nodiscard]] double readKnotCell (std::size_t i, double f) const noexcept
        {
            const double cell = static_cast<double> (i);
            double u0 = 0.0;
            double v0 = table_[i];
            for (std::size_t k = 0; k < knotCount_; ++k)
            {
                const double u = knotPos_[k] * static_cast<double> (kCurveTableSize) - cell;
                if (u <= 0.0)
                    continue; // an earlier cell's knot
                if (u > 1.0)
                    break; // the knots are pos-sorted: the rest are later cells'
                if (u <= f)
                {
                    u0 = u; // a tie: the LATER point wins from its pos on
                    v0 = knotVal_[k];
                    continue;
                }
                return v0 + (knotVal_[k] - v0) * ((f - u0) / (u - u0));
            }
            return v0 + (table_[i + 1] - v0) * ((f - u0) / (1.0 - u0));
        }

        std::array<double, kCurveTableSize + 1> table_ {};
        // [T-CGUI-82] The knots, pos-sorted, unused entries 0; one bit per cell that holds one.
        std::array<double, kCurveKnotCapacity> knotPos_ {};
        std::array<double, kCurveKnotCapacity> knotVal_ {};
        std::array<std::uint64_t, kCurveTableSize / 64> knotCells_ {};
        std::size_t knotCount_ = 0;
        bool baked_ = false;
    };
    static_assert (kCurveKnotCapacity >= kMaxCurvePoints, "every point of a maximal curve may be a knot");
    static_assert (kCurveTableSize % 64 == 0, "the knot-cell bitmap is whole 64-bit words");
}
