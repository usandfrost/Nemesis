#ifndef __APP_SERVICES_H
#define __APP_SERVICES_H

#include <stdint.h>

#include "app/Model.h"

// What the UI needs from the rest of the instrument: time, sound previews, the SD card and the engine's status. The board implements it
// over the drivers and the audio engine; the host tests implement it in memory.
namespace app {

struct SlotInfo {
    char name[16];
    uint8_t usedPatterns;
    uint8_t songs;          // songs with at least one row
    uint16_t bpm;
};

constexpr int kCardSlots = 30;

// Recording: what the REC screen's knobs say, and how it is going. (audio/Recorder.h does the work on the board.)
struct RecSettings {
    uint8_t source = 0;       // 0 the line input, 1 what the Drumboy is playing
    uint8_t channel = 0;      // 0 L+R, 1 L, 2 R
    uint8_t autoCapture = 0;  // 1: wait for the input to pass the threshold
    uint8_t gain = 0;         // the line input's gain: 0..15, 1.5 dB each
    uint8_t monitor = 0;      // 0..127: how much of the line input goes on to the outputs
    uint8_t stop = 0;         // index into kRecStops
    int8_t thresholdDb = -30; // -60..-6
};
constexpr int kRecStopChoices = 5;
static const float kRecStops[kRecStopChoices] = {0.0f, 0.5f, 1.0f, 2.0f, 4.0f};   // seconds below the threshold that end a take (0: off)
enum RecState : uint8_t { REC_IDLE, REC_ARMED, REC_TAKING, REC_DONE };
struct RecStatus {
    uint8_t state = REC_IDLE;
    bool clipped = false;
    float peak = 0.0f;        // 0..1, since the last look
    uint32_t frames = 0, capacity = 0, rate = 44100;
    const int16_t* data = nullptr;
};

// EXPORT on PROJECT: a song to files
enum ExportKind : uint8_t { EXPORT_WAV, EXPORT_STEMS, EXPORT_MIDI };
constexpr int kExportKinds = 3;
enum ExportResult : uint8_t { EXPORT_RUNNING, EXPORT_DONE, EXPORT_STOPPED, EXPORT_FAILED };
struct ExportStatus {
    uint8_t kind = 0, result = EXPORT_RUNNING, percent = 0, stem = 0, stems = 0;
    uint16_t missing = 0;     // samples the project uses that could not be loaded
};

class Services {
   public:
    virtual ~Services() {}
    virtual uint32_t nowMs() = 0;
    virtual uint32_t random() = 0;                       // uniform 32 bits (Math.random's stand-in)

    // sound previews (BROWSE, the sample knobs)
    virtual void auditionSample(int sample) {}
    virtual const char* auditionProblem() { return nullptr; }   // why the last auditionSample stayed silent, for the screen
    virtual void auditionSlice(int sample, int slice) {}
    virtual void previewPreset(int part, int preset) {}  // three notes in the project key on a copy of the pages; the part is not changed

    // recording
    virtual bool recAvailable() { return false; }
    virtual void recListen(const RecSettings& s, bool on) {}   // the screen is open: the meter runs, the line input is on with this gain and monitor
    virtual bool recStart(const RecSettings& s) { return false; }   // false: no memory free
    virtual const char* recNoRoom() { return "NO MEMORY FREE: SAVE, AND LOAD THE PROJECT AGAIN"; }   // what a refused recStart means here
    virtual void recStop() {}                            // waiting: gives up. Taking: the take ends
    virtual void recStatus(RecStatus& out) {}
    virtual int recKeep() { return -1; }                 // the take becomes a sample (saved to the card if there is one): its number, or -1
    virtual void recDiscard() {}
    virtual bool exportStart(int kind, int song) { return false; }   // false: it did not start, and cardProblem may say why
    virtual bool exportStep(ExportStatus& out) { return false; }     // some more of it, from the main loop. False: it is over, out.result says how
    virtual void exportCancel() {}
    virtual const char* exportWhy() { return cardProblem(); }          // why it did not start, or failed
    virtual const char* exportWhere() { return "FOLDER EXPORT"; }      // for the message at the end
    virtual const char* exportPlace() { return "It goes to the card, in Export, in a folder with the project's name."; }
    virtual int recSaveError() { return 0; }             // why the last kept take is not on the card: 0 it is, -1 no card, else a card error

    // the SD card: the project slots
    virtual bool slotInfo(int slot, SlotInfo& out) = 0;  // false: the slot is empty
    virtual bool saveSlot(int slot, const Project& pj) = 0;   // keeps what was there, for undoSave
    virtual const char* cardProblem() { return nullptr; }   // why the card is not being written to, for the screen
    virtual bool loadFellBack() { return false; }        // the last load's newest save was damaged: this is the one before it
    virtual bool undoSave(int slot) = 0;                 // puts back what saveSlot overwrote (or empties the slot)
    virtual void loadFactory(int i, Project& pj) { makeFactoryProject(i, pj); }
    virtual const Project* peekSlot(int slot, int& missing) = 0;   // reads a slot, touching nothing that plays. nullptr: it cannot be read. missing: samples the project uses that this card does not have. Stays until the next read
    virtual void takePeek(Project& pj) = 0;              // that project becomes `pj`: what plays stops, its samples are loaded
    virtual bool deleteSlot(int slot) = 0;               // false: the project is still there

    // engine status
    virtual int activeVoices(int part) { return 0; }
    virtual float cpuLoad() { return 0.0f; }             // smoothed, 0..1+
    virtual int degradeLevel() { return 0; }             // steps of the ladder applied now
    virtual void setAutoDegrade(bool on) {}
    virtual void wavesChanged() {}                       // a part's oscillators may play other wavetable banks now: load them without waiting for the sweep
    virtual void busy(const char* msg) {}                // before a slow card operation: the screen says so (nothing is drawn while it runs)
    virtual void stopAllSound() {}                       // STOP / project load: release every voice
    // A few microseconds in which the audio interrupt does not run: for an edit the sequencer must see whole (a song row removed and
    // the playing position moved with it). Nothing slow inside.
    virtual void holdAudio(bool hold) {}
};

}  // namespace app

#endif
