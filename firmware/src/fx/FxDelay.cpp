#include "fx/FxDelay.h"

namespace fx {

// the halfband pair delays the signal by 30 samples (0.68 ms) in all; taken off the delay time so a tempo-synced echo lands where it did
#if FX_HALF_RATE
static const float kHalfLatencyMs = 30.0f / 44.1f;
#else
static const float kHalfLatencyMs = 0.0f;
#endif

void FxDelay::prepare(float sr) {
    sr_ = sr;
#if FX_HALF_RATE
    const float coreSr = sr * 0.5f;   // (half the memory too)
    decL_.reset();
    decR_.reset();
    upL_.reset();
    upR_.reset();
    wl_ = wr_ = 0.0f;
#else
    const float coreSr = sr;
#endif
    l_.prepare(coreSr, kMaxMs);
    r_.prepare(coreSr, kMaxMs);
    for (auto* c : {&l_, &r_}) {
        c->setTimeMode(nemesis::dsp::DelayCore::TimeMode::Varispeed);   // a time change glides (tape-like) instead of crossfading: MOD needs it
        c->setGlideMs(120.0f);
        c->setFeedback(0.4f);
        c->setToneLpHz(5000.0f);
        c->setToneHpHz(150.0f);
        c->setDelayMs(300.0f);
    }
    ret_.setTime(8.0f, sr);
    ret_.snap(0.0f);
    l_.reset();
    r_.reset();
}

void FxDelay::set(float timeMs, float feedback, float toneHz, float spread, float mod, float hpfHz, float returnLin) {
    timeMs_ = timeMs < 1.0f ? 1.0f : timeMs > kMaxMs - 60.0f ? kMaxMs - 60.0f : timeMs;
    spread_ = spread;
    mod_ = mod;
    fb_ = feedback;
    for (auto* c : {&l_, &r_}) {
        c->setToneLpHz(toneHz);
        c->setToneHpHz(hpfHz);
    }
    ret_.set(returnLin);
}

float FxDelay::process(const float* inL, const float* inR, float* outL, float* outR, int n) {
    // the feedback glides (a 95% to 0% step would put a click into the loop that comes round again a delay time later): ~75 ms
    fbCur_ += 0.01f * (fb_ - fbCur_);
    l_.setFeedback(fbCur_);
    r_.setFeedback(fbCur_);
    // the time, once per block: the right line earlier by up to a quarter at full spread, both wobbling by up to 4 ms
    phase_ += 0.55f * (float)n / sr_;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
    const float wob = mod_ * 4.0f * sinf(6.2831853f * phase_);
    l_.setDelayMs(timeMs_ + wob - kHalfLatencyMs);
    r_.setDelayMs(timeMs_ * (1.0f - 0.25f * spread_) - wob - kHalfLatencyMs);
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float g = ret_.next();
#if FX_HALF_RATE
        float dl, dr;
        const bool fresh = decL_.push(inL[i], dl);
        decR_.push(inR[i], dr);
        if (fresh) {
            wl_ = l_.processSample(dl);
            wr_ = r_.processSample(dr);
        }
        const float a0 = upL_.push(fresh, wl_), b0 = upR_.push(fresh, wr_);   // (the tail detector looks before the return)
#else
        const float a0 = l_.processSample(inL[i]), b0 = r_.processSample(inR[i]);   // (the tail detector looks before the return)
#endif
        const float a = a0 * g, b = b0 * g;
        outL[i] = a;
        outR[i] = b;
        const float x = a0 < 0 ? -a0 : a0, y = b0 < 0 ? -b0 : b0;
        if (x > peak) peak = x;
        if (y > peak) peak = y;
    }
    return peak;
}

void FxDelay::wake() {
    ret_.snap(ret_.tgt);   // (nothing was coming out: what changed while it slept is simply there)
    // the set time may have changed while the lines slept (set() only stores it): the pointers go to it, not to where they were
    l_.setDelayMs(timeMs_ - kHalfLatencyMs);
    r_.setDelayMs(timeMs_ * (1.0f - 0.25f * spread_) - kHalfLatencyMs);
    l_.snap();
    r_.snap();
    fbCur_ = fb_;
}

void FxDelay::reset() {
    l_.reset();
    r_.reset();
#if FX_HALF_RATE
    decL_.reset();
    decR_.reset();
    upL_.reset();
    upR_.reset();
    wl_ = wr_ = 0.0f;
#endif
    ret_.cur = 0.0f;
}

}  // namespace fx
