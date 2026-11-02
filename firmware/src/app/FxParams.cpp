#include "app/FxParams.h"

#include <math.h>

namespace app {

static const char* const kOff[] = {"OFF", "ON"};
static const char* const kDelayMode[] = {"FREE", "1/4", "1/8.", "1/8", "1/16.", "1/16", "1/8T", "1/16T"};
static const char* const kRatio[] = {"1.5:1", "2:1", "3:1", "4:1", "6:1", "8:1", "12:1", "20:1", "INF"};
static const char* const kAmpModel[] = {"OFF", "CLEAN", "CRUNCH", "LEAD", "BASS"};
static const char* const kCab[] = {"OFF", "1X12", "2X12", "4X12"};

#define LIN(n, u, a, b, d) {n, FxUnit::u, false, a, b, d, nullptr, 0}
#define EXP(n, u, a, b, d) {n, FxUnit::u, true, a, b, d, nullptr, 0}
#define LST(n, l, d) {n, FxUnit::List, false, 0, 0, d, l, (int8_t)(sizeof(l) / sizeof(l[0]))}

static const FxParamDef kDelay[kFxParams] = {
    LST("MODE", kDelayMode, 3), EXP("TIME", Ms, 20, 1000, 300), LIN("FEEDBACK", Pct, 0, 95, 40), EXP("TONE", Hz, 500, 12000, 5000),
    LIN("SPREAD", Pct, 0, 100, 50), LIN("MOD", Pct, 0, 100, 10), EXP("HPF", Hz, 20, 1000, 150), LIN("RETURN", Pct, 0, 100, 100)};
static const FxParamDef kReverb[kFxParams] = {
    LIN("SIZE", Pct, 0, 100, 60), EXP("DECAY", Sec, 0.2, 12, 2.5), EXP("DAMP", Hz, 1000, 16000, 6000), LIN("PRE-DLY", Ms, 0, 200, 20),
    LIN("WIDTH", Pct, 0, 100, 100), LIN("MOD", Pct, 0, 100, 30), EXP("LOW CUT", Hz, 20, 600, 120), LIN("RETURN", Pct, 0, 100, 100)};
static const FxParamDef kChorus[kFxParams] = {
    EXP("RATE", Hz, 0.1, 8, 0.8), LIN("DEPTH", Pct, 0, 100, 50), LIN("DELAY", Ms, 5, 30, 12), LIN("FEEDBACK", Pct, 0, 70, 0),
    LIN("WIDTH", Pct, 0, 100, 100), EXP("TONE", Hz, 1000, 16000, 8000), EXP("HPF", Hz, 20, 1000, 100), LIN("RETURN", Pct, 0, 100, 100)};
static const FxParamDef kComp[kFxParams] = {
    LST("ACTIVE", kOff, 0), LIN("THRESH", Db, -60, 0, -18), LST("RATIO", kRatio, 3), EXP("ATTACK", Ms, 0.1, 100, 10),
    EXP("RELEASE", Ms, 10, 1000, 150), LIN("MAKEUP", Db, 0, 24, 3), LIN("MIX", Pct, 0, 100, 100), EXP("SC HPF", Hz, 20, 300, 60)};
static const FxParamDef kAmp[kFxParams] = {
    LST("MODEL", kAmpModel, 0), LIN("DRIVE", Pct, 0, 100, 40), LIN("BASS", Db, -12, 12, 0), LIN("MID", Db, -12, 12, 0),
    LIN("TREBLE", Db, -12, 12, 0), LIN("PRESENCE", Pct, 0, 100, 40), LST("CAB", kCab, 2), LIN("LEVEL", Db, -24, 6, 0)};
static const FxParamDef kSend[3] = {LIN("DELAY", Pct, 0, 100, 0), LIN("REVERB", Pct, 0, 100, 0), LIN("CHORUS", Pct, 0, 100, 0)};

const char* fxViewName(int view) {
    static const char* const n[FX_VIEWS] = {"DELAY", "REVERB", "CHORUS", "COMP", "AMP SIM", "SENDS"};
    return view >= 0 && view < FX_VIEWS ? n[view] : "";
}
const FxParamDef& fxDef(int view, int i) {
    if (i < 0) i = 0;
    switch (view) {
        case FX_DELAY: return kDelay[i & 7];
        case FX_REVERB: return kReverb[i & 7];
        case FX_CHORUS: return kChorus[i & 7];
        case FX_COMP: return kComp[i & 7];
        case FX_AMP: return kAmp[i & 7];
        default: return kSend[i % 3];
    }
}
int fxCount(int view) { return view == FX_SENDS ? kSendsUsed : kFxParams; }

double fxReal(const FxParamDef& d, int v) {
    if (d.list) return v;
    v = v < 0 ? 0 : v > 127 ? 127 : v;
    const double t = v / 127.0;
    return d.exp ? d.a * pow(d.b / d.a, t) : d.a + (d.b - d.a) * t;
}

int fxDefault(int view, int i) {
    const FxParamDef& d = fxDef(view, i);
    if (d.list) return (int)d.defReal;
    const double t = d.exp ? log(d.defReal / d.a) / log(d.b / d.a) : (d.defReal - d.a) / (d.b - d.a);
    const int v = (int)floor(t * 127.0 + 0.5);
    return v < 0 ? 0 : v > 127 ? 127 : v;
}

void formatFx(Text& out, const FxParamDef& d, int v) {
    if (d.list) { out.add(d.list[v < 0 ? 0 : v >= d.nList ? d.nList - 1 : v]); return; }
    v = v < 0 ? 0 : v > 127 ? 127 : v;
    const double r = fxReal(d, v);
    switch (d.unit) {
        case FxUnit::Pct: out.num((int)floor(r + 0.5)).ch('%'); return;
        case FxUnit::Db: {
            const int x = r < 0 ? -(int)floor(-r + 0.5) : (int)floor(r + 0.5);   // round half away from zero
            if (x > 0) out.ch('+');
            out.num(x).add(" dB");
            return;
        }
        case FxUnit::Sec:
            if (r < 9.95) out.fixed(r, 1); else out.num((int)floor(r + 0.5));
            out.add(" s");
            return;
        case FxUnit::Ms:
            if (r < 9.95) out.fixed(r, 1); else out.num((int)floor(r + 0.5));
            out.add(" ms");
            return;
        case FxUnit::Hz:
            if (r >= 999.5) out.fixed(r / 1000.0, r >= 9950 ? 0 : 1).add(" kHz");
            else if (r < 9.95) out.fixed(r, 1).add(" Hz");
            else out.num((int)floor(r + 0.5)).add(" Hz");
            return;
        default: out.num(v); return;
    }
}

double delayMs(int mode, int timeValue, int bpm) {
    if (mode <= 0 || mode >= kDelayModes) return fxReal(kDelay[1], timeValue);
    // a quarter note is 60000 / bpm ms; the modes: 1/4, dotted 1/8, 1/8, dotted 1/16, 1/16, 1/8 triplet, 1/16 triplet
    static const double kQuarters[kDelayModes] = {0, 1.0, 0.75, 0.5, 0.375, 0.25, 1.0 / 3.0, 1.0 / 6.0};
    const double q = 60000.0 / (bpm < 20 ? 20 : bpm);
    return q * kQuarters[mode];
}

double compRatio(int index) {
    static const double r[9] = {1.5, 2, 3, 4, 6, 8, 12, 20, 100};
    return r[index < 0 ? 0 : index > 8 ? 8 : index];
}

int16_t* fxValues(FxSettings& fx, int view, int part) {
    part = part < 0 ? 0 : part >= kFxParts ? kFxParts - 1 : part;
    switch (view) {
        case FX_DELAY: return fx.delay;
        case FX_REVERB: return fx.reverb;
        case FX_CHORUS: return fx.chorus;
        case FX_COMP: return fx.comp;
        case FX_AMP: return fx.amp[part];
        default: return fx.send[part];
    }
}
const int16_t* fxValues(const FxSettings& fx, int view, int part) { return fxValues(const_cast<FxSettings&>(fx), view, part); }

void defaultFx(FxSettings& fx) {
    for (int v = 0; v < FX_SENDS; ++v) {
        for (int p = 0; p < (v == FX_AMP ? kFxParts : 1); ++p) {
            int16_t* a = fxValues(fx, v, p);
            for (int i = 0; i < kFxParams; ++i) a[i] = (int16_t)fxDefault(v, i);
        }
    }
    for (int p = 0; p < kFxParts; ++p) for (int i = 0; i < kSends; ++i) fx.send[p][i] = 0;   // sends off: the sound is as it was without effects
}

void sanitizeFx(FxSettings& fx) {
    for (int v = 0; v < FX_SENDS; ++v) {
        for (int p = 0; p < (v == FX_AMP ? kFxParts : 1); ++p) {
            int16_t* a = fxValues(fx, v, p);
            for (int i = 0; i < kFxParams; ++i) {
                const int hi = fxMax(fxDef(v, i));
                a[i] = (int16_t)(a[i] < 0 ? 0 : a[i] > hi ? hi : a[i]);
            }
        }
    }
    for (int p = 0; p < kFxParts; ++p) {
        for (int i = 0; i < kSends; ++i) fx.send[p][i] = (int16_t)(i >= kSendsUsed ? 0 : fx.send[p][i] < 0 ? 0 : fx.send[p][i] > 127 ? 127 : fx.send[p][i]);
    }
}

}  // namespace app
