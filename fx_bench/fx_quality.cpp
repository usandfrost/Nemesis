// Behavioural checks of the upstream FX candidates on the host (measured, not listened to):
//   Q1 a parameter changed in ONE step while a steady tone plays: the biggest sample-to-sample jump in the next 20 ms against the biggest
//      jump just before it (ratio ~1 = no click/zipper; >> 1 = a step in the output)
//   Q2 mix = 0: how far the output is from the input (bit-exact bypass?), and the effect's reported latency
//   Q3 tail: seconds until the output of an impulse stays under -60 dBFS / -90 dBFS (what an "idle" switch-off may use)
//   Q4 extremes: finite and bounded with the loudest settings and a +6 dBFS input
//   Q5 switching off: the click when the effect is bypassed by cutting its output to the dry signal (not implemented in the classes: the
//      host must fade; reported as the jump of a hard cut)
// stock_fx.h is checked for the tail only.
#include <nemesis/dsp/ChorusEffect.h>
#include <nemesis/dsp/CompEffect.h>
#include <nemesis/dsp/DelayEffect.h>
#include <nemesis/dsp/DriveEffect.h>
#include <nemesis/dsp/GampEffect.h>
#include <nemesis/dsp/PhaserEffect.h>
#include <nemesis/dsp/PlateReverbCore.h>
#include <nemesis/dsp/ReverbEffect.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "stock_fx.h"

using namespace nemesis::dsp;
constexpr int kBlock = 32;
constexpr double kSr = 44100.0;

template <class T>
struct Rig {
    std::unique_ptr<T> fx{new T};
    Rig() { fx->prepare(kSr, kBlock); }
    // renders n samples of `in(i)` (stereo, same both sides unless in2 given)
    std::vector<float> run(int n, const std::function<float(int)>& in, const std::function<void(int)>& atBlock = nullptr, int start = 0) {
        std::vector<float> out(n);
        std::vector<float> l(kBlock), r(kBlock);
        for (int p = 0; p < n; p += kBlock) {
            if (atBlock) atBlock(p + start);
            for (int i = 0; i < kBlock; ++i) l[i] = r[i] = (p + i < n) ? in(p + i + start) : 0.0f;
            fx->process(l.data(), r.data(), kBlock);
            for (int i = 0; i < kBlock && p + i < n; ++i) out[p + i] = l[i];
        }
        return out;
    }
};

static float tone(int i) { return 0.25f * (float)std::sin(2.0 * M_PI * 440.0 * i / kSr) + 0.1f * (float)std::sin(2.0 * M_PI * 1330.0 * i / kSr); }

static double maxJump(const std::vector<float>& y, int a, int b) {
    double m = 0;
    for (int i = std::max(a, 1); i < b && i < (int)y.size(); ++i) m = std::max(m, (double)std::fabs(y[i] - y[i - 1]));
    return m;
}

struct Q1 { std::string name; double before, after, ratio; };
static std::vector<Q1> gQ1;

// Q1: warm up 1.5 s, then change in one step; compare the jump in the 20 ms after the change with the one in the 20 ms before.
template <class T>
static void q1(const char* name, const std::function<void(T&)>& base, const std::function<void(T&)>& change, int warm = 66150) {
    Rig<T> rig;
    base(*rig.fx);
    const int atBlockStart = (warm / kBlock) * kBlock;
    const int total = atBlockStart + 4096;
    auto y = rig.run(total, tone, [&](int pos) { if (pos == atBlockStart) change(*rig.fx); });
    const int w = 882;
    // the jump scale of the steady signal: the largest jump in the last 20 ms before the change
    const double before = maxJump(y, atBlockStart - w, atBlockStart);
    const double after = maxJump(y, atBlockStart, atBlockStart + w);
    gQ1.push_back({name, before, after, before > 0 ? after / before : 0});
}

struct Q2 { std::string name; double maxErr; int latency; };
static std::vector<Q2> gQ2;
template <class T>
static void q2(const char* name, const std::function<void(T&)>& setup) {
    Rig<T> rig;
    setup(*rig.fx);
    auto y = rig.run(44100, tone);
    int lat = rig.fx->latencySamples();
    double err = 0;
    for (int i = 4410; i < 44100; ++i) err = std::max(err, (double)std::fabs(y[i] - tone(i - lat)));
    gQ2.push_back({name, err, lat});
}

