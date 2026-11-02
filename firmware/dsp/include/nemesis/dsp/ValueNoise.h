#pragma once

#include <nemesis/dsp/Real.h>
#include <cstdint>

namespace nemesis::dsp
{
    // [T-CMP-08 leaf C] A seeded low-frequency "VALUE NOISE": random targets in [-1, 1)
    // linearly interpolated across `seg` samples (rate = fs/seg Hz). Continuous (C0),
    // bounded in [-1, 1) by construction (a lerp of two in-range points stays in range),
    // and pure integer/float arithmetic — no libm even at control rate, so this is
    // header-only and callable from the hot path.
    //
    // It was `ConverterDrift`'s own private nested class (M1-09xb) and is LIFTED here
    // unchanged, to the line, because there is now a second consumer: this leaf's Analog
    // delay needs it for §2.1 clock drift ("slow random modulation of fc"), and leaf D's
    // Tape needs it for wow/flutter. The lift is behaviour-preserving by construction —
    // `ConverterDrift`'s committed golden is the proof, and it did not move.
    //
    // DELIBERATELY A PLAIN STRUCT with public fields, which is how `ConverterDrift`
    // already drove it: a consumer sets `seed` and `seg` and calls `reseed()`. One rate
    // per instance is the whole primitive; a consumer wanting wow + flutter sums TWO
    // instances at different rates with weights that sum to 1 (the `ConverterDrift`
    // idiom), which keeps the sum bounded without this class knowing about it.
    //
    // Determinism is load-bearing (§11 bounce parity): same `seed` → bit-identical
    // stream. The owning primitive's `reset()` calls `reseed()`.
    struct ValueNoise
    {
        std::uint32_t seed = 0;
        std::uint32_t state = 0;
        int seg = 1;
        int pos = 0;
        Real v0 = 0.0, v1 = 0.0;

        // Deterministic LCG in [-1, 1) (same generator as BbdDelayLine).
        static Real lcg (std::uint32_t& s) noexcept
        {
            s = s * 1664525u + 1013904223u;
            return static_cast<Real> (static_cast<std::int32_t> (s)) * (1.0 / 2147483648.0);
        }

        // Segment length in samples for a modulator rate in Hz. Truncation is fine — a
        // ±1-sample difference in a multi-thousand-sample wow segment is inaudible and
        // keeps this cmath-free. Was `ConverterDrift::segFromRate`; every consumer needs
        // it, so it travels with the noise rather than being copied per consumer.
        static int segFromRate (Real fs, Real rateHz) noexcept
        {
            const int s = static_cast<int> (fs / rateHz);
            return s < 1 ? 1 : s;
        }

        void reseed() noexcept
        {
            state = seed;
            pos = 0;
            v0 = lcg (state);
            v1 = lcg (state);
        }

        Real next() noexcept
        {
            const Real t = static_cast<Real> (pos) / static_cast<Real> (seg);
            const Real out = v0 + (v1 - v0) * t;
            if (++pos >= seg)
            {
                pos = 0;
                v0 = v1;
                v1 = lcg (state);
            }
            return out;
        }
    };
}
