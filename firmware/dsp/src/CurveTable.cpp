#include <nemesis/dsp/CurveTable.h>

#include <algorithm>
#include <cmath>

namespace nemesis::dsp
{
    double curveEase (double t, int curve) noexcept
    {
        // A COPY of `core::autoEase` (core/src/AutoLane.cpp), statement for statement: the same
        // endpoint guards BEFORE the pow, the same int clamp to ±100 divided by 100.0, the same two
        // pow arms. Any edit here is an edit there too — the later cross-layer leaf pins the two.
        if (t <= 0.0)
            return 0.0;
        if (t >= 1.0)
            return 1.0;
        const double c = std::clamp (curve, -100, 100) / 100.0;
        if (c >= 0.0)
            return std::pow (t, 1.0 + 3.0 * c);
        return 1.0 - std::pow (1.0 - t, 1.0 - 3.0 * c);
    }

    bool CurveTable::bake (const CurvePoint* points, std::size_t count) noexcept
    {
        // Validate EVERYTHING before the first write, so a refusal leaves the playing table intact.
        // Each test is phrased so a NaN pos or val fails it.
        if (points == nullptr || count < 2 || count > kMaxCurvePoints)
            return false;
        if (!(points[0].pos == 0.0 && points[count - 1].pos == 1.0))
            return false;
        for (std::size_t k = 0; k < count; ++k)
        {
            if (!(points[k].val >= 0.0 && points[k].val <= 1.0))
                return false;
            if (k > 0 && !(points[k].pos >= points[k - 1].pos))
                return false;
        }

        std::size_t seg = 0;
        for (std::size_t i = 0; i < kCurveTableSize; ++i)
        {
            const double x = static_cast<double> (i) / static_cast<double> (kCurveTableSize);
            // The LAST point with pos ≤ x starts the segment (core's drawSegAt rule). x < 1 == the
            // last pos, so this stops at count − 2 at the latest, and b.pos > x ≥ a.pos → span > 0.
            while (points[seg + 1].pos <= x)
                ++seg;
            const CurvePoint& a = points[seg];
            const CurvePoint& b = points[seg + 1];
            const double t = (x - a.pos) / (b.pos - a.pos);
            table_[i] = a.val + (b.val - a.val) * curveEase (t, a.curve);
        }
        table_[kCurveTableSize] = points[count - 1].val; // x == 1: the last point, exactly

        // [T-CGUI-82] THE KNOTS: both points of every segment narrower than one cell, in point order
        // (so pos-sorted, a tie's points in drawn order — the later one wins from its pos on). A knot
        // at pos 0 has no cell left of it and is dropped. Cleared first: a re-bake starts from none.
        knotPos_.fill (0.0);
        knotVal_.fill (0.0);
        knotCells_.fill (0U);
        knotCount_ = 0;
        constexpr double kCell = 1.0 / static_cast<double> (kCurveTableSize);
        for (std::size_t k = 0; k < count; ++k)
        {
            const bool narrowLeft = k > 0 && points[k].pos - points[k - 1].pos < kCell;
            const bool narrowRight = k + 1 < count && points[k + 1].pos - points[k].pos < kCell;
            if (!(narrowLeft || narrowRight) || !(points[k].pos > 0.0))
                continue;
            // The cell whose (start, end] holds pos: ceil(pos·N) − 1 (pos·N is exact, N a power of two).
            const double x = points[k].pos * static_cast<double> (kCurveTableSize);
            auto cell = static_cast<std::size_t> (x);
            if (static_cast<double> (cell) == x)
                --cell; // on a grid line: the knot ends the cell to its left
            knotPos_[knotCount_] = points[k].pos;
            knotVal_[knotCount_] = points[k].val;
            ++knotCount_; // ≤ count ≤ kCurveKnotCapacity (static_assert in the header)
            knotCells_[cell >> 6U] |= std::uint64_t { 1 } << (cell & 63U);
        }
        baked_ = true;
        return true;
    }

    bool CurveTable::operator== (const CurveTable& other) const noexcept
    {
        // Out of line: the double compares live in this layer lib (the -Wfloat-equal rule DrawCurve.cpp states).
        return baked_ == other.baked_ && table_ == other.table_ && knotCount_ == other.knotCount_
               && knotPos_ == other.knotPos_ && knotVal_ == other.knotVal_ && knotCells_ == other.knotCells_;
    }
}
