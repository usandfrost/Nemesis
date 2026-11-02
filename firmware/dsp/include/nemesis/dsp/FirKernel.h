#pragma once

#include <cmath>

// [T-CMP-37 L2] THE OVERSAMPLER'S FIR ROUNDING, PINNED (owner 2026-10-23). One definition for the scalar `Oversampler` and the
// lane-wise `OversamplerLanes`, so the two cannot drift and the rounding no longer depends on the optimisation level.
//
// A serial reduction acc = sum_j c[j] * x[-j] in j order, where taps [0, unfusedEnd (taps)) round twice (product, then sum)
// and the remaining taps fuse (one rounding). That is what the shipping build (-O3 -mcpu=apple-m1, -ffp-contract=on) made
// of the plain loop before this pin: the vectoriser computed the products as `fmul.2d` and added them IN ORDER, eight per
// iteration, and its scalar remainder loop contracted to `fmadd` (perf-budgets.md §3k). Pinning reproduces it exactly, so
// release output does not move; the debug build, which fused every tap, moves at the last double ulp.
namespace nemesis::dsp::fir
{
    // The block the release vectoriser added in order (eight products per iteration).
    inline constexpr int kUnfusedBlock = 8;

    // Taps before this index round twice; the rest fuse. For every design here (taps = 17 or 16L+1) only the last tap fuses.
    [[nodiscard]] constexpr int unfusedEnd (int taps) noexcept { return taps - taps % kUnfusedBlock; }

    // sum_j c[j] * newest[-j], j = 0..taps-1, rounded as above.
    [[nodiscard]] inline double dot (const double* c, const double* newest, int taps) noexcept
    {
        double acc = 0.0;
        const int u = unfusedEnd (taps);
        {
#pragma clang fp contract(off)
            for (int j = 0; j < u; ++j)
                acc = acc + c[j] * newest[-j];
        }
        for (int j = u; j < taps; ++j)
            acc = std::fma (c[j], newest[-j], acc);
        return acc;
    }

    // FIREFORK: single-precision twin for Real = float. Every tap fuses (one VFMA.F32 each on the M7); bit-exactness with
    // a double reference is not a goal in float builds. The double overload above stays the pinned reference.
    [[nodiscard]] inline float dot (const float* c, const float* newest, int taps) noexcept
    {
        float acc = 0.0f;
        for (int j = 0; j < taps; ++j)
            acc = std::fma (c[j], newest[-j], acc);
        return acc;
    }
}
