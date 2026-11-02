#ifndef __FX_REVERB_H
#define __FX_REVERB_H

#include <nemesis/dsp/ReverbCore.h>

#include "fx/FxUtil.h"

// The reverb: upstream's 8-line Hall network (ReverbCore, Hadamard mixing, four diffusers, per-line damping and a slow modulation), run
// directly so the knobs can reach what its Hall type does not (DECAY up to 12 s, PRE-DLY 0..200 ms). Mono in (the two channels summed),
// stereo out. WIDTH is mid/side on the output, LOW CUT the network's input high-pass. A shared send effect: 100% wet out.
// A SIZE change ducks the return for a few ms while the lines move: the lengths jump, and the network would click.
namespace fx {

class FxReverb {
   public:
    void prepare(float sampleRate);
    void set(float size01, float decaySec, float dampHz, float preMs, float width01, float mod01, float lowCutHz, float returnLin);
    float process(const float* inL, const float* inR, float* outL, float* outR, int n);
    void reset();
    void wake();   // idle for a while: any pending size change lands now, not as a dip

   private:
    void applyLines();
    nemesis::dsp::ReverbCore core_;
    float sr_ = 44100.0f, size_ = 0.6f, appliedSize_ = -1.0f, decay_ = 2.5f;
    int duck_ = 0;     // samples left of the dip around a size change
    Smooth ret_, duckG_, width_;
#if FX_HALF_RATE
    Decim2 dec_;
    Interp2 upL_, upR_;
    float wl_ = 0.0f, wr_ = 0.0f;   // the core's latest half-rate output
#endif
};

}  // namespace fx

#endif
