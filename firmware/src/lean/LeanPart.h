#ifndef __LEAN_LEAN_PART_H
#define __LEAN_LEAN_PART_H

#include <stdint.h>

#include "audio/PatchMap.h"
#include "lean/LeanDsp.h"

// A synth part's voices in the lean engine (docs/LEAN_VOICE.md). It takes the same audio::Patch as audio::CathodePart (the panel's nine pages), so the
// panel, the presets and the project files stay as they are; only what turns the knobs into sound changes. Voices are rendered one after the other
// over a whole block (everything a voice needs stays in the cache), the modulation is worked out once per kCtl samples and ramped across them.
namespace lean {

constexpr int kMaxVoices = 5;   // a part has 1..5 voices: the panel and the preset format stop there (docs/LEAN_VOICE.md)
constexpr int kMaxUnison = 4;

// What one voice plays, in physical units (built from a Patch by makeParams). Names follow the panel; the laws are docs/LEAN_VOICE.md's.
enum Model : uint8_t { M_LP, M_BP, M_HP, M_NOTCH, M_LADDER, M_ACID, M_SOVIET, M_CLASSIC };

struct Params {
    // oscillators 1 and 2
    const nemesis::dsp::Wavetable* wt[2] = {nullptr, nullptr};
    int16_t bank[2] = {-1, -1};         // the bank an oscillator plays (to find it again when the tables move), -1 = a basic shape
    float pos[2] = {0, 0};              // the table position the knob gives (basic shapes: a frame; a bank: PD SHAPE)
    bool tab = false;                   // an oscillator plays a bank: the position routes (LFO, ENV>PD) move both oscillators' positions
    float level[2] = {0, 0};
    float semis[2] = {0, 0};            // octave, semitone and fine+tune, in semitones
    float ratio[2] = {1, 1};            // 2^(semis/12) and, for the stack's members, 2^(cents/1200): made by makeParams, so a block makes no exp2 for them
    float uniRatio[4] = {1, 1, 1, 1};
    bool sine[2] = {false, false};      // WAVE = SIN: a sine; with phase distortion (pd) when PD AMT is up or the PD amount is modulated
    bool pd[2] = {false, false};
    float pdBase[2] = {0, 0}, pdShape[2] = {0.5f, 0.5f};
    int unison = 1;
    float uniDetune = 0, uniBlend = 0.5f, uniWidth = 0;
    float ring = 0, sync = 0;           // SUB RING (osc2 x osc1 depth) and OSC2 SYNC (osc1 restarted by osc2's cycle, the amount a crossfade)
    float subLevel = 0;
    int subOct = -1;
    float noise = 0;
    // filter 1
    uint8_t model = M_LP;
    float cutoff = 1000, q = 0.707f, drive = 0, keyTrack = 0, envAmt = 0, hpf = 0;   // envAmt: matrix units (1 = 4 octaves)
    // filter 2 (own model, base cutoff and Q; it takes filter 1's cutoff modulation) and the filter-1 gain of a parallel pair
    bool f2on = false;
    int f2route = 0;                    // 0 series, 1 parallel
    uint8_t model2 = M_LP;
    float cutoff2 = 1000, q2 = 0.707f, drive2 = 0, balance = 0, keyTrack2 = 0, envAmt2 = 0;   // filter 2 follows filter 1, and these on top of it
    // amp
    // the broken converter (GRIT): bit depth, mu-law companding, sample-and-hold rate, clock drift, and the dry/wet mix. 16 bits at 44.1 kHz with no drift is transparent.
    float gritBits = 16, gritCompand = 0, gritRate = 44100, gritDrift = 0, gritMix = 1, gritWow = 0, gritLoopErr = 0;
    bool gritOn = false;
    float clip = 0;                     // GRIT CLIP: the output drive (gain 1 + 6c into a soft clip, after the envelope and velocity)
    float atkMs = 2, decMs = 100, sustain = 1, relMs = 100, velSens = 0.5f, level_ = 0.5f, pan = 0, velAtk = 0;
    // modulation
    float lfoRate = 1;
    int lfoShape = 0, lfoDest = 0;
    float lfoDepth = 0;                 // u01 of the knob; the destination's own scale is applied where it is used
    float env2Pd = 0, wheel = 0, aftertouch = 0;   // wheel/aftertouch: matrix units
    float bendRange = 2;
};

void makeParams(const audio::Patch& p, const audio::SharedTables& tables, Params& out);

class Part {
   public:
    void init(float sampleRate, const audio::SharedTables& tables);
    // Starts a note on a free voice (the oldest is taken when all are busy). Returns the voice, or -1.
    int noteOn(int note, float vel01, const Params& p);
    void noteOff(int note);
    uint32_t lastOrder() const { return order_; }               // the order stamp of the note just started (to find it again)
    void release(int voice, uint32_t order);                    // end that note if the voice still plays it (a stolen voice does not)
    void setParams(int voice, const Params& p);                 // a knob moved: the voice plays the new sound from now on (its envelope levels carry on)
    void panic();
    void idle(int frames);                                      // no voice sounds: the LFO moves on
    void rebind(const audio::SharedTables& t);                  // the banks moved: point the sounding voices at the tables that are there now
    void setVoices(int n) { cap_ = n < 1 ? 1 : (n > kMaxVoices ? kMaxVoices : n); }
    void setBend(float bend) { bend_ = bend; }   // -1..1
    void setWheel(float v) { wheel_ = v; }
    void setPressure(float v) { pressure_ = v; }
    int activeVoices() const;
    bool voiceUsed(int v) const;
    // Adds `frames` samples (a multiple of kCtl, at most 64) of every sounding voice, times gain, into left/right (not cleared).
    void render(float* left, float* right, int frames, float gain = 1.0f);

   private:
    struct Voice;
    Voice* v_[kMaxVoices] = {};
    const nemesis::dsp::Wavetable* shapes_ = nullptr;   // the basic shapes (the sub's sine)
    float sr_ = 44100.0f;
    int cap_ = kMaxVoices;
    uint32_t order_ = 0, rng_ = 0x2545F491u;
    float bend_ = 0, wheel_ = 0, pressure_ = 0;   // bend -1..1 (times the patch's BEND RNG), wheel and pressure 0..1
    float lfoPhase_ = 0.0f, lfoRate_ = 1.0f;       // the LFO is free-running and shared by the part's voices
    void renderVoice(Voice& v, float* left, float* right, int frames);
    float gain_ = 1.0f;
    friend struct VoiceAccess;
};

}  // namespace lean

#endif
