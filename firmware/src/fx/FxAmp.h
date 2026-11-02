#ifndef __FX_AMP_H
#define __FX_AMP_H

#include <nemesis/dsp/GampEffect.h>

#include "fx/FxUtil.h"

// The amp simulator, one per part: upstream's GampEffect (preamp, tone stack, power amp, speaker) as a MONO insert with no oversampling
// (docs/FX_ANALYSIS.md: stereo at the native factor would cost 3-9 voices; 2x cost twice this on the board, docs/LEAN_VOICE.md). The two channels are summed in, the result
// goes out on both. A model change or an on/off is a short fade through the dry signal: the class steps 7x on a model switch.
// It rests when nothing has come in for kRestMs: an amp costs as much as a voice or two whether or not it hears anything.
namespace fx {

struct AmpSettings {
    int model = 0;          // 0 off, 1 clean, 2 crunch, 3 lead, 4 bass
    float drive = 0.0f;     // 0..1
    float bassDb = 0, midDb = 0, trebleDb = 0;
    float presence = 0.4f;  // 0..1
    int cab = 2;            // 0 off, 1 1x12, 2 2x12, 3 4x12
    float levelDb = 0;
    bool operator==(const AmpSettings& o) const {
        return model == o.model && drive == o.drive && bassDb == o.bassDb && midDb == o.midDb && trebleDb == o.trebleDb && presence == o.presence &&
               cab == o.cab && levelDb == o.levelDb;
    }
};

class FxAmp {
   public:
    void prepare(float sampleRate);
    void set(const AmpSettings& s);
    bool running() const;
    bool resting() const { return quiet_ >= rest_; }
    void process(float* l, float* r, int n);   // in place
    void reset();

   private:
    void apply();
    nemesis::dsp::GampEffect amp_;
    AmpSettings want_, have_;
    float a_[2][64] = {};
    Smooth fade_;
    bool primed_ = false;
    int quiet_ = 0, rest_ = 1;   // samples with nothing coming in; how many it takes to rest (the tail is long gone by then)
};

}  // namespace fx

#endif
