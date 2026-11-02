// The sound side on the host: the sampler (a port of the mockup's playSample checks), the patch map and the instrument (p-locks reach
// the voice, a note's end releases that note only, the degrade governor caps voices).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <functional>
#include <string>
#include <vector>

#include <nemesis/dsp/CathodeOscCore.h>
#include <nemesis/dsp/ForkConfig.h>

#include "PresetHost.h"
#include "app/Model.h"
#include "app/SampleLib.h"
#include "audio/Instrument.h"
#include "audio/PatchMap.h"
#include "app/ProjectFile.h"
#include "audio/Sampler.h"
#include "audio/MemoryPool.h"
#include "audio/Recorder.h"
#include "audio/SongExport.h"
#include "audio/WaveStore.h"
#include "fx/FxSection.h"
#include "midi/MidiOutRing.h"

using namespace app;
using audio::SampleHit;
using audio::makeHit;

static int checks = 0, fails = 0;
static void ok(bool c, const std::string& m) {
    ++checks;
    if (!c) { ++fails; printf("FAIL %s\n", m.c_str()); }
}
static bool near(double a, double b, double tol) { return fabs(a - b) <= tol; }

static double rms(const std::vector<float>& x, size_t a, size_t b) {
    double s = 0;
    for (size_t i = a; i < b && i < x.size(); ++i) s += x[i] * x[i];
    return sqrt(s / (double)(b > a ? b - a : 1));
}

static void testHits(const SampleLib& lib) {
    Lane ln;
    makeLane(ln, 2, 60, 16, 4, 0, 127);
    ln.smpParam(PG_SMP, 0) = 0;
    SampleHit h;
    const double dur = lib.items[0].length;
    ok(makeHit(ln, nullptr, 0, 0, 100, 1.0f, lib, 44100.0, h), "a hit from the lane's pages");
    ok(near(h.start, 0, 1e-9) && near(h.end, dur - 1, 1) && h.mode == 0, "one-shot plays the whole sample");
    ln.smpParam(PG_SMP, 1) = 64;
    makeHit(ln, nullptr, 0, 0, 100, 1.0f, lib, 44100.0, h);
    ok(near(h.start, 64 / 127.0 * dur, 1e-6), "START offsets playback");
    Lock lk{lockKey(PG_SMP, 1), 0, 100};
    makeHit(ln, &lk, 1, 0, 100, 1.0f, lib, 44100.0, h);
    ok(near(h.start, 100 / 127.0 * dur, 1e-6), "a p-lock replaces START for that hit only");
    makeHit(ln, nullptr, 0, 0, 100, 1.0f, lib, 44100.0, h);
    ok(near(h.start, 64 / 127.0 * dur, 1e-6), "the next hit uses the lane's own START again");
    ln.smpParam(PG_SMP, 1) = 0;
    makeHit(ln, nullptr, 0, 12, 100, 1.0f, lib, 44100.0, h);
    ok(near(h.step, 2.0, 1e-9), "+12 semitones doubles the rate");
    Lock lp{lockKey(PG_SMP, 4), 0, -12};
    makeHit(ln, &lp, 1, 0, 100, 1.0f, lib, 44100.0, h);
    ok(near(h.step, 0.5, 1e-9), "a pitch lock of -12 halves it");
    ln.smpParam(PG_SMP, 6) = 1;
    ln.smpParam(PG_SMP, 2) = 100;
    makeHit(ln, nullptr, 0, 0, 100, 1.0f, lib, 44100.0, h);
    ok(h.reverse && near(h.end, 100 / 127.0 * dur, 1e-6), "reverse plays END to START");
    ln.smpParam(PG_SMP, 6) = 0;
    ln.smpParam(PG_SMP, 3) = 2; ln.smpParam(PG_SMP, 1) = 20; ln.smpParam(PG_SMP, 2) = 80;
    makeHit(ln, nullptr, 0, 0, 100, 1.0f, lib, 44100.0, h);
    ok(h.mode == 2 && near(h.start, 20 / 127.0 * dur, 1e-6) && near(h.end, 80 / 127.0 * dur, 1e-6), "loop mode loops START..END");
    Lock ls{lockKey(PG_SMP, 0), 0, 2};
    makeHit(ln, &ls, 1, 0, 100, 1.0f, lib, 44100.0, h);
    ok(h.smp == &lib.items[2], "a sample lock (SMP:0) plays that step's own sample");
    Lock bad{lockKey(PG_SMP, 0), 0, 999};
    makeHit(ln, &bad, 1, 0, 100, 1.0f, lib, 44100.0, h);
    ok(h.smp == &lib.items[lib.count - 1], "a sample number past the library is held to the last sample (a project from another card)");
}

static void testSampler(const SampleLib& lib) {
    Lane ln;
    makeLane(ln, 2, 60, 16, 4, 0, 127);
    SampleHit h;
    makeHit(ln, nullptr, 0, 0, 120, 1.0f, lib, 44100.0, h);   // Kick 808, one-shot
    audio::Sampler s;
    s.init(44100.0);
    std::vector<float> L(44100 * 2), R(44100 * 2);
    const int32_t a = s.start(h, 4, 10);
    ok(a >= 0, "a hit starts");
    s.release(a, 100);   // a one-shot ignores the gate
    for (size_t off = 0; off < L.size(); off += 32) s.render(&L[off], &R[off], 32);
    const size_t len = lib.items[0].length;
    ok(rms(L, 0, 10) == 0.0 && rms(L, 10, 2000) > 0.01, "the hit starts at its offset (10 samples in)");
    ok(rms(L, len / 2, len / 2 + 2000) > 0.001, "a one-shot plays on after its gate");
    ok(rms(L, len + 2000, len + 6000) < 1e-6 && s.active() == 0, "and stops at the end of the sample");
    // gated: the release starts at the gate
    std::fill(L.begin(), L.end(), 0.0f); std::fill(R.begin(), R.end(), 0.0f);
    ln.smpParam(PG_SMP, 3) = 1;
    ln.smpParam(PG_SAMP, 3) = 10;   // a short release
    makeHit(ln, nullptr, 0, 0, 120, 1.0f, lib, 44100.0, h);
    const int32_t g = s.start(h, 4, 0);
    s.release(g, 2205);
    for (size_t off = 0; off < 44100; off += 32) s.render(&L[off], &R[off], 32);
    ok(rms(L, 1000, 2000) > 0.01 && rms(L, 8000, 9000) < 1e-3, "gated: the hit fades after its gate (50 ms)");
    // choke group: a second hit in the group cuts the first
    ln.smpParam(PG_SMP, 3) = 0;
    ln.smpParam(PG_SAMP, 7) = 1;
    makeHit(ln, nullptr, 0, 0, 120, 1.0f, lib, 44100.0, h);
    audio::Sampler c;
    c.init(44100.0);
    c.start(h, 4, 0);
    std::fill(L.begin(), L.end(), 0.0f);
    for (int k = 0; k < 10; ++k) c.render(&L[k * 32], &R[k * 32], 32);
    c.start(h, 4, 0);
    for (int k = 10; k < 30; ++k) c.render(&L[k * 32], &R[k * 32], 32);
    ok(c.active() == 1, "choke group: the first hit is cut when the second starts");
    ln.smpParam(PG_SAMP, 7) = 0;
    makeHit(ln, nullptr, 0, 0, 120, 1.0f, lib, 44100.0, h);
    audio::Sampler n;
    n.init(44100.0);
    n.start(h, 4, 0); n.start(h, 4, 0);
    for (int k = 0; k < 30; ++k) n.render(&L[k * 32], &R[k * 32], 32);
    ok(n.active() == 2, "without a choke group both ring");
    audio::Sampler v;
    v.init(44100.0);
    for (int k = 0; k < 4; ++k) { v.start(h, 2, 0); v.render(&L[0], &R[0], 32); }
    for (int k = 0; k < 20; ++k) v.render(&L[0], &R[0], 32);
    ok(v.active() <= 2, "a sampler part never holds more voices than it has: " + std::to_string(v.active()));
    const int32_t stale = v.start(h, 2, 0);
    for (int k = 0; k < 4; ++k) v.start(h, 2, 0);
    for (int k = 0; k < 20; ++k) v.render(&L[0], &R[0], 32);
    v.release(stale, 0);   // its voice belongs to a later hit now: nothing happens
    ok(v.active() >= 1, "a stale handle does not release a later hit");
}

static void testPatch() {
    Project* pj = new Project;
    makeDefaultProject(*pj);
    audio::Patch p;
    makePatch(pj->parts[0], nullptr, 0, 0, p);
    ok(memcmp(p.p, pj->parts[0].pages, sizeof(p.p)) == 0, "no locks, no degrade: the part's own pages");
    Lock l{lockKey(PG_FLT1, 1), 0, 33};
    makePatch(pj->parts[0], &l, 1, 0, p);
    ok(p.p[PG_FLT1][1] == 33, "a lock overrides one value");
    Lock g{lockKey(PG_GLOB, 0), 0, 1};
    makePatch(pj->parts[0], &g, 1, 0, p);
    ok(p.p[PG_GLOB][0] == pj->parts[0].pages[PG_GLOB][0], "GLOBAL is never locked");
    pj->parts[0].pages[PG_FLT1][0] = 7; pj->parts[0].pages[PG_FLT2][0] = 1; pj->parts[0].pages[PG_FLT2][5] = 1;
    Lock heavy{lockKey(PG_FLT2, 0), 0, 5};
    makePatch(pj->parts[0], &heavy, 1, 0, p);
    ok(!(cost::isHeavy((cost::Filter)p.f1) && cost::isHeavy((cost::Filter)p.f2)), "a lock never makes a second heavy filter");
    pj->parts[0].pages[PG_FLT1][3] = 50; pj->parts[0].pages[PG_SUB][4] = 3;
    makePatch(pj->parts[0], nullptr, 0, (1u << (int)cost::Action::HeavyToLight) | (1u << (int)cost::Action::UnisonOff), p);
#if DBOY_COST_LEAN
    ok(p.p[PG_FLT1][3] == 50 && p.p[PG_SUB][4] == 1, "degrade: no model is heavy on the lean voice, so the drive stays; unison off");
    makePatch(pj->parts[0], nullptr, 0, 1u << (int)cost::Action::CompandOff, p);
    ok(p.p[PG_GRIT][1] == 0 && p.p[PG_GRIT][7] == 0, "degrade: the converter's bypass takes all of it out, the cost it saves");
#else
    ok(!cost::isHeavy((cost::Filter)p.f1) && p.p[PG_FLT1][3] == 0 && p.p[PG_SUB][4] == 1, "degrade: heavy to light (and its drive), unison off");
#endif
    delete pj;
}

// MIDI in and out on the lanes' channels (app/MidiMap.h)
struct Sent { uint8_t st, d1, d2; int block; };
static int gBlock = 0;   // the block being rendered (InstRig::run)
static std::vector<Sent> gSent;
static bool recordOut(const uint8_t* b, int n) {
    for (int i = 0; i + 2 < n; i += 3) gSent.push_back({b[i], b[i + 1], b[i + 2], gBlock});
    return true;
}
static midi::SpscQueue<64> gIn;
static void midiIn(uint8_t type, int ch, int d1, int d2) {
    midi::Event e;
    e.type = (midi::Type)type; e.channel = (uint8_t)(ch - 1); e.data1 = (uint8_t)d1; e.data2 = (uint8_t)d2;
    gIn.push(e);
}
// every note-on sent has exactly one note-off, on the same channel and note, after it; no note-off without one
static bool balanced(std::string& why) {
    int on[16][128] = {};
    for (const Sent& m : gSent) {
        const int ch = m.st & 0x0F;
        if ((m.st & 0xF0) == 0x90 && m.d2 > 0) ++on[ch][m.d1];
        else if ((m.st & 0xF0) == 0x80 || (m.st & 0xF0) == 0x90) {
            if (on[ch][m.d1] <= 0) { why = "note-off without a note-on: ch " + std::to_string(ch + 1) + " note " + std::to_string(m.d1); return false; }
            on[ch][m.d1] = 0;   // one note-off ends every start of that note (a retrigger sent its own off before the new on)
        }
    }
    for (int c = 0; c < 16; ++c) for (int n = 0; n < 128; ++n)
        if (on[c][n]) { why = "still on: ch " + std::to_string(c + 1) + " note " + std::to_string(n); return false; }
    return true;
}
static int countSent(uint8_t st, int d1 = -1) {
    int n = 0;
    for (const Sent& m : gSent) n += m.st == st && (d1 < 0 || m.d1 == d1);
    return n;
}


struct InstRig {
    Project* pj = new Project;
    Transport tp;
    SampleLib* lib;
    audio::Instrument* inst = new audio::Instrument;
    std::vector<float> L, R;
    explicit InstRig(SampleLib* l) : lib(l) {
        makeDefaultProject(*pj);
        for (Pattern& p : pj->patterns) for (Lane& ln : p.lanes) ln.mute = 1;
        inst->init(44100.0, *pj, tp, *lib);
    }
    void run(int blocks) {
        float l[32], r[32];
        for (int b = 0; b < blocks; ++b) {
            ++gBlock;
            inst->beginBlock();
            inst->render(l, r, 32);
            L.insert(L.end(), l, l + 32);
            R.insert(R.end(), r, r + 32);
        }
    }
};

static void testInstrument(SampleLib* lib) {
    {   // one long synth note per beat; voices never exceed the part's VOICES
        InstRig r(lib);
        Lane& a = r.pj->patterns[0].lanes[0];
        a.mute = 0; a.eu = 0; a.n = 2;
        for (Step& s : a.st) { s.ov = 1; s.gate = 127; }
        r.pj->parts[0].voices = 2;
        r.pj->parts[0].pages[PG_AMP][3] = 0;   // the shortest release, so only held notes sound
        r.pj->parts[0].pages[PG_AMP][2] = 127;
        r.tp.playing = 1; r.tp.playGen = 1;
        int most = 0;
        for (int k = 0; k < 400; ++k) { r.run(1); most = std::max(most, r.inst->activeVoices(0)); }
        ok(rms(r.L, 2000, r.L.size()) > 0.005, "part A sounds");
        ok(most <= 3, "VOICES 2: no more than two held (plus a release tail): " + std::to_string(most));
    }
    {   // the drums play
        InstRig r(lib);
        Lane& k = r.pj->patterns[0].lanes[7];
        k.mute = 0;
        r.tp.playing = 1; r.tp.playGen = 1;
        r.run(400);
        ok(rms(r.L, 0, r.L.size()) > 0.01, "the kick lane sounds");
    }
    {   // the preview of a preset plays three notes and leaves the part as it was
        InstRig r(lib);
        int16_t before[kSynthPages][kParamsPerPage];
        memcpy(before, r.pj->parts[0].pages, sizeof(before));
        r.inst->previewPreset(0, 2);   // Dimension Bloom: a pad
        r.run(1200);
        ok(rms(r.L, 0, r.L.size()) > 0.002 && memcmp(before, r.pj->parts[0].pages, sizeof(before)) == 0, "a preset preview sounds, the part is unchanged");
        r.run(200);
        ok(r.inst->activeVoices(0) == 0 || rms(r.L, r.L.size() - 3000, r.L.size()) < 0.01, "and stops");
    }
    {   // FILTER 2's CUTOFF and RESO reach the sound (they did not once: the voice stage's own write was overwritten by the modulated voice's push of its defaults)
        auto render = [&](int cutoff, int reso) {
            InstRig r(lib);
            Lane& a = r.pj->patterns[0].lanes[0];
            a.mute = 0; a.eu = 0; a.n = 2;
            for (Step& s : a.st) { s.ov = 1; s.gate = 127; }
            Part& part = r.pj->parts[0];
            part.pages[PG_AMP][2] = 127;
            part.pages[PG_FLT1][0] = 0; part.pages[PG_FLT1][1] = 127; part.pages[PG_FLT1][4] = 0; part.pages[PG_FLT1][5] = 0;   // LP wide open, no tracking, no envelope
            part.pages[PG_FLT2][0] = 1; part.pages[PG_FLT2][1] = cutoff; part.pages[PG_FLT2][2] = reso; part.pages[PG_FLT2][4] = 0; part.pages[PG_FLT2][5] = 1;   // BP in series
            r.tp.playing = 1; r.tp.playGen = 1;
            r.run(300);
            return r.L;
        };
        auto diff = [](const std::vector<float>& x, const std::vector<float>& y) {
            double d = 0, e = 0;
            for (size_t i = 4000; i < x.size() && i < y.size(); ++i) { d += (x[i] - y[i]) * (x[i] - y[i]); e += x[i] * x[i]; }
            return e > 0 ? sqrt(d / e) : 0.0;
        };
        const std::vector<float> lo = render(50, 40), hi = render(100, 40), res = render(50, 110);
        ok(rms(lo, 4000, lo.size()) > 1e-4, "a band-pass in filter 2 still lets the note through");
        ok(diff(lo, hi) > 0.2, "FILTER 2 CUTOFF changes the sound: " + std::to_string(diff(lo, hi)));
        ok(diff(lo, res) > 0.2, "FILTER 2 RESO changes the sound: " + std::to_string(diff(lo, res)));
    }
    {   // the audition of a sample
        InstRig r(lib);
        r.inst->auditionSample(0);
        r.run(300);
        ok(rms(r.L, 0, r.L.size()) > 0.01, "auditioning a sample sounds");
    }
    {   // stop all: everything fades
        InstRig r(lib);
        Lane& a = r.pj->patterns[0].lanes[3];
        a.mute = 0; a.eu = 0; a.n = 1; a.st[0].ov = 1; a.st[0].gate = 127;
        r.pj->parts[1].pages[PG_AMP][2] = 127;
        r.tp.playing = 1; r.tp.playGen = 1;
        r.run(200);
        r.tp.playing = 0;
        r.inst->stopAllSound();
        r.run(2000);
        ok(rms(r.L, r.L.size() - 4000, r.L.size()) < 1e-4, "STOP + stop all: silence");
    }
}

// A decimated pyramid (the device's) against the full one: the same table, the same oscillator, outputs that agree; a table adopted from the 16-bit
// file the host tool writes agrees with the one built from the frames.
static void testWavetablePyramid() {
    const int L = 2048, N = 8;
    std::vector<float> fr((size_t)L * N);
    for (int f = 0; f < N; ++f)
        for (int i = 0; i < L; ++i) {
            const double ph = (double)i / L;
            double v = 0;
            for (int h = 1; h <= 40; ++h) v += sin(2 * M_PI * h * ph + 0.3 * f * h) / (h * (1.0 + 0.4 * f * (h % 3)));   // a morphing, harmonic-rich shape
            fr[(size_t)f * L + i] = (float)(v * 0.3);
        }
    nemesis::dsp::Wavetable full, dec, adopted;
    full.build(fr.data(), N, L, false);
    dec.build(fr.data(), N, L, true);
    ok(!full.decimated() && dec.decimated() && full.mipLength(5) == L && dec.mipLength(0) == L && dec.mipLength(1) == L && dec.mipLength(2) == L && dec.mipLength(3) == L / 2 && dec.mipLength(10) == 128,
       "the decimated pyramid stores a mip at 4 x its Nyquist length, never under 128");
    ok(dec.rawSize() * 5 < full.rawSize() * 2, "and is under 40% of the full one: " + std::to_string(dec.rawSize()) + " vs " + std::to_string(full.rawSize()) + " samples");
    // the 16-bit file: a scale and the quantised raw data
    float mx = 0;
    for (size_t i = 0; i < dec.rawSize(); ++i) mx = std::max(mx, fabsf((float)dec.raw()[i]));
    const float scale = mx / 32767.0f;
    std::vector<int16_t> q(dec.rawSize());
    for (size_t i = 0; i < q.size(); ++i) q[i] = (int16_t)lrintf((float)dec.raw()[i] / scale);
    ok(adopted.adoptPyramid(N, L, true, q.data(), q.size(), scale), "adopt: the right sample count is accepted");
    nemesis::dsp::Wavetable wrong;
    ok(!wrong.adoptPyramid(N, L, true, q.data(), q.size() - 1, scale) && wrong.numFrames() == 0, "adopt: a wrong count is refused and leaves an empty table");
    auto render = [&](const nemesis::dsp::Wavetable& t, double hz, float pos) {
        nemesis::dsp::WavetableOscillator o;
        o.setSampleRate(44100.0);
        o.setWavetable(t);
        o.setFrequency(hz);
        o.setPosition(pos);
        std::vector<float> y(4096);
        for (float& v : y) v = o.processSample();
        return y;
    };
    double worstDec = 0, worstAd = 0;
    for (double hz : {55.0, 110.0, 220.0, 440.0, 880.0, 1760.0, 3520.0, 7040.0})
        for (float pos : {0.0f, 0.37f, 0.8f}) {
            const auto a = render(full, hz, pos), b = render(dec, hz, pos), c = render(adopted, hz, pos);
            double ea = 0, ed = 0, ec = 0;
            for (size_t i = 0; i < a.size(); ++i) { ea += (double)a[i] * a[i]; ed += (double)(a[i] - b[i]) * (a[i] - b[i]); ec += (double)(b[i] - c[i]) * (b[i] - c[i]); }
            worstDec = std::max(worstDec, sqrt(ed / (ea + 1e-12)));
            worstAd = std::max(worstAd, sqrt(ec / (ea + 1e-12)));
        }
    printf("wavetable: decimated vs full, worst relative error %.4f; adopted vs built %.5f\n", worstDec, worstAd);
    {   // a bright table (a saw: every harmonic): the decimated pyramid against the full one, the -42 dB claim
        std::vector<float> saw((size_t)L);
        for (int i = 0; i < L; ++i) saw[(size_t)i] = (float)(2.0 * i / L - 1.0) * 0.5f;
        nemesis::dsp::Wavetable fs, ds;
        fs.build(saw.data(), 1, L, false);
        ds.build(saw.data(), 1, L, true);
        double worst = 0;
        for (double hz : {110.0, 440.0, 1760.0, 3520.0, 7040.0}) {
            const auto a1 = render(fs, hz, 0.0f), b1 = render(ds, hz, 0.0f);
            double ea = 0, ed = 0;
            for (size_t i = 0; i < a1.size(); ++i) { ea += (double)a1[i] * a1[i]; ed += (double)(a1[i] - b1[i]) * (a1[i] - b1[i]); }
            worst = std::max(worst, sqrt(ed / (ea + 1e-12)));
        }
        ok(worst < 0.015, "a saw table: decimated vs full within -36 dB (measured ~-41): " + std::to_string(worst));
    }
    ok(worstDec < 0.015, "the decimated table sounds like the full one (error under -36 dB): " + std::to_string(worstDec));
    ok(worstAd < 0.002, "a table adopted from the 16-bit file equals the built one: " + std::to_string(worstAd));
    // the default stays the original: a table built without `decimate` reads exactly as before (mip lengths all L)
    bool same = true;
    for (int j = 0; j < full.numMips(); ++j) same = same && full.mipLength(j) == L;
    ok(same && full.frameSamples() == (size_t)full.numMips() * L, "not decimated: every mip is full length, as in the original");
}

