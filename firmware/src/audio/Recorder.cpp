#include "audio/Recorder.h"

#include <math.h>

namespace audio {

static constexpr int kAboveFor = 8;    // samples the input must be over the threshold: one tick of noise is not a sound
static constexpr float kFullScale = 1.0f / 2147483648.0f;

void Recorder::start(int16_t* buffer, uint32_t capacity, const Config& c) {
    buffer_ = buffer;
    capacity_ = capacity;
    used_ = 0;
    c_ = c;
    source_ = c.source;
    preAt_ = 0;
    preFull_ = false;
    env_ = 0.0f;
    above_ = 0;
    quiet_ = 0;
    clipped_ = false;
    decay_ = expf(-1.0f / (0.02f * (float)c.rate));   // the level falls in 20 ms
    quietNeed_ = (uint32_t)(c.stopSeconds * (float)c.rate);
    state_ = c.autoCapture ? Armed : Capturing;
}

void Recorder::stop() {
    if (state_ == Armed) state_ = Idle;
    else if (state_ == Capturing) state_ = Done;
}

void Recorder::clear() {
    state_ = Idle;
    used_ = 0;
}

inline void Recorder::take(float x) {
    const float mag = fabsf(x);
    env_ = mag > env_ ? mag : env_ * decay_;
    const int16_t v = (int16_t)(x >= 1.0f ? 32767 : x <= -1.0f ? -32768 : (int32_t)(x * 32767.0f));
    if (state_ == Armed) {
        above_ = mag >= c_.threshold ? above_ + 1 : (above_ > 0 ? above_ - 1 : 0);
        if (above_ < kAboveFor) {
            pre_[preAt_] = v;
            if (++preAt_ >= kPreRoll) { preAt_ = 0; preFull_ = true; }
            return;
        }
        // the moment the sound came: what led up to it first, oldest first
        const uint32_t n = preFull_ ? kPreRoll : preAt_;
        uint32_t at = preFull_ ? preAt_ : 0, out = 0;
        for (uint32_t i = 0; i < n && out < capacity_; ++i) {
            buffer_[out++] = pre_[at];
            if (++at >= kPreRoll) at = 0;
        }
        used_ = out;
        quiet_ = 0;
        state_ = Capturing;
    }
    if (state_ != Capturing) return;
    if (used_ >= capacity_) { state_ = Done; return; }
    buffer_[used_] = v;
    used_ = used_ + 1;
    if (quietNeed_) {
        quiet_ = env_ < c_.threshold * 0.5f ? quiet_ + 1 : 0;
        if (quiet_ >= quietNeed_) { used_ = used_ - quiet_ + (uint32_t)(0.02f * (float)c_.rate); state_ = Done; }   // the silence goes, all but a moment of tail
    }
}

void Recorder::block(const uint32_t* rx, float* left, float* right, int frames) {
    if (!listening_ && state_ == Idle) return;
    const bool line = source_ == Line;
    const float mon = line ? monitor_ : 0.0f;
    const bool taking = state_ == Armed || state_ == Capturing;
    const uint8_t channel = c_.channel;
    float pk = peak_;
    for (int i = 0; i < frames; ++i) {
        float l, r;
        if (line) {
            // the ADC has a little DC on it: a high-pass at about 5 Hz
            const float a = (float)(int32_t)rx[2 * i] * kFullScale, b = (float)(int32_t)rx[2 * i + 1] * kFullScale;
            l = a - dcInL_ + 0.9993f * dcOutL_;
            dcInL_ = a;
            dcOutL_ = l;
            r = b - dcInR_ + 0.9993f * dcOutR_;
            dcInR_ = b;
            dcOutR_ = r;
            if (mon > 0.0f) { left[i] += l * mon; right[i] += r * mon; }
        } else {
            l = left[i];
            r = right[i];
        }
        float x = channel == Left ? l : channel == Right ? r : (l + r) * 0.5f;
        if (!(x > -4.0f && x < 4.0f)) x = 0.0f;   // not a sane number (the output path can hand one over): not in a take, not on the meter
        const float m = fabsf(x);
        if (m > pk) pk = m;
        if ((fabsf(l) >= 0.999f || fabsf(r) >= 0.999f) && state_ != Idle) clipped_ = true;   // each side: L+R of one loud side would read half
        if (taking) take(x);
    }
    peak_ = pk;
}

}  // namespace audio
