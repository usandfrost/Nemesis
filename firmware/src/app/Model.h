#ifndef __APP_MODEL_H
#define __APP_MODEL_H

#include <stdint.h>
#include <string.h>

#include "app/FxParams.h"
#include "app/Params.h"
#include "app/Theory.h"
#include "cost/LeanCostTable.h"

// Everything that makes a project: the mockup's S.patterns, S.parts and S.songs as plain data. Every field holds a valid value at all
// times (the sequencer reads it from the audio interrupt while the UI edits it), and nothing in here points anywhere, so a project is
// copied, saved and undone with memcpy.
namespace app {

constexpr int kLanes = 10;
constexpr int kMaxSteps = 32;
constexpr int kPatterns = 16;
constexpr int kParts = 7;      // A to G. C is the sampler a new project plays its drums on; the others are synths
constexpr int kSongs = 8;
constexpr int kSongRows = 64;
constexpr int kLocksPerStep = 8;
constexpr int kTrackerRows = 16;   // a page of the tracker
static_assert(kParts == kFxParts, "the effects have one amp and one set of sends per part");
static_assert(kParts <= 8, "Project::partsIdle has a bit per part");
static const char* const kPartName[kParts] = {"A", "B", "C", "D", "E", "F", "G"};
static const uint32_t kPartHex[kParts] = {0xff8a3d, 0x19d3e6, 0xff4fa3, 0xffd93d, 0x7ee05a, 0xa98bff, 0x4f8dff};
constexpr int kVoiceBudget = DBOY_COST_LEAN ? cost::lean_table::kVoiceBudget : 5;   // in plain voices of the engine the cost model describes

enum PartType : uint8_t { PT_CATHODE = 0, PT_SAMPLER = 1 };

// A parameter locked to one step (a p-lock).
struct Lock {
    uint8_t key;     // lockKey(page, param)
    uint8_t pad;
    int16_t value;
};

struct Step {
    uint8_t ov;      // tapped on top of the Euclid rhythm (toggles the step)
    int8_t note;     // offset from the lane's root (a sampler lane: pitch in semitones)
    uint8_t vel;     // 1..127
    uint8_t gate;    // 1..127, in 32nds of a step
    uint8_t prob;    // 0..100 %
    uint8_t chord, inv, spread, strum;   // chord type, inversion 0..3, open voicing, strum ms 0..120
    uint8_t nLocks;
    Lock locks[kLocksPerStep];

    int findLock(uint8_t key) const {
        for (int i = 0; i < nLocks; ++i) if (locks[i].key == key) return i;
        return -1;
    }
    bool hasLock(uint8_t key) const { return findLock(key) >= 0; }
    int16_t lockValue(uint8_t key, int16_t otherwise) const {
        const int i = findLock(key);
        return i < 0 ? otherwise : locks[i].value;
    }
    // false when the step already has kLocksPerStep other locks
    bool setLock(uint8_t key, int16_t value);
    void removeLock(uint8_t key);
    void clearLocks() { nLocks = 0; }
};

struct Arp {
    uint8_t mode;    // kArpModes
    uint8_t rate;    // index into kArpRates
    uint8_t oct;     // 1..4
    uint8_t gate;    // 5..100 %
};

struct Lane {
    uint8_t part;    // 0..kParts-1 (A..G)
    uint8_t root;    // MIDI note, 12..108
    uint8_t n, k, r; // Euclid length 1..32, hits 0..n, rotation 0..n-1
    uint8_t eu;      // the Euclid rhythm is on
    uint8_t level;   // 0..127
    uint8_t mute;
    uint8_t gate;    // the gate new steps get
    Arp arp;
    int16_t smp[kLanePages][kParamsPerPage];   // a sampler lane's own pages (SMP, FILTER, AMP, MOD, GRIT, SLICE, LOOP)
    Step st[kMaxSteps];