static void testWavetablePhase() {
    // a pitch at or above the sample rate (MIDI 127 plus 36 semitones of OSC OCT / SEMI is 8 times Nyquist): the phase stays in [0,1)
    nemesis::dsp::Wavetable wt;
    float sine[2048];
    for (int i = 0; i < 2048; ++i) sine[i] = (float)sin(2.0 * M_PI * i / 2048.0);
    wt.build(sine, 1, 2048);
    nemesis::dsp::WavetableOscillator o;
    o.setSampleRate(44100.0);
    o.setWavetable(wt);
    bool fin = true, inRange = true;
    for (double hz : {1000.0, 22050.0, 44100.0, 50000.0, 150000.0, 400000.0}) {
        o.reset();
        o.setFrequency(hz);
        for (int i = 0; i < 5000; ++i) {
            const float v = o.processSample();
            fin = fin && std::isfinite(v);
            inRange = inRange && o.phase() >= 0.0 && o.phase() < 1.0;
        }
    }
    ok(fin && inRange, "a wavetable oscillator at or above the sample rate keeps its phase in [0,1)");
}

static void testReviewFindings(SampleLib* lib) {
    {   // a second preview while the first still sounds: nothing hangs
        InstRig r(lib);
        r.pj->parts[0].pages[PG_AMP][3] = 0;
        r.inst->previewPreset(0, 2);
        r.run(100);
        r.inst->previewPreset(0, 2);
        r.run(100);
        r.inst->previewPreset(0, 1);
        r.run(12000);   // Dimension Bloom releases over 1.5 s
        ok(r.inst->activeVoices(0) == 0, "repeated previews all end: " + std::to_string(r.inst->activeVoices(0)) + " voices left");
    }
    {   // a live note on and off in the same block, then the sequencer: no voice is left held
        InstRig r(lib);
        r.pj->parts[0].pages[PG_AMP][3] = 0;
        r.pj->parts[0].pages[PG_AMP][2] = 127;
        Lane& a = r.pj->patterns[0].lanes[0];
        a.mute = 0; a.eu = 0; a.n = 4; for (Step& st : a.st) { st.ov = 0; st.gate = 8; } a.st[0].ov = 1;
        r.tp.playing = 1; r.tp.playGen = 1;
        r.inst->setMidi(&gIn, nullptr);
        for (int k = 0; k < 50; ++k) { midiIn(2, 1, 60 + k % 5, 100); midiIn(1, 1, 60 + k % 5, 0); r.run(7); }
        r.tp.playing = 0;
        r.run(3000);
        ok(r.inst->activeVoices(0) == 0, "live notes mixed with sequenced ones all end: " + std::to_string(r.inst->activeVoices(0)));
    }
    {   // a part turned into a sampler while its synth notes ring: the tails fade out, nothing freezes or comes back
        InstRig r(lib);
        r.pj->parts[1].pages[PG_AMP][2] = 127;
        Lane& a = r.pj->patterns[0].lanes[3];
        a.mute = 0; a.eu = 0; a.n = 1; a.st[0].ov = 1; a.st[0].gate = 127;
        r.tp.playing = 1; r.tp.playGen = 1;
        r.run(100);
        r.tp.playing = 0;
        r.pj->parts[1].type = PT_SAMPLER;
        r.run(4000);
        r.pj->parts[1].type = PT_CATHODE;
        r.run(50);
        ok(r.inst->activeVoices(1) == 0 && rms(r.L, r.L.size() - 1000, r.L.size()) < 1e-4, "after a type change and back: silence (no ghost notes)");
    }
    {   // the sampler with START past END, looped, reversed: finite, and short one-shots are heard
        Lane ln;
        makeLane(ln, 2, 60, 16, 4, 0, 127);
        ln.smpParam(PG_SMP, 1) = 127; ln.smpParam(PG_SMP, 2) = 0; ln.smpParam(PG_SMP, 3) = 2;
        SampleHit h;
        ok(makeHit(ln, nullptr, 0, 24, 120, 1.0f, *lib, 44100.0, h) && h.end > h.start, "START past END: a region of at least 4 ms at the end");
        for (int rev = 0; rev < 2; ++rev) {
            ln.smpParam(PG_SMP, 6) = (int16_t)rev;
            makeHit(ln, nullptr, 0, 24, 120, 1.0f, *lib, 44100.0, h);
            audio::Sampler s;
            s.init(44100.0);
            const int32_t hd = s.start(h, 4, 0);
            std::vector<float> L(8832), R(8832);   // whole 32-frame blocks
            for (size_t off = 0; off < L.size(); off += 32) s.render(&L[off], &R[off], 32);
            s.release(hd, 0);
            bool fin = true;
            for (float x : L) fin = fin && std::isfinite(x);
            ok(fin && rms(L, 0, L.size()) > 1e-4, std::string("a tiny looped region plays, finite") + (rev ? " (reversed)" : ""));
        }
        ln.smpParam(PG_SMP, 3) = 0; ln.smpParam(PG_SMP, 6) = 0; ln.smpParam(PG_SMP, 1) = 0; ln.smpParam(PG_SMP, 2) = 1;
        makeHit(ln, nullptr, 0, 0, 120, 1.0f, *lib, 44100.0, h);
        audio::Sampler s;
        s.init(44100.0);
        s.start(h, 4, 0);
        std::vector<float> L(4416), R(4416);   // whole 32-frame blocks
        for (size_t off = 0; off < L.size(); off += 32) s.render(&L[off], &R[off], 32);
        ok(rms(L, 0, 400) > 1e-3, "a very short one-shot is heard");
    }
}

