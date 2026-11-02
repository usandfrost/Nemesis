// Host micro-benchmark of the FX candidates against one CATHODE voice, in the same build.
//
//   upstream:  the user's FX classes (DelayEffect, ReverbEffect, GampEffect, ChorusEffect, CompEffect, DriveEffect, PhaserEffect), converted
//            with the fork's double->Real rule (fx_bench/make_tree.py). With NEMESIS_DSP_FLOAT they run in float like the firmware.
//   stock:   fx_bench/stock_fx.h (the stock Drumboy firmware's loops, mono, transcribed).
//   voice:   firmware/dsp CathodeVoiceEngine, one plain voice (PD oscillator + low-pass), the firmware's switches (as bench/cost_bench.cpp
//            under COST_CFG=fw COST_OS=2). This is the unit of the repo's cost model (cost::voiceCost == 1.0).
//
// Every cost is measured on a stereo signal in 32-sample blocks (the firmware's block) and reported in ns per stereo sample and in
// "voice equivalents" (effect ns / voice ns). Host numbers are RELATIVE: the M7 has no NEON, a tiny cache and (today) an uncached SDRAM.
// Also reported per effect: sizeof, bytes allocated by prepare() (a counting operator new), and allocations made while processing.
#include <nemesis/dsp/CathodeVoiceEngine.h>
#include <nemesis/dsp/ChorusEffect.h>
#include <nemesis/dsp/CompEffect.h>
#include <nemesis/dsp/DelayCore.h>
#include <nemesis/dsp/DelayEffect.h>
#include <nemesis/dsp/DriveEffect.h>
#include <nemesis/dsp/ForkConfig.h>
#include <nemesis/dsp/GampEffect.h>
#include <nemesis/dsp/PhaserEffect.h>
#include <nemesis/dsp/PlateReverbCore.h>
#include <nemesis/dsp/SpringReverbCore.h>
#include <nemesis/dsp/BbdDelayLine.h>
#include <nemesis/dsp/Oversampler.h>
#include <nemesis/dsp/ReverbEffect.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "proto_fx.h"
#include "stock_fx.h"

using namespace nemesis::dsp;

// ---- allocation counting -------------------------------------------------------------------------------------------------------------
static size_t gBytes = 0, gCalls = 0;
static bool gCount = false;
void* operator new(size_t n) {
    if (gCount) { gBytes += n; ++gCalls; }
    void* p = std::malloc(n ? n : 1);
    if (!p) std::abort();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

// ---- test signal: a drum-ish stereo loop at 44.1 kHz (kick, hat noise, a decaying saw chord), peak about -6 dBFS --------------------------
constexpr int kBlock = 32;
constexpr int kSecs = 8;
constexpr int kN = 44100 * kSecs;
static std::vector<float> gL, gR;

static void makeSignal() {
    gL.assign(kN, 0.0f);
    gR.assign(kN, 0.0f);
    uint32_t rng = 12345;
    auto noise = [&]() { rng = rng * 1664525u + 1013904223u; return (float)(int32_t)rng * (1.0f / 2147483648.0f); };
    for (int i = 0; i < kN; ++i) {
        const double t = i / 44100.0;
        const double beat = std::fmod(t, 0.5);          // 120 bpm
        const double sixt = std::fmod(t, 0.125);
        float kick = (float)(std::sin(2 * M_PI * (50.0 * beat + 90.0 * (1 - std::exp(-beat * 30)) / 30.0)) * std::exp(-beat * 9.0));
        float hat = noise() * (float)std::exp(-sixt * 60.0) * 0.4f;
        const double chordT = std::fmod(t, 2.0);
        float chord = 0;
        for (double f : {110.0, 164.8, 220.0}) chord += (float)(2.0 * std::fmod(f * t, 1.0) - 1.0) * 0.12f;
        chord *= (float)std::exp(-chordT * 1.5);
        gL[i] = 0.5f * kick + hat + chord;
        gR[i] = 0.5f * kick + 0.8f * noise() * (float)std::exp(-sixt * 60.0) * 0.4f + chord * 0.9f;
    }
}

// ---- timing --------------------------------------------------------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;
static double nsPerSample(const std::function<void(const float*, const float*, float*, float*, int)>& blockFn, bool silent = false) {
    std::vector<float> oL(kBlock), oR(kBlock), zero(kBlock, 0.0f);
    double best = 1e30;
    volatile float sink = 0;
    const int total = 44100 * 4;       // 4 s per repetition (the loop repeats)
    for (int rep = 0; rep < 4; ++rep) {
        gCount = true;
        const auto t0 = Clock::now();
        for (int pos = 0; pos < total; pos += kBlock) {
            const int p = pos % (kN - kBlock);
            if (silent) blockFn(zero.data(), zero.data(), oL.data(), oR.data(), kBlock);
            else blockFn(&gL[p], &gR[p], oL.data(), oR.data(), kBlock);
            sink = sink + oL[0] + oR[kBlock - 1];
        }
        const auto t1 = Clock::now();
        gCount = false;
        if (rep > 0) best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / total);
    }
    (void)sink;
    return best;
}

