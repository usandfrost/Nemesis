#ifndef __AUDIO_SAMPLER_H
#define __AUDIO_SAMPLER_H

#include <stdint.h>

#include "app/Model.h"
#include "app/SampleLib.h"

// The sampler instrument: a part whose lanes each play their own sample (the mockup's playSample). Each voice reads a mono 16-bit
// sample from SDRAM with linear interpolation, through a soft clip, a state-variable filter whose cutoff sweeps down from an envelope
// peak, a one-pole high-pass, an optional bit/rate reducer, an ADSR and an equal-power pan. Audio interrupt only; never allocates.
namespace audio {

// Everything one hit needs, worked out from the lane's pages and the step's locks when the note starts.
struct SampleHit {
    const app::SampleItem* smp = nullptr;
    const int16_t* data = nullptr; // the audio as it was when the hit started (the library's cache may be refilled later)
    uint32_t length = 0;
    double start = 0, end = 0;     // frames of the playing region
    double step = 1;               // frames per output sample (pitch and the sample's own rate)
    bool reverse = false;
    uint8_t mode = 0;              // 0 one-shot (plays the region, ignores the gate), 1 gated, 2 loop
    float attack = 0, decayTau = 0, sustain = 1, releaseTau = 0;   // seconds (taus are the exponential time constants)
    float level = 1, pan = 0;      // pan -1..1
    uint8_t choke = 0;             // 0 none, 1..4 a group: a new hit cuts the others in it
    uint8_t cutLane = 0;           // a slice: its lane + 1. The next slice of the lane cuts it (a chopped loop is one voice)
    double loopFade = 0;           // frames at the end of a LOOP that melt into what comes before its start (0: a plain jump)
    double loopPct = 0;            // the fade as a share of the loop (0: it is in frames)
    double loopTurn = 0, loopLen = 0;   // where the end goes back to: the start, unless the sample has too little before it for the fade; and the lap that makes
    uint8_t loopCurve = 0;         // app::LoopCurve
    float loopCorr = 0;            // 0..1: how alike the two ends are (CURVE MATCH)
    int16_t wantS = 0, wantE = 0;  // the START and END this hit asked for, and how it wants them improved (0: as they are)
    uint8_t autoLoop = 0, search = 0, fadeKey = 0;
    double sliceTail = 0;          // a slice: frames past its end it runs on and fades, so the next one crossfades with it instead of leaving a hole
    uint8_t model = 0;             // 0 LP, 1 BP, 2 HP
    float cutoff = 18000, peak = 18000, q = 0.7f;   // the cutoff starts at `peak` and settles at `cutoff` in 120 ms
    float drive = 0, hpf = 0;      // drive 0..1, high-pass Hz (0 off)
    float bits = 16, srHz = 0, gritMix = 1;   // srHz 0 = no rate reduction
    float compand = 0, clip = 0;   // GRIT: mu-law around the bits, and a soft clip
    float drift = 0, wow = 0;      // GRIT: the pitch wanders, quickly and slowly
    float loopErr = 0;             // GRIT: a loop's points land a little off on each lap
    float lfoHz = 0, lfoDepth = 0; // MOD
    uint8_t lfoDest = 0, lfoShape = 0;   // 0 cutoff, 1 pitch, 2 amp; app::kWaves
    float wheelAmt = 0, pressAmt = 0;    // MOD WHL and AFTERT, on the cutoff
};

// `vel` is the step's velocity scaled by the lane level (0..127), `partLevel` 0..1. Returns false if the lane has no playable sample.
bool makeHit(const app::Lane& lane, const app::Lock* locks, int nLocks, int semis, float vel, float partLevel, const app::SampleLib& lib,
             double sampleRate, SampleHit& out);

// Narrows a hit to a slice: its region, its own pitch, level and direction, and FIT. `flip`: this hit plays the other way round. False: the map is not for this
// sample as it is now, or has no such slice. The hit must not play then: the whole sample would sound where a slice was meant.
bool applySlice(SampleHit& h, const app::Lane& lane, const app::SliceMap& m, int slice, bool flip, int bpm, int laneIndex);

// A hit that loops, on the points AUTO found for its sample. If they have not been found yet it plays on the knobs' points.
void applyLoopFit(SampleHit& h, const app::Project& pj, int sample);

class Sampler {
   public:
    static constexpr int kVoices = 8;

    void init(double sampleRate);
    // Starts a hit `offset` samples into the next render; at most `maxVoices` sound at once (the oldest is faded out). Returns a
    // handle for release (unique per hit), or -1.
    int32_t start(const SampleHit& h, int maxVoices, int offset);
    void release(int32_t handle, int offset);   // the step's gate ended (a one-shot ignores it)
    void render(float* left, float* right, int frames);   // adds into the buffers
    void setWheel(float v) { wheel_ = v; }
    void setPressure(float v) { pressure_ = v; }
    void stopAll();                              // a fast fade on everything
    void cut(int cutLane);                       // a fast fade on one lane's slices
    int active() const;

   private:
    struct Voice {
        bool on = false;
        uint32_t gen = 0;
        uint32_t age = 0;
        int startIn = 0, releaseIn = -1;   // samples until the hit starts / the gate ends (-1: not scheduled)
        SampleHit h;
        double pos = 0;
        int stage = 0;                     // 0 waiting, 1 attack, 2 decay/sustain, 3 release, 4 fast fade (stolen / choked)
        float env = 0, fadeStep = 0;
        float sweepT = 0;                  // seconds since the start, for the cutoff sweep
        float g = 0, k = 0, a1 = 1, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;   // SVF coefficients and state
        float attackStep = 1, decayK = 0, releaseK = 0, oneShotK = 0, levels = 0, holdInc = 1;   // worked out once per hit
        double tail = 0;           // a one-shot's own release starts this many frames before the end of its region
        bool swept = false;        // the cutoff sweep is over: the coefficients no longer change
        float hpA = 0, hpX = 0, hpY = 0;
        float holdVal = 0, holdPhase = 0;  // rate reducer
        float gl = 1, gr = 1;
        int coefIn = 0;
        bool moving = false;       // something in MOD or GRIT moves this hit
        int modIn = 0;
        float lfoPhase = 0, wanderT = 0, ampMod = 1, cutMul = 1;
        double stepMul = 1;
        uint32_t rng = 1;
        float mu = 0, lmuInv = 0;
        int cutIn = -1;            // a slice: samples until the next slice of its lane cuts it (-1: nothing yet)
        float cutSecs = 0.002f;    // ... over this long: the attack of the slice that takes over, so at the loop's own tempo the two add up to the loop
    };
    void fade(Voice& v, float seconds);
    float sampleAt(const Voice& v, double pos) const;
    void updateFilter(Voice& v);
    void modulate(Voice& v);
    float jitter(Voice& v);
    double sr_ = 44100.0;
    Voice v_[kVoices];
    uint32_t gen_ = 1, age_ = 0;
    float wheel_ = 0, pressure_ = 0;
};

}  // namespace audio

#endif
