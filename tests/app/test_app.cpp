// The panel's behaviour on the host: a port of the mockup's test suite (scratchpad t.js) to the C++ app core. Screen checks live in
// test_screen.cpp. Run: ctest --test-dir tests/build
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <string>
#include <map>
#include <vector>

#include "CardLib.h"
#include "PresetHost.h"
#include "HostServices.h"
#include "app/CostBridge.h"
#include "app/MidiMap.h"
#include "app/Model.h"
#include "app/ProjectFile.h"
#include "app/Looper.h"
#include "app/SampleLib.h"
#include "app/MidiExport.h"
#include "app/Sequencer.h"
#include "app/Slicer.h"
#include "app/WaveLib.h"
#include "app/Theory.h"
#include "app/Ui.h"
#include "gfx/Canvas.h"
#include "gfx/Screen.h"
#include "gfx/TrackerLayout.h"

using namespace app;

static int checks = 0, fails = 0;
static void ok(bool c, const std::string& m) {
    ++checks;
    if (!c) { ++fails; printf("FAIL %s\n", m.c_str()); }
}

// ---- a recording note sink
struct Rec : NoteSink {
    struct On { int part, pattern, lane, pitch; float vel; int nLocks; Lock locks[kLocksPerStep]; uint64_t at; int slice = -1, sliceRev = 0; };
    struct Off { int part, handle, pitch; uint64_t at; };
    std::vector<On> ons;
    std::vector<Off> offs;
    const Sequencer* seq = nullptr;
    std::vector<int> pitchOf;   // handle -> pitch: every note gets its own handle
    int32_t noteOn(const NoteStart& n, int offset) override {
        On o{n.part, n.pattern, n.lane, n.pitch, n.vel01, n.nLocks, {}, seq->sampleClock() + offset};
        for (int i = 0; i < n.nLocks; ++i) o.locks[i] = n.locks[i];
        o.slice = n.slice;
        o.sliceRev = n.sliceRev;
        ons.push_back(o);
        pitchOf.push_back(n.pitch);
        return (int32_t)pitchOf.size() - 1;
    }
    void noteOff(int part, int32_t handle, int offset) override { offs.push_back({part, handle, pitchOf[handle], seq->sampleClock() + offset}); }
    struct Fx { bool set; int lane, part, n; Lock locks[kLocksPerStep]; uint32_t token; uint64_t at; };
    std::vector<Fx> fxs;
    int releaseAll = 0;
    uint32_t activeTok[kLanes] = {};   // what the instrument would be holding: the token of each lane's set that has not been let go
    int active() const { int n = 0; for (uint32_t k : activeTok) n += k != 0; return n; }
    void fxSet(int lane, int part, const Lock* l, int n, uint32_t token, int offset) override {
        activeTok[lane] = token;
        Fx f{true, lane, part, n, {}, token, seq->sampleClock() + offset};
        for (int i = 0; i < n; ++i) f.locks[i] = l[i];
        fxs.push_back(f);
    }
    void fxRelease(int lane, uint32_t token, int offset) override {
        if (activeTok[lane] == token) activeTok[lane] = 0;
        fxs.push_back({false, lane, 0, 0, {}, token, seq->sampleClock() + offset});
    }
    void fxReleaseAll() override { ++releaseAll; for (uint32_t& k : activeTok) k = 0; }
};

struct Rig {
    Project* pj = new Project;
    Transport tp;
    SampleLib* lib = new SampleLib;
    std::vector<int16_t> store = std::vector<int16_t>(kFactoryFrames);
    HostServices sv;
    Ui* ui;
    Rec rec;
    Sequencer* seq;
    Rig() {
        makeDefaultProject(*pj);
        buildFactorySamples(*lib, store.data(), store.size());
        ui = new Ui(*pj, tp, *lib, sv);
        seq = new Sequencer(*pj, tp, rec);
        rec.seq = seq;
    }
    ~Rig() { delete seq; delete ui; delete lib; delete pj; }
    void key(Key k) { ui->keyDown(k); ui->keyUp(k); }
    void shiftKey(Key k) { ui->keyDown(Key::Shift); key(k); ui->keyUp(Key::Shift); }
    void home() { shiftKey(Key::Lane); }
    void tap(int i) { ui->layerDown(i); ui->layerUp(i); }
    void hold(int i, uint32_t ms) { ui->layerDown(i); sv.now += ms; ui->tick(); ui->layerUp(i); }
    void turn(int e, int n) { for (int k = 0; k < (n > 0 ? n : -n); ++k) ui->turn(e, n > 0 ? 1 : -1); }
    void shiftTurn(int e, int n) { ui->keyDown(Key::Shift); turn(e, n); ui->keyUp(Key::Shift); }
    Lane& ln() { return ui->lane(); }
    Lane& lane(int i) { return pj->patterns[tp.pat].lanes[i]; }
    std::string val(int i) { Cell c[8]; ui->cells(c); return std::string(c[i].value.c_str()) + (c[i].dest.empty() ? "" : std::string(" ") + c[i].dest.c_str()); }
    std::string label(int i) { Cell c[8]; ui->cells(c); return c[i].label.c_str(); }
    Cell cell(int i) { Cell c[8]; ui->cells(c); return c[i]; }
    std::string toast() { const char* t = ui->toast(); return t ? t : ""; }
    std::string labels(int from) { std::string s; for (int i = from; i < from + 4; ++i) s += (i > from ? "," : "") + label(i); return s; }
    void run(int blocks) { for (int b = 0; b < blocks; ++b) seq->process(32, sv.now); }
};

static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }
static std::string euStr(int n, int k, int r) { std::string s; for (int i = 0; i < n; ++i) s += euclidHit(n, k, r, i) ? 'x' : '.'; return s; }

static void testTheory() {
    ok(euStr(8, 3, 0) == "x..x..x.", "E(3,8) = " + euStr(8, 3, 0));
    ok(euStr(16, 4, 0) == "x...x...x...x...", "E(4,16)");
    ok(euStr(8, 3, 1) == ".x..x..x", "rotation by 1: " + euStr(8, 3, 1));
    bool exact = true;
    for (int n = 1; n <= 32; ++n) for (int k = 0; k <= n; ++k) for (int r : {0, 1, n - 1}) {
        int h = 0; for (int i = 0; i < n; ++i) h += euclidHit(n, k, r, i);
        if (h != k) exact = false;
    }
    ok(exact, "Euclid hit counts exact for all n<=32, k<=n");
    auto cn = [](int root, int c, int inv, int sp) { int o[5]; int n = chordNotes(root, c, inv, sp, o); std::string s; for (int i = 0; i < n; ++i) s += (i ? "," : "") + std::to_string(o[i]); return s; };
    ok(cn(60, 1, 0, 0) == "60,64,67", "C major");
    ok(cn(60, 1, 1, 0) == "64,67,72", "first inversion");
    ok(cn(60, 1, 2, 0) == "67,72,76", "second inversion");
    ok(cn(60, 1, 0, 1) == "60,67,76", "open voicing: " + cn(60, 1, 0, 1));
    ok(cn(60, 8, 0, 0) == "60,63,67,70", "minor seventh");
    ok(cn(60, 0, 3, 1) == "60", "chord OFF stays a single note");
    auto run = [](int m, int o, int c) { int nt[3] = {60, 64, 67}, out[8]; arpSeq(nt, 3, m, o, c, [] { return 0.5; }, out); std::string s; for (int i = 0; i < c; ++i) s += (i ? "," : "") + std::to_string(out[i]); return s; };
    ok(run(1, 1, 6) == "60,64,67,60,64,67", "arp up: " + run(1, 1, 6));
    ok(run(2, 1, 4) == "67,64,60,67", "arp down: " + run(2, 1, 4));
    ok(run(3, 1, 6) == "60,64,67,64,60,64", "arp up/down bounces: " + run(3, 1, 6));
    ok(run(1, 2, 6) == "60,64,67,72,76,79", "two octaves: " + run(1, 2, 6));
    { int nt[1] = {60}, out[4]; arpSeq(nt, 1, 1, 1, 4, [] { return 0.3; }, out); ok(out[0] == 60 && out[3] == 60, "arp of a single note repeats it"); }
    ok(scaleNote(60, 0, 1) == 60 && scaleNote(61, 0, 1) == 60 && scaleNote(63, 0, 1) == 62 && scaleNote(66, 0, 1) == 65 && scaleNote(70, 0, 1) == 69, "C major snapping (ties go down)");
    ok(scaleNote(61, 9, 2) == 60 && scaleNote(68, 9, 2) == 67 && scaleNote(66, 9, 2) == 65, "A minor snapping");
    ok(scaleNote(61, 0, 0) == 61, "scale OFF leaves notes alone");
    bool allIn = true;
    for (int k = 0; k < 12; ++k) for (int sc = 1; sc < kScaleCount; ++sc) for (int m = 24; m < 96; ++m) {
        const int q = scaleNote(m, k, sc);
        if (!scaleHas(sc, mod(q - k, 12)) || (q - m > 2 || m - q > 2)) allIn = false;
    }
    ok(allIn, "every key x scale: in the scale and at most 2 semitones away");
    Text t;
    fmtHz(t, cutHz(100)); ok(t == "4.2k Hz", "cutoff 100 = " + std::string(t.c_str()));
    Text p1, p2, p3; formatParam(p1, PG_AMP, 6, 64, nullptr); formatParam(p2, PG_AMP, 6, 0, nullptr); formatParam(p3, PG_AMP, 6, 127, nullptr);
    ok(p1 == "C" && p2 == "L100" && p3 == "R100", "pan L/C/R");
}

static void testBasics() {
    Rig r;
    ok(r.pj->parts[2].type == PT_SAMPLER && r.pj->parts[0].type == PT_CATHODE, "part C is the sampler by default");
    ok(std::string(r.lib->items[r.lane(7).sample()].name) + "|" + r.lib->items[r.lane(8).sample()].name + "|" + r.lib->items[r.lane(9).sample()].name ==
           "Kick 808|Snare Crack|Hat Closed", "lanes 8-10 play Kick, Snare, Hat");
    ok(r.lib->count == 12, "12 factory samples");
    // PLAY encoders
    const int k0 = r.ln().k, n0 = r.ln().n;
    r.turn(7, 2); ok(r.ln().k == k0 + 2, "B4 raises k");
    r.turn(6, -3); ok(r.ln().n == n0 - 3 && r.ln().k <= r.ln().n, "B3 lowers n, k stays <= n");
    const int bpm0 = r.pj->bpm; r.turn(0, 5); ok(r.pj->bpm == bpm0 + 5, "T1 tempo");
    ok(r.val(6) == std::to_string(r.ln().n), "screen shows n");
    r.turn(6, 3);
    r.tap(4); ok(r.ui->sel() == 4 && r.ui->part() == 1, "layer key 5 selects lane 5 (part B)");
    r.key(Key::Mute); ok(r.ln().mute, "mute"); r.key(Key::Mute);
    r.key(Key::PartK); ok(r.ui->part() == 2 && r.ln().part == 2, "D cycles part and jumps to its lane");
    // STEP
    r.tap(0); r.key(Key::StepK); ok(r.ui->mode() == Mode::Step, "STEP mode");
    Lane& L = r.ln();
    const uint8_t before = L.st[2].ov;
    r.tap(2); ok(L.st[2].ov != before && r.ui->cursor() == 2, "tap toggles step 3 and moves the cursor");
    r.tap(2); ok(L.st[2].ov == before, "second tap restores it");
    r.tap(9); ok(r.ui->stepPage() == 1 || (L.n + 7) / 8 == 1, "key 10 pages forward");
    r.tap(8); ok(r.ui->stepPage() == 0, "key 9 pages back");
    r.tap(1); r.turn(5, 4); ok(L.st[1].vel == 104, "B2 velocity on the cursor step");
    r.turn(4, 3); ok(L.st[1].note == 3, "B1 note offset");
    // P-LOCK
    r.hold(3, 450);
    ok(r.ui->lockOpen() && r.ui->lockIdx() == 3 && r.ui->lockLane() == r.ui->sel(), "long press latches the lock on step 4");
    const int base0 = r.pj->parts[r.ui->part()].pages[PG_FLT1][1];
    r.key(Key::Flt1); ok(r.ui->mode() == Mode::Sound && r.ui->lockOpen(), "a sound page keeps the lock");
    r.turn(1, 6);
    ok(L.st[3].lockValue(lockKey(PG_FLT1, 1), -1) == base0 + 6, "cutoff lock stored on the step");
    ok(r.pj->parts[r.ui->part()].pages[PG_FLT1][1] == base0, "the part's own cutoff untouched");
    ok(r.cell(1).lock && std::string(r.cell(1).tag.c_str()) == "LOCK", "the locked cell is marked");
    { Text t; formatParam(t, PG_FLT1, 1, base0 + 6, nullptr); ok(r.val(1) == t.c_str(), "locked value displayed in Hz: " + r.val(1)); }
    r.tap(4); ok(r.ui->lockOpen() && r.ui->lockIdx() == 4, "a tap moves the lock to step 5");
    r.turn(2, -4); ok(L.st[4].hasLock(lockKey(PG_FLT1, 2)), "reso lock on step 5");
    r.key(Key::Glob); r.turn(0, 2); ok(L.st[4].nLocks == 1 && has(r.toast(), "NOT LOCKABLE"), "GLOBAL params are not lockable");
    r.key(Key::Amp); r.turn(7, 1); ok(L.st[4].hasLock(lockKey(PG_AMP, 7)), "AMP B4 (VEL>ATK) is lockable");
    r.key(Key::Clear); ok(L.st[4].nLocks == 0, "CLEAR removes the step's locks");
    r.tap(4); ok(!r.ui->lockOpen(), "tapping the locked step leaves lock mode");
    ok(r.ui->mode() == Mode::Sound, "still on the sound page");
    r.home(); ok(r.ui->mode() == Mode::Play && !r.ui->lockOpen(), "going to PLAY clears a lock");
    ok(L.st[3].lockValue(lockKey(PG_FLT1, 1), -1) == base0 + 6, "the lock on step 4 survived");
}

static void testSampler() {
    Rig r;
    r.tap(7);
    ok(r.ui->sel() == 7 && r.ui->part() == 2, "layer key 8 selects the sampler lane");
    ok(r.val(4) == "Kick 808" && r.label(4) == "KICK" && r.cell(4).tag == "1/2", "PLAY B1 is the sample on a sampler lane, with its folder and its place in it");
    r.turn(4, 1); ok(std::string(r.lib->items[r.lane(7).sample()].name) == "Kick Punch" && r.sv.auditions.size() == 1, "B1 changes (and auditions) the lane's sample");
    r.turn(4, -1);
    r.key(Key::Osc1); ok(r.ui->mode() == Mode::Sound && r.ui->page() == PG_SMP, "OSC 1 opens the SAMPLE page on a sampler part");
    const int st0 = r.lane(7).smpParam(PG_SMP, 1);
    r.turn(1, 10); ok(r.lane(7).smpParam(PG_SMP, 1) == st0 + 10, "START moves the lane's own start point");
    ok(r.pj->parts[2].pages[PG_OSC1][1] == 40, "the part-level CATHODE pages are not touched");
    r.key(Key::Osc1); ok(r.ui->mode() == Mode::Play, "pressing the page key again goes back");
    r.key(Key::Osc1); r.key(Key::Flt1); ok(r.ui->page() == PG_SFLT, "moving between sound pages");
    r.key(Key::Lane); ok(r.ui->mode() == Mode::Play, "L from a sound page");
    r.key(Key::Flt2); ok(r.ui->mode() == Mode::Play && has(r.toast(), "FLT 2: NOT ON A SAMPLER PART"), "FLT 2 says so on a sampler part and stays put");
    r.key(Key::Amp); r.turn(7, 1); ok(r.lane(7).smpParam(PG_SAMP, 7) == 1, "AMP page works for a sampler lane (choke group)"); r.home();
    r.key(Key::StepK); r.hold(2, 450); ok(r.ui->lockOpen(), "latched a lock from STEP on the sampler lane");
    r.key(Key::Osc1); r.turn(1, 5); ok(r.lane(7).st[2].hasLock(lockKey(PG_SMP, 1)), "sample START can be p-locked");
    r.key(Key::Osc1); ok(r.ui->mode() == Mode::Step && r.ui->lockOpen(), "back from the page returns to STEP with the lock open");
    r.home();
    // browser
    r.tap(7); r.key(Key::StepK); r.key(Key::File);
    ok(r.ui->mode() == Mode::Browse && r.ui->browseCount() == 2 && r.ui->browseItem(r.ui->browseIdx()) == 0, "the browser lists samples for a sampler part, in the folder of the lane's sample");
    r.turn(1, -1);
    ok(r.ui->browseCat() == 0 && r.ui->browseCount() == r.lib->count, "CATEGORY still has ALL");
    r.turn(4, 2); r.key(Key::Ok);
    ok(r.ui->mode() == Mode::Step && r.lane(7).sample() == 2, "OK assigns the sample and goes back to where you were");
    r.key(Key::File); r.turn(1, 1); ok(r.ui->browseCat() == r.lib->items[2].cat + 1, "category filter"); r.key(Key::File); ok(r.ui->mode() == Mode::Step, "the browser key again goes back");
    r.home();
    // instrument switch
    r.tap(7); r.key(Key::File); r.turn(3, -1);
    ok(r.pj->parts[2].type == PT_SAMPLER && has(r.toast(), "HOLD SHIFT"), "INSTRUMENT needs SHIFT");
    r.shiftTurn(3, -1);
    ok(r.pj->parts[2].type == PT_CATHODE && r.ui->browseCount() == presetCount(), "INSTRUMENT turns the part into a CATHODE part");
    r.key(Key::Osc1); ok(r.ui->page() == PG_OSC1, "and OSC 1 now opens the oscillator page");
    r.home(); r.key(Key::File); r.shiftTurn(3, 1); ok(r.pj->parts[2].type == PT_SAMPLER, "and back"); r.home();
    // copy/paste keep sample data
    r.tap(7); r.lane(7).smpParam(PG_SMP, 1) = 33; r.key(Key::Copy); r.tap(8); r.key(Key::Paste);
    ok(r.lane(8).smpParam(PG_SMP, 1) == 33 && r.lane(8).part == 2, "paste copies sample settings and keeps the lane's part");
    // drum lane: sample per step
    r.home(); r.tap(7); r.key(Key::StepK); while (r.ui->cursor() != 2) r.key(Key::Right);
    r.key(Key::StepK);
    ok(r.ui->stepSub() == 1 && has(r.toast(), "SAMPLE PER STEP"), "on a drum lane STEP view 2 is SAMPLE PER STEP");
    ok(r.label(4) == "STEP SAMPLE" && r.val(4) == "LANE", "B1 STEP SAMPLE, the step plays the lane's sample");
    r.turn(4, 1); ok(r.lane(7).st[2].lockValue(lockKey(PG_SMP, 0), -1) == 3 && r.lane(7).sample() == 2, "turning B1 gives step 3 its own sample: the one after the lane's, in its folder");
    ok(r.val(4) == r.lib->items[3].name, "B1 shows it");
    r.turn(4, -5); ok(!r.lane(7).st[2].hasLock(lockKey(PG_SMP, 0)) && r.val(4) == "LANE", "all the way down: back to the lane's sample");
}

static void testChordsArpSequencer() {
    Rig r;
    // mute all but lane 1 and 8; lane 1 plays a minor chord on step 1, lane 8 a major chord
    for (Lane& l : r.pj->patterns[0].lanes) l.mute = 1;
    Lane& a = r.lane(0); Lane& d = r.lane(7);
    for (Lane* l : {&a, &d}) { l->mute = 0; l->eu = 0; l->n = 8; for (Step& s : l->st) s.ov = 0; l->st[0].ov = 1; }
    a.st[0].chord = 2; d.st[0].chord = 1;
    r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0;
    r.run(4);
    int synth = 0, smp = 0;
    for (auto& o : r.rec.ons) { synth += o.part == 0; smp += o.part == 2; }
    ok(synth == 3, "a minor chord on a CATHODE lane plays 3 notes: " + std::to_string(synth));
    ok(smp == 3, "a major chord on a sampler lane plays 3 pitched hits");
    bool rel = true; for (auto& o : r.rec.ons) if (o.part == 2 && !(o.pitch == 0 || o.pitch == 4 || o.pitch == 7)) rel = false;
    ok(rel, "on a sampler lane the notes are pitch offsets in semitones");
    // strum
    r.rec.ons.clear(); r.tp.playing = 0; r.run(1); r.rec.ons.clear(); r.rec.offs.clear();
    a.st[0].chord = 1; a.st[0].strum = 40; d.mute = 1;
    r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0; r.run(200);
    std::vector<uint64_t> at; for (auto& o : r.rec.ons) if (o.lane == 0 && at.size() < 3) at.push_back(o.at);
    ok(at.size() == 3 && at[1] - at[0] == 1764 && at[2] - at[1] == 1764, "strum 40 ms spreads the chord: " + (at.size() == 3 ? std::to_string(at[1] - at[0]) : std::string("?")));
    // arp x4
    r.tp.playing = 0; r.run(1); r.rec.ons.clear(); r.rec.offs.clear();
    a.st[0].strum = 0; a.arp = {1, 3, 1, 50};
    r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0; r.run(150);
    std::vector<Rec::On> ar; for (auto& o : r.rec.ons) if (o.lane == 0 && ar.size() < 4) ar.push_back(o);
    const double sub = 44100.0 * 60 / r.pj->bpm / 4 / 4;
    ok(ar.size() == 4 && ar[1].at - ar[0].at == (uint64_t)(sub + 0.5) && ar[0].pitch == a.root && ar[1].pitch == a.root + 4 && ar[2].pitch == a.root + 7 && ar[3].pitch == a.root,
       "arp x4: four notes across the step, up through the chord");
    // note offs: every on gets its off, the arp gate is 50% of the sub-step
    r.run(2000);
    ok(r.rec.offs.size() >= 4, "notes are released");
    bool gateOk = false;
    for (auto& f : r.rec.offs) if (f.part == 0 && f.pitch == a.root) { gateOk = f.at - ar[0].at == (uint64_t)(sub * 0.5); break; }
    ok(gateOk, "the arp gate is a percentage of the sub-step");
    // locks reach the note
    r.tp.playing = 0; r.run(1); r.rec.ons.clear();
    a.arp.mode = 0; a.st[0].chord = 0; a.st[0].setLock(lockKey(PG_FLT1, 1), 99);
    r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0; r.run(3);
    ok(!r.rec.ons.empty() && r.rec.ons[0].nLocks == 1 && r.rec.ons[0].locks[0].value == 99, "a locked step passes its locks to the note");
    // the scale snaps synth notes, never drums
    r.tp.playing = 0; r.run(1); r.rec.ons.clear();
    a.root = 48; a.st[0].note = 1; r.pj->key = 0; r.pj->scale = 1; d.mute = 0; d.st[0].note = 1; d.st[0].chord = 0;
    r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0; r.run(3);
    int sp = -99, dp = -99; for (auto& o : r.rec.ons) { if (o.lane == 0) sp = o.pitch; if (o.lane == 7) dp = o.pitch; }
    ok(sp == 48 && dp == 1, "C major: C#3 on a synth plays C3; the drum lane keeps +1");
    r.pj->scale = 0;
    // STOP drops unstarted notes but releases the sounding ones
    r.tp.playing = 0; r.run(2000); r.rec.ons.clear(); r.rec.offs.clear();   // drain the earlier notes' ends first
    a.st[0].note = 0; a.st[0].gate = 127; d.mute = 1;
    r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0; r.run(2);
    r.tp.playing = 0; r.run(800);   // a gate of 127/32 steps is ~22k frames at 118 BPM
    ok(r.rec.ons.size() == 1 && r.rec.offs.size() == 1, "STOP: the sounding note still gets its release");
    ok(r.seq->pending() == 0, "nothing left in the queue");
}

static void testPatternsSongs() {
    Rig r;
    ok(usedPatterns(*r.pj) == 4, "P1-P4 have content, P5-P16 start empty");
    r.turn(3, 20); ok(r.tp.pat == 15 && r.val(3) == "P16", "the PATTERN knob reaches P16 (stopped: at once)"); r.turn(3, -20);
    r.ui->keyDown(Key::Shift); r.tap(2); r.ui->keyUp(Key::Shift);
    ok(r.tp.pat == 2, "SHIFT + key 3 picks P3"); r.tap(4); ok(r.ui->sel() == 4 && r.tp.pat == 2, "key 5 alone still picks lane 5");
    r.tp.pat = 0;
    // queued switch
    r.key(Key::Play); r.run(3 * 4);
    r.turn(3, 2); ok(r.tp.pat == 0 && r.tp.nextPat == 2 && r.val(3) == "P1 \xE2\x96\xB8P3", "playing: P3 is queued: " + r.val(3));
    const double stepS = 44100.0 * 60 / r.pj->bpm / 4;
    r.run((int)(stepS * 17 / 32) + 2);
    ok(r.tp.pat == 2 && r.tp.nextPat == -1, "at the end of the pattern P3 takes over");
    r.ui->tick(); ok(r.tp.patChanges == 1, "the switch is counted");
    r.key(Key::Stop); r.run(2);
    // the song screen
    r.tp.pat = 0;
    r.key(Key::SongK); ok(r.ui->mode() == Mode::Song && r.labels(0) == "SONG,ROW,PATTERN,REPEATS", "SONG: SONG, ROW, PATTERN, REPEATS");
    ok(r.ui->songBars(r.ui->song()) == 7, "song length 2+2+2+1 = 7 bars");
    r.key(Key::Down); r.key(Key::Down); ok(r.ui->songRow() == 2, "down moves the row cursor");
    r.turn(2, 3); r.turn(3, 2); ok(r.ui->song().rows[2].pat == 5 && r.ui->song().rows[2].rep == 4, "T3/T4 set the row's pattern and repeats");
    r.key(Key::Add); ok(r.ui->song().nRows == 5 && r.ui->songRow() == 3 && r.ui->song().rows[3].pat == 5, "ADD adds a copy of the row after it");
    r.key(Key::Clear); ok(r.ui->song().nRows == 4, "CLEAR removes the selected row");
    r.shiftKey(Key::Clear); ok(r.ui->song().nRows == 5, "SHIFT + CLEAR brings it back");
    r.key(Key::Clear);
    // restore song 1
    r.ui->song().rows[2] = {2, 2};
    ok(r.val(4) == "LOOP" && r.cell(5).ro && r.cell(6).ro && std::string(r.cell(5).tag.c_str()) == "INFO", "LENGTH and TIME are read-only INFO cells");
    const std::string len = r.val(5); r.turn(5, 3); ok(r.val(5) == len && has(r.toast(), "INFORMATION ONLY"), "turning them changes nothing and says so");
    r.key(Key::Up); r.key(Key::Up); r.key(Key::Up); r.key(Key::Up);
    r.key(Key::Play);
    ok(r.tp.playing && r.tp.songPlaying && r.tp.pat == 0 && r.tp.repLeft == 2, "PLAY on the SONG screen plays the song from row 1");
    const int p1 = patLen(r.pj->patterns[0]);
    r.run((int)(stepS * p1 / 32) + 2);
    ok(r.tp.pat == 0 && r.tp.repLeft == 1, "row 1 repeats (x2)");
    r.run((int)(stepS * p1 / 32) + 2);
    ok(r.tp.pat == 1 && r.tp.playRow == 1, "then row 2 (P2)");
    r.home(); r.turn(3, 1); ok(r.tp.pat == 1 && has(r.toast(), "THE SONG CHOOSES"), "the PATTERN knob is locked while a song plays");
    r.key(Key::SongK); while (r.ui->songRow() < 3) r.key(Key::Down); r.key(Key::Ok); ok(r.tp.jumpTo == 3, "OK on a row: it plays next");
    r.run((int)(stepS * 16 / 32) + 40);
    ok(r.tp.playRow == 3 && r.tp.pat == r.ui->song().rows[3].pat, "jumped to row 4");
    r.ui->song().loop = 0;
    r.run((int)(stepS * 17 / 32) + 40);
    r.ui->tick();
    ok(!r.tp.playing && !r.tp.songPlaying && has(r.toast(), "END OF SONG"), "END STOP: the last row ends the song");
    r.ui->song().loop = 1;
    r.run(4);
    // copy / paste a song
    r.key(Key::Copy); r.turn(0, 1); ok(r.tp.songIdx == 1 && r.ui->song().nRows == 0, "song 2 is empty");
    r.key(Key::Paste); ok(r.ui->song().nRows == 4, "C / P copy a whole song");
    r.shiftKey(Key::Clear); ok(r.ui->song().nRows == 0, "the paste is undoable");
    r.turn(0, -1); r.home();
    r.tp.pat = 0; r.shiftKey(Key::Copy); r.tp.pat = 9;
    bool empty = true; for (Lane& l : r.pj->patterns[9].lanes) empty = empty && !l.eu;
    ok(empty, "P10 is empty");
    r.shiftKey(Key::Paste);
    ok(memcmp(&r.pj->patterns[9], &r.pj->patterns[0], sizeof(Pattern)) == 0, "SHIFT + C / SHIFT + P copy P1 onto P10");
    r.tp.pat = 0;
}

static void testEmptyPattern() {
    auto holdClear = [](Rig& r, uint32_t ms) { r.ui->keyDown(Key::Clear); r.sv.now += ms; r.ui->tick(); r.ui->keyUp(Key::Clear); };
    auto same = [](Rig& r, const Pattern& p) { return memcmp(&r.pj->patterns[r.tp.pat], &p, sizeof(Pattern)) == 0; };
    Rig r;
    Lane& L0 = r.lane(0);
    L0.st[1].ov = 1;
    L0.st[2].setLock(1, 5);
    L0.st[3].note = 7;
    L0.mute = 1;
    r.lane(9).level = 33;
    auto before = std::make_unique<Pattern>(r.pj->patterns[r.tp.pat]);
    const Pattern other = r.pj->patterns[1];

    holdClear(r, 999);
    r.sv.now += 2000; r.ui->tick();
    ok(r.lane(3).eu && r.lane(3).k, "CLEAR let go before the second is an ordinary press, however long the wait after it");
    r.shiftKey(Key::Clear); ok(same(r, *before), "and one undo takes it back");
    r.sv.now += 1000;

    holdClear(r, 1000);
    bool empty = true;
    for (int i = 0; i < kLanes; ++i) {
        const Lane& l = r.lane(i);
        empty = empty && !laneHasContent(l) && !l.k;
        for (const Step& s : l.st) empty = empty && !s.ov && !s.nLocks && !s.note && s.vel == 100 && s.prob == 100 && s.gate == l.gate;
    }
    ok(empty, "CLEAR held for a second empties every lane: no rhythm, no tapped steps, no locks, fresh steps");
    ok(has(r.toast(), "P1 EMPTY") && has(r.toast(), "SHIFT + \xE2\x97\x8B UNDOES"), "and says so, and how to undo: " + r.toast());
    ok(L0.part == before->lanes[0].part && L0.root == before->lanes[0].root && L0.mute == 1 && r.lane(9).level == 33 && L0.n == before->lanes[0].n,
       "the lanes keep their sound, level, mute and length");
    ok(memcmp(&r.pj->patterns[1], &other, sizeof(Pattern)) == 0, "the other patterns stay as they were");
    r.shiftKey(Key::Clear); ok(same(r, *before), "one undo brings the whole pattern back, the lane under the press too");
    r.shiftKey(Key::Clear); ok(has(r.toast(), "NOTHING TO UNDO"), "and that was one undo");

    holdClear(r, 1000);
    r.sv.now += 1000;
    r.key(Key::Clear);
    ok(has(r.toast(), "IS EMPTY"), "CLEAR on a lane with nothing on it says so: " + r.toast());
    r.shiftKey(Key::Clear); ok(same(r, *before), "and leaves the undo of the whole pattern alone");

    holdClear(r, 1000);
    r.sv.now += 1000;
    holdClear(r, 1000);
    ok(has(r.toast(), "P1 IS EMPTY"), "a second hold finds nothing to do: " + r.toast());
    r.shiftKey(Key::Clear); ok(same(r, *before), "and the first hold's undo still brings the pattern back");

    r.sv.now += 1000;
    r.key(Key::Clear); r.sv.now += 100; holdClear(r, 1000);
    ok(!r.lane(5).eu, "the second press of a double press, held, empties the pattern too");
    r.shiftKey(Key::Clear); ok(same(r, *before), "one undo goes back to before the first press");

    r.sv.now += 1000;
    r.ui->keyDown(Key::Clear); r.key(Key::Up); r.sv.now += 2000; r.ui->tick(); r.ui->keyUp(Key::Clear);
    ok(r.lane(5).eu, "another key while CLEAR is held calls it off");
    r.shiftKey(Key::Clear);
    r.sv.now += 1000;
    r.ui->keyDown(Key::Clear); r.ui->turn(0, 1); r.sv.now += 2000; r.ui->tick(); r.ui->keyUp(Key::Clear);
    ok(r.lane(5).eu, "so does a knob");
    r.ui->keyDown(Key::Clear); r.tap(3); r.sv.now += 2000; r.ui->tick(); r.ui->keyUp(Key::Clear);
    ok(r.lane(5).eu, "and a step key");

    for (Key k : {Key::Mixer, Key::File, Key::Osc1}) {
        Rig s;
        s.key(k);
        holdClear(s, 1000);
        ok(s.lane(5).eu, std::string("the hold does nothing on a screen without the lanes: ") + s.toast());
    }
    Rig st;
    st.key(Key::StepK);
    holdClear(st, 1000);
    ok(!st.lane(5).eu, "it works from STEP");

    Rig kh;
    kh.key(Key::StepK);
    kh.ui->layerDown(2); kh.sv.now += 400; kh.ui->tick();
    holdClear(kh, 1000);
    kh.ui->layerUp(2);
    ok(kh.lane(5).eu, "a step key held first keeps CLEAR an ordinary press");
    Rig lh;
    lh.ui->keyDown(Key::Lane); holdClear(lh, 1000); lh.ui->keyUp(Key::Lane);
    ok(lh.lane(5).eu, "and so does L");

    Rig sy;
    sy.lane(7).smpParam(PG_SLC, SLC_MODE) = SLICE_SYNC;
    ok(laneHasContent(sy.lane(7)), "a lane in SYNC plays by itself");
    holdClear(sy, 1000);
    ok(!laneHasContent(sy.lane(7)) && sy.lane(7).smpParam(PG_SLC, SLC_MODE) == SLICE_OFF, "emptying stops it");
    sy.shiftKey(Key::Clear);
    ok(sy.lane(7).smpParam(PG_SLC, SLC_MODE) == SLICE_SYNC, "and the undo starts it again");

    Rig u;
    auto was = std::make_unique<Pattern>(u.pj->patterns[0]);
    u.ui->keyDown(Key::Shift); u.ui->keyDown(Key::Clear);
    ok(has(u.toast(), "NOTHING TO UNDO"), "SHIFT + CLEAR is still undo");
    u.sv.now += 2000; u.ui->tick(); u.ui->keyUp(Key::Clear); u.ui->keyUp(Key::Shift);
    ok(same(u, *was), "and held, nothing else");

    Rig m;
    m.ui->keyDown(Key::Clear); m.tp.pat = 1; m.sv.now += 1000; m.ui->tick(); m.ui->keyUp(Key::Clear);
    ok(m.pj->patterns[1].lanes[0].eu && has(m.toast(), "NOTHING EMPTIED"), "a song that moves on while CLEAR is held is not emptied");

    Rig h;
    holdClear(h, 1000);
    h.home();
    Lane& e = h.lane(0);
    ok(!e.eu && !e.k, "an emptied lane");
    for (int i = 0; i < 4; ++i) h.turn(7, 1);
    ok(e.eu && e.k == 4, "HITS brings the rhythm back: " + std::to_string(e.k));
    bool hit = false;
    for (int i = 0; i < e.n; ++i) hit = hit || trig(e, i, false);
    ok(hit, "and the lane plays");
    h.turn(7, -4);
    ok(e.k == 0 && !laneHasContent(e), "HITS at 0 is empty again");
}

static void testUndoFillNav() {
    Rig r;
    Lane& L0 = r.lane(0);
    L0.st[1].ov = 1; const uint8_t eu0 = L0.eu;
    r.key(Key::Clear); r.sv.now += 100; r.key(Key::Clear);
    ok(!L0.eu && !L0.st[1].ov && has(r.toast(), "SHIFT + \xE2\x97\x8B UNDOES"), "CLEAR twice empties the lane and says how to undo");
    r.shiftKey(Key::Clear); ok(L0.eu == eu0 && L0.st[1].ov && has(r.toast(), "UNDONE"), "SHIFT + CLEAR restores the emptied lane");
    r.shiftKey(Key::Clear); ok(has(r.toast(), "NOTHING TO UNDO"), "one level only");
    L0.st[1].ov = 0; r.sv.now += 1000;
    // paste undo
    r.tap(1); r.key(Key::Copy); r.tap(0); const int pn0 = L0.n, pk0 = L0.k; r.lane(1).n = 12;
    r.tap(1); r.key(Key::Copy); r.tap(0);
    r.key(Key::Paste); ok(L0.n == 12, "paste overwrote lane 1");
    r.shiftKey(Key::Clear); ok(L0.n == pn0 && L0.k == pk0, "SHIFT + CLEAR undoes the paste");
    // preset undo
    r.key(Key::File); const int pre = r.pj->parts[0].preset, cut0 = r.pj->parts[0].pages[PG_FLT1][1];
    r.key(Key::Down); r.key(Key::Down); r.key(Key::Ok);
    ok(r.pj->parts[0].preset != pre && r.ui->mode() == Mode::Play, "OK loaded another preset and went back");
    r.shiftKey(Key::Clear); ok(r.pj->parts[0].preset == pre && r.pj->parts[0].pages[PG_FLT1][1] == cut0, "SHIFT + CLEAR puts the old preset back");
    // FILL
    L0.eu = 1; L0.n = 16; L0.k = 4;
    auto hits = [&](bool f) { int h = 0; for (int i = 0; i < L0.n; ++i) h += baseHit(L0, i, f); return h; };
    r.ui->keyDown(Key::Fill); ok(r.tp.fill && hits(r.tp.fill) == 8, "FILL held: twice the hits"); r.ui->keyUp(Key::Fill);
    ok(!r.tp.fill && hits(r.tp.fill) == 4, "FILL released: back to the pattern");
    // L = back one level; again home; SHIFT + L home
    r.key(Key::StepK); r.key(Key::Osc1); ok(r.ui->mode() == Mode::Sound, "STEP then OSC 1");
    r.key(Key::Lane); ok(r.ui->mode() == Mode::Step, "L goes back one level (to STEP)");
    r.key(Key::Lane); ok(r.ui->mode() == Mode::Play, "L again goes home");
    r.key(Key::StepK); r.key(Key::Osc1); r.home(); ok(r.ui->mode() == Mode::Play, "SHIFT + L goes straight home");
    r.key(Key::Lane); ok(r.ui->mode() == Mode::Play, "L on PLAY stays home (the mockup has no toast there)");
    // preview on scroll: after a short delay, with the part unchanged
    r.key(Key::File);
    int16_t before[kSynthPages][kParamsPerPage]; memcpy(before, r.pj->parts[0].pages, sizeof(before));
    r.key(Key::Down); r.sv.now += 50; r.ui->tick(); ok(r.sv.previews.empty(), "no preview while scrolling fast");
    r.sv.now += 200; r.ui->tick(); ok(r.sv.previews.size() == 1 && r.sv.previews[0].preset == r.ui->browseItem(1), "the preset plays after the knob settles");
    ok(memcmp(before, r.pj->parts[0].pages, sizeof(before)) == 0, "the part is not changed by a preview");
    for (int k = 0, n = r.ui->browseCount() + 5; k < n; ++k) r.key(Key::Down);   // (the whole list, however many presets the library holds)
    ok(r.ui->browseIdx() == r.ui->browseCount() - 1, "down stops at the end of the list");
    r.key(Key::Right); ok(r.ui->browseCat() == 1 && r.ui->browseIdx() == 0 && has(r.toast(), "CATEGORY"), "right: the next category, list at its top");
    r.key(Key::Left); ok(r.ui->browseCat() == 0, "left: the previous one");
    r.home();
    // the NOTE knob shows the destination
    r.key(Key::StepK); L0.root = 48; L0.st[0].note = 0; r.tap(0); r.tap(0);
    r.turn(4, 7); ok(L0.st[0].note == 7 && r.val(4) == "+7 G3", "NOTE +7 on C3 shows +7 G3: " + r.val(4));
    r.pj->scale = 1; r.turn(4, -1); ok(r.val(4) == "+6 F3", "with C major, +6 lands on F3: " + r.val(4));
    r.pj->scale = 0;
    // hold + turn
    L0.st[2].vel = 100; const uint8_t ov0 = L0.st[2].ov;
    r.ui->layerDown(2); ok(r.ui->cursor() == 2, "pressing a step key selects it at once");
    r.turn(5, -10); r.ui->layerUp(2);
    ok(L0.st[2].vel == 90 && L0.st[2].ov == ov0 && !r.ui->lockOpen(), "hold + turn edits the step, no toggle, no lock");
    r.tap(2); ok(L0.st[2].ov != ov0, "a tap still toggles");
    r.home();
    // PLAYED BY
    const int lp0 = L0.part; r.turn(2, 1);
    ok(L0.part == lp0 && has(r.toast(), "HOLD SHIFT"), "T3 without SHIFT does not move the lane");
    ok(std::string(r.cell(2).tag.c_str()) == "SHIFT" && r.label(2) == "PLAYED BY", "T3 is PLAYED BY, tagged SHIFT");
    r.shiftTurn(2, 1); ok(L0.part == lp0 + 1 && r.ui->part() == L0.part && has(r.toast(), "LANE 1 NOW PLAYS ON PART") && r.ui->laneFlashing(0), "SHIFT + T3 moves the lane, says so, flashes the row");
    L0.part = (uint8_t)lp0;
    // labels
    r.home(); ok(r.labels(4) == "ROOT,LEVEL,LENGTH,HITS", "PLAY bottom row: " + r.labels(4));
    r.key(Key::StepK); ok(r.labels(0) == "ROOT,LENGTH,HITS,ROTATE" && r.label(4) == "NOTE", "STEP: same names for the same parameters");
    r.key(Key::StepK); ok(r.label(0) == "ARP" && r.label(4) == "CHORD", "view 2 on a synth lane: ARP above, CHORD below");
    r.tap(0); r.tap(0);
    r.turn(4, 3); ok(L0.st[r.ui->cursor()].chord == 3, "B1 sets the chord");
    r.turn(5, 1); ok(L0.st[r.ui->cursor()].inv == 1, "B2 the inversion");
    r.turn(0, 2); ok(L0.arp.mode == 2, "T1 the arp mode");
    r.turn(1, 1); ok(L0.arp.rate == 2, "T2 the arp rate");
    r.turn(7, 5); ok(L0.st[r.ui->cursor()].strum == 0, "strum is disabled while an arp is on");
    r.turn(0, -2); r.turn(7, 3); ok(L0.st[r.ui->cursor()].strum == 12, "strum works with the arp off");
    r.home();
}

static void testCost() {
    Rig r;
#if DBOY_COST_LEAN
    ok(voiceTotal(*r.pj) <= kVoiceBudget && voiceTotal(*r.pj) > 3, "default total fits the budget: " + std::to_string(voiceTotal(*r.pj)));
    r.key(Key::Mixer); ok(!r.cell(7).warn, "the default project is well inside it: " + r.val(7));
    Part& A = r.pj->parts[0];
    // the table's own options reach the description (a sine with PD AMT, the sub, noise, the HPF, GRIT CLIP, a converter)
    {
        const int16_t saved[4] = {A.pages[PG_SUB][0], A.pages[PG_SUB][2], A.pages[PG_FLT1][6], A.pages[PG_GRIT][6]};
        A.pages[PG_SUB][0] = 90; A.pages[PG_SUB][2] = 20; A.pages[PG_FLT1][6] = 30; A.pages[PG_GRIT][6] = 40;
        const cost::VoiceDesc d = descOf(*r.pj, 0);
        ok(d.sub && d.noise && d.hpf && d.clip, "the sub, noise, HPF and clip are charged");
        A.pages[PG_SUB][0] = saved[0]; A.pages[PG_SUB][2] = saved[1]; A.pages[PG_FLT1][6] = saved[2]; A.pages[PG_GRIT][6] = saved[3];
        A.pages[PG_OSC1][0] = 0; A.pages[PG_OSC1][2] = 60;
        ok(descOf(*r.pj, 0).pd1, "a sine with PD AMT up runs the PD oscillator");
        A.pages[PG_OSC1][2] = 0; A.pages[PG_MOD][4] = 0; A.pages[PG_MOD][1] = 0;
        ok(!descOf(*r.pj, 0).pd1, "a plain sine does not");
        A.pages[PG_SUB][4] = 4; A.pages[PG_SUB][5] = 0; A.pages[PG_OSC1][7] = 0;
        ok(descOf(*r.pj, 0).slot[0].unison == 1, "a stack with no detune is one oscillator");
        A.pages[PG_SUB][5] = 20;
        ok(descOf(*r.pj, 0).slot[0].unison == 4, "with detune it is four");
        const int16_t lvl = A.pages[PG_OSC1][6];
        A.pages[PG_OSC1][6] = 0;
        ok(!descOf(*r.pj, 0).slot[0].audible, "osc 1 at level 0 is not charged");
        A.pages[PG_OSC1][6] = lvl;
        A.pages[PG_SUB][4] = 1; A.pages[PG_SUB][5] = 0;
    }
    { Str<16> b; A.pages[PG_FLT1][0] = 7; costBadge(b, *r.pj, 0, PG_FLT1, 0); ok(b.c_str()[0] == '+', "the MODEL knob shows what a dearer model adds: " + std::string(b.c_str())); A.pages[PG_FLT1][0] = 0; }
    { Str<16> b; A.pages[PG_GRIT][0] = 8; A.pages[PG_GRIT][7] = 127; costBadge(b, *r.pj, 0, PG_GRIT, 7); ok(b.c_str()[0] == '+', "the converter's cost shows on its MIX"); A.pages[PG_GRIT][0] = 16; }
    ok(!menuHeavy(5) && !menuHeavy(7), "no model is heavy on the lean voice");
    A.pages[PG_FLT1][0] = 7; A.pages[PG_FLT2][5] = 1; A.pages[PG_FLT2][0] = 5;
    Text lt;
    ok(!enforceHeavy(*r.pj, 0, PG_FLT2, 0, lt), "two of the dearest filters are allowed");
    A.pages[PG_FLT1][0] = 0; A.pages[PG_FLT2][5] = 0;
    {   // all the parts at the top, with the dearest settings the panel has, go over, and the meter says so
        Rig o;
        for (int i = 0; i < 2; ++i) {
            Part& p = o.pj->parts[i];
            p.voices = 5; p.pages[PG_OSC2][6] = 100; p.pages[PG_SUB][0] = 100; p.pages[PG_FLT1][0] = 7; p.pages[PG_SUB][4] = 4; p.pages[PG_SUB][5] = 30;
            o.pj->fx.amp[i][0] = 3;
        }
        o.pj->fx.send[0][1] = 80;
        ok(voiceTotal(*o.pj) > kVoiceBudget, "a project that asks too much goes over: " + std::to_string(voiceTotal(*o.pj)));
        o.key(Key::Mixer); o.key(Key::Mixer); ok(o.cell(7).warn, "and the meter turns red");
    }
#else
    ok(voiceTotal(*r.pj) <= 5.0f + 1e-6f && voiceTotal(*r.pj) > 3, "default total fits the budget");
    r.key(Key::Mixer); r.key(Key::Mixer);
    ok(r.ui->mixSub() == 1 && r.label(6) == "VOICES G", "MIXER view 2: a knob for each part's voices");
    ok(r.val(7) == "5.0/5" && !r.cell(7).warn, "default cost 2 + 2 + 4 x 0.25 = 5.0, the parts no lane plays not counted: " + r.val(7));
    r.turn(2, 4); ok(r.pj->parts[2].voices == 8 && r.cell(7).warn, "8 sampler voices = cost 6 turns the meter red");
    r.turn(2, -4);
    r.key(Key::Mixer); ok(r.ui->mixSub() == 2 && r.label(0) == "PRIO A" && r.label(6) == "PRIO G", "MIXER view 3: PRIO A..G");
    const int p0 = r.pj->parts[0].priority; r.turn(0, 1); ok(r.pj->parts[0].priority == (p0 + 1 > 3 ? 3 : p0 + 1), "encoder changes the priority"); r.turn(0, -1);
    r.key(Key::Mixer); ok(r.ui->mixSub() == 3 && r.label(3) == "AUTO DEGRADE", "MIXER view 4: AUTO DEGRADE");
    ok(r.cell(4).ro && r.cell(5).ro, "CPU LOAD and DEGRADE are read-only");
    r.turn(3, -1); ok(!r.ui->autoDegrade() && !r.sv.autoDeg, "AUTO DEGRADE off reaches the engine"); r.turn(3, 1);
    r.key(Key::Mixer); ok(r.ui->mixSub() == 0, "MIXER goes round its four views"); r.home();
    // heavy rule through the knobs
    Part& A = r.pj->parts[0];
    A.pages[PG_FLT1][0] = 0; A.pages[PG_FLT2][0] = 1; A.pages[PG_FLT2][5] = 0;
    r.key(Key::Flt1); r.turn(0, 7);
    ok(A.pages[PG_FLT1][0] == 7 && cost::heavyCount(descOf(*r.pj, 0)) == 1, "F1 can be a heavy filter");
    ok(has(r.cell(0).tag.c_str(), "HEAVY"), "the heavy model cell shows HEAVY: " + std::string(r.cell(0).tag.c_str()));
    A.pages[PG_FLT2][5] = 1; A.pages[PG_FLT2][0] = 4; A.pages[PG_FLT1][0] = 5;
    r.key(Key::Flt2); r.turn(0, 3);
    ok(cost::heavyCount(descOf(*r.pj, 0)) == 1 && A.pages[PG_FLT2][0] == 7 && A.pages[PG_FLT1][0] == 4, "choosing CLASSIC on F2 sends ACID on F1 to LADDER");
    ok(has(r.toast(), "ONE HEAVY FILTER"), "a toast explains it");
    ok(menuHeavy(5) && menuHeavy(7) && !menuHeavy(4) && !menuHeavy(0) && !menuHeavy(6), "ACID and CLASSIC are heavy; LADDER, LP, SOVIET are not");
    // p-lock rule
    A.pages[PG_FLT1][0] = 7; A.pages[PG_FLT2][5] = 1; A.pages[PG_FLT2][0] = 4;
    Step blank = Step(); blank.vel = 100;
    Text t;
    ok(lockedValue(*r.pj, 0, lockKey(PG_FLT2, 0), 5, blank, t) == 4, "lock F2=ACID beside heavy F1 > LADDER");
    ok(lockedValue(*r.pj, 0, lockKey(PG_FLT2, 0), 6, blank, t) == 6, "lock F2=SOVIET stays");
    Step s2 = blank; s2.setLock(lockKey(PG_FLT2, 0), 5);
    ok(lockedValue(*r.pj, 0, lockKey(PG_FLT2, 5), 1, s2, t) == 0, "lock F2 ON with a heavy F2 lock beside heavy F1 stays off");
#endif
    // the cost badge
    A.pages[PG_FLT1][0] = 0; A.pages[PG_FLT2][5] = 0; A.pages[PG_FLT1][3] = 40;
    { Str<16> b; costBadge(b, *r.pj, 0, PG_FLT1, 3); ok(b.c_str()[0] == '+', "the drive badge shows a cost: " + std::string(b.c_str())); }
    A.pages[PG_FLT1][3] = 0;
    { Str<16> b; costBadge(b, *r.pj, 0, PG_FLT1, 3); ok(b.empty(), "no badge when the feature is off"); }
    // the ladder
    const Ladder& lad = r.ui->ladder();
    ok(lad.count > 0, "the ladder is built: " + std::to_string(lad.count));
    int lowest = 0; for (int i = 1; i < kParts; ++i) if (r.pj->parts[i].priority < r.pj->parts[lowest].priority) lowest = i;
    ok(lad.steps[0].part == lowest, "the first victim is the lowest-priority part");
    ok(effTotal(*r.pj, lad, lad.count) < voiceTotal(*r.pj), "fully degraded costs less");
    ok(r.pj->parts[2].priority > r.pj->parts[0].priority && r.pj->parts[2].priority > r.pj->parts[1].priority, "the sampler part is kept longest");
}

static void testProjectFile() {
    Rig r;
    r.key(Key::Info); ok(r.ui->mode() == Mode::ProjectM && r.label(0) == "KEY" && r.label(1) == "SCALE", "PROJECT: T1 KEY, T2 SCALE");
    r.turn(0, 9); r.turn(1, 2); ok(r.pj->key == 9 && std::string(kScales[r.pj->scale].name) == "MINOR", "A minor");
    r.turn(0, -9); r.turn(1, -2);
    r.turn(4, 2); ok(r.ui->slot() == 2 && r.val(4) == "3 EMPTY", "B1 picks card slot 3: " + r.val(4));
    r.key(Key::Ok); ok(has(r.toast(), "SLOT 3 IS EMPTY"), "loading an empty slot says so");
    r.shiftKey(Key::Ok); ok(!r.sv.card[2].empty() && has(r.toast(), "SAVED TO SLOT 3"), "SHIFT + OK saves the whole project to slot 3");
    ok(r.ui->loadedSlot() == 2 && r.val(4) == "3", "the slot is no longer empty");
    const int bpm = r.pj->bpm, root = r.pj->patterns[0].lanes[0].root, cut = r.pj->parts[0].pages[PG_FLT1][1];
    r.pj->bpm = 77; r.pj->patterns[0].lanes[0].root = 30; r.pj->parts[0].pages[PG_FLT1][1] = 5; r.pj->songs[0].nRows = 0; r.pj->patterns[0].lanes[7].smpParam(PG_SMP, 0) = 3;
    r.key(Key::Ok);
    ok(r.pj->bpm == bpm && r.pj->patterns[0].lanes[0].root == root && r.pj->parts[0].pages[PG_FLT1][1] == cut && r.pj->songs[0].nRows == 4 &&
           r.pj->patterns[0].lanes[7].sample() != 3, "OK loads it back: tempo, lanes, sounds, songs, samples");
    ok(has(r.toast(), "PROJECT 3 LOADED"), "says what was loaded: " + r.toast());
    r.shiftKey(Key::Clear); ok(r.pj->bpm == 77 && r.pj->patterns[0].lanes[0].root == 30, "SHIFT + CLEAR undoes the load");
    r.key(Key::Ok);
    // overwrite and undo the save
    r.pj->bpm = 99; r.shiftKey(Key::Ok); ok(has(r.toast(), "OVERWROTE SLOT 3") && has(r.toast(), "UNDOES"), "overwriting says how to undo");
    r.shiftKey(Key::Clear);
    { SlotInfo si; r.sv.slotInfo(2, si); ok(si.bpm == bpm, "undo puts the old file back"); }
    // a damaged file is refused
    std::vector<uint8_t> bad = r.sv.card[2]; bad[200] ^= 0xFF; r.sv.card[3] = bad; r.ui->refreshCard();   // a card changed outside the UI is re-read
    r.key(Key::Down); r.pj->bpm = 101; r.key(Key::Ok);
    ok(r.pj->bpm == 101 && has(r.toast(), "COULD NOT BE READ"), "a damaged file is refused, the project stays");
    // sanitize
    Project* p = new Project; makeDefaultProject(*p);
    p->patterns[0].lanes[0].n = 0; p->patterns[0].lanes[0].st[0].chord = 200; p->bpm = 5; p->parts[0].voices = 40;
    sanitize(*p);
    ok(p->patterns[0].lanes[0].n == 1 && p->patterns[0].lanes[0].st[0].chord == kChordCount - 1 && p->bpm == 40 && p->parts[0].voices == 5, "sanitize clamps a damaged project");
    delete p;
}

static void testSilentKnobs() {
    // every knob that is neither disabled nor read-only changes something when turned
    Rig r;
    struct M { Key k; int presses; const char* name; };
    const M modes[] = {{Key::Lane, 0, "PLAY"}, {Key::StepK, 1, "STEP 1"}, {Key::StepK, 2, "STEP 2"}, {Key::SongK, 1, "SONG"},
                       {Key::Mixer, 1, "MIXER 1"}, {Key::Mixer, 2, "MIXER 2"}, {Key::Info, 1, "PROJECT"}};
    std::string silent;
    for (const M& m : modes)
        for (int k = 0; k < 8; ++k) {
            Rig q;
            for (int i = 0; i < m.presses; ++i) q.key(m.k);
            Cell c = q.cell(k);
            if (c.dis || c.ro) continue;
            auto snap = [&]() {
                std::vector<uint8_t> s((const uint8_t*)q.pj, (const uint8_t*)q.pj + sizeof(Project));
                s.push_back((uint8_t)q.tp.pat); s.push_back((uint8_t)q.tp.nextPat); s.push_back((uint8_t)q.tp.songIdx);
                s.push_back((uint8_t)q.ui->slot()); s.push_back((uint8_t)q.ui->songRow()); s.push_back((uint8_t)q.ui->autoDegrade());
                return s;
            };
            const auto s0 = snap();
            q.shiftTurn(k, 1);
            bool changed = snap() != s0;
            if (!changed) { q.shiftTurn(k, -1); changed = snap() != s0; }
            if (!changed) silent += std::string(m.name) + " " + c.label.c_str() + ", ";
        }
    ok(silent.empty(), "every knob that looks like a setting changes something: " + silent);
    (void)r;
}

static void testReviewFindings() {
    {   // an undo that shrinks the song under the row cursor: the cursor follows, + never writes past the rows
        Rig r;
        r.key(Key::SongK);
        Song big; memset(&big, 0, sizeof(big)); big.loop = 1; big.nRows = kSongRows; for (auto& x : big.rows) x = {0, 1};
        r.pj->songs[1] = big; r.turn(0, 1); r.key(Key::Copy); r.turn(0, -1);   // copy a 64-row song
        r.pj->songs[0].nRows = 1;
        r.key(Key::Paste); for (int k = 0; k < 70; ++k) r.key(Key::Down);
        ok(r.ui->songRow() == 63, "on the pasted 64-row song, row 64");
        r.shiftKey(Key::Clear);
        ok(r.ui->song().nRows == 1, "undo: the song has one row again");
        const uint8_t next = r.pj->songs[1].nRows;
        r.key(Key::Add);
        ok(r.ui->song().nRows == 2 && r.ui->songRow() == 1 && r.pj->songs[1].nRows == next, "ADD adds row 2, nothing past the song is touched");
    }
    {   // a late end never releases a newer note of the same pitch
        Rig r;
        for (Lane& l : r.pj->patterns[0].lanes) l.mute = 1;
        Lane& a = r.lane(0);
        a.mute = 0; a.eu = 0; a.n = 2; for (Step& s : a.st) { s.ov = 1; s.gate = 40; s.note = 0; }   // every step, gate longer than a step
        r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0;
        r.run(2000);
        std::vector<int> ended;
        bool ok1 = true;
        for (auto& f : r.rec.offs) { for (int h : ended) if (h == f.handle) ok1 = false; ended.push_back(f.handle); }
        ok(ok1 && r.rec.offs.size() + 2 >= r.rec.ons.size(), "overlapping notes of one pitch: each end releases its own note, once");
    }
    {   // a full queue releases early instead of dropping an end
        Rig r;
        for (Lane& l : r.pj->patterns[0].lanes) { l.mute = 0; l.eu = 0; l.n = 1; l.part = 0; l.st[0].ov = 1; l.st[0].chord = 10; l.st[0].gate = 127; }
        r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0;
        r.run(6000);
        r.tp.playing = 0; r.run(4000);
        ok(r.rec.ons.size() == r.rec.offs.size() && r.seq->pending() == 0, "10 lanes of 5-note chords at gate 127: every note ends (" +
           std::to_string(r.rec.ons.size()) + " on, " + std::to_string(r.rec.offs.size()) + " off)");
    }
    {   // STOP + PLAY between two blocks is a new start, not a stop
        Rig r;
        r.key(Key::Play); r.run(10);
        r.key(Key::Play); r.key(Key::Play);   // stop and play again before the next block
        r.run(10);
        ok(r.tp.playing && r.tp.gstep >= 0, "STOP + PLAY between blocks: still playing, from the top");
    }
    {   // a part made a CATHODE keeps at most 5 voices, undo gives 8 back
        Rig r;
        r.pj->parts[2].voices = 8; r.tap(7); r.key(Key::File); r.shiftTurn(3, -1);
        ok(r.pj->parts[2].type == PT_CATHODE && r.pj->parts[2].voices == 5, "sampler with 8 voices made a CATHODE: 5 voices");
        r.shiftKey(Key::Clear); ok(r.pj->parts[2].type == PT_SAMPLER && r.pj->parts[2].voices == 8, "undo: a sampler with 8 again");
    }
    {   // the long toast is not cut
        Rig r;
        r.tap(6); r.shiftTurn(2, 1);
        ok(r.toast() == "LANE 7 NOW PLAYS ON PART C (SAMPLER: ITS OWN SAMPLE, NOTES = PITCH)", "the full toast: " + r.toast());
    }
    {   // the degrade toast
        Rig r;
        r.sv.level = 1; r.ui->tick();
        ok(has(r.toast(), "DEGRADE 1: "), "the governor climbing says what it gave up: " + r.toast());
        r.sv.level = 0; r.ui->tick();
        ok(has(r.toast(), "RESTORED: "), "and what it took back");
    }
    {   // a lock does not follow into the next pattern
        Rig r;
        r.key(Key::StepK); r.hold(1, 450);
        ok(r.ui->lockOpen(), "lock open");
        r.tp.pat = 1;
        ok(!r.ui->lockOpen(), "after a pattern switch the lock is closed at once (before tick)");
    }
    {   // sanitize re-applies the heavy-filter rule and drops duplicate locks
        Project* p = new Project; makeDefaultProject(*p);
        p->parts[0].pages[PG_FLT1][0] = 7; p->parts[0].pages[PG_FLT2][0] = 5; p->parts[0].pages[PG_FLT2][5] = 1;
        Step& s = p->patterns[0].lanes[0].st[0]; s.nLocks = 2; s.locks[0] = {9, 0, 10}; s.locks[1] = {9, 0, 20};
        sanitize(*p);
        ok(cost::heavyRuleOk(descOf(*p, 0)) && s.nLocks == 1, "sanitize: one heavy filter, no duplicate locks");
        delete p;
    }
}

// a library with the built-in samples and a fake card: `names` become "Kick/<name>.wav", sorted, after the built-ins
static void fakeCard(SampleLib& lib, std::vector<int16_t>& store, std::vector<std::string>& paths, std::vector<std::string> names) {
    lib = SampleLib();
    buildFactorySamples(lib, store.data(), store.size());
    std::sort(names.begin(), names.end());
    paths.clear();
    paths.reserve(names.size());
    const int cat = lib.addCategory("KICK");
    for (const std::string& n : names) {
        paths.push_back("Kick/" + n + ".wav");
        SampleItem& it = lib.items[lib.count++];
        memset(&it, 0, sizeof(it));
        snprintf(it.name, sizeof(it.name), "%s", n.c_str());
        it.cat = (uint8_t)cat;
        it.path = paths.back().c_str();
    }
}

// The payload of a file older than version 8: lanes without the LOOP page, effects' lock keys a page lower, no loop points. Before version 7 the same again for
// the SLICE page, and no slices. Older still: three parts, the effects' amps and sends for three, and only what that version kept.
static std::vector<uint8_t> oldPayload(const Project& p, int version) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&p);
    const int parts = version < 6 ? 3 : kParts, gone = version < 7 ? 2 : 1;   // pages the file has not got
    std::vector<uint8_t> out(b, b + offsetof(Project, parts) + parts * sizeof(Part));
    for (const Pattern& pt : p.patterns)
        for (const Lane& l : pt.lanes) {
            Lane c = l;
            for (Step& s : c.st)
                for (int k = 0; k < s.nLocks; ++k) if (s.locks[k].key >= PG_COUNT * kParamsPerPage) s.locks[k].key = (uint8_t)(s.locks[k].key - gone * kParamsPerPage);
            const uint8_t* cb = reinterpret_cast<const uint8_t*>(&c);
            out.insert(out.end(), cb, cb + offsetof(Lane, st) - gone * kParamsPerPage * 2);
            out.insert(out.end(), reinterpret_cast<const uint8_t*>(c.st), reinterpret_cast<const uint8_t*>(c.st) + sizeof(c.st));
        }
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(p.songs), reinterpret_cast<const uint8_t*>(&p.midi));
    if (version >= 3) out.insert(out.end(), reinterpret_cast<const uint8_t*>(&p.midi), reinterpret_cast<const uint8_t*>(&p.fx));
    if (version >= 6) out.insert(out.end(), reinterpret_cast<const uint8_t*>(&p.fx), reinterpret_cast<const uint8_t*>(&p.fx + 1));
    if (version >= 7) out.insert(out.end(), reinterpret_cast<const uint8_t*>(p.slices), reinterpret_cast<const uint8_t*>(p.loops));
    if (version >= 4 && version < 6) {
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&p.fx.delay), reinterpret_cast<const uint8_t*>(&p.fx.amp));
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&p.fx.amp), reinterpret_cast<const uint8_t*>(&p.fx.amp[3]));
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&p.fx.send), reinterpret_cast<const uint8_t*>(&p.fx.send[3]));
    }
    return out;
}

static void testSamplePaths() {
    static SampleLib a, b, c;
    std::vector<int16_t> sa(kFactoryFrames), sb(kFactoryFrames), sc(kFactoryFrames);
    std::vector<std::string> pa, pb, pc;
    fakeCard(a, sa, pa, {"Bravo", "Delta"});
    fakeCard(b, sb, pb, {"Alpha", "Bravo", "Charlie", "Delta"});   // two samples added on the card, before and between
    fakeCard(c, sc, pc, {"Alpha", "Delta"});                       // Bravo removed
    const int bravoA = a.find("Kick/Bravo.wav"), deltaA = a.find("Kick/Delta.wav");
    ok(bravoA == 12 && deltaA == 13 && a.find("@Kick 808") == 0 && a.find("Kick/None.wav") < 0, "sample keys: card paths and built-in names");
    Project* pj = new Project;
    makeDefaultProject(*pj);
    pj->patterns[0].lanes[7].smp[0][0] = (int16_t)bravoA;
    pj->patterns[3].lanes[8].st[5].setLock(lockKey(PG_SMP, 0), (int16_t)deltaA);
    MemSink sink;
    ok(writeProject(*pj, sink, &a), "saved with the sample paths");
    // the same card: the same numbers
    {
        Project* out = new Project; Project* scratch = new Project; int missing = -1;
        MemSource src(sink.bytes);
        ok(readProject(src, *scratch, *out, &a, &missing) && missing == 0 && out->patterns[0].lanes[7].smp[0][0] == bravoA, "same card: same samples");
        delete out; delete scratch;
    }
    // samples added: the numbers move, the samples stay
    {
        Project* out = new Project; Project* scratch = new Project; int missing = -1;
        MemSource src(sink.bytes);
        ok(readProject(src, *scratch, *out, &b, &missing) && missing == 0, "a card with more samples: nothing missing");
        ok(out->patterns[0].lanes[7].smp[0][0] == b.find("Kick/Bravo.wav") &&
               out->patterns[3].lanes[8].st[5].lockValue(lockKey(PG_SMP, 0), -1) == b.find("Kick/Delta.wav"),
           "lane sample and sample lock follow their files (Bravo " + std::to_string(bravoA) + " -> " + std::to_string(b.find("Kick/Bravo.wav")) + ")");
        ok(out->patterns[0].lanes[8].smp[0][0] == pj->patterns[0].lanes[8].smp[0][0], "built-in samples keep their numbers");
        delete out; delete scratch;
    }
    // a sample removed: counted, its lanes play sample 0
    {
        Project* out = new Project; Project* scratch = new Project; int missing = -1;
        MemSource src(sink.bytes);
        ok(readProject(src, *scratch, *out, &c, &missing) && missing == 1 && out->patterns[0].lanes[7].smp[0][0] == 0, "a removed sample: 1 missing, its lane plays sample 0");
        delete out; delete scratch;
    }
    // a damaged table is refused
    {
        std::vector<uint8_t> bad = sink.bytes;
        bad[bad.size() - 3] ^= 0x20;
        Project* out = new Project; Project* scratch = new Project; int missing = 0;
        MemSource src(bad);
        ok(!readProject(src, *scratch, *out, &a, &missing), "a damaged sample table fails the CRC");
        delete out; delete scratch;
    }
    // a version 1 file still loads (numbers as they were)
    {
        ProjectHeader h;
        makeHeader(*pj, h);
        h.version = 1;
        const std::vector<uint8_t> body = oldPayload(*pj, 1);   // before the lanes had MIDI channels
        h.payloadSize = (uint32_t)body.size();
        h.crc = crc32(body.data(), body.size());
        MemSink v1;
        v1.write(&h, sizeof(h));
        v1.write(body.data(), body.size());
        Project* out = new Project; Project* scratch = new Project; int missing = -1;
        MemSource src(v1.bytes);
        ok(readProject(src, *scratch, *out, &b, &missing) && out->patterns[0].lanes[7].smp[0][0] == bravoA, "a version 1 file loads as it was saved");
        ok(out->midi[0].ch == 1 && out->midi[7].ch == 10 && out->midi[8].note == 38, "a version 1 file gets the default lane channels");
        delete out; delete scratch;
    }
    // through the UI: the load says what is missing
    {
        Rig r;
        r.sv.lib = &a;
        r.pj->patterns[0].lanes[7].smp[0][0] = (int16_t)bravoA;
        r.key(Key::Info);
        r.shiftKey(Key::Ok);
        r.sv.lib = &c;
        r.key(Key::Ok);
        ok(has(r.toast(), "1 SAMPLE MISSING"), "loading on a card without the sample says so: " + r.toast());
    }
    // the cache compaction: kept samples move down, the rest are dropped
    {
        static SampleLib l;
        std::vector<int16_t> st(kFactoryFrames), cache(1000);
        std::vector<std::string> ps;
        fakeCard(l, st, ps, {"A", "B", "C"});
        const int i0 = l.find("Kick/A.wav"), i1 = l.find("Kick/B.wav"), i2 = l.find("Kick/C.wav");
        size_t used = 0;
        for (int i : {i0, i1, i2}) {
            for (int k = 0; k < 100; ++k) cache[used + k] = (int16_t)(i * 1000 + k);
            l.items[i].data = &cache[used];
            l.items[i].length = 100;
            used += 100;
        }
        bool keep[kMaxSamples] = {};
        keep[i0] = keep[i2] = true;
        const size_t now = l.compactCache(cache.data(), used, keep);
        ok(now == 200 && l.items[i1].data == nullptr && l.items[i0].data == &cache[0] && l.items[i2].data == &cache[100] &&
               l.items[i2].data[0] == i2 * 1000 && l.items[i2].data[99] == i2 * 1000 + 99,
           "compaction: the unused sample is dropped, the last one moves down intact");
        ok(l.items[0].data != nullptr, "the built-in samples are not in the cache and stay");
    }
    delete pj;
}

// The lanes' MIDI channels: which lane a note plays, what a lane sends, and the PROJECT screen's MIDI view.
static void testMidiMap() {
    Project* pj = new Project;
    makeDefaultProject(*pj);
    ok(pj->midi[0].ch == 1 && pj->midi[6].ch == 7 && pj->midi[7].ch == 10 && pj->midi[9].ch == 10, "default: lanes 1-7 on 1-7, 8-10 share 10");
    ok(lanesOnChannel(*pj, 10) == 3 && lanesOnChannel(*pj, 1) == 1 && lanesOnChannel(*pj, 11) == 0, "lanes per channel");
    MidiRoute r;
    // a lane alone on its channel plays every note
    const bool smp0 = isSampler(*pj, pj->patterns[0].lanes[0].part);
    ok(routeIn(*pj, 0, 1, 64, r) && r.lane == 0 && r.pitch == (smp0 ? 4 : 64), "channel 1 note 64: lane 1");
    // the shared channel: the trigger note picks the lane, other notes play nothing
    ok(routeIn(*pj, 0, 10, 38, r) && r.lane == 8, "channel 10 note 38: lane 9");
    ok(r.pitch == (r.sampler ? 0 : pj->patterns[0].lanes[8].root), "a trigger note plays the lane's own sound");
    ok(!routeIn(*pj, 0, 10, 40, r), "channel 10 note 40: no lane");
    ok(!routeIn(*pj, 0, 11, 60, r), "channel 11: no lane");
    // out: the rule backwards
    ok(noteOut(*pj, 8, 5, true) == 38 && noteOut(*pj, 8, 70, false) == 38, "a lane on a shared channel sends its trigger note");
    ok(noteOut(*pj, 0, 67, false) == 67, "a synth lane alone sends its pitch");
    ok(noteOut(*pj, 0, 7, true) == 67 && noteOut(*pj, 0, 100, true) == 127, "a sampler lane alone sends base + offset, clamped");
    // a lane moved off the shared channel leaves the other two sharing; the last one alone plays every note
    pj->midi[9].ch = 0;
    pj->midi[8].ch = 11;
    ok(lanesOnChannel(*pj, 10) == 1 && routeIn(*pj, 0, 10, 50, r) && r.lane == 7, "lane 8 alone on 10 hears every note");
    ok(!routeIn(*pj, 0, 0, 50, r) && !routeIn(*pj, 0, 17, 50, r), "channel 0 / 17 route nowhere");
    // a version 2 file (before the lanes had channels, with its sample table): the default table
    {
        Project* v = new Project;
        makeDefaultProject(*v);
        v->midi[0].ch = 16;   // not in a version 2 file
        MemSink sink;
        ok(writeProject(*v, sink, nullptr), "write");
        ProjectHeader h;
        memcpy(&h, sink.bytes.data(), sizeof(h));
        const std::vector<uint8_t> body = oldPayload(*v, 2);
        std::vector<uint8_t> table(sink.bytes.begin() + sizeof(h) + sizeof(Project), sink.bytes.end());
        h.version = 2;
        h.payloadSize = (uint32_t)body.size();
        h.crc = crc32(table.data(), table.size(), crc32(body.data(), body.size()));
        std::vector<uint8_t> v2((const uint8_t*)&h, (const uint8_t*)&h + sizeof(h));
        v2.insert(v2.end(), body.begin(), body.end());
        v2.insert(v2.end(), table.begin(), table.end());
        Project* out = new Project; Project* scratch = new Project;
        MemSource src(v2);
        ok(readProject(src, *scratch, *out, nullptr, nullptr) && out->midi[0].ch == 1 && out->midi[9].note == 42, "a version 2 file loads with the default lane channels");
        // out-of-range values in a file are clamped
        v->midi[2].ch = 200; v->midi[2].note = 255;
        MemSink bad;
        ok(writeProject(*v, bad, nullptr), "write");
        MemSource src2(bad.bytes);
        ok(readProject(src2, *scratch, *out, nullptr, nullptr) && out->midi[2].ch == 0 && out->midi[2].note <= 127 && out->midi[0].ch == 16,
           "a damaged channel turns the lane off, a note is clamped, the rest kept");
        delete out; delete scratch; delete v;
    }
    // the file: v3 keeps the table
    {
        MemSink sink;
        ok(writeProject(*pj, sink, nullptr), "write");
        Project* out = new Project; Project* scratch = new Project;
        MemSource src(sink.bytes);
        ok(readProject(src, *scratch, *out, nullptr, nullptr) && out->midi[8].ch == 11 && out->midi[9].ch == 0, "the lane channels are saved");
        delete out; delete scratch;
    }
    delete pj;

    // the PROJECT screen, view 2
    Rig r2;
    r2.key(Key::Info);
    ok(r2.ui->projSub() == 0, "PROJECT opens on key and card");
    r2.key(Key::Info);
    ok(r2.ui->projSub() == 1 && has(r2.toast(), "MIDI"), "INFO again: the MIDI view");
    ok(r2.labels(0) == "LANE,MIDI CH,BASE NOTE,ON CHANNEL" || r2.labels(0) == "LANE,MIDI CH,TRIG NOTE,ON CHANNEL", "MIDI view knobs: " + r2.labels(0));
    ok(r2.val(0) == "LANE 1" && r2.val(1) == "CH 1" && r2.val(3) == "ALONE", "lane 1: " + r2.val(1) + " " + r2.val(3));
    r2.turn(0, 8);
    ok(r2.val(0) == "LANE 9" && r2.label(2) == "TRIG NOTE" && r2.val(2) == "38 D2" && r2.val(3) == "3 LANES", "lane 9: " + r2.val(2) + " " + r2.val(3));
    ok(r2.cell(3).ro, "ON CHANNEL is read-only");
    r2.turn(2, 2);
    ok(r2.pj->midi[8].note == 40, "T3 sets the trigger note");
    r2.turn(1, -10);
    ok(r2.pj->midi[8].ch == 0 && r2.val(1) == "OFF" && r2.cell(2).dis, "MIDI CH down to OFF; the note is then unused");
    r2.turn(1, 30);
    ok(r2.pj->midi[8].ch == 16, "MIDI CH stops at 16");
    r2.key(Key::Up);
    ok(r2.ui->midiLane() == 7, "the list keys move the lane");
    r2.key(Key::Info);
    r2.key(Key::Info); ok(r2.ui->projSub() == 0, "INFO again and again: back to key and card");
    // the part's old MIDI CH knob points to the lanes' table
    Rig r3;
    r3.key(Key::Glob);
    int at = -1;
    for (int i = 0; i < 8; ++i) if (r3.label(i) == "MIDI CH") at = i;
    ok(at >= 0 && r3.val(at) == "IN PROJECT" && r3.cell(at).ro, "GLOBAL MIDI CH: read-only, IN PROJECT");
    if (at >= 0) { r3.turn(at, 1); ok(has(r3.toast(), "PROJECT > MIDI"), "turning it says where the channels are: " + r3.toast()); }
}

// The end-to-end stress's findings: UI state that pointed past the model after the model changed under it.
static void testSettle() {
    {   // a sample category kept when the part on screen becomes a synth part
        Rig r;
        r.tap(7);
        ok(isSampler(*r.pj, r.ui->part()), "lane 8 plays a sampler part");
        r.key(Key::File);
        r.turn(1, 30);
        ok(r.ui->browseCat() > 0, "a sample category picked");
        r.tap(0);
        ok(!isSampler(*r.pj, r.ui->part()) && r.ui->browseCat() == 0 && !r.val(1).empty(), "a synth lane: the preset categories, from the first");
        ok(r.ui->browseIdx() == 0, "and the list from the top");
    }
    {   // a sound page the new lane's part does not have
        Rig r;
        r.tap(0);
        r.key(Key::Flt1);
        r.tap(7);
        ok(r.ui->page() == PG_SFLT, "FLT1 on a synth lane, then a sampler lane: its FILTER page");
        r.key(Key::Osc1);
        r.tap(0);
        ok(r.ui->page() == PG_OSC1, "SMP on a sampler lane, then a synth lane: OSC1");
    }
    {   // paste of a shorter lane: the cursor comes back inside it
        Rig r;
        r.key(Key::StepK);
        r.ln().n = 16;
        r.key(Key::Copy);
        r.ln().n = 24;
        r.key(Key::Left);
        ok(r.ui->cursor() == 23, "cursor on step 24");
        r.key(Key::Paste);
        r.cell(0);
        ok(r.ln().n == 16 && r.ui->cursor() < 16 && r.ui->stepPage() <= 1, "pasted 16 steps: cursor " + std::to_string(r.ui->cursor()));
    }
    {   // a step held while its lane gets shorter: no lock opens past the end
        Rig r;
        r.key(Key::StepK);
        r.ln().n = 16;
        r.ui->layerDown(7);
        r.ln().n = 4;
        r.sv.now += 450;
        r.ui->tick();
        r.ui->layerUp(7);
        ok(!r.ui->lockOpen(), "held step 8 on a lane now 4 long: no lock");
    }
    {   // hold + turn with a lock open: the held step is edited (the lock moves to it)
        Rig r;
        r.key(Key::StepK);
        Lane& L = r.ln();
        r.hold(4, 450);
        ok(r.ui->lockOpen() && r.ui->lockIdx() == 4, "lock on step 5");
        r.key(Key::Flt1);
        r.ui->layerDown(6);
        r.sv.now += 400;
        r.ui->tick();
        r.turn(1, 3);
        r.ui->layerUp(6);
        ok(L.st[6].hasLock(lockKey(PG_FLT1, 1)) && !L.st[4].hasLock(lockKey(PG_FLT1, 1)), "hold step 7 + turn: step 7's lock, not step 5's");
        ok(r.ui->lockOpen() && r.ui->lockIdx() == 6, "the lock stays on step 7");
        // in STEP: hold another step and turn NOTE
        r.key(Key::StepK);
        int noteKnob = -1;
        for (int i = 0; i < 8; ++i) if (r.label(i) == "NOTE") noteKnob = i;
        if (noteKnob >= 0) {
            const int before5 = L.st[5].note, before6 = L.st[6].note;
            r.ui->layerDown(5);
            r.turn(noteKnob, 2);
            r.ui->layerUp(5);
            ok(L.st[5].note != before5 && L.st[6].note == before6, "STEP, lock open: hold step 6 + turn NOTE edits step 6");
        } else ok(false, "STEP page has a NOTE knob: " + r.labels(0) + " / " + r.labels(4));
    }
    {   // song rows removed while the song plays
        Rig r;
        r.key(Key::SongK);
        while (r.ui->song().nRows < 4) r.key(Key::Add);
        while (r.ui->songRow() > 0) r.key(Key::Up);
        r.key(Key::Play);
        ok(r.tp.songPlaying && r.tp.playRow == 0, "the song plays from row 1");
        r.key(Key::Down); r.key(Key::Down); r.key(Key::Ok);
        ok(r.tp.jumpTo == 2, "row 3 next");
        r.key(Key::Up); r.key(Key::Up);
        r.key(Key::Clear);   // row 1 (playing) removed
        ok(r.tp.jumpTo == 1, "the jump follows its row up: " + std::to_string(r.tp.jumpTo));
        ok(r.sv.holds > 0 && r.sv.held == 0, "the row edit holds the audio, and lets it go");
        // undo while the removed row would still be playing: it is simply back, no jump left behind
        r.shiftKey(Key::Clear);
        while (r.ui->songRow() > 0) r.key(Key::Up);
        const int rows = r.ui->song().nRows;
        r.tp.jumpTo = -1;
        r.tp.playRow = 0;
        r.key(Key::Clear);   // the playing row
        ok(r.tp.jumpTo == 0 && r.ui->song().nRows == rows - 1, "removing the playing row: the next one moves up and plays next");
        r.shiftKey(Key::Clear);
        ok(r.tp.jumpTo == -1 && r.tp.playRow == 0 && r.ui->song().nRows == rows && r.sv.held == 0, "undo before the pattern ends: the row is back, no jump: " +
           std::to_string(r.tp.jumpTo) + " row " + std::to_string(r.tp.playRow));
        while (r.ui->song().nRows) r.key(Key::Clear);
        r.cell(0);
        ok(r.tp.playing && !r.tp.songPlaying, "every row removed: the song stops, the pattern plays on");
        r.key(Key::Stop);
    }
    {   // a shorter song pasted over the playing one; a jump left over from a song that stopped
        Rig r;
        r.key(Key::SongK);
        r.key(Key::Add); r.key(Key::Add);
        r.key(Key::Copy);
        const int n = r.ui->song().nRows;
        r.key(Key::Clear); r.key(Key::Clear);   // two fewer rows than the copy
        r.key(Key::Add); r.key(Key::Add); r.key(Key::Add);
        while (r.ui->song().nRows > 2) r.key(Key::Clear);
        r.key(Key::Copy);
        while (r.ui->song().nRows < n) r.key(Key::Add);
        r.key(Key::Down); r.key(Key::Down);
        r.key(Key::Play);
        r.tp.playRow = (uint8_t)(r.ui->song().nRows - 1);
        r.key(Key::Paste);
        r.cell(0);
        ok(r.ui->song().nRows == 2 && (!r.tp.songPlaying || r.tp.playRow < 2), "a shorter song pasted while playing: the position stays inside it, row " + std::to_string(r.tp.playRow));
        r.tp.jumpTo = 1;
        r.key(Key::Stop);
        ok(r.tp.jumpTo == -1, "STOP clears a pending jump");
    }
}

// The effects' settings: knob values in real units, the FX screen, the project file.
static void testFx() {
    // every default lands inside its range and formats; the stored default maps back to the real default
    for (int v = 0; v < FX_SENDS; ++v) {
        for (int i = 0; i < kFxParams; ++i) {
            const FxParamDef& d = fxDef(v, i);
            const int def = fxDefault(v, i);
            Text t;
            formatFx(t, d, def);
            ok(def >= 0 && def <= fxMax(d) && !t.empty(), std::string("default ") + fxViewName(v) + " " + d.name + " = " + t.c_str());
            if (!d.list) ok(fabs(fxReal(d, def) - d.defReal) <= (d.exp ? d.defReal * 0.06 : (d.b - d.a) / 127.0 * 0.6 + 1e-9),
                            std::string("the default is the stored default: ") + fxViewName(v) + " " + d.name);
            ok(fxReal(d, 0) == (d.list ? 0 : d.a) || d.list, "the low end");
        }
    }
    ok(fabs(fxReal(fxDef(FX_DELAY, 1), 127) - 1000.0) < 1e-6 && fabs(fxReal(fxDef(FX_COMP, 1), 0) + 60.0) < 1e-9, "the ends of the ranges are the real limits");
    ok(fabs(delayMs(3, 0, 120) - 250.0) < 1e-9 && fabs(delayMs(1, 0, 120) - 500.0) < 1e-9 && fabs(delayMs(2, 0, 120) - 375.0) < 1e-9, "tempo-synced delay times");
    ok(delayMs(0, 127, 120) > 999.0 && delayMs(0, 0, 120) < 21.0, "FREE: the TIME knob");
    ok(compRatio(8) == 100 && compRatio(3) == 4, "compressor ratios");
    {   // negative dB values round to the nearest whole dB, not one lower
        Text t0, t1, t2, t3;
        formatFx(t0, fxDef(FX_COMP, 1), 0); formatFx(t1, fxDef(FX_COMP, 1), fxDefault(FX_COMP, 1));
        formatFx(t2, fxDef(FX_AMP, 2), fxDefault(FX_AMP, 2)); formatFx(t3, fxDef(FX_AMP, 4), 127);
        ok(t0 == "-60 dB" && t1 == "-18 dB" && t2 == "0 dB" && t3 == "+12 dB", std::string("dB formats: ") + t0.c_str() + " " + t1.c_str() + " " + t2.c_str() + " " + t3.c_str());
        Text u0, u1, u2;
        formatFx(u0, fxDef(FX_REVERB, 1), 127); formatFx(u1, fxDef(FX_DELAY, 3), 127); formatFx(u2, fxDef(FX_COMP, 3), 70);
        ok(u0 == "12 s" && u1 == "12 kHz", std::string("units do not round into the next: ") + u0.c_str() + " " + u1.c_str());
    }
    ok(fabs(delayMs(5, 0, 120) - 125.0) < 1e-9 && fabs(delayMs(6, 0, 120) - 500.0 / 3) < 1e-9 && fabs(delayMs(7, 0, 120) - 250.0 / 3) < 1e-9 && fabs(delayMs(4, 0, 120) - 187.5) < 1e-9,
       "1/16, triplet and dotted 1/16 delay times");

    // a new project: every send off (the sound is as it was), the amp and compressor off
    Project* pj = new Project;
    makeDefaultProject(*pj);
    bool quiet = true;
    for (int p = 0; p < 3; ++p) for (int i = 0; i < kSends; ++i) quiet = quiet && pj->fx.send[p][i] == 0;
    ok(quiet && pj->fx.comp[0] == 0 && pj->fx.amp[0][0] == 0 && pj->fx.amp[2][0] == 0, "defaults: no sends, comp off, amps off");
    // damaged values are clamped
    pj->fx.delay[0] = 99; pj->fx.reverb[1] = -5; pj->fx.amp[1][6] = 200; pj->fx.send[0][1] = 4000; pj->fx.send[2][3] = 77;
    sanitizeFx(pj->fx);
    ok(pj->fx.delay[0] == 7 && pj->fx.reverb[1] == 0 && pj->fx.amp[1][6] == 3 && pj->fx.send[0][1] == 127 && pj->fx.send[2][3] == 0, "sanitize clamps lists and values, the spare send is 0");
    delete pj;

    // the FX screen: the bottom-left key, its six views, the knobs
    {
        Rig r;
        r.key(Key::Spare);
        ok(r.ui->mode() == Mode::Fx && r.ui->fxView() == FX_DELAY && has(r.toast(), "FX 1/6: DELAY"), "the bottom-left key opens FX on DELAY");
        ok(r.labels(0) == "MODE,TIME,FEEDBACK,TONE" && r.labels(4) == "SPREAD,MOD,HPF,RETURN", "DELAY knobs: " + r.labels(0) + " / " + r.labels(4));
        ok(r.val(0) == "1/8" && r.val(1) == "254 ms" && r.cell(1).dis, "MODE 1/8 at the default 118 BPM sets the time from the tempo; TIME is greyed out (" + r.val(1) + ")");
        r.turn(1, 1);
        ok(has(r.toast(), "TIME FOLLOWS THE TEMPO"), "turning a greyed TIME says why");
        r.turn(0, -5);
        ok(r.val(0) == "FREE" && !r.cell(1).dis, "MODE FREE: TIME counts");
        r.turn(1, 30);
        ok(r.pj->fx.delay[1] == fxDefault(FX_DELAY, 1) + 30, "TIME turns by detents");
        r.turn(2, 10);
        ok(r.pj->fx.delay[2] == fxDefault(FX_DELAY, 2) + 10, "FEEDBACK");
        r.shiftTurn(2, 2);
        ok(r.pj->fx.delay[2] == fxDefault(FX_DELAY, 2) + 10 + 12, "SHIFT turns six times faster");
        r.key(Key::Spare);
        ok(r.ui->fxView() == FX_REVERB && r.labels(0) == "SIZE,DECAY,DAMP,PRE-DLY", "FX again: REVERB");
        r.key(Key::Down);
        ok(r.ui->fxView() == FX_CHORUS, "the list keys move through the views");
        r.key(Key::Down); r.key(Key::Down);
        ok(r.ui->fxView() == FX_AMP && r.labels(0) == "MODEL,DRIVE,BASS,MID", "AMP: " + r.labels(0));
        r.turn(0, 2);
        ok(r.pj->fx.amp[r.ui->part()][0] == 2 && r.val(0) == "CRUNCH", "AMP MODEL CRUNCH on the part on screen");
        r.key(Key::PartK);
        ok(r.val(0) == "OFF", "the other part's amp is its own");
        r.key(Key::Down);
        ok(r.ui->fxView() == FX_SENDS && r.labels(0) == "PART,DELAY,REVERB,CHORUS" && r.cell(4).dis, "SENDS: the part and its three sends, the rest greyed");
        r.turn(2, 50);
        ok(r.pj->fx.send[r.ui->part()][1] == 50 && r.val(2) == "39%", "a send: " + r.val(2));
        r.turn(0, 1);
        ok(r.ui->part() == 2 && r.val(2) == "0%", "T1 picks the part");
        r.key(Key::Down);
        ok(r.ui->fxView() == FX_SENDS, "the last view is the last");
        r.key(Key::Spare);
        ok(r.ui->fxView() == FX_DELAY && r.ui->mode() == Mode::Fx, "the key from the last view wraps to the first");
        r.key(Key::Down); r.key(Key::Down); r.key(Key::Down); r.key(Key::Down); r.key(Key::Down);
        // CLEAR: the view back to defaults, undone by SHIFT + CLEAR
        r.turn(1, 20);
        const int v1 = r.pj->fx.send[2][0];
        r.key(Key::Clear);
        ok(r.pj->fx.send[2][0] == 0 && v1 == 20 && has(r.toast(), "BACK TO DEFAULTS"), "- resets the view (for the part on screen)");
        r.shiftKey(Key::Clear);
        ok(r.pj->fx.send[2][0] == 20, "SHIFT + CLEAR brings it back");
        ok(r.pj->fx.send[0][0] == 0 && r.pj->fx.send[1][0] == 0, "the other parts' sends were not touched");
        // an undo restores only what the reset replaced: edits to other views made since stay
        r.turn(1, 5);                       // SENDS, part C: DELAY 25
        const int sendBefore = r.pj->fx.send[2][0];
        r.key(Key::Clear);
        r.key(Key::Up);                     // to AMP
        r.turn(1, 9);
        const int driveBefore = r.pj->fx.amp[r.ui->part()][1];
        r.key(Key::Down);                   // back to SENDS
        r.shiftKey(Key::Clear);
        ok(r.pj->fx.send[2][0] == sendBefore && r.pj->fx.amp[r.ui->part()][1] == driveBefore, "undo brings back the reset sends and keeps the amp edit made after it");
        r.key(Key::Clear); r.key(Key::Clear);
        ok(has(r.toast(), "ALREADY AT THE DEFAULTS"), "- on a view at its defaults says so");
        // the AMP reset is per part
        r.key(Key::Up);
        const int p0 = r.ui->part();
        r.turn(0, 3);
        ok(r.pj->fx.amp[p0][0] == 3, "amp model set on the part on screen");
        r.key(Key::PartK);
        r.turn(0, 1);
        r.key(Key::Clear);
        ok(r.pj->fx.amp[r.ui->part()][0] == 0 && r.pj->fx.amp[p0][0] == 3, "the AMP reset is for the part on screen");
        r.key(Key::Down);
        // keys that mean something on other screens do nothing here: no lane is changed
        const uint8_t ov = r.lane(0).st[0].ov;
        r.key(Key::Ok); r.key(Key::Add); r.key(Key::Copy); r.key(Key::Paste);
        ok(r.lane(0).st[0].ov == ov && r.ui->mode() == Mode::Fx, "OK, +, copy and paste do nothing on FX");
        r.key(Key::Lane);
        ok(r.ui->mode() == Mode::Play, "LANE goes back");
    }

    // the project file: round trip, and a version 3 file (no effects) loads with the defaults
    {
        Project* a = new Project;
        makeDefaultProject(*a);
        a->fx.send[1][2] = 90; a->fx.comp[0] = 1; a->fx.amp[2][4] = 12; a->fx.delay[2] = 70;
        MemSink sink;
        ok(writeProject(*a, sink, nullptr), "write");
        Project* out = new Project; Project* scratch = new Project;
        MemSource src(sink.bytes);
        ok(readProject(src, *scratch, *out, nullptr, nullptr) && out->fx.send[1][2] == 90 && out->fx.comp[0] == 1 && out->fx.amp[2][4] == 12 && out->fx.delay[2] == 70,
           "the effects are saved and loaded");
        // version 3: the same file without the effects
        ProjectHeader h;
        memcpy(&h, sink.bytes.data(), sizeof(h));
        const std::vector<uint8_t> body = oldPayload(*a, 3);
        std::vector<uint8_t> table(sink.bytes.begin() + sizeof(h) + sizeof(Project), sink.bytes.end());
        h.version = 3;
        h.payloadSize = (uint32_t)body.size();
        h.crc = crc32(table.data(), table.size(), crc32(body.data(), body.size()));
        std::vector<uint8_t> v3((const uint8_t*)&h, (const uint8_t*)&h + sizeof(h));
        v3.insert(v3.end(), body.begin(), body.end());
        v3.insert(v3.end(), table.begin(), table.end());
        MemSource src3(v3);
        ok(readProject(src3, *scratch, *out, nullptr, nullptr) && out->fx.send[1][2] == 0 && out->fx.comp[0] == 0 && out->fx.delay[2] == fxDefault(FX_DELAY, 2) && out->midi[7].ch == 10,
           "a version 3 file loads with the default effects and keeps its MIDI table");
        delete a; delete out; delete scratch;
    }
}

// The effects in the cost model: the meter, the degrade ladder, and the order they are given up in.
static void testFxCost() {
    Project* pj = new Project;
    makeDefaultProject(*pj);
    const float base = voiceTotal(*pj);
    ok(fabsf(fxCostOf(*pj)) < 1e-6f && !fxDesc(*pj).fx, "no effects in use: no cost");
    pj->fx.send[0][1] = 40;                 // part A to the reverb
    pj->fx.send[1][0] = 20;                 // part B to the delay
    pj->fx.comp[0] = 1;
    ok(fabsf(fxCostOf(*pj) - (cost::kCostReverb + cost::kCostDelay + cost::kCostComp)) < 1e-5f, "the shared effects in use are summed: " + std::to_string(fxCostOf(*pj)));
    ok(fabsf(voiceTotal(*pj) - base - fxCostOf(*pj)) < 1e-5f, "the voice total includes them");
    pj->fx.amp[2][0] = 2;                   // a crunch amp on part C
    ok(fabsf(partCost(*pj, 2) - cost::partCost(partDesc(*pj, 2), kFwCost)) < 1e-6f && partDesc(*pj, 2).amp == 2, "a part's cost includes its amp");
    Ladder lad;
    buildLadder(*pj, lad);
    bool seenFx = false, seenAmp = false;
    int firstFx = -1, firstOther = -1;
    for (int i = 0; i < lad.count; ++i) {
        const cost::Step& st = lad.steps[i];
        if (st.part == kParts) { seenFx = true; if (firstFx < 0) firstFx = i; }
        else if (st.action != cost::Action::StealVoice && firstOther < 0) firstOther = i;
        if (st.action == cost::Action::AmpOff || st.action == cost::Action::AmpToClean) seenAmp = true;
    }
    ok(seenFx && seenAmp, "the ladder has steps for the shared effects and for the amp");
    ok(firstFx >= 0 && firstOther >= 0 && firstFx < firstOther, "the effects are given up before any part's own options: " + std::to_string(firstFx) + " < " + std::to_string(firstOther));
    Text t;
    for (int i = 0; i < lad.count; ++i) if (lad.steps[i].part == kParts) { describeStep(t, lad.steps[i]); break; }
    ok(has(t.c_str(), "FX: reverb off"), std::string("the biggest saving first, named: ") + t.c_str());
    // the effects cost goes down as the ladder is walked, to what is left of them
    float prev = effTotal(*pj, lad, 0);
    bool mono = true;
    for (int lv = 1; lv <= lad.count; ++lv) { const float e = effTotal(*pj, lad, lv); mono = mono && e <= prev + 1e-5f; prev = e; }
    ok(mono, "every step lowers the total (or leaves it)");
    ok(effTotal(*pj, lad, lad.count) < voiceTotal(*pj) - 1.0f, "walked to the end the effects are gone and the voices are down");
    const cost::PartDesc fx0 = degradedPart(*pj, kParts, lad, lad.count);
    ok(fx0.fx == 0, "at the bottom of the ladder no shared effect is left");
    delete pj;
}

// The WAVE knob and wavetable banks on the OSC pages.
static void testWaves() {
    WaveSet& ws = waveSet();
    for (bool& b : ws.present) b = false;
    ws.present[4] = ws.present[21] = ws.present[95] = true;
    Rig r;
    r.tap(0);
    r.key(Key::Osc1);
    ok(r.label(0) == "WAVE" && r.label(1) == "PD SHAPE", "OSC 1: WAVE, PD SHAPE");
    const int start = r.pj->parts[r.ui->part()].pages[PG_OSC1][0];
    r.turn(0, 3);
    {
        const int w = r.pj->parts[r.ui->part()].pages[PG_OSC1][0];
        ok(w > start && (w < kBasicWaves || waveSet().present[w - kBasicWaves]), "WAVE turns up, only onto waves the card has: " + std::to_string(start) + " -> " + std::to_string(w));
    }
    r.pj->parts[r.ui->part()].pages[PG_OSC1][0] = 3;
    r.turn(0, 1);
    ok(r.pj->parts[r.ui->part()].pages[PG_OSC1][0] == kBasicWaves + 4 && r.val(0) == "TRI CIRCLE" && !r.cell(0).warn, "past SQR: the first bank the card has: " + r.val(0));
    ok(r.label(1) == "POSITION" && r.label(2) == "PD AMT" && r.cell(2).dis, "on a bank PD SHAPE is POSITION and PD AMT is unused");
    r.turn(0, 1);
    ok(r.val(0) == "ORGAN TIBIA" || r.val(0) == "BEAT PAIR", "the next bank on the card: " + r.val(0));
    r.shiftTurn(0, 1);   // SHIFT: six steps: past the last present bank it stays
    ok(r.pj->parts[r.ui->part()].pages[PG_OSC1][0] == kBasicWaves + 95, "SHIFT + turn goes on and stops at the last bank on the card");
    r.turn(0, -1);
    r.turn(0, -1);
    ok(r.pj->parts[r.ui->part()].pages[PG_OSC1][0] == kBasicWaves + 4, "and back down through the banks");
    r.turn(0, -1);
    ok(r.val(0) == "SQR", "down to the basic waves");
    // a bank that is not on the card is shown and flagged
    r.pj->parts[r.ui->part()].pages[PG_OSC1][0] = kBasicWaves + 77;
    ok(r.cell(0).warn && has(r.val(0), "?"), "a bank whose file is missing is flagged: " + r.val(0));
    // WAVE cannot be p-locked (a table is loaded for a part, not for a step); POSITION can
    r.pj->parts[r.ui->part()].pages[PG_OSC1][0] = kBasicWaves + 4;
    r.key(Key::StepK);
    r.hold(2, 450);
    r.key(Key::Osc1);
    r.turn(0, 1);
    ok(has(r.toast(), "NOT LOCKABLE"), "the WAVE knob is not lockable: " + r.toast());
    r.turn(1, 5);
    ok(r.lane(r.ui->sel()).st[2].hasLock(lockKey(PG_OSC1, 1)), "the table position can be locked per step");
    // the file keeps a bank, and clamps nonsense
    Project* pj = new Project;
    makeDefaultProject(*pj);
    pj->parts[0].pages[PG_OSC1][0] = (int16_t)(kBasicWaves + 150);
    pj->parts[1].pages[PG_OSC2][0] = 9999;
    MemSink sink;
    ok(writeProject(*pj, sink, nullptr), "write");
    Project* out = new Project; Project* scratch = new Project;
    MemSource src(sink.bytes);
    ok(readProject(src, *scratch, *out, nullptr, nullptr) && out->parts[0].pages[PG_OSC1][0] == kBasicWaves + 150 && out->parts[1].pages[PG_OSC2][0] == kWaveMax, "a bank is saved in the project; a value out of range is clamped");
    // an old file (version 4: the preset one signed byte, junk in the padding after it) loads its presets right
    {
        Project* v = new Project;
        makeDefaultProject(*v);
        v->parts[0].preset = -1; v->parts[1].preset = 3; v->parts[2].preset = 9;
        MemSink sink;
        ok(writeProject(*v, sink, nullptr), "write");
        ProjectHeader h;
        memcpy(&h, sink.bytes.data(), sizeof(h));
        std::vector<uint8_t> body = oldPayload(*v, 4);
        std::vector<uint8_t> table(sink.bytes.begin() + sizeof(h) + sizeof(Project), sink.bytes.end());
        // the old layout: byte 4 of a part is the preset (int8), bytes 5-7 the padding: junk there; the part is `offsetof(parts)` + n * sizeof(Part)
        for (int i = 0; i < 3; ++i) {
            uint8_t* part = body.data() + offsetof(Project, parts) + (size_t)i * sizeof(Part);
            const int8_t old = (int8_t)v->parts[i].preset;
            part[offsetof(Part, preset)] = (uint8_t)old;
            part[offsetof(Part, preset) + 1] = 0x5A;    // junk
            part[offsetof(Part, preset) + 2] = 0xA5;
            part[offsetof(Part, preset) + 3] = 0x33;
        }
        h.version = 4;
        h.payloadSize = (uint32_t)body.size();
        h.crc = crc32(table.data(), table.size(), crc32(body.data(), body.size()));
        std::vector<uint8_t> file((const uint8_t*)&h, (const uint8_t*)&h + sizeof(h));
        file.insert(file.end(), body.begin(), body.end());
        file.insert(file.end(), table.begin(), table.end());
        Project* out = new Project; Project* scratch = new Project;
        MemSource src(file);
        ok(readProject(src, *scratch, *out, nullptr, nullptr) && out->parts[0].preset == -1 && out->parts[1].preset == 3 && out->parts[2].preset == 9,
           "a version 4 file: the one-byte presets come back as they were (-1 stays -1, junk padding ignored): " + std::to_string(out->parts[0].preset) + " " + std::to_string(out->parts[1].preset) + " " + std::to_string(out->parts[2].preset));
        // the same bytes as a version 5 file would read 16 bits: a preset above 127 survives
        v->parts[1].preset = 140;
        MemSink s5;
        ok(writeProject(*v, s5, nullptr), "write v5");
        MemSource src5(s5.bytes);
        ok(readProject(src5, *scratch, *out, nullptr, nullptr) && out->parts[1].preset == 140, "a version 5 file keeps a preset number above 127");
        v->parts[1].preset = 32500;
        MemSink s6;
        writeProject(*v, s6, nullptr);
        MemSource src6(s6.bytes);
        ok(readProject(src6, *scratch, *out, nullptr, nullptr) && out->parts[1].preset == 32000, "and an id above the largest possible is clamped to it");
        delete v; delete out; delete scratch;
    }
    // the cost model: a table oscillator costs less than a phase-distortion one
    Project* q = new Project;
    makeDefaultProject(*q);
    const float before = cost::voiceCost(descOf(*q, 0), kFwCost);
    q->parts[0].pages[PG_OSC1][0] = (int16_t)(kBasicWaves + 4);
    const float after = cost::voiceCost(descOf(*q, 0), kFwCost);
#if !DBOY_COST_LEAN
    ok(after < before - 0.01f, "a bank costs less than the PD oscillator: " + std::to_string(before) + " -> " + std::to_string(after));
#else
    ok(after >= before && after < before + 0.2f, "a bank costs about what a basic wave does");
#endif
    delete pj; delete out; delete scratch; delete q;
    (void)start;
    for (bool& b : ws.present) b = false;
}


// A project with more parts than the three it used to have.
static void testParts() {
    {   // the panel reaches every part
        Rig r;
        std::string seen;
        for (int i = 0; i < kParts; ++i) { seen += kPartName[r.ui->part()]; r.key(Key::PartK); }
        ok(seen == "ABCDEFG" && r.ui->part() == 0, "PART goes through A to G and round again: " + seen);
        r.key(Key::Mixer);
        ok(r.label(0) == "LEVEL A" && r.label(6) == "LEVEL G" && r.label(7) == "MASTER", "MIXER view 1: a level for each part and the master");
        r.turn(6, -3);
        ok(r.pj->parts[6].level == 97, "the seventh knob is part G's level");
        r.home();
    }
    {   // a lane on each synth part: a part on the lane's own sends and amp
        Rig r;
        for (int i = 0; i < 6; ++i) r.shiftTurn(2, 1);
        ok(r.pj->patterns[0].lanes[0].part == 6, "PLAYED BY reaches part G: " + std::to_string(r.pj->patterns[0].lanes[0].part));
        ok(r.ui->part() == 6, "and the screen follows the lane to it");
        r.key(Key::Spare);
        while (r.ui->fxView() != FX_AMP) r.key(Key::Spare);
        r.turn(0, 2);
        ok(r.pj->fx.amp[6][0] == 2 && r.pj->fx.amp[2][0] == 0, "part G has its own amp");
        while (r.ui->fxView() != FX_SENDS) r.key(Key::Spare);
        r.turn(2, 50);   // (T1 picks the part)
        ok(r.pj->fx.send[6][1] > 0 && r.pj->fx.send[2][1] == 0, "and its own sends");
        r.home();
    }
    {   // the budget and the ladder count the parts that play
        Project* p = new Project;
        makeDefaultProject(*p);
        ok(p->partsIdle == 0x78, "a new project's parts D to G have no lanes: " + std::to_string(p->partsIdle));
        const float before = voiceTotal(*p);
        p->patterns[9].lanes[0].part = 4;
        refreshIdleParts(*p);
        ok(isIdlePart(*p, 3) && !isIdlePart(*p, 4), "a lane in any pattern makes a part count");
        ok(voiceTotal(*p) > before, "and the budget charges it");
        Ladder l;
        p->parts[3].voices = 5;
        p->fx.amp[3][0] = 3;
        buildLadder(*p, l);
        bool onIdle = false;
        for (int i = 0; i < l.count; ++i) onIdle = onIdle || l.steps[i].part == 3;
        ok(!onIdle, "the ladder has nothing to give up on a part that plays nothing");
        delete p;
    }
    {   // a part with no lanes that is playing counts, and its sends count only then
        Project* p = new Project;
        makeDefaultProject(*p);
        p->parts[3].voices = 5;
        p->fx.send[3][1] = 90;
        ok(!(fxMaskOf(p->fx, p->partsIdle) & cost::kFxReverb) && !(fxDesc(*p).fx & cost::kFxReverb), "a send on a part nothing plays costs nothing");
        const float before = voiceTotal(*p);
        refreshIdleParts(*p, 1u << 3);
        ok(!isIdlePart(*p, 3) && voiceTotal(*p) > before, "a part played from MIDI goes into the budget while it sounds");
        Ladder l;
        buildLadder(*p, l);
        bool onD = false;
        for (int i = 0; i < l.count; ++i) onD = onD || l.steps[i].part == 3;
        ok(onD, "and the ladder can take from it");
        ok(fxDesc(*p).fx & cost::kFxReverb, "and its sends count");
        delete p;
    }
    {   // notes played live on such a part keep it in: in and out with each note would change the ladder under the other parts
        Rig r;
        auto after = [&](uint32_t ms) { r.sv.now += ms; r.ui->tick(); };
        after(100);
        ok(isIdlePart(*r.pj, 3), "a part with no lanes and no sound is idle");
        r.sv.active[3] = 1;
        after(100);
        ok(!isIdlePart(*r.pj, 3), "a note on it brings it in");
        r.sv.active[3] = 0;
        after(100);
        ok(!isIdlePart(*r.pj, 3), "and it stays in when the note ends");
        after(5000);
        ok(!isIdlePart(*r.pj, 3), "through a gap between notes");
        after(6000);
        ok(isIdlePart(*r.pj, 3), "and goes when the part has been quiet for ten seconds");
    }
    {   // the MIXER's ladder follows a lane moved to a part nothing played
        Rig r;
        r.pj->parts[3].voices = 3;
        const int before = r.ui->ladder().count;
        r.pj->patterns[0].lanes[3].part = 3;
        refreshIdleParts(*r.pj);
        ok(r.ui->ladder().count != before, "the MIXER's ladder is rebuilt when a part starts to play: " + std::to_string(before) + " -> " + std::to_string(r.ui->ladder().count));
    }
    {   // a sampler part no lane plays has no sample to edit: another part's lane stays as it is
        Rig r;
        r.ui->layerDown(7); r.ui->layerUp(7);
        ok(r.ui->part() == 2, "lane 8 is part C");
        r.key(Key::PartK);
        ok(r.ui->part() == 3 && !r.ui->partHasLane(3), "D has no lane");
        r.pj->parts[3].type = PT_SAMPLER;
        const int s0 = r.pj->patterns[0].lanes[7].sample();
        r.key(Key::File);
        r.key(Key::Down);
        r.key(Key::Ok);
        ok(r.pj->patterns[0].lanes[7].sample() == s0, "OK in the list does not change lane 8's sample");
        ok(has(r.toast(), "NO LANE PLAYS THIS PART"), "it says why: " + r.toast());
        r.home();
        r.key(Key::Osc1);
        ok(r.ui->mode() != Mode::Sound && has(r.toast(), "NO LANE PLAYS THIS PART"), "and neither do the sampler's sound pages open");
        r.key(Key::Glob);
        ok(r.ui->mode() == Mode::Sound && r.ui->page() == PG_GLOB, "GLOBAL is the part's own, and opens");
        r.home();
    }
    {   // every part at its dearest: the ladder holds all of it
        Project* p = new Project;
        makeDefaultProject(*p);
        for (Lane& ln : p->patterns[0].lanes) ln.part = (uint8_t)(&ln - p->patterns[0].lanes) % kParts;
        refreshIdleParts(*p);
        for (int i = 0; i < kParts; ++i) {
            Part& q = p->parts[i];
            if (q.type == PT_SAMPLER) continue;
            q.voices = 5; q.pages[PG_OSC2][6] = 100; q.pages[PG_SUB][0] = 100; q.pages[PG_FLT1][0] = 7; q.pages[PG_SUB][4] = 4; q.pages[PG_SUB][5] = 30;
            q.pages[PG_FLT1][3] = 100; q.pages[PG_FLT2][0] = 2; p->fx.amp[i][0] = 3;
        }
        p->fx.send[0][1] = 80; p->fx.send[0][0] = 80; p->fx.send[0][2] = 80; p->fx.comp[0] = 1;
        Ladder l;
        buildLadder(*p, l);
        ok(l.count < cost::kMaxSteps, "the ladder of seven parts at their dearest fits: " + std::to_string(l.count));
        bool all = true;
        for (int i = 0; i < kParts; ++i) all = all && degradedPart(*p, i, l, l.count).voices == 1;
        ok(all, "and walking all of it leaves one voice on each part");
        delete p;
    }
    {   // a version 5 file: three parts ahead of the patterns, the effects for three
        Project* v = new Project;
        makeDefaultProject(*v);
        v->parts[1].voices = 4; v->parts[2].level = 77;
        v->patterns[3].lanes[2].root = 41; v->songs[7].nRows = 3; v->midi[9].note = 50;
        v->fx.delay[2] = 60; v->fx.comp[7] = 9; v->fx.amp[2][4] = 12; v->fx.send[1][1] = 90; v->fx.send[2][2] = 33;
        const std::vector<uint8_t> body = oldPayload(*v, 5);
        ProjectHeader h;
        makeHeader(*v, h);
        h.version = 5;
        h.payloadSize = (uint32_t)body.size();
        const uint8_t empty[2] = {0, 0};
        h.crc = crc32(empty, 2, crc32(body.data(), body.size()));
        std::vector<uint8_t> file((const uint8_t*)&h, (const uint8_t*)&h + sizeof(h));
        file.insert(file.end(), body.begin(), body.end());
        file.insert(file.end(), empty, empty + 2);
        Project* out = new Project; Project* scratch = new Project;
        MemSource src(file);
        ok(readProject(src, *scratch, *out, nullptr, nullptr), "a version 5 file loads");
        ok(out->parts[1].voices == 4 && out->parts[2].level == 77 && out->parts[2].type == PT_SAMPLER, "its three parts are as they were");
        ok(out->patterns[3].lanes[2].root == 41 && out->songs[7].nRows == 3 && out->midi[9].note == 50, "and its patterns, songs and MIDI table");
        ok(out->fx.delay[2] == 60 && out->fx.comp[7] == 9 && out->fx.amp[2][4] == 12 && out->fx.send[1][1] == 90 && out->fx.send[2][2] == 33, "and its effects, amps and sends");
        ok(out->parts[6].type == PT_CATHODE && out->parts[6].voices == 2 && out->parts[6].level == 100 && out->fx.amp[6][0] == 0 && out->fx.send[6][1] == 0 && out->fx.amp[3][1] == fxDefault(FX_AMP, 1),
           "the new parts are new: synths with their effects off");
        ok(out->partsIdle == 0x78, "and no lane plays them");
        {   // damaged: a lane past the three parts such a file had. It stays on the last of them, as the older reader had it
            std::vector<uint8_t> bad = oldPayload(*v, 5);
            bad[offsetof(Project, parts) + 3 * sizeof(Part)] = 6;   // pattern 0, lane 0: its part
            ProjectHeader hb = h;
            hb.crc = crc32(empty, 2, crc32(bad.data(), bad.size()));
            std::vector<uint8_t> f2((const uint8_t*)&hb, (const uint8_t*)&hb + sizeof(hb));
            f2.insert(f2.end(), bad.begin(), bad.end());
            f2.insert(f2.end(), empty, empty + 2);
            MemSource s2(f2);
            ok(readProject(s2, *scratch, *out, nullptr, nullptr) && out->patterns[0].lanes[0].part == 2, "a lane past part C in an old file lands on C");
        }
        delete v; delete out; delete scratch;
    }
    {   // a file of this version round trips the last part
        Project* v = new Project;
        makeDefaultProject(*v);
        v->parts[6].preset = 321; v->parts[6].level = 55; v->fx.amp[6][0] = 4; v->fx.send[6][2] = 44;
        v->patterns[0].lanes[0].part = 6;
        MemSink sink;
        ok(writeProject(*v, sink, nullptr), "write seven parts");
        Project* out = new Project; Project* scratch = new Project;
        MemSource src(sink.bytes);
        ok(readProject(src, *scratch, *out, nullptr, nullptr) && out->parts[6].level == 55 && out->fx.amp[6][0] == 4 && out->fx.send[6][2] == 44 && out->patterns[0].lanes[0].part == 6,
           "part G comes back from the card");
        delete v; delete out; delete scratch;
    }
}

// Cutting a loop, and playing the slices.
static std::vector<int16_t> burstLoop(const std::vector<uint32_t>& at, uint32_t frames, double level = 0.8) {
    std::vector<int16_t> d(frames, 0);
    uint32_t x = 1234567;
    for (uint32_t pos : at)
        for (uint32_t i = 0; i < 6000 && pos + i < frames; ++i) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            const double env = exp(-(double)i / 900.0) * level;
            d[pos + i] = (int16_t)(env * 30000.0 * ((double)(x >> 16) / 32768.0 - 1.0));
        }
    return d;
}

static void setSlices(Project& pj, int sample, const std::vector<uint32_t>& at, uint32_t frames) {
    SliceMap* m = sliceMapFor(pj, sample);
    memset(m, 0, sizeof(*m));
    m->sample = (int16_t)sample;
    m->frames = frames;
    for (uint32_t s : at) { Slice& q = m->s[m->count++]; q.start = s; q.level = 127; }
}

static void testSlices() {
    const uint32_t rate = 44100, frames = rate * 2;
    {   // transients
        std::vector<uint32_t> truth;
        for (int i = 0; i < 8; ++i) truth.push_back((uint32_t)(i * 11025 + (i % 3) * 500));
        const std::vector<int16_t> d = burstLoop(truth, frames);
        SliceMap m;
        m.sample = 5;
        const int n = detectSlices(d.data(), frames, rate, 64, 0, m);
        ok(n == 8 && m.frames == frames && m.sample == 5, "eight attacks are eight slices: " + std::to_string(n));
        bool near = n == 8;
        for (int i = 0; near && i < 8; ++i) near = fabs((double)m.s[i].start - truth[i]) < rate * 0.006;
        ok(near, "each slice starts within 6 ms of its attack");
        bool ordered = true;
        for (int i = 1; i < n; ++i) ordered = ordered && m.s[i].start > m.s[i - 1].start;
        ok(ordered && m.s[0].start == 0, "in order, from the start of the sample");
        ok(m.s[0].level == 127 && m.s[0].pitch == 0 && m.s[0].flags == 0, "and each as it was recorded");
        // a soft hit between loud ones: SENS decides
        std::vector<int16_t> mix = burstLoop({0, 44100}, frames);
        const std::vector<int16_t> soft = burstLoop({22050}, frames, 0.0025);
        for (uint32_t i = 0; i < frames; ++i) mix[i] = (int16_t)(mix[i] + soft[i]);
        for (uint32_t i = 3000; i < frames; ++i) mix[i] = (int16_t)(mix[i] + ((i * 7919) % 41 - 20));   // a little noise floor
        SliceMap hi, lo;
        const int nHi = detectSlices(mix.data(), frames, rate, 127, 0, hi), nLo = detectSlices(mix.data(), frames, rate, 0, 0, lo);
        ok(nHi > nLo, "a higher SENS finds more: " + std::to_string(nHi) + " against " + std::to_string(nLo));
        const std::vector<int16_t> silent(frames, 0);
        ok(detectSlices(silent.data(), frames, rate, 127, 0, m) == 1, "silence is one slice");
        ok(detectSlices(nullptr, frames, rate, 64, 0, m) == 1 && detectSlices(d.data(), 10, rate, 64, 0, m) == 1, "no sample, or a tiny one, is one slice");
        ok(detectSlices(d.data(), frames, rate, 64, 16, m) == 16 && fabs((double)m.s[8].start - frames / 2) < rate * 0.003, "GRID 16: sixteen equal parts");
        std::vector<uint32_t> many;
        for (int i = 0; i < 40; ++i) many.push_back((uint32_t)i * 2100);
        const std::vector<int16_t> busy = burstLoop(many, frames);
        const int nb = detectSlices(busy.data(), frames, rate, 100, 0, m);
        ok(nb == kSlices, "more attacks than the table holds: the table is full: " + std::to_string(nb));
        ok(guessBeats(rate * 4, rate, 120) == 3, "four seconds at 120 is 8 beats");
        ok(guessBeats(rate * 2, rate, 174) == 3 || guessBeats(rate * 2, rate, 174) == 2, "two seconds near 174: 4 or 8 beats");
        SliceMap e;
        memset(&e, 0, sizeof(e));
        e.frames = 1000; e.count = 2; e.s[1].start = 500; e.s[1].pitch = 3; e.s[1].level = 127;
        ok(splitSlice(e, 1, nullptr) && e.count == 3 && e.s[2].start == 750 && e.s[2].pitch == 3, "split halves a slice, both keep its settings");
        ok(mergeSlice(e, 2) && e.count == 2 && e.s[1].start == 500, "merge joins it to the one before");
        ok(!mergeSlice(e, 0), "the first has none before it");
        e.s[1].start = 990;
        ok(!splitSlice(e, 1, nullptr), "too short to split");
    }
    auto arm = [](Rig& r, int laneIdx, int mode, std::vector<uint32_t> at, uint32_t len) {
        for (Lane& l : r.pj->patterns[0].lanes) l.mute = 1;
        Lane& l = r.lane(laneIdx);
        l.mute = 0; l.eu = 0; l.n = 16; l.arp.mode = 0;
        for (Step& s : l.st) { s.ov = 0; s.chord = 0; s.prob = 100; s.nLocks = 0; }
        l.smpParam(PG_SLC, SLC_MODE) = (int16_t)mode;
        setSlices(*r.pj, l.sample(), at, len);
        r.pj->swing = 0;
        r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0;
    };
    const double stepF = [] { return 44100.0 * 60.0 / 120.0 / 4.0; }();
    {   // STEP: a step plays the slice of its place, or the one it locks
        Rig r;
        r.pj->bpm = 120;
        arm(r, 7, SLICE_STEP, {0, 1000, 2000, 3000}, 44100);
        Lane& l = r.lane(7);
        for (int i : {0, 1, 2, 5}) l.st[i].ov = 1;
        l.st[2].setLock(lockKey(PG_SLC, SLC_SLICE), 4);
        r.run((int)(stepF * 6.5 / 32));
        std::string got;
        for (auto& o : r.rec.ons) got += std::to_string(o.slice) + " ";
        ok(got == "0 1 3 1 ", "steps play their place in the loop, a lock picks another: " + got);
        l.smpParam(PG_SLC, SLC_RANDOM) = 127;
        for (int i = 0; i < 16; ++i) l.st[i].ov = 1;
        l.st[2].removeLock(lockKey(PG_SLC, SLC_SLICE));
        r.rec.ons.clear();
        r.run((int)(stepF * 64 / 32));
        bool seen[4] = {};
        for (auto& o : r.rec.ons) if (o.slice >= 0 && o.slice < 4) seen[o.slice] = true;
        ok(seen[0] && seen[1] && seen[2] && seen[3] && r.rec.ons.size() > 40, "RANDOM reaches every slice");
        l.smpParam(PG_SLC, SLC_RANDOM) = 0;
        l.smpParam(PG_SLC, SLC_REVRND) = 127;
        r.rec.ons.clear();
        r.run((int)(stepF * 8 / 32));
        bool allRev = !r.rec.ons.empty();
        for (auto& o : r.rec.ons) allRev = allRev && o.sliceRev == 1;
        ok(allRev, "RND REV at the top turns every hit round");
        l.smpParam(PG_SLC, SLC_REVRND) = 0;
        SliceMap* m = sliceMapFor(*r.pj, l.sample());
        m->s[1].flags = SLICE_MUTED;
        r.rec.ons.clear();
        r.run((int)(stepF * 8 / 32));
        bool none = !r.rec.ons.empty();
        for (auto& o : r.rec.ons) none = none && o.slice != 1;
        ok(none, "a muted slice does not play");
    }
    {   // a lane that does not slice is as it was
        Rig r;
        arm(r, 7, SLICE_OFF, {0, 1000}, 44100);
        r.lane(7).st[0].ov = 1;
        r.run(200);
        ok(!r.rec.ons.empty() && r.rec.ons[0].slice == -1, "MODE OFF plays the sample as ever");
    }
    {   // SYNC: every slice at its own time, whatever the lane's rhythm and length
        Rig r;
        r.pj->bpm = 120;   // 4 beats at 120 are 2 s
        const uint32_t len = 88200;
        const std::vector<uint32_t> at = {0, 7000, 22050, 30000, 44100, 66150};
        arm(r, 7, SLICE_SYNC, at, len);
        Lane& l = r.lane(7);
        l.n = 5;
        l.smpParam(PG_SLC, SLC_BEATS) = 2;
        const uint64_t t0 = r.seq->sampleClock();
        r.run((int)(88200.0 * 1.25 / 32));
        std::vector<int> order;
        std::vector<double> times;
        for (auto& o : r.rec.ons) if (o.lane == 7) { order.push_back(o.slice); times.push_back((double)(o.at - t0)); }
        ok(order.size() >= 7 && order[0] == 0 && order[5] == 5 && order[6] == 0, "the slices play through in order and come round: " + std::to_string(order.size()));
        bool timed = order.size() >= 7;
        for (size_t i = 0; timed && i < 6; ++i) timed = fabs(times[i] - at[i]) <= 1.0;
        ok(timed, "each falls where it falls in the loop, inside its step");
        ok(timed && fabs(times[6] - 88200.0) <= 1.0, "and the loop comes round on the beat");
    }
    {   // at another tempo the slices keep their place in the bar
        Rig r;
        r.pj->bpm = 90;
        const std::vector<uint32_t> at = {0, 7000, 22050};
        arm(r, 7, SLICE_SYNC, at, 88200);
        r.lane(7).smpParam(PG_SLC, SLC_BEATS) = 2;
        const uint64_t t0 = r.seq->sampleClock();
        r.run((int)(88200.0 * 1.2 / 32));
        const double scale = 120.0 / 90.0;
        bool timed = r.rec.ons.size() >= 3;
        for (size_t i = 0; timed && i < 3; ++i) timed = fabs((double)(r.rec.ons[i].at - t0) - at[i] * scale) <= 1.5;
        ok(timed, "at 90 BPM the loop stretches over the same four beats");
    }
    {   // swing: a slice just before its beat stays with the beat
        Rig r;
        r.pj->bpm = 120;
        std::vector<uint32_t> at;
        for (int i = 0; i < 8; ++i) at.push_back((uint32_t)(i * stepF) - (i ? 40u : 0u));   // cut at a zero crossing a little early, as GRID does
        arm(r, 7, SLICE_SYNC, at, 88200);
        Lane& l = r.lane(7);
        l.smpParam(PG_SLC, SLC_BEATS) = 2;
        r.pj->swing = 30;
        l.st[4].prob = 0;
        const uint64_t t0 = r.seq->sampleClock();
        r.run((int)(stepF * 6.9 / 32));
        std::vector<int> order;
        std::vector<double> times;
        for (auto& o : r.rec.ons) { order.push_back(o.slice); times.push_back((double)(o.at - t0)); }
        ok(order == std::vector<int>({0, 1, 2, 3, 5, 6}), "PROB on step 5 drops the slice of step 5, not its neighbour");
        // each is 40 frames before its beat, stretched or squeezed with the step it falls in
        ok(times.size() == 6 && fabs(times[1] - (stepF * 1.3 - 40 * 1.3)) < 2.0, "the odd steps swing, and a slice keeps its own time");
        ok(times.size() == 6 && fabs(times[2] - (stepF * 2 - 40 * 0.7)) < 2.0 && fabs(times[5] - (stepF * 6 - 40 * 0.7)) < 2.0, "the even ones do not");
    }
    {   // slices either side of a swung step stay in order
        Rig r;
        r.pj->bpm = 120;
        const double sf = stepF;
        arm(r, 7, SLICE_SYNC, {0, (uint32_t)(1.9 * sf), (uint32_t)(2.0 * sf), (uint32_t)(3.0 * sf)}, (uint32_t)(sf * 8));
        r.lane(7).smpParam(PG_SLC, SLC_BEATS) = 1;
        r.pj->swing = 50;
        r.run((int)(sf * 4.0 / 32));
        bool ordered = r.rec.ons.size() >= 3;
        for (size_t i = 1; i < r.rec.ons.size(); ++i) ordered = ordered && r.rec.ons[i].at >= r.rec.ons[i - 1].at && r.rec.ons[i].slice > r.rec.ons[i - 1].slice;
        ok(ordered, "swing does not swap slices");
    }
    {   // AUTO on a step is no lock at all; lanes past the loop's length still count
        Rig r;
        r.pj->bpm = 120;
        std::vector<uint32_t> at;
        for (int i = 0; i < 8; ++i) at.push_back((uint32_t)i * 11025);
        arm(r, 7, SLICE_SYNC, at, 88200);
        Lane& l = r.lane(7);
        l.smpParam(PG_SLC, SLC_BEATS) = 1;   // 8 steps
        l.st[2].setLock(lockKey(PG_SLC, SLC_SLICE), 0);
        l.st[11].prob = 0;
        r.run((int)(stepF * 12.5 / 32));
        std::string got;
        for (auto& o : r.rec.ons) got += std::to_string(o.slice) + " ";
        ok(got == "0 1 2 3 4 5 6 7 0 1 2 " + std::string("4 "), "a lock of AUTO leaves the slice, and PROB on lane step 12 drops it: " + got);
    }
    {   // a step in SYNC: a lock swaps the slices in it, its PROB can drop them, and its other locks reach them
        Rig r;
        r.pj->bpm = 120;
        std::vector<uint32_t> at;
        for (int i = 0; i < 16; ++i) at.push_back((uint32_t)i * 5512 + 200);   // one a step, a little late
        at[0] = 0;
        arm(r, 7, SLICE_SYNC, at, 88200);
        Lane& l = r.lane(7);
        l.smpParam(PG_SLC, SLC_BEATS) = 2;
        l.st[2].setLock(lockKey(PG_SLC, SLC_SLICE), 10);
        l.st[3].prob = 0;
        l.st[4].setLock(lockKey(PG_SFLT, 1), 40);
        r.run((int)(stepF * 7 / 32));
        std::string got;
        for (auto& o : r.rec.ons) got += std::to_string(o.slice) + " ";
        ok(got == "0 1 9 4 5 6 " || got == "0 1 9 4 5 6 7 ", "a locked step plays its slice, PROB 0 drops one: " + got);
        bool carried = false;
        for (auto& o : r.rec.ons) if (o.slice == 4) carried = o.nLocks == 1 && o.locks[0].key == lockKey(PG_SFLT, 1);
        ok(carried, "and a step's other locks go with its slices");
    }
    {   // the panel
        Rig r;
        r.tap(7);
        r.key(Key::Osc2);
        ok(r.ui->mode() == Mode::Sound && r.ui->page() == PG_SLC, "OSC 2 on a sampler lane opens SLICE");
        ok(r.label(0) == "MODE" && r.label(7) == "GRID", "with its eight knobs");
        r.turn(7, 3);   // GRID 16
        r.turn(0, 2);   // SYNC
        r.sv.now += 200; r.ui->tick();
        const int smp = r.lane(7).sample();
        const SliceMap* m = findSlices(*r.pj, smp);
        ok(m && m->count == 16 && r.lane(7).smpParam(PG_SLC, SLC_MODE) == SLICE_SYNC, "turning MODE on cuts the sample: " + std::to_string(m ? m->count : -1));
        r.key(Key::Osc2);
        ok(r.ui->page() == PG_SLC && r.ui->sliceEdit() && r.label(0) == "SLICE" && r.label(7) == "MERGE", "the key again: one slice at a time");
        r.turn(0, 3);
        ok(r.ui->sliceSel() == 3 && r.val(0) == "4/16" && r.sv.sliceAuditions.back() == 3, "SLICE walks through them, and plays each");
        const uint32_t s3 = m->s[3].start;
        r.turn(1, 2);
        ok(m->s[3].start > s3 && m->s[3].start < m->s[4].start, "START moves it, between its neighbours");
        r.turn(2, 5); r.turn(3, -27); r.turn(4, 1); r.turn(5, -1);
        ok(m->s[3].pitch == 5 && m->s[3].level == 100 && (m->s[3].flags & SLICE_REVERSE) && (m->s[3].flags & SLICE_MUTED), "pitch, level, reverse and mute are the slice's own");
        r.turn(6, 1);
        ok(m->count == 17 && m->s[4].pitch == 5, "SPLIT makes two, both as it was");
        r.turn(7, 1);
        ok(m->count == 16 && r.ui->sliceSel() == 2, "MERGE joins it to the one before, which is selected now");
        r.shiftKey(Key::Clear);
        ok(m->count == 16 && m->s[3].start == s3 && m->s[3].pitch == 0, "SHIFT + CLEAR undoes the run of edits");
        r.key(Key::Osc2);
        ok(r.ui->mode() != Mode::Sound || r.ui->page() != PG_SLC, "and the key a third time leaves");
        r.home();
        // a step's slice
        r.key(Key::StepK);
        r.tap(7);
        r.key(Key::StepK);
        ok(r.ui->stepSub() == 1 && r.label(1) == "STEP SLICE" && r.val(1) == "AUTO", "the step's second view has its slice");
        r.turn(1, 3);
        const Step& st = r.lane(7).st[r.ui->cursor()];
        ok(st.lockValue(lockKey(PG_SLC, SLC_SLICE), 0) == 3 && r.val(1) == "3", "a lock on the step");
        r.turn(1, -3);
        ok(!st.hasLock(lockKey(PG_SLC, SLC_SLICE)), "AUTO takes the lock away");
        r.home();
    }
    {   // undo keeps what was there
        Rig r;
        r.tap(7);
        r.key(Key::Osc2);
        r.turn(7, 1);
        r.turn(0, 1);
        const int smp = r.lane(7).sample();
        const SliceMap* m = findSlices(*r.pj, smp);
        ok(m && m->count == 4, "GRID 4 and STEP cut four");
        r.key(Key::Osc2);
        r.turn(2, 5);
        r.key(Key::Osc2);        // leaves SLICE EDIT
        r.key(Key::Osc2);        // and back on the page
        r.turn(7, 1);            // GRID 8
        r.ui->tick();
        ok(m->count == 4, "the cut waits for the knob to stop");
        r.sv.now += 200;
        r.ui->tick();
        ok(m->count == 8 && m->s[0].pitch == 0, "then the sample is cut again");
        r.shiftKey(Key::Clear);
        ok(m->count == 4 && m->s[0].pitch == 5, "undo brings back the hand edits");
        r.home();
    }
    {   // the other lanes: a table for a file of another length is cut again without taking the undo with it
        Rig r;
        r.tap(0);
        Lane& l = r.lane(8);
        l.part = 2;
        l.smpParam(PG_SLC, SLC_MODE) = SLICE_SYNC;
        l.smpParam(PG_SLC, SLC_GRID) = 1;
        setSlices(*r.pj, l.sample(), {0, 10}, 99);
        r.shiftTurn(2, 1);       // something to undo: PLAYED BY moves nothing that matters here
        const std::string before = r.toast();
        r.sv.now += 100;
        r.ui->tick();
        const SliceMap* m = findSlices(*r.pj, l.sample());
        ok(m && m->frames == r.lib->items[l.sample()].length && m->count == 4, "a lane off screen is cut again too");
        r.pj->slices[0].s[1].pitch = 3;
        r.sv.now += 100;
        r.ui->tick();
        ok(r.pj->slices[0].s[1].pitch == 3, "and then left alone");
    }
    {   // the table of maps
        Project* p = new Project;
        makeDefaultProject(*p);
        for (int i = 0; i < kSliceMaps; ++i) setSlices(*p, 100 + i, {0, 10}, 100);
        ok(sliceMapFor(*p, 200) != nullptr && sliceMapFor(*p, 200)->sample >= 100, "a map no lane plays can go");
        for (int i = 0; i < kSliceMaps; ++i) p->patterns[0].lanes[0].st[i].setLock(lockKey(PG_SMP, 0), 0);
        for (int i = 0; i < kSliceMaps; ++i) p->patterns[1 + i / kLanes].lanes[i % kLanes].smp[0][0] = (int16_t)(100 + i);
        ok(sliceMapFor(*p, 200) == nullptr, "and all sixteen in use are safe");
        bool used[kMaxSamples];
        Project* q = new Project;
        makeDefaultProject(*q);
        setSlices(*q, 321, {0, 10}, 100);
        samplesUsed(*q, used, kMaxSamples);
        ok(!used[321], "a cut no lane plays does not hold its sample in memory");
        int16_t map[kMaxSamples];
        for (int i = 0; i < kMaxSamples; ++i) map[i] = -1;
        remapSamples(*q, map, kMaxSamples);
        ok(!findSlices(*q, 321), "and has no meaning on another card");
        delete p; delete q;
        SliceMap e;
        const std::vector<int16_t> low(1984, 0);
        std::vector<int16_t> slow(1984);
        for (int i = 0; i < 1984; ++i) slow[i] = (int16_t)(20000 * sin(i * 0.002));
        ok(detectSlices(slow.data(), 1984, 44100, 64, 32, e) >= 2, "a short slow wave still cuts");
        bool apart = true;
        for (int i = 1; i < e.count; ++i) apart = apart && e.s[i].start > e.s[i - 1].start;
        ok(apart, "and no two slices share a start");
    }
    {   // version 7: its slices stay, and each lane has a LOOP page at its defaults
        Project* w = new Project;
        makeDefaultProject(*w);
        Lane& l = w->patterns[1].lanes[7];
        l.smpParam(PG_SLC, SLC_MODE) = SLICE_STEP;
        setSlices(*w, 9, {0, 40, 90}, 500);
        l.st[3].setLock(lockKey(PG_FXAMP, 2), 33);
        l.st[3].setLock(lockKey(PG_SLC, SLC_SLICE), 2);
        w->patterns[15].lanes[9].root = 44;
        w->songs[7].nRows = 3;
        w->fx.send[6][1] = 91;
        const std::vector<uint8_t> body = oldPayload(*w, 7);
        ProjectHeader h;
        makeHeader(*w, h);
        h.version = 7;
        h.payloadSize = (uint32_t)body.size();
        const uint8_t empty[2] = {0, 0};
        h.crc = crc32(empty, 2, crc32(body.data(), body.size()));
        std::vector<uint8_t> file((const uint8_t*)&h, (const uint8_t*)&h + sizeof(h));
        file.insert(file.end(), body.begin(), body.end());
        file.insert(file.end(), empty, empty + 2);
        Project* out = new Project; Project* scratch = new Project;
        MemSource s7(file);
        ok(readProject(s7, *scratch, *out, nullptr, nullptr), "a version 7 file loads");
        const SliceMap* m = findSlices(*out, 9);
        ok(m && m->count == 3 && m->s[2].start == 90 && out->patterns[1].lanes[7].smpParam(PG_SLC, SLC_MODE) == SLICE_STEP, "with its slices");
        ok(out->fx.send[6][1] == 91 && out->songs[7].nRows == 3 && out->patterns[15].lanes[9].root == 44, "and everything else where it was");
        const Step& s = out->patterns[1].lanes[7].st[3];
        ok(s.lockValue(lockKey(PG_FXAMP, 2), -1) == 33 && s.lockValue(lockKey(PG_SLC, SLC_SLICE), -1) == 2, "an effects lock is still that lock");
        ok(out->patterns[1].lanes[7].smpParam(PG_LOOP, LP_XFADE) == 0 && out->patterns[1].lanes[7].smpParam(PG_LOOP, LP_CURVE) == CURVE_POWER && out->patterns[1].lanes[7].smpParam(PG_LOOP, LP_SEARCH) == 32, "the LOOP page is at its defaults");
        delete w; delete out; delete scratch;
    }
    {   // a project keeps its slices, and an older file comes in without any
        Project* v = new Project;
        makeDefaultProject(*v);
        Lane& l = v->patterns[0].lanes[7];
        l.smpParam(PG_SLC, SLC_MODE) = SLICE_SYNC;
        setSlices(*v, l.sample(), {0, 500, 900}, 2000);
        v->slices[0].s[1].pitch = -7;
        l.st[0].setLock(lockKey(PG_FXSEND, 1), 88);
        l.st[0].setLock(lockKey(PG_SLC, SLC_SLICE), 2);
        MemSink sink;
        ok(writeProject(*v, sink, nullptr), "write slices");
        Project* out = new Project; Project* scratch = new Project;
        MemSource src(sink.bytes);
        ok(readProject(src, *scratch, *out, nullptr, nullptr), "read slices");
        const SliceMap* m = findSlices(*out, l.sample());
        ok(m && m->count == 3 && m->frames == 2000 && m->s[1].start == 500 && m->s[1].pitch == -7 && out->patterns[0].lanes[7].smpParam(PG_SLC, SLC_MODE) == SLICE_SYNC, "slices come back from the card");
        // damaged: out of order, out of range
        v->slices[0].s[1].start = 5000; v->slices[0].count = 40;
        sanitize(*v);
        ok(findSlices(*v, l.sample())->count <= kSlices && findSlices(*v, l.sample())->s[1].start != 5000, "a damaged table is put right");
        // version 6: seven parts, lanes one page shorter, the effects' lock keys one page lower
        Project* w = new Project;
        makeDefaultProject(*w);
        w->parts[5].level = 66; w->fx.send[6][1] = 91; w->midi[9].note = 50; w->songs[7].nRows = 3;
        w->patterns[15].lanes[9].root = 44; w->patterns[0].lanes[0].smp[4][2] = 77;
        w->patterns[3].lanes[2].st[5].setLock(lockKey(PG_FXSEND, 1), 88);
        w->patterns[3].lanes[2].st[5].setLock(lockKey(PG_FLT1, 1), 55);
        const std::vector<uint8_t> body = oldPayload(*w, 6);
        ProjectHeader h;
        makeHeader(*w, h);
        h.version = 6;
        h.payloadSize = (uint32_t)body.size();
        const uint8_t empty[2] = {0, 0};
        h.crc = crc32(empty, 2, crc32(body.data(), body.size()));
        std::vector<uint8_t> file((const uint8_t*)&h, (const uint8_t*)&h + sizeof(h));
        file.insert(file.end(), body.begin(), body.end());
        file.insert(file.end(), empty, empty + 2);
        MemSource s6(file);
        ok(readProject(s6, *scratch, *out, nullptr, nullptr), "a version 6 file loads");
        ok(out->parts[5].level == 66 && out->fx.send[6][1] == 91 && out->midi[9].note == 50 && out->songs[7].nRows == 3 && out->patterns[15].lanes[9].root == 44 && out->patterns[0].lanes[0].smp[4][2] == 77,
           "with everything where it was");
        const Step& s = out->patterns[3].lanes[2].st[5];
        ok(s.lockValue(lockKey(PG_FXSEND, 1), -1) == 88 && s.lockValue(lockKey(PG_FLT1, 1), -1) == 55, "an effects lock is still that lock");
        ok(out->patterns[0].lanes[0].smpParam(PG_SLC, SLC_MODE) == SLICE_OFF && out->patterns[0].lanes[0].smpParam(PG_SLC, SLC_SENS) == 64 && !findSlices(*out, 0), "and no lane slices");
        delete v; delete w; delete out; delete scratch;
    }
}

// Loop points and the panel for them.
static void testLoops() {
    const uint32_t rate = 44100;
    auto joinError = [](const std::vector<int16_t>& d, uint32_t s, uint32_t e) {   // how far the wave jumps at the turn, against its usual step
        double jump = fabs((double)d[s] - d[e - 1]), usual = 0;
        for (uint32_t i = s + 1; i < s + 2000 && i < d.size(); ++i) usual = std::max(usual, fabs((double)d[i] - d[i - 1]));
        return jump / (usual + 1);
    };
    {   // EQ AMP: a held note, with a loop that does not fit its period. Zero crossings that match take it to a whole number of periods
        std::vector<int16_t> d(rate * 2);
        for (uint32_t i = 0; i < d.size(); ++i) d[i] = (int16_t)(12000.0 * sin(2 * M_PI * 196.0 * i / rate) + 6000.0 * sin(2 * M_PI * 394.0 * i / rate + 0.4));
        const uint32_t s0 = 11003, e0 = 70007;
        const double before = joinError(d, s0, e0);
        uint32_t s = 0, e = 0;
        float corr = 0;
        ok(findLoopPoints(d.data(), (uint32_t)d.size(), rate, s0, e0, AUTO_AMP, 32, 0.0, s, e, corr), "EQ AMP finds a loop");
        ok(d[s] >= 0 && d[s - 1] < 0 && d[e] >= 0 && d[e - 1] < 0, "on two rising zero crossings");
        ok(joinError(d, s, e) < 1.0 && before > 2.0, "and the wave carries on through the join: " + std::to_string(before) + " -> " + std::to_string(joinError(d, s, e)));
        ok(fabs((double)s - s0) < rate && fabs((double)e - e0) < rate, "near where it was asked");
        ok(corr > 0.9f, "a held note's two ends are alike: " + std::to_string(corr));
    }
    {   // EQ POWER: noise that swells: the points fall where it is as loud at both ends
        std::vector<int16_t> d(rate * 3);
        uint32_t x = 7;
        for (uint32_t i = 0; i < d.size(); ++i) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            const double t = (double)i / rate;
            const double env = 0.15 + 0.1 * sin(2 * M_PI * 0.5 * t) + 0.07 * sin(2 * M_PI * 3.3 * t);
            d[i] = (int16_t)(env * ((double)(x >> 16) - 32768.0));
        }
        const uint32_t s0 = rate / 2, e0 = rate * 2 + 3000;
        auto level = [&](uint32_t from, uint32_t n) { double a = 0; for (uint32_t i = 0; i < n; ++i) a += (double)d[from + i] * d[from + i]; return 10 * log10(a / n + 1); };
        const uint32_t w = rate / 10;
        uint32_t s = 0, e = 0;
        float corr = 1;
        ok(findLoopPoints(d.data(), (uint32_t)d.size(), rate, s0, e0, AUTO_POWER, 64, w, s, e, corr), "EQ POWER finds a loop");
        const double was = fabs(level(s0 - w, w) - level(e0 - w, w)), now = fabs(level(s - w, w) - level(e - w, w));   // what fades in, against what fades out
        ok(now < 1.0 && now < was, "the two ends are equally loud: " + std::to_string(was) + " -> " + std::to_string(now) + " dB");
        ok(e > s && e - s > (e0 - s0) / 2, "and the loop stays long");
        ok(corr < 0.4f, "noise's two ends are not alike: " + std::to_string(corr));
    }
    {   // EQ POWER on a held note ends in step with the turn, even with no fade at all, and a burst nearby does not draw it away
        std::vector<int16_t> d(rate * 2);
        for (uint32_t i = 0; i < d.size(); ++i) d[i] = (int16_t)(9000.0 * sin(2 * M_PI * 220.0 * i / rate));
        for (uint32_t i = 60400; i < 60900; ++i) d[i] = (int16_t)(d[i] * 3);
        uint32_t s = 0, e = 0;
        float corr = 0;
        ok(findLoopPoints(d.data(), (uint32_t)d.size(), rate, 11003, 60000, AUTO_POWER, 32, 0.0, s, e, corr), "EQ POWER with no fade");
        ok(joinError(d, s, e) < 1.2, "the join is in step: " + std::to_string(joinError(d, s, e)));
        ok(!(e > 60300 && e < 61000), "and it stays off the burst: " + std::to_string(e));
    }
    {   // near the sample's ends a short comparison must not win by being short
        std::vector<int16_t> d(rate * 2);
        for (uint32_t i = 0; i < d.size(); ++i) d[i] = (int16_t)(12000.0 * exp(-(double)i / 40000.0) * sin(2 * M_PI * 110.0 * i / rate));
        uint32_t s = 0, e = 0;
        float corr = 0;
        ok(findLoopPoints(d.data(), (uint32_t)d.size(), rate, 0, (uint32_t)d.size() - 1, AUTO_AMP, 32, 0.0, s, e, corr), "a whole sample loops");
        ok(s >= 256 && e + 256 <= d.size(), "on points it can really compare: " + std::to_string(s) + " " + std::to_string(e));
    }
    {   // nothing to go on
        std::vector<int16_t> quiet(rate, 0);
        uint32_t s = 0, e = 0;
        float c = 0;
        ok(!findLoopPoints(quiet.data(), rate, rate, 100, 30000, AUTO_AMP, 32, 0.0, s, e, c) && s == 100 && e == 30000, "silence leaves EQ AMP's points alone");
        ok(!findLoopPoints(quiet.data(), rate, rate, 100, 30000, AUTO_POWER, 32, 0.0, s, e, c) && s == 100 && e == 30000, "and EQ POWER's");
        ok(!findLoopPoints(nullptr, rate, rate, 100, 30000, AUTO_AMP, 32, 0.0, s, e, c), "and no sample");
        ok(!findLoopPoints(quiet.data(), rate, rate, 500, 100, AUTO_AMP, 32, 0.0, s, e, c), "and points the wrong way round");
    }
    {   // the region
        int16_t sm[8] = {0, 0, 127, 2, 0, 0, 0, 100}, lp[8] = {0, 127, LOOP_PCT, 0, 0, 0, 32, 0};
        LoopRegion r;
        loopRegion(sm, lp, 44100, 44100, r);
        ok(fabs(r.end - 44099) < 1 && fabs(r.fade - r.end * 0.5) < 1, "XFADE at the top in % is half the loop");
        lp[LP_UNIT] = LOOP_MS;
        loopRegion(sm, lp, 441000, 44100, r);
        ok(fabs(r.fade - 88200) < 1, "and in ms two seconds");
        lp[LP_XFADE] = 64;
        loopRegion(sm, lp, 441000, 44100, r);
        ok(r.fade > 44100 * 0.45 && r.fade < 44100 * 0.55, "the knob spreads the short fades: 64 is about half a second");
        lp[LP_XFADE] = 127;
        loopRegion(sm, lp, 44100, 44100, r);
        ok(r.fade <= (r.end - r.start) * 0.5 + 1e-9, "never more than half the loop");
        lp[LP_XFADE] = 0; lp[LP_SFINE] = 63; lp[LP_EFINE] = -63;
        LoopRegion q;
        loopRegion(sm, lp, 127 * 63 * 10, 44100, q);
        ok(fabs(q.start - 630) < 1 && fabs(q.end - (127 * 63 * 10 - 630)) < 1.5, "the fine knobs move the points inside one of START's steps");
        lp[LP_SFINE] = -63;
        loopRegion(sm, lp, 44100, 44100, q);
        ok(q.start == 0, "and stop at the sample's ends");
        loopRegion(sm, nullptr, 44100, 44100, q);
        ok(q.fade == 0, "a sample that does not loop has no fade");
    }
    {   // the panel
        Rig r;
        r.tap(7);
        r.key(Key::Sub);
        ok(r.ui->mode() == Mode::Sound && r.ui->page() == PG_LOOP && r.label(0) == "AUTO" && r.label(1) == "XFADE" && r.label(3) == "CURVE", "SUB on a sampler lane opens LOOP");
        ok(r.val(1) == "OFF", "with no fade");
        r.turn(1, 64);
        ok(has(r.val(1), " ms"), "XFADE reads in ms: " + r.val(1));
        r.turn(2, 1);
        ok(has(r.val(1), "%"), "or in %: " + r.val(1));
        ok(r.cell(1).idle, "dim while the sample does not loop");
        r.lane(7).smpParam(PG_SMP, 3) = 2;
        ok(!r.cell(1).idle, "and lit when it does");
        r.turn(0, 2);
        ok(r.lane(7).smpParam(PG_LOOP, LP_AUTO) == AUTO_POWER && r.lane(7).smpParam(PG_LOOP, LP_CURVE) == CURVE_MATCH, "EQ POWER takes the curve made for it");
        r.turn(3, -1);
        r.turn(0, 1);
        ok(r.lane(7).smpParam(PG_LOOP, LP_CURVE) == CURVE_LOG, "and a knob already at its end leaves your curve alone");
        r.turn(3, -3);
        r.turn(0, -1);
        ok(r.lane(7).smpParam(PG_LOOP, LP_CURVE) == CURVE_MATCH, "and EQ AMP too: both know how alike the ends are");
        const int smp = r.lane(7).sample();
        const int16_t* sm = r.lane(7).smp[lanePageIndex(PG_SMP)];
        const int16_t* lp = r.lane(7).smp[lanePageIndex(PG_LOOP)];
        r.sv.now += 60; r.ui->tick();
        ok(!findLoopFit(*r.pj, smp, sm[1], sm[2], lp[LP_AUTO], lp[LP_SEARCH], loopFadeKey(lp)), "the search waits for the knobs to stop");
        r.sv.now += 300; r.ui->tick();
        const LoopFit* f = findLoopFit(*r.pj, smp, sm[1], sm[2], lp[LP_AUTO], lp[LP_SEARCH], loopFadeKey(lp));
        ok(f && f->end > f->start && f->frames == r.lib->items[smp].length, "then it finds the points");
        uint32_t s = 0, e = 0; double x = 0, lap = 0;
        ok(r.ui->shownLoop(s, e, x, lap, lp[LP_XFADE], smp) && s == f->start && e == f->end, "and the screen shows them");
        ok(!r.ui->shownLoop(s, e, x, lap, lp[LP_XFADE], smp + 1), "but not on a sample a step has locked instead");
        r.turn(6, 4);
        r.sv.now += 300; r.ui->tick();
        ok(findLoopFit(*r.pj, smp, sm[1], sm[2], lp[LP_AUTO], lp[LP_SEARCH], loopFadeKey(lp)), "SEARCH looks again");
        r.home();
        r.tap(7);
        r.key(Key::Sub);
        r.turn(1, 5);
        r.home();
        Project* out = new Project; Project* scratch = new Project;
        MemSink sink;
        ok(writeProject(*r.pj, sink, nullptr), "write loop points");
        MemSource src(sink.bytes);
        ok(readProject(src, *scratch, *out, nullptr, nullptr) && findLoopFit(*out, smp, sm[1], sm[2], lp[LP_AUTO], lp[LP_SEARCH], loopFadeKey(lp)), "they come back from the card");
        delete out; delete scratch;
    }
    {   // the table
        Project* p = new Project;
        makeDefaultProject(*p);
        for (int i = 0; i < kLoopFits; ++i) { LoopFit* f = loopFitFor(*p); f->sample = (int16_t)(200 + i); f->frames = 100; f->start = 1; f->end = 2; }
        ok(loopFitFor(*p) != nullptr, "an entry no lane asks for can go");
        for (int i = 0; i < kLoopFits; ++i) {   // every entry asked for by some lane: a further search must not throw one out, or they would chase each other for ever
            Lane& l = p->patterns[i / kLanes].lanes[i % kLanes];
            l.smpParam(PG_SMP, 0) = (int16_t)(200 + i);
            p->loops[i].wantS = l.smpParam(PG_SMP, 1); p->loops[i].wantE = l.smpParam(PG_SMP, 2);
            p->loops[i].algo = (uint8_t)l.smpParam(PG_LOOP, LP_AUTO); p->loops[i].search = (uint8_t)l.smpParam(PG_LOOP, LP_SEARCH);
            p->loops[i].fadeKey = loopFadeKey(l.smp[lanePageIndex(PG_LOOP)]);
        }
        ok(loopFitFor(*p) == nullptr, "with all of them in use it says so");
        p->loops[3].end = 500;
        sanitize(*p);
        ok(p->loops[3].sample == -1, "a damaged entry is dropped");
        int16_t map[kMaxSamples];
        for (int i = 0; i < kMaxSamples; ++i) map[i] = -1;
        map[200] = 7;
        remapSamples(*p, map, kMaxSamples);
        ok(p->loops[0].sample == 7 && p->loops[1].sample == -1, "numbers follow the card, and one that is gone goes");
        delete p;
    }
}

// The REC screen.
static void testRecord() {
    {   // a web page has no recorder
        Rig r;
        r.key(Key::Rec);
        ok(r.ui->mode() == Mode::Play && has(r.toast(), "ON THE DRUMBOY"), "REC says so where there is no recorder");
    }
    Rig r;
    r.sv.recOn = true;
    r.tap(7);
    r.key(Key::Rec);
    ok(r.ui->mode() == Mode::Record && r.sv.recListening, "REC opens the screen and the input");
    {   // the plain way out, and back: goBack is not setMode
        Rig g;
        g.sv.recOn = true;
        g.key(Key::Rec);
        g.key(Key::Rec);
        ok(g.sv.recListening, "a take runs");
        g.key(Key::Lane);
        ok(g.ui->mode() == Mode::Play && !g.sv.recListening, "the LANE key leaves the REC screen properly: no take runs on out of sight");
        g.key(Key::Rec);
        ok(g.sv.recListening, "REC opens it again");
        g.key(Key::Mixer);
        ok(g.ui->mode() == Mode::Mixer && !g.sv.recListening, "another screen closes the input");
        g.key(Key::Lane);
        ok(g.ui->mode() == Mode::Record && g.sv.recListening, "and back on the REC screen the input is open again");
    }
    ok(r.label(0) == "SOURCE" && r.label(3) == "THRESHOLD" && r.label(6) == "AUTO STOP", "with its knobs");
    r.turn(0, 1);
    ok(r.sv.recLast.source == 1 && r.val(0) == "OUTPUT", "SOURCE reaches the recorder: what the Drumboy plays");
    r.turn(0, -1);
    r.turn(1, 4);
    ok(r.sv.recLast.gain == 4 && r.val(1) == "+6.0 dB", "GAIN in 1.5 dB steps");
    r.turn(2, 1);
    ok(r.ui->recSettings().autoCapture == 1 && r.val(2) == "AUTO", "CAPTURE AUTO");
    r.turn(3, -6);
    ok(r.ui->recSettings().thresholdDb == -36, "THRESHOLD in dB");
    r.turn(3, -100);
    ok(r.ui->recSettings().thresholdDb == -60, "down to -60");
    r.turn(3, 24);
    r.key(Key::Rec);
    ok(r.sv.recNow.state == REC_ARMED && r.sv.recLast.autoCapture == 1 && r.sv.recLast.thresholdDb == -36, "REC arms it with the threshold");
    r.key(Key::Rec);
    ok(r.sv.recNow.state == REC_IDLE, "REC again gives up the wait");
    r.key(Key::Rec);
    r.sv.recNow.state = REC_TAKING;
    r.sv.now += 100; r.ui->tick();
    ok(has(r.toast(), "SOUND: RECORDING"), "it says when the sound came");
    r.key(Key::Rec);
    ok(r.sv.recNow.state == REC_DONE, "REC ends the take");
    r.sv.now += 100; r.ui->tick();
    ok(has(r.toast(), "OK KEEPS"), "and says what is next");
    r.key(Key::Rec);
    ok(r.sv.recNow.state == REC_DONE && has(r.toast(), "REC AGAIN DROPS"), "REC on a finished take asks first: it may have stopped by itself");
    const int was = r.lane(7).sample();
    r.key(Key::Ok);
    ok(r.sv.recKeeps == 1 && r.lane(7).sample() == r.sv.recNext, "OK puts the take on the lane");
    r.shiftKey(Key::Clear);
    ok(r.lane(7).sample() == was, "and SHIFT + CLEAR takes it off");
    r.ui->turn(2, -1);
    r.key(Key::Rec);
    ok(r.sv.recNow.state == REC_TAKING, "CAPTURE MANUAL starts at once");
    r.key(Key::Rec);
    r.key(Key::Clear);
    ok(r.sv.recDrops == 1 && r.sv.recNow.state == REC_IDLE, "- drops a take");
    r.key(Key::Rec);
    r.key(Key::Rec);
    r.key(Key::Rec);   // (the first asks)
    r.key(Key::Rec);
    ok(r.sv.recDrops == 2 && r.sv.recNow.state == REC_TAKING, "REC on a finished take goes again");
    r.home();
    ok(r.sv.recKeeps == 2 && !r.sv.recListening, "walking off keeps what was recorded, and closes the input");
    r.sv.recRoom = false;
    r.key(Key::Rec);
    r.key(Key::Rec);
    ok(has(r.toast(), "NO MEMORY FREE"), "with no memory it says so");
    r.home();
}

// Reaching one sample of a card's 2,500 in two moves: a folder, then a sample in it.
static void testSampleBrowser() {
    struct Card : Rig {
        std::deque<std::string> paths;
        explicit Card(int total = 2500, bool empty = true) { fillCard(*lib, paths, total, empty); sv.lib = lib; tap(7); }
        int cat(const char* name) { for (int c = 0; c < lib->catCount; ++c) if (!strcmp(lib->cats[c], name)) return c; return -1; }
        int nth(const char* folder, int n) { const int c = cat(folder); for (int i = 0; i < lib->count; ++i) if (lib->items[i].cat == c && n-- == 0) return i; return -1; }
        const char* name(int i) { return lib->items[i].name; }
        std::string folderCell(int e) { ui->keyDown(Key::Shift); const std::string v = val(e); ui->keyUp(Key::Shift); return v; }
        void later(uint32_t ms) { sv.now += ms; ui->tick(); }
        void toFolder(int e, const char* folder) {   // the folder knob, as far as that one
            ui->keyDown(Key::Shift);
            ui->turn(e, -1);
            for (int i = 0; i < 64; ++i) ui->turn(e, -1);
            for (int i = 0; i < 64 && val(e) != folder; ++i) ui->turn(e, 1);
            ui->keyUp(Key::Shift);
            later(1000);
        }
    };

    {   // the two moves
        Card r;
        const int start = r.lane(7).sample(), kicks = r.lib->catSize(r.cat("KICK"));
        ok(r.lib->count == 2512 && kicks > 100, "a card-sized list");
        ok(r.label(4) == "KICK" && r.val(4) == "Kick 808" && r.cell(4).tag.size() > 0 && has(r.cell(4).tag.c_str(), "1/"), "B1 starts in the folder of the lane's sample, at its place");
        r.ui->keyDown(Key::Shift);
        ok(r.label(4) == "FOLDER" && r.val(4) == "KICK", "with SHIFT B1 is the folder knob");
        const int auds = (int)r.sv.auditions.size();
        r.ui->turn(4, 1);
        ok(r.val(4) == "SNARE" && r.lane(7).sample() == start, "the folder knob moves the folder: the lane plays on");
        ok(has(r.toast(), "SNARE") && has(r.toast(), "SAMPLES"), "and says where it is");
        ok((int)r.sv.auditions.size() == auds, "no sound while it moves");
        r.ui->keyUp(Key::Shift);
        ok(r.label(4) == "SNARE" && r.val(4) == "Snare Crack" && r.cell(4).pend, "without SHIFT: the sample that would be taken, marked as not picked");
        ok(r.lane(7).sample() == start, "still the lane's own sample");
        r.later(1000);
        ok((int)r.sv.auditions.size() == auds + 1 && r.sv.auditions.back() == r.lib->catFirst(r.cat("SNARE")), "heard once when the knob stops");
        ok(r.lane(7).sample() == start, "a preview, not a pick");
        r.ui->turn(4, 1);
        ok(r.lane(7).sample() == r.lib->catFirst(r.cat("SNARE")) && !r.cell(4).pend, "the first turn picks what the screen showed");
        r.later(1000);
        r.ui->turn(4, 1); r.later(1000); r.ui->turn(4, 1);
        ok(r.lane(7).sample() == r.nth("SNARE", 2) && r.cell(4).tag == (std::string("3/") + std::to_string(r.lib->catSize(r.cat("SNARE")))).c_str(), "then a sample a turn, its place in the corner");
        ok(has(r.toast(), "SNARE 3/") && has(r.toast(), r.name(r.lane(7).sample())), "folder, place and name, all of the name, in the toast");
        r.later(1000);
        ok(r.sv.auditions.back() == r.lane(7).sample(), "and it is heard");
    }

    {   // what is on either side of a folder
        Card r;
        r.toFolder(4, "CLAP");
        ok(r.lane(7).sample() == 0, "(moved, nothing picked)");
        r.turn(4, -1);
        ok(r.lane(7).sample() == r.nth("CLAP", 0), "the first turn picks, also turned left");
        const int n = r.lib->catSize(r.cat("CLAP"));
        r.turn(4, n * 2);
        ok(r.lane(7).sample() == r.nth("CLAP", n - 1), "a sample knob stops at the end of its folder");
        ok(has(r.toast(), "LAST IN CLAP") && has(r.toast(), "SHIFT"), "and says how to go on");
        r.turn(4, -n * 2);
        ok(r.lane(7).sample() == r.nth("CLAP", 0) && has(r.toast(), "FIRST IN CLAP"), "the other end too");
        // the built-in samples share a folder with the card's, and are not next to them
        Card q;
        ok(q.lib->catNext(1, 1) == q.nth("KICK", 2), "the built-in kicks run on into the card's");
        ok(q.lib->catPlace(q.nth("KICK", 2)) == 2, "and count in the place");
    }

    {   // an empty folder is not a stop; ALL is not a folder here
        Card r;
        r.ui->keyDown(Key::Shift);
        std::vector<std::string> seen;
        for (int i = 0; i < 64; ++i) r.ui->turn(4, -1);
        ok(r.val(4) == "KICK", "the folder knob has no ALL");
        for (int i = 0; i < 64; ++i) { seen.push_back(r.val(4)); r.ui->turn(4, 1); }
        r.ui->keyUp(Key::Shift);
        ok(std::find(seen.begin(), seen.end(), "EMPTY") == seen.end() && std::find(seen.begin(), seen.end(), "ALL") == seen.end(), "and passes a folder with no samples");
        int places = 0;
        ok(r.lib->catsInUse(r.cat("KICK"), places) == r.lib->catCount - 2 && places == 0, "the count is of folders with samples");
        ok(std::find(seen.begin(), seen.end(), "TEXTURE") != seen.end(), "and reaches the last");
    }

    {   // it remembers
        Card r;
        r.toFolder(4, "TOM");
        r.key(Key::Osc1);
        ok(r.label(0) == "TOM" && r.lane(7).sample() == 0, "the folder stays when the screen changes: the SAMPLE page's T1 is the same picker");
        r.home();
        r.tap(8);
        ok(r.label(4) == "SNARE", "another lane starts in the folder of its own sample");
        r.tap(7);
        ok(r.label(4) == "TOM", "and back, the folder it was in");
        r.toFolder(4, "KICK");
        ok(r.label(4) == "KICK" && r.val(4) == "Kick 808" && !r.cell(4).pend, "back in its own folder the knob is on the lane's sample again");
        r.toFolder(4, "PAD");
        r.turn(4, 1); r.later(1000); r.turn(4, 5);
        const int pad = r.lane(7).sample();
        r.toFolder(4, "LEAD");
        r.turn(4, 1);
        r.toFolder(4, "PAD");
        ok(r.val(4) == r.name(pad), "a folder comes back at the sample last picked in it");
    }

    {   // one undo for a run of turns, and none for the folder knob
        Card r;
        r.toFolder(4, "BASS");
        r.turn(4, 1); r.turn(4, 7);
        ok(r.lane(7).sample() != 0, "(picked)");
        r.shiftKey(Key::Clear);
        ok(r.lane(7).sample() == 0 && has(r.toast(), "UNDONE: SAMPLE ON LANE 8"), "SHIFT + CLEAR goes back to before the run");
        ok(r.label(4) == "KICK" && !r.cell(4).pend, "and the knob with it");
        r.shiftKey(Key::Clear);
        ok(has(r.toast(), "NOTHING TO UNDO"), "one level, as ever");
        r.toFolder(4, "BASS");
        r.shiftKey(Key::Clear);
        ok(has(r.toast(), "NOTHING TO UNDO"), "moving the folder knob is nothing to undo");
        r.turn(4, 1);
        r.tap(8); r.turn(4, 1);
        r.shiftKey(Key::Clear);
        ok(r.lane(8).sample() == 2, "a pick on another lane is its own undo");
        // FILE
        r.tap(7); r.key(Key::File); r.key(Key::Down); r.key(Key::Ok);
        const int picked = r.lane(7).sample();
        r.shiftKey(Key::Clear);
        ok(r.lane(7).sample() != picked && has(r.toast(), "UNDONE: SAMPLE ON LANE 8"), "FILE's OK undoes as before");
    }

    {   // a step's sample lock
        Card r;
        r.key(Key::StepK); r.key(Key::StepK);
        r.ui->layerDown(2); r.ui->layerUp(2);
        const int key = lockKey(PG_SMP, 0);
        Step& st = r.lane(7).st[2];
        st.ov = 0;
        ok(r.label(4) == "STEP SAMPLE" && r.val(4) == "LANE" && r.cell(4).tag == "KICK", "a step with no lock: LANE, in the folder of the lane's sample");
        ok(r.ui->knobPlace(4) == 0.0f, "LANE is the knob's left stop");
        r.ui->turn(4, -1);
        ok(!st.hasLock((uint8_t)key) && r.val(4) == "LANE", "nothing to the left of it");
        r.toFolder(4, "HIHAT");
        ok(r.val(4) == "LANE" && !st.hasLock((uint8_t)key) && r.cell(4).tag == "HIHAT", "the folder knob locks nothing");
        r.turn(4, 1);
        ok(st.lockValue((uint8_t)key, -1) == r.nth("HIHAT", 0) && r.lane(7).sample() == 0, "the first turn locks the sample shown; the lane keeps its own");
        ok(r.label(4) == "STEP HIHAT" && r.val(4) == "Hihat_001", "the step's folder and sample on the cell");
        ok(r.sv.auditions.back() == r.nth("HIHAT", 0), "heard");
        r.turn(4, 3);
        ok(st.lockValue((uint8_t)key, -1) == r.nth("HIHAT", 3), "the sample knob stays in the folder");
        r.shiftKey(Key::Clear);
        ok(!st.hasLock((uint8_t)key) && has(r.toast(), "UNDONE: SAMPLE ON STEP 3"), "one undo for the run: the step has no lock again");
        r.turn(4, 1);
        ok(st.lockValue((uint8_t)key, -1) == r.nth("HIHAT", 3), "a turn takes the folder and the sample it was on");
        r.shiftKey(Key::Clear);
        ok(!st.hasLock((uint8_t)key), "(no lock again)");
        st.setLock((uint8_t)key, (int16_t)r.nth("HIHAT", 2));
        r.turn(4, 2);
        r.shiftKey(Key::Clear);
        ok(st.lockValue((uint8_t)key, -1) == r.nth("HIHAT", 2), "a lock that was there comes back");
        r.turn(4, -1); r.turn(4, -1); r.turn(4, -1);
        ok(!st.hasLock((uint8_t)key) && has(r.toast(), "PLAYS THE LANE'S SAMPLE"), "past the folder's first the lock goes");
        r.shiftKey(Key::Clear);
        ok(st.lockValue((uint8_t)key, -1) == r.nth("HIHAT", 2), "and that is a turn of the knob: undo brings it back");
        // the next step starts where the last one was
        st.setLock((uint8_t)key, (int16_t)r.nth("HIHAT", 4));
        r.ui->layerDown(5); r.ui->layerUp(5);
        r.lane(7).st[5].ov = 0;
        ok(r.val(4) == "LANE" && r.cell(4).tag == "HIHAT", "the next step with no lock starts in the folder of the last pick");
        r.turn(4, 1);
        ok(r.lib->items[r.lane(7).st[5].lockValue((uint8_t)key, -1)].cat == r.cat("HIHAT"), "so the same folder is a turn away");
        // a step with a lock of its own starts at that lock
        r.lane(7).st[6].setLock((uint8_t)key, (int16_t)r.nth("BASS", 9));
        r.ui->layerDown(6); r.ui->layerUp(6);
        r.lane(7).st[6].ov = 0;
        ok(r.label(4) == "STEP BASS" && r.val(4) == r.name(r.nth("BASS", 9)), "a step with its own lock starts there");
        r.ui->keyDown(Key::Shift); r.ui->turn(4, 1); r.ui->keyUp(Key::Shift);
        ok(r.cell(4).pend && r.lane(7).st[6].lockValue((uint8_t)key, -1) == r.nth("BASS", 9), "the folder knob leaves the lock as it is");
        r.ui->layerDown(5); r.ui->layerUp(5);
        r.lane(7).st[5].ov = 0;
        ok(!r.cell(4).pend && has(r.label(4).c_str(), "HIHAT"), "and only that step is out of its folder");
        r.pj->patterns[1].lanes[7].st[6].setLock((uint8_t)key, (int16_t)r.nth("BASS", 9));
        r.ui->layerDown(6); r.ui->layerUp(6);
        r.lane(7).st[6].ov = 0;
        ok(r.cell(4).pend, "(back on the step that moved)");
        r.tp.pat = 1; ++r.tp.patChanges; r.ui->tick();
        ok(!r.cell(4).pend && r.val(4) == r.name(r.nth("BASS", 9)), "the same step of another pattern has not moved");
        r.tp.pat = 0; ++r.tp.patChanges; r.ui->tick();
        // eight locks
        Step& full = r.lane(7).st[7];
        for (int i = 0; i < 8; ++i) full.setLock(lockKey(PG_SAMP, i), 1);
        ok(full.nLocks == 8, "(a full step)");
        r.ui->layerDown(7); r.ui->layerUp(7);
        full.ov = 0;
        r.turn(4, 1);
        ok(!full.hasLock((uint8_t)key) && has(r.toast(), "8 LOCKS"), "a full step says so");
        // the lane's knob on the same view
        ok(r.label(0) == "KICK" && r.val(0) == "Kick 808", "T1 is the lane's own sample, as it was");
    }

    {   // the folder of a step's pick is where the lane's next step starts, also when the folder knob never moved
        Card r;
        r.key(Key::StepK); r.key(Key::StepK);
        r.lane(7).st[2].setLock(lockKey(PG_SMP, 0), (int16_t)r.nth("TOM", 3));
        r.key(Key::Right); r.key(Key::Right);
        r.turn(4, 1);
        ok(r.lane(7).st[2].lockValue(lockKey(PG_SMP, 0), -1) == r.nth("TOM", 4), "(a turn inside the lock's folder)");
        r.key(Key::Right);
        ok(r.val(4) == "LANE" && r.cell(4).tag == "TOM", "the next step with no lock starts there");
    }

    {   // a lock picked here is what the step plays, and it survives a save
        Card r;
        for (Lane& l : r.pj->patterns[0].lanes) l.mute = 1;
        Lane& a = r.lane(7);
        a.mute = 0; a.eu = 0; a.n = 8;
        for (Step& s : a.st) { s.ov = 0; s.nLocks = 0; }
        a.st[2].ov = 1;
        r.key(Key::StepK); r.key(Key::StepK);
        r.key(Key::Right); r.key(Key::Right);
        r.toFolder(4, "CYMBAL");
        r.turn(4, 1); r.turn(4, 4);
        const int want = r.nth("CYMBAL", 4);
        const double dur = 44100.0 * 60 / r.pj->bpm / 4;
        r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0;
        r.run((int)(dur * 3.5 / 32));
        r.tp.playing = 0;
        bool played = false;
        for (auto& o : r.rec.ons)
            for (int i = 0; i < o.nLocks; ++i) played = played || (o.locks[i].key == lockKey(PG_SMP, 0) && o.locks[i].value == want);
        ok(played, "the note carries the sample lock picked in the folder");
        r.home();
        r.key(Key::Info);
        r.ui->keyDown(Key::Shift); r.key(Key::Ok); r.ui->keyUp(Key::Shift);
        a.st[2].nLocks = 0;
        r.key(Key::Ok);
        ok(r.lane(7).st[2].lockValue(lockKey(PG_SMP, 0), -1) == want, "saved and loaded by its path, the same sample");
        r.key(Key::StepK);
        ok(r.ui->stepSub() == 1 && r.ui->cursor() == 2 && r.label(4) == "STEP CYMBAL", "and the knob on it starts in its folder");
    }

    {   // the SAMPLE page, and a lock opened on it
        Card r;
        r.key(Key::Osc1);
        ok(r.label(0) == "KICK" && r.val(0) == "Kick 808", "T1 on the SAMPLE page");
        r.toFolder(0, "SYNTH");
        ok(r.lane(7).sample() == 0, "SHIFT + T1 is the folder knob here too");
        r.turn(0, 1);
        ok(r.lane(7).sample() == r.nth("SYNTH", 0), "T1 picks");
        r.home();
        r.key(Key::StepK); r.hold(2, 450);
        ok(r.ui->lockOpen(), "(a lock is open)");
        r.key(Key::Osc1);
        const uint8_t key = lockKey(PG_SMP, 0);
        ok(r.val(0) == "LANE" && r.label(0) == "STEP SAMPLE", "on an open lock T1 is the step's sample");
        r.turn(0, 1);
        const int own = r.lane(7).st[2].lockValue(key, -1);
        ok(own == r.nth("SYNTH", 1) && r.lane(7).sample() == r.nth("SYNTH", 0), "T1 locks the sample after the lane's, the lane keeps its own");
        ok(r.cell(0).lock && r.cell(0).tag == "LOCK" && has(r.label(0), "SYNTH 2/"), "marked LOCK, with its folder and place");
        r.toFolder(0, "VOCAL");
        ok(r.lane(7).st[2].lockValue(key, -1) == own && r.cell(0).pend, "the folder knob does not touch the lock");
        r.turn(0, 1);
        ok(r.lane(7).st[2].lockValue(key, -1) == r.nth("VOCAL", 0) && r.lane(7).sample() == r.nth("SYNTH", 0), "the turn does");
    }

    {   // the web panel's knobs have stops: their place follows the folder
        Card r;
        auto knobTo = [&](int enc, float target) {   // wb_knob_to
            float was = r.ui->knobPlace(enc);
            if (was < 0.0f || was == target) return;
            const int dir = target > was ? 1 : -1;
            for (int n = 0; n < 1024; ++n) {
                r.ui->turn(enc, dir);
                const float now = r.ui->knobPlace(enc);
                if (now < 0.0f || now == was || (dir > 0 ? now >= target : now <= target)) return;
                was = now;
            }
        };
        const int kicks = r.lib->catSize(r.cat("KICK"));
        ok(r.ui->knobPlace(4) == 0.0f, "the first sample of a folder: the bottom of the knob");
        r.lane(7).smpParam(PG_SMP, 0) = (int16_t)r.nth("KICK", kicks - 1);
        ok(r.ui->knobPlace(4) == 1.0f, "the last: the top");
        knobTo(4, 0.5f);
        const float mid = r.ui->knobPlace(4);
        ok(fabsf(mid - 0.5f) < 2.0f / kicks && r.lib->items[r.lane(7).sample()].cat == r.cat("KICK"), "a drag lands inside the folder, where it was put");
        ok(fabsf(r.cell(4).bar - mid) < 1e-6f, "and the bar on the screen says the same");
        r.ui->keyDown(Key::Shift);
        const float f0 = r.ui->knobPlace(4);
        ok(f0 == 0.0f, "with SHIFT the knob shows the folder: the first of them");
        knobTo(4, 1.0f);
        ok(r.ui->knobPlace(4) == 1.0f && r.val(4) == "TEXTURE", "a drag reaches the last");
        ok(r.lib->items[r.lane(7).sample()].cat == r.cat("KICK"), "and picks nothing");
        knobTo(4, 0.0f);
        ok(r.val(4) == "KICK", "and back");
        r.ui->keyUp(Key::Shift);
        r.toFolder(4, "VINYL");
        ok(r.ui->knobPlace(4) == 0.0f, "a folder not picked in yet: its first sample");
        const int vin = r.lib->catSize(r.cat("VINYL"));
        knobTo(4, 0.5f);   // (the first detent takes the sample shown, so a drag lands on its next move)
        knobTo(4, 0.5f);
        ok(fabsf(r.ui->knobPlace(4) - 0.5f) < 2.0f / vin && r.lib->items[r.lane(7).sample()].cat == r.cat("VINYL"), "and a drag from there picks in it");
        // STEP SAMPLE: LANE, then the folder
        r.key(Key::StepK); r.key(Key::StepK);
        r.ui->layerDown(2); r.ui->layerUp(2);
        const float lane = r.ui->knobPlace(4);
        r.turn(4, 1);
        ok(lane == 0.0f && r.ui->knobPlace(4) > 0.0f, "a lock is to the right of LANE");
        r.turn(4, 1000);
        ok(r.ui->knobPlace(4) == 1.0f, "the folder's last is the stop");
    }

    {   // takes
        Card r;
        // a take kept to a lane, as Card::keepRecording leaves it: at the end of the list, in REC
        SampleItem& it = r.lib->items[r.lib->count];
        memset(&it, 0, sizeof(it));
        snprintf(it.name, sizeof it.name, "REC0001");
        it.path = "REC/REC0001.wav";
        it.cat = (uint8_t)r.lib->addCategory("REC");
        r.sv.recNext = r.lib->count++;
        r.sv.recOn = true;
        r.key(Key::Rec);
        r.key(Key::Rec);
        r.sv.recNow.state = REC_DONE;
        r.key(Key::Ok);
        ok(r.lane(7).sample() == r.sv.recNext, "(the take on the lane)");
        r.home();
        ok(r.label(4) == "REC" && r.val(4) == "REC0001" && r.cell(4).tag == "1/1", "the lane's folder is REC, as the file's is");
        r.shiftKey(Key::Clear);
        ok(r.label(4) == "KICK", "undo takes it off");
        r.toFolder(4, "REC");
        ok(r.val(4) == "REC0001", "and the folder knob reaches REC");
        r.turn(4, 1);
        ok(r.lane(7).sample() == r.sv.recNext, "a take is picked as any sample");
    }

    {   // FILE goes with the knobs
        Card r;
        const int s = r.nth("FOLEY", 7);
        r.lane(7).smpParam(PG_SMP, 0) = (int16_t)s;
        r.key(Key::File);
        ok(r.ui->browseCat() == r.cat("FOLEY") && r.ui->browseItem(r.ui->browseIdx()) == s, "FILE opens in the folder of the lane's sample, on that sample");
        r.key(Key::Down);
        const int row = r.ui->browseIdx();
        ok(r.ui->browseItem(row) == s + 1 && r.sv.auditions.back() == s + 1, "UP and DOWN stay in it, and play");
        r.key(Key::File);
        r.key(Key::File);
        ok(r.ui->browseItem(r.ui->browseIdx()) == s, "again it opens on the lane's sample: nothing was loaded");
        for (int i = 0; i < 64; ++i) r.turn(1, 1);
        const int last = r.ui->browseCat();
        ok(r.ui->browseCount() > 0 && !strcmp(r.lib->cats[last], "TEXTURE"), "CATEGORY passes the folder with no samples");
        r.turn(1, -1);
        ok(!strcmp(r.lib->cats[r.ui->browseCat()], "VINYL"), "(one back)");
        r.key(Key::File);
        ok(r.label(4) == "VINYL" && r.lane(7).sample() == s, "the folder chosen in FILE is the one the sample knob is in");
        r.key(Key::File);
        ok(r.ui->browseCat() == r.cat("VINYL"), "and FILE opens there again: it remembers");
        r.turn(4, 3);
        r.key(Key::Ok);
        ok(r.lane(7).sample() == r.nth("VINYL", 3), "OK loads it");
        ok(r.label(4) == "VINYL" && !r.cell(4).pend && r.val(4) == r.name(r.nth("VINYL", 3)), "the knob goes on from there");
        r.turn(4, 1);
        ok(r.lane(7).sample() == r.nth("VINYL", 4), "one sample on");
        r.key(Key::File);
        r.turn(1, -40);
        ok(r.ui->browseCat() == 0 && r.ui->browseItem(r.ui->browseIdx()) == r.lane(7).sample(), "ALL stays the list's first, on the same sample");
    }

    {   // the card is read for each audition: a fast knob must not queue them up
        Card r;
        r.sv.auditions.clear();
        for (int i = 0; i < 100; ++i) { r.ui->turn(4, 1); r.sv.now += 10; r.ui->tick(); }
        const int during = (int)r.sv.auditions.size();
        ok(during == 1, "a spin plays its first detent, and then waits: " + std::to_string(during));
        ok(r.lane(7).sample() == r.nth("KICK", 100), "while the lane has followed every detent");
        r.later(200);
        ok(r.sv.auditions.size() == 2 && r.sv.auditions.back() == r.lane(7).sample(), "and where it stops is heard");
        r.later(2000);
        ok(r.sv.auditions.size() == 2, "once");
        for (int i = 0; i < 5; ++i) { r.ui->turn(4, 1); r.later(200); }
        ok(r.sv.auditions.size() == 7, "slow turns are each heard");
        r.key(Key::File);
        r.sv.auditions.clear();
        for (int i = 0; i < 40; ++i) { r.key(Key::Down); r.sv.now += 10; r.ui->tick(); }
        ok(r.sv.auditions.size() == 1, "FILE's list too");
        r.later(200);
        ok(r.sv.auditions.size() == 2, "and it plays where it stops");
    }

    {   // what the audition could not do is said, once the sample is reached
        Card r;
        r.sv.auditionWhy = "TOO LONG TO PREVIEW";
        r.turn(4, 1);
        ok(has(r.toast(), "TOO LONG TO PREVIEW") && r.lane(7).sample() == 1, "a sample that cannot be previewed says so, and is picked all the same");
        r.key(Key::File);
        r.later(200);
        r.key(Key::Down);
        ok(has(r.toast(), "TOO LONG TO PREVIEW"), "in FILE too");
    }

    {   // the short lists
        Rig r;
        r.tap(7);
        ok(r.lib->catSize(0) == 12, "the built-in twelve: no card");
        r.ui->keyDown(Key::Shift); r.ui->turn(4, 1); r.ui->keyUp(Key::Shift);
        ok(r.label(4) == "SNARE", "the folders work on the built-in samples too");
        Rig e;
        e.lib->count = 0;
        e.tap(7);
        ok(e.label(4) == "SAMPLE" && e.val(4) == "?", "no samples at all: nothing to show");
        e.ui->turn(4, 1);
        e.shiftTurn(4, 1);
        e.key(Key::Osc1); e.ui->turn(0, 1); e.shiftTurn(0, 1); e.home();
        e.key(Key::StepK); e.key(Key::StepK); e.ui->turn(4, 1); e.shiftTurn(4, 1);
        ok(!e.lane(7).st[0].hasLock(lockKey(PG_SMP, 0)), "and the knobs do nothing");
        ok(e.ui->knobPlace(4) < 0.0f || e.ui->knobPlace(4) == 0.0f, "no place to show");
        e.home();
        e.key(Key::File);
        ok(e.ui->mode() == Mode::Browse && e.ui->browseCount() == 0, "FILE opens on an empty list");
        Rig one;
        one.lib->count = 1;
        one.tap(7);
        ok(one.ui->knobPlace(4) == 0.0f, "a folder of one sample has one place");
    }

    {   // more folders than the old table held: each stays its own
        SampleLib* lib = new SampleLib;
        std::vector<int> c;
        for (int i = 0; i < 40; ++i) { char n[12]; snprintf(n, sizeof n, "F%d", i); c.push_back(lib->addCategory(n)); }
        std::sort(c.begin(), c.end());
        ok(std::unique(c.begin(), c.end()) == c.end(), "forty folders are forty categories");
        delete lib;
    }
    {   // more folders than there are places: the takes keep theirs
        SampleLib* lib = new SampleLib;
        for (int i = 0; i < 60; ++i) { char n[12]; snprintf(n, sizeof n, "F%d", i); lib->addCategory(n); }
        const int rec = lib->addCategory("REC");
        ok(rec == kMaxSampleCats - 1 && !strcmp(lib->cats[rec], "REC"), "REC has a place with every other one taken");
        ok(lib->addCategory("REC") == rec && lib->addCategory("LATE") == rec - 1, "and a folder after it lands in the last folder, not in REC");
        delete lib;
    }
}

// The REC screen, and takes
static void testRecordFixes() {
    {   // a full list: a take is refused before it is made
        Rig r;
        std::deque<std::string> paths;
        fillCard(*r.lib, paths, kMaxSamples - r.lib->count, false);
        ok(r.lib->count == kMaxSamples, "(a full list)");
        r.sv.recOn = true;
        r.tap(7);
        const int was = r.lane(7).sample();
        r.key(Key::Rec);
        ok(r.ui->mode() == Mode::Record, "the screen opens");
        r.key(Key::Rec);
        ok(r.sv.recNow.state == REC_IDLE, "REC does not start");
        ok(has(r.toast(), "THE SAMPLE LIST IS FULL") && has(r.toast(), "3072"), "and says why");
        r.ui->turn(2, 1);
        r.key(Key::Rec);
        ok(r.sv.recNow.state == REC_IDLE && has(r.toast(), "SAMPLE LIST IS FULL"), "nor does it arm");
        r.home();
        ok(r.sv.recKeeps == 0 && r.lane(7).sample() == was, "nothing was kept, nothing changed");
        r.lib->count = kMaxSamples - 1;
        r.key(Key::Rec);
        r.ui->turn(2, -1);
        r.key(Key::Rec);
        ok(r.sv.recNow.state == REC_TAKING, "one place left: a take can be made");
    }

    {   // ADD and MUTE do nothing on the REC screen
        Rig r;
        r.sv.recOn = true;
        r.tap(7);
        r.key(Key::Rec);
        Lane before = r.lane(7);
        const std::string toastBefore = r.toast();
        r.key(Key::Add);
        r.key(Key::Mute);
        ok(memcmp(&before, &r.lane(7), sizeof(Lane)) == 0, "the lane is as it was");
        ok(r.toast() == toastBefore, "and nothing is said: the keys are not there");
        r.home();
        r.key(Key::Mute);
        ok(r.lane(7).mute != before.mute, "off the REC screen MUTE works");
    }

    {   // a folder with other capitals: a take's key finds the file again
        SampleLib* lib = new SampleLib;
        lib->addCategory("REC");
        SampleItem& a = lib->items[lib->count++];
        memset(&a, 0, sizeof(a));
        snprintf(a.name, sizeof a.name, "REC0001");
        a.path = "Rec/REC0001.wav";
        SampleItem& b = lib->items[lib->count++];
        memset(&b, 0, sizeof(b));
        snprintf(b.name, sizeof b.name, "x");
        b.path = "Rec/REC0002.wav";
        lib->sortedEnd = lib->count;
        ok(lib->find("Rec/REC0001.wav") == 0, "the index's own key");
        ok(lib->find("REC/REC0001.wav") == 0, "the key a take was kept under, in a folder with other capitals");
        ok(lib->find("rec/rec0002.WAV") == 1, "any capitals: the card has none");
        ok(lib->find("REC/REC0003.wav") == -1, "and only that file");
        ok(lib->find("REC/REC000") == -1 && lib->find("REC/REC0001.wav.bak") == -1, "the whole key");
        delete lib;
    }
}

// What the screens call the synth
static void testNames() {
    Rig r;
    r.key(Key::File);
    ok(r.val(3) == "SYNTH", "FILE's INSTRUMENT says SYNTH");
    r.turn(3, 1);
    ok(has(r.toast(), "MAKE PART A A SAMPLER"), "(the other way)");
    r.home();
    r.tap(7); r.key(Key::File); r.turn(3, -1);
    ok(has(r.toast(), "A SYNTH") && !has(r.toast(), "CATHODE"), "the toast asks for a synth");
    r.shiftTurn(3, -1);
    ok(has(r.toast(), "IS NOW A SYNTH") && !has(r.toast(), "CATHODE") && r.pj->parts[2].type == PT_CATHODE, "and says so");
    r.shiftKey(Key::Clear);
    ok(has(r.toast(), "UNDONE: PART C MADE A SYNTH"), "the undo too");
    r.home();
    r.tap(1);
    r.ui->keyDown(Key::Shift); r.ui->turn(2, 1); r.ui->keyUp(Key::Shift);
    r.ui->keyDown(Key::Shift); r.ui->turn(2, -1); r.ui->keyUp(Key::Shift);
    ok(!has(r.toast(), "CATHODE") && has(r.toast(), "(SYNTH)"), "PLAYED BY's toast");
    r.key(Key::Spare);
    for (int i = 0; i < 5; ++i) r.key(Key::Spare);
    ok(!has(r.val(0), "CATHODE") && has(r.val(0), "SYNTH"), "the FX screen's PART");
}

// What a panel with stops needs: how far apart a knob's values are, so a drag can go to the nearer one (web/src/web_api.cpp wb_knob_to)
static void testKnobStops() {
    Rig r;
    float stop = -1.0f;
    ok(r.ui->knobPlace(0, &stop) >= 0.0f && fabs(stop - 1.0f / 200.0f) < 1e-6f, "TEMPO has 200 steps between its stops");
    r.ui->knobPlace(6, &stop);
    ok(fabs(stop - 1.0f / 32.0f) < 1e-6f, "LENGTH, up to 32");
    r.ui->knobPlace(7, &stop);
    ok(fabs(stop - 1.0f / 16.0f) < 1e-6f, "HITS, as many as the lane has steps");
    r.ui->knobPlace(5, &stop);
    ok(stop == 0.0f, "LEVEL: too fine to matter");
    r.tap(9);
    r.key(Key::Osc2);
    ok(r.ui->mode() == Mode::Sound && r.ui->page() == PG_SLC, "the SLICE page");
    r.ui->knobPlace(0, &stop);
    ok(fabs(stop - 0.5f) < 1e-6f, "MODE is a list of three");
    r.ui->knobPlace(7, &stop);
    ok(fabs(stop - 0.25f) < 1e-6f, "GRID is a list of five");
    r.turn(0, 1);
    r.ui->tick();
    r.key(Key::StepK);
    r.key(Key::StepK);
    ok(r.ui->mode() == Mode::Step && r.ui->stepSub() == 1, "a sliced lane's second step view");
    ok(r.ui->knobPlace(1, &stop) == 0.0f && stop > 0.0f, "STEP SLICE has a place: no lock, at the bottom");
    r.key(Key::Osc1);
    ok(r.ui->page() == PG_SMP, "the SAMPLE page");
    r.ui->knobPlace(3, &stop);
    ok(fabs(stop - 0.5f) < 1e-6f, "PLAY is a list of three");
    r.key(Key::Spare);
    for (int i = 0; i < FX_VIEWS && r.ui->fxView() != FX_AMP; ++i) r.key(Key::Spare);
    r.ui->knobPlace(0, &stop);
    ok(stop > 0.1f, "the amp's MODEL is a short list: " + std::to_string(stop));
    r.ui->knobPlace(1, &stop);
    ok(stop == 0.0f, "and its knobs are not");
    r.ui->knobPlace(-1, &stop);
    ok(stop == 0.0f, "no such knob, no stops");
    // the knobs of two values
    Rig t;
    t.key(Key::SongK);
    t.ui->knobPlace(4, &stop);
    ok(stop == 1.0f, "AT THE END is one of two");
    t.key(Key::SongK);
    t.key(Key::StepK);
    t.key(Key::StepK);
    t.ui->knobPlace(6, &stop);
    ok(stop == 1.0f, "VOICING is one of two");
    t.key(Key::Mixer);
    for (int i = 0; i < 3; ++i) t.key(Key::Mixer);
    t.ui->knobPlace(3, &stop);
    ok(stop == 1.0f, "the degrade switch is one of two");
}

// A step can lock its part's sends and amp knobs: held for the step, then let go.
static void testFxLocks() {
    {   // the sequencer: a set at the step's start, a release at its end, the sound locks still go with the note
        Rig r;
        Lane& a = r.pj->patterns[0].lanes[0];
        for (Lane& l : r.pj->patterns[0].lanes) l.mute = 1;
        a.mute = 0; a.eu = 0; a.n = 8; a.arp.mode = 0;
        for (Step& s : a.st) { s.ov = 0; s.chord = 0; }
        a.st[0].ov = 1; a.st[1].ov = 1; a.st[2].ov = 1;
        a.st[0].setLock(lockKey(PG_FLT1, 1), 99);
        a.st[0].setLock(lockKey(PG_FXSEND, 0), 100);
        a.st[0].setLock(lockKey(PG_FXAMP, 1), 70);
        a.st[1].setLock(lockKey(PG_FXSEND, 1), 40);
        const double dur = 44100.0 * 60 / r.pj->bpm / 4;
        r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0;
        r.run((int)(dur * 3.5 / 32));
        ok(!r.rec.ons.empty() && r.rec.ons[0].nLocks == 1 && r.rec.ons[0].locks[0].key == lockKey(PG_FLT1, 1), "the note carries only the sound lock");
        std::vector<Rec::Fx> sets, rels;
        for (auto& x : r.rec.fxs) (x.set ? sets : rels).push_back(x);
        ok(sets.size() == 2 && rels.size() == 2, "two locked steps: two sets, two releases");
        ok(sets[0].n == 2 && sets[0].part == a.part && sets[0].lane == 0 && sets[0].at == 0, "step 1: its two effects locks are set at the step's start");
        auto near1 = [](uint64_t x, double y) { return (double)x >= y - 1.0 && (double)x <= y + 1.0; };
        ok(rels[0].token == sets[0].token && near1(rels[0].at, dur * 1.12), "and let go as step 2 starts (swung 12%), by the same token");
        ok(sets[1].n == 1 && sets[1].token != sets[0].token && near1(sets[1].at, dur * 1.12), "step 2 sets its own at that moment");
        ok(rels[1].token == sets[1].token && near1(rels[1].at, dur * 2.0), "and lets go as step 3 starts, with its own token");
        ok(r.rec.fxs.size() == 4, "a step with no effects lock sends nothing (step 3)");
        // STOP in the middle of a locked step lets go at once
        r.tp.playing = 0; r.run(1);
        r.rec.fxs.clear(); r.rec.releaseAll = 0;
        r.tp.gstep = -1; r.tp.pstep = 0; r.tp.playing = 1; r.run(2);
        r.tp.playing = 0; r.run(1);
        ok(r.rec.releaseAll >= 1, "STOP lets go of every effects lock");
        r.run((int)(dur * 2 / 32));
        bool lateRelease = false;
        for (auto& x : r.rec.fxs) lateRelease = lateRelease || !x.set;
        ok(!lateRelease, "and the step's own release is not delivered later");
        ok(r.seq->pending() == 0, "nothing is left in the queue");
    }

    {   // a chord or an arpeggio is several notes but one step: one set, one release
        Rig r;
        for (Lane& l : r.pj->patterns[0].lanes) l.mute = 1;
        Lane& a = r.pj->patterns[0].lanes[0];
        a.mute = 0; a.eu = 0; a.n = 8;
        for (Step& s : a.st) { s.ov = 0; s.chord = 0; s.nLocks = 0; }
        a.st[0].ov = 1; a.st[0].chord = 10; a.st[0].strum = 40;
        a.st[0].setLock(lockKey(PG_FXSEND, 0), 90);
        a.st[2].ov = 1; a.st[2].chord = 1; a.arp.mode = 0;
        a.st[2].setLock(lockKey(PG_FXSEND, 2), 90);
        r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0;
        r.run((int)(44100.0 * 60 / 118 / 4 * 4 / 32));
        int sets = 0, rels = 0;
        for (auto& x : r.rec.fxs) (x.set ? sets : rels) += 1;
        ok(sets == 2 && rels == 2 && r.rec.ons.size() >= 8, "a 5-note strummed chord and a triad: two sets, two releases (" + std::to_string(sets) + "/" + std::to_string(rels) + ", " + std::to_string(r.rec.ons.size()) + " notes)");
        a.arp.mode = 1; a.arp.rate = 5; a.arp.oct = 2;
        r.tp.playing = 0; r.run(2000); r.rec.fxs.clear();
        r.tp.gstep = -1; r.tp.pstep = 0; r.tp.playing = 1;
        r.run((int)(44100.0 * 60 / 118 / 4 * 1.5 / 32));
        sets = 0;
        for (auto& x : r.rec.fxs) sets += x.set;
        ok(sets == 1, "an arpeggio of eight notes over a step: still one set: " + std::to_string(sets));
    }
    {   // a lock lets go where the next step starts, swing or not
        Rig r;
        r.pj->bpm = 120; r.pj->swing = 50;
        for (Lane& l : r.pj->patterns[0].lanes) l.mute = 1;
        Lane& a = r.pj->patterns[0].lanes[0];
        a.mute = 0; a.eu = 0; a.n = 8; a.arp.mode = 0;
        for (Step& s : a.st) { s.ov = 0; s.chord = 0; s.nLocks = 0; }
        for (int i = 0; i < 4; ++i) { a.st[i].ov = 1; a.st[i].setLock(lockKey(PG_FXSEND, 0), (int16_t)(20 + i)); }
        r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0;
        r.run((int)(44100.0 * 60 / 120 / 4 * 4.2 / 32));
        std::vector<Rec::Fx> sets, rels;
        for (auto& x : r.rec.fxs) (x.set ? sets : rels).push_back(x);
        ok(sets.size() == 4 && rels.size() >= 3, "four locked steps, their releases");
        bool exact = true;
        for (size_t k = 0; k + 1 < sets.size() && k < rels.size(); ++k) {
            const int64_t d = (int64_t)rels[k].at - (int64_t)sets[k + 1].at;
            exact = exact && d >= -1 && d <= 1 && rels[k].token == sets[k].token;
        }
        ok(exact, "at 50% swing each lock lets go as the next step starts (not later, not inside it)");
    }
    {   // a full queue: a set never goes in without its release, so nothing is left held
        Rig r;
        r.pj->bpm = 240;
        for (Lane& l : r.pj->patterns[0].lanes) { l.mute = 0; l.eu = 0; l.n = 16; l.arp.mode = 0; }
        for (Lane& l : r.pj->patterns[0].lanes)
            for (int i = 0; i < 16; ++i) { l.st[i].ov = 1; l.st[i].chord = 10; l.st[i].gate = 127; l.st[i].nLocks = 0; l.st[i].setLock(lockKey(PG_FXSEND, 1), 50); }
        r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0;
        int peak = 0;
        for (int b = 0; b < 400; ++b) { r.run(1); peak = r.seq->pending() > peak ? r.seq->pending() : peak; }
        for (Lane& l : r.pj->patterns[0].lanes) l.mute = 1;   // no new steps: what is queued plays out
        for (int b = 0; b < 3000; ++b) r.run(1);
        ok(peak >= 150, "ten lanes of 5-note chords at full gate keep the queue busy: peak " + std::to_string(peak) + " of 256");
        ok(r.seq->pending() == 0 && r.rec.active() == 0, "and every lock was let go: " + std::to_string(r.rec.active()) + " held, " + std::to_string(r.seq->pending()) + " pending");
    }
    {   // the UI: hold a step, press FX, turn
        Rig r;
        r.key(Key::StepK);
        Lane& a = r.ln();
        a.eu = 0; a.n = 8;
        r.ui->layerDown(2); r.sv.now += 400; r.ui->tick(); r.ui->layerUp(2);
        ok(r.ui->lockOpen(), "a held step opens its lock");
        r.key(Key::Spare);
        ok(r.ui->mode() == Mode::Fx && r.ui->fxView() == FX_SENDS && r.ui->lockOpen(), "FX keeps the lock open on the SENDS view");
        ok(r.cell(0).dis && r.label(1) == "DELAY", "the part knob is off; DELAY, REVERB and CHORUS are the locks");
        r.turn(2, 6);
        ok(a.st[2].lockValue(lockKey(PG_FXSEND, 1), -1) == 6 && r.cell(2).lock && r.pj->fx.send[a.part][1] == 0, "turning REVERB locks the step's send; the part's own send is not touched");
        ok(has(r.toast(), "NOTHING ELSE SENDS"), "an effect nobody else sends to: " + r.toast());
        r.turn(0, 1);
        ok(has(r.toast(), "OWN PART"), "the part knob explains itself: " + r.toast());
        r.key(Key::PartK);
        ok(has(r.toast(), "OWN PART") && r.ui->part() == a.part, "PART is refused while a step is locked");
        r.key(Key::Spare);
        ok(r.ui->fxView() == FX_AMP, "FX again: the amp (only the two views the lock can use)");
        r.turn(1, 3);
        ok(has(r.toast(), "AMP IS OFF") && !a.st[2].hasLock(lockKey(PG_FXAMP, 1)), "an amp that is off cannot be locked");
        r.pj->fx.amp[a.part][0] = 2;
        r.pj->fx.amp[a.part][1] = 50;
        r.turn(1, 4);
        ok(a.st[2].lockValue(lockKey(PG_FXAMP, 1), -1) == 54, "with an amp on, DRIVE locks from its value");
        r.turn(0, 1);
        ok(has(r.toast(), "ONLY SENDS AND AMP KNOBS") && !a.st[2].hasLock(lockKey(PG_FXAMP, 0)), "MODEL is not lockable");
        r.turn(6, 1);
        ok(!a.st[2].hasLock(lockKey(PG_FXAMP, 6)), "nor is CAB");
        r.key(Key::Lane);
        ok(r.ui->mode() == Mode::Step && r.ui->lockOpen(), "L goes back to the step with its lock still open");
        r.key(Key::Spare);
        r.key(Key::Clear);
        ok(a.st[2].nLocks == 0, "CLEAR clears the step's locks, effects included");
        r.key(Key::Lane);
        r.tap(2);   // the same step again: the lock closes
        ok(!r.ui->lockOpen(), "tapping the step closes the lock");
        r.key(Key::Spare);
        ok(r.ui->mode() == Mode::Fx && !r.ui->lockOpen() && r.ui->fxView() == FX_AMP, "without a lock the FX screen is as before (it stays on the view it was on)");
        r.key(Key::Spare);
        ok(r.ui->fxView() == FX_SENDS, "and FX goes through all six views again");
        r.turn(1, 3);
        ok(r.pj->fx.send[r.ui->part()][0] == 3 && !a.st[2].hasLock(lockKey(PG_FXSEND, 0)), "and a knob there sets the part's own send");
    }
    {   // the project file keeps them, and refuses what is not lockable
        Project* v = new Project;
        Project* out = new Project;
        Project* scratch = new Project;
        makeDefaultProject(*v);
        Step& s = v->patterns[0].lanes[0].st[3];
        s.nLocks = 0;
        s.setLock(lockKey(PG_FXSEND, 2), 90);
        s.setLock(lockKey(PG_FXAMP, 5), 33);
        s.setLock(lockKey(PG_FXAMP, 0), 2);        // MODEL: not lockable
        s.setLock(lockKey(PG_FXSEND, 3), 5);       // the spare send
        s.setLock(lockKey(PG_FXAMP, 6), 1);        // CAB
        s.setLock(lockKey(PG_FXSEND, 1), 127);
        Step& s2 = v->patterns[0].lanes[0].st[4];
        s2.nLocks = 0;
        s2.setLock(lockKey(PG_FXSEND, 0), 300);     // too much
        s2.setLock(lockKey(PG_FXAMP, 7), 999);
        s2.setLock(lockKey(PG_FXAMP, 2), -5);
        MemSink sink;
        writeProject(*v, sink, nullptr);
        MemSource src(sink.bytes);
        ok(readProject(src, *scratch, *out, nullptr, nullptr), "a project with effects locks reads back");
        const Step& g = out->patterns[0].lanes[0].st[3];
        ok(g.lockValue(lockKey(PG_FXSEND, 2), -1) == 90 && g.lockValue(lockKey(PG_FXAMP, 5), -1) == 33 && g.lockValue(lockKey(PG_FXSEND, 1), -1) == 127, "the sends and amp knobs are kept");
        ok(!g.hasLock(lockKey(PG_FXAMP, 0)) && !g.hasLock(lockKey(PG_FXSEND, 3)) && !g.hasLock(lockKey(PG_FXAMP, 6)) && g.nLocks == 3, "MODEL, CAB and the spare send are dropped: " + std::to_string(g.nLocks));
        const Step& g2 = out->patterns[0].lanes[0].st[4];
        ok(g2.lockValue(lockKey(PG_FXSEND, 0), -1) == 127 && g2.lockValue(lockKey(PG_FXAMP, 7), -1) == 127 && g2.lockValue(lockKey(PG_FXAMP, 2), -1) == 0, "values past the knob's range are held to it");
        delete v; delete out; delete scratch;
    }
    {   // the state follows the lock: back from a view that cannot lock, and PART cannot move away from the lane
        Rig r;
        r.key(Key::Spare); r.key(Key::Spare); r.key(Key::Spare); r.key(Key::Spare);   // the COMP view
        r.key(Key::StepK);
        r.ui->layerDown(3); r.sv.now += 400; r.ui->tick(); r.ui->layerUp(3);
        r.key(Key::Lane);
        r.ui->settle();
        ok(!(r.ui->mode() == Mode::Fx && r.ui->lockOpen()) || (r.ui->fxView() == FX_SENDS || r.ui->fxView() == FX_AMP), "an open lock never sits on a view that cannot lock it");
        r.key(Key::Spare);
        r.ui->settle();
        ok(r.ui->mode() == Mode::Fx && r.ui->lockOpen() && (r.ui->fxView() == FX_SENDS || r.ui->fxView() == FX_AMP) && r.ui->part() == r.ln().part, "FX with a lock: the lane's part, SENDS or AMP");
    }
}

// SAVE PRESET: SHIFT + OK on the sound pages or in BROWSE; a name editor; a new preset or a replacement.
static void loadInBrowse(Rig& r, int id) {
    r.key(Key::File);
    int row = -1;
    for (int i = 0; i < r.ui->browseCount(); ++i) if (r.ui->browseItem(i) == id) row = i;
    while (r.ui->browseIdx() < row) r.key(Key::Down);
    r.key(Key::Ok);
}
// A Standard MIDI File, read back the way a sequencer would
struct MidiNote { int track, ch, note, vel; uint32_t on, off; };
struct MidiFile {
    int format = -1, tracks = -1, ppq = 0;
    uint32_t tempo = 0, lastTick = 0;
    std::vector<std::string> names;
    std::vector<MidiNote> notes;
    bool bad = false;
    std::string why;
    explicit MidiFile(const std::vector<uint8_t>& b) {
        size_t p = 0;
        const auto be = [&](int n) { uint32_t v = 0; while (n--) { if (p >= b.size()) { fail("ran off the end"); return v; } v = (v << 8) | b[p++]; } return v; };
        if (b.size() < 14 || memcmp(b.data(), "MThd", 4)) { fail("no MThd"); return; }
        p = 4;
        if (be(4) != 6) fail("header length");
        format = (int)be(2); tracks = (int)be(2); ppq = (int)be(2);
        for (int t = 0; t < tracks && !bad; ++t) {
            if (p + 8 > b.size() || memcmp(&b[p], "MTrk", 4)) { fail("no MTrk"); return; }
            p += 4;
            const uint32_t length = be(4);
            const size_t end = p + length;
            if (end > b.size()) { fail("track past the end"); return; }
            uint32_t tick = 0;
            std::map<int, std::vector<size_t>> open;
            bool ended = false;
            std::string name;
            while (p < end && !bad) {
                uint32_t d = 0;
                uint8_t c;
                do { c = b[p++]; d = (d << 7) | (c & 0x7F); } while ((c & 0x80) && p < end);
                tick += d;
                const uint8_t st = b[p++];
                if (st == 0xFF) {
                    const uint8_t kind = b[p++];
                    uint32_t n = 0;
                    do { c = b[p++]; n = (n << 7) | (c & 0x7F); } while (c & 0x80);
                    if (kind == 0x51) tempo = (uint32_t)((b[p] << 16) | (b[p + 1] << 8) | b[p + 2]);
                    if (kind == 0x03) name.assign((const char*)&b[p], n);
                    if (kind == 0x2F) ended = true;
                    p += n;
                } else if ((st & 0xF0) == 0x90 && b[p + 1]) {
                    open[(st & 15) * 128 + b[p]].push_back(notes.size());
                    notes.push_back({t, st & 15, b[p], b[p + 1], tick, 0});
                    p += 2;
                } else if ((st & 0xF0) == 0x80) {
                    auto& o = open[(st & 15) * 128 + b[p]];
                    if (o.empty()) { fail("a note off with no note on"); return; }
                    notes[o.front()].off = tick;
                    o.erase(o.begin());
                    p += 2;
                } else { fail("an event that should not be here"); return; }
                if (tick > lastTick) lastTick = tick;
            }
            if (p != end || !ended) fail("a track that does not end where it says");
            for (auto& kv : open) if (!kv.second.empty()) fail("a note with no end");
            names.push_back(name);
        }
    }
    void fail(const char* w) { if (!bad) why = w; bad = true; }
};

static void testMidiExport() {
    Project* pj = new Project;
    Project* work = new Project;
    SampleLib* lib = new SampleLib;
    std::vector<int16_t> store(kFactoryFrames);
    buildFactorySamples(*lib, store.data(), store.size());
    const auto make = [&](int song, MemSink& out) { *work = *pj; return writeSongMidi(*work, song, lib, out); };

    // one lane, one bar, played twice: every tick can be worked out by hand
    makeNewProject(*pj);
    pj->bpm = 120;
    pj->swing = 0;
    pj->patterns[0].lanes[0].root = 40;
    for (const int i : {0, 4, 8, 12}) pj->patterns[0].lanes[0].st[i].ov = 1;
    pj->patterns[0].lanes[0].st[8].note = 7;
    pj->patterns[0].lanes[0].st[8].vel = 64;
    pj->midi[0].ch = 12;   // (a channel of its own: on a shared one a lane is its trigger note)
    pj->songs[0].rows[0] = {0, 2};
    pj->songs[0].nRows = 1;
    MemSink a;
    ok(make(0, a), "a song makes a file");
    MidiFile m(a.bytes);
    ok(!m.bad, "the file reads back: " + m.why);
    if (m.notes.size() < 8) return;
    ok(m.format == 1 && m.tracks == 2 && m.ppq == kMidiPpq, "type 1, the tempo track and one lane");
    ok(m.tempo == 500000, "120 BPM is 500,000 microseconds a beat: " + std::to_string(m.tempo));
    ok(m.notes.size() == 8, "four notes, twice: " + std::to_string(m.notes.size()));
    bool times = true;
    for (size_t i = 0; i < m.notes.size(); ++i) times = times && m.notes[i].on == i * kMidiPpq && m.notes[i].ch == 11;
    ok(times, "each on its beat, on the lane's MIDI channel");
    ok(m.notes[0].note == 40 && m.notes[2].note == 47 && m.notes[2].vel < m.notes[0].vel, "the root, a step's own note and its velocity");
    ok(m.notes[0].off > m.notes[0].on && m.notes[0].off < m.notes[1].on, "a note ends at its gate");
    ok(m.names[1] == "Lane 1 Part A", "the track says what it is: " + m.names[1]);

    // what the sequencer does is in the file
    pj->swing = 50;
    MemSink sw;
    make(0, sw);
    pj->swing = 0;
    pj->patterns[0].lanes[1].st[0].ov = 1;
    pj->patterns[0].lanes[1].st[1].ov = 1;
    pj->patterns[0].lanes[0].st[0].chord = 2;
    pj->patterns[0].lanes[8].st[0].ov = 1;
    pj->patterns[0].lanes[8].st[0].note = 2;
    MemSink c;
    make(0, c);
    MidiFile mc(c.bytes);
    int at0 = 0;
    for (const MidiNote& n : mc.notes) at0 += n.track == 1 && n.on == 0;
    ok(!mc.bad && mc.tracks == 4 && at0 == 3, "a chord is its three notes, and each lane with notes has a track");
    bool drum = false;
    for (const MidiNote& n : mc.notes) drum = drum || (n.track == 3 && n.note == noteOut(*pj, 8, 2, true));
    ok(drum && mc.names[3].find("Lane 9") == 0 && mc.names[3].find(lib->items[pj->patterns[0].lanes[8].sample()].name) != std::string::npos, "a sampler lane is what its MIDI out sends, named after its sample: " + mc.names[3]);
    MidiFile ms(sw.bytes);
    ok(!ms.bad && ms.notes[1].on == m.notes[1].on, "swing moves the off-beat sixteenths, not the beats");

    // a chance plays or not, and is the same each time, also with other lanes in the file
    pj->patterns[0].lanes[0].st[4].prob = 50;
    MemSink p1, p2;
    make(0, p1); make(0, p2);
    ok(p1.bytes == p2.bytes, "the same project makes the same file");

    MemSink none;
    ok(!make(3, none) && none.bytes.empty(), "a song with no rows makes no file");

    // what the break test found
    for (const int lanes : {1, 2}) {
        Project* q = new Project;
        makeNewProject(*q); q->bpm = 120; q->swing = 0;
        for (int l = 0; l < lanes; ++l) {
            Lane& n = q->patterns[0].lanes[l]; n.root = 48;
            for (const int s : {0, 4, 8, 12}) { n.st[s].ov = 1; n.st[s].note = (int8_t)s; }
            q->midi[l].ch = 0;
        }
        q->songs[0].rows[0] = {0, 1}; q->songs[0].nRows = 1;
        MemSink o; *work = *q; writeSongMidi(*work, 0, lib, o); MidiFile f(o.bytes);
        std::map<int, int> pitches;
        for (const MidiNote& n : f.notes) ++pitches[n.note];
        ok(!f.bad && pitches.size() == 4, "lanes with MIDI off keep their notes: " + std::to_string(pitches.size()));
        delete q;
    }
    {
        Project* q = new Project;
        makeNewProject(*q); q->bpm = 120; q->swing = 0;
        Lane& n = q->patterns[0].lanes[0];
        n.arp.mode = 1; n.arp.rate = 5; n.arp.oct = 2; n.arp.gate = 5; n.st[0].ov = 1;
        q->songs[0].rows[0] = {0, 1}; q->songs[0].nRows = 1;
        MemSink o; *work = *q; writeSongMidi(*work, 0, lib, o); MidiFile f(o.bytes);
        int zero = 0;
        for (const MidiNote& e : f.notes) zero += e.off <= e.on;
        ok(!f.bad && !f.notes.empty() && zero == 0, "an arpeggio's short notes have a length");

        makeNewProject(*q); q->bpm = 120; q->swing = 0;
        q->patterns[0].lanes[7].st[0].ov = 1;
        q->songs[0].rows[0] = {0, 1}; q->songs[0].rows[1] = {3, 2}; q->songs[0].nRows = 2;
        MemSink e2; *work = *q; writeSongMidi(*work, 0, lib, e2); MidiFile f2(e2.bytes);
        ok(!f2.bad && f2.lastTick == 3 * 16 * kMidiPpq / 4, "a song that ends in rests is its whole length: " + std::to_string(f2.lastTick));

        makeNewProject(*q); q->bpm = 120; q->swing = 0;
        Lane& h = q->patterns[0].lanes[0]; h.root = 48;
        for (int s = 0; s < 8; ++s) { h.st[s].ov = 1; h.st[s].gate = 64; }
        q->songs[0].rows[0] = {0, 1}; q->songs[0].nRows = 1;
        MemSink e3; *work = *q; writeSongMidi(*work, 0, lib, e3); MidiFile f3(e3.bytes);
        int over = 0;
        for (size_t i = 0; i + 1 < f3.notes.size(); ++i) over += f3.notes[i].off > f3.notes[i + 1].on;
        ok(!f3.bad && f3.notes.size() == 8 && over == 0, "a repeated note ends before it starts again: " + std::to_string(over));

        makeNewProject(*q); q->bpm = 120; q->swing = 0;
        q->patterns[0].lanes[0].st[0].ov = 1;
        q->patterns[1].lanes[0].part = 2; q->patterns[1].lanes[0].smpParam(PG_SMP, 0) = 3; q->patterns[1].lanes[0].st[0].ov = 1;
        q->patterns[0].lanes[0].st[0].ov = 0;
        q->songs[0].rows[0] = {0, 1}; q->songs[0].rows[1] = {1, 1}; q->songs[0].nRows = 2;
        ok(laneFirstRow(*q, 0, 0) == 1, "a lane is named from the first row it plays in");
        delete q;
    }
    pj->patterns[0].lanes[0].mute = 1;
    MemSink mu;
    make(0, mu);
    MidiFile mm(mu.bytes);
    bool lane1 = false;
    for (const std::string& n : mm.names) lane1 = lane1 || n.find("Lane 1 ") == 0;
    ok(!lane1, "a muted lane is not in the file, as it is not in the song");

    struct Refuse : ByteSink { size_t left; explicit Refuse(size_t n) : left(n) {} bool write(const void*, size_t n) override { if (n > left) return false; left -= n; return true; } };
    { Refuse r(40); *work = *pj; ok(!writeSongMidi(*work, 0, lib, r), "a card that refuses is reported"); }

    // the factory's: every one, in full, and as long as its song says
    for (int i = 0; i < kFactoryProjects; ++i) {
        makeFactoryProject(i, *pj);
        MemSink f;
        const std::string name = factoryProjectName(i);
        ok(make(0, f), name + " makes a file");
        MidiFile mf(f.bytes);
        ok(!mf.bad, name + " reads back: " + mf.why);
        ok(mf.notes.size() > 100, name + ": notes: " + std::to_string(mf.notes.size()));
        const Song& so = pj->songs[0];
        uint32_t ticks = 0;
        for (int r = 0; r < so.nRows; ++r) {
            int longest = 0;
            for (const Lane& l : pj->patterns[so.rows[r].pat].lanes) longest = l.n > longest ? l.n : longest;
            ticks += (uint32_t)(so.rows[r].rep * longest * kMidiPpq / 4);
        }
        ok(mf.lastTick >= ticks - kMidiPpq && mf.lastTick <= ticks + 4 * kMidiPpq, name + " is the song's length: " + std::to_string(mf.lastTick) + " of " + std::to_string(ticks));
    }
    delete pj; delete work; delete lib;
}

// PROJECT's third view: only STOP while a song goes to files
// GLOBAL's CLOCK is the project's, and a sampler lane's LFO has three places to go
static void testGlobalAndMod() {
    Rig r;
    r.key(Key::Glob);
    ok(r.label(4) == "CLOCK", "GLOBAL has CLOCK: " + r.label(4));
    r.turn(4, 1);
    bool all = true;
    for (const Part& p : r.pj->parts) all = all && p.pages[PG_GLOB][4] == 1;
    ok(all && r.val(4) == "MIDI", "it is set for the project, on every part: " + r.val(4));
    r.turn(4, 5);
    ok(r.pj->parts[3].pages[PG_GLOB][4] == 2, "and stops at SYNC");
    r.turn(4, -5);
    r.tp.clockExt = 1;
    r.pj->parts[0].pages[PG_GLOB][4] = 1;
    r.key(Key::Play);
    ok(has(r.toast(), "WAITING FOR THE MIDI CLOCK"), "PLAY says what it waits for: " + r.toast());
    r.key(Key::Stop);
    r.tp.clockExt = 0;
    r.pj->parts[0].pages[PG_GLOB][4] = 0;
    {   // CLOCK is the project's: a preset leaves it, an undo keeps it, and a file from before it worked means nothing by it
        Rig u;
        u.key(Key::Glob);
        u.turn(4, 1);
        const int id = presetIdAt(4);
        ok(presetLoad(id), "a preset from the card");
        int16_t pg[kSynthPages][kParamsPerPage];
        memcpy(pg, u.pj->parts[0].pages, sizeof pg);
        ok(applyPresetIndex(pg, id) && pg[PG_GLOB][4] == 1, "a preset leaves CLOCK alone");
        u.pj->patterns[0].lanes[0].root = 50;
        u.key(Key::Clear); u.shiftKey(Key::Clear);
        bool same = true;
        for (const Part& p : u.pj->parts) same = same && p.pages[PG_GLOB][4] == 1;
        ok(same, "and so does an undo");
        Rig c;
        c.pj->parts[0].pages[PG_GLOB][4] = 1;
        c.pj->parts[3].pages[PG_GLOB][4] = 2;
        ok(c.sv.saveSlot(0, *c.pj), "a project with a stray CLOCK");
        c.sv.card[0][4] = 8;   // an older file: the version
        c.sv.card[0][5] = 0;
        Project* q = new Project;
        int m = 0;
        const bool read = c.sv.loadSlot(0, *q, m);
        bool zero = true;
        for (const Part& p : q->parts) zero = zero && p.pages[PG_GLOB][4] == 0;
        ok(read && zero, "before version 9 CLOCK meant nothing, and loads as INT");
        c.sv.card[0][4] = kProjectVersion;
        ok(c.sv.loadSlot(0, *q, m) && q->parts[6].pages[PG_GLOB][4] == 1, "now it is the project's: every part takes part A's");
        delete q;
    }
    r.tap(7);
    r.key(Key::ModK);
    ok(r.ui->mode() == Mode::Sound && r.label(2) == "LFO DEST", "a sampler lane has MOD");
    r.turn(2, 8);
    ok(r.val(2) == "AMP", "its LFO stops at AMP: " + r.val(2));
}

static void testExportScreen() {
    Rig r;
    r.key(Key::Info); r.key(Key::Info); r.key(Key::Info);
    ok(r.ui->projSub() == 2 && has(r.toast(), "PROJECT 3/3") && r.label(4) == "SONG" && r.label(5) == "EXPORT", "INFO goes on to EXPORT: " + r.toast());
    ok(r.val(5) == "SONG WAV", "a song to one WAV first");
    r.turn(5, 1); ok(r.val(5) == "STEMS", "then stems");
    r.turn(5, 1); ok(r.val(5) == "MIDI FILE", "then a MIDI file");
    r.turn(5, 5); ok(r.ui->exportKind() == EXPORT_MIDI, "and stops there");
    r.turn(5, -2);
    r.turn(4, 2); ok(r.val(4) == "SONG 3", "B1 picks the song");
    r.key(Key::Ok); ok(has(r.toast(), "SONG 3 HAS NO ROWS") && !r.ui->exporting() && !r.sv.exportCalls, "a song with no rows says so: " + r.toast());
    r.turn(4, -2);
    r.sv.exportRefuses = true;
    r.key(Key::Ok); ok(has(r.toast(), "NOT EXPORTED") && !r.ui->exporting(), "a card that refuses says so: " + r.toast());
    r.sv.exportRefuses = false;
    r.tp.playing = 1;
    r.key(Key::Ok);
    ok(r.ui->exporting() && !r.tp.playing && r.sv.exportKind == EXPORT_WAV && r.sv.exportSong == 0, "OK stops what plays and starts it");
    const int part = r.ui->part(), bpm = r.pj->bpm, sel = r.ui->sel();
    r.key(Key::Play); r.key(Key::Lane); r.key(Key::Clear); r.tap(3); r.turn(0, 5);
    ok(r.ui->exporting() && r.ui->mode() == Mode::ProjectM && !r.tp.playing && r.ui->part() == part && r.pj->bpm == bpm && r.ui->sel() == sel, "keys, knobs and layer keys do nothing meanwhile");
    r.sv.exportPercent = 50; r.ui->tick();
    ok(r.ui->exportStatus().percent == 50, "the progress comes from the work");
    r.key(Key::Stop); ok(r.sv.exportCancels == 1 && r.ui->exporting(), "STOP gives up");
    r.sv.exportResult = EXPORT_STOPPED; r.ui->tick();
    ok(!r.ui->exporting() && has(r.toast(), "EXPORT STOPPED"), "and says so: " + r.toast());
    r.key(Key::Ok); r.sv.exportResult = EXPORT_DONE; r.tp.songEnds = r.tp.songEnds + 3; r.ui->tick(); r.ui->tick();
    ok(!r.ui->exporting() && has(r.toast(), "SONG SAVED") && std::string(r.ui->exportNote()).find("SONG SAVED") == 0, "a finished one says where, and its passes are not the end of a song: " + r.toast());
    r.key(Key::Ok); ok(!r.ui->exportNote()[0], "the next one starts clean");
    r.sv.exportResult = EXPORT_FAILED; r.tp.songEnds = r.tp.songEnds + 1; r.ui->tick(); r.ui->tick();
    ok(!r.ui->exporting() && has(r.toast(), "EXPORT FAILED"), "and a failed one says so: " + r.toast());
    r.sv.exportResult = EXPORT_RUNNING; r.sv.exportMissing = 2;
    r.key(Key::Ok); r.sv.exportResult = EXPORT_DONE; r.ui->tick();
    ok(has(r.toast(), "2 SAMPLES MISSING"), "samples that did not load are told: " + r.toast());
    r.sv.exportMissing = 0;
    r.sv.exportResult = EXPORT_RUNNING;
    r.turn(5, 2); r.tp.playing = 1; r.key(Key::Ok);
    ok(r.ui->exporting() && r.tp.playing, "a MIDI file leaves what plays alone");
    r.sv.exportResult = EXPORT_DONE; r.ui->tick(); r.tp.playing = 0; r.turn(5, -2);
    r.sv.exportResult = EXPORT_RUNNING;
    r.key(Key::Clear); ok(has(r.toast(), "DELETING IS IN PROJECT 1/3") && !r.sv.card[0].size(), "the small ring deletes nothing here: " + r.toast());
}

// A new project has nothing to play. The factory's come ahead of the card on PROJECT, and loading one gives a copy to change
static void testFactoryProjects() {
    Project* p = new Project;
    Project* q = new Project;
    makeNewProject(*p);
    bool quiet = true;
    for (const Pattern& pt : p->patterns)
        for (const Lane& l : pt.lanes) {
            quiet = quiet && !l.eu && !l.k;
            for (const Step& st : l.st) quiet = quiet && !st.ov && !st.nLocks;
        }
    ok(quiet && p->songs[0].nRows == 0, "a new project plays nothing");
    makeDefaultProject(*q);
    ok(memcmp(p->parts, q->parts, sizeof p->parts) == 0 && p->patterns[0].lanes[7].part == 2 && p->patterns[0].lanes[7].sample() == q->patterns[0].lanes[7].sample(), "with the same sounds and lanes");

    for (int i = 0; i < kFactoryProjects; ++i) {
        Rig r;
        makeFactoryProject(i, *p);
        const std::string name = factoryProjectName(i);
        ok(name == p->name, "the project carries its name: " + name);
        ok(r.sv.saveSlot(0, *p) && [&] { int m = 0; return r.sv.loadSlot(0, *q, m) && m == 0; }() && memcmp(p->patterns, q->patterns, sizeof p->patterns) == 0 && memcmp(p->parts, q->parts, sizeof p->parts) == 0 && memcmp(&p->fx, &q->fx, sizeof p->fx) == 0, name + " survives the card unchanged");
        ok(p->songs[0].nRows >= 4, name + " has a song");
        for (int row = 0; row < p->songs[0].nRows; ++row) ok(p->songs[0].rows[row].pat < kPatterns && p->songs[0].rows[row].rep >= 1, name + ": song row");
        if (i == 0) continue;
        int locks = 0, kinds = 0, onSynth = 0, onSampler = 0, onFx = 0;
        bool seen[256] = {}, legal = true, bars = true;
        for (int pi = 0; pi < 4; ++pi) {
            for (const Lane& l : p->patterns[pi].lanes) {
                bars = bars && l.n >= 1 && l.n <= kMaxSteps;
                for (int k = 0; k < kMaxSteps; ++k) {
                    const Step& st = l.st[k];
                    legal = legal && (k < l.n || !st.nLocks);
                    for (int n = 0; n < st.nLocks; ++n) {
                        const int key = st.locks[n].key, page = key / kParamsPerPage, param = key % kParamsPerPage;
                        ++locks;
                        kinds += !seen[key];
                        seen[key] = true;
                        if (page >= PG_COUNT) { ++onFx; legal = legal && fxLockableParam(page, param); continue; }
                        const ParamDef& d = pageDef(page).p[param];
                        const int lo = d.min, hi = d.unit == Unit::Sample ? 11 : d.max;
                        legal = legal && st.locks[n].value >= lo && st.locks[n].value <= hi && page != PG_GLOB;
                        if (page >= PG_SMP || p->parts[l.part].type == PT_SAMPLER) ++onSampler;
                        else ++onSynth;
                    }
                }
            }
        }
        ok(legal && bars, name + ": every lock is one a player could set: page, range, inside the lane");
        ok(locks >= 60 && kinds >= 8 && onSynth >= 20 && onSampler >= 15 && onFx >= 3, name + " shows locks off: " + std::to_string(locks) + " locks of " + std::to_string(kinds) + " kinds");
        const int used = p->parts[0].voices + p->parts[1].voices;
        ok(used <= 4 && p->parts[2].type == PT_SAMPLER, name + " stays on the voices the board carries");
        // it plays: a run through every pattern, at once and with nothing refused
        r.key(Key::Info);
        r.turn(4, i - kFactoryProjects);
        r.key(Key::Ok);
        ok(std::string(r.pj->name) == name && r.ui->loadedSlot() == i - kFactoryProjects, name + " loads from the list");
    }

    {
        Rig r;
        makeNewProject(*r.pj);
        r.key(Key::Play); ok(r.tp.playing && has(r.toast(), "NOTHING TO PLAY YET") && has(r.toast(), "FACTORY PROJECTS"), "PLAY on nothing says what to do: " + r.toast());
        r.key(Key::Stop);
        r.sv.now += 2000;
        makeFactoryProject(1, *r.pj);
        r.tp.pat = 1;
        r.key(Key::Play); ok(!has(r.toast(), "NOTHING TO PLAY"), "and only then");
    }

    Rig r;
    r.key(Key::Info);
    r.turn(4, -1); ok(r.ui->slot() == -1 && r.val(4) == "F3" && r.label(4) == "FACTORY", "B1 goes on past slot 1 to the factory: " + r.val(4));
    r.turn(4, -5); ok(r.ui->slot() == -kFactoryProjects && r.val(4) == "F1", "and stops at F1");
    r.key(Key::Up); ok(r.ui->slot() == -kFactoryProjects, "so does the up key");
    r.pj->bpm = 77;
    r.key(Key::Ok); ok(r.pj->bpm == 118 && std::string(r.pj->name) == "DEMO" && has(r.toast(), "DEMO LOADED") && has(r.toast(), "UNDOES"), "OK loads it: " + r.toast());
    r.shiftKey(Key::Clear); ok(r.pj->bpm == 77 && r.ui->loadedSlot() == kNoSlot, "SHIFT + CLEAR goes back to what was there");
    r.key(Key::Down); r.key(Key::Ok); ok(r.pj->bpm == 132, "F2");
    r.pj->bpm = 140; r.key(Key::Ok);
    ok(r.pj->bpm == 132, "it can be changed, and loading it again gives the original");
    r.shiftKey(Key::Ok); ok(has(r.toast(), "FACTORY PROJECT STAYS"), "saving over it is refused and says where: " + r.toast());
    r.key(Key::Clear); ok(has(r.toast(), "CANNOT BE DELETED") && r.ui->deleteAsked() == -1, "and it cannot be deleted: " + r.toast());
    r.pj->bpm = 140;
    r.turn(4, 2); ok(r.ui->slot() == 0, "the card's slots come next");
    r.shiftKey(Key::Ok);
    { SlotInfo si; ok(r.sv.slotInfo(0, si) && std::string(si.name) == "TECHNO" && si.bpm == 140 && r.ui->loadedSlot() == 0, "a changed copy goes to a slot under its own name"); }
    r.turn(4, kCardSlots); ok(r.ui->slot() == kCardSlots - 1 && r.val(4) == "30 EMPTY", "thirty slots: " + r.val(4));
    r.sv.slotReads = 0;
    r.shiftKey(Key::Ok); ok(!r.sv.card[kCardSlots - 1].empty() && r.sv.slotReads <= 2, "a save reads its own slot again, not all thirty: " + std::to_string(r.sv.slotReads));
    r.sv.slotReads = 0;
    r.shiftKey(Key::Clear); ok(r.sv.slotReads <= 2, "and so does its undo");
    delete p;
    delete q;
}

// CLEAR on a PROJECT slot asks, and again deletes; one undo writes it back
static void testDeleteSlot() {
    Rig r;
    r.key(Key::Info);
    r.turn(4, 2);
    r.key(Key::Clear); ok(has(r.toast(), "SLOT 3 IS EMPTY") && r.ui->deleteAsked() == -1, "an empty slot has nothing to delete: " + r.toast());
    const int bpm = r.pj->bpm;
    r.shiftKey(Key::Ok);
    const std::vector<uint8_t> saved = r.sv.card[2];
    r.pj->patterns[0].lanes[0].root = 31;   // the project in memory, ahead of the card

    r.key(Key::Clear);
    ok(r.ui->deleteAsked() == 2 && has(r.toast(), "AGAIN DELETES PROJECT 3") && r.sv.card[2] == saved, "the first press only asks: " + r.toast());
    r.key(Key::Clear); ok(r.sv.card[2] == saved && r.ui->deleteAsked() == 2, "a bounce of that press is not the second one");
    r.key(Key::Down); ok(r.ui->deleteAsked() == -1 && r.ui->slot() == 3, "moving on takes the question back");
    r.key(Key::Up);
    r.key(Key::Clear); r.turn(0, 1); r.turn(0, -1); ok(r.ui->deleteAsked() == -1, "so does a knob");
    r.key(Key::Clear); r.tap(0); ok(r.ui->deleteAsked() == -1, "and a layer key");
    r.key(Key::Clear); r.ui->keyDown(Key::Shift); ok(r.ui->deleteAsked() == -1, "and SHIFT"); r.ui->keyUp(Key::Shift);
    r.key(Key::Clear); r.sv.now += 3999; r.ui->tick(); ok(r.ui->deleteAsked() == 2, "it waits four seconds");
    r.sv.now += 1; r.ui->tick(); ok(r.ui->deleteAsked() == -1, "and no longer");
    r.key(Key::Clear); r.sv.now += 4000; r.ui->tick(); r.key(Key::Clear); ok(r.sv.card[2] == saved && r.ui->deleteAsked() == 2, "a press after that asks again");

    r.sv.now += 400; r.sv.deleteFails = true; r.key(Key::Clear);
    ok(r.sv.card[2] == saved && has(r.toast(), "NOT DELETED") && r.ui->loadedSlot() == 2, "a card that refuses says so, and the project stays: " + r.toast());
    r.sv.deleteFails = false;
    r.key(Key::Clear); r.sv.now += 4001; r.key(Key::Clear); ok(r.sv.card[2] == saved && r.ui->deleteAsked() == 2, "four seconds count without a tick too");
    r.sv.now += 4001; r.ui->tick();

    r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear);
    ok(r.sv.card[2].empty() && r.val(4) == "3 EMPTY" && has(r.toast(), "SLOT 3 DELETED") && has(r.toast(), "PROJECT 3") && has(r.toast(), "UNDOES"), "the second press deletes: " + r.toast());
    ok(r.ui->loadedSlot() == kNoSlot && r.pj->patterns[0].lanes[0].root == 31 && r.pj->bpm == bpm, "what is in memory stays, and is no longer the loaded slot");
    r.shiftKey(Key::Clear);
    { SlotInfo si; ok(r.sv.slotInfo(2, si) && si.bpm == bpm && r.ui->loadedSlot() == 2 && has(r.toast(), "UNDONE: DELETE OF SLOT 3"), "SHIFT + CLEAR writes it back: " + r.toast()); }
    ok(r.pj->patterns[0].lanes[0].root == 31, "the undo leaves the project in memory alone");
    Project* back = new Project;
    { int missing = 0; ok(r.sv.loadSlot(2, *back, missing) && back->patterns[0].lanes[0].root != 31 && back->bpm == bpm, "the project written back is the deleted one"); }

    // a slot that was not the loaded one
    r.key(Key::Down); r.shiftKey(Key::Ok); r.key(Key::Up);
    r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear); ok(r.ui->loadedSlot() == 3, "deleting another slot leaves the loaded one");
    r.shiftKey(Key::Clear); ok(r.ui->loadedSlot() == 3, "and so does its undo");

    // the undo fails: it stays
    r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear);
    r.sv.saveFails = true; r.shiftKey(Key::Clear); ok(r.sv.card[2].empty() && has(r.toast(), "NOT UNDONE"), "an undo the card refuses says so: " + r.toast());
    r.sv.saveFails = false; r.shiftKey(Key::Clear); ok(!r.sv.card[2].empty() && has(r.toast(), "UNDONE: DELETE"), "and can be tried again: " + r.toast());

    // a refused delete leaves the undo that was waiting: slot 4's project must not land in slot 3
    const std::vector<uint8_t> three = r.sv.card[2], four = r.sv.card[3];
    r.pj->bpm = 141; r.key(Key::Down); r.shiftKey(Key::Ok); r.key(Key::Up); r.pj->bpm = bpm;
    const std::vector<uint8_t> four2 = r.sv.card[3];
    r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear);
    r.key(Key::Down); r.sv.deleteFails = true; r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear); r.sv.deleteFails = false;
    ok(r.sv.card[3] == four2 && has(r.toast(), "NOT DELETED"), "a second delete is refused");
    r.shiftKey(Key::Clear);
    { int missing = 0; ok(r.sv.loadSlot(2, *back, missing) && back->bpm == bpm && has(r.toast(), "DELETE OF SLOT 3"), "the undo still writes slot 3's own project back: " + r.toast()); }

    // a slot that fails to load leaves a deleted project's copy
    r.key(Key::Up); r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear);
    std::vector<uint8_t> bad = four2; bad[200] ^= 0xFF; r.sv.card[3] = bad; r.ui->refreshCard();
    r.key(Key::Down); r.key(Key::Ok); ok(has(r.toast(), "COULD NOT BE READ"), "a slot that cannot be read says so");
    r.shiftKey(Key::Clear);
    { int missing = 0; ok(r.sv.loadSlot(2, *back, missing) && back->bpm == bpm, "and the undo still has the deleted project"); }

    // a slot that cannot be read: one press more, then it goes for good, and the undo waiting from before stays
    r.key(Key::Up); r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear);   // (waiting: slot 3)
    r.key(Key::Down); r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear);
    ok(r.sv.card[3] == bad && has(r.toast(), "CANNOT BE READ") && has(r.toast(), "FOR GOOD"), "it says there is no way back first: " + r.toast());
    r.sv.now += 400; r.key(Key::Clear);
    ok(r.sv.card[3].empty() && has(r.toast(), "SLOT 4 DELETED") && has(r.toast(), "NO UNDO"), "and goes on the next press: " + r.toast());
    r.shiftKey(Key::Clear);
    { int missing = 0; ok(r.sv.loadSlot(2, *back, missing) && back->bpm == bpm, "the undo from before it still works"); }

    // an undo of a save to the slot that went cannot be left waiting
    r.sv.card[3] = four; r.shiftKey(Key::Ok); r.sv.card[3][200] ^= 0xFF; r.ui->refreshCard();
    r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear);
    r.shiftKey(Key::Clear); ok(has(r.toast(), "NOTHING TO UNDO"), "the save to that slot is no longer undone: " + r.toast());

    // what the toast has to say
    r.key(Key::Up); r.sv.peekMissing = 2; r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear); r.sv.peekMissing = 0;
    ok(has(r.toast(), "UNDO: 2 SAMPLES LOST") && has(r.toast(), "UNDOES"), "an undo that loses samples says so first: " + r.toast());
    r.shiftKey(Key::Clear);
    r.sv.fellBack = true; r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear); r.sv.fellBack = false;
    ok(has(r.toast(), "UNDO: OLDER SAVE") && has(r.toast(), "UNDOES"), "so does one that brings the save before: " + r.toast());
    r.shiftKey(Key::Clear);

    // both, with the longest name a slot can have: the way back still fits on the screen
    { memset(r.pj->name, 0, sizeof r.pj->name); strcpy(r.pj->name, "ABCDEFGHIJKLMNO"); r.sv.saveSlot(2, *r.pj); r.ui->refreshCard(); }
    r.sv.fellBack = true; r.sv.peekMissing = 12; r.key(Key::Clear); r.sv.now += 400; r.key(Key::Clear); r.sv.fellBack = false; r.sv.peekMissing = 0;
    ok(has(r.toast(), "OLDER SAVE, 12 SAMPLES LOST") && has(r.toast(), "SHIFT + \xE2\x97\x8B UNDOES"), "and both together: " + r.toast());
    r.sv.now += 5; r.key(Key::Clear); ok(has(r.toast(), "12 SAMPLES LOST"), "a bounce of that press leaves the message");
    r.shiftKey(Key::Clear);
    delete back;
    {   // a card that has gone: nothing was deleted, whatever it took to find out
        Rig c;
        c.key(Key::Info);
        c.shiftKey(Key::Ok);
        c.sv.pulled = true;
        c.key(Key::Clear); c.sv.now += 400; c.key(Key::Clear); c.sv.now += 400; c.key(Key::Clear);
        ok(!has(c.toast(), "SLOT 1 DELETED") && has(c.toast(), "NOT DELETED") && c.ui->loadedSlot() == 0, "a pulled card is not a delete: " + c.toast());
        c.sv.pulled = false;
        c.ui->refreshCard();
        ok(!c.sv.card[0].empty(), "and the project is there when it comes back");
    }
    {   // an undo never leaves LOADED on a slot that has gone
        Project* b = new Project;
        Rig c;
        c.key(Key::Info);
        c.pj->bpm = 100; c.shiftKey(Key::Ok);
        c.key(Key::Down); c.pj->bpm = 110; c.shiftKey(Key::Ok); c.key(Key::Up);
        c.key(Key::Clear); c.sv.now += 400; c.key(Key::Clear); c.sv.now += 400;            // slot 1 goes, and slot 2 is the loaded one
        c.key(Key::Down); c.sv.card[1][200] ^= 0xFF; c.ui->refreshCard();
        c.key(Key::Clear); c.sv.now += 400; c.key(Key::Clear); c.sv.now += 400; c.key(Key::Clear); c.sv.now += 400;   // and slot 2, unreadable
        c.shiftKey(Key::Clear);
        ok(c.sv.card[1].empty() && c.ui->loadedSlot() == kNoSlot, "the undo of slot 1 leaves no LOADED on slot 2");
        c.shiftKey(Key::Ok); c.shiftKey(Key::Clear); ok(c.sv.card[1].empty() && c.ui->loadedSlot() == kNoSlot, "nor does the undo of a save to an empty slot");
        // a load that fails costs nothing, whichever undo is waiting
        c.key(Key::Up); c.pj->bpm = 120; c.shiftKey(Key::Ok);
        c.pj->bpm = 77; c.key(Key::Ok); ok(c.pj->bpm == 120, "loaded");
        c.sv.readFails = true; c.key(Key::Ok); c.sv.readFails = false; ok(has(c.toast(), "COULD NOT BE READ") && c.pj->bpm == 120, "a read that fails leaves memory");
        c.shiftKey(Key::Clear); ok(c.pj->bpm == 77, "and the load's undo");
        c.key(Key::Clear); c.sv.now += 400; c.key(Key::Clear); c.sv.now += 400;            // slot 1 goes, its copy waiting
        c.sv.saveSlot(2, *c.pj); c.ui->refreshCard();
        c.pj->bpm = 133;
        c.key(Key::Down); c.key(Key::Down); c.sv.readFails = true; c.key(Key::Ok); c.sv.readFails = false;
        ok(has(c.toast(), "COULD NOT BE READ"), "a slot that cannot be read says so");
        c.shiftKey(Key::Clear);
        { int m = 0; ok(c.sv.loadSlot(0, *b, m) && b->bpm == 120, "and the deleted project's copy is still the deleted project"); }
        delete b;
    }

    // the MIDI view has no slots, and CLEAR takes nothing from the lanes behind this screen
    const Lane before = r.pj->patterns[0].lanes[r.ui->sel()];
    r.key(Key::Clear); ok(memcmp(&before, &r.pj->patterns[0].lanes[r.ui->sel()], sizeof before) == 0, "CLEAR on PROJECT leaves the lane");
    r.key(Key::Info); r.key(Key::Clear); ok(has(r.toast(), "DELETING IS IN PROJECT 1/3") && r.ui->deleteAsked() == -1, "the MIDI view says where: " + r.toast());
}

static void testSavePreset() {
    DirPresets& dp = shippedPresets();
    presetsOpen(dp);
    const int count0 = presetCount();
    Rig r;
    loadInBrowse(r, 10);
    const int part = r.ui->part();
    ok(r.pj->parts[part].preset == 10, "a library preset is on the part");
    r.pj->parts[part].pages[PG_FLT1][1] = 33;   // the sound is edited after the load
    r.key(Key::Flt1);
    ok(r.ui->mode() == Mode::Sound, "on the FILTER 1 page");
    r.shiftKey(Key::Ok);
    ok(r.ui->mode() == Mode::Save, "SHIFT + OK on a sound page opens SAVE PRESET");
    ok(std::string(r.ui->saveName(), 11) == "Acid Roll 2" && r.ui->saveCategory() == 1, std::string("the name starts from the loaded preset's: '") + r.ui->saveName() + "'");
    ok(r.label(0) == "CURSOR" && r.label(1) == "CHARACTER" && r.val(1) == "A" && r.val(2) == "BASS" && r.val(3) == "NEW" && r.val(4) == "1000", "the knobs: cursor, character, category, mode, id: " + r.val(4));
    r.turn(1, 1);
    ok(r.val(1) == "B", "knob 2 changes the character");
    r.key(Key::Right);
    r.turn(2, 1);
    ok(r.val(0) == "2/20" && r.val(2) == "LEAD", "the arrow moves the cursor, knob 3 the category: " + r.val(0) + " " + r.val(2));
    r.turn(2, -1);
    // nothing else reaches the project while the editor is open
    const Project snap = *r.pj;
    r.key(Key::Clear); r.key(Key::Copy); r.key(Key::Add); r.tap(2);
    ok(memcmp(&snap, r.pj, sizeof(Project)) == 0 && r.ui->mode() == Mode::Save, "other keys do nothing in SAVE PRESET");
    const int writes0 = dp.fileWrites;
    r.key(Key::Ok);
    ok(r.ui->mode() == Mode::Sound && has(r.toast(), "SAVED") && has(r.toast(), "ID 1000"), "OK saves and returns: " + r.toast());
    ok(dp.fileWrites == writes0 + 1 && presetCount() == count0 + 1 && presetExists(1000) && std::string(presetName(1000)) == "Bcid Roll 2", "one file, on the list at once: " + std::string(presetName(1000)));
    ok(r.pj->parts[part].preset == 1000 && r.ui->browseLoaded(part) == 1000, "the part now shows the new preset");
    {   // the sound that was saved is the sound on the part
        int16_t pg[kSynthPages][kParamsPerPage];
        defaultSynthPages(pg);
        ok(presetLoad(1000) && applyPresetIndex(pg, 1000), "the new preset loads");
        bool same = true;
        for (int p = 0; p < kSynthPages; ++p) for (int k = 0; k < kParamsPerPage; ++k) same = same && pg[p][k] == r.pj->parts[part].pages[p][k];
        ok(same && pg[PG_FLT1][1] == 33, "all 72 knob values come back as they were saved");
    }
    {   // the next start finds it without reading the files (the index was brought up to date)
        const int reads = dp.fileReads;
        const PresetOpen o = presetsOpen(dp);
        ok(!o.rebuilt && o.count == presetCount() - kBasePresets && dp.fileReads == reads && presetExists(1000), "the index already has it: the next start reads no preset file");
    }
    // replace it
    r.shiftKey(Key::Ok);
    ok(r.ui->mode() == Mode::Save && r.ui->saveSource() == 1000 && r.val(3) == "NEW", "SHIFT + OK again: the source is the preset just saved");
    r.turn(3, 1);
    ok(r.val(3) == "REPLACE" && r.val(4) == "1000" && std::string(r.ui->saveName(), 10) == "Bcid Roll " , "knob 4 switches to REPLACE, the id is the preset's own");
    r.pj->parts[part].pages[PG_FLT1][1] = 44;
    const int writes1 = dp.fileWrites;
    r.key(Key::Ok);
    ok(r.ui->mode() == Mode::Save && dp.fileWrites == writes1 && has(r.toast(), "PRESS OK AGAIN"), "a replace asks for OK again: " + r.toast());
    r.turn(1, 1);   // an edit disarms it
    r.key(Key::Ok);
    ok(r.ui->mode() == Mode::Save && dp.fileWrites == writes1, "an edit in between asks again");
    r.key(Key::Ok);
    ok(r.ui->mode() == Mode::Sound && has(r.toast(), "OVER ID 1000") && dp.fileWrites == writes1 + 1 && presetCount() == count0 + 1, "the second OK replaces it: " + r.toast());
    ok(std::string(presetName(1000)) == "Ccid Roll 2", std::string("under its new name: ") + presetName(1000));
    {
        int16_t pg[kSynthPages][kParamsPerPage];
        defaultSynthPages(pg);
        ok(presetLoad(1000) && applyPresetIndex(pg, 1000) && pg[PG_FLT1][1] == 44, "and the file holds the new sound, not the cached old one");
        int files = 0;
        DIR* d = opendir(dp.dir.c_str());
        while (dirent* e = readdir(d)) files += strstr(e->d_name, "1000") != nullptr;
        closedir(d);
        ok(files == 1, "the file was rewritten, no second one: " + std::to_string(files));
    }
    // an OK right behind the SHIFT + OK that opened it (a bounce) does not save
    r.shiftKey(Key::Ok);
    const int writesB = dp.fileWrites;
    r.key(Key::Ok);
    ok(r.ui->mode() == Mode::Save && dp.fileWrites == writesB, "an OK at once after opening does not save");
    r.key(Key::Lane);
    // cancel
    r.shiftKey(Key::Ok);
    const int writes2 = dp.fileWrites;
    r.key(Key::Lane);
    ok(r.ui->mode() == Mode::Sound && dp.fileWrites == writes2 && has(r.toast(), "NOT SAVED"), "L cancels: " + r.toast());
    // a name must start with a character
    r.shiftKey(Key::Ok);
    r.turn(1, -1); r.turn(1, -1);   // the first character: C -> B -> A... down to space
    for (int i = 0; i < 4 && r.val(1) != "SPACE"; ++i) r.turn(1, -1);
    ok(r.val(1) == "SPACE", "the character wraps down to SPACE");
    r.key(Key::Ok);
    ok(r.ui->mode() == Mode::Save && has(r.toast(), "MUST START") && dp.fileWrites == writes2, "a name starting with a space is refused: " + r.toast());
    r.key(Key::Lane);
    // an open step lock: not now
    {
        r.key(Key::Flt1);
        r.key(Key::StepK);
        r.ui->layerDown(2); r.sv.now += 400; r.ui->tick(); r.ui->layerUp(2);   // hold a step: its lock opens
        r.key(Key::Flt1);
        if (r.ui->lockOpen()) {
            r.shiftKey(Key::Ok);
            ok(r.ui->mode() != Mode::Save && has(r.toast(), "CLOSE THE STEP LOCK"), "a step lock that is open: " + r.toast());
        }
        r.home();
    }
    // a factory preset can only be saved as new
    loadInBrowse(r, 2);
    if (r.ui->mode() != Mode::Sound) r.key(Key::Flt1);
    r.shiftKey(Key::Ok);
    ok(r.ui->mode() == Mode::Save && r.ui->saveSource() == -1 && r.cell(3).dis && std::string(r.ui->saveName(), 17) == "Dimension Bloom 2", "a factory preset: the mode is NEW only: '" + std::string(r.ui->saveName()) + "'");
    r.turn(3, 1);
    ok(r.val(3) == "NEW" && has(r.toast(), "NOTHING TO REPLACE"), "turning it says why");
    r.sv.now += 500;
    r.key(Key::Ok);
    ok(has(r.toast(), "ID 1001") && presetExists(1001), "the next new preset takes the next id: " + r.toast());
    // a card that cannot be written; no card
    r.shiftKey(Key::Ok);
    r.sv.now += 500;
    dp.failWrites = true;
    const int n = presetCount();
    r.key(Key::Ok);
    ok(r.ui->mode() == Mode::Save && has(r.toast(), "COULD NOT BE WRITTEN") && presetCount() == n, "a write that fails says so and changes nothing: " + r.toast());
    dp.failWrites = false;
    presetsForget();
    r.key(Key::Ok);
    ok(r.ui->mode() == Mode::Save && has(r.toast(), "NO PRESET FOLDER"), "no card: " + r.toast());
    r.key(Key::Lane);
    presetsOpen(dp);
    {   // a save from BROWSE comes back with the cursor on the new preset
        Rig b;
        loadInBrowse(b, 11);
        b.key(Key::File);
        b.shiftKey(Key::Ok);
        b.sv.now += 500;
        b.turn(1, 1);
        b.key(Key::Ok);
        ok(b.ui->mode() == Mode::Browse && b.ui->browseItem(b.ui->browseIdx()) == b.ui->project().parts[b.ui->part()].preset, "saved from BROWSE: the cursor is on the saved preset");
    }
    // a sampler part has nothing to save
    r.key(Key::Spare);   // (leave any page)
    r.home();
    r.tap(8);
    r.key(Key::File);
    r.shiftKey(Key::Ok);
    ok(r.ui->mode() == Mode::Browse && has(r.toast(), "SYNTH PARTS"), "a sampler part: " + r.toast());
}

// A preset's effects: its sends and amp, the shared settings, load and undo.
static void testPresetFx() {
    // find presets: one with a delay and a reverb send, one with nothing
    int rich = -1, plain = -1;
    FxSettings probe;
    for (int k = kBasePresets; k < presetCount(); ++k) {
        const int i = presetIdAt(k);
        presetLoad(i);
        defaultFx(probe);
        applyPresetFx(probe, 0, i);
        if (rich < 0 && probe.send[0][0] > 0 && probe.send[0][1] > 0) rich = i;
        if (plain < 0 && probe.send[0][0] == 0 && probe.send[0][1] == 0 && probe.send[0][2] == 0 && probe.amp[0][0] == 0) plain = i;
    }
    ok(rich >= 0 && plain >= 0, "the library has presets with and without effects");
    if (rich < 0 || plain < 0) return;
    presetLoad(rich);
    presetLoad(plain);
    FxSettings f;
    defaultFx(f);
    applyPresetFx(f, 1, rich);
    ok(f.send[1][0] > 0 && f.send[1][1] > 0 && f.send[0][0] == 0 && f.send[2][0] == 0, "a preset sets its part's sends and no other part's");
    {
        FxSettings g;
        defaultFx(g);
        ok(applyPresetFx(g, kParts - 1, rich) && g.send[kParts - 1][0] > 0 && g.send[2][0] == 0, "and the last part gets its preset's effects too");
    }
    ok(f.delay[1] != fxDefault(FX_DELAY, 1) || f.delay[2] != fxDefault(FX_DELAY, 2) || f.reverb[1] != fxDefault(FX_REVERB, 1), "and the shared effects take its character");
    const int16_t rv[8] = {f.reverb[0], f.reverb[1], f.reverb[2], f.reverb[3], f.reverb[4], f.reverb[5], f.reverb[6], f.reverb[7]};
    // another part is using the reverb: a preset on part 0 leaves the shared reverb alone
    FxSettings g;
    defaultFx(g);
    g.send[2][1] = 40;
    g.reverb[1] = 3;
    applyPresetFx(g, 0, rich);
    ok(g.reverb[1] == 3 && g.send[0][1] > 0, "a shared effect another part sends to is not overwritten (the send is still set)");
    g.send[2][1] = 0;
    applyPresetFx(g, 0, rich);
    ok(g.reverb[1] != 3, "when no other part uses it, it takes the preset's");
    (void)rv;
    // loading a preset without effects clears the part's sends and amp
    applyPresetFx(f, 1, plain);
    bool clear = true;
    for (int k = 0; k < kSendsUsed; ++k) clear = clear && f.send[1][k] == 0;
    ok(clear && f.amp[1][0] == 0, "a preset without effects leaves the part dry, its amp off");
    ok(f.send[1][3] == 0, "the spare send stays 0");

    // through the UI: BROWSE, OK, undo
    Rig r;
    r.key(Key::File);
    int row = -1;
    for (int i = 0; i < r.ui->browseCount(); ++i) if (r.ui->browseItem(i) == rich) row = i;
    ok(row >= 0, "the preset is in the BROWSE list");
    while (r.ui->browseIdx() < row) r.key(Key::Down);
    const FxSettings before = r.pj->fx;
    const int part = r.ui->part();
    r.key(Key::Ok);
    ok(r.pj->parts[part].preset == rich && r.pj->fx.send[part][0] > 0 && r.pj->fx.send[part][1] > 0, "OK loads the preset and its sends");
    ok(r.sv.holds > 0 && r.sv.held == 0, "the sends and the amp change with the audio held, and let go");
    r.shiftKey(Key::Clear);
    ok(memcmp(&before, &r.pj->fx, sizeof(before)) == 0, "undo puts the effects back exactly");

    // a preset without an amp leaves the AMP view at its real defaults; every preset's effects are inside the knob ranges
    {
        int offDefaults = 0, offTotal = 0, ranges = 0;
        for (int k = kBasePresets; k < presetCount(); ++k) {
            const int i = presetIdAt(k);
            presetLoad(i);
            FxSettings x;
            defaultFx(x);
            applyPresetFx(x, 0, i);
            FxSettings y = x;
            sanitizeFx(y);
            ranges += memcmp(&x, &y, sizeof(x)) != 0;
            if (x.amp[0][0] == 0) {
                ++offTotal;
                bool same = true;
                for (int k = 0; k < kFxParams; ++k) same = same && x.amp[0][k] == fxDefault(FX_AMP, k);
                offDefaults += same;
            }
        }
        ok(ranges == 0, "every preset's effects are inside the knob ranges (sanitize leaves them as they are): " + std::to_string(ranges));
        ok(offTotal > 50 && offDefaults == offTotal, "a preset without an amp keeps the amp knobs at their defaults: " + std::to_string(offDefaults) + " of " + std::to_string(offTotal));
    }
    // undo gives back what the load changed, and keeps what was edited after it
    {
        Rig q;
        q.key(Key::File);
        int row2 = -1;
        for (int i = 0; i < q.ui->browseCount(); ++i) if (q.ui->browseItem(i) == rich) row2 = i;
        while (q.ui->browseIdx() < row2) q.key(Key::Down);
        const int part2 = q.ui->part();
        q.pj->fx.comp[0] = 1;                     // before the load: on, and not the preset's business
        q.pj->fx.send[(part2 + 1) % 3][2] = 33;   // another part's send
        const FxSettings pre = q.pj->fx;
        q.key(Key::Ok);
        const FxSettings post = q.pj->fx;
        ok(post.comp[0] == 1 && post.send[(part2 + 1) % 3][2] == 33, "a load leaves the compressor and the other parts' sends alone");
        q.pj->fx.reverb[1] = (int16_t)(post.reverb[1] == 77 ? 78 : 77);   // an edit after the load (the FX screen)
        q.pj->fx.send[(part2 + 2) % 3][0] = 9;
        q.shiftKey(Key::Clear);
        ok(q.pj->fx.send[part2][0] == pre.send[part2][0] && q.pj->fx.send[part2][1] == pre.send[part2][1] && q.pj->fx.amp[part2][0] == pre.amp[part2][0],
           "undo gives back the part's sends and amp");
        ok(q.pj->fx.reverb[1] != pre.reverb[1] && q.pj->fx.send[(part2 + 2) % 3][0] == 9, "but keeps the shared reverb and the other part's send as edited since");
        ok(q.pj->fx.comp[0] == 1 && q.pj->fx.send[(part2 + 1) % 3][2] == 33, "and the compressor and the other part's send");
    }
    // a preset whose file cannot be read: a toast, and nothing changes
    {
        DirPresets& dp = shippedPresets();
        presetsOpen(dp);          // (empties the cache)
        dp.failFiles = true;
        Rig q;
        q.key(Key::File);
        int row3 = -1;
        for (int i = 0; i < q.ui->browseCount(); ++i) if (q.ui->browseItem(i) == rich) row3 = i;
        while (q.ui->browseIdx() < row3) q.key(Key::Down);
        const Project before = *q.pj;
        q.key(Key::Ok);
        const char* tt = q.ui->toast();
        ok(tt && strstr(tt, "PRESET FILE"), std::string("an unreadable preset file says so: ") + (tt ? tt : "(no toast)"));
        ok(memcmp(&before.parts[q.ui->part()], &q.pj->parts[q.ui->part()], sizeof(Part)) == 0 && memcmp(&before.fx, &q.pj->fx, sizeof(FxSettings)) == 0 && q.ui->mode() == Mode::Browse,
           "and the part and the effects are as they were, BROWSE stays open");
        dp.failFiles = false;
        presetsOpen(dp);
        q.key(Key::Ok);
        ok(q.pj->parts[q.ui->part()].preset == rich, "the same preset loads once the card reads again");
    }
}

// ---- the tracker: STEP's third view (docs/TRACKER.md)
static std::vector<uint16_t> frameOf(Rig& r) {
    static uint8_t arena[64 * 1024];   // the board's size (main.cpp frameArena)
    static uint16_t strip[gfx::kWidth * 20];
    gfx::Canvas cv(arena, sizeof(arena));
    gfx::drawScreen(cv, *r.ui);
    ok(!cv.overflowed(), "the frame fits the board's display list");
    std::vector<uint16_t> fb((size_t)gfx::kWidth * gfx::kHeight);
    for (int y = 0; y < gfx::kHeight; y += 20) {
        const int rows = gfx::kHeight - y < 20 ? gfx::kHeight - y : 20;
        cv.raster(strip, y, rows);
        memcpy(&fb[(size_t)y * gfx::kWidth], strip, sizeof(uint16_t) * gfx::kWidth * rows);
    }
    return fb;
}
// What board/Lcd.cpp sends: the 32 x 20 tiles that changed since the last frame.
static int tilesChanged(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    int n = 0;
    for (int ty = 0; ty < gfx::kHeight / 20; ++ty)
        for (int tx = 0; tx < gfx::kWidth / 32; ++tx) {
            bool same = true;
            for (int y = 0; y < 20 && same; ++y) same = !memcmp(&a[(size_t)(ty * 20 + y) * gfx::kWidth + tx * 32], &b[(size_t)(ty * 20 + y) * gfx::kWidth + tx * 32], 32 * sizeof(uint16_t));
            n += !same;
        }
    return n;
}
static void toTracker(Rig& r) { for (int i = 0; i < 3 && !r.ui->tracker(); ++i) r.key(Key::StepK); }
// an empty pattern on screen: every lane 16 steps with no rhythm, so a test says what is on it
static void emptied(Rig& r) { for (int i = 0; i < kLanes; ++i) { Lane& l = r.lane(i); l.eu = 0; l.n = 16; l.k = 0; l.r = 0; for (Step& s : l.st) s.ov = 0; } }
static void okHold(Rig& r, Key k, int n = 1) { r.ui->keyDown(Key::Ok); for (int i = 0; i < n; ++i) r.key(k); r.ui->keyUp(Key::Ok); }
static bool stepOn(Rig& r, int lane, int i) { return trig(r.lane(lane), i, false); }

static void testTrackerViews() {
    {   // STEP: the third press, and the other two as they were
        Rig r;
        r.key(Key::StepK);
        ok(r.ui->mode() == Mode::Step && r.ui->stepSub() == 0 && !r.ui->tracker(), "STEP once: the step editor");
        ok(r.labels(4) == "NOTE,VELOCITY,GATE,PROB", "and its knobs: " + r.labels(4));
        r.key(Key::StepK);
        ok(r.ui->stepSub() == 1 && has(r.toast(), "2/3") && r.labels(4) == "CHORD,INVERSION,VOICING,STRUM", "again: the second view: " + r.toast());
        ok(r.label(0) == "ARP", "with the arp on top");
        r.key(Key::StepK);
        ok(r.ui->tracker() && has(r.toast(), "TRACKER"), "again: the tracker: " + r.toast());
        ok(r.labels(4) == "NOTE,OCTAVE,CHORD,INVERSION" && r.labels(0) == "VELOCITY,GATE,PROB,IN SCALE", "its knobs: " + r.labels(4) + " " + r.labels(0));
        r.key(Key::StepK);
        ok(r.ui->stepSub() == 0 && !r.ui->tracker() && has(r.toast(), "1/3"), "and round to the editor");
        r.key(Key::StepK);
        r.key(Key::StepK);
        r.key(Key::Lane);
        ok(r.ui->mode() == Mode::Play, "L goes back from the tracker");
        r.key(Key::StepK);
        ok(r.ui->tracker(), "and STEP comes back to the view it was on, as it does to the second");
        Rig d;
        d.tap(7);
        d.key(Key::StepK);
        d.key(Key::StepK);
        ok(d.ui->stepSub() == 1 && has(d.toast(), "SAMPLE PER STEP"), "a sampler lane's second view is still there");
    }
    {   // only a view: nothing in the project moves until a step is edited
        Rig r;
        Project* before = new Project;
        *before = *r.pj;
        toTracker(r);
        for (int i = 0; i < 40; ++i) { r.key(Key::Down); r.key(Key::Right); }
        r.shiftKey(Key::Down);
        r.tap(4);
        ok(!memcmp(before, r.pj, sizeof(Project)), "looking around changes nothing in the project");
        delete before;
    }
    {   // the cursor's step is the editor's: the views stay on one step
        Rig r;
        emptied(r);
        r.key(Key::StepK);
        for (int i = 0; i < 10; ++i) r.key(Key::Right);
        r.key(Key::StepK);
        r.key(Key::StepK);
        ok(r.ui->trackerRow() == 10 && r.ui->cursor() == 10, "the tracker opens on the editor's step");
        r.key(Key::Down);
        r.key(Key::StepK);
        ok(r.ui->cursor() == 11 && r.ui->stepPage() == 1, "and the editor comes back on the tracker's, on its page");
    }
    {   // the layer keys are lanes, all ten: 9 and 10 are not pages here
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Down);
        for (int i : {8, 9, 2}) {
            r.tap(i);
            ok(r.ui->sel() == i && r.ui->trackerRow() == 1 && r.ui->stepPage() == 0, "a layer key picks the lane, on the same row: " + std::to_string(i));
            ok(!stepOn(r, i, 1), "and taps no step");
        }
        ok(r.ui->mode() == Mode::Step && r.ui->tracker(), "and stays in the view");
    }
    {   // the note as it is printed
        Text t;
        trackerNoteName(t, 60); ok(!strcmp(t.c_str(), "C-4"), "C-4 is middle C, as STEP's C4");
        t.clear(); trackerNoteName(t, 61); ok(!strcmp(t.c_str(), "C#4"), "a sharp");
        t.clear(); trackerNoteName(t, 12); ok(!strcmp(t.c_str(), "C-0"), "octave 0");
        t.clear(); trackerNoteName(t, 11); ok(!strcmp(t.c_str(), "B_1"), "below it: " + std::string(t.c_str()));
        t.clear(); trackerNoteName(t, 1); ok(!strcmp(t.c_str(), "C#_1"), "a sharp below it");
    }
}

static void testTrackerCursor() {
    {   // the longest lane sets the rows; arrows go round, with the page following
        Rig r;
        emptied(r);
        r.lane(4).n = 32;
        toTracker(r);
        ok(r.ui->trackerRows() == 32, "32 rows");
        for (int i = 0; i < 15; ++i) r.key(Key::Down);
        ok(r.ui->trackerRow() == 15, "the last row of page 1");
        r.key(Key::Down);
        ok(r.ui->trackerRow() == 16 && r.ui->trackerRow() / kTrackerRows == 1, "down from it: the first row of page 2");
        r.key(Key::Up);
        ok(r.ui->trackerRow() == 15, "and back up over the edge");
        r.shiftKey(Key::Down);
        ok(r.ui->trackerRow() == 31, "SHIFT + down: the same place on the other page");
        r.key(Key::Down);
        ok(r.ui->trackerRow() == 0, "down from the last row goes round");
        r.key(Key::Up);
        ok(r.ui->trackerRow() == 31, "up from the first row goes round");
        r.shiftKey(Key::Up);
        ok(r.ui->trackerRow() == 15, "SHIFT + up");
        r.shiftKey(Key::Up);
        ok(r.ui->trackerRow() == 31, "SHIFT + up goes round too");
    }
    {   // a page not full: SHIFT lands on the last row there is
        Rig r;
        emptied(r);
        r.lane(0).n = 20;
        toTracker(r);
        for (int i = 0; i < 10; ++i) r.key(Key::Down);
        r.shiftKey(Key::Down);
        ok(r.ui->trackerRow() == 19, "row 11 on to a page of four rows: its last");
        r.key(Key::Down);
        ok(r.ui->trackerRow() == 0, "and round");
    }
    {   // one page: SHIFT has nowhere to go
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Down);
        r.shiftKey(Key::Down);
        ok(r.ui->trackerRow() == 1, "16 rows: SHIFT + down stays");
    }
    {   // lanes go round, the row stays
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Left);
        ok(r.ui->sel() == 9, "left from lane 1 goes round to lane 10");
        r.key(Key::Right);
        ok(r.ui->sel() == 0, "right from lane 10 goes round to lane 1");
    }
    {   // every lane one step: one row, and a key that moves nothing is harmless
        Rig r;
        for (int i = 0; i < kLanes; ++i) { r.lane(i).n = 1; r.lane(i).k = 0; r.lane(i).r = 0; r.lane(i).eu = 0; r.lane(i).st[0].ov = 0; }
        toTracker(r);
        ok(r.ui->trackerRows() == 1, "one row");
        r.key(Key::Down); r.key(Key::Up); r.shiftKey(Key::Down);
        ok(r.ui->trackerRow() == 0 && r.ui->cursor() == 0, "the arrows stay on it");
        r.key(Key::Ok);
        ok(stepOn(r, 0, 0) && r.ui->popover(), "and it takes a note");
        frameOf(r);
    }
    {   // a 1-step lane beside a long one: every row of it is step 1
        Rig r;
        emptied(r);
        r.lane(1).n = 1;
        r.lane(0).n = 32;
        toTracker(r);
        r.key(Key::Right);
        for (int i = 0; i < 9; ++i) r.key(Key::Down);
        ok(r.ui->trackerRow() == 9 && r.ui->cursor() == 0, "row 10 of a 1-step lane is its step");
        r.key(Key::Ok);
        ok(stepOn(r, 1, 0) && r.lane(1).st[0].ov == 1, "a note on the repeat is on the step");
        r.key(Key::Left);
        ok(r.ui->cursor() == 9, "the long lane's own step at that row");
    }
    {   // the lane under the cursor gets shorter, the pattern switches under it: the cursor stays on what exists
        Rig r;
        emptied(r);
        r.lane(0).n = 32;
        toTracker(r);
        for (int i = 0; i < 20; ++i) r.key(Key::Down);
        r.lane(0).n = 8;
        r.ui->settle();
        ok(r.ui->trackerRow() == 15 && r.ui->cursor() == 7, "rows gone: the last row left, and its step: " + std::to_string(r.ui->trackerRow()));
        r.lane(0).n = 16;
        r.tp.pat = 3;
        emptied(r);
        for (int i = 0; i < kLanes; ++i) r.lane(i).n = 4;
        r.ui->settle();
        ok(r.ui->trackerRow() == 3 && r.ui->cursor() == 3, "another pattern, shorter");
    }
}

static void testTrackerEntry() {
    {   // OK on an empty step: the lane's last note, and the step switched on as a tap does it
        Rig r;
        emptied(r);
        toTracker(r);
        r.ui->keyDown(Key::Ok);
        ok(!stepOn(r, 0, 0), "OK acts when it comes up: until then it could be a hold");
        r.ui->keyUp(Key::Ok);
        ok(stepOn(r, 0, 0) && r.lane(0).st[0].ov == 1 && r.lane(0).st[0].note == 0, "the first note on a lane is its root");
        ok(r.ui->popover(), "the popover opens by itself");
        okHold(r, Key::Right, 3);
        ok(r.lane(0).st[0].note == 3, "OK held + right: three semitones");
        r.key(Key::Down);
        r.key(Key::Ok);
        ok(stepOn(r, 0, 1) && r.lane(0).st[1].note == 3, "the next step gets the last note");
        r.key(Key::Right);
        r.key(Key::Ok);
        ok(r.lane(1).st[1].note == 0, "each lane has its own: a root of its own makes a last note of its own");
        r.key(Key::Left);
        r.key(Key::Up);
        r.key(Key::Ok);
        ok(r.ui->popover() && stepOn(r, 0, 0) && r.lane(0).st[0].note == 3, "OK on a step that has a note: the popover, and nothing changes");
        r.key(Key::Ok);
        ok(!r.ui->popover() && stepOn(r, 0, 0), "OK again closes it");
    }
    {   // OK on a filled step takes its note: the one you looked at comes next
        Rig r;
        emptied(r);
        r.lane(0).st[5].ov = 1;
        r.lane(0).st[5].note = -7;
        toTracker(r);
        for (int i = 0; i < 5; ++i) r.key(Key::Down);
        r.key(Key::Ok);
        r.key(Key::Down);
        r.key(Key::Ok);
        ok(r.lane(0).st[6].note == -7, "the note looked at is the next one: " + std::to_string(r.lane(0).st[6].note));
    }
    {   // the moves
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Ok);
        okHold(r, Key::Right); ok(r.lane(0).st[0].note == 1, "right: a semitone up");
        okHold(r, Key::Left, 2); ok(r.lane(0).st[0].note == -1, "left: a semitone down");
        okHold(r, Key::Up); ok(r.lane(0).st[0].note == 11, "up: an octave up");
        okHold(r, Key::Down); ok(r.lane(0).st[0].note == -1, "down: an octave down");
        ok(r.ui->trackerRow() == 0 && r.ui->sel() == 0, "and the cursor has stayed");
        ok(r.ui->popover(), "letting go of OK after a move is not a tap: the popover stays");
        // on an empty step a move is its note too
        r.key(Key::Down);
        okHold(r, Key::Up);
        ok(stepOn(r, 0, 1) && r.lane(0).st[1].note == 11, "OK held + up on an empty step: the last note, an octave up");
    }
    {   // Euclid: an off step gets a note the way a tap does it, a hit taken off comes back the same way
        Rig r;
        emptied(r);
        Lane& l = r.lane(0);
        l.eu = 1; l.k = 4; l.n = 16;
        toTracker(r);
        ok(baseHit(l, 0, false) && !baseHit(l, 1, false), "four on the floor");
        r.key(Key::Down);
        r.key(Key::Ok);
        ok(l.st[1].ov == 1 && trig(l, 1, false), "off in the rhythm, now on: Step.ov, as a tap");
        ok(baseHit(l, 0, false) && !l.st[0].ov, "the rhythm itself has not moved");
        r.key(Key::Up);
        r.key(Key::Clear);
        ok(l.st[0].ov == 1 && !trig(l, 0, false), "CLEAR on a hit of the rhythm takes it off, as a tap");
        r.key(Key::Ok);
        ok(l.st[0].ov == 0 && trig(l, 0, false), "and OK puts it back");
    }
    {   // the rhythm's repeats in a shorter lane: the same step, in step with the longest
        Rig r;
        emptied(r);
        r.lane(0).n = 32;
        Lane& s = r.lane(1);
        s.n = 5; s.eu = 1; s.k = 2;
        toTracker(r);
        r.key(Key::Right);
        for (int i = 0; i < 12; ++i) r.key(Key::Down);
        ok(r.ui->trackerRow() == 12 && r.ui->cursor() == 2, "row 13 of a five-step lane is its step 3");
        const bool was = trig(s, 2, false);
        ok(!was, "off in E(2,5)");
        r.key(Key::Ok);
        ok(trig(s, 2, false), "a note on the repeat switches the step on");
        ok(s.st[2].ov == 1, "with Step.ov");
        r.key(Key::Clear);
        ok(!trig(s, 2, false), "CLEAR on a repeat removes the step itself");
        // what the row says is what plays: lanes run on the pattern's step
        emptied(r);
        r.lane(0).n = 32;
        r.lane(1).n = 5;
        r.lane(1).st[2].ov = 1;
        r.lane(0).st[0].ov = 1;
        r.tp.playing = 1;
        r.run(9000);
        std::vector<int> rows;
        for (auto& o : r.rec.ons) if (o.lane == 1) rows.push_back((int)(o.at / (uint64_t)(44100.0 * 60.0 / r.pj->bpm / 4.0)));
        ok(rows.size() >= 3 && rows[0] == 2 && rows[1] == 7 && rows[2] == 12, "the step plays on the rows it repeats on: " + (rows.size() >= 3 ? std::to_string(rows[0]) + " " + std::to_string(rows[1]) + " " + std::to_string(rows[2]) : std::string("?")));
    }
    {   // CLEAR: the whole step, and nothing on an empty one
        Rig r;
        emptied(r);
        Step& st = r.lane(0).st[0];
        st.ov = 1; st.note = 5; st.chord = 8; st.inv = 2; st.spread = 1; st.strum = 20; st.vel = 30; st.prob = 40; st.gate = 7;
        st.setLock(lockKey(PG_FLT1, 1), 90);
        toTracker(r);
        r.key(Key::Ok);
        r.key(Key::Clear);
        ok(!stepOn(r, 0, 0) && !st.chord && !st.inv && !st.spread && !st.strum && !st.nLocks && !st.note && st.vel == 100 && st.prob == 100 && st.gate == r.lane(0).gate, "CLEAR removes the step, all of it");
        ok(!r.ui->popover(), "the popover goes with it");
        ok(has(r.toast(), "STEP 1 REMOVED"), "and it says how to undo: " + r.toast());
        r.key(Key::Down);
        r.key(Key::Clear);
        ok(has(r.toast(), "EMPTY"), "CLEAR on an empty step says so: " + r.toast());
        r.sv.now += 100;
        r.key(Key::Clear);
        ok(!r.lane(0).eu && r.ui->tracker(), "and a second press does not empty the lane, here");
    }
    {   // CLEAR held is not the editor's hold: here it is pressed all the time, and it must not take the pattern
        Rig r;
        toTracker(r);
        r.ui->keyDown(Key::Clear);
        r.sv.now += 3000;
        r.ui->tick();
        r.ui->keyUp(Key::Clear);
        ok(!stepOn(r, 0, 0), "CLEAR held removes the step");
        ok(laneHasContent(r.lane(3)) && stepOn(r, 0, 4), "and nothing else: the other steps and lanes stay");
    }
    {   // what you enter is what plays
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Ok);
        okHold(r, Key::Right, 7);
        r.tp.playing = 1;
        r.run(40);
        ok(r.rec.ons.size() == 1 && r.rec.ons[0].lane == 0 && r.rec.ons[0].pitch == r.lane(0).root + 7, "the sequencer plays the note entered");
    }
}

static void testTrackerLimits() {
    {   // an octave stops at +-24: the note stays what it is
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Ok);
        okHold(r, Key::Right, 3);
        okHold(r, Key::Up);
        ok(r.lane(0).st[0].note == 15 && !r.ui->trackerStop(), "+3 up an octave: +15");
        {   // the NOTE knob says what plays, where the notes run out
            Rig h;
            emptied(h);
            h.lane(0).root = 108;
            toTracker(h);
            h.key(Key::Ok);
            okHold(h, Key::Up, 2);
            ok(h.val(4) == "+24 G9", "root 108 + 24 plays G9, the top note: " + h.val(4));
            h.lane(0).root = 12;
            h.lane(0).st[0].note = -24;
            ok(has(h.val(4), "C-1"), "root 12 - 24 plays the bottom note: " + h.val(4));
        }
        okHold(r, Key::Up);
        ok(r.lane(0).st[0].note == 15, "another would be +27: it stays, the note is not bent to fit");
        ok(r.ui->trackerStop() == 1 && r.ui->popover(), "and the popover shows it stopped");
        ok(r.cell(5).warn && r.cell(4).warn, "the knobs too");
        okHold(r, Key::Left);
        ok(r.lane(0).st[0].note == 14 && !r.ui->trackerStop(), "a move that can be made clears it");
        r.turn(5, 1);
        ok(r.lane(0).st[0].note == 14 && r.ui->trackerStop() == 1, "the OCTAVE knob stops the same way");
        r.turn(5, -1);
        r.turn(5, -1);
        ok(r.lane(0).st[0].note == -10 && !r.ui->trackerStop(), "and comes down");
        r.turn(5, -1);
        ok(r.lane(0).st[0].note == -22 && !r.ui->trackerStop(), "-22 is inside");
        r.turn(5, -1);
        ok(r.lane(0).st[0].note == -22 && r.ui->trackerStop() == -1, "-34 is not");
    }
    {   // exactly on the limit, and a semitone past it
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Ok);
        okHold(r, Key::Up, 2);
        ok(r.lane(0).st[0].note == 24 && !r.ui->trackerStop(), "+24 itself is allowed");
        okHold(r, Key::Right);
        ok(r.lane(0).st[0].note == 24 && r.ui->trackerStop() == 1, "a semitone past it stops");
        r.turn(4, 1);
        ok(r.lane(0).st[0].note == 24 && r.ui->trackerStop() == 1, "the NOTE knob stops there");
        for (int i = 0; i < 60; ++i) r.turn(4, -1);
        ok(r.lane(0).st[0].note == -24 && r.ui->trackerStop() == -1, "and at -24");
        r.ui->keyDown(Key::Shift);
        r.ui->turn(4, 1);
        ok(r.lane(0).st[0].note == -18, "SHIFT is the panel's coarse turn");
        r.ui->keyUp(Key::Shift);
        r.lane(0).st[0].note = 22;
        r.ui->turn(4, 5);
        ok(r.lane(0).st[0].note == 24 && r.ui->trackerStop() == 1, "a fast turn past the limit lands on it");
        r.key(Key::Down);
        ok(!r.ui->trackerStop(), "moving on clears it");
    }
    {   // the format holds it: a note the tracker made saves and loads
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Ok);
        okHold(r, Key::Up, 2);
        r.sv.lib = r.lib;
        ok(r.sv.saveSlot(0, *r.pj), "saved");
        Project* back = new Project;
        int missing = 0;
        ok(r.sv.loadSlot(0, *back, missing), "loaded");
        ok(back->patterns[0].lanes[0].st[0].note == 24 && back->patterns[0].lanes[0].st[0].ov == 1, "the note comes back");
        delete back;
    }
    {   // IN SCALE: off by default, offered when there is a scale
        Rig r;
        emptied(r);
        toTracker(r);
        ok(r.cell(3).dis && r.val(3) == "OFF", "no scale: nothing to stay in");
        r.turn(3, 1);
        ok(has(r.toast(), "SCALE") && !r.ui->inScale(), "turning it says where to set one: " + r.toast());
        r.pj->key = 0; r.pj->scale = 1;   // C major
        r.lane(0).root = 60;
        ok(!r.cell(3).dis && r.val(3) == "OFF" && !r.ui->inScale(), "a scale: offered, off");
        r.key(Key::Ok);
        okHold(r, Key::Right);
        ok(r.lane(0).st[0].note == 1, "off: a semitone is a semitone (the sequencer snaps what plays)");
        r.lane(0).st[0].note = 0;
        r.turn(3, 1);
        ok(r.ui->inScale() && r.val(3) == "ON", "on");
        okHold(r, Key::Right);
        ok(r.lane(0).st[0].note == 2, "C to D");
        okHold(r, Key::Right, 2);
        ok(r.lane(0).st[0].note == 5, "D, E, F: over the half step");
        r.turn(4, -1);
        ok(r.lane(0).st[0].note == 4, "the NOTE knob walks the scale too");
        r.ui->turn(4, 2);
        ok(r.lane(0).st[0].note == 7, "two detents, two notes");
        r.lane(0).st[0].note = 23;
        okHold(r, Key::Right);
        ok(r.lane(0).st[0].note == 24, "up to the limit");
        okHold(r, Key::Right);
        ok(r.lane(0).st[0].note == 24 && r.ui->trackerStop() == 1, "the limit stops it here too");
        okHold(r, Key::Down, 2);
        ok(r.lane(0).st[0].note == 0, "an octave is in the scale already");
        r.lane(0).st[0].note = 1;
        okHold(r, Key::Right);
        ok(r.lane(0).st[0].note == 2, "from outside the scale: the next note in it");
        r.lane(0).st[0].note = 1;
        okHold(r, Key::Left);
        ok(r.lane(0).st[0].note == -1, "down from C#, which plays as C: B, not the C it already plays");
        // the limit with no scale note left before it
        r.pj->scale = 11;   // minor pentatonic on C: 0 3 5 7 10
        r.lane(0).st[0].note = 22;
        okHold(r, Key::Right);
        ok(r.lane(0).st[0].note == 24, "C is in it");
        r.lane(0).root = 61;
        r.lane(0).st[0].note = 22;   // 83 = B
        okHold(r, Key::Right);
        ok(r.lane(0).st[0].note == 23 && !r.ui->trackerStop(), "C");
        okHold(r, Key::Right);
        ok(r.lane(0).st[0].note == 23 && r.ui->trackerStop() == 1, "no scale note left under the limit: stopped");
        Rig d;
        d.pj->scale = 1;
        d.tap(7);
        toTracker(d);
        ok(d.cell(3).dis && !d.ui->inScale(), "a sampler lane has no IN SCALE: the scale is for synth parts");
    }
}

static void testTrackerPopover() {
    {   // what opens it and what closes it
        Rig r;
        emptied(r);
        toTracker(r);
        ok(!r.ui->popover(), "not on arrival");
        r.key(Key::Down);
        ok(!r.ui->popover(), "not on moving: only on editing");
        for (int e : {0, 1, 2, 4, 5, 6}) {
            r.ui->settle();
            r.turn(e, 1);
            ok(r.ui->popover(), "a knob on the step opens it: " + std::to_string(e));
            r.key(Key::Down);
            ok(!r.ui->popover(), "and moving on closes it");
        }
        r.turn(6, 1);
        r.turn(7, 1);
        ok(r.ui->popover() && r.lane(0).st[r.ui->cursor()].inv == 1, "B4 on a chord opens it and sets the inversion");
        r.key(Key::Right); ok(!r.ui->popover(), "a lane to the side closes it");
        r.key(Key::Ok); r.tap(3); ok(!r.ui->popover(), "a layer key closes it");
        r.key(Key::Ok); r.key(Key::StepK); ok(!r.ui->popover(), "the next view closes it");
        r.key(Key::StepK); r.key(Key::StepK); ok(!r.ui->popover(), "and it does not come back with the view");
        r.key(Key::Ok); r.key(Key::Ok); r.key(Key::Ok); ok(r.ui->popover(), "OK brings it back");
        r.key(Key::Flt1); r.ui->tick(); ok(!r.ui->popover(), "another screen closes it");
        r.key(Key::Flt1); r.ui->settle(); ok(!r.ui->popover(), "and it is still shut on coming back");
        r.key(Key::Ok);
        ok(r.ui->popover(), "open");
        r.tp.pat = 2;
        r.ui->settle();
        ok(!r.ui->popover(), "the pattern changing under it closes it");
        r.tp.pat = 0;
    }
    {   // beside the cell: on the right while it fits, then on the left. It never covers the cell, in any place, and it is on the screen
        int right = 0, left = 0;
        bool inside = true, clear = true;
        for (int lane = 0; lane < kLanes; ++lane)
            for (int row = 0; row < kTrackerRows; ++row) {
                const gfx::Box c = gfx::trackerCell(lane, row), p = gfx::popoverBox(c);
                inside = inside && p.x >= 0 && p.y >= 87 && p.x + p.w <= gfx::kWidth && p.y + p.h <= 454;
                clear = clear && (p.x + p.w <= c.x || c.x + c.w <= p.x || p.y + p.h <= c.y || c.y + c.h <= p.y);
                (p.x > c.x ? right : left) += 1;
            }
        ok(inside, "the popover stays on the main part of the screen, for every cell");
        ok(clear, "and never covers the cell, for every cell");
        ok(right > 0 && left > 0 && right + left == kLanes * kTrackerRows, "it flips: " + std::to_string(right) + " right, " + std::to_string(left) + " left");
        ok(gfx::popoverBox(gfx::trackerCell(0, 0)).x > gfx::trackerCell(0, 0).x, "lane 1: on the right");
        ok(gfx::popoverBox(gfx::trackerCell(9, 0)).x < gfx::trackerCell(9, 0).x, "lane 10: on the left, near the edge");
    }
    {   // the same on the pixels: open at each corner, the cell under the cursor is what it was, and the popover has changed the screen
        for (int lane : {0, 5, 6, 9})
            for (int row : {0, 7, 15}) {
                Rig r;
                emptied(r);
                r.lane(lane).st[row].ov = 1;
                toTracker(r);
                r.ui->layerDown(lane); r.ui->layerUp(lane);
                for (int i = 0; i < row; ++i) r.key(Key::Down);
                r.sv.now += 2000;
                r.ui->tick();
                const std::vector<uint16_t> shut = frameOf(r);
                r.key(Key::Ok);
                r.sv.now += 2000;
                r.ui->tick();
                ok(r.ui->popover(), "open");
                const std::vector<uint16_t> open = frameOf(r);
                const gfx::Box c = gfx::trackerCell(lane, row);
                bool same = true;
                for (int y = c.y; y < c.y + c.h; ++y)
                    for (int x = c.x; x < c.x + c.w; ++x) same = same && shut[(size_t)y * gfx::kWidth + x] == open[(size_t)y * gfx::kWidth + x];
                ok(same, "the cell is not touched by the popover, lane " + std::to_string(lane) + " row " + std::to_string(row));
                ok(tilesChanged(shut, open) > 20, "and the popover is on the screen");
            }
    }
    {   // the notes it shows are the notes that play: every chord, inversion and voicing, on scales, at the limits
        int tried = 0;
        for (int scale : {0, 2, 11})
            for (int chord = 0; chord < kChordCount; ++chord)
                for (int inv = 0; inv < 4; ++inv)
                    for (int spread = 0; spread < 2; ++spread)
                        for (int note : {0, 7, -24, 24}) {
                            Rig r;
                            emptied(r);
                            r.pj->key = 9; r.pj->scale = (uint8_t)scale;
                            r.pj->parts[0].voices = 5;
                            Lane& l = r.lane(0);
                            Step& st = l.st[0];
                            st.ov = 1; st.chord = (uint8_t)chord; st.inv = (uint8_t)inv; st.spread = (uint8_t)spread; st.note = (int8_t)note; st.strum = 0;
                            int shown[kMaxChordNotes];
                            const int n = stepPitches(*r.pj, l, st, shown);
                            r.tp.playing = 1;
                            r.run(10);
                            std::vector<int> played;
                            for (auto& o : r.rec.ons) played.push_back(o.pitch);
                            bool same = (int)played.size() == n;
                            for (int i = 0; same && i < n; ++i) same = played[i] == shown[i];
                            ok(same, "the chord shown is what plays: scale " + std::to_string(scale) + " chord " + std::to_string(chord) + " inv " + std::to_string(inv));
                            ++tried;
                        }
        ok(tried == 3 * kChordCount * 4 * 2 * 4, "every chord, inversion, voicing and scale was tried: " + std::to_string(tried));
        // a sampler lane: pitch offsets, no scale, and a root that means nothing
        Rig r;
        emptied(r);
        r.pj->key = 9; r.pj->scale = 2;
        Lane& l = r.lane(7);
        l.st[0].ov = 1; l.st[0].note = 5;
        int shown[kMaxChordNotes];
        ok(stepPitches(*r.pj, l, l.st[0], shown) == 1, "one hit");
        r.tp.playing = 1;
        r.run(10);
        ok(r.rec.ons.size() == 1 && r.rec.ons[0].pitch == shown[0] && shown[0] == 5, "and it plays what is shown");
    }
    {   // B3, B4 and what the empty step offers
        Rig r;
        emptied(r);
        toTracker(r);
        ok(r.val(4) == "0 C2", "an empty step's NOTE shows what it would get: " + r.val(4));
        ok(r.cell(4).idle && r.cell(6).idle && r.cell(0).idle, "its knobs are dimmed");
        ok(r.cell(7).dis, "no chord: no inversion");
        r.turn(7, 1);
        ok(!stepOn(r, 0, 0), "and a knob with nothing to do adds nothing");
        r.turn(6, 1);
        ok(stepOn(r, 0, 0) && r.lane(0).st[0].chord == 1 && r.ui->popover(), "B3 on an empty step: the note, with the chord");
        ok(!r.cell(4).idle && !r.cell(6).idle, "no longer dimmed");
        r.turn(7, 2);
        ok(r.lane(0).st[0].inv == 2, "B4");
        r.key(Key::Down);
        r.turn(0, -10);
        ok(stepOn(r, 0, 1) && r.lane(0).st[1].vel == 90, "T1 on an empty step: the note, at that velocity");
        r.key(Key::Down);
        r.turn(1, 3);
        ok(r.lane(0).st[2].gate == r.lane(0).st[3].gate + 3, "T2: gate");
        r.key(Key::Down);
        r.turn(2, -5);
        ok(r.lane(0).st[3].prob == 95, "T3: probability");
    }
    {   // the web panel's knobs have a place for each of these
        Rig r;
        emptied(r);
        toTracker(r);
        float stop = 0;
        r.key(Key::Ok);
        ok(fabs(r.ui->knobPlace(4, &stop) - 0.5f) < 1e-6f && fabs(stop - 1.0f / 48.0f) < 1e-6f, "NOTE: root in the middle, 48 steps");
        ok(fabs(r.ui->knobPlace(5, &stop) - 0.5f) < 1e-6f && fabs(stop - 0.25f) < 1e-6f, "OCTAVE: on the same travel, a quarter of it to the octave");
        r.turn(4, 12);
        ok(fabs(r.ui->knobPlace(4) - 0.75f) < 1e-6f && fabs(r.ui->knobPlace(5) - 0.75f) < 1e-6f, "they move together");
        ok(r.ui->knobPlace(0) > 0.5f && r.ui->knobPlace(1) >= 0.0f && r.ui->knobPlace(2) == 1.0f, "velocity, gate, probability");
        ok(r.ui->knobPlace(6, &stop) == 0.0f && fabs(stop - 1.0f / (kChordCount - 1)) < 1e-6f, "CHORD");
        r.pj->scale = 1;
        ok(r.ui->knobPlace(3, &stop) == 0.0f && stop == 1.0f, "IN SCALE, off");
        r.turn(3, 1);
        ok(r.ui->knobPlace(3) == 1.0f, "and on");
    }
}

static void testTrackerUndoLocks() {
    {   // undo: as elsewhere, one level, SHIFT + CLEAR
        Rig r;
        emptied(r);
        Step& st = r.lane(0).st[0];
        st.ov = 1; st.note = 5; st.chord = 8; st.setLock(lockKey(PG_FLT1, 1), 90);
        toTracker(r);
        const Step was = st;
        r.key(Key::Clear);
        ok(!stepOn(r, 0, 0), "removed");
        r.shiftKey(Key::Clear);
        ok(!memcmp(&st, &was, sizeof(Step)), "undo brings all of it back, locks too");
        ok(has(r.toast(), "UNDONE: STEP 1 REMOVED"), "and says what: " + r.toast());
        r.shiftKey(Key::Clear);
        ok(has(r.toast(), "NOTHING TO UNDO"), "one level");
    }
    {   // a run of edits on one step is one undo
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Ok);
        okHold(r, Key::Right, 4);
        r.turn(6, 2);
        r.turn(0, -5);
        r.shiftKey(Key::Clear);
        ok(!stepOn(r, 0, 0) && !r.lane(0).st[0].chord && r.lane(0).st[0].vel == 100, "undo takes the whole edit back, to the empty step");
    }
    {   // moving on starts a new one, and CLEAR after edits gives back the edited step
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Ok);
        okHold(r, Key::Right, 4);
        r.key(Key::Down);
        r.key(Key::Ok);
        okHold(r, Key::Right);
        r.shiftKey(Key::Clear);
        ok(!stepOn(r, 0, 1) && stepOn(r, 0, 0) && r.lane(0).st[0].note == 4, "only the step last edited goes back");
        r.key(Key::Up);
        r.key(Key::Ok);
        r.turn(6, 1);
        r.key(Key::Clear);
        r.shiftKey(Key::Clear);
        ok(stepOn(r, 0, 0) && r.lane(0).st[0].chord == 1, "undo of CLEAR: the step as it was just before");
    }
    {   // undo goes to the pattern and lane it came from
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Right);
        r.key(Key::Ok);
        okHold(r, Key::Up);
        r.tp.pat = 4;
        r.ui->settle();
        r.shiftKey(Key::Clear);
        ok(!trig(r.pj->patterns[0].lanes[1], 0, false) && r.tp.pat == 4, "another pattern on screen: the step goes back where it was");
        r.tp.pat = 0;
    }
    {   // the popover closed and opened again on the same step: a new run, so undo stops at where it was reopened
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Ok);
        r.turn(4, 2);
        r.key(Key::Ok);
        ok(!r.ui->popover(), "closed");
        r.key(Key::Ok);
        ok(r.ui->popover(), "open again");
        r.turn(4, 3);
        ok(r.lane(0).st[0].note == 5, "edited again");
        r.shiftKey(Key::Clear);
        ok(stepOn(r, 0, 0) && r.lane(0).st[0].note == 2, "undo takes back the second run only: " + std::to_string(r.lane(0).st[0].note));
    }
    {   // the same with the popover that moved away and came back
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Ok);
        r.turn(4, 2);
        r.key(Key::Down);
        r.key(Key::Up);
        r.turn(4, 3);
        r.shiftKey(Key::Clear);
        ok(r.lane(0).st[0].note == 2, "moved off and back: a new run too");
    }
    {   // a layer key held, then an arrow: the cursor has moved on, a knob edits where it is now, and no lock opens on the old cell
        Rig r;
        emptied(r);
        toTracker(r);
        r.ui->layerDown(2);
        r.key(Key::Down);
        r.turn(6, 1);
        r.turn(6, 1);
        ok(r.lane(2).st[1].chord == 2 && !stepOn(r, 2, 0) && r.ui->popover(), "the turns go to the cell under the cursor");
        r.sv.now += 500;
        r.ui->tick();
        r.ui->layerUp(2);
        ok(!r.ui->lockOpen(), "and letting go opens no lock");
    }
    {   // a key held while the pattern switches under it: its cell has gone, the knob edits the cursor's step, once, and undo has all of it
        Rig r;
        emptied(r);
        r.lane(0).n = 4;
        r.pj->patterns[1] = r.pj->patterns[0];
        r.pj->patterns[1].lanes[0].n = 16;
        for (int i = 1; i < kLanes; ++i) r.pj->patterns[0].lanes[i].n = 4;
        for (int i = 1; i < kLanes; ++i) r.pj->patterns[1].lanes[i].n = 16;
        r.pj->patterns[0].lanes[1].n = 16;
        toTracker(r);
        for (int i = 0; i < 5; ++i) r.key(Key::Down);
        ok(r.ui->trackerRow() == 5 && r.ui->cursor() == 1, "row 6 of a 4-step lane: step 2");
        r.ui->layerDown(0);
        r.tp.pat = 1;
        r.ui->settle();
        ok(r.ui->cursor() == 5, "the longer pattern: step 6 is that row's own");
        r.turn(0, -1);
        r.turn(0, -1);
        const Lane& l = r.pj->patterns[1].lanes[0];
        ok(l.st[5].vel == 98 && l.st[1].vel == 100 && !trig(l, 1, false), "both turns on the cursor's step, nothing on the old one");
        r.ui->layerUp(0);
        r.shiftKey(Key::Clear);
        ok(l.st[5].vel == 100 && !trig(l, 5, false), "and one undo takes it back");
    }
    {   // a key held out of the view: letting go on another screen taps nothing
        Rig r;
        emptied(r);
        toTracker(r);
        r.ui->layerDown(1);
        r.key(Key::Info);
        r.ui->layerUp(1);
        ok(r.ui->mode() == Mode::ProjectM && !stepOn(r, 1, 0) && !r.ui->lockOpen(), "held into PROJECT and let go: no step changes");
        Rig b;
        emptied(b);
        toTracker(b);
        b.ui->layerDown(1);
        b.key(Key::Info);
        b.key(Key::Lane);
        ok(b.ui->tracker(), "back in the view with the key still down");
        b.turn(0, -1);
        b.turn(0, -1);
        b.ui->layerUp(1);
        ok(b.lane(1).st[0].vel == 98 && !stepOn(b, 1, 1), "the key no longer holds anything: the turns are the cursor's");
    }
    {   // PART moves the lane without selecting it: a lock on the old lane closes, as another lane does
        Rig r;
        emptied(r);
        toTracker(r);
        r.hold(0, 500);
        ok(r.ui->lockOpen(), "open");
        r.key(Key::PartK);
        r.ui->settle();   // (as every frame does)
        ok(r.ui->sel() != 0 && !r.ui->lockOpen(), "PART to another lane closes it");
    }
    {   // the undo of something bigger is there until an edit takes its place, as a sample pick does elsewhere
        Rig r;
        toTracker(r);
        r.key(Key::StepK);
        r.key(Key::Clear); r.key(Key::Clear);
        ok(!r.ln().eu, "the lane emptied in the editor");
        r.key(Key::StepK); r.key(Key::StepK);
        r.key(Key::Down); r.key(Key::Down);
        ok(r.ui->tracker(), "back in the tracker");
        r.shiftKey(Key::Clear);
        ok(r.ln().eu, "moving around has not taken the undo");
    }
    {   // a lock opens from a held layer key, on the row's step, and works as in the editor
        Rig r;
        emptied(r);
        r.lane(2).n = 8;
        r.lane(0).n = 32;
        r.lane(2).st[3].ov = 1;
        toTracker(r);
        for (int i = 0; i < 11; ++i) r.key(Key::Down);
        r.hold(2, 500);
        ok(r.ui->lockOpen() && r.ui->lockLane() == 2 && r.ui->lockIdx() == 3, "held: the lock of that lane's step at this row (row 12 of 8 is step 4)");
        ok(r.ui->tracker() && has(r.toast(), "LOCK STEP 4"), "the view stays, and it says so: " + r.toast());
        r.key(Key::Flt1);
        r.turn(1, 7);
        ok(r.lane(2).st[3].nLocks == 1, "a sound page: the turn is the step's lock");
        r.key(Key::Flt1);
        ok(r.ui->tracker() && r.ui->lockOpen(), "back in the tracker with it open");
        r.key(Key::Clear);
        ok(r.lane(2).st[3].nLocks == 0 && stepOn(r, 2, 3), "CLEAR with a lock open clears the locks, the note stays");
        r.tap(2);
        ok(!r.ui->lockOpen() && has(r.toast(), "LOCK OFF"), "tapping the same key closes it");
    }
    {   // arrows with a lock open: up and down leave it, a lane to the side closes it, as a lane does in the editor
        Rig r;
        emptied(r);
        toTracker(r);
        r.hold(0, 500);
        ok(r.ui->lockOpen(), "open");
        r.key(Key::Down);
        ok(r.ui->lockOpen() && r.ui->lockIdx() == 0 && r.ui->cursor() == 1, "the cursor moves, the lock stays on its step");
        r.key(Key::Ok);
        ok(stepOn(r, 0, 1) && r.ui->lockIdx() == 0, "a note goes in beside it");
        r.tap(0);
        ok(r.ui->lockOpen() && r.ui->lockIdx() == 1, "a tap moves the lock to the row's step");
        r.key(Key::Right);
        ok(!r.ui->lockOpen(), "another lane closes it");
    }
    {   // a lock opened in the editor is there in the tracker
        Rig r;
        emptied(r);
        r.key(Key::StepK);
        r.hold(4, 500);
        ok(r.ui->lockOpen() && r.ui->lockIdx() == 4, "the editor's lock");
        r.key(Key::StepK);
        r.key(Key::StepK);
        ok(r.ui->tracker() && r.ui->lockOpen() && r.ui->trackerRow() == 4, "still open, the cursor on its step");
        const std::vector<uint16_t> a = frameOf(r);
        size_t lime = 0;
        const gfx::Box c = gfx::trackerCell(0, 4);
        for (int y = c.y; y < c.y + c.h; ++y) for (int x = c.x; x < c.x + c.w; ++x) lime += a[(size_t)y * gfx::kWidth + x] == gfx::rgb565(0xd7ff3c);
        ok(lime > 20, "and drawn on the cell: " + std::to_string(lime));
        r.key(Key::StepK);
        ok(r.ui->lockOpen(), "and on into the editor");
    }
    {   // held and turned: the held step, no lock left open
        Rig r;
        emptied(r);
        toTracker(r);
        r.key(Key::Down);
        r.ui->layerDown(5);
        r.turn(6, 2);
        r.ui->layerUp(5);
        ok(r.ui->sel() == 5 && stepOn(r, 5, 1) && r.lane(5).st[1].chord == 2 && !r.ui->lockOpen(), "hold + turn edits that lane's step");
    }
    {   // a lock mark and a sample mark on the screen, and they go with the step
        Rig r;
        emptied(r);
        r.lane(0).st[0].ov = 1;
        r.lane(0).st[0].setLock(lockKey(PG_FLT1, 1), 90);
        r.lane(7).st[1].ov = 1;
        r.lane(7).st[1].setLock(lockKey(PG_SMP, 0), 2);
        toTracker(r);
        r.key(Key::Down); r.key(Key::Down);
        r.sv.now += 2000;
        r.ui->tick();
        const std::vector<uint16_t> a = frameOf(r);
        const auto count = [&](const gfx::Box& c, uint32_t col) { size_t n = 0; for (int y = c.y; y < c.y + c.h; ++y) for (int x = c.x; x < c.x + c.w; ++x) n += a[(size_t)y * gfx::kWidth + x] == gfx::rgb565(col); return n; };
        ok(count(gfx::trackerCell(0, 0), 0xd7ff3c) > 10, "a locked step has the lock's mark");
        ok(count(gfx::trackerCell(1, 0), 0xd7ff3c) == 0, "its neighbour does not");
        ok(count(gfx::trackerCell(7, 1), 0xd7ff3c) > 10 && count(gfx::trackerCell(7, 1), 0xffffff) > 10, "a sample lock has both marks");
    }
}

static void testTrackerSampler() {
    {   // pitch, not a note; the sample by the sample browser's knobs
        Rig r;
        std::deque<std::string> paths;
        fillCard(*r.lib, paths);
        r.tap(7);
        emptied(r);
        toTracker(r);
        ok(r.labels(4) == "PITCH,OCTAVE,STEP SAMPLE," && r.label(3).empty(), "its knobs: " + r.labels(4));
        ok(r.val(4) == "0 st", "a pitch has no note name: " + r.val(4));
        r.key(Key::Ok);
        ok(stepOn(r, 7, 0) && r.lane(7).st[0].note == 0, "the first entry plays as recorded");
        r.turn(4, 3);
        ok(r.lane(7).st[0].note == 3 && r.val(4) == "+3 st", "B1 is pitch");
        r.turn(5, 1);
        ok(r.lane(7).st[0].note == 15 && r.val(5) == "+1", "B2 octaves it: " + r.val(5));
        r.turn(5, 1);
        ok(r.lane(7).st[0].note == 15 && r.ui->trackerStop() == 1, "and stops at +24 as a synth step does");
        const int lane = r.lane(7).sample();
        ok(!r.lane(7).st[0].nLocks, "no sample lock yet");
        r.turn(6, 1);
        ok(r.lane(7).st[0].nLocks == 1 && r.lane(7).st[0].lockValue(lockKey(PG_SMP, 0), -1) != lane, "B3 picks the step's sample: a lock");
        ok(r.lane(7).sample() == lane, "the lane's own stays");
        Text name, folder;
        bool pending = true;
        r.ui->trackerSample(name, folder, pending);
        ok(!pending && !strcmp(name.c_str(), r.lib->items[r.lane(7).st[0].lockValue(lockKey(PG_SMP, 0), -1)].name) && !folder.empty(), "the popover has its name and folder: " + std::string(name.c_str()) + " / " + folder.c_str());
        // SHIFT + B3 is the folder knob, nothing is picked until the sample knob moves: the browser's way
        const int16_t picked = r.lane(7).st[0].lockValue(lockKey(PG_SMP, 0), -1);
        r.ui->keyDown(Key::Shift);
        ok(r.label(6) == "FOLDER", "SHIFT makes it the folder knob");
        r.ui->turn(6, 1);
        r.ui->keyUp(Key::Shift);
        ok(r.lane(7).st[0].lockValue(lockKey(PG_SMP, 0), -1) == picked, "the folder moves, the step plays on");
        r.ui->trackerSample(name, folder, pending);
        ok(pending, "and the popover shows the sample is not picked yet");
        r.turn(6, 1);
        ok(r.lane(7).st[0].lockValue(lockKey(PG_SMP, 0), -1) != picked, "the knob then takes it");
        r.shiftKey(Key::Clear);
        ok(!stepOn(r, 7, 0) && !r.lane(7).st[0].nLocks, "undo of the whole edit: the step that was empty, no lock");
        r.key(Key::Clear);
        ok(has(r.toast(), "EMPTY"), "CLEAR has nothing to do then");
        frameOf(r);
    }
    {   // a step with no sample of its own: it shows what plays, and once the folder knob has moved, what the next turn takes
        Rig r;
        std::deque<std::string> paths;
        fillCard(*r.lib, paths);
        r.tap(7);
        emptied(r);
        r.lane(7).st[0].ov = 1;
        toTracker(r);
        Text name, folder;
        bool pending = true;
        r.ui->trackerSample(name, folder, pending);
        ok(!pending && !strcmp(name.c_str(), r.lib->items[r.lane(7).sample()].name), "no lock: the lane's sample: " + std::string(name.c_str()));
        const SampleItem& it = r.lib->items[r.lane(7).sample()];
        const std::string place = std::string(r.lib->cats[it.cat]) + " " + std::to_string(r.lib->catPlace(r.lane(7).sample()) + 1) + "/" + std::to_string(r.lib->catSize(it.cat));
        ok(place == folder.c_str(), "and its place in its folder: " + std::string(folder.c_str()) + " is " + place);
        r.ui->keyDown(Key::Shift);
        r.ui->turn(6, 1);
        r.ui->keyUp(Key::Shift);
        name.clear(); folder.clear();
        r.ui->trackerSample(name, folder, pending);
        ok(pending, "the folder knob elsewhere: pending");
        const std::string shown = name.c_str();
        ok(has(folder.c_str(), r.cell(6).tag.c_str()) && !has(folder.c_str(), r.lib->cats[it.cat]), "the folder line is that folder: " + std::string(folder.c_str()));
        const std::string cat = std::string(folder.c_str()).substr(0, std::string(folder.c_str()).find(' '));
        const int at = atoi(std::string(folder.c_str()).substr(cat.size() + 1).c_str());
        const int size = atoi(std::string(folder.c_str()).substr(std::string(folder.c_str()).find('/') + 1).c_str());
        ok(at >= 1 && at <= size, "its place is inside it: " + std::string(folder.c_str()));
        r.turn(6, 1);
        const int picked = r.lane(7).st[0].lockValue(lockKey(PG_SMP, 0), -1);
        ok(picked >= 0 && shown == r.lib->items[picked].name, "the next turn takes the one shown: " + shown);
    }
    {   // on an empty step the folder knob adds nothing: it picks nothing
        Rig r;
        std::deque<std::string> paths;
        fillCard(*r.lib, paths);
        r.tap(7);
        emptied(r);
        toTracker(r);
        r.ui->keyDown(Key::Shift);
        r.ui->turn(6, 1);
        r.ui->keyUp(Key::Shift);
        ok(!stepOn(r, 7, 0) && r.ui->popover(), "the folder knob opens the popover and leaves the step empty");
        frameOf(r);
    }
    {   // no samples at all, a sliced lane
        Rig r;
        r.lib->count = 0;
        r.tap(7);
        emptied(r);
        toTracker(r);
        r.key(Key::Ok);
        r.turn(6, 1);
        frameOf(r);
        ok(stepOn(r, 7, 0), "no samples: the view still works");
        Rig s;
        s.tap(9);
        emptied(s);
        toTracker(s);
        ok(s.cell(7).dis, "not sliced: B4 has nothing");
        s.lane(9).smpParam(PG_SLC, SLC_MODE) = SLICE_SYNC;
        ok(s.label(7) == "STEP SLICE" && s.val(7) == "AUTO", "sliced: B4 is the step's slice: " + s.label(7));
        s.turn(7, 2);
        ok(stepOn(s, 9, 0) && s.lane(9).st[0].lockValue(lockKey(PG_SLC, SLC_SLICE), 0) == 2, "and locks one");
        s.shiftKey(Key::Clear);
        ok(!stepOn(s, 9, 0) && !s.lane(9).st[0].nLocks, "undo");
    }
}

// Every key and every control in every state the view has: nothing may leave it broken, whatever the key does.
static void trackerSound(Rig& r, const std::string& what) {
    r.ui->settle();
    bool notes = true;
    for (const Pattern& p : r.pj->patterns) for (const Lane& l : p.lanes) for (const Step& s : l.st) notes = notes && s.note >= -24 && s.note <= 24 && s.nLocks <= kLocksPerStep;
    ok(notes, what + ": every note inside +-24");
    ok(r.ui->cursor() < r.ln().n, what + ": the cursor on a step");
    ok(r.ui->trackerRow() < r.ui->trackerRows() || !r.ui->tracker(), what + ": the row exists");
    ok(!r.ui->popover() || r.ui->tracker(), what + ": no popover outside the view");
    if (r.ui->tracker()) ok(r.ui->cursor() == r.ui->trackerRow() % r.ln().n, what + ": the step is the row's");
    static uint8_t arena[64 * 1024];
    gfx::Canvas cv(arena, sizeof(arena));
    gfx::drawScreen(cv, *r.ui);
    ok(!cv.overflowed(), what + ": it draws");
}
static void testTrackerEveryKey() {
    using Set = void (*)(Rig&);
    struct State { const char* name; Set set; };
    static const State kStates[] = {
        {"empty cell", [](Rig& r) { emptied(r); toTracker(r); }},
        {"filled cell", [](Rig& r) { toTracker(r); }},
        {"popover", [](Rig& r) { toTracker(r); r.key(Key::Ok); }},
        {"note moved", [](Rig& r) { emptied(r); toTracker(r); okHold(r, Key::Up, 2); }},
        {"OK held", [](Rig& r) { toTracker(r); r.ui->keyDown(Key::Ok); }},
        {"OK held, moved", [](Rig& r) { toTracker(r); r.ui->keyDown(Key::Ok); r.key(Key::Right); }},
        {"SHIFT held", [](Rig& r) { toTracker(r); r.ui->keyDown(Key::Shift); }},
        {"FILL held", [](Rig& r) { toTracker(r); r.ui->keyDown(Key::Fill); }},
        {"layer held", [](Rig& r) { toTracker(r); r.ui->layerDown(3); }},
        {"lock open", [](Rig& r) { toTracker(r); r.hold(0, 500); }},
        {"playing", [](Rig& r) { toTracker(r); r.key(Key::Play); r.run(400); r.ui->tick(); }},
        {"sampler lane", [](Rig& r) { r.tap(7); toTracker(r); r.key(Key::Ok); }},
        {"sliced lane", [](Rig& r) { r.tap(9); r.ln().smpParam(PG_SLC, SLC_MODE) = SLICE_SYNC; toTracker(r); }},
        {"page 2 of 32", [](Rig& r) { r.lane(0).n = 32; toTracker(r); for (int i = 0; i < 20; ++i) r.key(Key::Down); }},
        {"last row", [](Rig& r) { r.lane(0).n = 32; toTracker(r); for (int i = 0; i < 31; ++i) r.key(Key::Down); }},
        {"one step", [](Rig& r) { for (int i = 0; i < kLanes; ++i) { r.lane(i).n = 1; r.lane(i).k = 0; r.lane(i).r = 0; } toTracker(r); }},
        {"repeat", [](Rig& r) { r.lane(1).n = 3; r.lane(1).k = 1; r.lane(1).r = 0; toTracker(r); r.key(Key::Right); for (int i = 0; i < 7; ++i) r.key(Key::Down); }},
        {"scale", [](Rig& r) { r.pj->scale = 2; toTracker(r); r.turn(3, 1); r.key(Key::Ok); }},
    };
    for (const State& s : kStates) {
        for (int k = 0; k < (int)Key::Count; ++k) {
            Rig r;
            s.set(r);
            const std::string what = std::string(s.name) + ", key " + std::to_string(k);
            r.ui->keyDown((Key)k);
            trackerSound(r, what + " down");
            r.ui->keyUp((Key)k);
            r.sv.now += 700;
            r.ui->tick();
            trackerSound(r, what + " up");
        }
        for (int i = 0; i < kLanes; ++i) {
            Rig r;
            s.set(r);
            r.ui->layerDown(i);
            r.sv.now += 400;
            r.ui->tick();
            r.ui->layerUp(i);
            trackerSound(r, std::string(s.name) + ", layer " + std::to_string(i));
        }
        for (int e = 0; e < 8; ++e)
            for (int d : {1, -1, 5, -5, 64, -64}) {
                Rig r;
                s.set(r);
                r.ui->turn(e, d);
                trackerSound(r, std::string(s.name) + ", knob " + std::to_string(e) + " by " + std::to_string(d));
            }
    }
    // what each key does here, one by one, from a cell with a note
    {
        Rig r;
        toTracker(r);
        r.key(Key::Mute); ok(r.lane(0).mute == 1 && r.ui->tracker(), "MUTE mutes the lane, the view stays");
        r.key(Key::Mute);
        r.key(Key::Play); ok(r.tp.playing == 1 && r.ui->tracker(), "PLAY plays, the view stays");
        r.key(Key::Stop); ok(!r.tp.playing && r.ui->tracker(), "STOP");
        r.ui->keyDown(Key::Fill); ok(r.tp.fill == 1, "FILL"); r.ui->keyUp(Key::Fill);
        r.key(Key::Copy); ok(has(r.toast(), "LANE 1 COPIED"), "COPY is the lane's: " + r.toast());
        r.key(Key::Right);
        r.key(Key::Paste); ok(has(r.toast(), "PASTED TO LANE 2"), "PASTE too: " + r.toast());
        r.shiftKey(Key::Clear); ok(has(r.toast(), "UNDONE"), "and undoes");
        r.key(Key::Add); ok(has(r.toast(), "EUCLID"), "ADD: a new rhythm on the lane");
        r.key(Key::PartK); ok(r.ui->tracker() && has(r.toast(), "PART"), "PART");
        r.key(Key::File); ok(r.ui->mode() == Mode::Browse, "FILE");
        r.key(Key::File); ok(r.ui->tracker(), "and back");
        r.key(Key::Mixer); ok(r.ui->mode() == Mode::Mixer, "MIXER");
        r.key(Key::Lane); ok(r.ui->tracker(), "L back to the tracker");
        r.key(Key::Spare); ok(r.ui->mode() == Mode::Fx, "FX");
        r.key(Key::Lane);
        r.key(Key::Info); ok(r.ui->mode() == Mode::ProjectM, "PROJECT");
        r.key(Key::Lane);
        r.key(Key::SongK); ok(r.ui->mode() == Mode::Song, "SONG");
        r.key(Key::SongK); ok(r.ui->tracker(), "and back");
        r.key(Key::Osc1); ok(r.ui->mode() == Mode::Sound, "a sound page");
        r.key(Key::Osc1); ok(r.ui->tracker(), "and back");
        r.key(Key::StepK); ok(r.ui->stepSub() == 0, "STEP: the next view");
        // knobs while OK is down: whatever they do, OK coming up is no tap
        {
            Rig q;
            emptied(q);
            q.pj->scale = 1;
            toTracker(q);
            q.ui->keyDown(Key::Ok);
            q.turn(3, 1);
            q.ui->keyUp(Key::Ok);
            ok(q.ui->inScale() && !stepOn(q, 0, 0), "IN SCALE turned with OK down: no note");
            q.ui->keyDown(Key::Ok);
            q.turn(7, 1);
            q.ui->keyUp(Key::Ok);
            ok(!stepOn(q, 0, 0), "a knob with nothing to do, with OK down: no note");
            q.ui->keyDown(Key::Ok);
            q.ui->keyDown(Key::Shift);
            q.ui->keyUp(Key::Shift);
            q.ui->keyUp(Key::Ok);
            ok(!stepOn(q, 0, 0), "SHIFT pressed with OK down: no note");
            q.ui->keyDown(Key::Ok);
            q.ui->keyDown(Key::Fill);
            q.ui->keyUp(Key::Fill);
            q.ui->keyUp(Key::Ok);
            ok(!stepOn(q, 0, 0), "FILL with OK down: no note");
        }
        // keys while OK is down: they do what they do, and OK coming up is no tap
        Rig o;
        emptied(o);
        toTracker(o);
        o.ui->keyDown(Key::Ok);
        o.key(Key::Mute);
        o.ui->keyUp(Key::Ok);
        ok(o.lane(0).mute == 1 && !stepOn(o, 0, 0), "a key while OK is down: OK was no tap");
        o.ui->keyDown(Key::Ok);
        o.tap(2);
        o.ui->keyUp(Key::Ok);
        ok(o.ui->sel() == 2 && !stepOn(o, 2, 0), "a layer key too");
        o.ui->keyDown(Key::Ok);
        o.turn(0, -1);
        o.ui->keyUp(Key::Ok);
        ok(stepOn(o, 2, 0) && o.ui->popover(), "a knob edits, and the popover stays when OK comes up");
        o.ui->keyDown(Key::Ok);
        o.key(Key::Lane);
        o.ui->keyUp(Key::Ok);
        ok(!o.ui->tracker() && o.ui->mode() == Mode::Play, "left the view with OK down");
        // OK down here, up in the next view: not a tap
        Rig h;
        emptied(h);
        toTracker(h);
        h.ui->keyDown(Key::Ok);
        h.key(Key::StepK);
        h.ui->keyUp(Key::Ok);
        ok(!h.ui->tracker() && !stepOn(h, 0, 0), "OK let go in another view adds nothing");
        // OK down in another view, up in this one: no tap, and no note
        Rig v;
        emptied(v);
        v.key(Key::StepK);
        v.key(Key::StepK);
        v.ui->keyDown(Key::Ok);
        v.key(Key::StepK);
        v.ui->keyUp(Key::Ok);
        ok(v.ui->tracker() && !v.ui->popover() && v.lane(0).st[0].ov == 1, "OK pressed in the second view did its toggle there, and is no tap here");
        // OK on the SAVE and other screens is theirs
        Rig b;
        toTracker(b);
        b.key(Key::File);
        const int was2 = b.ui->browseIdx();
        b.key(Key::Ok);
        ok(b.ui->browseIdx() == was2 && !b.ui->popover(), "OK in BROWSE loads, as ever");
    }
}

// What the display bus has to carry, counted the way board/Lcd.cpp does: the tiles that differ from the last frame.
static void testTrackerTiles() {
    Rig r;
    r.sv.now += 2000;
    toTracker(r);
    r.sv.now += 2000;
    r.ui->tick();
    const auto settled = [&]() { r.sv.now += 2000; r.ui->tick(); return frameOf(r); };
    std::vector<uint16_t> a = settled();
    int worst = 0, total = 0;
    r.tp.playing = 1;
    r.tp.shownStep = 0;
    r.ui->tick();
    std::vector<uint16_t> b = frameOf(r);
    for (int s = 1; s < 32; ++s) {
        r.tp.shownStep = (int16_t)(s % r.ui->trackerRows());
        r.ui->tick();
        const std::vector<uint16_t> c = frameOf(r);
        const int n = tilesChanged(b, c);
        worst = n > worst ? n : worst;
        total += n;
        b = c;
    }
    r.tp.playing = 0;
    r.tp.shownStep = -1;
    a = settled();
    printf("tracker tiles of %d: playhead a step: worst %d, mean %d\n", 30 * 27, worst, total / 31);
    ok(worst <= 64, "a playhead step sends two rows of tiles at most: " + std::to_string(worst));
    r.key(Key::Down);
    std::vector<uint16_t> c = settled();
    const int down = tilesChanged(a, c);
    r.key(Key::Right);
    std::vector<uint16_t> d = settled();
    const int right = tilesChanged(c, d);
    r.key(Key::Ok);
    std::vector<uint16_t> e = settled();
    const int pop = tilesChanged(d, e);
    r.ui->turn(4, 1);
    std::vector<uint16_t> f = settled();
    const int note = tilesChanged(e, f);
    r.key(Key::Down);
    std::vector<uint16_t> g = settled();
    const int shut = tilesChanged(f, g);
    printf("tracker tiles: cursor down %d, cursor right %d, popover opens %d, a note turned in it %d, it closes %d\n", down, right, pop, note, shut);
    ok(down <= 140 && right <= 140, "the cursor moves without a redraw of the grid");
    ok(pop <= 200 && shut <= 200, "a popover costs its own area");
    Rig p;
    p.sv.now += 2000;
    p.ui->tick();
    std::vector<uint16_t> pa = frameOf(p);
    int pworst = 0;
    p.tp.playing = 1;
    for (int s = 0; s < 32; ++s) {
        p.tp.shownStep = (int16_t)s;
        p.ui->tick();
        const std::vector<uint16_t> pb = frameOf(p);
        const int n = tilesChanged(pa, pb);
        pworst = n > pworst && s ? n : pworst;
        pa = pb;
    }
    printf("for comparison, PLAY: a step of the default project, worst %d tiles\n", pworst);
    // the fullest screen there is: every cell a note with both marks, a toast, and a popover
    {
        Rig f;
        static uint8_t arena[64 * 1024];
        for (int i = 0; i < kLanes; ++i) {
            Lane& l = f.lane(i);
            l.n = 16; l.eu = 0;
            for (int k = 0; k < 16; ++k) { Step& st = l.st[k]; st.ov = 1; st.chord = 10; st.vel = 127; st.setLock(lockKey(PG_SMP, 0), 2); st.setLock(lockKey(PG_FLT1, 1), 9); }
        }
        toTracker(f);
        f.key(Key::Ok);
        f.ui->say("A LONG TOAST ON TOP OF ALL OF IT");
        gfx::Canvas cv(arena, sizeof(arena));
        gfx::drawScreen(cv, *f.ui);
        printf("tracker display list: %zu bytes at its fullest, of %zu\n", cv.used(), sizeof(arena));
        ok(!cv.overflowed() && cv.used() < sizeof(arena) * 3 / 4, "the fullest grid leaves room in the board's display list: " + std::to_string(cv.used()));
    }
    // page 2 of 32 steps: the page turn is the one big redraw, and it is a key press
    Rig q;
    q.lane(0).n = 32;
    toTracker(q);
    q.sv.now += 2000;
    q.ui->tick();
    std::vector<uint16_t> h = frameOf(q);
    q.shiftKey(Key::Down);
    q.sv.now += 2000;
    q.ui->tick();
    const int page = tilesChanged(h, frameOf(q));
    printf("tracker tiles: a page turn %d\n", page);
    // the other page playing: only the strip under the grid moves
    q.tp.playing = 1;
    q.tp.shownStep = 2;
    q.ui->tick();
    std::vector<uint16_t> i = frameOf(q);
    q.tp.shownStep = 3;
    q.ui->tick();
    const int off = tilesChanged(i, frameOf(q));
    printf("tracker tiles: the playhead on the other page, a step: %d\n", off);
    ok(off <= 4, "the playhead on the other page is a mark under the grid: " + std::to_string(off));
}

int main() {
    shippedPresets();   // the library, as the card would give it
    testTheory();
    testBasics();
    testSampler();
    testChordsArpSequencer();
    testPatternsSongs();
    testUndoFillNav();
    testEmptyPattern();
    testCost();
    testProjectFile();
    testDeleteSlot();
    testFactoryProjects();
    testMidiExport();
    testExportScreen();
    testGlobalAndMod();
    testSilentKnobs();
    testReviewFindings();
    testSamplePaths();
    testKnobStops();
    testMidiMap();
    testSettle();
    testFx();
    testFxCost();
    testWaves();
    testPresetFx();
    testSavePreset();
    testParts();
    testSlices();
    testLoops();
    testRecord();
    testSampleBrowser();
    testRecordFixes();
    testNames();
    testFxLocks();
    testTrackerViews();
    testTrackerCursor();
    testTrackerEntry();
    testTrackerLimits();
    testTrackerPopover();
    testTrackerUndoLocks();
    testTrackerSampler();
    testTrackerEveryKey();
    testTrackerTiles();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