static void testMidi(SampleLib* lib) {
    midi::SpscQueue<64>& in = gIn;
    auto send = midiIn;
    const uint8_t On = (uint8_t)midi::Type::NoteOn, Off = (uint8_t)midi::Type::NoteOff, CC = (uint8_t)midi::Type::ControlChange;
    {   // in: channel 1 plays lane 1's part; the note-off ends it
        InstRig r(lib);
        r.inst->setMidi(&in, recordOut);
        r.pj->parts[0].pages[PG_AMP][3] = 0;
        r.pj->parts[0].pages[PG_AMP][2] = 127;
        send(On, 1, 60, 100);
        r.run(200);
        ok(rms(r.L, 0, r.L.size()) > 0.005 && r.inst->activeVoices(0) == 1, "MIDI ch 1: lane 1's part plays");
        send(Off, 1, 60, 0);
        r.run(2000);
        ok(r.inst->activeVoices(0) == 0, "and its note-off ends it");
        send(On, 1, 62, 100);
        send(On, 1, 64, 0);   // velocity 0 = off (for a note never on: nothing)
        r.run(100);
        send(CC, 1, 123, 0);
        r.run(2000);
        ok(r.inst->activeVoices(0) == 0, "all notes off ends what the channel holds");
    }
    {   // in: the shared drum channel plays the lane whose trigger note it is
        InstRig r(lib);
        r.inst->setMidi(&in, recordOut);
        send(On, 10, 40, 100);
        r.run(300);
        ok(rms(r.L, 0, r.L.size()) < 1e-6, "ch 10 note 40: no lane, silence");
        send(On, 10, 36, 100);
        send(Off, 10, 36, 0);
        r.run(300);
        ok(rms(r.L, 0, r.L.size()) > 0.01, "ch 10 note 36: lane 8 (kick) sounds");
    }
    {   // in: a channel no lane uses plays the part on screen
        InstRig r(lib);
        r.inst->setMidi(&in, recordOut);
        r.inst->setLivePart(1);
        r.pj->parts[1].pages[PG_AMP][2] = 127;
        send(On, 12, 60, 100);
        r.run(200);
        ok(r.inst->activeVoices(1) == 1 && r.inst->activeVoices(0) == 0, "ch 12 (no lane): the part on screen plays");
        r.inst->setLivePart(0);   // the screen moves on while the note is held: its note-off still ends it
        send(Off, 12, 60, 0);
        r.run(3000);
        ok(r.inst->activeVoices(1) == 0, "ch 12's note-off ends it on the part it started on");
    }
    {   // out: each lane's notes go out on its channel, every note-on gets its note-off
        InstRig r(lib);
        gSent.clear();
        r.inst->setMidi(&in, recordOut);
        Lane& a = r.pj->patterns[0].lanes[0];
        a.mute = 0; a.eu = 0; a.n = 4; for (Step& st : a.st) { st.ov = 1; st.gate = 16; }   // a note on every step, half its length (32 = one step)
        r.pj->swing = 0;   // every step as long as the next
        r.pj->patterns[0].lanes[7].mute = 0;   // the kick, on 10
        r.pj->patterns[0].lanes[3].mute = 0;
        r.pj->midi[3].ch = 0;                   // a lane with MIDI off sends nothing
        r.tp.playing = 1; r.tp.playGen = 1;
        r.run(3000);
        // stop while lane 1's note is held: 10 blocks after one starts
        for (int k = 0; k < 2000; ++k) { const size_t n = gSent.size(); r.run(1); bool on = false; for (size_t i = n; i < gSent.size(); ++i) on = on || gSent[i].st == 0x90; if (on) break; }
        r.run(10);
        const int synthOn = countSent(0x90), kickOn = countSent(0x99, 36);
        ok(synthOn > 0 && kickOn > 0, "lane 1 sends on ch 1, the kick sends note 36 on ch 10: " + std::to_string(synthOn) + " " + std::to_string(kickOn));
        ok(countSent(0x99) == kickOn, "the kick sends only its trigger note");
        int ch4 = 0;
        for (const Sent& m : gSent) ch4 += (m.st & 0x0F) == 3;
        ok(ch4 == 0, "a lane with MIDI off sends nothing");
        r.tp.playing = 0;
        r.inst->stopAllSound();
        r.run(2);
        std::string why;
        ok(balanced(why), "every note-on has its note-off after STOP: " + why);
        // PLAY again at once, while the old note's end is still due: it must not end the new note
        const size_t mark = gSent.size();
        r.tp.playGen = 2; r.tp.playing = 1;
        r.run(4);   // the first new step starts at once
        const int newOn = countSent(0x90);
        r.run(2000);
        r.tp.playing = 0;
        r.inst->stopAllSound();
        r.run(2000);
        ok(balanced(why), "PLAY again inside the old gates, then STOP: still balanced: " + why);
        // lane 1's gate, from its first note; the first new note must last as long (an old end must not cut it)
        auto gateOf = [](size_t from) {
            for (size_t i = from; i < gSent.size(); ++i)
                if (gSent[i].st == 0x90)
                    for (size_t j = i + 1; j < gSent.size(); ++j)
                        if (gSent[j].st == 0x80 && gSent[j].d1 == gSent[i].d1) return gSent[j].block - gSent[i].block;
            return -1;
        };
        const int gate = gateOf(0), again = gateOf(mark);
        ok(newOn > 0 && gate > 50 && again >= gate - 2, "a note started after STOP keeps its gate: " + std::to_string(again) + " of " + std::to_string(gate) + " blocks");
        r.run(500);
        ok(balanced(why), "and nothing more afterwards: " + why);
        bool velOk = true;
        for (const Sent& m : gSent) if ((m.st & 0xF0) == 0x90 && (m.d2 < 1 || m.d2 > 127)) velOk = false;
        ok(velOk, "note-on velocities 1..127");
    }
    {   // out: a note whose part is a sampler with no sample still goes out (and ends)
        static SampleLib none;   // no samples at all: no hit starts
        InstRig r(&none);
        gSent.clear();
        r.inst->setMidi(&in, recordOut);
        r.pj->patterns[0].lanes[7].mute = 0;
        r.tp.playing = 1; r.tp.playGen = 1;
        r.run(3000);
        r.tp.playing = 0;
        r.inst->stopAllSound();
        r.run(50);
        ok(countSent(0x99) > 0 && countSent(0x89) == countSent(0x99), "an unsounding lane still sends, on and off");
    }
    {   // a chord on a lane on a shared channel: one trigger note, retriggered, ended once
        InstRig r(lib);
        gSent.clear();
        r.inst->setMidi(&in, recordOut);
        r.pj->midi[0].ch = 10; r.pj->midi[0].note = 50;   // lane 1 joins the drums on 10
        Lane& a = r.pj->patterns[0].lanes[0];
        a.mute = 0; a.eu = 0; a.n = 4; for (Step& st : a.st) { st.ov = 0; st.gate = 100; } a.st[0].ov = 1; a.st[0].chord = 2;
        r.tp.playing = 1; r.tp.playGen = 1;
        r.run(3000);
        r.tp.playing = 0;
        r.inst->stopAllSound();
        r.run(3000);
        std::string why;
        ok(countSent(0x99, 50) > 0 && balanced(why), "chords on a trigger note: balanced: " + why);
        int maxOn = 0, on = 0;
        for (const Sent& m : gSent) if (m.d1 == 50) { on += m.st == 0x99 ? 1 : -1; maxOn = std::max(maxOn, on); }
        ok(maxOn == 1, "never two starts of the trigger note without an end between: " + std::to_string(maxOn));
    }
    {   // the socket behind: a note-on that is not queued is not ended either
        InstRig r(lib);
        gSent.clear();
        static int budget = 0;
        r.inst->setMidi(&in, [](const uint8_t* b, int n) {
            bool on = false;
            for (int i = 0; i + 2 < n; i += 3) on = on || (b[i] & 0xF0) == 0x90;
            if (on && budget-- <= 0) return false;
            return recordOut(b, n);
        });
        budget = 3;
        r.pj->patterns[0].lanes[7].mute = 0;
        r.tp.playing = 1; r.tp.playGen = 1;
        r.run(3000);
        r.tp.playing = 0;
        r.inst->stopAllSound();
        r.run(500);
        std::string why;
        ok(countSent(0x99) == 3 && balanced(why), "dropped note-ons send no note-offs: " + why);
    }
    {   // the out ring keeps room for note-offs
        midi::OutRing<64> ring(3 * 8);
        int ons = 0;
        while (ring.put(0x90, 60, 100)) ++ons;
        ok(ons == (64 - 24) / 3, "note-ons stop at the reserve: " + std::to_string(ons));
        int offs = 0;
        while (ring.put(0x80, 60, 0)) ++offs;
        ok(offs >= 8, "note-offs still fit: " + std::to_string(offs));
        uint8_t b; int n = 0;
        while (ring.take(b)) ++n;
        ok(n == 3 * (ons + offs) && ring.empty() && ring.dropped() == 2, "the ring hands back every byte");
        // a retrigger (an end and a start) beyond the reserve is refused whole: the end does not eat the room kept for ends
        while (ring.put(0x90, 60, 100)) {}
        const uint32_t before = ring.size();
        const uint8_t pair[6] = {0x80, 60, 0, 0x90, 60, 100};
        ok(!ring.put(pair, 6) && ring.size() == before, "a retrigger over the reserve: nothing queued");
    }
    {   // MIDI in on a sampler lane alone on its channel: transposed from the base note
        InstRig r(lib);
        r.inst->setMidi(&in, recordOut);
        for (int i = 8; i < 10; ++i) r.pj->midi[i].ch = 0;   // the kick alone on 10
        send(On, 10, 36, 100);
        r.run(400);
        const double base = rms(r.L, 0, r.L.size());
        ok(base > 0.01, "ch 10 note 36 (the base note) plays the kick");
        send(On, 10, 127, 100);   // far above: clamped to +48, still plays
        r.run(50);
        bool finite = true;
        for (float v : r.L) finite = finite && std::isfinite(v);
        ok(finite, "a far transposition stays finite");
        send(CC, 10, 123, 0);
        r.run(10);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The effects (fx/FxSection.h): each on its own through the section, then through the instrument.

struct FxRig {
    fx::FxSection s;
    FxSettings f;
    int bpm = 120;
    float gain[fx::FxSection::kParts] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};   // the parts' faders
    std::vector<float> L, R;       // what came out
    bool allSounding = false;      // tell the section every part has sound (silent parts then go the long way round)
    FxRig() { s.init(44100.0f); defaultFx(f); }
    // one block of 32: `src(part, i)` gives each part's input sample at absolute index
    template <class F> void run(int blocks, F src) {
        for (int b = 0; b < blocks; ++b) {
            s.apply(f, bpm);
            constexpr int P = fx::FxSection::kParts;
            float pl[P][32], pr[P][32], ol[32] = {}, orr[32] = {};
            float* PL[P]; float* PR[P];
            float unity[P];
            uint32_t sounding = 0;
            const size_t base = L.size();
            for (int p = 0; p < P; ++p) {
                PL[p] = pl[p]; PR[p] = pr[p];
                unity[p] = gain[p];
                for (int i = 0; i < 32; ++i) { pl[p][i] = src(p, base + i); pr[p][i] = pl[p][i]; if (pl[p][i] != 0.0f) sounding |= 1u << p; }
            }
            s.process(PL, PR, unity, allSounding ? 0x7Fu : sounding, ol, orr, 32);
            L.insert(L.end(), ol, ol + 32);
            R.insert(R.end(), orr, orr + 32);
        }
    }
    void send(int part, int eff, int pct) { f.send[part][eff] = (int16_t)(pct * 127 / 100); }
};
static double maxAbs(const std::vector<float>& v, size_t a, size_t b) {
    double m = 0;
    for (size_t i = a; i < b && i < v.size(); ++i) m = std::max(m, (double)fabsf(v[i]));
    return m;
}
static double maxStep(const std::vector<float>& v, size_t a, size_t b) {
    double m = 0;
    for (size_t i = a + 1; i < b && i < v.size(); ++i) m = std::max(m, (double)fabsf(v[i] - v[i - 1]));
    return m;
}

static void testFxSection() {
    // (the send ramps in over a few ms, so the impulse comes after it has settled)
    auto impulse = [](int p, size_t i) { return p == 0 && i == 8000 ? 0.5f : 0.0f; };
    const size_t I0 = 8000;
    auto sine = [](int p, size_t i) { return p == 0 ? 0.3f * sinf(6.2831853f * 220.0f * (float)i / 44100.0f) : 0.0f; };
    {   // nothing in use: the section is idle (the instrument keeps its old summing)
        FxRig r;
        r.s.apply(r.f, r.bpm);
        ok(!r.s.active(), "defaults: the effects section has nothing to do");
        r.f.comp[0] = 1;
        r.s.apply(r.f, r.bpm);
        ok(r.s.active(), "the compressor on: active");
    }
    {   // DELAY: an echo at the set time, a second one fb times smaller
        FxRig r;
        r.f.delay[0] = 0;                                   // FREE
        r.f.delay[1] = (int16_t)fxDefault(FX_DELAY, 1);     // 300 ms
        r.send(0, 0, 100);
        r.run(1200, impulse);                               // ~0.87 s
        const double want = delayMs(0, r.f.delay[1], 120) * 0.001 * 44100.0;
        size_t at = 0; double best = 0;
        for (size_t i = I0 + (size_t)want - 400; i < I0 + (size_t)want + 400; ++i) if (fabsf(r.L[i]) > best) { best = fabsf(r.L[i]); at = i; }
        ok(best > 0.05 && fabs((double)(at - I0) - want) < 250, "delay: the echo is at the set time (" + std::to_string(at - I0) + " vs " + std::to_string((int)want) + ", level " + std::to_string(best) + ")");
        double second = maxAbs(r.L, I0 + (size_t)(2 * want) - 400, I0 + (size_t)(2 * want) + 400);
        ok(second > 0.08 * best && second < 0.6 * best, "delay: a second echo, quieter than the first (the loop filters take their share): " + std::to_string(second / best));
        FxRig nf;
        nf.f.delay[0] = 0; nf.f.delay[2] = 0; nf.send(0, 0, 100);   // FEEDBACK 0
        nf.run(1200, impulse);
        ok(maxAbs(nf.L, I0 + (size_t)(2 * want) - 400, I0 + (size_t)(2 * want) + 400) < 0.02 * best, "delay: no feedback, no second echo");
        ok(maxAbs(r.L, I0 + 100, I0 + (size_t)want - 400) < 0.01, "delay: nothing between the dry hit and the echo");
        r.f.delay[7] = 0;   // RETURN 0
        r.run(2, impulse);
    }
    {   // REVERB: a tail that decays by the set time
        FxRig r;
        r.f.reverb[1] = (int16_t)fxDefault(FX_REVERB, 1);   // 2.5 s
        r.send(0, 1, 100);
        r.run(4800, impulse);                               // 3.5 s
        double early = 0, late = 0;
        for (size_t i = I0 + 2000; i < I0 + 12000; ++i) early += (double)r.L[i] * r.L[i];
        for (size_t i = I0 + 100000; i < I0 + 110000 && i < r.L.size(); ++i) late += (double)r.L[i] * r.L[i];
        const double db = 10.0 * log10(late / early);
        ok(early > 1e-6 && db < -30 && db > -90, "reverb: the tail decays: " + std::to_string(db) + " dB between 0.05-0.27 s and 2.3-2.5 s");
        ok(maxAbs(r.L, I0 + 1, I0 + 3) < 0.5 && maxAbs(r.L, I0 + 2000, I0 + 8000) > 1e-3, "reverb: the tail builds up after the dry hit");
        // width: 0% = mono
        r.f.reverb[4] = 0;
        FxRig m; m.f.reverb[4] = 0; m.send(0, 1, 100);
        m.run(1500, impulse);
        double d = 0;
        for (size_t i = I0 + 5000; i < I0 + 40000; ++i) d = std::max(d, (double)fabsf(m.L[i] - m.R[i]));
        ok(d < 1e-6, "reverb: WIDTH 0 is mono: " + std::to_string(d));
    }
    {   // the level of a 100% send of noise through each return, the wet alone (the dry part is subtracted): near the noise itself
        auto noise = [](int p, size_t i) {   // white noise as a pure function of the sample index
            uint32_t x = (uint32_t)i * 2654435761u + 12345u;
            x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
            return p == 0 ? 0.2f * ((int)(x >> 8) / (float)(1 << 23) - 1.0f) : 0.0f;
        };
        for (int e = 0; e < 3; ++e) {
            FxRig r;
            r.send(0, e, 100);
            r.run(3000, noise);
            std::vector<float> src(r.L.size());
            for (size_t i = 0; i < src.size(); ++i) src[i] = noise(0, i);
            double in = 0, out = 0;
            for (size_t i = 60000; i < 90000; ++i) { const double w = r.L[i] - src[i]; out += w * w; in += (double)src[i] * src[i]; }
            const double gain = sqrt(out / in);
            printf("fx level: the wet of a full send of noise through %s is x%.2f of the noise\n", e == 0 ? "DELAY" : e == 1 ? "REVERB" : "CHORUS", gain);
            ok(gain > 0.3 && gain < 2.0, std::string("a full send through ") + (e == 0 ? "DELAY" : e == 1 ? "REVERB" : "CHORUS") + " returns about the noise's level: x" + std::to_string(gain));
        }
    }
    {   // CHORUS: finite, wet, no dry; width moves the channels apart
        FxRig r;
        r.f.chorus[4] = 127;
        r.send(0, 2, 100);
        r.run(1500, sine);
        bool fin = true;
        for (float v : r.L) fin = fin && std::isfinite(v);
        double diff = 0;
        for (size_t i = 20000; i < 40000; ++i) diff = std::max(diff, (double)fabsf(r.L[i] - r.R[i]));
        ok(fin && maxAbs(r.L, 20000, 40000) > 0.05 && diff > 0.02, "chorus: a wet stereo signal: " + std::to_string(maxAbs(r.L, 20000, 40000)) + " " + std::to_string(diff));
    }
    {   // COMP: a loud sine is pulled down by about what the ratio says
        FxRig r;
        r.f.comp[0] = 1;
        r.f.comp[1] = (int16_t)fxDefault(FX_COMP, 1);        // -18 dB
        r.f.comp[2] = 3;                                     // 4:1
        r.f.comp[5] = 0;                                     // no makeup
        auto loud = [](int p, size_t i) { return p == 0 ? 0.5f * sinf(6.2831853f * 220.0f * (float)i / 44100.0f) : 0.0f; };
        r.run(1500, loud);
        const double out = maxAbs(r.L, 40000, 47000), inp = 0.5;
        const double reduction = 20.0 * log10(inp / out);
        const double expect = (20.0 * log10(0.5) + 18.0) * (1.0 - 1.0 / 4.0);   // (-6 dB over a -18 threshold) * (1 - 1/ratio) = 9 dB
        ok(fabs(reduction - expect) < 3.5, "comp: gain reduction " + std::to_string(reduction) + " dB, expected about " + std::to_string(expect));
        FxRig q;   // below the threshold nothing happens
        q.f.comp[0] = 1;
        q.f.comp[5] = 0;     // no makeup
        auto quiet = [](int p, size_t i) { return p == 0 ? 0.02f * sinf(6.2831853f * 220.0f * (float)i / 44100.0f) : 0.0f; };
        q.run(1500, quiet);
        ok(fabs(maxAbs(q.L, 40000, 47000) / 0.02 - 1.0) < 0.1, "comp: below the threshold the signal passes: x" + std::to_string(maxAbs(q.L, 40000, 47000) / 0.02));
        // off: the sine comes through the dry bus untouched, bit for bit
        FxRig o;
        o.f.comp[0] = 0;
        o.f.amp[0][0] = 0;
        o.send(0, 0, 100);   // something must be active for the section to run
        o.run(100, loud);
        double dev = 0;
        for (size_t i = 2000; i < 3000; ++i) dev = std::max(dev, (double)fabsf(o.L[i] - loud(0, i)));
        ok(dev < 1e-6, "comp off: the dry bus carries the part unchanged: " + std::to_string(dev));
    }
    {   // AMP: distorts (harmonics appear), model OFF leaves the part untouched
        FxRig r;
        r.f.amp[0][0] = 2;                                   // crunch
        r.f.amp[0][1] = 100;                                 // drive
        r.run(2000, sine);
        bool fin = true;
        for (float v : r.L) fin = fin && std::isfinite(v);
        // the third harmonic: a Goertzel at 660 Hz against the fundamental at 220
        auto gz = [&](double hz) {
            double c = 2 * cos(2 * M_PI * hz / 44100.0), s1 = 0, s2 = 0;
            for (size_t i = 30000; i < 62000; ++i) { const double x = r.L[i] + c * s1 - s2; s2 = s1; s1 = x; }
            return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
        };
        ok(fin && maxAbs(r.L, 30000, 62000) > 0.02 && gz(660.0) > 0.05 * gz(220.0), "amp: crunch makes harmonics: 3rd/1st = " + std::to_string(gz(660.0) / gz(220.0)));
        FxRig off;
        off.f.amp[0][0] = 0;
        off.f.comp[0] = 1;
        off.f.comp[6] = 0;     // comp MIX 0 so only the dry bus remains
        off.run(300, sine);
        double dev = 0;
        for (size_t i = 2000; i < 9000; ++i) dev = std::max(dev, (double)fabsf(off.L[i] - sine(0, i)));
        ok(dev < 1e-3, "amp OFF: the part passes through unchanged: " + std::to_string(dev));
    }
    {   // no clicks: moving a return, a send or the comp makeup mid-signal does not step the output
        auto run = [&](auto move, const char* name, int eff) {
            FxRig r;
            r.send(0, eff, 100);
            r.run(800, sine);
            const size_t at = r.L.size();
            move(r);
            r.run(60, sine);
            const double before = maxStep(r.L, at - 6000, at), after = maxStep(r.L, at, at + 1800);
            ok(after < 3.0 * before + 0.01, std::string("no step when moving ") + name + ": " + std::to_string(after) + " vs " + std::to_string(before));
        };
        run([](FxRig& r) { r.f.delay[7] = 20; }, "the delay return", 0);
        run([](FxRig& r) { r.send(0, 0, 10); }, "a delay send", 0);
        run([](FxRig& r) { r.f.reverb[7] = 30; }, "the reverb return", 1);
        run([](FxRig& r) { r.f.reverb[0] = 120; }, "the reverb size", 1);
        run([](FxRig& r) { r.f.chorus[7] = 30; }, "the chorus return", 2);
        run([](FxRig& r) { r.f.reverb[4] = 0; }, "the reverb width", 1);
        run([](FxRig& r) { r.f.delay[2] = 0; }, "the delay feedback", 0);
        run([](FxRig& r) { r.f.chorus[5] = 127; }, "the chorus tone", 2);
        run([](FxRig& r) { r.f.chorus[2] = 100; }, "the chorus delay", 2);
        run([](FxRig& r) { r.f.chorus[1] = 120; }, "the chorus depth", 2);
        run([](FxRig& r) { r.f.comp[0] = 1; r.f.comp[5] = 100; }, "the compressor makeup", 0);
        {   // the amp model: the signal itself changes character, so the steps are compared with the amp's own steady sound
            FxRig r;
            r.f.amp[0][0] = 2; r.f.amp[0][1] = 90;
            r.send(0, 0, 0);
            r.run(800, sine);
            const size_t at = r.L.size();
            r.f.amp[0][0] = 3;
            r.run(60, sine);
            r.run(800, sine);
            const double steady = maxStep(r.L, at + 20000, at + 27000), around = maxStep(r.L, at, at + 3000);
            ok(around < 2.0 * steady + 0.01, "no step when switching the amp model: " + std::to_string(around) + " vs the steady " + std::to_string(steady));
        }
    }
    {   // a delay longer than a second: the echo still comes after the send has been dropped, and the effect sleeps only afterwards
        FxRig r;
        r.bpm = 40;
        r.f.delay[0] = 1;                      // 1/4 at 40 BPM: 1500 ms
        r.f.delay[2] = 0;                      // no feedback: one echo
        r.send(0, 0, 100);
        auto hit = [](int p, size_t i) { return p == 0 && i == 8000 ? 0.5f : 0.0f; };
        r.run(300, hit);
        r.send(0, 0, 0);                       // the send is dropped right after the hit
        r.run(2400, hit);                      // 1.74 s: the echo is due at 8000 + 66150
        ok(maxAbs(r.L, 8000 + 66150 - 400, 8000 + 66150 + 400) > 0.03, "a 1.5 s echo arrives although the send was dropped: " + std::to_string(maxAbs(r.L, 8000 + 66150 - 400, 8000 + 66150 + 400)));
        ok(!r.s.asleep(0), "the delay is not asleep before its echo");
        r.run(4500, hit);
        ok(r.s.asleep(0), "and sleeps once the echo is long gone");
    }
    {   // a delay that wakes goes to the time set while it slept (not to where it was)
        FxRig r;
        r.f.delay[0] = 0; r.f.delay[1] = 10; r.f.delay[2] = 0;   // FREE, short, no feedback
        r.send(0, 0, 100);
        r.run(300, [](int, size_t) { return 0.0f; });
        r.send(0, 0, 0);
        r.run(5200, [](int, size_t) { return 0.0f; });            // long enough to fall asleep (its longest time, 2 s, plus 1.5 s)
        ok(r.s.asleep(0), "the delay has fallen asleep");
        r.f.delay[1] = 127;                                        // 1000 ms while it sleeps
        r.send(0, 0, 100);
        const size_t at0 = r.L.size();
        r.run(1700, [at0](int p, size_t i) { return p == 0 && i == at0 + 6000 ? 0.5f : 0.0f; });
        const double want = 1000.0 * 44.1;
        ok(maxAbs(r.L, at0 + 6000 + (size_t)want - 600, at0 + 6000 + (size_t)want + 600) > 0.03 && maxAbs(r.L, at0 + 6000 + 1000, at0 + 6000 + (size_t)want - 3000) < 0.01,
           "a sleeping delay wakes at the time set meanwhile: the echo at 1 s, nothing before it");
    }
    {   // a send up with nothing coming in: the effect sleeps once its tail has gone, and the first sound wakes it, in that same block
        FxRig r;
        r.send(0, 1, 100);
        auto hitAt = [](size_t at) { return [at](int p, size_t i) { return p == 0 && i >= at && i < at + 2000 ? 0.3f : 0.0f; }; };
        r.run(10, hitAt(1000000));
        ok(r.s.asleep(1) && r.s.active(), "a send up with nothing coming in: asleep, and the section keeps listening");
        r.run(300, hitAt(4000));
        ok(!r.s.asleep(1), "the reverb is awake while it rings");
        r.run(12000, hitAt(4000));                                 // 8.7 s: about 4.2 s of tail to -100 dBFS and a second more
        ok(r.s.asleep(1), "and sleeps when nothing comes in and the tail has gone, the send still up");
        ok(r.s.active(), "still listening");
        const size_t at = r.L.size() + 320;
        r.run(400, hitAt(at));
        ok(!r.s.asleep(1), "the first sound wakes it");
        ok(maxAbs(r.L, at + 8000, at + 11000) > 1e-3, "and it has a tail again");
    }
    {   // the delay and the chorus sleep with a send up as well, and a hit part-way through a block wakes them at once
        for (int e : {0, 2}) {
            FxRig r;
            r.f.delay[0] = 0; r.f.delay[1] = 40; r.f.delay[2] = 0;
            r.send(0, e, 100);
            r.run(10, [](int, size_t) { return 0.0f; });
            ok(r.s.asleep(e), "asleep with a send up and nothing coming in");
            const size_t at = r.L.size() + 320 + 7;
            r.run(300, [at](int p, size_t i) { return p == 0 && i == at ? 0.5f : 0.0f; });
            ok(!r.s.asleep(e), "a hit in the middle of a block wakes it");
            const size_t from = at + 30;
            ok(maxAbs(r.L, from, r.L.size()) > 1e-3, "and its echo comes out");
        }
    }
    {   // a send raised on a part already playing, with the effect asleep: the wet signal comes in on the send's glide, no step
        for (int e : {0, 2}) {
            FxRig r;
            r.f.delay[0] = 0; r.f.delay[1] = 5; r.f.delay[2] = 0;
            auto sine = [](int p, size_t i) { return p == 0 ? 0.3f * sinf((float)i * 0.0313f) : 0.0f; };
            r.run(30, sine);
            ok(r.s.asleep(e), "the effect is asleep with its send at 0");
            const size_t at = r.L.size();
            r.send(0, e, 100);
            r.run(400, sine);
            ok(!r.s.asleep(e), "the send on a playing part wakes it");
            double worst = 0;
            for (size_t i = at + 2; i < r.L.size(); ++i) worst = std::max(worst, (double)fabsf(r.L[i] - 2.0f * r.L[i - 1] + r.L[i - 2]));
            ok(worst < 0.03, "and no step when it comes in: " + std::to_string(worst));
        }
    }
    {   // the ladder takes an effect while it sleeps: the next hit does not wake it just to fade it out
        FxRig r;
        r.send(0, 2, 100);
        r.run(10, [](int, size_t) { return 0.0f; });
        const uint16_t none[fx::FxSection::kParts] = {};
        r.s.setDegrade(1u << 13, none);                            // ChorusOff
        const size_t at = r.L.size() + 320;
        r.run(100, [at](int p, size_t i) { return p == 0 && i >= at && i < at + 2000 ? 0.3f : 0.0f; });
        ok(r.s.asleep(2), "an effect the ladder took stays asleep through a hit");
    }
    {   // an amp rests when nothing has come in, makes no sound doing it, and answers the next sound at once
        FxRig r;
        r.f.amp[0][0] = 2;                                         // CRUNCH
        r.f.amp[0][6] = 0;                                         // no cabinet: nothing but the amp to tell it from the input
        auto hitAt = [](size_t at) { return [at](int p, size_t i) { return p == 0 && i >= at && i < at + 4000 ? 0.1f * sinf((float)i * 0.03f) : 0.0f; }; };
        r.run(300, hitAt(4000));
        double changed = 0;
        for (size_t i = 6000; i < 8000; ++i) changed = std::max(changed, (double)fabsf(r.L[i] - 0.1f * sinf((float)i * 0.03f)));
        ok(changed > 0.02 && !r.s.ampResting(0), "the amp plays, and is no longer the input: " + std::to_string(changed));
        r.run(1000, hitAt(4000));
        ok(r.s.ampResting(0), "and rests after the input stopped");
        ok(maxAbs(r.L, 8000 + 22050 - 200, 8000 + 22050 + 200) < 1e-5, "with nothing of its tail left to cut off: " + std::to_string(maxAbs(r.L, 29850, 30250)));
        r.f.amp[0][1] = 90;                                        // DRIVE moved while it rests
        r.run(2, hitAt(10000000));
        ok(!r.s.ampResting(0), "a knob moved at rest wakes it, so the change has landed before the next note");
        r.run(1000, hitAt(10000000));
        ok(r.s.ampResting(0), "and it rests again");
        const size_t at = r.L.size() + 320 + 11;
        r.run(200, hitAt(at));
        double moved = 0;
        for (size_t i = at + 2000; i < at + 3000; ++i) moved = std::max(moved, (double)fabsf(r.L[i] - 0.1f * sinf((float)i * 0.03f)));
        ok(!r.s.ampResting(0) && moved > 0.02, "the next sound goes straight through it, a hit part-way through a block included");
    }
    {   // a reverb with its return at 0 still has a tail in its network: it must not fall asleep over it
        FxRig r;
        r.f.reverb[1] = 127; r.f.reverb[7] = 0;                   // 12 s, return 0
        r.send(0, 1, 100);
        r.run(300, [](int p, size_t i) { return p == 0 && i < 2000 ? 0.3f : 0.0f; });
        r.send(0, 1, 0);
        r.run(3000, [](int, size_t) { return 0.0f; });            // 2.2 s later
        ok(!r.s.asleep(1), "the reverb still holds its tail (return 0): not asleep");
    }
    {   // bad values: a non-finite or huge sample in a part is dropped before it reaches a feedback loop
        FxRig r;
        r.send(0, 0, 100); r.send(0, 1, 100); r.send(0, 2, 100);
        auto bad = [](int p, size_t i) { return p == 0 ? (i == 9000 ? NAN : i == 9100 ? INFINITY : i == 9200 ? 1e9f : 0.1f) : 0.0f; };
        r.run(600, bad);
        bool fin = true;
        for (float v : r.L) fin = fin && std::isfinite(v);
        ok(fin, "NaN, Inf and a 1e9 sample do not get through the effects");
        ok(!r.s.fault(), "and nothing was left holding garbage");
    }
    {   // the part's fader comes after its amp: a quieter part keeps the amp's tone, and just gets quieter
        FxRig a, b;
        for (FxRig* r : {&a, &b}) { r->f.amp[0][0] = 2; r->f.amp[0][1] = 100; }
        b.gain[0] = 0.25f;
        a.run(1500, sine); b.run(1500, sine);
        // the same waveform, a quarter as loud: the correlation of the two is 1 and the ratio of the peaks is 0.25
        const double pa = maxAbs(a.L, 30000, 45000), pb = maxAbs(b.L, 30000, 45000);
        ok(fabs(pb / pa - 0.25) < 0.03, "the fader after the amp: the level drops to a quarter, the shape stays: " + std::to_string(pb / pa));
    }
    {   // the degrade ladder takes effects away and gives them back (cost::Action bits: AmpToClean 9, AmpOff 10, DelayOff 11, ReverbOff 12)
        auto noise = [](int p, size_t i) {
            uint32_t x = (uint32_t)i * 2654435761u + 777u;
            x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
            return p == 0 ? 0.2f * ((int)(x >> 8) / (float)(1 << 23) - 1.0f) : 0.0f;
        };
        FxRig r;
        r.send(0, 1, 100);
        r.run(1500, noise);
        auto wetEnergy = [&](size_t a, size_t b) { double e = 0; for (size_t i = a; i < b; ++i) { const double w = r.L[i] - noise(0, i); e += w * w; } return e; };
        const double before = wetEnergy(40000, 47000);
        const uint16_t none[fx::FxSection::kParts] = {};
        r.s.setDegrade((uint16_t)(1u << 12), none);          // reverb off
        r.run(1500, noise);
        const double off = wetEnergy(r.L.size() - 7000, r.L.size());
        r.s.setDegrade(0, none);
        r.run(1500, noise);
        const double back = wetEnergy(r.L.size() - 7000, r.L.size());
        ok(before > 1e-3 && off < 1e-6 * before && back > 0.2 * before, "reverb off by the ladder: " + std::to_string(before) + " -> " + std::to_string(off) + " -> " + std::to_string(back));
        // an amp: off (bit 10) passes the part through; to clean (bit 9) is the clean model
        FxRig a2, a3;
        for (FxRig* q : {&a2, &a3}) { q->f.amp[0][0] = 2; q->f.amp[0][1] = 100; q->run(300, noise); }
        const uint16_t ampOff[fx::FxSection::kParts] = {(uint16_t)(1u << 10)}, ampClean[fx::FxSection::kParts] = {(uint16_t)(1u << 9)};
        a2.s.setDegrade(0, ampOff);
        a2.run(800, noise);
        double dev = 0;
        for (size_t i = a2.L.size() - 3000; i < a2.L.size(); ++i) dev = std::max(dev, (double)fabsf(a2.L[i] - noise(0, i)));
        ok(dev < 1e-4, "amp off by the ladder: the part passes through: " + std::to_string(dev));
        a3.s.setDegrade(0, ampClean);
        a3.run(800, noise);
        FxRig ref;
        ref.f.amp[0][0] = 1; ref.f.amp[0][1] = 100;
        ref.run(1100, noise);
        double diff = 0, ampl = 0;
        for (size_t i = a3.L.size() - 2000; i < a3.L.size(); ++i) { diff = std::max(diff, (double)fabsf(a3.L[i] - ref.L[i])); ampl = std::max(ampl, (double)fabsf(ref.L[i])); }
        ok(diff < 0.2 * ampl, "amp to clean by the ladder: it runs the clean model: " + std::to_string(diff) + " of " + std::to_string(ampl));
    }
    {   // sleep and clear: a tail ends, the section goes quiet, and clearing leaves no ghost
        FxRig r;
        r.f.reverb[1] = 0;   // the shortest decay
        r.send(0, 1, 100);
        r.run(300, sine);
        r.send(0, 1, 0);
        r.run(2500, sine);
        r.send(0, 1, 0);
        ok(r.s.active() || r.s.asleep(1), "reverb: ringing or asleep (never in between)");
        r.run(3200, impulse);    // silence from here on (the impulse is long gone)
        ok(r.s.asleep(1) && !r.s.active(), "reverb: with the send off and the tail gone the effect sleeps");
        // clear, then a send with no input: nothing comes out
        FxRig g;
        g.send(0, 0, 100);
        g.run(600, impulse);
        g.s.clear();
        g.L.clear(); g.R.clear();
        g.run(1200, [](int, size_t) { return 0.0f; });
        ok(maxAbs(g.L, 0, g.L.size()) < 1e-6, "clear: the delay holds nothing of the old sound: " + std::to_string(maxAbs(g.L, 0, g.L.size())));
    }
}

static void testFxInstrument(SampleLib* lib) {
    {   // under load the governor walks the ladder, and the effects go first: reverb off, then the rest
        InstRig r(lib);
        static float load;
        load = 1.5f;
        r.inst->setLoadSource([]() { return load; });
        r.inst->setAutoDegrade(true);
        for (int p = 0; p < 3; ++p) r.pj->fx.send[p][1] = 100;
        Lane& k = r.pj->patterns[0].lanes[7];
        k.mute = 0;
        r.tp.playing = 1; r.tp.playGen = 1;
        r.run(2000);
        ok(r.inst->degradeLevel() > 0, "under load the governor degrades: level " + std::to_string(r.inst->degradeLevel()));
        // at a deep enough level the reverb send is bypassed: the tail after the drum is gone
        load = 0.2f;
        r.inst->setAutoDegrade(false);   // (back to no degrade) the reverb returns
        r.run(1500);
        ok(r.inst->degradeLevel() == 0, "and with auto degrade off nothing is given up");
    }
    {   // effects idle: whatever else is set (returns, tones, a compressor that is off, an amp model OFF, the spare send), the output is the
        // same bit for bit: the parts are summed straight into the output as before
        InstRig a(lib), b(lib);
        for (InstRig* r : {&a, &b}) {
            for (int l : {0, 3, 7}) r->pj->patterns[0].lanes[l].mute = 0;
            r->tp.playing = 1; r->tp.playGen = 1;
        }
        b.pj->fx.delay[7] = 10; b.pj->fx.delay[3] = 99; b.pj->fx.reverb[1] = 100; b.pj->fx.reverb[7] = 3; b.pj->fx.chorus[2] = 90;
        b.pj->fx.comp[1] = 10; b.pj->fx.comp[5] = 120; b.pj->fx.amp[0][1] = 120; b.pj->fx.amp[1][4] = 5; b.pj->fx.send[0][3] = 77;
        a.run(900); b.run(900);
        ok(a.L == b.L && a.R == b.R && maxAbs(a.L, 0, a.L.size()) > 0.01 && !b.inst->effects().ampRunning(0), "effects idle but set: the output is identical, bit for bit");
        b.pj->fx.send[2][1] = 60;   // a send: now the section runs, and the output differs
        b.run(900); a.run(900);
        double d = 0;
        for (size_t i = 900 * 32; i < a.L.size(); ++i) d = std::max(d, (double)fabsf(a.L[i] - b.L[i]));
        ok(d > 1e-4, "and with a send on it is not the plain sum");
    }
    {   // a send to the reverb adds a tail the dry render does not have
        InstRig dry(lib), wet(lib);
        for (InstRig* r : {&dry, &wet}) {
            Lane& k = r->pj->patterns[0].lanes[7];
            k.mute = 0; k.eu = 0; k.n = 16; k.st[0].ov = 1;
            r->tp.playing = 1; r->tp.playGen = 1;
        }
        wet.pj->fx.send[2][1] = 100;   // the drum part's reverb send
        wet.pj->fx.reverb[1] = (int16_t)fxDefault(FX_REVERB, 1);
        dry.run(1500); wet.run(1500);
        double dd = 0, ww = 0, df = 0;
        for (size_t i = 24000; i < 48000; ++i) { dd += (double)dry.L[i] * dry.L[i]; ww += (double)wet.L[i] * wet.L[i]; const double x = wet.L[i] - dry.L[i]; df += x * x; }
        ok(df > 0.1 * dd && df > 1e-4, "a reverb send adds a tail after the drum (the part itself is untouched): added energy " + std::to_string(df) + " vs " + std::to_string(dd));
        // clearing the effects after a "project load" leaves them silent
        wet.pj->fx.send[2][1] = 0;
        wet.tp.playing = 0;
        wet.inst->stopAllSound();
        wet.inst->clearFx();
        wet.L.clear(); wet.R.clear();
        wet.run(4000);
        ok(maxAbs(wet.L, 0, wet.L.size()) < 1e-5, "clearFx: the old project's tail is gone: " + std::to_string(maxAbs(wet.L, 0, wet.L.size())));
    }
    {   // an amp on a part changes its sound and still leaves the others alone
        InstRig a(lib), b(lib);
        for (InstRig* r : {&a, &b}) {
            Lane& n = r->pj->patterns[0].lanes[0];
            n.mute = 0; n.eu = 0; n.n = 4; for (Step& st : n.st) { st.ov = 1; st.gate = 100; }
            r->tp.playing = 1; r->tp.playGen = 1;
        }
        b.pj->fx.amp[0][0] = 2; b.pj->fx.amp[0][1] = 110;
        a.run(1500); b.run(1500);
        double diff = 0;
        for (size_t i = 10000; i < 40000; ++i) diff = std::max(diff, (double)fabsf(a.L[i] - b.L[i]));
        ok(diff > 0.01, "an amp insert changes the part's sound: " + std::to_string(diff));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Wavetable banks: the store (files -> arena -> published tables) and the oscillators that play them.

struct StdioWaves : audio::WaveFile {
    std::string dir;
    FILE* f = nullptr;
    int opens = 0;
    bool open(int bank) override {
        char n[64];
        snprintf(n, sizeof n, "/wt%03d.wtp", bank);
        f = fopen((dir + n).c_str(), "rb");
        ++opens;
        return f != nullptr;
    }
    bool read(void* dst, size_t bytes) override { return f && fread(dst, 1, bytes, f) == bytes; }
    void close() override { if (f) fclose(f); f = nullptr; }
};

static void bit(uint8_t* b, int bank) { b[bank >> 3] = (uint8_t)(b[bank >> 3] | (1u << (bank & 7))); }

// Every part a sound of its own: the synths A, B, D, E, F, G at once, each on its own lane.
static void testSevenParts(SampleLib* lib) {
    static const int kSynths[6] = {0, 1, 3, 4, 5, 6};
    auto arm = [](InstRig& r, int lane, int part) {
        Lane& l = r.pj->patterns[0].lanes[lane];
        l.part = (uint8_t)part; l.mute = 0; l.eu = 0; l.n = 16; l.arp.mode = 0;
        for (Step& s : l.st) { s.ov = 0; s.chord = 0; }
        l.st[0].ov = 1; l.st[0].gate = 127;
    };
    auto rms = [](const std::vector<float>& v, size_t from, size_t to) {
        double e = 0;
        for (size_t i = from; i < to && i < v.size(); ++i) e += v[i] * v[i];
        return sqrt(e / (double)(to - from));
    };
    {   // all six at once
        InstRig r(lib);
        for (int i = 0; i < 6; ++i) arm(r, i, kSynths[i]);
        refreshIdleParts(*r.pj);
        r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0;
        int most[kParts] = {};
        for (int b = 0; b < 300; ++b) {
            r.run(1);
            for (int p = 0; p < kParts; ++p) most[p] = std::max(most[p], r.inst->activeVoices(p));
        }
        bool all = true;
        for (int p : kSynths) all = all && most[p] >= 1;
        ok(all, "six synths on six lanes all sound at once");
        ok(most[2] == 0, "and the sampler part stays out of it");
        ok(rms(r.L, 0, r.L.size()) > 1e-3, "and the output has them: " + std::to_string(rms(r.L, 0, r.L.size())));
        delete r.inst;
    }
    for (int p : kSynths) {   // each part on its own, its own fader
        InstRig r(lib);
        arm(r, 0, p);
        refreshIdleParts(*r.pj);
        r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0;
        r.run(300);
        const double on = rms(r.L, 0, r.L.size());
        InstRig q(lib);
        arm(q, 0, p);
        q.pj->parts[p].level = 0;
        q.tp.playing = 1; q.tp.gstep = -1; q.tp.pstep = 0;
        q.run(300);
        ok(on > 1e-3 && rms(q.L, 0, q.L.size()) < on * 1e-3, std::string("part ") + kPartName[p] + " sounds, and its fader is its own");
        delete r.inst; delete q.inst;
    }
    {   // the last part's reverb: what is left after the note is the send's
        InstRig r(lib), q(lib);
        for (InstRig* x : {&r, &q}) {
            arm(*x, 0, 6);
            x->pj->fx.send[6][1] = 127;
            x->tp.playing = 1; x->tp.gstep = -1; x->tp.pstep = 0;
        }
        q.pj->fx.send[6][1] = 0;
        r.run(1400); q.run(1400);
        ok(rms(r.L, 40000, 44000) > rms(q.L, 40000, 44000) * 3.0, "part G's send reaches the reverb");
        delete r.inst; delete q.inst;
    }
    {   // a looped hit that has decayed to nothing must still end when it is stopped (its fade step is too small to reach 0)
        Lane ln;
        makeLane(ln, 2, 60, 16, 4, 0, 127);
        ln.smpParam(PG_SMP, 3) = 2;
        ln.smpParam(PG_SAMP, 1) = 0;
        ln.smpParam(PG_SAMP, 2) = 0;
        SampleHit h;
        makeHit(ln, nullptr, 0, 0, 120, 1.0f, *lib, 44100.0, h);
        audio::Sampler s;
        s.init(44100.0);
        s.start(h, 4, 0);
        float l[32], r[32];
        for (int i = 0; i < 6000; ++i) { memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r)); s.render(l, r, 32); }
        s.stopAll();
        for (int i = 0; i < 2000; ++i) { memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r)); s.render(l, r, 32); }
        ok(s.active() == 0, "a looped hit decayed to nothing ends when it is stopped");
    }
    auto tone = [](int p, size_t i) { return p == 0 ? 0.3f * (float)sin((double)i * 0.05) : 0.0f; };
    {   // a part with nothing in its buffers is skipped, and nothing changes for it
        FxRig a, b;
        b.allSounding = true;
        for (FxRig* r : {&a, &b}) { r->send(0, 1, 80); r->send(6, 0, 50); r->f.amp[0][0] = 2; }
        a.run(800, tone); b.run(800, tone);
        bool same = a.L.size() == b.L.size();
        for (size_t i = 0; same && i < a.L.size(); ++i) same = a.L[i] == b.L[i] && a.R[i] == b.R[i];
        ok(same, "the output is bit for bit the same with the silent parts skipped");
    }
    {   // an amp's tail on a part that has stopped is not lost
        FxRig a, b;
        b.allSounding = true;
        for (FxRig* r : {&a, &b}) r->f.amp[0][0] = 2;
        auto burst = [](int p, size_t i) { return p == 0 && i < 8000 ? 0.3f * (float)sin((double)i * 0.05) : 0.0f; };
        a.run(600, burst); b.run(600, burst);
        bool same = a.L.size() == b.L.size();
        for (size_t i = 0; same && i < a.L.size(); ++i) same = a.L[i] == b.L[i];
        ok(same, "and so is the tail of an amp whose part has stopped");
    }
}