// ---- results table -------------------------------------------------------------------------------------------------------------------
struct Row { std::string group, name; double ns, nsTail; size_t objBytes, heapBytes, heapCalls, procAllocs; double peak; bool finite; };
static std::vector<Row> gRows;
static double gVoiceNs = 1;

// An FxEffect (upstream): `setup` configures it after prepare().
template <class T>
static void benchFx(const char* group, const std::string& name, const std::function<void(T&)>& setup,
                    const std::function<void(T&, int)>& perBlock = nullptr) {
    const double extraNsDividedBy = 1.0;
    int blk = 0;
    gBytes = gCalls = 0;
    gCount = true;
    auto fx = std::make_unique<T>();
    fx->prepare(44100.0, kBlock);
    gCount = false;
    const size_t heapBytes = gBytes, heapCalls = gCalls;
    setup(*fx);
    fx->reset();
    std::vector<float> wl(kBlock), wr(kBlock);
    double peak = 0;
    bool finite = true;
    auto fn = [&](const float* l, const float* r, float* oL, float* oR, int n) {
        std::copy(l, l + n, oL);
        std::copy(r, r + n, oR);
        if (perBlock) perBlock(*fx, blk++);
        fx->process(oL, oR, n);
    };
    // warm-up + sanity pass over the whole signal
    {
        std::vector<float> a(kBlock), b(kBlock);
        for (int p = 0; p + kBlock <= kN; p += kBlock) {
            std::copy(&gL[p], &gL[p] + kBlock, a.begin());
            std::copy(&gR[p], &gR[p] + kBlock, b.begin());
            fx->process(a.data(), b.data(), kBlock);
            for (int i = 0; i < kBlock; ++i) {
                if (!std::isfinite(a[i]) || !std::isfinite(b[i])) finite = false;
                peak = std::max(peak, (double)std::max(std::fabs(a[i]), std::fabs(b[i])));
            }
        }
    }
    gBytes = gCalls = 0;
    const double ns = nsPerSample(fn) / extraNsDividedBy;
    const size_t procAllocs = gCalls;
    const double nsTail = nsPerSample(fn, true) / extraNsDividedBy;
    gRows.push_back({group, name, ns, nsTail, sizeof(T), heapBytes, heapCalls, procAllocs, peak, finite});
}

