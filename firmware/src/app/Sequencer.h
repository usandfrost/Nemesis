#ifndef __APP_SEQUENCER_H
#define __APP_SEQUENCER_H

#include <stdint.h>

#include "app/Model.h"
#include "app/Ui.h"

// The step clock: swing, Euclid rhythms with their tapped overlays, probability, chords (strummed or not), arpeggios, the project scale,
// queued pattern changes and songs. A port of the mockup's sched()/stepEvents()/patternEnd(), but sample-accurate: it runs at the start
// of every audio block, in the audio interrupt, and hands the instrument the notes that start or end inside that block with their
// sample offsets. Never allocates, bounded work per block.
namespace app {

// One note as the sequencer starts it. A CATHODE part gets a MIDI note (the scale already applied); a sampler part gets the pitch
// offset in semitones and plays the lane's own sample (pattern + lane say which).
struct NoteStart {
    uint8_t part;
    uint8_t pattern, lane;
    int8_t pitch;          // a synth lane: the MIDI note; a sampler lane: semitones from the sample's pitch
    uint8_t sampler;       // which of the two `pitch` is (the part's type when the step was read)
    float vel01;
    int8_t slice;          // a sampler lane: the slice it plays, -1 the sample as ever
    uint8_t sliceRev;      // 1: this hit plays its slice the other way round (RND REV)
    uint8_t nLocks;
    Lock locks[kLocksPerStep];
};

class NoteSink {
   public:
    virtual ~NoteSink() {}
    // Returns a handle for the matching noteOff, or -1 if nothing started. A handle names THIS note, never just its pitch or its voice
    // slot: the sink makes it unique (a voice number plus a generation), so a late end cannot release a newer note that has the same
    // pitch or was given the same voice. noteOff for a note that is already gone (stolen, choked) does nothing.
    virtual int32_t noteOn(const NoteStart& n, int offset) = 0;
    virtual void noteOff(int part, int32_t handle, int offset) = 0;
    virtual void sliceCut(int part, int lane) {}   // a lane playing a loop by itself has stopped: what it was playing ends now
    // A step's effects locks (a send, the amp's knobs; Params.h PG_FXSEND / PG_FXAMP) take hold at the step and let go at its end. `lane` keeps one
    // set per lane; `token` names this set, so the end of an earlier step cannot let go of the next one. fxReleaseAll: the sequencer stopped.
    virtual void fxSet(int lane, int part, const Lock* locks, int n, uint32_t token, int offset) {}
    virtual void fxRelease(int lane, uint32_t token, int offset) {}
    virtual void fxReleaseAll() {}
};

class Sequencer {
   public:
    Sequencer(Project& pj, Transport& tp, NoteSink& sink) : pj_(pj), tp_(tp), sink_(sink) {}
    void setSampleRate(double sr) { sr_ = sr; }

    // The block [now, now + frames). `nowMs` stamps the lane hit flashes for the LEDs.
    void process(int frames, uint32_t nowMs);

    uint64_t sampleClock() const { return clock_; }
    void reseed() { seed_ = 0x9e3779b9u; }   // a render: the same chances each time
    int pending() const { return count_; }

   private:
    struct Ev {
        uint64_t at;          // absolute sample time
        uint32_t seq;         // order of pushing: equal times keep their order
        uint32_t gate;        // samples, for a start
        int32_t handle;       // for an end
        uint32_t token;       // an effects lock: the set it belongs to
        uint8_t fx;           // 0 a note (start or end), 1 an effects lock takes hold, 2 it lets go
        bool start;
        NoteStart note;
    };
    static constexpr int kQueue = 256;   // a binary heap: push and pop are O(log n), so a busy block stays cheap in the interrupt

    void step(uint64_t when, uint32_t nowMs);
    void syncLane(int li, const Lane& ln, int pat, uint64_t when, uint32_t nowMs, int laneStep);
    int pickSlice(const SliceMap& m, const Lane& ln, const Step& st, int pos, uint8_t& rev);
    void pushFxLocks(int li, int part, const Lock* locks, int n, uint64_t when);
    static bool before(const Ev& a, const Ev& b) { return a.at < b.at || (a.at == b.at && a.seq < b.seq); }
    void pop();
    bool patternEnd();            // false: a song that stops has ended
    void push(const Ev& e);
    void dropStarts();
    void deliver(uint64_t end);
    double rnd();
    int swing() const { return tp_.clockExt ? 0 : pj_.swing; }   // a clock from outside sets the time

    Project& pj_;
    Transport& tp_;
    NoteSink& sink_;
    double sr_ = 44100.0;
    uint64_t clock_ = 0;
    double grid_ = 0;             // the unswung time of the next step (samples)
    bool running_ = false;
    bool ending_ = false;         // the last step of a song that stops has played; stop at grid_
    uint32_t gen_ = 0;            // Transport::playGen when this run started
    uint32_t seq_ = 0;
    uint32_t fxToken_ = 0;
    Ev q_[kQueue];
    int count_ = 0;
    uint32_t seed_ = 0x9e3779b9u;
};

}  // namespace app

#endif
