#ifndef __AUDIO_INSTRUMENT_H
#define __AUDIO_INSTRUMENT_H

#include <stdint.h>

#include "app/CostBridge.h"
#include "app/Model.h"
#include "app/SampleLib.h"
#include "app/Sequencer.h"
#include "app/Ui.h"
#include "audio/AudioRenderer.h"
#include "audio/PatchMap.h"
#include "audio/Sampler.h"
#include "cost/Governor.h"
#include "fx/FxSection.h"
#ifndef DBOY_LEAN_DEFAULT
#define DBOY_LEAN_DEFAULT 0   // 1: the CATHODE parts start on the lean voice (the board build's DBOY_LEAN_DEFAULT option)
#endif
#ifndef DBOY_LEAN
#define DBOY_LEAN 0   // 1: the lean voice (lean/LeanPart.h) is built in, and setLean() can play the CATHODE parts through it
#endif
#if DBOY_LEAN
#include "lean/LeanPart.h"
#endif
#ifndef DBOY_OLD_ENGINE
#define DBOY_OLD_ENGINE 1   // 0: only the lean voice is built in (the board: seven of the CATHODE engine would not fit the SDRAM)
#endif
#include "midi/MidiOutRing.h"
#include "midi/MidiTypes.h"

// The whole instrument behind the audio interrupt: the parts (each a synth or a sampler, as the project says), the
// sequencer that plays them, the previews the UI asks for, live MIDI notes, the mix, and one degrade ladder over all the parts walked
// by one governor (the order the MIXER's second view shows). No HAL in here: the board gives it the CPU load of each block, the host
// tests render it into WAV files.
namespace audio {

class Instrument : public AudioRenderer, public app::NoteSink {
   public:
    // Allocates the engines and the tables (SDRAM on the board): before the audio starts.
    void init(double sampleRate, app::Project& pj, app::Transport& tp, const app::SampleLib& lib);
    // The share of the last block's budget that rendering took (0..1+); the board passes the cycle counter's reading.
    void setLoadSource(float (*fn)()) { loadFn_ = fn; }
    void setClockMs(uint32_t (*fn)()) { msFn_ = fn; }
    // MIDI: the queue the UART fills (drained at the start of every block, so a note lands within one block) and the function that
    // sends a 3-byte message (called from the audio interrupt). Lanes play and send on their channels (Project::midi, app/MidiMap.h).
    // `out` queues whole 3-byte messages, all or none, and returns false when it did not (the socket is behind: a note-on is dropped,
    // and its note-off is then never sent). It must always take a note-off while fewer than 64 notes are out (midi/MidiOutRing.h).
    void setMidi(midi::SpscQueue<64>* in, bool (*out)(const uint8_t* bytes, int n)) { midiIn_ = in; midiOut_ = out; }
    // The effects (fx/FxSection.h): main loop. Empties every delay line and reverb tail after a project load (they hold the old project's
    // sound). The audio interrupt skips the effects for the moment this takes (a few ms); it returns when they are empty.
    void clearFx();
    const fx::FxSection& effects() const { return fx_; }
    SharedTables& sharedTables() { return *tables_; }   // the wavetable banks the main loop publishes into
    bool fxFault() const { return fx_.fault(); }   // an effect holds garbage: the main loop calls clearFx()
    // The part on screen: a note on a channel no lane uses plays it.
    void setLivePart(int part) { livePart_ = part; }
#if DBOY_LEAN
    // Which engine the CATHODE parts play through: the full engine (default) or the lean voice. Every CATHODE note ends with the switch (nothing is left to release a voice of the other engine); the sound that is left fades with the old engine's reset.
    void setLean(bool on);
    bool lean() const { return lean_; }
#endif

    // AudioRenderer (audio interrupt)
    void beginBlock() override;
    void render(float* left, float* right, int frames) override;

    // app::NoteSink (called by the sequencer inside render)
    int32_t noteOn(const app::NoteStart& n, int offset) override;
    void noteOff(int part, int32_t handle, int offset) override;
    void sliceCut(int part, int lane) override { sampler_[part].cut(lane + 1); }
    // a step's effects locks (sends, the amp's knobs): the project's settings with them laid over for the step's length
    void fxSet(int lane, int part, const app::Lock* locks, int n, uint32_t token, int offset) override;
    void fxRelease(int lane, uint32_t token, int offset) override;
    void fxReleaseAll() override;

