#ifndef __FX_SECTION_H
#define __FX_SECTION_H

#include "app/FxParams.h"
#include "fx/FxAmp.h"
#include "fx/FxChorus.h"
#include "fx/FxComp.h"
#include "fx/FxDelay.h"
#include "fx/FxReverb.h"

// The effects section (docs/FX_ANALYSIS.md section 5). The parts' signals come in on one buffer pair each:
//   part -> [AMP insert, if on] -> dry bus
//   part -> x send -> DELAY, REVERB, CHORUS (100% wet) -> x return -> bus
//   bus -> [COMP] -> out
// A part's send is smoothed, so a knob or a p-lock never clicks. An effect with nothing coming in and nothing left in its tail goes to sleep
// (no processing at all); `active()` says whether the section has anything to do, and the instrument skips it (and keeps its old
// summing, bit for bit) when it has not.
namespace fx {

class FxSection {
   public:
    static constexpr int kParts = app::kFxParts;
    static constexpr int kChunk = 64;
    enum Eff { DELAY, REVERB, CHORUS, EFFECTS };

    void init(float sampleRate);   // allocates the delay lines and the reverb network: boot only
    void apply(const app::FxSettings& fx, int bpm);   // control rate: only what changed is pushed on
    // What the CPU governor has taken from the effects (bit n = cost::Action n; cost/Degrade.h): the shared ones in `fxMask`, each part's amp in
    // `ampMask[p]`. The settings stay as they are; the effect is bypassed (faded out and not run) until the ladder gives it back.
    void setDegrade(uint16_t fxMask, const uint16_t ampMask[kParts]);
    bool active() const;
    // `out` is added to (it holds nothing of the parts yet)
    // `gain` is each part's level fader: applied AFTER the amp (the fader must not change how hard the amp is driven) and before the sends
    // `sounding`: a bit per part whose buffers hold anything (the others are all zeros and cost nothing here)
    void process(float* const partL[kParts], float* const partR[kParts], const float gain[kParts], uint32_t sounding, float* outL, float* outR, int n);
    void clear();                  // empties every line and tail: the audio must not run (the instrument holds it)

    float tailPeak(int e) const { return peak_[e]; }
    bool asleep(int e) const { return sleeping_[e]; }
    bool ampResting(int part) const { return amp_[part].resting(); }
    float sendLevel(int part, int e) const { return send_[part][e].tgt; }
    bool ampRunning(int p) const { return amp_[p].running(); }
    // A non-finite or runaway sample was seen in a return: the effects hold garbage that only clear() removes (main loop).
    bool fault() const { return fault_; }

    // The wet signal of a 100% send of noise through each effect, against the noise itself (test_audio.cpp measures it): the reverb's
    // network is quieter than its input, so its return is lifted to about -5 dB (delay -1 dB, chorus -4 dB).
    static constexpr float kReverbTrim = 1.4f;

   private:
    void block(float* const partL[kParts], float* const partR[kParts], const float gain[kParts], uint32_t sounding, float* outL, float* outR, int n);
    float sr_ = 44100.0f;
    FxDelay delay_;
    FxReverb reverb_;
    FxChorus chorus_;
    FxComp comp_;
    FxAmp amp_[kParts];
    Smooth send_[kParts][EFFECTS];
    bool sleeping_[EFFECTS] = {true, true, true};
    int quiet_[EFFECTS] = {};
    volatile bool fault_ = false;
    float peak_[EFFECTS] = {};
    bool inited_ = false;
    app::FxSettings applied_;
    int bpm_ = 0;
    bool haveApplied_ = false;
    uint16_t degFx_ = 0, degAmp_[kParts] = {};
    bool degDirty_ = false;
    Smooth byp_[EFFECTS];       // 1: in use, 0: bypassed by the degrade ladder
    bool wasByp_[EFFECTS] = {};
    float busL_[kChunk], busR_[kChunk], retL_[kChunk], retR_[kChunk];
};

}  // namespace fx

#endif
