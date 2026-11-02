#include "fx/FxChorus.h"

#include <string.h>

namespace fx {

void FxChorus::prepare(float sr) {
    sr_ = sr;
    for (int i = 0; i <= kTable; ++i) sinTab_[i] = sinf(6.2831853f * (float)i / (float)kTable);
    ret_.setTime(8.0f, sr);
    ret_.snap(0.0f);
    delayS_.setTime(25.0f, sr);     // the delay and the sweep depth move the read pointer: they glide
    depthS_.setTime(25.0f, sr);
    toneS_.setTime(30.0f, sr);
    toneS_.snap(lpCoef(8000.0f, sr));
    delayS_.snap(12.0f);
    depthS_.snap(0.0f);
    reset();
}

void FxChorus::reset() {
    memset(ring_, 0, sizeof(ring_));
    for (int c = 0; c < 2; ++c) hpState_[c] = lp_[c] = 0.0f;
    w_ = 0;
    ret_.cur = 0.0f;
}

void FxChorus::set(float rateHz, float depth, float delayMs, float feedback, float width, float toneHz, float hpfHz, float returnLin) {
    inc_ = rateHz / sr_;
    delayS_.set(delayMs);
    depthS_.set(depth * delayMs * 0.5f);     // the sweep: up to half the base delay either way
    fb_ = feedback;
    width_ = width;
    toneS_.set(lpCoef(toneHz, sr_));
    hpA_ = lpCoef(hpfHz, sr_);
    ret_.set(returnLin);
}

float FxChorus::lfo(float ph) const {
    const float x = (ph - floorf(ph)) * (float)kTable;
    const int i = (int)x;
    const float f = x - (float)i;
    return sinTab_[i] + (sinTab_[i + 1] - sinTab_[i]) * f;
}

void FxChorus::wake() {
    ret_.snap(ret_.tgt);
    delayS_.snap(delayS_.tgt);
    depthS_.snap(depthS_.tgt);
    toneS_.snap(toneS_.tgt);
}

float FxChorus::process(const float* inL, const float* inR, float* outL, float* outR, int n) {
    const float* in[2] = {inL, inR};
    float* out[2] = {outL, outR};
    float peak = 0.0f;
    const float fb = fb_;
    const float msToS = sr_ * 0.001f;
    for (int i = 0; i < n; ++i) {
        const float g = ret_.next();
        delayMs_ = delayS_.next();
        depthMs_ = depthS_.next();
        toneA_ = toneS_.next();
        phase_ += inc_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        for (int c = 0; c < 2; ++c) {
            // the high-pass on the input keeps the lows out of the chorus; the right LFO trails by width * half a cycle
            const float x = in[c][i];
            hpState_[c] += hpA_ * (x - hpState_[c]);
            const float hx = x - hpState_[c];
            const float d = (delayMs_ + depthMs_ * lfo(phase_ + (c ? 0.5f * width_ : 0.0f))) * msToS;
            float rp = (float)w_ - d;
            if (rp < 0) rp += (float)kRing;
            const int i0 = (int)rp;
            const float f = rp - (float)i0;
            const float a = ring_[c][i0 & (kRing - 1)], b = ring_[c][(i0 + 1) & (kRing - 1)];
            float wet = a + (b - a) * f;
            lp_[c] += toneA_ * (wet - lp_[c]);
            wet = lp_[c];
            ring_[c][w_] = hx + fb * wet;
            out[c][i] = wet * g;
            const float ay = wet < 0 ? -wet : wet;   // (the tail detector looks before the return)
            if (ay > peak) peak = ay;
        }
        w_ = (w_ + 1) & (kRing - 1);
    }
    return peak;
}

}  // namespace fx