    int16_t& smpParam(int page, int i) { return smp[lanePageIndex(page)][i]; }
    int16_t smpParam(int page, int i) const { return smp[lanePageIndex(page)][i]; }
    int sample() const { return smp[0][0]; }
};

struct Pattern {
    Lane lanes[kLanes];
};

struct Part {
    uint8_t type;      // PartType
    uint8_t voices;    // CATHODE 1..5, sampler 1..8
    uint8_t level;     // 0..127
    uint8_t priority;  // 0 LOW .. 3 KEEP (what the degrade ladder gives up last)
    int16_t preset;    // the preset list's number (app/Params.h presetName), -1 none. (An int8 before project version 5.)
    uint8_t pad[2];
    int16_t pages[kSynthPages][kParamsPerPage];
};

struct SongRow {
    uint8_t pat;
    uint8_t rep;       // 1..16
};

struct Song {
    uint8_t nRows;
    uint8_t loop;      // at the end: 1 loop, 0 stop
    SongRow rows[kSongRows];
};

// A lane's MIDI: the channel it listens and sends on, and (on a channel it shares with other lanes, as drum lanes on 10 do) the
// note that is it. Project-wide: lane n of every pattern.
struct LaneMidi {
    uint8_t ch;      // 0 off, 1..16
    uint8_t note;    // 0..127: the trigger note on a shared channel; a sampler lane alone on its channel plays around it
};

// A sample cut into slices. It belongs to the sample, not to a lane: every lane that plays the sample plays the same slices. Each slice has settings of
// its own, on top of what a step's locks do to it.
constexpr int kSliceMaps = 16;
enum : uint8_t { SLICE_REVERSE = 1, SLICE_MUTED = 2 };
struct Slice {
    uint32_t start;   // frame
    int8_t pitch;     // semitones, -24..24
    uint8_t level;    // 0..127, 127 as recorded
    uint8_t flags;    // SLICE_REVERSE, SLICE_MUTED
    uint8_t pad;
};
struct SliceMap {
    int16_t sample;   // the sample's number, -1: this map is free
    uint8_t count;    // slices, 0..kSlices
    uint8_t pad;
    uint32_t frames;  // the sample's length when it was cut: a different file of that name has to be cut again
    Slice s[kSlices];
};

// Loop points AUTO found. Like slices they go with the sample; a sample looped from other START and END, or another way, has an entry of its own.
constexpr int kLoopFits = 32;
struct LoopFit {
    int16_t sample;          // -1: this entry is free
    int16_t wantS, wantE;    // the START and END it was found from
    uint8_t algo, search;
    uint8_t fadeKey;         // the fade it was found for (loopFadeKey)
    uint8_t corr;            // 0..100: how alike the two ends are, for CURVE MATCH
    uint16_t pad;
    uint32_t frames;         // the sample's length then
    uint32_t start, end;
};
inline uint8_t loopFadeKey(const int16_t* lp) { return lp[LP_AUTO] == AUTO_POWER ? (uint8_t)(lp[LP_XFADE] | (lp[LP_UNIT] << 7)) : 0; }   // (matching zero crossings does not depend on it)

struct Project {
    char name[16];
    uint16_t bpm;      // 40..240
    uint8_t swing;     // 0..50 %
    uint8_t key;       // 0..11
    uint8_t scale;     // kScales
    uint8_t master;    // 0..127
    uint8_t partsIdle; // a bit per part no lane of any pattern plays and that is not sounding: not in the voice budget or the degrade ladder (refreshIdleParts)
    uint8_t pad;
    Part parts[kParts];
    Pattern patterns[kPatterns];
    Song songs[kSongs];
    LaneMidi midi[kLanes];   // a version 1/2 file is this struct without it and the effects (ProjectFile)
    FxSettings fx;           // a version 3 file is this struct without it
    SliceMap slices[kSliceMaps];   // a version 6 file is this struct without it, and with shorter lanes
    LoopFit loops[kLoopFits];      // last: a version 7 file is this struct without it, and with shorter lanes
};

void defaultMidi(LaneMidi midi[kLanes]);         // lanes 1-7 on channels 1-7; drum lanes 8-10 on 10 as kick 36, snare 38, closed hat 42

// ---- builders (the mockup's mkLane, mkPattern, emptyPattern and the boot state)
void makeLane(Lane& l, int part, int root, int n, int k, int r, int gate);
void emptyLane(Lane& l);                         // no rhythm, no SYNC, fresh steps; its sound, level and mute stay
void makePattern(Pattern& p, int seed);          // seed 0..3: the mockup's P1-P4
void makeEmptyPattern(Pattern& p);               // P1's lanes, parts and roots, no rhythm
void makeDefaultProject(Project& pj);            // the mockup's boot state, and the factory's DEMO
void makeNewProject(Project& pj);                // what the Drumboy starts with: the same lanes and sounds, and nothing to play
constexpr int kFactoryProjects = 3;              // in the firmware, ahead of the card's slots on PROJECT
const char* factoryProjectName(int i);
void makeFactoryProject(int i, Project& pj);
void makeNewPart(Part& p, int part);             // a synth part as a new project has it

// ---- queries
inline bool isSampler(const Project& pj, int part) { return pj.parts[part].type == PT_SAMPLER; }
inline bool isIdlePart(const Project& pj, int part) { return (pj.partsIdle >> part) & 1; }
void refreshIdleParts(Project& pj, uint32_t busy = 0);   // partsIdle from the lanes (`busy`: a bit per part sounding now, played from MIDI, say). Main loop
inline bool baseHit(const Lane& l, int i, bool fill) {   // the Euclid rhythm (FILL held: hits doubled)
    if (!l.eu) return false;
    const int k = fill ? (l.k * 2 < l.n ? l.k * 2 : l.n) : l.k;
    return euclidHit(l.n, k, l.r, i % l.n);
}
inline bool trig(const Lane& l, int i, bool fill) { return baseHit(l, i, fill) != (l.st[i].ov != 0); }
// The pitches a step plays, worked out as the sequencer does (Sequencer::step): the chord on its note, each note in the project scale, inside what the voices play. An arpeggio walks these.
inline int stepPitches(const Project& pj, const Lane& l, const Step& st, int* out) {
    const bool smp = isSampler(pj, l.part);
    const int n = chordNotes((smp ? 0 : l.root) + st.note, clampi(st.chord, 0, kChordCount - 1), st.inv, st.spread, out);
    for (int i = 0; i < n; ++i) out[i] = smp ? clampi(out[i], -48, 48) : clampi(scaleNote(out[i], pj.key, pj.scale), 0, 127);
    return n;
}
const SliceMap* findSlices(const Project& pj, int sample);   // the sample's slices, or nullptr
SliceMap* sliceMapFor(Project& pj, int sample);              // ... a map for it: its own, a free one, or one of a sample nothing plays (nullptr: all in use)
const LoopFit* findLoopFit(const Project& pj, int sample, int wantS, int wantE, int algo, int search, int fadeKey);
LoopFit* loopFitFor(Project& pj);                            // an entry to fill: a free one, or one no lane asks for (nullptr: all in use)
int patLen(const Pattern& p);                    // a pattern lasts as long as its longest lane
bool laneHasContent(const Lane& l);              // a rhythm or a tapped step
int usedPatterns(const Project& pj);
// Every sample number the project refers to (a lane's sample, a step's sample lock), whatever part the lane plays on: used[i] for
// i < count.
void samplesUsed(const Project& pj, bool* used, int count);
// Rewrites every sample reference through map[old] (old < count; a map entry < 0 leaves the reference as it is).
void remapSamples(Project& pj, const int16_t* map, int count);

}  // namespace app

#endif
