// Wet-level calibration of the Drumboy's shared effects against the upstream originals (content/tools/fxcal/README in map_fx.py's header).
// Built twice from this one file:  fx_cal_hyb  (upstream ReverbEffect / DelayEffect / ChorusEffect at 100 % mix, from ref/upstream/sdk/dsp)
//                                  fx_cal_dboy (firmware/src/fx FxReverb / FxDelay / FxChorus at send 1)
// usage: fx_cal_xxx cases.txt  ->  one line per case: "<id> <kind> <type> <energy>"   (energy = sum of squares of both wet channels
// over the whole render, for a 0.4 s burst of low-passed noise at 0.25 rms after 0.25 s of silence)
// cases.txt is written by map_fx.py --cases: R/D/C lines, upstream real units first, Drumboy real units after the '|'-free column split below.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef BUILD_HYB
#include <nemesis/dsp/ChorusEffect.h>
#include <nemesis/dsp/DelayEffect.h>
#include <nemesis/dsp/ReverbEffect.h>
using namespace nemesis::dsp;
#else
#include "fx/FxChorus.h"
#include "fx/FxDelay.h"
#include "fx/FxReverb.h"
#endif

static const float kSr = 48000.0f;
static const int kBlock = 64;

struct Sig {
    uint32_t s = 12345u;
    float lp1 = 0, lp2 = 0;
    float next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const float w = (float)(int32_t)s / 2147483648.0f;
        lp1 += 0.35f * (w - lp1);          // ~ 3 kHz: warm noise, like a pad
        lp2 += 0.35f * (lp1 - lp2);
        return lp2 * 1.8f;
    }
};

static bool gImpulse = false;
static std::vector<float> gLeft;   // the left wet channel of the last render (RT60 mode)

template <class F>
static double render(double seconds, F&& fx) {
    Sig sig;
    gLeft.clear();
    const int total = (int)(seconds * kSr), pre = (int)(0.25 * kSr), burst = (int)(0.4 * kSr);
    std::vector<float> a(kBlock), b(kBlock), c(kBlock), d(kBlock);
    double e = 0;
    for (int n = 0; n < total; n += kBlock) {
        for (int i = 0; i < kBlock; ++i) {
            const int t = n + i;
            a[i] = b[i] = gImpulse ? (t == pre ? 0.5f : 0.0f) : (t >= pre && t < pre + burst) ? 0.25f * sig.next() * 1.0f : 0.0f;
        }
        fx(a.data(), b.data(), c.data(), d.data(), kBlock);   // in L/R, out L/R (the upstream side works in place on a, b)
        for (int i = 0; i < kBlock; ++i) { e += (double)c[i] * c[i] + (double)d[i] * d[i]; if (gImpulse) gLeft.push_back(0.5f * (c[i] + d[i])); }
    }
    return e;
}

