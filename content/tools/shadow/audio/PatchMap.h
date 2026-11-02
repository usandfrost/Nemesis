#ifndef __AUDIO_PATCH_MAP_H
#define __AUDIO_PATCH_MAP_H

#include <stdint.h>
#include <string>

#include <nemesis/dsp/CathodeVoiceEngine.h>
#include <nemesis/dsp/WavetableOscillator.h>

#include "app/Model.h"
#include "cost/Degrade.h"

// The panel's synth pages (app/Params.h: OSC 1, OSC 2, SUB, FILTER 1, FILTER 2, GRIT, AMP, MOD, GLOBAL) as CATHODE settings. The table of
// what each knob drives is docs/PATCH_MAP.md. Every voice keeps the values it was last given, so a change (a knob, a p-locked note, a
// degrade step) reaches the engine as the few setters that differ, never a full re-patch.
namespace audio {

// CONTENT-PIPELINE SHADOW of firmware/src/audio/PatchMap.h (never linked into the firmware). It is the firmware header plus the wavetable
// prototype described in content/out/FIRMWARE_INTEGRATION.md: WAVE >= 4 selects slot WAVE-4 of the SD wavetable set, PD SHAPE is its
// position, and the LFO / ENV>PD routes that target PD AMT move the position instead. Everything else is the firmware's code, unchanged.
//
// The tables every voice reads: the four basic shapes (saw, square, triangle, sine; the WAVE knob), the pulse saw, the sub.
struct SharedTables {
    static constexpr int kExt = 64;
    nemesis::dsp::Wavetable shapes, pulse, sub;
    nemesis::dsp::Wavetable* ext[kExt] = {};   // SD wavetables, built on first use (set.txt: slot catalogue file)
    int extCatalogue[kExt] = {};
    std::string extFile[kExt];
    int extCount = 0;
    void build();   // allocates and runs FFTs: once at boot, before the audio starts
    void loadSet(const char* dir);   // reads dir/set.txt (host: DBOY_WT_DIR)
    const nemesis::dsp::Wavetable* table(int slot);
    std::string dir;
};

// What one voice plays: the part's pages with a step's locks over them, then the CPU governor's bypasses on top.
struct Patch {
    int16_t p[app::kSynthPages][app::kParamsPerPage];
    uint8_t f1, f2;    // the filter models as cost::Filter (a light fallback can be a model the MODEL knob does not list)
};

// Bitmask of cost::Action that the degrade ladder has applied to a part (1 << action).
using ActionMask = uint16_t;
ActionMask actionsAt(const cost::Step* ladder, int steps, int level, int part);

void makePatch(const app::Part& part, const app::Lock* locks, int nLocks, ActionMask degraded, Patch& out);

class CathodePart {
   public:
    static constexpr int kVoices = nemesis::dsp::CathodeVoiceEngine::kMaxVoices;
    static constexpr int kPoly = 5;   // the most voices a CATHODE part plays (MIXER: VOICES 1..5)

    // Allocates the engine (SDRAM heap) and binds the shared tables to every voice: main loop, before the audio starts.
    void init(double sampleRate, SharedTables& tables);
    nemesis::dsp::CathodeVoiceEngine& engine() { return *engine_; }

    // Voice `v` is to play with patch `p` (the part's, or a note's with its p-locks): the setters that differ are sent. `vel01` >= 0
    // when a note is starting (VEL>ATK scales its attack). Audio interrupt.
    void applyVoice(int v, const Patch& p, float vel01);
    void setBendRange(const Patch& base);   // the engine's bend range (GLOBAL BEND RNG)

   private:
    void set(int v, int key, const Patch& P);
    void setFilter1(int v, const Patch& P);
    nemesis::dsp::CathodeVoiceEngine* engine_ = nullptr;
    SharedTables* tables_ = nullptr;
    int8_t bound_[kVoices][2];   // wavetable slot bound to each oscillator (-1: the basic shapes)
    int16_t applied_[kVoices][app::kSynthPages][app::kParamsPerPage];
    uint8_t appliedF1_[kVoices], appliedF2_[kVoices];
    bool velAttack_[kVoices];   // the attack was scaled by a note's velocity (VEL>ATK): the next note sets it again
    float bendRange_ = -1;
};

}  // namespace audio

#endif