// A sliced loop through the real sampler: what comes out is the part of the sample each slice names.
static void testSliceAudio(SampleLib* lib) {
    // a loop of four tones, a second long, each tone a quarter of it. The zero crossings do not matter here
    static std::vector<int16_t> loop(44100);
    const double hz[4] = {220, 440, 880, 1760};
    for (int i = 0; i < 44100; ++i) loop[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * hz[i / 11025] * i / 44100.0));
    SampleItem& it = lib->items[0];
    const SampleItem keep = it;
    it.data = loop.data();
    it.length = 44100;
    it.rate = 44100;
    auto freq = [](const std::vector<float>& v, size_t from, size_t to) {   // by zero crossings
        int c = 0;
        for (size_t i = from + 1; i < to && i < v.size(); ++i) c += (v[i - 1] < 0) != (v[i] < 0);
        return c * 44100.0 / 2.0 / (double)(to - from);
    };
    auto peakOf = [](const std::vector<float>& v, size_t from, size_t to) { float p = 0; for (size_t i = from; i < to && i < v.size(); ++i) p = std::max(p, fabsf(v[i])); return p; };
    auto setup = [&](InstRig& r, int mode, int bpm) {
        Lane& l = r.pj->patterns[0].lanes[7];
        l.mute = 0; l.eu = 0; l.n = 16; l.arp.mode = 0;
        for (Step& s : l.st) { s.ov = 0; s.chord = 0; s.nLocks = 0; }
        l.smpParam(PG_SMP, 0) = 0;
        l.smpParam(PG_SLC, SLC_MODE) = (int16_t)mode;
        l.smpParam(PG_SLC, SLC_BEATS) = 1;   // 2 beats
        SliceMap* m = sliceMapFor(*r.pj, 0);
        memset(m, 0, sizeof(*m));
        m->sample = 0; m->frames = 44100; m->count = 4;
        for (int i = 0; i < 4; ++i) { m->s[i].start = (uint32_t)i * 11025; m->s[i].level = 127; }
        r.pj->bpm = (uint16_t)bpm;
        r.pj->swing = 0;
        r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0;
    };
    {   // SYNC at the loop's own tempo: the loop comes out as it went in
        InstRig r(lib);
        setup(r, SLICE_SYNC, 120);
        r.run(1800);
        for (int k = 0; k < 4; ++k) {
            const size_t a = (size_t)k * 11025 + 2500;
            const double f = freq(r.L, a, a + 6000);
            ok(fabs(f - hz[k]) < hz[k] * 0.03, "SYNC plays slice " + std::to_string(k + 1) + " in its place: " + std::to_string(f) + " Hz");
        }
        ok(fabs(freq(r.L, 44100 + 2500, 44100 + 8500) - hz[0]) < hz[0] * 0.03, "and comes round in time");
        delete r.inst;
    }
    {   // at the loop's own tempo the slices add up to the loop: no hole where one takes over from the next, on noise, which forgives nothing
        static std::vector<int16_t> noise(44100);
        uint32_t x = 99;
        for (int i = 0; i < 44100; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; noise[i] = (int16_t)((int)(x >> 17) - 16384); }
        const int16_t* tones = it.data;
        it.data = noise.data();
        auto render = [&](int mode) {
            InstRig r(lib);
            setup(r, mode, 120);
            Lane& l = r.pj->patterns[0].lanes[7];
            l.smpParam(PG_SFLT, 1) = 127;
            if (mode == SLICE_OFF) { l.st[0].ov = 1; }
            SliceMap* m = sliceMapFor(*r.pj, 0);
            for (int i = 1; i < 4; ++i) m->s[i].start -= 37u * (uint32_t)i;   // cut a little before the beat, as at a zero crossing
            r.run(1300);
            std::vector<float> out = r.L;
            delete r.inst;
            return out;
        };
        const std::vector<float> whole = render(SLICE_OFF), cut = render(SLICE_SYNC);
        double err = 0, sig = 0;
        for (size_t i = 200; i < 40000; ++i) { const double d = whole[i] - cut[i]; err += d * d; sig += whole[i] * whole[i]; }
        const double db = 10.0 * log10((err + 1e-20) / (sig + 1e-20));
        ok(db < -40.0, "SYNC at the loop's tempo gives back the loop: residual " + std::to_string(db) + " dB");
        it.data = tones;
    }
    {   // FIT PITCH: half the tempo, an octave down, and still the whole loop
        InstRig r(lib);
        setup(r, SLICE_SYNC, 60);
        r.pj->patterns[0].lanes[7].smpParam(PG_SLC, SLC_FIT) = FIT_PITCH;
        r.run(2800);
        const double f = freq(r.L, 3000, 15000);
        ok(fabs(f - hz[0] / 2) < hz[0] * 0.03, "FIT PITCH at half tempo: an octave down: " + std::to_string(f));
        const double g = freq(r.L, 3000 + 22050, 15000 + 22050);
        ok(fabs(g - hz[1] / 2) < hz[1] * 0.03, "with no gap, each slice in its stretched place: " + std::to_string(g));
        delete r.inst;
    }
    {   // FIT SLICES: half the tempo, the same pitch, and the slice ends before the next one
        InstRig r(lib);
        setup(r, SLICE_SYNC, 60);
        r.run(2800);
        ok(fabs(freq(r.L, 3000, 9000) - hz[0]) < hz[0] * 0.03, "FIT SLICES at half tempo keeps the pitch");
        ok(peakOf(r.L, 15000, 21000) < 0.005f, "and leaves a gap: " + std::to_string(peakOf(r.L, 15000, 21000)));
        delete r.inst;
    }
    {   // faster than the loop: a slice is cut by the next, one voice at a time, with no click
        InstRig r(lib);
        setup(r, SLICE_SYNC, 240);
        int most = 0;
        for (int b = 0; b < 1200; ++b) { r.run(1); most = std::max(most, r.inst->activeVoices(2)); }
        ok(most <= 2, "a lane's slices cut each other (one fading): " + std::to_string(most));
        float jump = 0, top = 0;
        for (size_t i = 1; i < r.L.size(); ++i) { jump = std::max(jump, fabsf(r.L[i] - r.L[i - 1])); top = std::max(top, fabsf(r.L[i])); }
        ok(jump < top * 0.85f, "and the cuts do not click: jump " + std::to_string(jump) + " of " + std::to_string(top));
        delete r.inst;
    }
    {   // a slice's own settings
        InstRig r(lib);
        setup(r, SLICE_STEP, 120);
        Lane& l = r.pj->patterns[0].lanes[7];
        l.st[0].ov = 1;
        l.st[0].setLock(lockKey(PG_SLC, SLC_SLICE), 2);
        sliceMapFor(*r.pj, 0)->s[1].pitch = 12;
        r.run(200);
        const double f = freq(r.L, 1000, 4800);
        ok(fabs(f - hz[1] * 2) < hz[1] * 0.06, "a slice's PITCH: " + std::to_string(f));
        delete r.inst;
    }
    {   // a sample that is not the file it was cut from: a slice number means nothing in it, so nothing plays until it is cut again
        InstRig r(lib);
        setup(r, SLICE_STEP, 120);
        Lane& l = r.pj->patterns[0].lanes[7];
        l.st[0].ov = 1;
        sliceMapFor(*r.pj, 0)->frames = 12345;
        r.run(200);
        ok(rms(r.L, 0, 6000) < 1e-6, "a map for another length plays nothing, not the whole sample");
        delete r.inst;
    }
    {   // STOP and mute end a loop playing by itself
        InstRig r(lib);
        setup(r, SLICE_SYNC, 120);
        Lane& l = r.pj->patterns[0].lanes[7];
        r.run(200);
        ok(r.inst->activeVoices(2) >= 1, "the loop is playing");
        r.tp.playing = 0;
        r.run(30);
        ok(r.inst->activeVoices(2) == 0, "STOP ends it");
        r.tp.playing = 1; r.tp.gstep = -1; r.tp.pstep = 0; r.tp.playGen = r.tp.playGen + 1;
        r.run(200);
        l.mute = 1;
        r.run(250);
        ok(r.inst->activeVoices(2) == 0, "and so does MUTE, at the next step");
        delete r.inst;
    }
    {   // two slices in one block, and a chord: each plays
        InstRig r(lib);
        setup(r, SLICE_STEP, 120);
        Lane& l = r.pj->patterns[0].lanes[7];
        l.st[0].ov = 1;
        l.st[0].chord = 1;
        int most = 0;
        for (int b = 0; b < 100; ++b) { r.run(1); most = std::max(most, r.inst->activeVoices(2)); }
        ok(most >= 2, "a chord on a sliced lane plays all its notes: " + std::to_string(most));
        delete r.inst;
    }
    it = keep;
}

