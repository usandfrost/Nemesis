#include "fx/FxReverb.h"

namespace fx {

static const float kLineMs[8] = {24.7f, 31.3f, 39.1f, 46.9f, 56.3f, 65.5f, 74.9f, 89.1f};   // upstream's Hall voicing
static const float kDiffMs[4] = {4.8f, 7.3f, 10.0f, 12.7f};
static const float kSizeMin = 0.7f, kSizeMax = 1.5f;
static const float kDuckMs = 30.0f;   // the dip around a size change: ~43 dB down at the bottom (3 ms time constant)

void FxReverb::prepare(float sr) {
    sr_ = sr;
#if FX_HALF_RATE
    const float coreSr = sr * 0.5f;   // everything in the core is in ms or Hz, so only the sample counts change
    dec_.reset();
    upL_.reset();
    upR_.reset();
    wl_ = wr_ = 0.0f;
#else
    const float coreSr = sr;
#endif
    core_.prepare(coreSr, 8, nemesis::dsp::ReverbCore::Mixing::Hadamard, kLineMs[7] * kSizeMax + 2.0f, 200.0f);
    core_.setDiffuserCount(4);
    for (int i = 0; i < 4; ++i) core_.setDiffuserDelayMs(i, kDiffMs[i]);
    core_.setDiffusion(0.7f);
    core_.setInputLpHz(10000.0f);
    core_.setModRateHz(0.6f);
    core_.setRt60Seconds(2.5f);
    ret_.setTime(8.0f, sr);
    ret_.snap(0.0f);
    duckG_.setTime(3.0f, sr);
    duckG_.snap(1.0f);
    width_.setTime(20.0f, sr);
    width_.snap(1.0f);
    appliedSize_ = -1.0f;
    applyLines();
    core_.reset();
}

void FxReverb::applyLines() {
    const float s = kSizeMin + (kSizeMax - kSizeMin) * size_;
    for (int i = 0; i < 8; ++i) core_.setLineDelayMs(i, kLineMs[i] * s);
    appliedSize_ = size_;
}

void FxReverb::set(float size01, float decaySec, float dampHz, float preMs, float width01, float mod01, float lowCutHz, float returnLin) {
    size_ = size01;
    decay_ = decaySec;
    width_.set(width01);
    core_.setRt60Seconds(decaySec);
    core_.setDampingHz(dampHz);
    core_.setPreDelayMs(preMs);
    #if FX_HALF_RATE
    core_.setModDepthSamples(mod01 * 3.0f);   // (samples of the half-rate lines)
#else
    core_.setModDepthSamples(mod01 * 6.0f);
#endif
    core_.setInputHpHz(lowCutHz);
    ret_.set(returnLin);
    if (fabsf(size_ - appliedSize_) > 0.004f && duck_ == 0) duck_ = (int)(kDuckMs * 0.001f * sr_);   // dip, then move the lines at the bottom
}

void FxReverb::wake() {
    width_.snap(width_.tgt);   // (nothing was coming out: what changed while it slept is simply there)
    ret_.snap(ret_.tgt);
    if (fabsf(size_ - appliedSize_) > 0.0f) applyLines();
    duck_ = 0;
    duckG_.snap(1.0f);
}

float FxReverb::process(const float* inL, const float* inR, float* outL, float* outR, int n) {
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (duck_ > 0) {
            // the first half of the dip fades the return out; at the bottom the lines move; the second half fades back in
            const int total = (int)(kDuckMs * 0.001f * sr_);
            duckG_.set(duck_ > total / 2 ? 0.0f : 1.0f);
            if (duck_ == total / 2) applyLines();
            --duck_;
            if (duck_ == 0 && fabsf(size_ - appliedSize_) > 0.004f) duck_ = total;   // moved again meanwhile: another dip
        } else {
            duckG_.set(1.0f);
        }
        const float g = ret_.next() * duckG_.next();
#if FX_HALF_RATE
        float down;
        const bool fresh = dec_.push(0.5f * (inL[i] + inR[i]), down);
        if (fresh) {
            const nemesis::dsp::ReverbCore::Stereo h = core_.processSample(down);
            wl_ = h.l;
            wr_ = h.r;
        }
        const float wL = upL_.push(fresh, wl_), wR = upR_.push(fresh, wr_);
#else
        const nemesis::dsp::ReverbCore::Stereo w = core_.processSample(0.5f * (inL[i] + inR[i]));
        const float wL = w.l, wR = w.r;
#endif
        const float m = 0.5f * (wL + wR), s = 0.5f * (wL - wR) * width_.next();
        const float a0 = m + s, b0 = m - s;   // (the tail detector looks before the return and the dip)
        outL[i] = a0 * g;
        outR[i] = b0 * g;
        const float x = a0 < 0 ? -a0 : a0, y = b0 < 0 ? -b0 : b0;
        if (x > peak) peak = x;
        if (y > peak) peak = y;
    }
    return peak;
}

void FxReverb::reset() {
    core_.reset();
#if FX_HALF_RATE
    dec_.reset();
    upL_.reset();
    upR_.reset();
    wl_ = wr_ = 0.0f;
#endif
    ret_.cur = 0.0f;
    duck_ = 0;
    duckG_.snap(1.0f);
    width_.snap(width_.tgt);
}

}  // namespace fx