// A mono-in/stereo-out or mono (stock) process wrapper: `fn(inL,inR,outL,outR)` per block.
static void benchFn(const char* group, const std::string& name, size_t objBytes, size_t heapBytes,
                    const std::function<void(const float*, const float*, float*, float*, int)>& fn) {
    double peak = 0;
    bool finite = true;
    {
        std::vector<float> a(kBlock), b(kBlock);
        for (int p = 0; p + kBlock <= kN; p += kBlock) {
            fn(&gL[p], &gR[p], a.data(), b.data(), kBlock);
            for (int i = 0; i < kBlock; ++i) {
                if (!std::isfinite(a[i]) || !std::isfinite(b[i])) finite = false;
                peak = std::max(peak, (double)std::max(std::fabs(a[i]), std::fabs(b[i])));
            }
        }
    }
    gBytes = gCalls = 0;
    const double ns = nsPerSample(fn);
    const size_t procAllocs = gCalls;
    const double nsTail = nsPerSample(fn, true);
    gRows.push_back({group, name, ns, nsTail, objBytes, heapBytes, 0, procAllocs, peak, finite});
}

// ---- the voice -----------------------------------------------------------------------------------------------------------------------
using OM = CathodeVoiceStage::OscMode;
using FM = CathodeVoiceStage::FilterMode;
static double voiceNs(const std::function<void(CathodeVoiceStage&)>& cfg, int voices = 1) {
    auto* eng = new CathodeVoiceEngine;
    eng->prepare(44100.0);
    eng->setVoiceCount(voices);
    eng->setLaneSitesForTest(0u);
    for (int v = 0; v < CathodeVoiceEngine::kMaxVoices; ++v) {
        auto& st = eng->voiceAt(v).outputStage();
        auto& t = st.voice();
        t.setOscMode(0, OM::PhaseDistortion); t.setPdShape(0, 0.3f); t.setPdAmount(0, 0.6f); t.setLevel(0, 1.0f);
        t.setOscMode(1, OM::PhaseDistortion); t.setLevel(1, 0.0f);
        t.setFilterMode(FM::LowPass); t.setCutoff(2000.0f, 3.0f);
        cfg(t);
        st.amp().setAttackMs(2.0f); st.amp().setDecayMs(20.0f); st.amp().setSustain(1.0f); st.amp().setReleaseMs(100.0f);
    }
    eng->reset();
    for (int v = 0; v < voices; ++v) eng->noteOn(48 + 5 * v, 0.9f);
    float sink = 0;
    for (int i = 0; i < 4410; ++i) sink += eng->processSample().left;
    double best = 1e30;
    for (int rep = 0; rep < 4; ++rep) {
        const int n = 44100;
        const auto t0 = Clock::now();
        for (int i = 0; i < n; i += kBlock)
            for (int k = 0; k < kBlock; ++k) sink += eng->processSample().left;
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
    }
    if (sink == 1234.5f) std::puts("");
    delete eng;
    return best;
}

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string(argv[1]) == "quick";
    fork::Config::idleMatrixSkip = true;
    fork::Config::skipSilentSlots = true;
#ifdef NEMESIS_FORK_EXACT
    fork::Config::lanes = false;
#endif
    fork::Config::filterOversample = 2;
    fork::Config::driveOversample = 2;
    makeSignal();
#ifdef NEMESIS_DSP_FLOAT
    std::puts("build: Real = float (firmware-like: NEMESIS_DSP_FLOAT, fast math, EXACT+APPROX, 8 voices)");
#else
    std::puts("build: Real = double (host default; fork with the firmware switches)");
