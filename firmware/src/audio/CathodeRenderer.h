#ifndef __CATHODE_RENDERER_H
#define __CATHODE_RENDERER_H

#include <nemesis/dsp/CathodeVoiceEngine.h>

#include <atomic>

#include "audio/AudioEngine.h"
#include "cost/Degrade.h"
#include "cost/Governor.h"
#include "midi/MidiIn.h"
#include "midi/MidiRouter.h"

// Adapts the original CATHODE voice engine (read-only, built from UPSTREAM_OLD) to the audio engine.
// The engine is sample-by-sample, so a block is just a loop of processSample().
class CathodeRenderer : public AudioRenderer {
   public:
    // Not real-time safe (allocates): call once at start-up, before the I2S DMA runs.
    void init(double sampleRate, int voices);

    void setLevel(float level) { level_ = level; }

    // A chord that plays by itself for 1.5 s after boot, so the first power-on proves the whole audio path without a MIDI
    // keyboard. It runs inside the audio context (beginBlock), never from the main loop.
    void setDemoChord(bool on) { demoChord_ = on; }

    void beginBlock() override;
    void render(float* left, float* right, int frames) override;

    // midi::Router sink. Only ever called from beginBlock(), i.e. the audio interrupt.
    void noteOn(int note, float velocity01);
    void noteOff(int note);
    void pitchBend(float bend) { engine_->setPitchBend(bend); }
    void modWheel(float wheel01) { engine_->setModWheel(wheel01); }
    void channelPressure(float pressure01) { engine_->setChannelPressure(pressure01); }
    void allNotesOff();

    // ---- CPU cost model, one-heavy-filter rule and degrade governor (firmware/src/cost) ----
    // The cost-relevant features of the running patch. The parameter layer will call this when a lane's sound changes; the heavy-filter rule is
    // enforced here so no caller can create a second heavy filter.
    // MAIN LOOP ONLY (any context below the audio interrupt). It only posts the request (a sequence lock, no engine access); the audio
    // interrupt picks it up at the start of the next block, so the ladder, the governor and the engine are only ever touched from there.
    void setPatchCost(const cost::VoiceDesc& voice, int voices, int priority = 0);
    float currentCost() const { return cost::partCost(effective_, costSettings_); }   // in voice units, at the current degrade level
    float baseCost() const { return cost::partCost(base_, costSettings_); }
    int degradeLevel() const { return degradeLevel_; }
    int degradeSteps() const { return ladderSteps_; }

   private:
    // The engine object is hundreds of KB (kMaxVoices voices, each with a stereo right chain), more than internal RAM, so it is
    // allocated on the SDRAM heap in init() instead of living in .bss.
    nemesis::dsp::CathodeVoiceEngine* engine_ = nullptr;
    float level_ = 0.3f;
    midi::Router<CathodeRenderer> router_;
    bool demoChord_ = false;
    uint32_t blocks_ = 0;   // audio blocks since start, for the demo chord

    // patch requests from the main loop (sequence lock: odd = being written)
    struct PatchRequest {
        cost::VoiceDesc voice;
        int voices = 1;
        int priority = 0;
    };
    PatchRequest pending_;
    std::atomic<uint32_t> pendingSeq_{0};
    uint32_t appliedSeq_ = 0;
    void takePatchRequest();
    void applyPatch(const PatchRequest& r);
    float releaseSeconds_ = 0.2f;         // the amp release, to hold the governor while a released voice fades

    // degrade state
    void rebuildLadder();
    void applyLevel(int level);
    void applyVoiceDesc(const cost::VoiceDesc& d, const cost::VoiceDesc* prev);
    bool engineHasDesc_ = false;          // effective_.voice is on the engine (else the next apply pushes every field)
    void releaseOldest();
    cost::PartDesc base_;                 // the patch as the player set it
    cost::PartDesc effective_;            // base_ with the first `level_` ladder steps applied
    cost::Settings costSettings_;
    cost::Step ladder_[cost::kMaxSteps];
    int ladderSteps_ = 0;
    int degradeLevel_ = 0;                // what is applied to the engine now (the governor's wish may differ for one block)
    cost::Governor governor_;
    uint8_t held_[16] = {};               // sounding notes, oldest first
    int heldCount_ = 0;
    int voiceCap_ = 5;                    // polyphony after degrading
};

#endif