struct Q3 { std::string name; double t60, t90; };
static std::vector<Q3> gQ3;
static void tailTimes(const std::vector<float>& y, double& t60, double& t90) {
    // last time the 10 ms RMS envelope exceeds the threshold
    t60 = t90 = 0;
    const int w = 441;
    for (int i = 0; i + w <= (int)y.size(); i += w / 2) {
        double s = 0;
        for (int k = 0; k < w; ++k) s += (double)y[i + k] * y[i + k];
        const double db = 10.0 * std::log10(s / w + 1e-30);
        if (db > -60) t60 = (i + w) / kSr;
        if (db > -90) t90 = (i + w) / kSr;
    }
}
template <class T>
static void q3(const char* name, const std::function<void(T&)>& setup, int secs = 40) {
    Rig<T> rig;
    setup(*rig.fx);
    auto y = rig.run(44100 * secs, [](int i) { return i < 2000 ? 0.7f * (float)std::sin(2.0 * M_PI * 220.0 * i / kSr) : 0.0f; });
    Q3 q{name, 0, 0};
    tailTimes(y, q.t60, q.t90);
    gQ3.push_back(q);
}

struct Q4 { std::string name; bool finite; double peak; };
static std::vector<Q4> gQ4;
template <class T>
static void q4(const char* name, const std::function<void(T&)>& setup) {
    Rig<T> rig;
    setup(*rig.fx);
    uint32_t rng = 1;
    auto y = rig.run(44100 * 10, [&](int i) { rng = rng * 1664525u + 1013904223u; return (i / 22050) % 2 ? 2.0f * (float)(int32_t)rng * (1.0f / 2147483648.0f) : 2.0f * (float)std::sin(2.0 * M_PI * 110.0 * i / kSr); });
    Q4 q{name, true, 0};
    for (float v : y) { if (!std::isfinite(v)) q.finite = false; q.peak = std::max(q.peak, (double)std::fabs(v)); }
    gQ4.push_back(q);
}