#endif
    gVoiceNs = voiceNs([](CathodeVoiceStage&) {});
    const double voiceClassic = voiceNs([](CathodeVoiceStage& t) { t.setFilterMode(FM::ClassicLp24); });
    const double voice4 = voiceNs([](CathodeVoiceStage&) {}, 4);
    std::printf("voice: plain %.1f ns/sample, Classic24 %.1f ns, 4 plain voices %.1f ns\n\n", gVoiceNs, voiceClassic, voice4);

    // ================================================================= DELAY
    using D = DelayEffect;
    auto delay = [&](const char* nm, int type, int tone, int fb, int width, int mod) {
        benchFx<D>("DELAY", std::string("upstream ") + nm, [=](D& d) {
            d.setType(type); d.setTimeMs(375); d.setFeedbackPercent((float)fb); d.setTonePercent((float)tone);
            d.setWidthPercent((float)width); d.setModPercent((float)mod); d.setMixPercent(100); });
    };
    delay("Digital stereo (fb 40)", 0, 70, 40, 0, 0);
    delay("Digital + L/R width 50", 0, 70, 40, 50, 0);
    delay("Digital + mod 30 (varispeed)", 0, 70, 40, 0, 30);
    delay("Ping-pong", 3, 70, 40, 0, 0);
    delay("Analog (BBD)", 1, 70, 40, 0, 30);
    delay("Tape (3 heads)", 2, 70, 40, 0, 30);
    delay("Multi-tap", 4, 70, 40, 0, 0);
    delay("Reverse", 5, 70, 40, 0, 0);
    {
        // the proposal: upstream DelayCore x2 only (cubic read, 20 ms crossfade on a time change, LP + HP + DC block + ADAA clip in the loop),
        // wrapped by our own stereo/ping-pong/tempo code; 2 s of delay
        for (int pingpong = 0; pingpong < 1; ++pingpong) {
            DelayCore* c = new DelayCore[2];
            gBytes = gCalls = 0; gCount = true;
            for (int k = 0; k < 2; ++k) c[k].prepare(44100.0, 2000.0);
            gCount = false;
            const size_t heap = gBytes;
            for (int k = 0; k < 2; ++k) { c[k].setDelayMs(375.0); c[k].setFeedback(pingpong ? 0.0 : 0.4); c[k].setToneLpHz(6000.0); c[k].setToneHpHz(30.0); c[k].snap(); }
            float tapL = 0, tapR = 0;
            benchFn("DELAY", pingpong ? "upstream DelayCore x2 + ping-pong wrapper, 2 s (proposal)" : "upstream DelayCore x2, 2 s (proposal)", 2 * sizeof(DelayCore), heap,
                    [&, c, pingpong](const float* l, const float* r, float* oL, float* oR, int n) mutable {
                for (int i = 0; i < n; ++i) {
                    if (pingpong) {
                        const float mono = 0.5f * (l[i] + r[i]);
                        const float wl = c[0].processSample(mono + 0.4f * tapR);
                        const float wr = c[1].processSample(0.4f * tapL);
                        tapL = wl * 0.98f; tapR = wr * 0.98f;       // the loop filtering a real wrapper would do (here: a bare gain)
                        oL[i] = wl; oR[i] = wr;
                    } else {
                        oL[i] = c[0].processSample(l[i]);
                        oR[i] = c[1].processSample(r[i]);
                    }
                } });
        }
    }
    {
        // stock: two mono instances = stereo equivalent
        stock::Delay a, b;
        a.init(5512); b.init(5512);
        benchFn("DELAY", "stock Delay (x2 = stereo)", 2 * sizeof(stock::Delay), 2 * 96000 * 4, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) { oL[i] = a.process(l[i]); oR[i] = b.process(r[i]); } });
    }
    {
        proto::Delay pd;
        pd.init(44100.0f, 2.0f);
        pd.set(375.0f, 0.4f, 6000.0f, 0.0f);
        benchFn("DELAY", "proto: small stereo delay (sketch)", sizeof(pd), pd.bytes(), [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) pd.process(l[i], r[i], oL[i], oR[i]); });
        proto::Delay pp;
        pp.init(44100.0f, 2.0f);
        pp.set(375.0f, 0.4f, 6000.0f, 1.0f);
        benchFn("DELAY", "proto: small ping-pong (sketch)", sizeof(pp), pp.bytes(), [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) pp.process(l[i], r[i], oL[i], oR[i]); });
    }

    // ================================================================= REVERB
    using R = ReverbEffect;
    const char* rn[] = {"Room (4-line FDN + 8 ER taps)", "Hall (8-line FDN)", "Plate (Dattorro)", "Spring", "Shimmer (hall + pitch shift)", "Cathedral (16-line FDN + modal)"};
    for (int t = 0; t < 6; ++t)
        benchFx<R>("REVERB", std::string("upstream ") + rn[t], [=](R& r) {
            r.setType(t); r.setDecayMs(2400); r.setPreDelayMs(20); r.setDampPercent(50); r.setSizePercent(50); r.setDiffusePercent(80);
            r.setModPercent(30); r.setMixPercent(100); if (t == 4) r.setShimmerPercent(30); });
    benchFx<R>("REVERB", "upstream Hall, mod 0", [](R& r) { r.setType(1); r.setDecayMs(2400); r.setModPercent(0); r.setMixPercent(100); });
    {
        stock::Reverb rv;
        rv.init();
        size_t heap = 0;
        for (int i = 0; i < 8; ++i) heap += stock::Reverb::combSize[i] * 4;
        for (int i = 0; i < 4; ++i) heap += stock::Reverb::apSize[i] * 4;
        heap += (stock::Reverb::kPre + stock::Reverb::kSur) * 4;
        benchFn("REVERB", "stock Freeverb (mono in, haas stereo)", sizeof(rv), heap, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) rv.process(0.5f * (l[i] + r[i]), oL[i], oR[i]); });
    }
    {
        PlateReverbCore* core = new PlateReverbCore;
        core->prepare(44100.0);
        core->setRt60Seconds(2.4); core->setPreDelayMs(20); core->setDamping(0.3); core->setModDepthSamples(0);
        benchFn("REVERB", "upstream PlateReverbCore alone, modulation OFF", sizeof(*core), 0, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) { auto s = core->processSample(0.5f * (l[i] + r[i])); oL[i] = s.l; oR[i] = s.r; } });
    }
    {
        // the Dattorro plate core on its own (what ReverbEffect's Plate type wraps), one mono input -> stereo
        PlateReverbCore* core = new PlateReverbCore;
        gBytes = gCalls = 0; gCount = true;
        core->prepare(44100.0);
        gCount = false;
        core->setRt60Seconds(2.4); core->setPreDelayMs(20); core->setDamping(0.3); core->setModDepthSamples(8);
        benchFn("REVERB", "upstream PlateReverbCore alone (no mix/dry line)", sizeof(*core), gBytes, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) { auto s = core->processSample(0.5f * (l[i] + r[i])); oL[i] = s.l; oR[i] = s.r; } });
    }

    // ================================================================= AMP
    using G = GampEffect;
    const char* gn[] = {"Clean (2x os, 1 stage)", "Crunch (4x os, 2 stages)", "Lead (4x os, 3 stages)", "Hi-gain (8x os, 4 stages)", "Bass (4x os, 2 stages, xover)", "Acoustic (linear)"};
    for (int m = 0; m < 6; ++m)
        benchFx<G>("AMP", std::string("upstream ") + gn[m] + " + 1x12 cab", [=](G& g) {
            g.setModel(m); g.setGainDb(18); g.setBassPercent(50); g.setMidPercent(50); g.setTreblePercent(50); g.setPresencePercent(50);
            g.setCab(2); g.setMic(0); g.setLevelDb(0); g.setMixPercent(100); });
    benchFx<G>("AMP", "upstream Crunch, cab Direct (no cabinet)", [](G& g) {
        g.setModel(1); g.setGainDb(18); g.setCab(5); g.setMixPercent(100); });
    for (int f : {1, 2})
        benchFx<G>("AMP", std::string("upstream Crunch + cab, oversample forced x") + std::to_string(f), [=](G& g) {
            g.prepare(44100.0, kBlock, f); g.setModel(1); g.setGainDb(18); g.setCab(2); g.setMixPercent(100); });
    for (int f : {2, 4})
        benchFx<G>("AMP", std::string("upstream Hi-gain + cab, oversample forced x") + std::to_string(f), [=](G& g) {
            g.prepare(44100.0, kBlock, f); g.setModel(3); g.setGainDb(18); g.setCab(2); g.setMixPercent(100); });
    benchFx<G>("AMP", "upstream Crunch + cab, bass/mid/treble knob moved every block (glides running)", [](G& g) {
        g.setModel(1); g.setGainDb(18); g.setCab(2); g.setMixPercent(100); },
        [](G& g, int b) { g.setBassPercent(20.0f + (float)(b % 60)); g.setMidPercent(80.0f - (float)(b % 50)); });
    benchFx<G>("AMP", "upstream Crunch + cab, cab mic switched every 8 blocks (corner glide)", [](G& g) {
        g.setModel(1); g.setGainDb(18); g.setCab(2); g.setMixPercent(100); },
        [](G& g, int b) { if (b % 8 == 0) g.setCab((b / 8) % 5); });
    {
        stock::Overdrive od; stock::Distortion di;
        benchFn("AMP", "stock Overdrive (mono x2, cubic + biquad)", 2 * sizeof(od), 0, [&](const float* l, const float* r, float* oL, float* oR, int n) {
                        for (int i = 0; i < n; ++i) { oL[i] = od.process(l[i]); oR[i] = od.process(r[i]); } });
        benchFn("AMP", "stock Distortion (mono x2, atan table + biquad)", 2 * sizeof(di), 0, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) { oL[i] = di.process(l[i]); oR[i] = di.process(r[i]); } });
        proto::Amp am;
        am.init(44100.0f);
        benchFn("AMP", "proto: small amp, 2x os, mono-sum, cab (sketch)", sizeof(am), 0, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) am.process(l[i], r[i], oL[i], oR[i]); });
    }

    // ================================================================= CHORUS
    using C = ChorusEffect;
    const char* cn[] = {"Chorus", "Flanger", "Ensemble", "Vibrato"};
    for (int t = 0; t < 4; ++t)
        benchFx<C>("CHORUS", std::string("upstream ") + cn[t] + (t == 0 ? " 3 voices" : t == 2 ? " 3 voices" : ""), [=](C& c) {
            c.setType(t); c.setVoices(3); c.setRateHz(0.8f); c.setDepthPercent(50); c.setWidthPercent(70); c.setMixPercent(50); });
    benchFx<C>("CHORUS", "upstream Chorus 2 voices", [](C& c) { c.setType(0); c.setVoices(2); c.setRateHz(0.8f); c.setDepthPercent(50); c.setMixPercent(50); });
    benchFx<C>("CHORUS", "upstream Chorus 1 voice", [](C& c) { c.setType(0); c.setVoices(1); c.setRateHz(0.8f); c.setDepthPercent(50); c.setMixPercent(50); });
    benchFx<C>("CHORUS", "upstream Chorus 3 voices, COLOUR 0 (no companding)", [](C& c) { c.setType(0); c.setVoices(3); c.setColourPercent(0); c.setRateHz(0.8f); c.setDepthPercent(50); c.setMixPercent(50); });
    benchFx<C>("CHORUS", "upstream Chorus 2 voices, COLOUR 0 (no companding)", [](C& c) { c.setType(0); c.setVoices(2); c.setColourPercent(0); c.setRateHz(0.8f); c.setDepthPercent(50); c.setMixPercent(50); });
    benchFx<C>("CHORUS", "upstream Ensemble 8 voices, mode Both (worst case)", [](C& c) { c.setType(2); c.setMode(2); c.setVoices(8); c.setRateHz(0.8f); c.setMixPercent(50); });
    {
        stock::Chorus ch1, ch2;
        ch1.init(); ch2.init();
        benchFn("CHORUS", "stock Chorus (2 voices, x2 = stereo)", 2 * sizeof(ch1), 2 * 5000 * 4, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) { oL[i] = ch1.process(l[i]); oR[i] = ch2.process(r[i]); } });
        stock::Flanger f1, f2;
        f1.init(); f2.init();
        benchFn("CHORUS", "stock Flanger (x2 = stereo)", 2 * sizeof(f1), 0, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) { oL[i] = f1.process(l[i]); oR[i] = f2.process(r[i]); } });
        proto::Chorus pc;
        pc.init(44100.0f);
        pc.set(0.8f, 0.5f, 0.5f);
        benchFn("CHORUS", "proto: small stereo chorus, 2 voices/ch, cubic (sketch)", sizeof(pc), pc.bytes(), [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) pc.process(l[i], r[i], oL[i], oR[i]); });
    }
    using P = PhaserEffect;
    benchFx<P>("CHORUS", "upstream Phaser (default)", [](P& p) { p.setRateHz(0.5f); p.setMixPercent(50); p.setFeedbackPercent(40); });
    {
        stock::Compressor unused;
        (void)unused;
    }

    // ================================================================= COMPRESSOR
    using K = CompEffect;
    const char* kn[] = {"VCA", "FET", "Opto", "Bus"};
    for (int t = 0; t < 4; ++t)
        benchFx<K>("COMPRESSOR", std::string("upstream ") + kn[t], [=](K& k) {
            k.setType(t); k.setThresholdDb(-20); k.setRatio(4); k.setAttackMs(5); k.setReleaseMs(100); k.setMakeupDb(6); k.setMixPercent(100); });
    benchFx<K>("COMPRESSOR", "upstream Bus, sidechain HP 100 Hz, knee 6", [](K& k) {
        k.setType(3); k.setThresholdDb(-20); k.setRatio(4); k.setAttackMs(5); k.setReleaseMs(100); k.setSidechainHpHz(100); k.setKneeDb(6); k.setMixPercent(100); });
    benchFx<K>("COMPRESSOR", "upstream Bus, saturation OFF", [](K& k) {
        k.setType(3); k.setThresholdDb(-20); k.setRatio(4); k.setAttackMs(5); k.setReleaseMs(100); k.setMakeupDb(6); k.setMixPercent(100); k.setSaturationEnabled(false); });
    benchFx<K>("COMPRESSOR", "upstream VCA, saturation OFF", [](K& k) {
        k.setType(0); k.setThresholdDb(-20); k.setRatio(4); k.setAttackMs(5); k.setReleaseMs(100); k.setMakeupDb(6); k.setMixPercent(100); k.setSaturationEnabled(false); });
    {
        stock::Compressor c1, c2;
        c1.init(); c2.init();
        benchFn("COMPRESSOR", "stock Compressor (mono x2, unlinked)", 2 * sizeof(c1), 0, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) { oL[i] = c1.process(l[i]); oR[i] = c2.process(r[i]); } });
        proto::Comp pcm;
        pcm.init(44100.0f);
        pcm.set(-20.0f, 4.0f, 5.0f, 100.0f, 6.0f);
        benchFn("COMPRESSOR", "proto: small linked compressor (sketch)", sizeof(pcm), 0, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) pcm.process(l[i], r[i], oL[i], oR[i]); });
    }

    // ================================================================= DRIVE (OVERDRIVE / DISTORTION / BITCRUSHER ...)
    using Dr = DriveEffect;
    const char* dn[] = {"Overdrive", "Distortion", "Fuzz", "BitCrush", "SampleRate", "Saturate"};
    for (int t = 0; t < 6; ++t)
        benchFx<Dr>("DRIVE", std::string("upstream ") + dn[t], [=](Dr& d) { d.setType(t); d.setDriveDb(18); d.setTonePercent(50); d.setMixPercent(100); d.setBits(8); d.setSampleRateHz(11025); });
    {
        stock::Overdrive od1, od2;
        benchFn("DRIVE", "stock Overdrive (x2)", 2 * sizeof(od1), 0, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) { oL[i] = od1.process(l[i]); oR[i] = od2.process(r[i]); } });
    }

    // ================================================================= cheap extras: stock phaser, bitcrusher
    {
        stock::Phaser p1, p2;
        benchFn("DRIVE", "stock Phaser (x2 = stereo)", 2 * sizeof(p1), 0, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) { oL[i] = p1.process(l[i]); oR[i] = p2.process(r[i]); } });
        stock::Bitcrusher b1, b2;
        benchFn("DRIVE", "stock Bitcrusher (fold, 12 bit, 1/4 rate; x2)", 2 * sizeof(b1), 0, [&](const float* l, const float* r, float* oL, float* oR, int n) {
            for (int i = 0; i < n; ++i) { oL[i] = b1.process(l[i]); oR[i] = b2.process(r[i]); } });
    }

    // ================================================================= state memory per building block (bytes allocated by prepare())
    {
        auto bytesOf = [](const std::function<void()>& f) { gBytes = gCalls = 0; gCount = true; f(); gCount = false; return gBytes; };
        std::printf("\nstate memory by building block (prepare() allocations, this build's Real):\n");
        auto line = [](const char* n, size_t b) { std::printf("  %-52s %9zu B  (%.0f KB)\n", n, b, b / 1024.0); };
        { ReverbCore c; line("ReverbCore Room (4 lines, householder)", bytesOf([&] { c.prepare(44100.0, 4, ReverbCore::Mixing::Householder, ReverbEffect::kRoomMaxLineMs, ReverbEffect::kRoomPreDelayMaxMs); })); }
        { ReverbCore c; line("ReverbCore Hall (8 lines, hadamard)", bytesOf([&] { c.prepare(44100.0, 8, ReverbCore::Mixing::Hadamard, ReverbEffect::kHallMaxLineMs, ReverbEffect::kHallPreDelayMaxMs); })); }
        { ReverbCore c; line("ReverbCore Cathedral (16 lines)", bytesOf([&] { c.prepare(44100.0, 16, ReverbCore::Mixing::Hadamard, ReverbEffect::kCathMaxLineMs, ReverbEffect::kCathPreDelayMaxMs); })); }
        { PlateReverbCore c; line("PlateReverbCore", bytesOf([&] { c.prepare(44100.0); })); }
        { SpringReverbCore c; line("SpringReverbCore", bytesOf([&] { c.prepare(44100.0); })); }
        { DelayCore c; line("DelayCore, 4 s (upstream DelayEffect: x2 for Digital)", bytesOf([&] { c.prepare(44100.0, 4000.0); })); }
        { DelayCore c; line("DelayCore, 2 s", bytesOf([&] { c.prepare(44100.0, 2000.0); })); }
        { DelayCore c; line("DelayCore, 1.5 s", bytesOf([&] { c.prepare(44100.0, 1500.0); })); }
        { BbdDelayLine c; line("BbdDelayLine, 30 ms (one chorus voice-line; 32 are allocated)", bytesOf([&] { c.prepare(44100.0, 1331); })); }
        { Oversampler o; line("Oversampler x2 (one)", bytesOf([&] { o.prepare(2); })); }
        { Oversampler o; line("Oversampler x4 (one)", bytesOf([&] { o.prepare(4); })); }
        { Oversampler o; line("Oversampler x8 (one)", bytesOf([&] { o.prepare(8); })); }
    }

    // ---- report
    std::printf("%-11s %-58s %9s %9s %8s %8s %9s %9s %6s\n", "group", "candidate", "ns/smp", "voice-eq", "tail ns", "sizeof", "prepare B", "alloc/blk", "peak");
    std::string g;
    for (const Row& r : gRows) {
        if (r.group != g) { g = r.group; std::puts(""); }
        std::printf("%-11s %-58s %9.1f %9.3f %8.1f %8zu %9zu %9zu %6.2f%s\n", r.group.c_str(), r.name.c_str(), r.ns, r.ns / gVoiceNs, r.nsTail,
                    r.objBytes, r.heapBytes, r.procAllocs, r.peak, r.finite ? "" : "  NON-FINITE");
    }
    (void)quick;
    return 0;
}