    // ---- requests from the main loop (a lock-free queue the audio interrupt drains)
    void auditionSample(int sample);
    void auditionSlice(int sample, int slice);   // one slice of a sample the project has cut
    void previewPreset(int part, int preset);
    void stopAllSound();
    void stopSamplers();   // the sampler parts and the audition fade out (6 ms); the synth parts play on
    // The interrupt has taken every stop asked for so far (and the voices are fading out, 6 ms): what may overwrite sample memory
    // waits for this.
    bool stopsTaken() const { return stopsTaken_ == stopsAsked_; }
    void setAutoDegrade(bool on) { autoDegrade_ = on; }
    // A render (SongExport): only this lane's notes and effects locks get through, -1 all of them. The sequencer still plays every lane, so its chances fall the same.
    // The SYNC sockets. `in`: the input's level; `out`: the output's pulse and gate. Read and written from the audio block, so they must be quick
    void setSyncPins(bool (*in)(), void (*out)(bool pulse, bool gate)) { syncIn_ = in; syncOut_ = out; }
    void setClockInput(bool on) { clockIn_ = on; }   // a clock can arrive here. Not in a browser: there CLOCK stays inside, or PLAY would wait for nothing
    void setOnlyLane(int lane) { onlyLane_ = lane; }
    void setRendering(bool on) { rendering_ = on; }   // nothing goes out of the MIDI socket and what comes in is dropped: a render is not a performance
    bool autoDegrade() const { return autoDegrade_; }
    void reseed() { if (seq_) seq_->reseed(); }
#if DBOY_LEAN
    // The CATHODE parts play through the lean voice (true) or the CATHODE engine, taken by the audio interrupt at the start of a block (the board's debug event).
    // setLean() itself is for a caller that is the only thread (the host tests, the web build).
    void requestLean(bool on) { push({ReqLean, 0, (int16_t)(on ? 1 : 0)}); }
#endif

    // ---- status for the UI (any thread; a word each)
    int activeVoices(int part) const { return active_[part]; }
    // Where the audio block's cycles go, for the CPU meter and the debug mailbox. The board hands in the cycle counter; the host leaves it null and
    // every number stays 0. Running sums, written in the audio interrupt (a word-sized tear in a statistic is harmless).
    struct SectionCycles {
        uint64_t part[app::kParts] = {}, sampler = 0, fx = 0, seq = 0, patchSum = 0, noteOnSum = 0;
        uint32_t blocks = 0, starts = 0, noteOnMax = 0, patchMax = 0;   // a note start: the engine's noteOn, then the patch applied to its voices
    };
    void setCycleCounter(uint32_t (*fn)()) { cyclesFn_ = fn; }
    const SectionCycles& sectionCycles() const { return sec_; }
    float cpuLoad() const { return governor_.smoothedLoad(); }
    int degradeLevel() const { return level_; }
    float peak(int ch) const { return peak_[ch]; }   // the output's peak since the last clearPeaks (the level LEDs)
    void clearPeaks() { peak_[0] = peak_[1] = 0.0f; }

