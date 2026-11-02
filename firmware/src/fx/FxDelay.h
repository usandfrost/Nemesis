#ifndef __FX_DELAY_H
#define __FX_DELAY_H

#include <nemesis/dsp/DelayCore.h>

#include "fx/FxUtil.h"

// A stereo delay on two upstream DelayCores (docs/FX_ANALYSIS.md): the left and right lines each have their own time (SPREAD moves the
// right one earlier), feedback, a loop low-pass (TONE) and high-pass, and a slow wobble (MOD). A shared send effect: 100% wet out.
namespace fx {

class FxDelay {
   public:
    static constexpr float kMaxMs = 2000.0f;   // 1/4 at 40 BPM is 1.5 s

    void prepare(float sampleRate);
    // control rate. feedback 0..0.95, spread/mod 0..1
    void set(float timeMs, float feedback, float toneHz, float spread, float mod, float hpfHz, float returnLin);
    // wet output, overwritten; returns the block's peak (for the tail detector)
    float process(const float* inL, const float* inR, float* outL, float* outR, int n);
    void reset();            // clears the lines (main loop only: it writes the whole ring)
    void wake();   // the lines have been idle: the read pointers go straight to the (latest) set time, no tape swoosh

   private:
    nemesis::dsp::DelayCore l_, r_;
    float sr_ = 44100.0f, timeMs_ = 300.0f, spread_ = 0.0f, mod_ = 0.0f, fb_ = 0.4f, fbCur_ = 0.4f, phase_ = 0.0f;
    Smooth ret_;
#if FX_HALF_RATE
    Decim2 decL_, decR_;
    Interp2 upL_, upR_;
    float wl_ = 0.0f, wr_ = 0.0f;
#endif
};

}  // namespace fx

#endif