int main() {
    using D = DelayEffect; using R = ReverbEffect; using G = GampEffect; using C = ChorusEffect; using K = CompEffect; using Dr = DriveEffect;

    // ---------------- Q1
    q1<D>("Delay: TIME 300 -> 450 ms (fb 40)", [](D& d) { d.setType(0); d.setTimeMs(300); d.setFeedbackPercent(40); d.setMixPercent(100); }, [](D& d) { d.setTimeMs(450); });
    q1<D>("Delay: TIME 300 -> 450 ms, MOD 30 (varispeed)", [](D& d) { d.setType(0); d.setTimeMs(300); d.setFeedbackPercent(40); d.setModPercent(30); d.setMixPercent(100); }, [](D& d) { d.setTimeMs(450); });
    q1<D>("Delay: FEEDBACK 10 -> 80 %", [](D& d) { d.setType(0); d.setTimeMs(120); d.setFeedbackPercent(10); d.setMixPercent(100); }, [](D& d) { d.setFeedbackPercent(80); });
    q1<D>("Delay: MIX 0 -> 100 %", [](D& d) { d.setType(0); d.setTimeMs(120); d.setFeedbackPercent(40); d.setMixPercent(0); }, [](D& d) { d.setMixPercent(100); });
    q1<D>("Delay: TONE 100 -> 10 %", [](D& d) { d.setType(0); d.setTimeMs(120); d.setFeedbackPercent(40); d.setTonePercent(100); d.setMixPercent(100); }, [](D& d) { d.setTonePercent(10); });
    q1<D>("Delay: type Digital -> Ping-pong", [](D& d) { d.setType(0); d.setTimeMs(120); d.setFeedbackPercent(40); d.setMixPercent(100); }, [](D& d) { d.setType(3); });
    q1<R>("Reverb Hall: DECAY 1 s -> 6 s", [](R& r) { r.setType(1); r.setDecayMs(1000); r.setMixPercent(100); }, [](R& r) { r.setDecayMs(6000); });
    q1<R>("Reverb Hall: SIZE 20 -> 90 %", [](R& r) { r.setType(1); r.setDecayMs(2000); r.setSizePercent(20); r.setMixPercent(100); }, [](R& r) { r.setSizePercent(90); });
    q1<R>("Reverb Plate: DECAY 1 s -> 3 s", [](R& r) { r.setType(2); r.setDecayMs(1000); r.setMixPercent(100); }, [](R& r) { r.setDecayMs(3000); });
    q1<R>("Reverb Plate: PRE-DELAY 0 -> 40 ms", [](R& r) { r.setType(2); r.setDecayMs(2000); r.setPreDelayMs(0); r.setMixPercent(100); }, [](R& r) { r.setPreDelayMs(40); });
    q1<R>("Reverb Plate: DAMP 0 -> 100 %", [](R& r) { r.setType(2); r.setDecayMs(2000); r.setDampPercent(0); r.setMixPercent(100); }, [](R& r) { r.setDampPercent(100); });
    q1<R>("Reverb Plate: MIX 0 -> 100 %", [](R& r) { r.setType(2); r.setDecayMs(2000); r.setMixPercent(0); }, [](R& r) { r.setMixPercent(100); });
    q1<G>("Amp Crunch: GAIN 6 -> 30 dB", [](G& g) { g.setModel(1); g.setGainDb(6); g.setCab(2); g.setMixPercent(100); }, [](G& g) { g.setGainDb(30); });
    q1<G>("Amp Crunch: BASS 20 -> 80 %", [](G& g) { g.setModel(1); g.setGainDb(18); g.setBassPercent(20); g.setCab(2); g.setMixPercent(100); }, [](G& g) { g.setBassPercent(80); });
    q1<G>("Amp Crunch: PRESENCE 0 -> 100 %", [](G& g) { g.setModel(1); g.setGainDb(18); g.setPresencePercent(0); g.setCab(2); g.setMixPercent(100); }, [](G& g) { g.setPresencePercent(100); });
    q1<G>("Amp: CAB 1x12 -> 4x12", [](G& g) { g.setModel(1); g.setGainDb(18); g.setCab(2); g.setMixPercent(100); }, [](G& g) { g.setCab(0); });
    q1<G>("Amp: CAB 1x12 -> Direct", [](G& g) { g.setModel(1); g.setGainDb(18); g.setCab(2); g.setMixPercent(100); }, [](G& g) { g.setCab(5); });
    q1<G>("Amp: MODEL Clean -> Lead", [](G& g) { g.setModel(0); g.setGainDb(18); g.setCab(2); g.setMixPercent(100); }, [](G& g) { g.setModel(2); });
    q1<G>("Amp: MIX 0 -> 100 %", [](G& g) { g.setModel(1); g.setGainDb(18); g.setCab(2); g.setMixPercent(0); }, [](G& g) { g.setMixPercent(100); });
    q1<C>("Chorus: RATE 0.5 -> 5 Hz", [](C& c) { c.setType(0); c.setVoices(3); c.setRateHz(0.5f); c.setDepthPercent(50); c.setMixPercent(50); }, [](C& c) { c.setRateHz(5.0f); });
    q1<C>("Chorus: DEPTH 20 -> 100 %", [](C& c) { c.setType(0); c.setVoices(3); c.setRateHz(0.5f); c.setDepthPercent(20); c.setMixPercent(50); }, [](C& c) { c.setDepthPercent(100); });
    q1<C>("Chorus: VOICES 2 -> 4", [](C& c) { c.setType(0); c.setVoices(2); c.setRateHz(0.5f); c.setDepthPercent(50); c.setMixPercent(50); }, [](C& c) { c.setVoices(4); });
    q1<C>("Chorus: MIX 0 -> 50 %", [](C& c) { c.setType(0); c.setVoices(3); c.setRateHz(0.5f); c.setDepthPercent(50); c.setMixPercent(0); }, [](C& c) { c.setMixPercent(50); });
    q1<K>("Comp: MAKEUP 0 -> 12 dB", [](K& k) { k.setType(3); k.setThresholdDb(-20); k.setRatio(4); k.setMakeupDb(0); k.setMixPercent(100); }, [](K& k) { k.setMakeupDb(12); });
    q1<K>("Comp: THRESHOLD -10 -> -40 dB", [](K& k) { k.setType(3); k.setThresholdDb(-10); k.setRatio(4); k.setMixPercent(100); }, [](K& k) { k.setThresholdDb(-40); });
    q1<K>("Comp: RATIO 2 -> 20", [](K& k) { k.setType(3); k.setThresholdDb(-30); k.setRatio(2); k.setMixPercent(100); }, [](K& k) { k.setRatio(20); });
    q1<K>("Comp: MIX 0 -> 100 %", [](K& k) { k.setType(3); k.setThresholdDb(-30); k.setRatio(8); k.setMixPercent(0); }, [](K& k) { k.setMixPercent(100); });
    q1<Dr>("Drive Overdrive: DRIVE 6 -> 30 dB", [](Dr& d) { d.setType(0); d.setDriveDb(6); d.setMixPercent(100); }, [](Dr& d) { d.setDriveDb(30); });

    // ---------------- Q2
    q2<D>("Delay Digital", [](D& d) { d.setType(0); d.setTimeMs(200); d.setMixPercent(0); });
    q2<D>("Delay Ping-pong", [](D& d) { d.setType(3); d.setTimeMs(200); d.setMixPercent(0); });
    q2<D>("Delay Reverse", [](D& d) { d.setType(5); d.setTimeMs(200); d.setMixPercent(0); });
    q2<R>("Reverb Plate", [](R& r) { r.setType(2); r.setMixPercent(0); });
    q2<R>("Reverb Hall", [](R& r) { r.setType(1); r.setMixPercent(0); });
    q2<G>("Amp Clean", [](G& g) { g.setModel(0); g.setMixPercent(0); });
    q2<G>("Amp Hi-gain", [](G& g) { g.setModel(3); g.setMixPercent(0); });
    q2<G>("Amp Acoustic", [](G& g) { g.setModel(5); g.setMixPercent(0); });
    q2<C>("Chorus", [](C& c) { c.setType(0); c.setMixPercent(0); });
    q2<K>("Comp Bus", [](K& k) { k.setType(3); k.setMixPercent(0); });
    q2<Dr>("Drive Overdrive", [](Dr& d) { d.setType(0); d.setMixPercent(0); });

    // ---------------- Q3
    q3<R>("Reverb Room, DECAY 2.4 s", [](R& r) { r.setType(0); r.setDecayMs(2400); r.setMixPercent(100); });
    q3<R>("Reverb Hall, DECAY 2.4 s", [](R& r) { r.setType(1); r.setDecayMs(2400); r.setMixPercent(100); });
    q3<R>("Reverb Hall, DECAY 6 s (max)", [](R& r) { r.setType(1); r.setDecayMs(6000); r.setMixPercent(100); });
    q3<R>("Reverb Plate, DECAY 2.4 s", [](R& r) { r.setType(2); r.setDecayMs(2400); r.setMixPercent(100); });
    q3<R>("Reverb Plate, DECAY 3 s (max)", [](R& r) { r.setType(2); r.setDecayMs(3000); r.setMixPercent(100); });
    q3<D>("Delay Digital, 375 ms, FB 40 %", [](D& d) { d.setType(0); d.setTimeMs(375); d.setFeedbackPercent(40); d.setMixPercent(100); });
    q3<D>("Delay Digital, 375 ms, FB 80 %", [](D& d) { d.setType(0); d.setTimeMs(375); d.setFeedbackPercent(80); d.setMixPercent(100); });
    q3<D>("Delay Digital, 375 ms, FB 95 % (max)", [](D& d) { d.setType(0); d.setTimeMs(375); d.setFeedbackPercent(100); d.setMixPercent(100); });
    q3<C>("Chorus, 3 voices (no feedback)", [](C& c) { c.setType(0); c.setVoices(3); c.setMixPercent(100); });
    q3<C>("Chorus, FEEDBACK 60 %", [](C& c) { c.setType(0); c.setVoices(3); c.setFeedbackPercent(60); c.setMixPercent(100); });
    q3<G>("Amp Crunch + cab (sag/cab ring)", [](G& g) { g.setModel(1); g.setGainDb(18); g.setCab(2); g.setMixPercent(100); }, 5);

    // ---------------- Q4
    q4<D>("Delay Digital, FB 100 %, tone 100", [](D& d) { d.setType(0); d.setTimeMs(20); d.setFeedbackPercent(100); d.setTonePercent(100); d.setMixPercent(100); });
    q4<D>("Delay Ping-pong, FB 100 %", [](D& d) { d.setType(3); d.setTimeMs(20); d.setFeedbackPercent(100); d.setMixPercent(100); });
    q4<D>("Delay Tape, FB 100 %, sat", [](D& d) { d.setType(2); d.setTimeMs(60); d.setFeedbackPercent(100); d.setMixPercent(100); });
    q4<R>("Reverb Hall, DECAY max, SIZE max", [](R& r) { r.setType(1); r.setDecayMs(6000); r.setSizePercent(100); r.setModPercent(100); r.setMixPercent(100); });
    q4<R>("Reverb Plate, DECAY max", [](R& r) { r.setType(2); r.setDecayMs(3000); r.setModPercent(100); r.setMixPercent(100); });
    q4<R>("Reverb Cathedral, DECAY max", [](R& r) { r.setType(5); r.setDecayMs(12000); r.setMixPercent(100); });
    q4<G>("Amp Hi-gain, GAIN max (36 dB)", [](G& g) { g.setModel(3); g.setGainDb(36); g.setMixPercent(100); g.setLevelDb(12); });
    q4<G>("Amp Bass, GAIN max", [](G& g) { g.setModel(4); g.setGainDb(36); g.setMixPercent(100); });
    q4<C>("Chorus FEEDBACK 95 %, 8 voices", [](C& c) { c.setType(2); c.setVoices(8); c.setFeedbackPercent(95); c.setMixPercent(100); });
    q4<K>("Comp VCA, ratio max, makeup +24", [](K& k) { k.setType(0); k.setThresholdDb(-60); k.setRatio(20); k.setMakeupDb(24); k.setMixPercent(100); });
    q4<Dr>("Drive Fuzz, DRIVE 36 dB", [](Dr& d) { d.setType(2); d.setDriveDb(36); d.setMixPercent(100); });

    // stock reverb tail (Freeverb) for reference
    {
        for (auto cfg : {std::pair<int, int>{25, 50}, {100, 100}}) {
            stock::Reverb rv;
            rv.init(cfg.first, cfg.second, 30);
            std::vector<float> y(44100 * 40);
            for (size_t i = 0; i < y.size(); ++i) { float l, r; rv.process(i < 2000 ? 0.7f * (float)std::sin(2.0 * M_PI * 220.0 * i / kSr) : 0.0f, l, r); y[i] = l; }
            Q3 q{"stock Freeverb size " + std::to_string(cfg.first) + " decay " + std::to_string(cfg.second) + " (wet 0.5)", 0, 0};
            tailTimes(y, q.t60, q.t90);
            gQ3.push_back(q);
        }
    }

    std::puts("Q1  parameter moved in one step: biggest sample-to-sample jump in the 20 ms after / in the 20 ms before (steady tone through the effect)");
    for (auto& q : gQ1) std::printf("  %-52s before %.4f  after %.4f  ratio %6.2f%s\n", q.name.c_str(), q.before, q.after, q.ratio, q.ratio > 3 ? "   <-- STEP" : "");
    std::puts("\nQ2  MIX = 0: max |out - in(delayed by the reported latency)| after 0.1 s");
    for (auto& q : gQ2) std::printf("  %-52s max err %.2e  latency %d samples\n", q.name.c_str(), q.maxErr, q.latency);
    std::puts("\nQ3  impulse tail (0.045 s burst): time the 10 ms RMS stays above -60 dBFS / -90 dBFS");
    for (auto& q : gQ3) std::printf("  %-52s -60 dB at %6.2f s   -90 dB at %6.2f s\n", q.name.c_str(), q.t60, q.t90);
    std::puts("\nQ4  extreme settings, +6 dBFS sine / noise, 10 s");
    for (auto& q : gQ4) std::printf("  %-52s %s  peak %.2f\n", q.name.c_str(), q.finite ? "finite" : "NON-FINITE", q.peak);
    return 0;
}
