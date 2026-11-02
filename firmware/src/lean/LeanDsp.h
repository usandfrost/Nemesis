#ifndef __LEAN_LEAN_DSP_H
#define __LEAN_LEAN_DSP_H

// Primitives of the lean voice (docs/LEAN_VOICE.md): float only, no oversampling, written from public techniques (Simper's TPT state-variable
// filter, Zavalishin's zero-delay ladder, phase-accumulator table reads, one-pole envelopes). Everything in a block loop is plain arithmetic: any
// transcendental function is made once per control block, never per sample, and through the short polynomials below (fitted for this file: a libm
// call is 100-250 cycles on the Cortex-M7, and a voice made about ten of them per block).
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <nemesis/dsp/WavetableOscillator.h>

namespace lean {

constexpr int kCtl = 16;             // samples per control block: modulation, envelopes' targets and filter coefficients move at this rate
constexpr float kPi = 3.14159265358979f;

inline float fclamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// tanh as a rational: exact at 0, odd, error below 0.4% over the range the filters use, hard-limited beyond +-3
inline float tanhFast(float x) {
    x = fclamp(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Short-polynomial stand-ins for libm. Each is a least-squares fit on Chebyshev nodes (error figures from the fit, in float).
inline int floorInt(float x) { const int i = (int)x; return i - (x < (float)i); }   // floorf for |x| < 2e9 (the cast truncates toward zero; a negative non-integer is one lower)

// 2^x, relative error 1e-7 (a degree-5 polynomial on the fraction, the integer part into the exponent); x is held to +-100
inline float fastExp2(float x) {
    x = fclamp(x, -100.0f, 100.0f);
    const int i = floorInt(x);
    const float f = x - (float)i;
    const float p = 0.99999990f + f * (0.69315462f + f * (0.24014077f + f * (0.05586328f + f * (0.0089462146f + f * 0.0018951073f))));
    const uint32_t bits = (uint32_t)(i + 127) << 23;
    float two;
    memcpy(&two, &bits, sizeof two);
    return p * two;
}

// log2(x) for a normal x > 0, absolute error 1e-7: the mantissa into [0.707, 1.414), then log2 m = s (c0 + c1 s^2 + c2 s^4), s = (m-1)/(m+1)
inline float fastLog2(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof bits);
    int e = (int)((bits >> 23) & 255u) - 127;
    bits = (bits & 0x7FFFFFu) | 0x3F800000u;   // m in [1, 2)
    float m;
    memcpy(&m, &bits, sizeof m);
    if (m > 1.41421356f) { m *= 0.5f; ++e; }
    const float s = (m - 1.0f) / (m + 1.0f), s2 = s * s;
    return (float)e + s * (2.88539026f + s2 * (0.96161518f + s2 * 0.59499344f));
}

// sin(2 pi p) for p in [0, 1) (any p within a few cycles is folded), error 1e-8
inline float fastSinCycles(float p) {
    float x = p - (float)floorInt(p);          // [0, 1)
    if (x > 0.5f) x -= 1.0f;                    // [-0.5, 0.5]
    if (x > 0.25f) x = 0.5f - x; else if (x < -0.25f) x = -0.5f - x;
    const float x2 = x * x;
    return x * (6.28318529f + x2 * (-41.3416850f + x2 * (81.6027660f + x2 * (-76.5878660f + x2 * 39.8098860f))));
}

// tan(pi u) for u in [0, 0.5): sin and cos as polynomials in u^2, relative error 2e-7 up to u = 0.3, 4e-6 at 0.49 (both filters keep u below that)
inline float fastTanPi(float u) {
    const float u2 = u * u;
    const float s = u * (3.14159265f + u2 * (-5.16771063f + u2 * (2.55008646f + u2 * (-0.59834271f + u2 * 0.07775372f))));
    const float c = 0.99999998f + u2 * (-4.93479472f + u2 * (4.05844240f + u2 * (-1.33205520f + u2 * 0.22002336f)));
    return s / c;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Wavetable reading. Two neighbouring mips (picked from the pitch at each control block), one or two frames (position); 32-bit phase, linear
// interpolation inside a mip. The mip rule is the engine's: harmonics up to a quarter of the sample rate at the slot's pitch, crossfaded
// between the two mips that bracket the pitch.
struct MipRead {
    const nemesis::dsp::Real* f0 = nullptr;   // frame 0
    const nemesis::dsp::Real* f1 = nullptr;   // frame 1 (== f0 when the position sits on a frame)
    float mix = 0.0f;                        // weight of frame 1
    int shift = 21;                          // 32 - log2(length)
    uint32_t mask = 2047;
};
struct TableRef {
    MipRead a, b;
    float mipMix = 0.0f;                     // weight of mip b
};

inline void pickMip(const nemesis::dsp::Wavetable& wt, int mip, float pos, MipRead& t) {
    const int nf = wt.numFrames();
    const float fp = fclamp(pos, 0.0f, 1.0f) * (float)(nf - 1);
    int i0 = (int)fp;
    if (i0 > nf - 2) i0 = nf > 1 ? nf - 2 : 0;
    float fr = nf > 1 ? fp - (float)i0 : 0.0f;
    if (fr < 1.0f / 512.0f) fr = 0.0f;
    else if (fr > 1.0f - 1.0f / 512.0f) { fr = 0.0f; ++i0; }   // sitting on the next frame (the top one, a basic SIN): one frame to read, not two blended to nothing
    t.f0 = wt.frame(i0, mip);
    t.f1 = fr > 0.0f ? wt.frame(i0 + 1, mip) : t.f0;
    t.mix = fr;
    const int ml = wt.mipLength(mip);
    int lg = 0;
    while ((1 << lg) < ml) ++lg;
    t.shift = 32 - lg;
    t.mask = (uint32_t)ml - 1u;
}

// `inc`: cycles per sample of the slot. forceMip >= 0: that mip only (the sub: a pure sine).
inline void pickTable(const nemesis::dsp::Wavetable& wt, float pos, float inc, TableRef& t, int forceMip = -1) {
    const int top = wt.numMips() - 1;
    if (forceMip >= 0) { pickMip(wt, forceMip > top ? top : forceMip, pos, t.a); t.mipMix = 0.0f; return; }
    const float x = (float)wt.frameLength() * inc;
    float m = (x > 1e-6f ? fastLog2(x) : -20.0f) + 1.0f;   // kMipGuard: one octave toward the band-limited side
    m = fclamp(m, 0.0f, (float)top);
    int j0 = (int)m;
    const float fr = m - (float)j0;
    const int j1 = j0 < top ? j0 + 1 : top;
    pickMip(wt, j0, pos, t.a);
    t.mipMix = fr < 1.0f / 64.0f || j1 == j0 ? 0.0f : fr;
    if (t.mipMix > 0.0f) pickMip(wt, j1, pos, t.b);
}

inline float readMip(const MipRead& t, uint32_t phase) {
    const uint32_t i = phase >> t.shift, j = (i + 1u) & t.mask;
    const float fr = (float)(phase << (32 - t.shift)) * (1.0f / 4294967296.0f);   // the bits below the index
    const float a = (float)t.f0[i] + fr * ((float)t.f0[j] - (float)t.f0[i]);
    if (t.mix == 0.0f) return a;
    const float b = (float)t.f1[i] + fr * ((float)t.f1[j] - (float)t.f1[i]);
    return a + t.mix * (b - a);
}

inline float readTable(const TableRef& t, uint32_t phase) {
    const float a = readMip(t.a, phase);
    if (t.mipMix == 0.0f) return a;
    return a + t.mipMix * (readMip(t.b, phase) - a);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The filters. Coefficients are set per control block and ramped across it.
struct SvfCoef { float a1, a2, a3, k; };

inline SvfCoef svfCoef(float cutoffHz, float q, float sr) {
    const float fc = fclamp(cutoffHz, 8.0f, 0.45f * sr);
    const float g = fastTanPi(fc / sr);
    const float k = 1.0f / q;
    const float a1 = 1.0f / (1.0f + g * (g + k));
    return {a1, g * a1, g * g * a1, k};
}

struct Svf {
    float ic1 = 0.0f, ic2 = 0.0f;
    // mode: 0 LP, 1 BP, 2 HP, 3 notch
    inline float tick(float x, const SvfCoef& c, int mode) {
        const float v3 = x - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;
        const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        switch (mode) {
            case 0: return v2;
            case 1: return v1;                       // not normalised: gain Q at the cutoff, as the engine's
            case 2: return x - c.k * v1 - v2;
            default: return x - c.k * v1;
        }
    }
};

// The engine's soft clip: x - x^3/3, held at +-2/3 beyond |x| = 1
inline float softClip(float x) {
    if (x >= 1.0f) return 2.0f / 3.0f;
    if (x <= -1.0f) return -2.0f / 3.0f;
    return x - x * x * x * (1.0f / 3.0f);
}

// The soft clip with first-order antiderivative anti-aliasing (Parker et al., "Reducing the aliasing of nonlinear waveshaping using continuous-time
// convolution", DAFx 2016): y = (F(x) - F(x_prev)) / (x - x_prev), F the antiderivative of softClip.
inline float softClipInt(float x) {
    const float a = fabsf(x);
    return a < 1.0f ? x * x * (0.5f - x * x * (1.0f / 12.0f)) : (2.0f / 3.0f) * a - 0.25f;
}
struct ClipAdaa {
    float xp = 0.0f, Fp = 0.0f;
    inline float tick(float x) {
        const float F = softClipInt(x), d = x - xp;
        const float y = fabsf(d) > 1e-4f ? (F - Fp) / d : softClip(0.5f * (x + xp));
        xp = x; Fp = F;
        return y;
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------
// The phase-distortion oscillator (the panel's WAVE = SIN with PD AMT): the phase p is bent by a two-segment map W (slope 0.5/d up to the knee d,
// 0.5/(1-d) after it) and read through a sine, y = sin(2 pi W(p)) - (2/pi)(2d - 1) (zero mean at every knee); d = 0.5 + amount*(shape - 0.5),
// kept two samples from the ends. The bent phase makes harmonics that fold back at high pitch, so it is generated at twice the rate and brought
// down through a 19-tap half-band FIR (Kaiser window); the engine's is 4x with a longer filter, so the top octave is a little less clean here.
struct PdCoef { float knee = 0.5f, s0 = 1.0f, s1 = 1.0f, dc = 0.0f; };

inline PdCoef pdCoef(float amount, float shape, float incCycles) {
    float eps = 2.0f * incCycles;
    eps = eps < 1.0e-4f ? 1.0e-4f : (eps > 0.5f ? 0.5f : eps);
    const float d = fclamp(0.5f + fclamp(amount, 0.0f, 1.0f) * (fclamp(shape, 0.0f, 1.0f) - 0.5f), eps, 1.0f - eps);
    return {d, 0.5f / d, 0.5f / (1.0f - d), (2.0f / kPi) * (2.0f * d - 1.0f)};
}

constexpr int kPdTaps = 5;   // odd taps either side of the centre: 19 in all
struct PdTables {
    float sine[2049];
    float h[kPdTaps];       // half-band taps at offsets 1, 3, 5, 7, 9 (the centre is 0.5)
    PdTables() {
        for (int i = 0; i <= 2048; ++i) sine[i] = sinf(2.0f * kPi * (float)i / 2048.0f);
        const float beta = 7.0f;
        auto i0 = [](float x) { float t = 1.0f, sum = 1.0f; for (int k = 1; k < 30; ++k) { t *= (x * 0.5f / (float)k) * (x * 0.5f / (float)k); sum += t; } return sum; };
        float tot = 0.5f;
        for (int j = 0; j < kPdTaps; ++j) {
            const float o = (float)(2 * j + 1), r = o / 10.0f;
            const float sgn = (j & 1) ? -1.0f : 1.0f;
            h[j] = sgn / (kPi * o) * i0(beta * sqrtf(1.0f - r * r)) / i0(beta);
            tot += 2.0f * h[j];
        }
        for (int j = 0; j < kPdTaps; ++j) h[j] /= tot;   // unity at DC
    }
};
inline const PdTables& pdTables() { static const PdTables t; return t; }

struct PdState { float x[32] = {}; int n = 0; };

// One base-rate sample: two oscillator steps at half the increment, then the half-band decimation. ph advances by inc.
inline float pdTick(uint32_t& ph, uint32_t inc, const PdCoef& c, PdState& st, const PdTables& T) {
    const uint32_t half = inc >> 1;
    for (int s = 0; s < 2; ++s) {
        ph += half;
        const float x = (float)ph * (1.0f / 4294967296.0f);
        const float w = x < c.knee ? c.s0 * x : 0.5f + c.s1 * (x - c.knee);
        const float fi = w * 2048.0f;
        int i = (int)fi;
        if (i > 2047) i = 2047;
        const float y = T.sine[i] + (fi - (float)i) * (T.sine[i + 1] - T.sine[i]) - c.dc;
        st.x[st.n & 31] = y;
        ++st.n;
    }
    const int n = st.n - 1, m = n - 9;   // the newest sample is at n; the filter is centred 9 back
    float acc = 0.5f * st.x[m & 31];
    for (int j = 0; j < kPdTaps; ++j) acc += T.h[j] * (st.x[(m - 2 * j - 1) & 31] + st.x[(m + 2 * j + 1) & 31]);
    return acc;
}

// A slow random wander: random targets in [-1, 1) joined by straight lines, one every `seg` samples (the converter's drift uses three of them).
struct SlowNoise {
    uint32_t s = 1;
    int seg = 1000, pos = 0;
    float v0 = 0.0f, v1 = 0.0f;
    static float draw(uint32_t& s) { s = s * 1664525u + 1013904223u; return (float)(int32_t)s * (1.0f / 2147483648.0f); }
    void seed(uint32_t sd, int segment) { s = sd; seg = segment < 1 ? 1 : segment; pos = 0; v0 = draw(s); v1 = draw(s); }
    float at() const { return v0 + (v1 - v0) * ((float)pos / (float)seg); }
    float advance(int n) {   // n samples on; the value there
        pos += n;
        while (pos >= seg) { pos -= seg; v0 = v1; v1 = draw(s); }
        return at();
    }
};

// A cascade of `poles` one-pole TPT stages (out = G*in + (1-G)*state, state = 2*out - state) with the feedback k around all of them solved in
// closed form, so the loop is exact and linear; a nonlinearity can sit before the loop (drive) and, for ACID, in the feedback term.
struct LadderCoef { float G = 0.1f, k = 0.0f, norm = 1.0f; };

inline LadderCoef ladderCoef(float cutoffHz, float k, float sr, int poles, float gCap = 1.0f) {
    const float fc = fclamp(cutoffHz, 8.0f, 0.49f * sr);
    const float g = fastTanPi(fc / sr);
    float G = g / (1.0f + g);
    if (G > gCap) G = gCap;
    float Gp = G;
    for (int i = 1; i < poles; ++i) Gp *= G;
    return {G, k, 1.0f / (1.0f + k * Gp)};
}

struct Ladder {
    float s[4] = {0, 0, 0, 0};
    float y3 = 0.0f;   // the last output of the third stage (ACID's feedback term)
    // `in` already carries any input gain and drive. POLES 3 or 4. extra: added to the loop input (ACID's feedback saturation).
    // STAGECLIP: every stage's input goes through the soft clip (CLASSIC, and the driven LADDER).
    template <int POLES, bool STAGECLIP>
    inline float tick(float in, const LadderCoef& c, float extra) {
        const float G = c.G, H = 1.0f - G;
        float S;
        if (POLES == 4) S = H * (G * G * G * s[0] + G * G * s[1] + G * s[2] + s[3]);
        else S = H * (G * G * s[0] + G * s[1] + s[2]);
        float u = (in + extra - c.k * S) * c.norm;
        if (STAGECLIP) u = softClip(u);
        const float o1 = G * u + H * s[0];
        const float i2 = STAGECLIP ? softClip(o1) : o1;
        const float o2 = G * i2 + H * s[1];
        const float i3 = STAGECLIP ? softClip(o2) : o2;
        const float o3 = G * i3 + H * s[2];
        s[0] = 2.0f * o1 - s[0];
        s[1] = 2.0f * o2 - s[1];
        s[2] = 2.0f * o3 - s[2];
        y3 = o3;
        if (POLES == 3) return o3;
        const float i4 = STAGECLIP ? softClip(o3) : o3;
        const float o4 = G * i4 + H * s[3];
        s[3] = 2.0f * o4 - s[3];
        return o4;
    }
};

// The drive in front of the ladder family: gain g = 1 + 6d, blended toward the soft clip as g rises (fully clipped from g = 2.5)
inline float driveIn(float x, float g, float mix) {
    const float y = g * x;
    return mix > 0.0f ? y + mix * (softClip(y) - y) : y;
}

// One-pole high-pass (the HPF knob)
struct OnePoleHp {
    float z = 0.0f;
    inline float tick(float x, float a) { const float lp = z + a * (x - z); z = lp; return x - lp; }
};

}  // namespace lean

#endif
