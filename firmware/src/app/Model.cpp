#include "app/Model.h"

#include "app/SampleLib.h"

namespace app {

bool Step::setLock(uint8_t key, int16_t value) {
    const int i = findLock(key);
    if (i >= 0) { locks[i].value = value; return true; }
    if (nLocks >= kLocksPerStep) return false;
    locks[nLocks].key = key;
    locks[nLocks].pad = 0;
    locks[nLocks].value = value;
    ++nLocks;
    return true;
}

void Step::removeLock(uint8_t key) {
    const int i = findLock(key);
    if (i < 0) return;
    for (int j = i; j + 1 < nLocks; ++j) locks[j] = locks[j + 1];
    --nLocks;
}

static void freshStep(Step& s, int gate) {
    memset(&s, 0, sizeof(s));
    s.vel = 100;
    s.gate = (uint8_t)gate;
    s.prob = 100;
}

void makeLane(Lane& l, int part, int root, int n, int k, int r, int gate) {
    memset(&l, 0, sizeof(l));
    if (gate <= 0) gate = 20;
    l.part = (uint8_t)part;
    l.root = (uint8_t)root;
    l.n = (uint8_t)n;
    l.k = (uint8_t)k;
    l.r = (uint8_t)r;
    l.eu = 1;
    l.level = 100;
    l.gate = (uint8_t)gate;
    l.arp = {0, 1, 1, 60};
    defaultLanePages(l.smp);
    for (Step& s : l.st) freshStep(s, gate);
}

void emptyLane(Lane& l) {
    l.eu = 0;
    l.k = 0;
    if (l.smpParam(PG_SLC, SLC_MODE) == SLICE_SYNC) l.smpParam(PG_SLC, SLC_MODE) = SLICE_OFF;   // (it plays by itself)
    for (Step& s : l.st) freshStep(s, l.gate);
}

void makePattern(Pattern& p, int seed) {
    struct L { int part, root, n, k, r, gate; };
    static const L kSeeds[4][kLanes] = {
        {{0, 36, 16, 4, 0, 18}, {0, 43, 16, 3, 2, 14}, {0, 39, 16, 5, 1, 12}, {1, 72, 16, 7, 0, 10}, {1, 75, 12, 5, 0, 10},
         {1, 79, 16, 3, 5, 14}, {1, 82, 11, 4, 0, 10}, {2, 60, 16, 2, 0, 90}, {2, 67, 16, 1, 8, 110}, {2, 63, 8, 1, 0, 60}},
        {{0, 36, 16, 5, 0, 16}, {0, 34, 16, 2, 0, 20}, {0, 41, 14, 3, 1, 12}, {1, 70, 16, 9, 0, 8}, {1, 74, 16, 5, 3, 8},
         {1, 77, 12, 4, 0, 10}, {1, 81, 16, 2, 0, 12}, {2, 58, 16, 1, 0, 120}, {2, 65, 16, 2, 4, 80}, {2, 62, 16, 0, 0, 60}},
        {{0, 38, 16, 3, 0, 16}, {0, 45, 8, 3, 0, 12}, {0, 41, 16, 7, 0, 10}, {1, 69, 16, 6, 2, 10}, {1, 72, 16, 4, 0, 12},
         {1, 76, 9, 3, 0, 10}, {1, 79, 16, 5, 1, 8}, {2, 57, 16, 1, 0, 100}, {2, 64, 16, 1, 8, 100}, {2, 60, 16, 0, 0, 60}},
        {{0, 36, 16, 8, 0, 8}, {0, 36, 16, 3, 4, 8}, {0, 48, 16, 5, 0, 8}, {1, 84, 16, 11, 0, 4}, {1, 79, 16, 6, 2, 4},
         {1, 91, 16, 4, 0, 4}, {1, 72, 16, 8, 3, 6}, {2, 60, 16, 0, 0, 60}, {2, 60, 16, 0, 0, 60}, {2, 60, 16, 0, 0, 60}},
    };
    for (int i = 0; i < kLanes; ++i) {
        const L& s = kSeeds[seed][i];
        makeLane(p.lanes[i], s.part, s.root, s.n, s.k, s.r, s.gate);
    }
}

void makeEmptyPattern(Pattern& p) {
    makePattern(p, 0);
    for (Lane& l : p.lanes) emptyLane(l);
}

void makeDefaultProject(Project& pj) {
    memset(&pj, 0, sizeof(pj));
    strcpy(pj.name, "PROJECT");
    pj.bpm = 118;
    pj.swing = 12;
    pj.master = 100;
    for (int i = 0; i < kParts; ++i) makeNewPart(pj.parts[i], i);
    for (int i = 0; i < kPatterns; ++i) {
        if (i < 4) makePattern(pj.patterns[i], i);
        else makeEmptyPattern(pj.patterns[i]);
    }
    Song& s0 = pj.songs[0];
    const SongRow rows[4] = {{0, 2}, {1, 2}, {2, 2}, {3, 1}};
    for (int i = 0; i < 4; ++i) s0.rows[i] = rows[i];
    s0.nRows = 4;
    for (Song& s : pj.songs) s.loop = 1;

    // two voices on the synth parts, then C made a sampler (lanes 8-10 are drums)
    pj.parts[2].pages[PG_OSC1][0] = 1;
    pj.parts[2].type = PT_SAMPLER;
    pj.parts[2].voices = 4;
    pj.parts[2].preset = -1;
    // lanes 8-10: Kick, Snare and a closed Hat, each its own sample; the hat in choke group G1
    static const int kSmp[3] = {0, 2, 5}, kR[3] = {0, 4, 0};
    static const int kHits[3][4] = {{4, 5, 3, 8}, {2, 2, 2, 3}, {7, 9, 6, 11}};
    for (int pi = 0; pi < kPatterns; ++pi)
        for (int j = 0; j < 3; ++j) {
            Lane& l = pj.patterns[pi].lanes[7 + j];
            l.n = 16;
            l.k = (uint8_t)(pi < 4 ? kHits[j][pi] : 0);
            l.r = (uint8_t)kR[j];
            l.gate = 127;
            for (Step& s : l.st) s.gate = 127;
            l.smpParam(PG_SMP, 0) = (int16_t)kSmp[j];
            if (kSmp[j] == 5) l.smpParam(PG_SAMP, 7) = 1;
        }
    defaultMidi(pj.midi);
    defaultFx(pj.fx);
    for (SliceMap& m : pj.slices) m.sample = -1;
    for (LoopFit& f : pj.loops) f.sample = -1;
    refreshIdleParts(pj);
}

void makeNewPart(Part& p, int part) {
    static const uint8_t kPriority[3] = {2, 1, 3};   // A, B, C; the parts after them keep the middle one
    memset(&p, 0, sizeof(p));
    p.type = PT_CATHODE;
    p.voices = 2;
    p.level = 100;
    p.priority = part < 3 ? kPriority[part] : 1;
    p.preset = -1;
    defaultSynthPages(p.pages);
    // the boot sounds (not presets: no name)
    applyPreset(p.pages, kBootSounds[(part < 3 ? part : part - 3) % kBootSoundCount]);
    if (part == 1) p.pages[PG_FLT1][1] = 96;
}

void refreshIdleParts(Project& pj, uint32_t busy) {
    uint8_t idle = (uint8_t)((1u << kParts) - 1u);
    for (const Pattern& p : pj.patterns)
        for (const Lane& l : p.lanes) idle &= (uint8_t)~(1u << l.part);
    pj.partsIdle = (uint8_t)(idle & ~busy);
}

void defaultMidi(LaneMidi midi[kLanes]) {
    static const uint8_t kDrumNotes[3] = {36, 38, 42};   // the General MIDI drum map
    for (int i = 0; i < kLanes; ++i) {
        midi[i].ch = (uint8_t)(i < 7 ? i + 1 : 10);
        midi[i].note = i < 7 ? 60 : kDrumNotes[i - 7];
    }
}

int patLen(const Pattern& p) {
    int m = 1;
    for (const Lane& l : p.lanes) if (l.n > m) m = l.n;
    return m;
}

bool laneHasContent(const Lane& l) {
    if (l.eu && l.k > 0) return true;
    for (int i = 0; i < kMaxSteps; ++i) if (l.st[i].ov) return true;
    return l.smpParam(PG_SLC, SLC_MODE) == SLICE_SYNC;   // (it plays by itself)
}

int usedPatterns(const Project& pj) {
    int n = 0;
    for (const Pattern& p : pj.patterns) {
        bool used = false;
        for (const Lane& l : p.lanes) used = used || laneHasContent(l);
        n += used;
    }
    return n;
}

const SliceMap* findSlices(const Project& pj, int sample) {
    for (const SliceMap& m : pj.slices) if (m.sample == sample && m.count > 0) return &m;
    return nullptr;
}

SliceMap* sliceMapFor(Project& pj, int sample) {
    for (SliceMap& m : pj.slices) if (m.sample == sample) return &m;
    for (SliceMap& m : pj.slices) if (m.sample < 0) return &m;
    static bool played[kMaxSamples];
    samplesUsed(pj, played, kMaxSamples);
    for (SliceMap& m : pj.slices) if (m.sample >= kMaxSamples || !played[m.sample]) return &m;
    return nullptr;
}

const LoopFit* findLoopFit(const Project& pj, int sample, int wantS, int wantE, int algo, int search, int fadeKey) {
    for (const LoopFit& f : pj.loops)
        if (f.sample == sample && f.wantS == wantS && f.wantE == wantE && f.algo == algo && f.search == search && f.fadeKey == fadeKey) return &f;
    return nullptr;
}

LoopFit* loopFitFor(Project& pj) {
    for (LoopFit& f : pj.loops) if (f.sample < 0) return &f;
    for (LoopFit& f : pj.loops) {
        bool asked = false;
        for (const Pattern& p : pj.patterns)
            for (const Lane& l : p.lanes) {
                const int16_t* sm = l.smp[lanePageIndex(PG_SMP)];
                const int16_t* lp = l.smp[lanePageIndex(PG_LOOP)];
                asked = asked || (sm[0] == f.sample && sm[1] == f.wantS && sm[2] == f.wantE && lp[LP_AUTO] == f.algo && lp[LP_SEARCH] == f.search && loopFadeKey(lp) == f.fadeKey);
            }
        if (!asked) return &f;
    }
    return nullptr;
}

void samplesUsed(const Project& pj, bool* used, int count) {
    for (int i = 0; i < count; ++i) used[i] = false;
    for (const Pattern& p : pj.patterns)
        for (const Lane& l : p.lanes) {
            if (l.smp[0][0] >= 0 && l.smp[0][0] < count) used[l.smp[0][0]] = true;
            for (const Step& s : l.st) {
                const int v = s.lockValue(lockKey(PG_SMP, 0), -1);
                if (v >= 0 && v < count) used[v] = true;
            }
        }
}

void remapSamples(Project& pj, const int16_t* map, int count) {
    const uint8_t key = lockKey(PG_SMP, 0);
    for (SliceMap& m : pj.slices) {   // a map for a sample nothing plays is not in the file's table, and its number means nothing here
        if (m.sample < 0) continue;
        if (m.sample < count && map[m.sample] >= 0) m.sample = map[m.sample];
        else { m.sample = -1; m.count = 0; }
    }
    for (LoopFit& f : pj.loops) {
        if (f.sample < 0) continue;
        if (f.sample < count && map[f.sample] >= 0) f.sample = map[f.sample];
        else f.sample = -1;
    }
    for (Pattern& p : pj.patterns)
        for (Lane& l : p.lanes) {
            int16_t& s = l.smp[0][0];
            if (s >= 0 && s < count && map[s] >= 0) s = map[s];
            for (Step& st : l.st) {
                const int i = st.findLock(key);
                if (i >= 0 && st.locks[i].value >= 0 && st.locks[i].value < count && map[st.locks[i].value] >= 0)
                    st.locks[i].value = map[st.locks[i].value];
            }
        }
}

}  // namespace app