// A sample looping through the real sampler.
static void testLoopAudio(SampleLib* lib) {
    static std::vector<int16_t> d(44100);
    SampleItem& it = lib->items[0];
    const SampleItem keep = it;
    it.data = d.data();
    it.length = 44100;
    it.rate = 44100;
    auto play = [&](int fadeVal, int curve, bool reverse, int blocks = 2000, int startKnob = 0) {
        Lane ln;
        makeLane(ln, 2, 60, 16, 4, 0, 127);
        ln.smpParam(PG_SMP, 0) = 0;
        ln.smpParam(PG_SMP, 3) = 2;
        ln.smpParam(PG_SMP, 6) = reverse ? 1 : 0;
        ln.smpParam(PG_SMP, 1) = (int16_t)startKnob;
        ln.smpParam(PG_SFLT, 1) = 127;
        ln.smpParam(PG_SFLT, 2) = 0;
        ln.smpParam(PG_SAMP, 2) = 127;
        ln.smpParam(PG_LOOP, LP_UNIT) = LOOP_PCT;
        ln.smpParam(PG_LOOP, LP_XFADE) = (int16_t)fadeVal;
        ln.smpParam(PG_LOOP, LP_CURVE) = (int16_t)curve;
        SampleHit h;
        makeHit(ln, nullptr, 0, 0, 120, 1.0f, *lib, 44100.0, h);
        audio::Sampler s;
        s.init(44100.0);
        s.start(h, 4, 0);
        std::vector<float> L((size_t)blocks * 32, 0.0f), R((size_t)blocks * 32, 0.0f);
        for (int b = 0; b < blocks; ++b) s.render(&L[(size_t)b * 32], &R[(size_t)b * 32], 32);
        return L;
    };
    auto worst = [](const std::vector<float>& v, size_t from) { float j = 0; for (size_t i = from + 1; i < v.size(); ++i) j = std::max(j, fabsf(v[i] - v[i - 1])); return j; };
    // a smooth wave that does not fit its loop: the plain jump is the biggest step in it
    for (int i = 0; i < 44100; ++i) d[i] = (int16_t)(12000.0 * sin(i * 0.00731) + 8000.0 * sin(i * 0.0173));
    {
        const float plain = worst(play(0, CURVE_LINEAR, false), 3000);
        const float faded = worst(play(30, CURVE_LINEAR, false), 3000);
        ok(faded < plain, "a crossfade takes the jump out of the loop point: " + std::to_string(plain) + " -> " + std::to_string(faded));
    }
    // noise, whose two ends have nothing to do with each other: the fade's middle is the test
    uint32_t x = 5;
    for (int i = 0; i < 44100; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; d[i] = (int16_t)((int)(x >> 17) - 16384); }
    auto dip = [&](int curve) {
        const std::vector<float> v = play(127, curve, false, 3400);
        // the fade is half the loop, so a whole lap is half the sample and the middle of the fade comes round: the level there against the level away from it
        double mid = 0, side = 0;
        const size_t lap = 22050;
        int nm = 0, ns = 0;
        for (size_t k = 2; (k + 1) * lap < v.size(); ++k) {
            for (size_t i = 0; i < 2000; ++i) { const double a = v[k * lap + 11025 - 1000 + i]; mid += a * a; ++nm; }
            for (size_t i = 0; i < 400; ++i) { const double a = v[k * lap + i]; side += a * a; ++ns; }
        }
        return 10.0 * log10((mid / nm + 1e-12) / (side / ns + 1e-12));
    };
    const double linear = dip(CURVE_LINEAR), power = dip(CURVE_POWER), lg = dip(CURVE_LOG), match = dip(CURVE_MATCH);
    ok(fabs(match) < 1.0, "MATCH on sounds with nothing in common holds the power: " + std::to_string(match) + " dB");
    ok(linear < -2.0, "a linear fade between unrelated sounds dips: " + std::to_string(linear) + " dB");
    ok(fabs(power) < 1.0, "an equal-power fade does not: " + std::to_string(power) + " dB");
    ok(lg > linear + 1.0 && fabs(lg) < 1.0, "the log fade holds about the same: " + std::to_string(lg) + " dB");
    {   // in step: the same tone at both ends, and a linear fade is seamless
        for (int i = 0; i < 44100; ++i) d[i] = (int16_t)(16000.0 * sin(2 * M_PI * 100.0 * i / 44100.0));
        const std::vector<float> v = play(60, CURVE_LINEAR, false);
        float lo = 1e9f, hi = 0;
        for (size_t a = 44100; a + 882 < v.size(); a += 441) { float p = 0; for (size_t i = a; i < a + 882; ++i) p = std::max(p, fabsf(v[i])); lo = std::min(lo, p); hi = std::max(hi, p); }
        ok(lo > hi * 0.97f, "a linear fade in step holds its level: " + std::to_string(lo) + " .. " + std::to_string(hi));
    }
    {   // the fade does not change the lap when there is sample before START to fade from: a click marks the lap
        for (int i = 0; i < 44100; ++i) d[i] = 0;
        for (int i = 30000; i < 30010; ++i) d[i] = 20000;
        auto lapOf = [&](int fadeVal) {
            const std::vector<float> v = play(fadeVal, CURVE_LINEAR, false, 3000, 32);   // START about a quarter in
            std::vector<size_t> at;
            for (size_t i = 1; i < v.size(); ++i) if (fabsf(v[i]) > 0.1f && fabsf(v[i - 1]) <= 0.1f && (at.empty() || i > at.back() + 100)) at.push_back(i);
            return at.size() >= 2 ? at[at.size() - 1] - at[at.size() - 2] : (size_t)0;
        };
        const size_t plain = lapOf(0), faded = lapOf(10);
        ok(plain > 30000 && plain == faded, "XFADE leaves the lap as it is: " + std::to_string(plain) + " and " + std::to_string(faded));
        for (int i = 0; i < 44100; ++i) d[i] = (int16_t)(16000.0 * sin(2 * M_PI * 100.0 * i / 44100.0));
    }
    {   // MATCH on two ends in step: it knows they are alike, and holds the level where equal power would swell
        Lane ln;
        makeLane(ln, 2, 60, 16, 4, 0, 127);
        ln.smpParam(PG_SMP, 3) = 2; ln.smpParam(PG_SFLT, 1) = 127; ln.smpParam(PG_SAMP, 2) = 127;
        ln.smpParam(PG_LOOP, LP_UNIT) = LOOP_PCT; ln.smpParam(PG_LOOP, LP_XFADE) = 60; ln.smpParam(PG_LOOP, LP_CURVE) = CURVE_MATCH;
        auto range = [&](float corr) {
            SampleHit h;
            makeHit(ln, nullptr, 0, 0, 120, 1.0f, *lib, 44100.0, h);
            h.start = 11025; h.end = 33516; h.loopFade = 5000; h.loopTurn = 11025; h.loopLen = 22491; h.loopPct = 0;   // a lap of 51 periods of the 100 Hz tone
            h.loopCorr = corr;
            audio::Sampler s;
            s.init(44100.0);
            s.start(h, 4, 0);
            std::vector<float> v(200000, 0.0f), w(200000, 0.0f);
            for (size_t o = 0; o + 32 <= v.size(); o += 32) s.render(&v[o], &w[o], 32);
            float lo = 1e9f, hi = 0;
            for (size_t a = 22050; a + 882 < v.size(); a += 441) { float p = 0; for (size_t i = a; i < a + 882; ++i) p = std::max(p, fabsf(v[i])); lo = std::min(lo, p); hi = std::max(hi, p); }
            return 20.0 * log10(hi / lo);
        };
        const double alike = range(1.0f), assumed = range(0.0f);
        ok(alike < 0.3, "told the ends are alike, the level stays: " + std::to_string(alike) + " dB");
        ok(assumed > 2.0, "told they are not, it would swell: " + std::to_string(assumed) + " dB");
    }
    {   // % is a share of the loop that plays, not of the knobs' loop
        Lane ln;
        makeLane(ln, 2, 60, 16, 4, 0, 127);
        ln.smpParam(PG_SMP, 3) = 2;
        ln.smpParam(PG_LOOP, LP_UNIT) = LOOP_PCT; ln.smpParam(PG_LOOP, LP_XFADE) = 25; ln.smpParam(PG_LOOP, LP_AUTO) = AUTO_AMP;
        InstRig r(lib);
        LoopFit* f = loopFitFor(*r.pj);
        memset(f, 0, sizeof(*f));
        f->sample = 0; f->wantS = ln.smpParam(PG_SMP, 1); f->wantE = ln.smpParam(PG_SMP, 2); f->algo = AUTO_AMP; f->search = (uint8_t)ln.smpParam(PG_LOOP, LP_SEARCH);
        f->frames = 44100; f->start = 1000; f->end = 3000;
        SampleHit h;
        makeHit(ln, nullptr, 0, 0, 120, 1.0f, *lib, 44100.0, h);
        applyLoopFit(h, *r.pj, 0);
        ok(fabs(h.loopFade - 2000.0 * 25 / 127 * 0.5) < 1.0, "XFADE in % follows the found loop: " + std::to_string(h.loopFade));
        ln.smpParam(PG_SLC, SLC_MODE) = SLICE_STEP;
        makeHit(ln, nullptr, 0, 0, 120, 1.0f, *lib, 44100.0, h);
        SliceMap m;
        memset(&m, 0, sizeof(m));
        m.sample = 0; m.frames = 44100; m.count = 2; m.s[0].level = 127; m.s[1].start = 800; m.s[1].level = 127;
        ok(applySlice(h, ln, m, 0, false, 120, 7) && h.end == 800 && fabs(h.loopFade - 800.0 * 25 / 127 * 0.5) < 1.0, "and a looped slice");
        delete r.inst;
    }
    {   // backwards too, and it keeps going
        for (int i = 0; i < 44100; ++i) d[i] = (int16_t)(12000.0 * sin(i * 0.00731) + 8000.0 * sin(i * 0.0173));
        const std::vector<float> v = play(30, CURVE_POWER, true, 6000);
        ok(rms(v, 150000, 190000) > 0.01, "a reversed loop with a fade keeps playing");
        ok(worst(v, 3000) < worst(play(0, CURVE_POWER, true), 3000), "and melts as well");
    }
    {   // off is off: the same output as before there was a fade
        const std::vector<float> a = play(0, CURVE_LINEAR, false), b = play(0, CURVE_POWER, false);
        ok(a == b, "with no XFADE the curve does nothing");
    }
    {   // AUTO through the instrument: the found points are the ones played, with a fade, and no click
        InstRig r(lib);
        Lane& l = r.pj->patterns[0].lanes[7];
        l.mute = 0; l.eu = 0; l.n = 16; l.arp.mode = 0;
        for (Step& s : l.st) { s.ov = 0; s.chord = 0; s.nLocks = 0; }
        l.st[0].ov = 1; l.st[0].gate = 127;
        l.smpParam(PG_SMP, 0) = 0; l.smpParam(PG_SMP, 3) = 2;
        l.smpParam(PG_LOOP, LP_AUTO) = AUTO_AMP;
        LoopFit* f = loopFitFor(*r.pj);
        memset(f, 0, sizeof(*f));
        f->sample = 0; f->wantS = l.smpParam(PG_SMP, 1); f->wantE = l.smpParam(PG_SMP, 2); f->algo = AUTO_AMP; f->search = (uint8_t)l.smpParam(PG_LOOP, LP_SEARCH);
        f->frames = 44100; f->start = 1000; f->end = 3000;
        SampleHit h;
        makeHit(l, nullptr, 0, 0, 120, 1.0f, *lib, 44100.0, h);
        applyLoopFit(h, *r.pj, 0);
        ok(h.start == 1000 && h.end == 3000, "a hit plays the points AUTO found");
        f->frames = 12345;
        makeHit(l, nullptr, 0, 0, 120, 1.0f, *lib, 44100.0, h);
        applyLoopFit(h, *r.pj, 0);
        ok(h.end > 40000, "and the knobs' points if those were for another file");
        delete r.inst;
    }
    it = keep;
}

// Samples and wavetables in one memory.
static void testMemoryPool() {
    audio::MemoryPool p;
    p.init(9000000, 500008, 6);
    ok(p.room() == 9000000 && p.slotsThatFit() == 6, "an empty pool is all free");
    p.useSlots(2);
    ok(p.room() == 9000000 - 2 * 500008, "two wavetables take their share, and only those");
    p.useSamples(p.top());
    ok(p.room() == 0 && !p.fitsSample(1), "the samples stop at the wavetables");
    ok(p.slotsThatFit() == 2, "and the wavetables at the samples");
    p.useSlots(0);
    ok(p.fitsSample(1000000), "a wavetable let go gives its room to the samples");
    p.useSamples(0);
    p.useSlots(6);
    ok(p.room() == 9000000 - 6 * 500008 && p.room() > 0, "six wavetables leave the rest");
}

// Taking sound in.
// a take at the end of the list, behind the card's sorted run
static void testSampleFind() {
    static app::SampleLib lib;
    static const char* const names[] = {"Kick/a.wav", "Snare/b.wav", "Tom/c.wav"};
    for (const char* n : names) { lib.items[lib.count].path = n; ++lib.count; }
    lib.sortedEnd = lib.count;
    lib.items[lib.count].path = "REC/REC0001.wav";
    ++lib.count;
    ok(lib.find("REC/REC0001.wav") == 3, "a take is found behind the sorted run");
    ok(lib.find("Tom/c.wav") == 2, "and the last of the run still is");
    ok(lib.find("Nope/x.wav") == -1, "what is not there is not found");
}

static void testRecorder() {
    const uint32_t rate = 44100;
    static std::vector<int16_t> mem(rate * 4);
    auto rxOf = [](const std::vector<float>& in, size_t at, int n, std::vector<uint32_t>& rx) {
        rx.resize((size_t)n * 2);
        for (int i = 0; i < n; ++i) {
            const float v = at + i < in.size() ? in[at + i] : 0.0f;
            rx[2 * i] = rx[2 * i + 1] = (uint32_t)(int32_t)(v * 2147483000.0f);
        }
    };
    auto run = [&](audio::Recorder& r, const std::vector<float>& in) {
        std::vector<uint32_t> rx;
        float l[32] = {}, q[32] = {};
        for (size_t at = 0; at + 32 <= in.size(); at += 32) { rxOf(in, at, 32, rx); r.block(rx.data(), l, q, 32); }
    };
    auto tone = [&](double secs, float level, double from = 0.0) {
        std::vector<float> v((size_t)(secs * rate), 0.0f);
        for (size_t i = (size_t)(from * rate); i < v.size(); ++i) v[i] = level * (float)sin(2 * M_PI * 440.0 * i / rate);
        return v;
    };
    {   // by hand
        audio::Recorder r;
        audio::Recorder::Config c;
        r.setListening(true);
        r.start(mem.data(), (uint32_t)mem.size(), c);
        ok(r.state() == audio::Recorder::Capturing, "by hand it starts at once");
        run(r, tone(0.5, 0.5f));
        const uint32_t n = r.frames();
        ok(n > rate * 0.49 && n <= rate / 2, "and takes what comes: " + std::to_string(n));
        r.stop();
        ok(r.state() == audio::Recorder::Done, "REC ends it");
        double top = 0;
        for (uint32_t i = 2000; i < n; ++i) top = std::max(top, fabs((double)mem[i]));
        ok(fabs(top / 32767.0 - 0.5) < 0.02, "at the level it came in: " + std::to_string(top / 32767.0));
        ok(r.peak() > 0.45f && r.peak() == 0.0f, "the meter shows the peak, and starts again from there");
    }
    {   // auto capture: nothing until the input passes the threshold, then the take starts a little before it
        audio::Recorder r;
        audio::Recorder::Config c;
        c.autoCapture = true;
        c.threshold = 0.1f;
        r.setListening(true);
        r.start(mem.data(), (uint32_t)mem.size(), c);
        ok(r.state() == audio::Recorder::Armed, "AUTO waits");
        std::vector<float> quiet = tone(0.3, 0.02f);
        run(r, quiet);
        ok(r.state() == audio::Recorder::Armed && r.frames() == 0, "a sound under the threshold is not it");
        std::vector<float> tick(4410, 0.0f);
        tick[1000] = 0.9f; tick[1001] = -0.9f;
        run(r, tick);
        ok(r.state() == audio::Recorder::Armed, "and neither is a tick of noise");
        std::vector<float> in((size_t)rate, 0.0f);
        for (size_t i = 20000; i < in.size(); ++i) in[i] = 0.5f * (float)sin(2 * M_PI * 440.0 * i / rate);
        run(r, in);
        ok(r.state() == audio::Recorder::Capturing, "the sound starts it");
        // where the sound is in the take: the pre-roll is in front of it
        uint32_t first = 0;
        for (uint32_t i = 0; i < r.frames(); ++i) if (abs(mem[i]) > 1000) { first = i; break; }
        ok(first > audio::Recorder::kPreRoll - 60 && first <= audio::Recorder::kPreRoll, "50 ms of what led up to it are in the take: " + std::to_string(first));
    }
    {   // an armed recorder that hears a sound before the pre-roll is full keeps what there is
        audio::Recorder r;
        audio::Recorder::Config c;
        c.autoCapture = true;
        c.threshold = 0.1f;
        r.setListening(true);
        r.start(mem.data(), (uint32_t)mem.size(), c);
        run(r, tone(0.2, 0.5f, 0.01));
        uint32_t first = 0;
        for (uint32_t i = 0; i < r.frames(); ++i) if (abs(mem[i]) > 1000) { first = i; break; }
        ok(r.state() == audio::Recorder::Capturing && first < 500, "and a sound right at the start is taken with what came before it");
    }
    {   // auto stop
        audio::Recorder r;
        audio::Recorder::Config c;
        c.autoCapture = true;
        c.threshold = 0.1f;
        c.stopSeconds = 0.5f;
        r.setListening(true);
        r.start(mem.data(), (uint32_t)mem.size(), c);
        std::vector<float> in((size_t)(rate * 2), 0.0f);
        for (size_t i = 4410; i < (size_t)rate; ++i) in[i] = 0.5f * (float)sin(2 * M_PI * 440.0 * i / rate);
        run(r, in);
        ok(r.state() == audio::Recorder::Done, "AUTO STOP ends the take after the silence");
        const double secs = (double)r.frames() / rate;
        ok(secs > 0.9 && secs < 1.05, "and leaves that silence out: " + std::to_string(secs) + " s");
    }
    {   // a full memory, and a clip
        static std::vector<int16_t> small(4410);
        audio::Recorder r;
        audio::Recorder::Config c;
        r.setListening(true);
        r.start(small.data(), (uint32_t)small.size(), c);
        run(r, tone(0.5, 1.2f));
        ok(r.state() == audio::Recorder::Done && r.frames() == small.size(), "a full memory ends the take");
        ok(r.clipped(), "and a take that hit the top says so");
        r.clear();
        ok(r.state() == audio::Recorder::Idle && r.frames() == 0, "clear empties it");
        r.stop();
        ok(r.state() == audio::Recorder::Idle, "stop on nothing is nothing");
    }
    {   // the DC the ADC adds does not reach the take
        audio::Recorder r;
        audio::Recorder::Config c;
        r.setListening(true);
        r.start(mem.data(), (uint32_t)mem.size(), c);
        std::vector<float> dc((size_t)rate * 2, 0.2f);
        run(r, tone(0.01, 0.0f));
        run(r, dc);
        double mean = 0;
        for (uint32_t i = r.frames() - 4000; i < r.frames(); ++i) mean += mem[i];
        ok(fabs(mean / 4000.0 / 32767.0) < 0.002, "DC is taken out: " + std::to_string(mean / 4000.0 / 32767.0));
    }
    {   // the Drumboy's own output, and the left or the right alone
        audio::Recorder r;
        audio::Recorder::Config c;
        c.source = audio::Recorder::Output;
        c.channel = audio::Recorder::Right;
        r.setListening(true);
        r.start(mem.data(), (uint32_t)mem.size(), c);
        float L[32], R[32];
        for (int i = 0; i < 32; ++i) { L[i] = 0.9f; R[i] = 0.25f; }
        uint32_t rx[64] = {};
        r.block(rx, L, R, 32);
        ok(fabs(mem[10] / 32767.0 - 0.25) < 0.001 && L[0] == 0.9f, "OUTPUT takes what is going out, and leaves it as it was");
    }
    {   // monitor
        audio::Recorder r;
        r.setListening(true);
        r.setMonitor(0.5f);
        float L[32] = {}, R[32] = {};
        std::vector<uint32_t> rx;
        const std::vector<float> in = tone(0.01, 0.8f);
        for (int b = 0; b < 8; ++b) { for (float& v : L) v = 0; for (float& v : R) v = 0; rxOf(in, (size_t)b * 32, 32, rx); r.block(rx.data(), L, R, 32); }
        float top = 0;
        for (float v : L) top = std::max(top, fabsf(v));
        ok(top > 0.2f && top < 0.45f, "MONITOR passes the line input to the outputs: " + std::to_string(top));
    }
}