   private:
    // A note on a CATHODE part. Every allocation of a slot gets a new generation; an event or a handle that names an older
    // generation does nothing (the slot has been reused).
    struct Note {
        bool used = false, live = false, preview = false;
        uint16_t gen = 0;
        int8_t midi = 0, voice = -1;
        uint64_t group = 0;
        uint32_t order = 0;
    };
    static constexpr int kNotes = 24;
    static constexpr int32_t kCathodeTag = 1 << 30;   // a CATHODE note's handle (a sampler hit's handle has this bit clear)
    // a CATHODE note event waiting for its sample inside the block
    struct Ev {
        uint8_t on, part, note;   // note: index into notes_[part]
        uint16_t gen;
        int16_t offset;
        float vel;
        uint8_t nLocks;
        app::Lock locks[app::kLocksPerStep];
    };
    // what a voice of a CATHODE part is playing with: the part's patch, a p-locked note's, or a preview's
    enum VoicePatch : uint8_t { VpBase = 0, VpLocks, VpPreview };
    struct VoiceState {
        uint8_t kind = VpBase;
        uint8_t nLocks = 0;
        app::Lock locks[app::kLocksPerStep];
    };
    struct Req {
        uint8_t type, part;
        int16_t a;
    };
    enum : uint8_t { ReqAudition = 1, ReqPreview, ReqStop, ReqStopSamplers, ReqLean, ReqAuditionSlice };
    // a note being played from MIDI in, so its note-off finds it
    struct MidiHeld {
        bool used = false;
        uint8_t ch = 0, note = 0, part = 0;
        bool cathode = false;
        int32_t handle = -1;      // a sampler hit's handle, or a CATHODE note's slot
        uint16_t gen = 0;
        bool sustained = false;   // its key is up, and the pedal holds it
    };
    // a note sent out, so its note-off goes to the channel and note it went out on (the table may change meanwhile)
    // A slot stays taken until the sequencer ends its note, even after STOP has already sent the note-off (`flushed`): the
    // sequencer still holds the handle, and a slot given to a new note meanwhile would be ended by the old note's end.
    struct MidiSent {
        bool used = false, flushed = false;
        uint8_t ch = 0, note = 0;
    };
    static constexpr int kMidiSent = midi::kMaxNotesOut;   // (the out ring keeps room for all their note-offs)
    static_assert(kMidiSent < 64, "a sent note's slot + 1 must fit the handle's 6 bits");
    static constexpr int kSentShift = 23;             // a handle's bits 23..28: the sent note's slot + 1
    static constexpr int32_t kNoInternal = 1 << 29;   // the handle has only a sent note (the part did not sound)
    // one note of a preset preview
    struct PreviewNote {
        uint64_t at = 0, off = 0;
        int8_t midi = -1;       // -1: none
        int8_t slot = -1;
        uint16_t gen = 0;
        uint8_t state = 2;      // 0 waiting, 1 sounding, 2 done
    };

    void takeRequests();
    void takeMidi();
    void midiNoteOn(int ch, int note, int vel);
    void midiNoteOff(int ch, int note);
    int sendOn(const app::NoteStart& n);      // a sent-note slot + 1, or 0
    void sendOff(int slot);
    void sendAllOff();
    void syncParts();
    int allocNote(int part, int midi, bool live, bool preview);   // a slot, or -1
    bool queue(const Ev& e);
    void startCathode(const Ev& e);
    void endCathode(int part, int note, int gen = -1);
    void endPreview();
    void renderCathode(int from, int to);
    int heldNotes(int part) const;
    void applyVoicePatch(int part, int v, float vel01);   // brings voice v to the patch its note plays with
    bool push(const Req& r);

    app::Project* pj_ = nullptr;
    app::Transport* tp_ = nullptr;
    const app::SampleLib* lib_ = nullptr;
    app::Sequencer* seq_ = nullptr;
    volatile int onlyLane_ = -1;
    volatile bool rendering_ = false;
    void runClock();
    void clockTick();
    void addExtStep();
    void releaseHeld(MidiHeld& m);
    void sendByte(uint8_t b);
    bool (*syncIn_)() = nullptr;
    void (*syncOut_)(bool, bool) = nullptr;
    uint16_t sustain_ = 0;            // the channels whose pedal is down, bit 0 is channel 1
    uint32_t tick_ = 0, outTicks_ = 0, sinceTick_ = 0, sincePulse_ = 0, pulsePeriod_ = 0, beatFrames_ = 0, beatTicks_ = 0;
    int32_t halfIn_ = 0, pulseLeft_ = 0;
    double outIn_ = 0;
    bool syncHigh_ = false, wasPlaying_ = false, tickSeen_ = false, halfOwed_ = false, clockIn_ = false;
    int lastSource_ = 0;
    int32_t lastStep_ = -1;
    uint32_t blockFrames_ = 32;
    bool following() const { return clockIn_ && !rendering_ && pj_->parts[0].pages[app::PG_GLOB][4] == 1; }
    void setFollowed(int bpm);
    void clockReset();
    double sr_ = 44100.0;
    float (*loadFn_)() = nullptr;
    uint32_t (*msFn_)() = nullptr;