// T20-extrapolated RT60 of the last impulse render (backward-integrated energy decay curve), in seconds
static double rt60() {
    std::vector<double> edc(gLeft.size());
    double acc = 0;
    for (size_t i = gLeft.size(); i-- > 0;) { acc += (double)gLeft[i] * gLeft[i]; edc[i] = acc; }
    if (acc <= 0) return 0;
    double t5 = -1, t25 = -1;
    for (size_t i = 0; i < edc.size(); ++i) {
        const double db = 10.0 * std::log10(edc[i] / edc[0] + 1e-30);
        if (t5 < 0 && db <= -5) t5 = (double)i / kSr;
        if (t25 < 0 && db <= -25) { t25 = (double)i / kSr; break; }
    }
    return t5 < 0 || t25 < 0 ? -1 : 3.0 * (t25 - t5);
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    const bool rtMode = argc > 2 && !strcmp(argv[2], "rt");
    gImpulse = rtMode;
    std::ifstream in(argv[1]);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string kind;
        int id;
        ss >> kind >> id;
        std::vector<double> v;
        double x;
        while (ss >> x) v.push_back(x);
        double e = 0;
        const int typ = (int)v[0];
#ifdef BUILD_HYB
        if (kind == "R") {   // typ decay_ms predly damp% size% mod%
            ReverbEffect r;
            r.prepare(kSr, kBlock);
            r.setAppliedUnit("type", typ); r.setAppliedUnit("decay", v[1]); r.setAppliedUnit("predly", v[2]); r.setAppliedUnit("damp", v[3]);
            r.setAppliedUnit("size", v[4]); r.setAppliedUnit("mod", v[5]); r.setAppliedUnit("mix", 100.0); r.setAppliedUnit("diffuse", 55.1);
            r.setAppliedUnit("shimmer", 0.0);
            e = render(18.0, [&](float* l, float* rr, float* ol, float* orr, int n) { r.process(l, rr, n); memcpy(ol, l, n * 4); memcpy(orr, rr, n * 4); });
        } else if (kind == "D") {   // typ ms fb% tone% width% mod%
            DelayEffect d;
            d.prepare(kSr, kBlock);
            d.setAppliedUnit("type", typ); d.setAppliedUnit("time", v[1]); d.setAppliedUnit("fbk", v[2]); d.setAppliedUnit("tone", v[3]);
            d.setAppliedUnit("tonehp", 30.0); d.setAppliedUnit("width", v[4]); d.setAppliedUnit("mod", v[5]); d.setAppliedUnit("mix", 100.0);
            d.setAppliedUnit("duck", 0.0);
            e = render(14.0, [&](float* l, float* rr, float* ol, float* orr, int n) { d.process(l, rr, n); memcpy(ol, l, n * 4); memcpy(orr, rr, n * 4); });
        } else if (kind == "C") {   // typ mode rate depth% width% fbk% tone% voices hp
            ChorusEffect c;
            c.prepare(kSr, kBlock);
            c.setAppliedUnit("type", typ); c.setAppliedUnit("mode", v[1]); c.setAppliedUnit("rate", v[2]); c.setAppliedUnit("depth", v[3]);
            c.setAppliedUnit("width", v[4]); c.setAppliedUnit("fbk", v[5]); c.setAppliedUnit("tone", v[6]); c.setAppliedUnit("voices", v[7]);
            c.setAppliedUnit("hp", v[8]); c.setAppliedUnit("mix", 100.0); c.setAppliedUnit("colour", 50.4);
            e = render(5.0, [&](float* l, float* rr, float* ol, float* orr, int n) { c.process(l, rr, n); memcpy(ol, l, n * 4); memcpy(orr, rr, n * 4); });
        }
#else
        // the Drumboy side: the columns after the upstream ones (R: 6, D: 5, C: 8 upstream numbers after the type)
        if (kind == "R") {   // typ + 5 upstream | size01 sec dampHz preMs width01 mod01 lowcut
            fx::FxReverb r;
            r.prepare(kSr);
            r.set((float)v[6], (float)v[7], (float)v[8], (float)v[9], (float)v[10], (float)v[11], (float)v[12], 1.4f);   // kReverbTrim
            r.wake();
            e = render(18.0, [&](float* l, float* rr, float* ol, float* orr, int n) { r.process(l, rr, ol, orr, n); });
        } else if (kind == "D") {   // typ + 5 upstream | ms fb tone spread mod hpf
            fx::FxDelay d;
            d.prepare(kSr);
            d.set((float)v[6], (float)v[7], (float)v[8], (float)v[9], (float)v[10], (float)v[11], 1.0f);
            d.wake();
            e = render(14.0, [&](float* l, float* rr, float* ol, float* orr, int n) { d.process(l, rr, ol, orr, n); });
        } else if (kind == "C") {   // typ + 8 upstream | rate depth delay fb width tone hpf
            fx::FxChorus c;
            c.prepare(kSr);
            c.set((float)v[9], (float)v[10], (float)v[11], (float)v[12], (float)v[13], (float)v[14], (float)v[15], 1.0f);
            e = render(5.0, [&](float* l, float* rr, float* ol, float* orr, int n) { c.process(l, rr, ol, orr, n); });
        }
#endif
        if (rtMode) printf("%d %s %d %.4f\n", id, kind.c_str(), typ, rt60());
        else printf("%d %s %d %.9g\n", id, kind.c_str(), typ, e);
    }
    return 0;
}
