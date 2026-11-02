#ifndef __FX_UTIL_H
#define __FX_UTIL_H

#include <math.h>

// Small helpers shared by the effect wrappers (docs/FX_ANALYSIS.md): the upstream classes do not smooth their mix or makeup, so every
// level that a knob or a p-lock can move goes through a Smooth here; a step of any of them would click.
namespace fx {

inline float dbToLin(float db) { return powf(10.0f, db * 0.05f); }

// One-pole smoother, per sample. `ms` is the time to cover ~63% of a step.
struct Smooth {
    float cur = 0.0f, tgt = 0.0f, coef = 0.01f;
    void setTime(float ms, float sr) { coef = 1.0f - expf(-1.0f / (ms * 0.001f * sr)); }
    void snap(float v) { cur = tgt = v; }
    void set(float v) { tgt = v; }
    float next() {
        cur += coef * (tgt - cur);
        return cur;
    }
    bool settled() const { return fabsf(tgt - cur) < 1e-5f; }
};

// A one-pole low-pass coefficient for a corner at hz.
inline float lpCoef(float hz, float sr) {
    if (hz >= sr * 0.45f) return 1.0f;
    return 1.0f - expf(-6.2831853f * hz / sr);
}

#ifndef FX_HALF_RATE
#define FX_HALF_RATE 0   // 1: the reverb and delay cores run at half the sample rate (DBOY_FX_HALF_RATE)
#endif

// ---- half-rate processing (FX_HALF_RATE): a 31-tap Kaiser halfband (flat to 8 kHz at 44.1 kHz, -0.5 dB at 9, -6 dB at the new Nyquist 11.025, -24 dB at 13, -83 dB at 16) to go
// down to half the rate and come back up, so a reverb or delay core runs half as often. Latency 15 samples each way at the full rate (0.68 ms in all). Streaming: call push()
// once per full-rate sample; the decimator says when a half-rate sample is ready, the interpolator is told when a new one arrives.
constexpr float kHalfBandOdd[8] = {0.31304553f, -0.09122481f, 0.04153670f, -0.01922764f, 0.00802032f, -0.00273444f, 0.00064225f, -0.00004963f};

struct Decim2 {
    float x[32] = {};
    int pos = 0;
    bool phase = false;
    void reset() { for (float& v : x) v = 0.0f; pos = 0; phase = false; }
    // returns true on every second call, with the half-rate sample in `out`
    bool push(float in, float& out) {
        x[pos & 31] = in;
        ++pos;
        phase = !phase;
        if (!phase) return false;   // (the first of each pair only stores)
        const int c = pos - 1 - 15;   // the centre of the filter: 15 behind the newest
        float a = 0.5f * x[c & 31];
        for (int j = 0; j < 8; ++j) a += kHalfBandOdd[j] * (x[(c + 2 * j + 1) & 31] + x[(c - 2 * j - 1) & 31]);
        out = a;
        return true;
    }
};

struct Interp2 {
    float u[32] = {};
    int pos = 0;
    void reset() { for (float& v : u) v = 0.0f; pos = 0; }
    // call once per full-rate sample; `fresh` = a new half-rate sample `w` arrives now (every second call), else w is ignored
    float push(bool fresh, float w) {
        u[pos & 31] = fresh ? w : 0.0f;
        ++pos;
        const int c = pos - 1 - 15;   // the filter's centre
        if (!fresh) return u[c & 31];   // the centre is a stuffed-in sample: the filter reduces to a copy of it (2 x h[0] = 1)
        float a = 0.0f;
        for (int j = 0; j < 8; ++j) a += kHalfBandOdd[j] * (u[(c + 2 * j + 1) & 31] + u[(c - 2 * j - 1) & 31]);
        return 2.0f * a;
    }
};

}  // namespace fx

#endif
