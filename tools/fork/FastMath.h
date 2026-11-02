#pragma once

// FIREFORK: single-precision exp2 / log2 / tan / log1p / expm1 for the Cortex-M7, behind NEMESIS_DSP_FAST_MATH.
//
// Why: newlib's float libm calls cost tens to hundreds of cycles each, and the CATHODE render path makes them per
// sample when a modulation destination is live (pitch, cutoff, rate, reso), when the filter cutoff moves (tan), and
// in the companding stage (log1p / expm1). These replacements are a few multiplies and one divide.
//
// Accuracy (measured by bench/fastmath_test.cpp, relative error unless noted):
//   exp2   <= 1e-6      log2  <= 3e-7 absolute      tan  <= 3e-7      log1p / expm1: see comments below
// All are plain Taylor / atanh series with range reduction, so the error is known analytically, not fitted.
//
// Use through nemesis::dsp::fm::exp2() etc. With NEMESIS_DSP_FAST_MATH undefined (the default, and always when Real is
// double) these forward to std:: and the output is bit-identical to upstream.

#include <nemesis/dsp/Real.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace nemesis::dsp::fm
{
#if defined(NEMESIS_DSP_FAST_MATH) && defined(NEMESIS_DSP_FLOAT)

    namespace detail
    {
        inline float bitsToFloat (std::uint32_t u) noexcept
        {
            float f;
            std::memcpy (&f, &u, sizeof f);
            return f;
        }
        inline std::uint32_t floatToBits (float f) noexcept
        {
            std::uint32_t u;
            std::memcpy (&u, &f, sizeof u);
            return u;
        }
    }

    // 2^x. x is clamped to [-126, 126]. x = n + f with n = round(x), f in [-0.5, 0.5]; e^(f ln2) by a degree-7
    // Taylor series (truncation error z^8/8! with |z| <= 0.3466 -> 5e-9 relative), then scaled by 2^n exactly.
    inline float exp2 (float x) noexcept
    {
        x = x < -126.0f ? -126.0f : (x > 126.0f ? 126.0f : x);
        const float r = x + (x >= 0.0f ? 0.5f : -0.5f);
        const int n = static_cast<int> (r); // truncates toward zero == round half away from zero for x
        const float z = (x - static_cast<float> (n)) * 0.69314718056f;
        float p = 1.0f / 5040.0f;
        p = p * z + 1.0f / 720.0f;
        p = p * z + 1.0f / 120.0f;
        p = p * z + 1.0f / 24.0f;
        p = p * z + 1.0f / 6.0f;
        p = p * z + 0.5f;
        p = p * z + 1.0f;
        p = p * z + 1.0f;
        return p * detail::bitsToFloat (static_cast<std::uint32_t> (n + 127) << 23);
    }

    // log2(x) for x > 0 (x is clamped to >= 1e-30 so a zero or denormal cannot return garbage).
    // x = m * 2^e with m in [sqrt(1/2), sqrt(2)); ln(m) = 2 atanh(s), s = (m-1)/(m+1), |s| <= 0.1716; the series is
    // cut after s^7, truncation error 2 s^9 / 9 / ln2 = 3e-8.
    inline float log2 (float x) noexcept
    {
        x = x < 1.0e-30f ? 1.0e-30f : x;
        std::uint32_t i = detail::floatToBits (x);
        int e = static_cast<int> ((i >> 23) & 0xFFu) - 127;
        float m = detail::bitsToFloat ((i & 0x007FFFFFu) | 0x3F800000u); // [1, 2)
        if (m > 1.41421356f)
        {
            m *= 0.5f;
            ++e;
        }
        const float s = (m - 1.0f) / (m + 1.0f);
        const float s2 = s * s;
        float p = 0.41219858f;
        p = p * s2 + 0.57707802f;
        p = p * s2 + 0.96179669f;
        p = p * s2 + 2.88539008f;
        return static_cast<float> (e) + s * p;
    }

    // tan(x) for the filter prewarp: x in (0, pi/2) (negative x is mirrored). For x > pi/4 uses 1 / tan(pi/2 - x), so
    // sin and cos are only ever evaluated on [0, pi/4], where Taylor to x^9 / x^10 has error < 3e-8.
    inline float tan (float x) noexcept
    {
        const float ax = x < 0.0f ? -x : x;
        // pi/2 as hi + lo: a single float constant is off by 3.6e-8, which is 1e-6 relative once ax is within 0.03 of
        // the pole (the prewarp can reach 0.49 pi). ax is exact near pi/2 (Sterbenz), so hi - ax loses nothing.
        const float halfPiHi = 1.57079637050628662109375f;
        const float halfPiLo = -4.37113882e-8f;
        const bool flip = ax > 0.78539816340f;
        const float y = flip ? (halfPiHi - ax) + halfPiLo : ax;
        const float y2 = y * y;
        float s = 1.0f / 362880.0f;
        s = s * y2 - 1.0f / 5040.0f;
        s = s * y2 + 1.0f / 120.0f;
        s = s * y2 - 1.0f / 6.0f;
        s = s * y2 + 1.0f;
        s *= y;
        float c = -1.0f / 3628800.0f;
        c = c * y2 + 1.0f / 40320.0f;
        c = c * y2 - 1.0f / 720.0f;
        c = c * y2 + 1.0f / 24.0f;
        c = c * y2 - 0.5f;
        c = c * y2 + 1.0f;
        const float t = flip ? c / s : s / c;
        return x < 0.0f ? -t : t;
    }

    // ln(1 + u) and e^v - 1 via the base-2 routines. Absolute error is ~1e-7 (not relative): for |u| below ~1e-6 the
    // true log1p is far more accurate than 1 + u can be in float. Used by the mu-law compander, where u = mu |x| and
    // the result feeds a quantiser, so an absolute 1e-7 is below the converter's own resolution.
    inline float log1p (float u) noexcept { return 0.69314718056f * fm::log2 (1.0f + u); }
    inline float expm1 (float v) noexcept { return fm::exp2 (v * 1.44269504089f) - 1.0f; }

#else

    inline Real exp2 (Real x) noexcept { return std::exp2 (x); }
    inline Real log2 (Real x) noexcept { return std::log2 (x); }
    inline Real tan (Real x) noexcept { return std::tan (x); }
    inline Real log1p (Real x) noexcept { return std::log1p (x); }
    inline Real expm1 (Real x) noexcept { return std::expm1 (x); }

#endif
}