static void testWaveStore(SampleLib* lib) {
    std::string root = __FILE__;
    root = root.substr(0, root.rfind('/')) + "/../../content/data/wtp";
    StdioWaves files;
    files.dir = root;
    // the banks the converted presets use: tricircle (4), beatpair (21), softhard (5), crushsaw (26), organtibia (95), throat (43)
    const int banks[] = {4, 21, 5, 26, 95, 43};
    WaveSet& ws = waveSet();
    for (bool& b : ws.present) b = false;
    int have = 0;
    for (int b : banks) { FILE* t = fopen((root + "/wt" + (b < 100 ? "0" : "") + (b < 10 ? "0" : "") + std::to_string(b) + ".wtp").c_str(), "rb"); if (t) { fclose(t); ws.present[b] = true; ++have; } }
    ok(have == 6, "the wavetable files for the test are in content/data/wtp (python3 tools: run wt_pyramid): " + std::to_string(have) + " of 6");
    if (have != 6) return;

    audio::SharedTables tables;
    tables.build();
    const size_t slotN = audio::WaveStore::slotSamples();
    ok(slotN == (size_t)32 * 8576, "a table is 32 frames x 8,576 samples: " + std::to_string(slotN));
    std::vector<nemesis::dsp::Real> arena(slotN * 3);
    std::vector<int16_t> staging(slotN);
    audio::WaveStore st;
    st.init(arena.data(), 3, staging.data(), &tables, &files);
    uint8_t want[(kWaveBanks + 7) / 8] = {};
    uint32_t now = 1000;
    bit(want, 4);
    const uint32_t g0 = tables.bankGen;
    ok(st.want(want, now) && st.resident(4) && tables.bank[4] != nullptr && tables.bankGen > g0 && st.residentCount() == 1, "a wanted bank is loaded and published");
    ok(tables.bank[4]->numFrames() == 32 && tables.bank[4]->decimated(), "it is the decimated 32-frame pyramid");
    const int opens = files.opens;
    st.want(want, now + 50);
    ok(files.opens == opens, "asking again does not read the file again");

    // it plays: an instrument with the part's OSC 1 on that bank sounds, and differently from the basic wave it is nearest to
    {
        InstRig r(lib);
        for (int b : banks) tables.publish(b, nullptr);   // (the rig has its own tables; this store publishes into its own)
    }
    {
        InstRig r(lib), base(lib);
        audio::WaveStore st2;
        std::vector<nemesis::dsp::Real> arena2(slotN * 3);
        st2.init(arena2.data(), 3, staging.data(), &r.inst->sharedTables(), &files);
        for (InstRig* q : {&r, &base}) {
            Lane& a = q->pj->patterns[0].lanes[0];
            a.mute = 0; a.eu = 0; a.n = 4; for (Step& stp : a.st) { stp.ov = 1; stp.gate = 100; }
            q->pj->parts[0].pages[PG_AMP][3] = 0; q->pj->parts[0].pages[PG_AMP][2] = 127;
            q->pj->parts[0].pages[PG_OSC1][0] = (int16_t)(kBasicWaves + 4);
            q->pj->parts[0].pages[PG_OSC1][1] = 90;   // table position
            q->tp.playing = 1; q->tp.playGen = 1;
        }
        base.run(1200);                                // no bank in memory: the nearest basic wave (tri circle: TRI)
        uint8_t w2[(kWaveBanks + 7) / 8] = {};
        audio::neededBanks(*r.pj, w2);
        ok((w2[0] >> 4 & 1) && !(w2[0] & 1), "the project asks for bank 4 (and nothing else)");
        st2.want(w2, 1000);
        r.run(1200);
        double diff = 0, rb = 0;
        for (size_t i = 10000; i < r.L.size(); ++i) { diff = std::max(diff, (double)fabsf(r.L[i] - base.L[i])); rb = std::max(rb, (double)fabsf(r.L[i])); }
        bool fin = true;
        for (float v : r.L) fin = fin && std::isfinite(v);
        ok(fin && rb > 0.01 && diff > 0.01, "a bank in memory plays, and not as the basic wave it falls back to: " + std::to_string(diff));
        // let go: the voices go back to the basic wave without a crash, and the pointer is gone at once
        uint8_t none[(kWaveBanks + 7) / 8] = {};
        st2.want(none, 5000);
        ok(r.inst->sharedTables().bank[4] == nullptr && !st2.resident(4), "a bank nobody asks for is un-published at once");
        r.run(600);
        bool fin2 = true;
        for (float v : r.L) fin2 = fin2 && std::isfinite(v);
        ok(fin2, "and the part plays on");
    }

    // slots: three banks fit, a fourth is refused until one is let go (and the slot is free only after the audio has had time)
    {
        uint8_t w3[(kWaveBanks + 7) / 8] = {};
        bit(w3, 4); bit(w3, 21); bit(w3, 5); bit(w3, 26);
        now = 10000;
        ok(!st.want(w3, now) && st.refused() >= 0 && st.residentCount() == 3, "four banks, three slots: the fourth waits: refused " + std::to_string(st.refused()));
        uint8_t w4[(kWaveBanks + 7) / 8] = {};
        bit(w4, 21); bit(w4, 5); bit(w4, 26);
        ok(!st.want(w4, now + 1) && !st.resident(4) && tables.bank[4] == nullptr, "bank 4 let go: un-published at once, its slot not yet free");
        ok(!st.resident(26), "the new bank does not take the slot before the audio is clear of the old one");
        ok(st.want(w4, now + 40) && st.resident(26) && st.resident(21) && st.resident(5), "after the retire time the slot is re-used and the bank loads");
        // the slot's table was overwritten in place: it is bank 26's now
        ok(tables.bank[26] != nullptr && tables.bank[26]->numFrames() == 32, "bank 26 is published");
    }
    // every exported bank loads: 32 frames, or 16 for the few banks that have 16 (a payload of that geometry, not a fixed 32)
    {
        audio::WaveStore all;
        std::vector<nemesis::dsp::Real> arenaAll(slotN);
        audio::SharedTables tAll;
        all.init(arenaAll.data(), 1, staging.data(), &tAll, &files);
        int loaded = 0, sixteen = 0, failed = 0, nfiles = 0;
        std::string firstFail;
        uint32_t t = 100000;
        for (int b = 0; b < kWaveBanks; ++b) {
            FILE* f = fopen((root + "/wt" + std::string(b < 10 ? "00" : b < 100 ? "0" : "") + std::to_string(b) + ".wtp").c_str(), "rb");
            if (!f) continue;
            fclose(f);
            ++nfiles;
            ws.present[b] = true;
            uint8_t one[(kWaveBanks + 7) / 8] = {};
            bit(one, b);
            t += 100;
            all.want(one, t);                 // (the previous bank is let go; its slot is free 25 ms on)
            all.want(one, t + 50);
            if (all.resident(b)) { ++loaded; sixteen += tAll.bank[b]->numFrames() == 16; }
            else { ++failed; if (firstFail.empty()) firstFail = std::to_string(b); }
        }
        ok(failed == 0 && loaded == nfiles && nfiles >= 56 && sixteen >= 1, "all " + std::to_string(nfiles) + " banks load (" + std::to_string(sixteen) + " with 16 frames); failed " + std::to_string(failed) + " " + firstFail);
        for (bool& b : ws.present) b = false;
        for (int b : banks) ws.present[b] = true;
    }
    // a bank asked for again inside its retire window goes back on in the slot it still has (no second copy, no false "memory full")
    {
        uint8_t w6[(kWaveBanks + 7) / 8] = {};
        bit(w6, 21); bit(w6, 5); bit(w6, 26);
        now = 20000;
        st.want(w6, now);                                   // 21, 5, 26 resident (loaded above)
        uint8_t w7[(kWaveBanks + 7) / 8] = {};
        bit(w7, 21); bit(w7, 5);
        st.want(w7, now + 1);                               // 26 let go (retiring)
        ok(!st.resident(26), "26 is un-published");
        const int opens2 = files.opens;
        ok(st.want(w6, now + 2) && st.resident(26) && files.opens == opens2 && st.refused() < 0, "asked for again inside the retire window: back on from its slot, no file read");
        // all slots busy and one retiring: a new bank is a short wait, not a refusal
        uint8_t w8[(kWaveBanks + 7) / 8] = {};
        bit(w8, 21); bit(w8, 5); bit(w8, 95);
        ok(!st.want(w8, now + 3) && st.refused() < 0 && !st.resident(95), "a slot being let go is a wait, not 'memory full'");
        ok(st.want(w8, now + 40) && st.resident(95), "and the bank loads once the slot is free");
    }
    // the header's scale is not under the CRC: nonsense is refused
    {
        struct BadScale : audio::WaveFile {
            StdioWaves* inner;
            float scale;
            bool open(int b) override { return inner->open(b); }
            bool read(void* d, size_t n) override { const bool ok = inner->read(d, n); if (ok && n == 64) memcpy((uint8_t*)d + 16, &scale, 4); return ok; }
            void close() override { inner->close(); }
        } bs;
        bs.inner = &files;
        for (float sc : {NAN, INFINITY, 0.0f, -1.0f, 5.0f}) {
            bs.scale = sc;
            audio::WaveStore st4;
            std::vector<nemesis::dsp::Real> arena4(slotN);
            audio::SharedTables t4;
            st4.init(arena4.data(), 1, staging.data(), &t4, &bs);
            uint8_t w9[(kWaveBanks + 7) / 8] = {};
            bit(w9, 21);
            ws.present[21] = true;
            st4.want(w9, 1);
            ok(!st4.resident(21) && t4.bank[21] == nullptr, "a scale of " + std::to_string(sc) + " is refused");
        }
        ws.present[21] = true;
    }
    // a part that stops being CATHODE while its notes release: the tails do not keep a table that is let go and overwritten
    {
        InstRig r(lib);
        audio::WaveStore st5;
        std::vector<nemesis::dsp::Real> arena5(slotN);
        st5.init(arena5.data(), 1, staging.data(), &r.inst->sharedTables(), &files);
        Lane& a = r.pj->patterns[0].lanes[0];
        a.mute = 0; a.eu = 0; a.n = 4; for (Step& stp : a.st) { stp.ov = 0; stp.gate = 100; } a.st[0].ov = 1;
        r.pj->parts[0].pages[PG_OSC1][0] = (int16_t)(kBasicWaves + 4);
        r.pj->parts[0].pages[PG_AMP][3] = 100;   // a long release
        r.tp.playing = 1; r.tp.playGen = 1;
        uint8_t w10[(kWaveBanks + 7) / 8] = {};
        audio::neededBanks(*r.pj, w10);
        st5.want(w10, 1000);
        r.run(200);
        r.pj->parts[0].type = PT_SAMPLER;       // the part is a sampler now; the engine keeps rendering its tails
        audio::neededBanks(*r.pj, w10);         // (nothing needed)
        r.tp.playing = 0;
        r.run(4);
        st5.want(w10, 2000);
        r.run(8);                               // the audio interrupt lets the table go
        st5.want(w10, 2100);
        // the slot is now overwritten with another bank
        uint8_t w11[(kWaveBanks + 7) / 8] = {};
        bit(w11, 21);
        st5.want(w11, 2200);
        r.run(300);
        bool fin = true;
        double pk = 0;
        for (float v : r.L) { fin = fin && std::isfinite(v); pk = std::max(pk, (double)fabsf(v)); }
        ok(fin && pk < 2.0, "the tails of a part that became a sampler stay sane when its bank is replaced: peak " + std::to_string(pk));
    }
    // a damaged file is refused and the bank is marked missing
    {
        std::string tmp = root + "/../wtp_damaged";
        (void)tmp;
        audio::WaveStore st3;
        std::vector<nemesis::dsp::Real> arena3(slotN);
        audio::SharedTables t3;
        struct Bad : audio::WaveFile {
            StdioWaves* inner;
            bool open(int b) override { return inner->open(b); }
            bool read(void* d, size_t n) override { const bool ok = inner->read(d, n); if (ok && n > 1000) ((uint8_t*)d)[500] ^= 0x55; return ok; }   // a bit flipped in the payload
            void close() override { inner->close(); }
        } bad;
        bad.inner = &files;
        st3.init(arena3.data(), 1, staging.data(), &t3, &bad);
        uint8_t w5[(kWaveBanks + 7) / 8] = {};
        bit(w5, 95);
        st3.want(w5, 1);
        ok(!st3.resident(95) && t3.bank[95] == nullptr && !waveSet().present[95], "a file that fails its CRC is not published and the bank is marked missing");
        ws.present[95] = true;
    }
    // the WAVE knob steps over what the card has
    {
        for (bool& b : ws.present) b = false;
        ws.present[4] = ws.present[21] = ws.present[95] = true;
        ok(stepWave(2, +1) == 3 && stepWave(3, +1) == kBasicWaves + 4 && stepWave(kBasicWaves + 4, +1) == kBasicWaves + 21 && stepWave(kBasicWaves + 21, +1) == kBasicWaves + 95, "WAVE steps through the basic waves, then the banks that are on the card");
        ok(stepWave(kBasicWaves + 95, +1) == kBasicWaves + 95 && stepWave(kBasicWaves + 4, -1) == 3 && stepWave(0, -1) == 0, "and stops at both ends");
        ok(stepWave(kBasicWaves + 60, -1) == kBasicWaves + 21 && stepWave(kBasicWaves + 60, +1) == kBasicWaves + 95, "a value on a missing bank steps to the nearest present one");
        Text t, u;
        formatWave(t, kBasicWaves + 4);
        formatWave(u, kBasicWaves + 5);
        ok(t == "TRI CIRCLE" && u == "SOFT HARD ?", std::string("names: ") + t.c_str() + " / " + u.c_str());
    }
}