    SharedTables* tables_ = nullptr;
    uint32_t tablesSeen_ = 0;       // SharedTables::bankGen as the last block saw it
#if DBOY_OLD_ENGINE
    CathodePart cathode_[app::kParts];
#endif
#if DBOY_LEAN
    lean::Part leanPart_[app::kParts];
    bool lean_ = DBOY_LEAN_DEFAULT != 0;
    lean::Params leanBase_[app::kParts];   // the parts' patches in the lean voice's units (made when a patch changes, not at every note)
#endif
    void ctlBend(int p, float v);
    void ctlWheel(int p, float v);
    void ctlPressure(int p, float v);
    Sampler sampler_[app::kParts];
    Sampler audition_;
    Patch base_[app::kParts];       // the part's own patch (its pages, the degrade bypasses), as its unlocked voices have it
    Patch scratch_;
    VoiceState voices_[app::kParts][CathodePart::kVoices];
    uint8_t type_[app::kParts];
    uint8_t engineVoices_[app::kParts] = {};
    Note notes_[app::kParts][kNotes];
    uint32_t noteOrder_ = 0;
    Ev ev_[64];
    int nev_ = 0;

    // The effects: with none in use the parts are summed straight into the output as before (bit for bit); otherwise each part has its
    // own buffer pair, which the section amps, sends and mixes.
    static constexpr int kFxFrames = 128;
    fx::FxSection fx_;
    float partL_[app::kParts][kFxFrames], partR_[app::kParts][kFxFrames];
    float* outL_[app::kParts] = {};
    float* outR_[app::kParts] = {};
    volatile uint8_t fxHoldReq_ = 0, fxHoldAck_ = 0;
    bool useFx_ = false;            // this block's parts go through the effects section
    uint32_t sounding_ = 0;         // a bit per part that rendered anything into its buffers this block
    uint64_t previewFxUntil_ = 0;   // the preview's effects stay on until its notes have released (a few seconds after the last note-off)
    struct FxOverride { bool on = false; uint8_t part = 0, n = 0; uint32_t token = 0, order = 0; app::Lock locks[app::kLocksPerStep]; };
    FxOverride fxOv_[app::kLanes];   // per lane: the effects locks of the step it is playing
    uint32_t fxOvOrder_ = 0;
    int fxOvOn_ = 0;
    app::FxSettings fxEff_;          // the project's effects with the locks laid over (when any is on)
    app::FxSettings previewFx_;     // the effects while a preset is previewed: the project's with the preset's sends, amp and shared settings on its part

    // the degrade ladder over the parts
    app::Ladder ladder_;
    uint32_t ladderSig_ = 0;
    cost::Governor governor_;
    int level_ = 0;
    ActionMask actions_[app::kParts] = {};
    uint8_t cap_[app::kParts];
    // what the last block saw: most blocks nothing has changed, and a part's description and patch are not worked out again for nothing
    app::Part seenPart_[app::kParts];
    uint8_t seenAmp_[app::kParts];
    cost::PartDesc desc_[app::kParts];
    uint8_t seenFx_ = 0xFF, seenIdle_ = 0xFF;
    int seenLevel_ = -1;
    bool autoDegrade_ = true;

    // a preset preview: three notes on the part with the preset's patch
    Patch previewPatch_;
    int previewPart_ = -1;
    PreviewNote preview_[3];
    uint64_t clock_ = 0;

    // main loop -> audio interrupt
    Req q_[32];
    volatile uint32_t qHead_ = 0, qTail_ = 0;

    midi::SpscQueue<64>* midiIn_ = nullptr;
    bool (*midiOut_)(const uint8_t*, int) = nullptr;
    void sendNoteOff(int ch, int note) { const uint8_t m[3] = {(uint8_t)(0x80 | (ch - 1)), (uint8_t)note, 0}; midiOut_(m, 3); }
    bool soundingOut(int ch, int note, int except) const;   // another sent note with that channel and note is still on
    volatile int livePart_ = 0;
    MidiHeld held_[32];
    MidiSent sent_[kMidiSent];

    volatile int active_[app::kParts] = {};
    uint32_t (*cyclesFn_)() = nullptr;
    SectionCycles sec_;
    volatile uint32_t stopsAsked_ = 0, stopsTaken_ = 0;
    volatile float peak_[2] = {};
    float partGain_[app::kParts] = {};
};

}  // namespace audio

#endif
