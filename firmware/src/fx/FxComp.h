#ifndef __FX_COMP_H
#define __FX_COMP_H

#include <nemesis/dsp/CompEffect.h>

#include "fx/FxUtil.h"

// The master compressor: upstream's CompEffect as its Bus voicing with the saturation off (it is 65% of the cost), stereo-linked, in place.
// The class does not smooth MAKEUP or MIX (a step is a 7x jump), so both are applied here, and ACTIVE is a fade.
namespace fx {

class FxComp {
   public:
    void prepare(float sampleRate);
    // control rate
    void set(bool active, float threshDb, float ratio, float attackMs, float releaseMs, float makeupDb, float mix01, float scHpfHz);
    bool running() const { return on_.tgt > 0.5f || on_.cur > 1e-4f; }
    void process(float* l, float* r, int n);   // in place
    void reset();

   private:
    nemesis::dsp::CompEffect comp_;
    float dry_[2][64] = {};
    float th_ = 1e9f, ra_ = 0, at_ = 0, re_ = 0, hp_ = 0;
    Smooth on_, makeup_, mix_;
};

}  // namespace fx

#endif