// Every preset in the list (the factory's three and the converted CATHODE presets): in range, plays, bounded, within the voice budget.
// A song to files, on the real instrument, with memory for a card
struct MemFiles : audio::ExportFiles {
    struct F { std::string path; std::vector<uint8_t> bytes; };
    std::vector<F> kept;
    F now;
    bool refuseOpen = false;
    long refuseAfter = -1;
    int removed = 0;
    bool open(const char* folder, const char* name) override {
        if (refuseOpen) return false;
        now = F{std::string(folder) + "/" + name, {}};
        return true;
    }
    bool write(const void* d, size_t n) override {
        if (refuseAfter >= 0 && (long)now.bytes.size() >= refuseAfter) return false;
        now.bytes.insert(now.bytes.end(), (const uint8_t*)d, (const uint8_t*)d + n);
        return true;
    }
    bool rewrite(uint32_t at, const void* d, size_t n) override { memcpy(&now.bytes[at], d, n); return true; }
    bool close(bool keep) override { if (keep) kept.push_back(now); else ++removed; return true; }
};
static uint32_t le32(const std::vector<uint8_t>& b, size_t at) { return (uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24; }
static double wavPeak(const std::vector<uint8_t>& b) {
    int most = 0;
    for (size_t i = 44; i + 1 < b.size(); i += 2) { int v = (int16_t)(b[i] | b[i + 1] << 8); v = v < 0 ? -v : v; most = v > most ? v : most; }
    return most / 32768.0;
}
// Where the sound of a WAV starts, in frames
static size_t wavStart(const std::vector<uint8_t>& b, int level = 300) {
    for (size_t i = 44; i + 1 < b.size(); i += 2) { int v = (int16_t)(b[i] | b[i + 1] << 8); if (v > level || v < -level) return (i - 44) / 4; }
    return (size_t)-1;
}

static void testSongExport(SampleLib* lib) {
    static Project pj, scratch;
    static Transport tp;
    static audio::Instrument inst;
    makeNewProject(pj);
    pj.bpm = 120;
    pj.swing = 0;
    inst.init(44100.0, pj, tp, *lib);
    Lane& kick = pj.patterns[0].lanes[7];
    Lane& hat = pj.patterns[0].lanes[9];
    Lane& bass = pj.patterns[0].lanes[0];
    for (const int i : {0, 8}) kick.st[i].ov = 1;
    for (const int i : {4, 12}) hat.st[i].ov = 1;
    hat.st[12].prob = 50;
    bass.st[2].ov = 1;
    pj.songs[0].rows[0] = {0, 2};
    pj.songs[0].nRows = 1;
    pj.songs[0].loop = 1;
    const auto run = [&](int kind, MemFiles& f, int stopAfter = -1) {
        audio::SongExport e(pj, tp, inst, *lib, scratch, f);
        if (!e.start(kind, 0)) return (int)-1;
        ExportStatus st;
        int n = 0;
        while (e.step(st)) { if (++n == stopAfter) e.cancel(); if (n > 100000) { ok(false, "an export that does not end"); break; } }
        return (int)st.result;
    };

    MemFiles mix;
    ok(run(EXPORT_WAV, mix) == EXPORT_DONE && mix.kept.size() == 1, "a song makes one WAV");
    const std::vector<uint8_t>& w = mix.kept[0].bytes;
    ok(mix.kept[0].path == "Export/NEW PROJECT/Song 1.wav", "named for the project and the song: " + mix.kept[0].path);
    ok(!memcmp(&w[0], "RIFF", 4) && !memcmp(&w[8], "WAVEfmt ", 8) && le32(w, 24) == 44100 && (w[22] | w[23] << 8) == 2 && (w[34] | w[35] << 8) == 16, "stereo, 16-bit, 44.1 kHz");
    ok(le32(w, 4) == w.size() - 8 && le32(w, 40) == w.size() - 44, "the header knows the length");
    const double seconds = (w.size() - 44) / 4 / 44100.0;
    ok(seconds >= 4.0 && seconds < 15.0, "two bars at 120 and a tail: " + std::to_string(seconds));
    ok(wavPeak(w) > 0.02 && wavPeak(w) <= 1.0, "it holds sound");
    ok(pj.songs[0].loop == 1 && !tp.playing && !tp.songPlaying, "the song's loop and the transport are as they were");

    // everything else as it was, with the screen somewhere else than the song
    {
        pj.patterns[2] = pj.patterns[0];
        pj.songs[1].rows[0] = {2, 1};
        pj.songs[1].rows[1] = {0, 1};
        pj.songs[1].nRows = 2;
        tp.pat = 5; tp.songIdx = 3; tp.playRow = 2; tp.repLeft = 4; tp.nextPat = 7; tp.fill = 1;
        inst.setAutoDegrade(false);
        MemFiles f;
        audio::SongExport e(pj, tp, inst, *lib, scratch, f);
        ok(e.start(EXPORT_WAV, 1), "another song starts");
        ok(tp.fill == 0, "a FILL held as it starts is not in the render");
        ExportStatus st;
        int lastPercent = 0;
        bool rising = true;
        while (e.step(st)) { rising = rising && st.percent >= lastPercent; lastPercent = st.percent; }
        ok(st.result == EXPORT_DONE && rising && lastPercent >= 90, "the bar only rises, and gets there: " + std::to_string(lastPercent));
        ok(tp.pat == 5 && tp.songIdx == 3 && tp.playRow == 2 && tp.repLeft == 4 && tp.nextPat == 7, "the pattern and song on screen are as they were");
        ok(!inst.autoDegrade(), "and a governor turned off stays off");
        inst.setAutoDegrade(true);
        tp.pat = 0; tp.songIdx = 0; tp.nextPat = -1; tp.playRow = 0; tp.repLeft = 0;
    }

    // what was ringing is not in the file
    {
        MemFiles f;
        pj.parts[0].pages[PG_AMP][3] = 100;
        tp.pat = 0; tp.songPlaying = 0; tp.playGen = tp.playGen + 1; tp.playing = 1;
        float dl[32], dr[32];
        for (int i = 0; i < 400; ++i) { inst.beginBlock(); inst.render(dl, dr, 32); }
        tp.playing = 0;
        ok(run(EXPORT_WAV, f) == EXPORT_DONE, "a render after long notes");
        size_t at = 0;
        const std::vector<uint8_t>& b = f.kept[0].bytes;
        bool silentFirst = true;
        for (size_t i = 44; i < 44 + 40 * 4 && i + 1 < b.size(); i += 2) { int v = (int16_t)(b[i] | b[i + 1] << 8); silentFirst = silentFirst && v > -3000 && v < 3000; at += 1; }
        ok(silentFirst, "it starts on the song, not on what was ringing");
        pj.parts[0].pages[PG_AMP][3] = 40;
    }

    // a delay's echoes are not cut off at the first gap
    {
        MemFiles f, none;
        const int16_t time = pj.fx.delay[1];
        pj.fx.delay[0] = 0;
        pj.fx.delay[1] = 126;              // close to a second
        pj.fx.send[2][0] = 100;
        ok(run(EXPORT_WAV, f) == EXPORT_DONE, "with a long delay");
        pj.fx.send[2][0] = 0;
        ok(run(EXPORT_WAV, none) == EXPORT_DONE, "without");
        ok(f.kept[0].bytes.size() > none.kept[0].bytes.size() + 44100, "the echoes are in the file: " + std::to_string(f.kept[0].bytes.size()) + " against " + std::to_string(none.kept[0].bytes.size()));
        pj.fx.delay[1] = time;
    }

    // nothing goes out of the socket and nothing comes in
    {
        static int sent;
        sent = 0;
        static midi::SpscQueue<64> in;
        inst.setMidi(&in, [](const uint8_t*, int) { ++sent; return true; });
        for (int l = 0; l < kLanes; ++l) pj.midi[l].ch = (uint8_t)(l + 1);
        MemFiles quiet;
        ok(run(EXPORT_WAV, quiet) == EXPORT_DONE, "with lanes on MIDI channels");
        ok(sent == 0, "a render plays nothing out of the MIDI socket: " + std::to_string(sent));
        audio::SongExport e(pj, tp, inst, *lib, scratch, quiet);
        ok(e.start(EXPORT_WAV, 0), "and while one runs");
        midi::Event ev;
        ev.type = midi::Type::NoteOn; ev.channel = 0; ev.data1 = 60; ev.data2 = 127;
        in.push(ev);
        ExportStatus st;
        e.step(st, 8);
        ok(inst.activeVoices(0) == 0, "a keyboard is not heard in it");
        e.cancel();
        while (e.step(st)) {}
        inst.setMidi(nullptr, nullptr);
        defaultMidi(pj.midi);
    }

    MemFiles stems;
    ok(run(EXPORT_STEMS, stems) == EXPORT_DONE && stems.kept.size() == 3, "a WAV for each lane that plays: " + std::to_string(stems.kept.size()));
    ok(stems.kept[0].path.find("Song 1 lane 01 part A") != std::string::npos && stems.kept[1].path.find("Song 1 lane 08 ") != std::string::npos && stems.kept[2].path.find("Song 1 lane 10 ") != std::string::npos,
       "named for the lane and what plays it: " + stems.kept[1].path);
    bool sound = true;
    for (const auto& f : stems.kept) sound = sound && wavPeak(f.bytes) > 0.01;
    ok(sound, "each holds sound");
    const size_t step = 44100 / 8;
    ok(wavStart(stems.kept[1].bytes) < 300, "the kick starts on step 1: " + std::to_string(wavStart(stems.kept[1].bytes)));
    ok(wavStart(stems.kept[2].bytes) + 300 > 4 * step && wavStart(stems.kept[2].bytes) < 4 * step + 300, "the hat alone, on step 5, in its place: " + std::to_string(wavStart(stems.kept[2].bytes)));
    ok(wavStart(stems.kept[0].bytes) + 300 > 2 * step && wavStart(stems.kept[0].bytes) < 2 * step + 600, "the bass on step 3, alone: " + std::to_string(wavStart(stems.kept[0].bytes)));

    // a chance falls the same way however it is rendered
    MemFiles a, b;
    run(EXPORT_STEMS, a); run(EXPORT_STEMS, b);
    const auto energyAt = [&](const std::vector<uint8_t>& f, size_t frame) { double e = 0; for (size_t i = 44 + frame * 4; i + 1 < f.size() && i < 44 + (frame + 4000) * 4; i += 2) e += fabs((double)(int16_t)(f[i] | f[i + 1] << 8)); return e; };
    bool same = true;
    for (int bar = 0; bar < 2; ++bar) { const size_t at = 16 * step * bar + 12 * step; same = same && (energyAt(a.kept[2].bytes, at) > 1000) == (energyAt(b.kept[2].bytes, at) > 1000); }
    ok(same, "a step's PROB falls the same in every render");

    MemFiles mf;
    ok(run(EXPORT_MIDI, mf) == EXPORT_DONE && mf.kept.size() == 1 && mf.kept[0].path == "Export/NEW PROJECT/Song 1.mid" && !memcmp(mf.kept[0].bytes.data(), "MThd", 4), "a MIDI file through the same road");

    // a muted lane has no stem, and the song keeps its place
    {
        pj.patterns[0].lanes[9].mute = 1;
        MemFiles m;
        ok(run(EXPORT_STEMS, m) == EXPORT_DONE && m.kept.size() == 2, "a muted lane has no stem: " + std::to_string(m.kept.size()));
        pj.patterns[0].lanes[9].mute = 0;
    }
    // a song with nothing to play
    {
        const uint8_t pat = pj.songs[0].rows[0].pat;
        pj.songs[0].rows[0].pat = 4;
        for (const int kind : {EXPORT_WAV, EXPORT_STEMS, EXPORT_MIDI}) {
            MemFiles f;
            audio::SongExport e(pj, tp, inst, *lib, scratch, f);
            ok(!e.start(kind, 0) && e.why() && f.kept.empty(), "nothing to play makes no file, and says so");
        }
        pj.songs[0].rows[0].pat = pat;
    }
    // too many notes says why
    {
        static Project dense;
        makeNewProject(dense); dense.bpm = 140;
        for (Lane& l : dense.patterns[0].lanes) for (int i = 0; i < 16; ++i) l.st[i].ov = 1;
        int n = 0;
        for (int left = 180; left > 0; left -= 16) dense.songs[0].rows[n++] = {0, (uint8_t)(left > 16 ? 16 : left)};
        dense.songs[0].nRows = (uint8_t)n;
        MemFiles f;
        audio::SongExport e(dense, tp, inst, *lib, scratch, f);
        ok(!e.start(EXPORT_MIDI, 0) && e.why() && std::string(e.why()).find("TOO MANY NOTES") == 0 && f.kept.empty() && f.removed == 1, "a song too dense for a MIDI file says why");
    }
    // a header that cannot be put right fails, and the file goes
    {
        struct NoRewrite : MemFiles { bool rewrite(uint32_t, const void*, size_t) override { return false; } } f;
        ok(run(EXPORT_WAV, f) == EXPORT_FAILED && f.kept.empty() && f.removed == 1, "a refused header leaves no file");
    }

    // what goes wrong
    MemFiles st;
    ok(run(EXPORT_WAV, st, 3) == EXPORT_STOPPED && st.kept.empty() && st.removed == 1, "STOP takes the unfinished file away");
    ok(!tp.playing && pj.songs[0].loop == 1, "and leaves everything as it was");
    {   // and nothing of it is heard when the instrument comes back
        MemFiles f;
        pj.fx.send[2][1] = 120;
        run(EXPORT_WAV, f, 3);
        float tl[32], tr[32], left = 0;
        for (int i = 0; i < 40; ++i) { inst.beginBlock(); inst.render(tl, tr, 32); for (int k = 0; k < 32; ++k) left = fabsf(tl[k]) > left ? fabsf(tl[k]) : left; }
        ok(left < 0.001f, "the reverb of a render given up is gone: " + std::to_string(left));
        pj.fx.send[2][1] = 0;
    }
    MemFiles st2;
    ok(run(EXPORT_STEMS, st2, 60) == EXPORT_STOPPED && st2.removed == 1, "STOP among the stems takes away the one it was on");
    MemFiles full;
    full.refuseAfter = 200000;
    ok(run(EXPORT_WAV, full) == EXPORT_FAILED && full.kept.empty() && full.removed == 1, "a card that fills up fails, and leaves no half a file");
    ok(!tp.playing && pj.songs[0].loop == 1, "as it was after that too");
    MemFiles no;
    no.refuseOpen = true;
    ok(run(EXPORT_WAV, no) == -1 && !tp.playing && pj.songs[0].loop == 1, "a file that cannot be made stops it at once, and changes nothing");
    { audio::SongExport e(pj, tp, inst, *lib, scratch, no); ok(!e.start(EXPORT_WAV, 5), "a song with no rows does not start"); }

    // it sounds again afterwards
    tp.pat = 0; tp.playGen = tp.playGen + 1; tp.playing = 1; tp.songPlaying = 0;
    float l[32], r[32], most = 0;
    for (int i = 0; i < 2000; ++i) { inst.beginBlock(); inst.render(l, r, 32); for (int k = 0; k < 32; ++k) most = fabsf(l[k]) > most ? fabsf(l[k]) : most; }
    ok(most > 0.02f, "and the instrument plays every lane again");
    tp.playing = 0;
}

// ---- what used to do nothing: each knob changes the sound, and at 0 nothing does
static std::vector<float> oneNote(const std::function<void(Project&)>& set, SampleLib* lib, int lane = 0, int notes = 1) {
    Project* pj = new Project;
    Transport* tp = new Transport;
    audio::Instrument* inst = new audio::Instrument;
    makeNewProject(*pj);
    pj->bpm = 120;
    pj->swing = 0;
    pj->patterns[0].lanes[lane].st[0].ov = 1;
    if (notes > 1) pj->patterns[0].lanes[lane].st[8].ov = 1;
    pj->patterns[0].lanes[lane].st[0].gate = 127;
    set(*pj);
    inst->init(44100.0, *pj, *tp, *lib);
    tp->playGen = 1;
    tp->playing = 1;
    std::vector<float> out;
    float l[32], r[32];
    for (int i = 0; i < 2600; ++i) {
        inst->beginBlock();
        inst->render(l, r, 32);
        out.insert(out.end(), l, l + 32);
    }
    return out;
}
static double difference(const std::vector<float>& a, const std::vector<float>& b) {
    double d = 0;
    for (size_t i = 0; i < a.size(); ++i) d += (a[i] - b[i]) * (a[i] - b[i]);
    return sqrt(d / (double)a.size());
}
static double rmsOf(const std::vector<float>& a) {
    double d = 0;
    for (const float x : a) d += x * x;
    return sqrt(d / (double)a.size());
}

static void testLiveControls(SampleLib* lib) {
    const auto synth = [](Project& p) {
        Part& a = p.parts[0];
        a.pages[PG_OSC1][0] = 2;
        a.pages[PG_AMP][1] = 90;
        a.pages[PG_AMP][2] = 127;
        a.pages[PG_FLT2][5] = 1;       // filter 2 on
        a.pages[PG_FLT2][1] = 50;
        p.patterns[0].lanes[0].root = 69;
    };
    const std::vector<float> base = oneNote(synth, lib);
    ok(rmsOf(base) > 0.002, "a synth note sounds");
    const auto changes = [&](const char* name, std::function<void(Project&)> extra, const std::vector<float>& from, const std::function<void(Project&)>& setup, int lane = 0) {
        const std::vector<float> a = oneNote([&](Project& p) { setup(p); extra(p); }, lib, lane);
        ok(difference(a, from) > 0.0003, std::string(name) + " changes the sound: " + std::to_string(difference(a, from)));
    };
#if DBOY_LEAN && DBOY_LEAN_DEFAULT   // (the lean voice is the one a Drumboy plays)
    changes("FILTER 2 KEYTRK", [](Project& p) { p.parts[0].pages[PG_FLT2][6] = 127; }, base, synth);
    changes("FILTER 2 ENV AMT", [](Project& p) { p.parts[0].pages[PG_FLT2][7] = 127; }, base, synth);
    changes("GRIT WOW", [](Project& p) { p.parts[0].pages[PG_GRIT][5] = 127; }, base, synth);
    changes("GRIT LOOP ERR", [](Project& p) { p.parts[0].pages[PG_GRIT][4] = 127; }, base, synth);
#endif
    ok(difference(oneNote(synth, lib), base) == 0.0, "and the same project sounds the same");

    const auto drum = [](Project& p) {
        Lane& l = p.patterns[0].lanes[7];
        l.smpParam(PG_SMP, 0) = 10;    // Bass Stab, the longest with a pitch
        l.smpParam(PG_SFLT, 1) = 90;
    };
    const std::vector<float> dry = oneNote(drum, lib, 7);
    ok(rmsOf(dry) > 0.002, "a sampler lane sounds");
    for (int dest = 0; dest < 3; ++dest) {
        const char* const names[3] = {"MOD to CUTOFF", "MOD to PITCH", "MOD to AMP"};
        changes(names[dest], [dest](Project& p) { Lane& l = p.patterns[0].lanes[7]; l.smpParam(PG_MOD, 0) = 60; l.smpParam(PG_MOD, 1) = 127; l.smpParam(PG_MOD, 2) = (int16_t)dest; }, dry, drum, 7);
    }
    changes("MOD ENV AMT+", [](Project& p) { p.patterns[0].lanes[7].smpParam(PG_MOD, 5) = 127; }, dry, drum, 7);
    changes("sampler GRIT DRIFT", [](Project& p) { p.patterns[0].lanes[7].smpParam(PG_GRIT, 3) = 127; }, dry, drum, 7);
    changes("sampler GRIT WOW", [](Project& p) { p.patterns[0].lanes[7].smpParam(PG_GRIT, 5) = 127; }, dry, drum, 7);
    changes("sampler GRIT CLIP", [](Project& p) { p.patterns[0].lanes[7].smpParam(PG_GRIT, 6) = 127; }, dry, drum, 7);
    changes("sampler GRIT COMPAND", [](Project& p) { Lane& l = p.patterns[0].lanes[7]; l.smpParam(PG_GRIT, 0) = 6; l.smpParam(PG_GRIT, 1) = 127; }, oneNote([&](Project& p) { drum(p); p.patterns[0].lanes[7].smpParam(PG_GRIT, 0) = 6; }, lib, 7), drum, 7);
    {   // each shape is its own, and a reversed hit moves too
        std::vector<float> shape[4];
        for (int w = 0; w < 4; ++w) shape[w] = oneNote([&](Project& p) { drum(p); Lane& l = p.patterns[0].lanes[7]; l.smpParam(PG_MOD, 0) = 40; l.smpParam(PG_MOD, 1) = 127; l.smpParam(PG_MOD, 2) = 1; l.smpParam(PG_MOD, 3) = (int16_t)w; }, lib, 7);
        for (int a = 0; a < 4; ++a) for (int b = a + 1; b < 4; ++b) ok(difference(shape[a], shape[b]) > 0.0003, "the four LFO shapes differ");
        const auto backward = [&](Project& p) { drum(p); p.patterns[0].lanes[7].smpParam(PG_SMP, 6) = 1; };
        changes("MOD to PITCH on a reversed hit", [](Project& p) { Lane& l = p.patterns[0].lanes[7]; l.smpParam(PG_MOD, 0) = 60; l.smpParam(PG_MOD, 1) = 127; l.smpParam(PG_MOD, 2) = 1; }, oneNote(backward, lib, 7), backward, 7);
    }
    const auto looped = [&](Project& p) { drum(p); Lane& l = p.patterns[0].lanes[7]; l.smpParam(PG_SMP, 3) = 2; l.smpParam(PG_SMP, 1) = 20; l.smpParam(PG_SMP, 2) = 60; };
    changes("sampler GRIT LOOP ERR", [](Project& p) { p.patterns[0].lanes[7].smpParam(PG_GRIT, 4) = 127; }, oneNote(looped, lib, 7), looped, 7);
    const auto backLoop = [&](Project& p) { looped(p); p.patterns[0].lanes[7].smpParam(PG_SMP, 6) = 1; };
    changes("sampler GRIT LOOP ERR in reverse", [](Project& p) { p.patterns[0].lanes[7].smpParam(PG_GRIT, 4) = 127; }, oneNote(backLoop, lib, 7), backLoop, 7);
    ok(difference(oneNote([&](Project& p) { looped(p); Lane& l = p.patterns[0].lanes[7]; l.smpParam(PG_GRIT, 4) = 127; l.smpParam(PG_GRIT, 3) = 127; l.smpParam(PG_GRIT, 5) = 127; l.smpParam(PG_GRIT, 7) = 0; }, lib, 7), oneNote(looped, lib, 7)) == 0.0, "with GRIT's MIX at 0 a sample is dry");
#if DBOY_LEAN && DBOY_LEAN_DEFAULT
    ok(difference(oneNote([&](Project& p) { synth(p); p.parts[0].pages[PG_GRIT][4] = 127; p.parts[0].pages[PG_GRIT][7] = 0; }, lib), base) == 0.0, "and so is a voice");
#endif

    // ---- the clock
    static midi::SpscQueue<64> in;
    static std::vector<uint8_t> sent;
    static Project pj;
    static Transport tp;
    static audio::Instrument inst;
    makeNewProject(pj);
    pj.bpm = 90;                       // (so that following it shows)
    pj.patterns[0].lanes[7].st[0].ov = 1;
    inst.init(44100.0, pj, tp, *lib);
    inst.setClockInput(true);
    inst.setMidi(&in, [](const uint8_t* b, int n) { sent.insert(sent.end(), b, b + n); return true; });
    float l[32], r[32];
    const auto blocks = [&](int n) { for (int i = 0; i < n; ++i) { inst.beginBlock(); inst.render(l, r, 32); } };
    const auto real = [&](midi::Type t) { midi::Event e; e.type = t; in.push(e); };
    for (Part& p : pj.parts) p.pages[PG_GLOB][4] = 1;
    blocks(2);
    ok(tp.clockExt == 1 && !tp.playing, "CLOCK MIDI: nothing has started");
    real(midi::Type::Start);
    blocks(100);
    ok(tp.playing && tp.gstep == -1, "START plays, and it waits for the clock");
    real(midi::Type::Clock); blocks(2);
    ok(tp.gstep == 0, "the first tick is the first step");
    for (int i = 0; i < 5; ++i) { real(midi::Type::Clock); blocks(29); }
    ok(tp.gstep == 0, "five more are not a step");
    real(midi::Type::Clock); blocks(2);
    ok(tp.gstep == 1, "the sixth is the next");
    for (int i = 0; i < 48; ++i) { real(midi::Type::Clock); blocks(29); }
    ok(tp.gstep == 9, "a step for every six: " + std::to_string((int)tp.gstep));
    ok(pj.bpm >= 117 && pj.bpm <= 121, "the tempo follows the clock: " + std::to_string((int)pj.bpm));
    for (const uint8_t b : sent) if (b >= 0xF0) ok(b == 0xFA || b == 0xF8, "following, it passes on what comes in");
    { int f8 = 0; for (const uint8_t b : sent) f8 += b == 0xF8; ok(f8 == 55, "each tick once, and none of its own: " + std::to_string(f8)); }
    real(midi::Type::Stop); blocks(2);
    ok(!tp.playing, "STOP stops");
    const int stoppedAt = tp.gstep;
    for (int i = 0; i < 4; ++i) { real(midi::Type::Clock); blocks(29); }   // a master that goes on ticking while it is stopped
    real(midi::Type::Continue); blocks(2);
    ok(tp.playing, "CONTINUE goes on");
    for (int i = 0; i < 2; ++i) { real(midi::Type::Clock); blocks(2); }
    ok((int)tp.gstep == stoppedAt, "from where it stopped: ticks while stopped do not count");
    for (int i = 0; i < 4; ++i) { real(midi::Type::Clock); blocks(29); }
    ok((int)tp.gstep == stoppedAt + 1, "the next step falls on its own tick: " + std::to_string((int)tp.gstep));
    real(midi::Type::Stop); blocks(4);
    pj.bpm = 120;
    real(midi::Type::Start); blocks(2);
    ok(tp.gstep == -1 && tp.pstep == 0, "START starts from the top, wherever it stopped");
    real(midi::Type::Clock); blocks(2);
    ok(tp.gstep == 0, "and the first tick after it is step 1 again");
    real(midi::Type::Stop); blocks(4);
    // a gap says nothing about the tempo
    {
        const int was = pj.bpm;
        real(midi::Type::Start); blocks(2);
        for (int i = 0; i < 20; ++i) { real(midi::Type::Clock); blocks(29); }
        blocks(30000);   // the master stops sending for a while
        for (int i = 0; i < 30; ++i) { real(midi::Type::Clock); blocks(29); }
        ok(abs((int)pj.bpm - was) <= 2, "a gap in the ticks leaves the tempo: " + std::to_string((int)pj.bpm));
        real(midi::Type::Stop); blocks(2);
        pj.songs[0].rows[0] = {0, 1};
        pj.songs[0].nRows = 1;
        tp.songPlaying = 1;
        tp.nextPat = 2;
        real(midi::Type::Start); blocks(2);
        real(midi::Type::Stop); blocks(2);
        ok(!tp.songPlaying && tp.nextPat == -1, "a STOP from outside ends a song as the panel does");
        tp.startRow = 0;
        real(midi::Type::Start); blocks(2);
        ok(tp.songPlaying && tp.playRow == 0 && tp.pat == 0, "START on the SONG screen plays the song");
        real(midi::Type::Stop); blocks(2);
        tp.startRow = -1;
    }
    // inside
    for (Part& p : pj.parts) p.pages[PG_GLOB][4] = 0;
    pj.bpm = 120;
    sent.clear();
    real(midi::Type::Start); real(midi::Type::Clock); blocks(4);
    ok(!tp.playing && tp.clockExt == 0, "CLOCK INT: nothing from outside moves it");
    {   // a big change of tempo cannot leave the clock and the steps apart
        std::vector<uint8_t> keep = sent;
        sent.clear();
        pj.bpm = 60;
        tp.playGen = tp.playGen + 1; tp.gstep = -1; tp.pstep = 0;
        tp.playing = 1;
        blocks(1378);
        pj.bpm = 240;
        blocks(2756);
        const int steps = tp.gstep + 1;
        int f8 = 0;
        for (const uint8_t b : sent) f8 += b == 0xF8;
        tp.playing = 0;
        tp.gstep = -1; tp.pstep = 0;   // (as STOP leaves them)
        blocks(2);
        ok(abs(f8 - steps * 6) <= 6, "60 to 240 BPM: six ticks a step all the same: " + std::to_string(f8) + " for " + std::to_string(steps));
        sent = keep;
        pj.bpm = 120;
    }
    tp.playGen = tp.playGen + 1;
    tp.playing = 1;
    blocks(1378);                      // a second
    tp.playing = 0;
    blocks(2);
    int ticks = 0;
    for (const uint8_t b : sent) ticks += b == 0xF8;
    ok(!sent.empty() && sent.front() == 0xFA && sent.back() == 0xFC, "START and STOP go out");
    ok(ticks >= 47 && ticks <= 49, "and 24 ticks to a beat: " + std::to_string(ticks));
    // the SYNC sockets
    static bool syncLevel;
    static int risen;
    static bool outWas, gateNow;
    syncLevel = false; risen = 0; outWas = false; gateNow = false;
    inst.setSyncPins([] { return syncLevel; }, [](bool pulse, bool gate) { risen += pulse && !outWas; outWas = pulse; gateNow = gate; });
    tp.playGen = tp.playGen + 1; tp.gstep = -1; tp.pstep = 0;
    tp.playing = 1;
    blocks(1378);
    ok(gateNow, "SYNC out: the gate is up while it plays");
    tp.playing = 0;
    blocks(2);
    ok(!gateNow && risen >= 3 && risen <= 5, "and two pulses to a beat: " + std::to_string(risen));
    for (Part& p : pj.parts) p.pages[PG_GLOB][4] = 2;
    tp.gstep = -1; tp.pstep = 0; tp.playGen = tp.playGen + 1;
    tp.playing = 1;
    blocks(2);
    for (int i = 0; i < 5; ++i) { syncLevel = true; blocks(4); syncLevel = false; blocks(340); }   // about a pulse each 345 blocks
    ok(tp.gstep == 9, "CLOCK SYNC: a pulse is two steps, the first one's half made up at the second: " + std::to_string((int)tp.gstep));
    {   // a bounce is not a pulse
        const int was = tp.gstep;
        syncLevel = true; blocks(4); syncLevel = false; blocks(1); syncLevel = true; blocks(1); syncLevel = false; blocks(340);
        ok((int)tp.gstep == was + 2, "a bounce adds nothing: " + std::to_string((int)tp.gstep - was));
    }
    ok(pj.bpm >= 110 && pj.bpm <= 125, "and the tempo follows it: " + std::to_string((int)pj.bpm));
    tp.playing = 0;
    blocks(2);
    inst.setSyncPins(nullptr, nullptr);
    for (Part& p : pj.parts) p.pages[PG_GLOB][4] = 0;
    pj.bpm = 120;
    // a render pays no attention to any of this
    for (Part& p : pj.parts) p.pages[PG_GLOB][4] = 1;
    inst.setRendering(true);
    blocks(2);
    ok(tp.clockExt == 0, "a render keeps its own time");
    {
        const int was = pj.bpm;
        real(midi::Type::Start); real(midi::Type::Clock); blocks(4);
        ok(!tp.playing && (int)pj.bpm == was, "and hears no clock");
    }
    inst.setRendering(false);
    inst.setClockInput(false);
    blocks(2);
    ok(tp.clockExt == 0, "with nothing that could send a clock, CLOCK stays inside: PLAY must not wait for nothing");
    inst.setClockInput(true);
    for (Part& p : pj.parts) p.pages[PG_GLOB][4] = 0;
    blocks(2);

    // ---- the pedal
    const auto noteEvent = [&](midi::Type t, int ch, int note, int vel) { midi::Event e; e.type = t; e.channel = (uint8_t)ch; e.data1 = (uint8_t)note; e.data2 = (uint8_t)vel; in.push(e); };
    for (LaneMidi& m : pj.midi) m.ch = 0;
    pj.parts[0].pages[PG_AMP][2] = 127;
    pj.parts[0].pages[PG_AMP][3] = 5;
    inst.setLivePart(0);
    noteEvent(midi::Type::ControlChange, 0, 64, 127);
    noteEvent(midi::Type::NoteOn, 0, 60, 100); blocks(100);
    ok(inst.activeVoices(0) >= 1, "a key plays");
    noteEvent(midi::Type::NoteOff, 0, 60, 0); blocks(1000);
    ok(inst.activeVoices(0) >= 1, "the pedal holds it after the key is up");
    noteEvent(midi::Type::ControlChange, 0, 64, 0); blocks(3000);
    ok(inst.activeVoices(0) == 0, "and letting it up lets it go");
    noteEvent(midi::Type::NoteOn, 0, 62, 100); blocks(100);
    noteEvent(midi::Type::NoteOff, 0, 62, 0); blocks(3000);
    ok(inst.activeVoices(0) == 0, "with no pedal a note ends with its key");
    noteEvent(midi::Type::ControlChange, 15, 64, 127);
    noteEvent(midi::Type::NoteOn, 15, 60, 100); blocks(100);
    noteEvent(midi::Type::NoteOff, 15, 60, 0); blocks(500);
    ok(inst.activeVoices(0) >= 1, "channel 16 has its pedal too");
    noteEvent(midi::Type::ControlChange, 15, 121, 0); blocks(3000);
    ok(inst.activeVoices(0) == 0, "reset all controllers lets a held note go");
    noteEvent(midi::Type::ControlChange, 3, 64, 127);    // one channel's pedal
    noteEvent(midi::Type::NoteOn, 0, 60, 100); blocks(100);
    noteEvent(midi::Type::NoteOff, 0, 60, 0); blocks(3000);
    ok(inst.activeVoices(0) == 0, "a pedal holds its own channel and no other");
    noteEvent(midi::Type::NoteOn, 3, 62, 100); blocks(100);
    noteEvent(midi::Type::NoteOff, 3, 62, 0); blocks(500);
    ok(inst.activeVoices(0) >= 1, "and it does hold that one");
    noteEvent(midi::Type::ControlChange, 3, 120, 0); blocks(3000);
    ok(inst.activeVoices(0) == 0, "all sound off lets it go");
    noteEvent(midi::Type::NoteOn, 3, 64, 100); blocks(100);
    noteEvent(midi::Type::NoteOff, 3, 64, 0); blocks(3000);
    ok(inst.activeVoices(0) == 0, "and takes the pedal with it");
    noteEvent(midi::Type::ControlChange, 0, 64, 127);
    for (int n = 0; n < 40; ++n) { noteEvent(midi::Type::NoteOn, 0, 40 + n, 100); blocks(4); noteEvent(midi::Type::NoteOff, 0, 40 + n, 0); blocks(4); }
    noteEvent(midi::Type::NoteOn, 0, 90, 100); blocks(50);
    noteEvent(midi::Type::NoteOff, 0, 90, 0);
    noteEvent(midi::Type::ControlChange, 0, 64, 0); blocks(3000);
    ok(inst.activeVoices(0) == 0, "forty notes under a pedal: keys still play, and all of it ends");
    inst.setMidi(nullptr, nullptr);
}

static void testPresetLibrary(SampleLib* lib) {
    ok(presetCount() > kBasePresets && presetIdAt(kBasePresets - 1) == kBasePresets - 1 && presetIdAt(kBasePresets) >= 10, "the factory three come first (ids 0-2), then the library presets from the folder (the listening pages and votes decide which: content/data/golden_votes.json): " + std::to_string(presetCount()));
    int silent = 0, nonfinite = 0, loud = 0, outOfRange = 0, overBudget = 0, badCat = 0;
    std::string firstSilent, firstLoud, firstRange, firstBudget;
    for (int k = 0; k < presetCount(); ++k) {
        const int i = presetIdAt(k);
        presetLoad(i);
        if (presetCat(i) < 1 || presetCat(i) >= kPresetCatCount) { ++badCat; continue; }
        InstRig r(lib);
        Part& part = r.pj->parts[0];
        applyPresetIndex(part.pages, i);
        if (presetVoices(i) > 0) part.voices = (uint8_t)presetVoices(i);
        // the knob ranges: what the file loader would leave unchanged
        Project* chk = new Project;
        makeDefaultProject(*chk);
        memcpy(chk->parts[0].pages, part.pages, sizeof(part.pages));
        Project* copy = new Project;
        memcpy(copy, chk, sizeof(Project));
        sanitize(*chk);
        if (memcmp(chk->parts[0].pages, copy->parts[0].pages, sizeof(part.pages)) != 0) { ++outOfRange; if (firstRange.empty()) firstRange = presetName(i); }
        delete chk; delete copy;
        if (cost::partCost(partDesc(*r.pj, 0), kFwCost) > kVoiceBudget + 0.001f) { ++overBudget; if (firstBudget.empty()) firstBudget = presetName(i); }
        Lane& a = r.pj->patterns[0].lanes[0];
        a.mute = 0; a.eu = 0; a.n = 4; for (Step& st : a.st) { st.ov = 0; st.gate = 80; } a.st[0].ov = 1; a.st[2].ov = 1;
        r.tp.playing = 1; r.tp.playGen = 1;
        r.run(800);
        bool fin = true;
        for (float v : r.L) fin = fin && std::isfinite(v);
        const double pk = maxAbs(r.L, 0, r.L.size());
        if (!fin) ++nonfinite;
        if (pk < 1e-3) { ++silent; if (firstSilent.empty()) firstSilent = presetName(i); }
        if (pk > 0.9995) { ++loud; if (firstLoud.empty()) firstLoud = presetName(i); }
    }
    ok(badCat == 0, "every preset has a category the list shows");
    ok(outOfRange == 0, "every preset is inside the knob ranges (the file loader leaves it unchanged): " + std::to_string(outOfRange) + " " + firstRange);
    ok(nonfinite == 0, "every preset renders finite");
    ok(silent == 0, "no preset is silent on a note: " + std::to_string(silent) + " " + firstSilent);
    ok(overBudget == 0, "every preset fits the voice budget with the voices it asks for: " + std::to_string(overBudget) + " " + firstBudget);
    printf("preset library: %d loud (peak at the limiter) %s\n", loud, firstLoud.c_str());
    // with their effects: every preset on part A through the section (sends, amp, shared settings), finite and bounded; the cost with the effects
    {
        int fxNonFinite = 0, fxOverBudget = 0, withFx = 0;
        double worstTotal = 0;
        for (int k = 0; k < presetCount(); ++k) {
            const int i = presetIdAt(k);
            presetLoad(i);
            InstRig r(lib);
            Part& part = r.pj->parts[0];
            applyPresetIndex(part.pages, i);
            if (presetVoices(i) > 0) part.voices = (uint8_t)presetVoices(i);
            applyPresetFx(r.pj->fx, 0, i);
            fitVoicesToBudget(*r.pj, 0);   // as loading it in BROWSE does
            bool any = r.pj->fx.amp[0][0] > 0;
            for (int k = 0; k < kSendsUsed; ++k) any = any || r.pj->fx.send[0][k] > 0;
            withFx += any;
            const double total = partCost(*r.pj, 0) + fxCostOf(*r.pj);   // the preset's part with its amp, and the shared effects it sends to
            worstTotal = std::max(worstTotal, total);
            if (total > kVoiceBudget + 0.001) ++fxOverBudget;
            Lane& a = r.pj->patterns[0].lanes[0];
            a.mute = 0; a.eu = 0; a.n = 4; for (Step& st : a.st) { st.ov = 0; st.gate = 80; } a.st[0].ov = 1; a.st[2].ov = 1;
            r.tp.playing = 1; r.tp.playGen = 1;
            r.run(900);
            r.tp.playing = 0;
            r.run(1500);   // the tails
            bool fin = true;
            for (float v : r.L) fin = fin && std::isfinite(v);
            for (float v : r.R) fin = fin && std::isfinite(v);
            if (!fin) ++fxNonFinite;
        }
        printf("preset effects: %d presets use effects; the worst part + amp + shared effects cost %.2f of %d (%d over)\n", withFx, worstTotal, kVoiceBudget, fxOverBudget);
        ok(fxNonFinite == 0, "every preset renders finite through its own effects");
        ok(withFx >= 100, "most presets use effects: " + std::to_string(withFx));
        ok(fxOverBudget == 0, "a preset with its effects fits the 5-voice budget on its own: " + std::to_string(fxOverBudget) + " over, worst " + std::to_string(worstTotal));
    }
    // a preview of a preset with effects on a project already near its budget plays dry (the ladder is built from the project); otherwise through them
    {
        int heavy = -1;
        for (int k = kBasePresets; k < presetCount() && heavy < 0; ++k) {
            const int i = presetIdAt(k);
            presetLoad(i);
            FxSettings t;
            defaultFx(t);
            applyPresetFx(t, 0, i);
            if (t.send[0][1] > 20 && t.amp[0][0] > 0) heavy = i;
        }
        ok(heavy >= 0, "a preset with a reverb send and an amp exists");
        if (heavy >= 0) presetLoad(heavy);   // the preview plays what the main loop has read
        if (heavy >= 0) {
            static float gLoad;
            for (int busy = 0; busy < 2; ++busy) {
                InstRig dry(lib), pre(lib);
                gLoad = busy ? 0.9f : 0.1f;
                for (InstRig* q : {&dry, &pre}) { q->inst->setLoadSource([]() { return gLoad; }); q->inst->setAutoDegrade(false); }
                pre.inst->previewPreset(0, heavy);
                dry.inst->previewPreset(0, heavy);
                pre.run(1500); dry.run(1500);
                bool same = pre.L == dry.L;
                bool fin = true;
                for (float v : pre.L) fin = fin && std::isfinite(v);
                ok(fin && same, std::string("preview ") + (busy ? "with the CPU busy" : "with the CPU idle") + ": two identical previews give identical audio");
            }
            {   // the same preview with the CPU idle and busy differ (the effects are on, or dropped)
                InstRig a2(lib), b2(lib);
                gLoad = 0.1f; a2.inst->setLoadSource([]() { return gLoad; }); a2.inst->setAutoDegrade(false);
                a2.inst->previewPreset(0, heavy); a2.run(1500);
                gLoad = 0.9f; b2.inst->setLoadSource([]() { return gLoad; }); b2.inst->setAutoDegrade(false);
                b2.inst->previewPreset(0, heavy); b2.run(1500);
                double d = 0;
                for (size_t i = 0; i < a2.L.size(); ++i) d = std::max(d, (double)fabsf(a2.L[i] - b2.L[i]));
                ok(d > 1e-3, "the preview plays through the preset's effects when the CPU is idle and dry when busy: " + std::to_string(d));
            }
            // the effects go on while the notes release: the preview's reverb is still heard well after its last note (~0.5 s), against the dry preview
            InstRig idle(lib), full(lib);
            gLoad = 0.1f;
            idle.inst->setLoadSource([]() { return gLoad; }); idle.inst->setAutoDegrade(false);
            full.inst->setLoadSource([]() { return 0.9f; }); full.inst->setAutoDegrade(false);   // busy: the preview plays dry
            idle.inst->previewPreset(0, heavy);
            full.inst->previewPreset(0, heavy);
            idle.run(2000); full.run(2000);
            double ei = 0, ef = 0;
            for (size_t i = 32000; i < 64000; ++i) { ei += (double)idle.L[i] * idle.L[i]; ef += (double)full.L[i] * full.L[i]; }   // 0.73-1.45 s
            ok(ei > 1.3 * ef && ei > 1e-8, "the preview's effects are still on after its last note: energy " + std::to_string(ei) + " vs dry " + std::to_string(ef));
        }
    }
    // the names are unique and fit the list
    bool uniq = true, fits = true;
    for (int k = 0; k < presetCount(); ++k) {
        const int i = presetIdAt(k);
        fits = fits && strlen(presetName(i)) <= 20 && strlen(presetName(i)) > 0;
        for (int m = k + 1; m < presetCount(); ++m) uniq = uniq && strcmp(presetName(i), presetName(presetIdAt(m))) != 0;
    }
    ok(uniq && fits, "preset names are unique and at most 20 characters");
    // banks: a preset on a bank sets WAVE to 4 + bank; the bank is a real catalogue number
    int tabled = 0;
    bool wavesOk = true;
    for (int k = 0; k < presetCount(); ++k) {
        const int i = presetIdAt(k);
        presetLoad(i);
        int b[2];
        presetBanks(i, b);
        int16_t pg[kSynthPages][kParamsPerPage];
        defaultSynthPages(pg);
        applyPresetIndex(pg, i);
        for (int s = 0; s < 2; ++s)
            if (b[s] >= 0) { ++tabled; wavesOk = wavesOk && b[s] < kWaveBanks && pg[s ? PG_OSC2 : PG_OSC1][0] == kBasicWaves + b[s]; }
            else if (i >= kBasePresets) wavesOk = wavesOk && pg[s ? PG_OSC2 : PG_OSC1][0] < kBasicWaves;
    }
    ok(wavesOk && tabled >= 60, "presets on banks set WAVE to 4 + the bank: " + std::to_string(tabled) + " oscillators");
}

// A step's effects locks reach the sound: the reverb send of one step, the amp's drive of another; and they let go.
static void testFxLocks(SampleLib* lib) {
    auto rig = [&](InstRig& r, bool lockSend, bool lockAmp) {
        Lane& a = r.pj->patterns[0].lanes[0];
        a.mute = 0; a.eu = 0; a.n = 16;
        for (Step& st : a.st) { st.ov = 0; st.gate = 24; st.nLocks = 0; }
        a.st[0].ov = 1; a.st[8].ov = 1;
        r.pj->parts[0].voices = 2;
        if (lockSend) a.st[0].setLock(lockKey(PG_FXSEND, 1), 127);   // REVERB send, 0 in the part
        if (lockAmp) a.st[0].setLock(lockKey(PG_FXAMP, 1), 127);     // amp DRIVE, 0 in the part
        r.pj->fx.send[0][1] = 0;
        r.tp.playing = 1; r.tp.playGen = 1;
    };
    auto energy = [&](const std::vector<float>& x, double t0, double t1) { double s = 0; for (size_t i = (size_t)(t0 * 44100); i < (size_t)(t1 * 44100) && i < x.size(); ++i) s += (double)x[i] * x[i]; return s; };
    {   // sends
        InstRig lock(lib), plain(lib);
        rig(lock, true, false);
        rig(plain, false, false);
        ok(lock.inst->effects().asleep(fx::FxSection::REVERB) && plain.inst->effects().asleep(fx::FxSection::REVERB), "the reverb sleeps with no send");
        lock.run(40); plain.run(40);   // 0.03 s into step 1
        ok(!lock.inst->effects().asleep(fx::FxSection::REVERB), "the locked step wakes the reverb");
        ok(plain.inst->effects().asleep(fx::FxSection::REVERB), "without the lock it stays asleep");
        lock.run(800); plain.run(800);   // to ~0.6 s: well past the step (0.127 s), before the pattern comes round
        double diff = 0;
        for (size_t i = (size_t)(0.2 * 44100); i < (size_t)(0.5 * 44100) && i < lock.L.size(); ++i) diff += (double)(lock.L[i] - plain.L[i]) * (lock.L[i] - plain.L[i]);
        ok(diff > 1e-6, "the reverb tail of the locked step is heard after the step: " + std::to_string(diff));
        const double dry = energy(plain.L, 0.0, 0.1), wet = energy(lock.L, 0.0, 0.1);
        ok(wet > 0 && dry > 0 && fabs(wet - dry) < 0.5 * dry + 1e-9 + 5 * diff, "the note itself is the same");
        lock.tp.playing = 0; plain.tp.playing = 0;
        lock.run(5000);   // 3.6 s
        ok(lock.inst->effects().asleep(fx::FxSection::REVERB), "the send let go: the reverb is asleep again once its tail is out");
    }
    {   // the amp
        InstRig lock(lib), plain(lib);
        for (InstRig* r : {&lock, &plain}) { r->pj->fx.amp[0][0] = 2; r->pj->fx.amp[0][1] = 10; }   // CRUNCH, a little drive
        rig(lock, false, true);
        rig(plain, false, false);
        lock.pj->fx.amp[0][0] = 2; plain.pj->fx.amp[0][0] = 2;
        lock.pj->fx.amp[0][1] = 10; plain.pj->fx.amp[0][1] = 10;
        lock.run(1500); plain.run(1500);   // the first step and the second note (step 9 at 1.02 s)
        const double d0 = energy(lock.L, 0.0, 0.12) - energy(plain.L, 0.0, 0.12);
        double diff0 = 0, diff9 = 0;
        for (size_t i = 0; i < (size_t)(0.12 * 44100); ++i) diff0 += (double)(lock.L[i] - plain.L[i]) * (lock.L[i] - plain.L[i]);
        for (size_t i = (size_t)(1.03 * 44100); i < (size_t)(1.12 * 44100) && i < lock.L.size(); ++i) diff9 += (double)(lock.L[i] - plain.L[i]) * (lock.L[i] - plain.L[i]);
        (void)d0;
        ok(diff0 > 1e-4, "the locked step's note is played through the amp at DRIVE 127: " + std::to_string(diff0));
        ok(diff9 < 0.02 * diff0 + 1e-9, "the next note is as the part's own: " + std::to_string(diff9) + " vs " + std::to_string(diff0));
    }
    for (int order = 0; order < 2; ++order) {   // two lanes of a part lock the same send in the same step: the lane that sets last (the higher one) wins
        InstRig r(lib);
        Lane& l0 = r.pj->patterns[0].lanes[0];
        Lane& l1 = r.pj->patterns[0].lanes[1];
        rig(r, false, false);
        l1.part = 0; l1.mute = 0; l1.eu = 0; l1.n = 16;
        for (Step& st : l1.st) { st.ov = 0; st.gate = 24; st.nLocks = 0; }
        l1.st[0].ov = 1;
        l0.st[0].setLock(lockKey(PG_FXSEND, 1), order == 0 ? 127 : 0);
        l1.st[0].setLock(lockKey(PG_FXSEND, 1), order == 0 ? 0 : 127);
        r.run(40);
        ok(r.inst->effects().asleep(fx::FxSection::REVERB) == (order == 0), order == 0 ? "the higher lane's 0 wins: the reverb stays asleep" : "the higher lane's 127 wins: the reverb wakes");
    }
    {   // the tokens: a release for another set does nothing; the right one lets go; junk is ignored
        InstRig r(lib);
        app::Lock lk[1] = {{lockKey(PG_FXSEND, 1), 0, 127}};
        r.inst->fxSet(0, 0, lk, 1, 5, 0);
        r.inst->fxRelease(0, 4, 0);     // another set's release
        r.run(40);
        ok(r.inst->effects().sendLevel(0, fx::FxSection::REVERB) > 0.5f, "a release with another token does not let go");
        r.inst->fxRelease(1, 5, 0);     // another lane
        r.run(10);
        ok(r.inst->effects().sendLevel(0, fx::FxSection::REVERB) > 0.5f, "nor does one for another lane");
        r.inst->fxRelease(0, 5, 0);
        r.inst->fxSet(99, 0, lk, 1, 6, 0);        // no such lane
        r.inst->fxSet(0, 9, lk, 1, 7, 0);         // no such part
        r.run(5000);
        ok(r.inst->effects().sendLevel(0, fx::FxSection::REVERB) == 0.0f, "the right token lets go; junk lanes and parts are ignored");
    }
    {   // STOP lets go
        InstRig r(lib);
        rig(r, true, false);
        r.run(60);
        ok(!r.inst->effects().asleep(fx::FxSection::REVERB), "locked");
        r.tp.playing = 0;
        r.run(3);
        r.run(400);
        // after the stop the override is gone: a send of 0 means the reverb falls asleep once the tail is out
        r.run(4500);
        ok(r.inst->effects().asleep(fx::FxSection::REVERB), "STOP lets go: nothing keeps the send up");
    }
}

// A one-voice part must play every note of a phrase (mono basses are common in the library).
static void testMonoPhrase(SampleLib* lib) {
    struct N { double t, d; int note; };
    const N phrase[9] = {{0.00, 0.35, 36}, {0.45, 0.12, 36}, {0.60, 0.12, 36}, {0.90, 0.25, 43}, {1.25, 0.25, 41}, {1.60, 0.12, 36}, {1.80, 0.35, 39}, {2.30, 0.30, 34}, {2.70, 1.00, 36}};
    for (int voices : {1, 2, 3}) {
        for (int preset : {-1, 10 /* Acid Roll */, 12}) {
            InstRig r(lib);
            midi::SpscQueue<64> q;   // this run's own queue
            Part& part = r.pj->parts[0];
            if (preset >= 0) { presetLoad(preset); applyPresetIndex(part.pages, preset); }
            part.voices = (uint8_t)voices;
            r.run(1);
            r.inst->setMidi(&q, nullptr);
            auto send = [&](midi::Type t, int note, int vel) {
                midi::Event e; e.type = t; e.channel = 0; e.data1 = (uint8_t)note; e.data2 = (uint8_t)vel; q.push(e);
            };
            const int blocks = (int)(4.0 * 44100 / 32);
            int onAt[9], offAt[9];
            for (int k = 0; k < 9; ++k) { onAt[k] = (int)(phrase[k].t * 44100 / 32); offAt[k] = (int)((phrase[k].t + phrase[k].d) * 44100 / 32); }
            const size_t base = r.L.size();
            for (int b = 0; b < blocks; ++b) {
                for (int k = 0; k < 9; ++k) {
                    if (offAt[k] == b) send(midi::Type::NoteOff, phrase[k].note, 0);
                    if (onAt[k] == b) send(midi::Type::NoteOn, phrase[k].note, 100);
                }
                r.run(1);
            }
            int missed = 0;
            std::string which;
            for (int k = 0; k < 9; ++k) {
                const size_t a = base + (size_t)onAt[k] * 32, e = a + (size_t)(0.04 * 44100);
                double en = 0;
                for (size_t i = a; i < e && i < r.L.size(); ++i) en += (double)r.L[i] * r.L[i];
                if (en < 1e-7) { ++missed; which += " " + std::to_string(k); }
            }
            ok(missed == 0, "voices " + std::to_string(voices) + ", " + (preset < 0 ? "default patch" : presetName(preset)) + ": every note of the phrase sounds (missing" + which + ")");
        }
    }
}

int main() {
    shippedPresets();   // the library, as the card would give it
    nemesis::dsp::CathodeOscCore::setLazyMacro(true);
    nemesis::dsp::fork::Config::lanes = false;
    nemesis::dsp::fork::Config::filterOversample = 2;
    nemesis::dsp::fork::Config::driveOversample = 2;
    static SampleLib lib;
    std::vector<int16_t> store(kFactoryFrames);
    buildFactorySamples(lib, store.data(), store.size());
    testHits(lib);
    testSampler(lib);
    testPatch();
    testInstrument(&lib);
    testWavetablePhase();
    testWavetablePyramid();
    testReviewFindings(&lib);
    testMidi(&lib);
    testFxSection();
    testFxInstrument(&lib);
    testSevenParts(&lib);
    testSliceAudio(&lib);
    testMemoryPool();
    testSampleFind();
    testRecorder();
    testLoopAudio(&lib);
    testWaveStore(&lib);
    testPresetLibrary(&lib);
    testMonoPhrase(&lib);
    testFxLocks(&lib);
    testSongExport(&lib);
    testLiveControls(&lib);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
