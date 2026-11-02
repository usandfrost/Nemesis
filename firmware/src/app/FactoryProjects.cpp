// The projects that come with the firmware. They are made here rather than kept on the card, so they are there on any card and nothing can overwrite them: loading
// one gives a project like any other, to change and save to a slot. Both new ones use only the built-in samples and show what a p-lock can do: read the locks below
// next to the same lanes without them, in the sound as well as here.
#include <string.h>

#include <initializer_list>

#include "app/Model.h"
#include "app/Params.h"
#include "app/Theory.h"

namespace app {

void makeNewProject(Project& pj) {
    makeDefaultProject(pj);
    strcpy(pj.name, "NEW PROJECT");
    for (Pattern& p : pj.patterns)
        for (Lane& l : p.lanes) emptyLane(l);
    pj.songs[0].nRows = 0;
    refreshIdleParts(pj);
}

namespace {

enum Sample { KICK_808, KICK_PUNCH, SNARE, CLAP, RIM, HAT_CLOSED, HAT_OPEN, CRASH, CONGA, TOM, BASS_STAB, SWEEP };
const int kChordMin = 2;

// A rhythm as it reads: x a hit, X an accent, o a quiet one, . nothing. Its length is the lane's.
void rhythm(Lane& l, const char* hits, const int8_t* notes = nullptr) {
    l.n = (uint8_t)strlen(hits);
    int next = 0;
    for (int i = 0; i < l.n; ++i) {
        if (hits[i] == '.') continue;
        l.st[i].ov = 1;
        l.st[i].vel = hits[i] == 'X' ? 127 : hits[i] == 'o' ? 64 : 100;
        if (notes) l.st[i].note = notes[next++];
    }
}
void lock(Lane& l, int step, int page, int param, int value) { l.st[step].setLock(lockKey(page, param), (int16_t)value); }

// Lanes on the sampler: a lane's own sample, played out to its end
void sampled(Lane& l, int sample, int level = 110) {
    l.part = 2;
    l.gate = 127;
    for (Step& s : l.st) s.gate = 127;
    l.smpParam(PG_SMP, 0) = (int16_t)sample;
    l.smpParam(PG_SMP, 7) = (int16_t)level;
}

void synthPart(Part& p, int voices) {
    defaultSynthPages(p.pages);
    p.voices = (uint8_t)voices;
    p.preset = -1;
}

void song(Project& pj, const SongRow* rows, int n) {
    for (int i = 0; i < n; ++i) pj.songs[0].rows[i] = rows[i];
    pj.songs[0].nRows = (uint8_t)n;
}

// ---- TECHNO: F minor at 132. One acid line, one chord stab, the rest drums, and all the movement in locks.
void technoAcid(Lane& l, int lowest, int highest) {
    static const int8_t kNotes[] = {0, 0, 12, 0, 3, 0, -2, 0, 12, 0, 7, 3};
    l.root = 41;
    l.gate = 14;
    for (Step& s : l.st) s.gate = 14;
    rhythm(l, "X.xxX.xxX.xxX.xx", kNotes);
    int k = 0;
    for (int i = 0; i < l.n; ++i) {                  // the filter opens over the bar, and shuts on the last hits
        if (!l.st[i].ov) continue;
        const int up = i < 12 ? i : 2 * (15 - i);
        lock(l, i, PG_FLT1, 1, lowest + (highest - lowest) * up / 12);
        if (++k % 4 == 0) lock(l, i, PG_FLT1, 2, 118);   // and every fourth hit screams
    }
    lock(l, 10, PG_FXSEND, 0, 90);                   // one note thrown into the delay
}

void technoStab(Lane& l, bool open) {
    l.root = 53;
    l.gate = 10;
    for (Step& s : l.st) s.gate = 10;
    rhythm(l, "x..x..x.........");
    for (const int i : {0, 3, 6}) l.st[i].chord = kChordMin;
    lock(l, 0, PG_FLT1, 1, open ? 74 : 58);
    lock(l, 3, PG_FLT1, 1, open ? 88 : 66);
    lock(l, 6, PG_FLT1, 1, open ? 104 : 76);
    lock(l, 6, PG_FXSEND, 1, 90);                    // the last one rings out
    lock(l, 6, PG_AMP, 1, 62);
}

void technoHats(Lane& l, bool open) {
    rhythm(l, "oxooXxoooxooXxoo");
    for (int i = 0; i < l.n; ++i) lock(l, i, PG_SAMP, 1, 18 + (i % 4) * 14);   // tight to loose over each beat
    if (!open) return;
    for (const int i : {6, 14}) {
        lock(l, i, PG_SMP, 0, HAT_OPEN);             // the closed hat's lane plays the open one here
        lock(l, i, PG_SMP, 7, 90);
    }
}

void technoRim(Lane& l) {
    rhythm(l, "..x..x....x..x.x");
    static const struct { int step, pitch; } kTune[] = {{2, 0}, {5, 3}, {10, -2}, {13, 7}, {15, 12}};
    for (const auto& t : kTune) lock(l, t.step, PG_SMP, 4, t.pitch);
    lock(l, 13, PG_FXSEND, 0, 100);
}

void makeTechno(Project& pj) {
    makeNewProject(pj);
    strcpy(pj.name, "TECHNO");
    pj.bpm = 132;
    pj.swing = 0;

    Part& acid = pj.parts[0];
    synthPart(acid, 1);
    acid.pages[PG_OSC1][0] = 2;
    acid.pages[PG_FLT1][0] = 5;      // ACID
    acid.pages[PG_FLT1][1] = 40;
    acid.pages[PG_FLT1][2] = 88;
    acid.pages[PG_FLT1][3] = 30;
    acid.pages[PG_FLT1][5] = 74;
    acid.pages[PG_AMP][0] = 0;
    acid.pages[PG_AMP][1] = 34;
    acid.pages[PG_AMP][2] = 0;
    acid.pages[PG_AMP][3] = 14;
    Part& stab = pj.parts[1];
    synthPart(stab, 3);
    stab.pages[PG_OSC1][0] = 2;
    stab.pages[PG_OSC2][0] = 2;
    stab.pages[PG_OSC2][6] = 96;
    stab.pages[PG_OSC2][4] = 0;
    stab.pages[PG_OSC2][5] = 9;
    stab.pages[PG_FLT1][1] = 70;
    stab.pages[PG_FLT1][2] = 36;
    stab.pages[PG_FLT1][5] = 30;
    stab.pages[PG_AMP][0] = 0;
    stab.pages[PG_AMP][1] = 30;
    stab.pages[PG_AMP][2] = 0;
    stab.pages[PG_AMP][3] = 26;
    acid.level = 127;
    acid.pages[PG_AMP][5] = 127;
    stab.level = 50;
    pj.parts[2].voices = 7;
    pj.fx.delay[1] = 92;             // a dotted eighth
    pj.fx.delay[2] = 62;
    pj.fx.send[0][0] = 18;
    pj.fx.send[1][0] = 52;
    pj.fx.send[1][1] = 34;
    pj.fx.send[2][1] = 10;

    for (int i = 0; i < 4; ++i) {
        Pattern& p = pj.patterns[i];
        const bool lastBars = i == 3;
        Lane& kick = p.lanes[7];
        sampled(kick, KICK_PUNCH, 120);
        rhythm(kick, lastBars ? "x...x...x...x.oo" : "x...x...x...x...");
        lock(kick, 12, PG_SMP, 4, lastBars ? -5 : 0);
        if (lastBars) { lock(kick, 14, PG_SMP, 4, 3); lock(kick, 15, PG_SMP, 4, 7); }

        Lane& clap = p.lanes[8];
        sampled(clap, CLAP, 96);
        rhythm(clap, "....x.......x...");
        lock(clap, 12, PG_FXSEND, 1, 70);

        sampled(p.lanes[9], HAT_CLOSED, 88);
        p.lanes[9].smpParam(PG_SAMP, 7) = 1;
        technoHats(p.lanes[9], i > 0);

        sampled(p.lanes[1], RIM, 78);
        if (i > 0) technoRim(p.lanes[1]);
        sampled(p.lanes[2], SWEEP, 70);
        if (lastBars) rhythm(p.lanes[2], "x...............");

        if (i > 0) technoAcid(p.lanes[0], i == 1 ? 28 : 34, i == 1 ? 58 : 96);
        if (i > 1) technoStab(p.lanes[3], i == 3);
    }
    const SongRow rows[] = {{0, 2}, {1, 4}, {2, 4}, {3, 1}, {1, 2}, {2, 4}, {3, 1}};
    song(pj, rows, sizeof rows / sizeof rows[0]);
    refreshIdleParts(pj);
}

// ---- MASCHINE: A minor at 124, straight as a ruler. A sequenced bass, a clean lead, bleeps and pitched percussion; the bleeps are the lead's own part, turned into
// something else by nothing but locks.
void maschineBass(Lane& l, bool open) {
    static const int8_t kNotes[] = {0, 0, 12, 0, -5, -2, 0, 3};
    l.root = 33;
    l.gate = 16;
    for (Step& s : l.st) s.gate = 16;
    rhythm(l, "x.x.x.x.x.x.x.x.", kNotes);
    for (int i = 0; i < 8; ++i) lock(l, i * 2, PG_FLT1, 1, (open ? 56 : 42) + (i % 4) * (open ? 12 : 6));   // stepping up, like a hand on a sequencer's knob
}

void maschineLead(Lane& l) {
    static const int8_t kNotes[] = {0, 3, 5, 7, 12, 10, 7, 5, 3, 5};
    l.root = 57;
    l.gate = 70;
    for (Step& s : l.st) s.gate = 70;
    rhythm(l, "x...x.x.x...x...x...x.x.x...x.x.", nullptr);
    int k = 0;
    for (int i = 0; i < l.n; ++i) if (l.st[i].ov) { l.st[i].note = kNotes[k < 10 ? k : 9]; ++k; }
    lock(l, 8, PG_OSC1, 2, 96);                      // the same oscillator, hollow
    lock(l, 16, PG_OSC1, 3, 1);                      // an octave up, for one note
    lock(l, 16, PG_AMP, 1, 90);
    lock(l, 24, PG_OSC1, 2, 96);
    lock(l, 30, PG_FXSEND, 0, 110);                  // and the last one walks off into the delay
}

void maschineBleeps(Lane& l) {
    static const int8_t kNotes[] = {0, 7, 12, 3, 7, 0, 12, 10, 7, 3};
    l.root = 81;
    l.gate = 6;
    for (Step& s : l.st) s.gate = 6;
    rhythm(l, "x.xxx.x.x.xxx.x.", kNotes);
    bool left = true;
    int k = 0;
    for (int i = 0; i < l.n; ++i) {
        if (!l.st[i].ov) continue;
        lock(l, i, PG_AMP, 1, 12);                   // short, where the lead is long
        lock(l, i, PG_AMP, 2, 0);
        lock(l, i, PG_AMP, 3, 8);
        lock(l, i, PG_AMP, 6, left ? 28 : 100);      // left, right, left
        lock(l, i, PG_FLT1, 1, 62 + (k % 5) * 12);
        left = !left;
        ++k;
    }
}

void maschineConga(Lane& l) {
    rhythm(l, "x.x.xx.xx.x.x.x.");
    static const struct { int step, pitch; } kTune[] = {{0, -5}, {2, -5}, {4, 0}, {5, 0}, {7, 3}, {8, 3}, {10, 7}, {12, 10}, {14, 12}};
    for (const auto& t : kTune) lock(l, t.step, PG_SMP, 4, t.pitch);   // a drum that plays the tune
}

void makeMaschine(Project& pj) {
    makeNewProject(pj);
    strcpy(pj.name, "MASCHINE");
    pj.bpm = 124;
    pj.swing = 0;

    Part& bass = pj.parts[0];
    synthPart(bass, 1);
    bass.pages[PG_OSC1][0] = 3;
    bass.pages[PG_FLT1][0] = 4;      // LADDER
    bass.pages[PG_FLT1][1] = 48;
    bass.pages[PG_FLT1][2] = 40;
    bass.pages[PG_FLT1][5] = 56;
    bass.pages[PG_AMP][0] = 0;
    bass.pages[PG_AMP][1] = 36;
    bass.pages[PG_AMP][2] = 30;
    bass.pages[PG_AMP][3] = 16;
    Part& lead = pj.parts[1];
    synthPart(lead, 3);
    lead.pages[PG_OSC1][0] = 3;
    lead.pages[PG_OSC1][1] = 64;
    lead.pages[PG_FLT1][1] = 84;
    lead.pages[PG_FLT1][2] = 14;
    lead.pages[PG_FLT1][5] = 16;
    lead.pages[PG_AMP][0] = 2;
    lead.pages[PG_AMP][1] = 56;
    lead.pages[PG_AMP][2] = 64;
    lead.pages[PG_AMP][3] = 30;
    bass.level = 118;
    lead.level = 28;
    pj.parts[2].voices = 6;
    pj.fx.delay[1] = 94;             // a dotted eighth
    pj.fx.delay[2] = 54;
    pj.fx.send[1][0] = 44;
    pj.fx.send[1][1] = 18;

    for (int i = 0; i < 4; ++i) {
        Pattern& p = pj.patterns[i];
        const bool fill = i == 3;
        Lane& kick = p.lanes[7];
        sampled(kick, KICK_808, 120);
        rhythm(kick, fill ? "x.......x......." : "x.....x...x.....");
        Lane& snare = p.lanes[8];
        sampled(snare, SNARE, 92);
        rhythm(snare, "....x.......x...");
        lock(snare, 12, PG_SMP, 4, -3);
        Lane& hat = p.lanes[9];
        sampled(hat, HAT_CLOSED, 84);
        hat.smpParam(PG_SAMP, 7) = 1;
        rhythm(hat, "x.o.x.o.x.o.x.o.");
        for (int k = 0; k < 8; ++k) lock(hat, k * 2, PG_SAMP, 1, k % 2 ? 16 : 30);
        Lane& rim = p.lanes[1];
        sampled(rim, RIM, 74);
        rhythm(rim, "...x..x....x..x.");
        for (const int k : {3, 11}) lock(rim, k, PG_SMP, 4, k == 3 ? 0 : 5);
        lock(rim, 14, PG_FXSEND, 0, 80);
        sampled(p.lanes[2], CONGA, 96);
        if (fill) maschineConga(p.lanes[2]);

        maschineBass(p.lanes[0], i > 1);
        if (i > 0) maschineBleeps(p.lanes[4]);
        if (i == 2 || i == 3) maschineLead(p.lanes[3]);
    }
    const SongRow rows[] = {{0, 2}, {1, 2}, {2, 4}, {3, 1}, {1, 2}, {2, 4}, {3, 1}};
    song(pj, rows, sizeof rows / sizeof rows[0]);
    refreshIdleParts(pj);
}

const char* const kFactoryNames[kFactoryProjects] = {"DEMO", "TECHNO", "MASCHINE"};

}  // namespace

const char* factoryProjectName(int i) { return kFactoryNames[i]; }

void makeFactoryProject(int i, Project& pj) {
    switch (i) {
        case 1: makeTechno(pj); break;
        case 2: makeMaschine(pj); break;
        default: makeDefaultProject(pj); strcpy(pj.name, kFactoryNames[0]); break;
    }
}

}  // namespace app
