#ifndef __FX_CHORUS_H
#define __FX_CHORUS_H

#include "fx/FxUtil.h"

// A small stereo chorus (docs/FX_ANALYSIS.md: 0.06 voice equivalents, 16 KB; upstream's own is 6 times dearer and has no DELAY knob):
// one modulated delay line per channel, the right LFO a quarter to half a cycle behind the left (WIDTH), feedback, a tone low-pass on the
// wet and a high-pass on the input. A shared send effect: 100% wet out.
namespace fx {

class FxChorus {
   public:
    void prepare(float sampleRate);
    void set(float rateHz, float depth, float delayMs, float feedback, float width, float toneHz, float hpfHz, float returnLin);
    float process(const float* inL, const float* inR, float* outL, float* outR, int n);   // wet, overwritten; the block's peak
    void wake();   // it has been idle: what changed meanwhile is simply there, not glided to under the first sound
    void reset();

   private:
    static constexpr int kRing = 4096;      // 93 ms at 44.1 kHz: room for 1.5 x 30 ms up to 48 kHz
    static constexpr int kTable = 256;
    float sinTab_[kTable + 1];
    float ring_[2][kRing];
    int w_ = 0;
    float sr_ = 44100.0f, phase_ = 0.0f, inc_ = 0.0f, depthMs_ = 0.0f, delayMs_ = 12.0f, fb_ = 0.0f, width_ = 1.0f, toneA_ = 1.0f, hpA_ = 0.0f;
    float hpState_[2] = {}, lp_[2] = {};
    Smooth ret_, delayS_, depthS_, toneS_;
    float lfo(float ph) const;
};

}  // namespace fx

#endif
