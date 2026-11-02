#include "lean/LeanPart.h"

#include <math.h>
#include <string.h>

#include <new>

#include "app/Params.h"
#include "app/WaveLib.h"

using app::PG_OSC1; using app::PG_OSC2; using app::PG_SUB; using app::PG_FLT1; using app::PG_FLT2;
using app::PG_GRIT; using app::PG_AMP; using app::PG_MOD; using app::PG_GLOB;

namespace lean {

namespace {

inline float u01(int v) { return (float)v / 127.0f; }
inline float resoQ(int v) { return 0.707f + powf(u01(v), 1.5f) * 15.3f; }          // as audio/PatchMap.cpp
constexpr float kWavePos[4] = {1.0f, 2.0f / 3.0f, 0.0f, 1.0f / 3.0f};              // WAVE: SIN, TRI, SAW, SQR on the basic-shapes table
constexpr float kKeyRef = 261.6255653f;                                              // C4: the key-track source's zero
constexpr float kUnisonSeed[kMaxUnison] = {0.901178f, 0.521387f, 0.769941f, 0.369571f};   // the engine's fixed start phases of the stack's members
constexpr double k2p32 = 4294967296.0;

inline float exp2_(float x) { return fastExp2(x); }
// reso01 as the engine's non-SVF filter cores take it (Q -> 0.30 .. 0.99)
inline float reso01(float q) { q = fclamp(q, 0.5f, 20.0f); return (2.0f - 1.0f / q) / 1.95f; }
inline uint32_t phaseInc(float freqHz, float invSr) {   // a frequency above Nyquist is held at it (the cast of a larger value would be undefined)
    double c = (double)freqHz * (double)invSr;
    c = c < 0.0 ? 0.0 : (c > 0.499 ? 0.499 : c);
    return (uint32_t)(c * k2p32);
}

// An envelope time in the engine's units: an RC attack reaching 1.0 in `ms`, exponential decay and release reaching -60 dB in `ms`.
inline float attackCoef(float ms, float sr) {
    const float n = ms * sr * 0.001f;
    return 1.0f - powf(1.0f - 1.0f / 1.3f, 1.0f / (n > 1.0f ? n : 1.0f));
}
inline float dbCoef(float ms, float sr) { return powf(10.0f, -3.0f / (ms * sr * 0.001f)); }

// the engine's pitch-LFO law: 2m + 10 m^3 semitones, m = 0.3 * depth * lfo
inline float pitchSemis(float m) { m = fclamp(m, -1.0f, 1.0f); return 2.0f * m + 10.0f * m * m * m; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// Patch -> physical units. The same reading of the knobs as audio/PatchMap.cpp (docs/PATCH_MAP.md).
void makeParams(const audio::Patch& P, const audio::SharedTables& tables, Params& o) {
    const auto& p = P.p;
    o.tab = false;
    for (int s = 0; s < 2; ++s) {
        const int pg = s == 0 ? PG_OSC1 : PG_OSC2;
        int w = p[pg][0];
        o.wt[s] = &tables.shapes;
        o.sine[s] = false;
        o.bank[s] = -1;
        o.pos[s] = 0.0f;
        if (w >= app::kBasicWaves) {
            const int b = w - app::kBasicWaves;
            const nemesis::dsp::Wavetable* tb = b < audio::SharedTables::kBanks ? tables.bank[b] : nullptr;
            if (tb) { o.bank[s] = (int16_t)b; o.wt[s] = tb; o.pos[s] = u01(p[pg][1]); o.tab = true; }
            else w = app::waveBankNearest(b);   // not in memory: the basic wave it is nearest to
        }
        if (w < app::kBasicWaves) {
            o.pos[s] = kWavePos[w & 3];
            o.sine[s] = (w & 3) == 0;
            o.pdBase[s] = u01(p[pg][2]); o.pdShape[s] = u01(p[pg][1]);
        }
        o.level[s] = u01(p[pg][6]);
        o.semis[s] = (float)(p[pg][3] * 12 + p[pg][4]) + (float)(p[pg][5] + p[PG_GLOB][3]) * 0.01f;
    }
    o.unison = p[PG_SUB][4] < 1 ? 1 : (p[PG_SUB][4] > kMaxUnison ? kMaxUnison : p[PG_SUB][4]);
    o.uniDetune = u01(p[PG_OSC1][7] > p[PG_SUB][5] ? p[PG_OSC1][7] : p[PG_SUB][5]);
    o.uniBlend = u01(p[PG_SUB][6]);
    o.uniWidth = o.unison > 1 ? u01(p[PG_SUB][7]) : 0.0f;
    o.ring = u01(p[PG_SUB][3]);
    o.sync = u01(p[PG_OSC2][7]);
    o.subLevel = u01(p[PG_SUB][0]);
    o.subOct = p[PG_SUB][1];
    o.noise = u01(p[PG_SUB][2]);
    auto modelOf = [](int f) {   // cost::Filter values: LowPass 0, BandPass 1, HighPass 2, Notch 3, Ladder24 6, Acid 10, SovietLp 11, ClassicLp24 14
        switch (f) {
            case 1: return (uint8_t)M_BP;
            case 2: return (uint8_t)M_HP;
            case 3: return (uint8_t)M_NOTCH;
            case 6: case 7: return (uint8_t)M_LADDER;
            case 10: return (uint8_t)M_ACID;
            case 11: case 12: case 13: return (uint8_t)M_SOVIET;
            case 14: case 15: case 16: case 17: return (uint8_t)M_CLASSIC;
            default: return (uint8_t)M_LP;
        }
    };
    o.model = modelOf(P.f1);
    o.f2on = p[PG_FLT2][5] != 0;
    o.f2route = p[PG_FLT2][4] ? 1 : 0;
    o.model2 = modelOf(P.f2);
    o.cutoff2 = (float)app::cutHz(p[PG_FLT2][1]);
    o.keyTrack2 = u01(p[PG_FLT2][6]);
    o.envAmt2 = log2f(1.0f + 6.0f * u01(p[PG_FLT2][7])) / 4.0f;
    o.q2 = resoQ(p[PG_FLT2][2]);
    o.drive2 = u01(p[PG_FLT2][3]);
    o.balance = u01(p[PG_FLT1][7]);
    o.cutoff = (float)app::cutHz(p[PG_FLT1][1]);
    o.q = resoQ(p[PG_FLT1][2]);
    o.drive = u01(p[PG_FLT1][3]);
    o.keyTrack = u01(p[PG_FLT1][4]);
    o.envAmt = log2f(1.0f + 6.0f * u01(p[PG_FLT1][5])) / 4.0f + u01(p[PG_MOD][5]) * 0.5f;
    if (o.envAmt > 1.0f) o.envAmt = 1.0f;
    o.hpf = p[PG_FLT1][6] > 0 ? (float)app::cutHz(p[PG_FLT1][6]) / 8.0f : 0.0f;
    o.clip = u01(p[PG_GRIT][6]);
    o.gritBits = (float)(p[PG_GRIT][0] < 1 ? 1 : (p[PG_GRIT][0] > 16 ? 16 : p[PG_GRIT][0]));
    o.gritCompand = u01(p[PG_GRIT][1]);
    o.gritRate = (1.0f + u01(p[PG_GRIT][2]) * 43.1f) * 1000.0f;
    o.gritDrift = u01(p[PG_GRIT][3]);
    o.gritLoopErr = u01(p[PG_GRIT][4]);
    o.gritWow = u01(p[PG_GRIT][5]);
    o.gritMix = u01(p[PG_GRIT][7]);
    o.gritOn = o.gritMix > 0.0f && (o.gritBits < 15.5f || o.gritCompand > 0.0f || o.gritRate < 44000.0f || o.gritDrift > 0.0f || o.gritWow > 0.0f || o.gritLoopErr > 0.0f);
    o.atkMs = (float)app::envMs(p[PG_AMP][0]);
    o.decMs = (float)app::envMs(p[PG_AMP][1]);
    o.sustain = u01(p[PG_AMP][2]);
    o.relMs = (float)app::envMs(p[PG_AMP][3]);
    o.velSens = u01(p[PG_AMP][4]);
    o.level_ = u01(p[PG_AMP][5]);
    o.pan = (float)(p[PG_AMP][6] - 64) / 64.0f;
    o.velAtk = u01(p[PG_AMP][7]);
    o.lfoRate = 0.2f + u01(p[PG_MOD][0]) * 12.0f;
    o.lfoShape = p[PG_MOD][3] & 3;
    o.lfoDest = p[PG_MOD][2] & 3;
    o.lfoDepth = u01(p[PG_MOD][1]);
    o.env2Pd = u01(p[PG_MOD][4]);
    o.wheel = u01(p[PG_MOD][6]) * 0.5f;
    o.aftertouch = u01(p[PG_MOD][7]) * 0.5f;
    o.bendRange = (float)p[PG_GLOB][1];
    // a sine slot bends its phase when PD AMT is up, or when the LFO / Env2 move the PD amount (they move the table position instead when a bank is loaded)
    const bool pdMoves = !o.tab && (o.env2Pd > 0.0f || (o.lfoDest == 3 && o.lfoDepth > 0.0f));
    for (int s = 0; s < 2; ++s) o.pd[s] = o.sine[s] && (o.pdBase[s] > 0.0f || pdMoves);
    for (int s = 0; s < 2; ++s) o.ratio[s] = exp2f(o.semis[s] / 12.0f);
    for (int k = 0; k < kMaxUnison; ++k) {
        const int N = o.unison > 1 ? o.unison : 2;
        const float cents = o.uniDetune * 100.0f * ((float)k / (float)(N - 1) - 0.5f);
        o.uniRatio[k] = exp2f(cents / 1200.0f);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// One filter (a model, its cutoff and Q, the drive in front or in the loop) run over n samples in place; the coefficients ramp across the block.
struct FilterUnit {
    Svf svf;
    Ladder ladder;
    SvfCoef cf;                  // the coefficients now (ramped across the block)
    LadderCoef lc;
    bool coefInit = false;
    // the engine's dry ceiling (peak followers on the input and the output: instant attack, 25 ms hold, 150 ms release) and its self-oscillation limiter
    float envI = 0.0f, envO = 0.0f;
    int holdI = 0, holdO = 0;
    ClipAdaa lim;
};

struct Part::Voice {
    Params P;
    bool used = false, held = false;
    int note = 60;
    float vel = 1.0f, velGain = 1.0f, keyTrk = 0.0f;
    uint32_t order = 0;
    // oscillators
    uint32_t ph1 = 0, ph1u[kMaxUnison] = {}, ph2 = 0, phSub = 0;
    uint32_t rng = 1;
    // envelopes (amp and Env2: attack RC toward 1.3, decay/release exponential)
    int stage = 3;               // 0 attack, 1 decay, 2 sustain, 3 release/idle
    float amp = 0.0f;
    float atkK = 0, decC = 0, relC = 0;
    int stage2 = 3;
    float e2 = 0.0f, atk2K = 0, dec2C = 0, rel2C = 0;
    // filter
    struct Chain {               // everything after the oscillators that has state: the HPF, the converter's hold, both filters, the output clip
        Svf hp;
        FilterUnit f1, f2;
        float cvPhase = 1.0f, cvHeld = 0.0f, gritGain = 1.0f, seam = 0.0f, seamPrev = 0.0f;
        ClipAdaa clip;
    } ch[2];                     // the second is the right channel of a stereo (width) voice
    PdState pdS1[kMaxUnison], pdS2;
    SlowNoise wow, flutter, lvl;                              // the converter's clock drift
    float wowPrev = 0.0f;
    bool stereo = false;
    uint32_t phS = 0;            // the synced copy of osc1's phase
};

#ifdef NEMESIS_FORK_FAST_DATA
// On the board a part's first voices sit in DTCM: a voice's 1.5 KB is touched on every sample, and the SDRAM heap would make it fight the wavetables and the effects' lines for the D-cache.
// DTCM has no room for all of them, and a part takes its lowest free voice first. The rest stay in internal RAM (.bss), which only shares the D-cache.
static constexpr int kFastVoices = 3;
static constexpr size_t kVoiceBytes = 1536;
alignas(8) NEMESIS_FORK_FAST_DATA static uint8_t gVoicePool[app::kParts * kFastVoices * kVoiceBytes];
alignas(8) static uint8_t gVoiceRest[app::kParts * (kMaxVoices - kFastVoices) * kVoiceBytes];
static int gFastUsed = 0, gRestUsed = 0;
#endif

void Part::init(float sampleRate, const audio::SharedTables& tables) {
    sr_ = sampleRate;
    shapes_ = &tables.shapes;
#ifdef NEMESIS_FORK_FAST_DATA
    static_assert(sizeof(Voice) <= kVoiceBytes, "the voice pools are too small: raise kVoiceBytes");
    for (int i = 0; i < kMaxVoices; ++i) {
        const bool fast = i < kFastVoices;
        int& used = fast ? gFastUsed : gRestUsed;
        if (used < (fast ? kFastVoices : kMaxVoices - kFastVoices) * app::kParts) v_[i] = new ((fast ? gVoicePool : gVoiceRest) + kVoiceBytes * used++) Voice();
        else v_[i] = new Voice();
    }
#else
    for (int i = 0; i < kMaxVoices; ++i) v_[i] = new Voice();
#endif
    (void)pdTables();   // its tables are built here, not on the first note
}

bool Part::voiceUsed(int v) const { return v >= 0 && v < kMaxVoices && v_[v]->used; }

int Part::activeVoices() const {
    int n = 0;
    for (int i = 0; i < kMaxVoices; ++i) n += v_[i]->used;
    return n;
}

void Part::rebind(const audio::SharedTables& t) {   // a bank came or went: sounding voices read the tables that are there now
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!v_[i]->used) continue;
        Params& p = v_[i]->P;
        for (int s = 0; s < 2; ++s)
            if (p.bank[s] >= 0) { const nemesis::dsp::Wavetable* tb = p.bank[s] < audio::SharedTables::kBanks ? t.bank[p.bank[s]] : nullptr; p.wt[s] = tb ? tb : &t.shapes; }
    }
}

void Part::panic() { for (int i = 0; i < kMaxVoices; ++i) { v_[i]->used = false; v_[i]->held = false; } }

int Part::noteOn(int note, float vel01, const Params& p) {
    // the part's cap: the oldest HELD note is let go first (its tail keeps sounding), as Instrument::startCathode does
    for (;;) {
        int held = 0, oldest = -1;
        for (int i = 0; i < cap_; ++i)
            if (v_[i]->used && v_[i]->held) { ++held; if (oldest < 0 || v_[i]->order < v_[oldest]->order) oldest = i; }
        if (held < cap_ || oldest < 0) break;
        v_[oldest]->held = false; v_[oldest]->stage = 3; v_[oldest]->stage2 = 3;
    }
    int pick = -1;
    for (int i = 0; i < cap_ && pick < 0; ++i) if (!v_[i]->used) pick = i;
    if (pick < 0) {   // all busy: a releasing voice beats a held one, the oldest first
        uint32_t best = 0xFFFFFFFFu; bool bestRel = false;
        for (int i = 0; i < cap_; ++i) {
            const bool rel = !v_[i]->held;
            if ((rel && !bestRel) || (rel == bestRel && v_[i]->order < best)) { best = v_[i]->order; bestRel = rel; pick = i; }
        }
    }
    Voice& v = *v_[pick];
    const bool fresh = !v.used;   // a stolen voice carries on from its levels and filter state; a free one starts clean
    v.P = p;
    v.used = true;
    v.held = true;
    v.note = note;
    v.vel = vel01;
    v.order = ++order_;
    v.velGain = (1.0f - p.velSens) + p.velSens * vel01 * sqrtf(vel01);
    const float hz = 440.0f * exp2_((float)(note - 69) / 12.0f);
    v.keyTrk = fclamp(log2f(hz / kKeyRef) / 5.0f, -1.0f, 1.0f);
    if (fresh) {
        rng_ = rng_ * 1664525u + 1013904223u;
        v.rng = rng_ | 1u;
        v.amp = 0.0f; v.e2 = 0.0f;
        v.ch[0] = Voice::Chain(); v.ch[1] = Voice::Chain(); v.stereo = false;
        for (int i = 0; i < kMaxUnison; ++i) v.pdS1[i] = PdState();
        v.pdS2 = PdState();
        v.phS = v.ph1;
        { const uint32_t sd = 0x13579BDFu + order_ * 2654435761u; v.wow.seed(sd, (int)(sr_ / 2.7f)); v.flutter.seed(sd ^ 0x9E3779B9u, (int)(sr_ / 11.0f)); v.lvl.seed(sd + 0x6D2B79F5u, (int)(sr_ / 1.3f)); v.wowPrev = 0.7f * v.wow.at() + 0.3f * v.flutter.at(); }

        for (int i = 0; i < kMaxUnison; ++i) v.ph1u[i] = (uint32_t)((double)kUnisonSeed[i] * k2p32);
    }
    v.stage = 0; v.stage2 = 0;
    const float atk = p.atkMs * (1.0f - p.velAtk * vel01 * 0.9f);
    v.atkK = attackCoef(atk, sr_);
    v.decC = dbCoef(p.decMs, sr_);
    v.relC = dbCoef(p.relMs, sr_);
    v.atk2K = attackCoef(1.0f, sr_);
    v.dec2C = dbCoef(p.decMs * 0.8f + 20.0f, sr_);
    v.rel2C = dbCoef(p.relMs, sr_);
    lfoRate_ = p.lfoRate;
    return pick;
}

void Part::release(int voice, uint32_t order) {
    if (voice < 0 || voice >= kMaxVoices) return;
    Voice& v = *v_[voice];
    if (v.used && v.held && v.order == order) { v.held = false; v.stage = 3; v.stage2 = 3; }
}

void Part::setParams(int voice, const Params& p) {
    if (voice < 0 || voice >= kMaxVoices || !v_[voice]->used) return;
    Voice& v = *v_[voice];
    v.P = p;
    v.velGain = (1.0f - p.velSens) + p.velSens * v.vel * sqrtf(v.vel);
    v.decC = dbCoef(p.decMs, sr_);
    v.relC = dbCoef(p.relMs, sr_);
    v.dec2C = dbCoef(p.decMs * 0.8f + 20.0f, sr_);
    v.rel2C = dbCoef(p.relMs, sr_);
    lfoRate_ = p.lfoRate;   // the LFO is the part's: its rate follows the knob
}

void Part::noteOff(int note) {
    // the most recently started held voice with that note (the engine releases the newest group)
    int pick = -1;
    for (int i = 0; i < kMaxVoices; ++i)
        if (v_[i]->used && v_[i]->held && v_[i]->note == note && (pick < 0 || v_[i]->order > v_[pick]->order)) pick = i;
    if (pick >= 0) { v_[pick]->held = false; v_[pick]->stage = 3; v_[pick]->stage2 = 3; }
}

void Part::idle(int frames) {   // no voice sounds: the free-running LFO moves on all the same
    lfoPhase_ += lfoRate_ * (float)frames / sr_;
    lfoPhase_ -= (float)floorInt(lfoPhase_);
}

void Part::render(float* left, float* right, int frames, float gain) {
    gain_ = gain;
    for (int i = 0; i < kMaxVoices; ++i)
        if (v_[i]->used) renderVoice(*v_[i], left, right, frames);
    // the LFO is free-running and shared by the part's voices: it moves on whether or not a voice sounds
    lfoPhase_ += lfoRate_ * (float)frames / sr_;
    lfoPhase_ -= (float)floorInt(lfoPhase_);
}

// A peak follower: instant attack, a hold, then a release (the engine's ResonanceCeiling).
static inline void follow(float& env, int& hold, float x, int holdSamples, float release) {
    if (x >= env) { env = x; hold = holdSamples; }
    else if (x >= 0.97f * env) hold = holdSamples;
    else if (hold > 0) --hold;
    else { const float d = env * release; env = d > x ? d : x; if (env < 1.0e-20f) env = 0.0f; }
}

static void runFilter(FilterUnit& u, int model, float cutoff, float q0, float drive, float sr, float* buf, int n) {
    float ref[kCtl];
    const float r0 = reso01(q0);
    const bool svf = model <= M_NOTCH;
    // the ceiling only matters where the core can lift its input past 1.25: a resonant peak
    const bool ceil = svf ? q0 > 3.0f : r0 > 0.6f;
    const float blend = svf ? 0.0f : (r0 > 0.92f ? (r0 >= 1.0f ? 1.0f : ([](float t) { return t * t * (3.0f - 2.0f * t); })((r0 - 0.92f) / 0.08f)) : 0.0f);
    const float gDrive = 1.0f + 6.0f * drive;
    const float dMix = fclamp((gDrive - 1.0f) / 1.5f, 0.0f, 1.0f);   // how far the drive's gain is bent into the soft clip (ladder family)
    if (drive > 0.0f && model <= M_NOTCH)
        for (int i = 0; i < n; ++i) buf[i] = softClip(gDrive * buf[i]);
    if (model == M_LP || model == M_BP || model == M_HP || model == M_NOTCH || model == M_SOVIET) {
        float q = q0;
        if (model == M_SOVIET) {
            q = 1.0f / (2.0f * (1.0f - reso01(q0)) + 0.02f * reso01(q0));
            if (drive > 0.0f) for (int i = 0; i < n; ++i) buf[i] = driveIn(buf[i], gDrive, dMix);
        }
        if (ceil || blend > 0.0f) for (int i = 0; i < n; ++i) ref[i] = fabsf(buf[i]);
        const SvfCoef c1 = svfCoef(cutoff, q, sr);
        if (!u.coefInit) { u.cf = c1; u.coefInit = true; }
        const float inv = 1.0f / (float)n;
        const float d1 = (c1.a1 - u.cf.a1) * inv, d2 = (c1.a2 - u.cf.a2) * inv, d3 = (c1.a3 - u.cf.a3) * inv;
        SvfCoef c = u.cf;
        const int mode = model == M_BP ? 1 : (model == M_HP ? 2 : (model == M_NOTCH ? 3 : 0));
        for (int i = 0; i < n; ++i) { buf[i] = u.svf.tick(buf[i], c, mode); c.a1 += d1; c.a2 += d2; c.a3 += d3; }
        u.cf = c1;
    } else {
        const float r = reso01(q0);
        LadderCoef c1;
        float inGain = 1.0f;
        switch (model) {
            case M_LADDER: c1 = ladderCoef(cutoff, 3.99f * r, sr, 4); inGain = gDrive; break;
            case M_ACID: c1 = ladderCoef(cutoff * 0.57735027f, 7.8f * r, sr, 3); break;
            default: {   // CLASSIC: input compensation (1 + k), and a cap on G that keeps the loop inside its stability
                const float k = 3.9f * r;
                c1 = ladderCoef(cutoff, k, sr, 4, 1.0f / sqrtf(1.0f + 1.5f * k));
                inGain = 1.0f + k;
                break;
            }
        }
        if (!u.coefInit) { u.lc = c1; u.coefInit = true; }
        if (ceil || blend > 0.0f) {
            if (model == M_ACID || model == M_CLASSIC) for (int i = 0; i < n; ++i) ref[i] = fabsf(driveIn(buf[i], gDrive, dMix));
            else for (int i = 0; i < n; ++i) ref[i] = fabsf(buf[i]);
        }
        const float inv = 1.0f / (float)n;
        const float dG = (c1.G - u.lc.G) * inv, dN = (c1.norm - u.lc.norm) * inv;
        LadderCoef c = u.lc;
        c.k = c1.k;
        if (model == M_ACID) {
            for (int i = 0; i < n; ++i) {
                const float yc = fclamp(u.ladder.y3, -4.0f, 4.0f);   // the clipped excess is fed back one sample late: held bounded so a loop at its edge cannot run away
                const float extra = c.k * (yc - softClip(yc));
                buf[i] = u.ladder.tick<3, false>(driveIn(buf[i], gDrive, dMix), c, extra);
                c.G += dG; c.norm += dN;
            }
        } else if (model == M_CLASSIC) {
            for (int i = 0; i < n; ++i) { buf[i] = u.ladder.tick<4, true>(inGain * driveIn(buf[i], gDrive, dMix), c, 0.0f); c.G += dG; c.norm += dN; }
        } else if (drive > 0.0f) {
            for (int i = 0; i < n; ++i) { buf[i] = u.ladder.tick<4, true>(inGain * buf[i], c, 0.0f); c.G += dG; c.norm += dN; }
        } else {
            for (int i = 0; i < n; ++i) { buf[i] = u.ladder.tick<4, false>(buf[i], c, 0.0f); c.G += dG; c.norm += dN; }
        }
        u.lc = c1;
    }
    if (ceil) {   // the dry ceiling: the output may not stand above max(1.25, 2 x the input's peak)
        const int hold = (int)(0.025f * sr + 0.5f);
        const float rel = fastExp2(-1.4426950f / (0.150f * sr));
        for (int i = 0; i < n; ++i) {
            follow(u.envI, u.holdI, ref[i], hold, rel);
            follow(u.envO, u.holdO, fabsf(buf[i]), hold, rel);
            const float cl = u.envI * 2.0f > 1.25f ? u.envI * 2.0f : 1.25f;
            if (u.envO > cl) buf[i] *= cl / u.envO;
        }
    }
    if (!(fabsf(buf[n - 1]) < 1.0e5f)) {   // a filter that has run away (or a NaN) is cleared rather than left to ring through the voice
        u = FilterUnit();
        for (int i = 0; i < n; ++i) buf[i] = 0.0f;
        return;
    }
    if (blend > 0.0f)   // near self-oscillation the output is held inside +-1: y = x + b (1.5 S(2x/3) - x)
        for (int i = 0; i < n; ++i) buf[i] += blend * (1.5f * u.lim.tick(buf[i] * (2.0f / 3.0f)) - buf[i]);
}

void Part::renderVoice(Voice& v, float* outL, float* outR, int frames) {
    const Params& P = v.P;
    const float invSr = 1.0f / sr_;
    float buf[kCtl];
    float lfoPh = lfoPhase_;
    for (int base = 0; base < frames; base += kCtl) {
        const int n = frames - base < kCtl ? frames - base : kCtl;

        // ---- control block: LFO, Env2 at the block's first sample, the sums that move cutoff, pitch, position and level
        float lfo;
        switch (P.lfoShape) {
            case 0: lfo = fastSinCycles(lfoPh); break;
            case 1: lfo = lfoPh < 0.25f ? 4.0f * lfoPh : (lfoPh < 0.75f ? 2.0f - 4.0f * lfoPh : 4.0f * lfoPh - 4.0f); break;
            case 2: lfo = 2.0f * lfoPh - 1.0f; break;
            default: lfo = lfoPh < 0.5f ? 1.0f : -1.0f; break;
        }
        lfoPh += lfoRate_ * invSr * (float)n;
        lfoPh -= (float)floorInt(lfoPh);
        const float e2 = v.e2;
        for (int i = 0; i < n; ++i) {   // Env2 moves on every sample, read at the block's start
            switch (v.stage2) {
                case 0: v.e2 += (1.3f - v.e2) * v.atk2K; if (v.e2 >= 1.0f) { v.e2 = 1.0f; v.stage2 = 1; } break;
                case 1: v.e2 *= v.dec2C; if (v.e2 < 1e-6f) { v.e2 = 0.0f; v.stage2 = 2; } break;
                case 2: break;
                default: v.e2 *= v.rel2C; if (v.e2 < 1e-5f) { v.e2 = 0.0f; } break;
            }
        }
        const float lfoCut = P.lfoDest == 0 ? 0.25f * P.lfoDepth * lfo : 0.0f;
        const float cutMod = P.envAmt * e2 + P.keyTrack * v.keyTrk + lfoCut + wheel_ * P.wheel + pressure_ * P.aftertouch;   // matrix units (x4 octaves)
        const float cutoff = P.cutoff * exp2_(4.0f * cutMod);
        const float pitchSt = (P.lfoDest == 1 ? pitchSemis(0.3f * P.lfoDepth * lfo) : 0.0f) + bend_ * P.bendRange;
        float driftSt = 0.0f, driftGain = 1.0f;
        if (P.gritOn && (P.gritDrift > 0.0f || P.gritWow > 0.0f)) {   // the converter's clock wanders: a delay that moves by `depth` samples is a pitch change
            const float wowNow = v.wow.advance(n);
            driftSt = 0.8f * P.gritWow * wowNow;   // WOW: the same slow wander, directly on the pitch, up to most of a semitone
            const float mp = 0.7f * wowNow + 0.3f * v.flutter.advance(n);
            const float ratio = -(0.0012f * sr_) * P.gritDrift * (mp - v.wowPrev) / (float)n;
            v.wowPrev = mp;
            driftSt += 17.312340f * ratio;
            driftGain = 1.0f + 0.15f * P.gritDrift * v.lvl.advance(n);
        }
        const float ampMod = P.lfoDest == 2 ? fmaxf(0.0f, 1.0f + 0.5f * P.lfoDepth * lfo) : 1.0f;
        float posMod = 0.0f;
        if (P.tab) posMod = (P.lfoDest == 3 ? P.lfoDepth * lfo : 0.0f) + P.env2Pd * e2;

        // ---- oscillators into buf
        const float noteHz = 440.0f * exp2_(((float)v.note - 69.0f + pitchSt + driftSt) / 12.0f);
        float f[2] = {noteHz * P.ratio[0], noteHz * P.ratio[1]};
        const bool osc1 = P.level[0] > 0.0f, osc2 = P.level[1] > 0.0f;
        TableRef t1, t2;
        if (osc1) pickTable(*P.wt[0], P.sine[0] ? 1.0f : P.pos[0] + posMod, f[0] * invSr, t1, P.sine[0] ? 99 : -1);
        if (osc2) pickTable(*P.wt[1], P.sine[1] ? 1.0f : P.pos[1] + posMod, f[1] * invSr, t2, P.sine[1] ? 99 : -1);
        const bool ringOn = osc1 && osc2 && P.ring > 0.0f, syncOn = osc1 && P.sync > 0.0f && !P.pd[0];
        const bool xmod = ringOn || syncOn;   // a live ring or sync collapses osc1's unison stack
        const bool stack = osc1 && P.unison > 1 && P.uniDetune > 0.0f && !xmod;
        uint32_t inc1[kMaxUnison];
        float gu[kMaxUnison];
        if (stack) {
            const int N = P.unison;
            float wsum = 0.0f, w[kMaxUnison];
            for (int k = 0; k < N; ++k) {
                inc1[k] = phaseInc(f[0] * P.uniRatio[k], invSr);
                const bool centre = (N & 1) ? (k == N / 2) : (k == N / 2 - 1 || k == N / 2);
                w[k] = centre ? 1.0f : P.uniBlend;
                wsum += w[k] * w[k];
            }
            const float nrm = 1.0f / sqrtf(wsum);
            for (int k = 0; k < N; ++k) gu[k] = w[k] * nrm;
        } else {
            inc1[0] = phaseInc(f[0], invSr);
        }
        const uint32_t inc2 = phaseInc(f[1], invSr);
        PdCoef pc[2];
        if (P.pd[0] || P.pd[1]) {
            const float mod = P.tab ? 0.0f : (P.lfoDest == 3 ? P.lfoDepth * lfo : 0.0f) + P.env2Pd * e2;
            for (int s = 0; s < 2; ++s) if (P.pd[s]) pc[s] = pdCoef(P.pdBase[s] + mod, P.pdShape[s], f[s] * invSr);
        }
        const PdTables& PT = pdTables();
        const float l1 = P.level[0], l2 = P.level[1];
        for (int i = 0; i < n; ++i) buf[i] = 0.0f;
        // width: the members are panned (hard left .. hard right, the sum of the positions 0), the voice becomes a stereo pair L = mid + side, R = mid - side
        const bool stereo = stack && P.uniWidth > 0.0f;
        if (stereo && !v.stereo) v.ch[1] = v.ch[0];   // the right chain starts from the left's state (no click when the width knob comes up)
        v.stereo = stereo;
        float side[kCtl], upan[kMaxUnison];
        if (stereo) for (int k = 0; k < P.unison; ++k) upan[k] = P.uniWidth * (float)(2 * k - (P.unison - 1)) / (float)(P.unison - 1);
        if (xmod) {
            // osc2 is the master: it sets the period the synced copy of osc1 restarts at (with a polyBLEP step correction), and it is the second factor of the ring
            const float amt = syncOn ? P.sync : 0.0f, d = ringOn ? P.ring : 0.0f;
            for (int i = 0; i < n; ++i) {
                const uint32_t old2 = v.ph2;
                float s1 = 0.0f;
                if (osc2) { s1 = P.pd[1] ? pdTick(v.ph2, inc2, pc[1], v.pdS2, PT) * l2 : readTable(t2, v.ph2) * l2; if (!P.pd[1]) v.ph2 += inc2; }
                else v.ph2 += inc2;
                float s0 = (P.pd[0] ? pdTick(v.ph1, inc1[0], pc[0], v.pdS1[0], PT) : readTable(t1, v.ph1)) * l1;
                if (!P.pd[0]) v.ph1 += inc1[0];
                if (syncOn) {
                    if (v.ph2 < old2 && inc2) {   // the master wrapped a fraction a of a sample ago
                        const float a = (float)v.ph2 / (float)inc2;
                        const float before = readTable(t1, v.phS - (uint32_t)(a * (float)inc1[0])) * l1, after = readTable(t1, 0u) * l1;
                        const float dl = after - before;
                        v.phS = (uint32_t)(a * (float)inc1[0]);
                        const float sy = readTable(t1, v.phS) * l1 + 0.5f * dl * (2.0f * a - a * a - 1.0f);
                        if (i > 0) buf[i - 1] += amt * 0.5f * dl * a * a;
                        s0 += amt * (sy - s0);
                    } else {
                        const float sy = readTable(t1, v.phS) * l1;
                        s0 += amt * (sy - s0);
                    }
                    v.phS += inc1[0];
                }
                if (ringOn) s0 += d * (s0 * s1 - s0);
                buf[i] = s0 + s1;
            }
        } else {
        if (osc1) {
            if (stack) {
                for (int i = 0; i < n; ++i) {
                    float a = 0.0f, sd = 0.0f;
                    for (int k = 0; k < P.unison; ++k) {
                        float x;
                        if (P.pd[0]) x = gu[k] * pdTick(v.ph1u[k], inc1[k], pc[0], v.pdS1[k], PT);
                        else { x = gu[k] * readTable(t1, v.ph1u[k]); v.ph1u[k] += inc1[k]; }
                        a += x;
                        if (stereo) sd += x * upan[k];
                    }
                    buf[i] = a * l1;
                    side[i] = sd * l1;
                }
            } else {
                if (P.pd[0]) for (int i = 0; i < n; ++i) buf[i] = pdTick(v.ph1, inc1[0], pc[0], v.pdS1[0], PT) * l1;
                else for (int i = 0; i < n; ++i) { buf[i] = readTable(t1, v.ph1) * l1; v.ph1 += inc1[0]; }
            }
        }
        if (osc2) {
            if (P.pd[1]) for (int i = 0; i < n; ++i) buf[i] += pdTick(v.ph2, inc2, pc[1], v.pdS2, PT) * l2;
            else for (int i = 0; i < n; ++i) { buf[i] += readTable(t2, v.ph2) * l2; v.ph2 += inc2; }
        }
        }
        if (P.subLevel > 0.0f) {
            // a pure sine, so no table: a rotation, restarted from the phase every control block (under -100 dB of error up to 2 kHz)
            const uint32_t incS = phaseInc(noteHz * (P.subOct <= -2 ? 0.25f : 0.5f), invSr);
            const float w = 6.2831853f * (float)incS * (1.0f / 4294967296.0f), w2 = w * w;
            const float rc = 1.0f - w2 * (0.5f - w2 / 24.0f), rs = w * (1.0f - w2 * (1.0f / 6.0f - w2 / 120.0f));
            const float p = (float)v.phSub * (1.0f / 4294967296.0f);
            float sn = fastSinCycles(p), cs = fastSinCycles(p + 0.25f);
            for (int i = 0; i < n; ++i) {
                buf[i] += sn * P.subLevel;
                const float t = sn * rc + cs * rs;
                cs = cs * rc - sn * rs;
                sn = t;
            }
            v.phSub += (uint32_t)n * incS;
        }
        float nz[kCtl];
        if (P.noise > 0.0f) {
            const float g = P.noise * (1.0f / 2147483648.0f);
            for (int i = 0; i < n; ++i) { v.rng = v.rng * 1664525u + 1013904223u; nz[i] = (float)(int32_t)v.rng * g; }
        }

        // ---- the chain after the oscillators, once per channel: HPF, converter, noise, filter 1 (and 2)
        auto runChain = [&](Voice::Chain& c, float* b) {
            // HPF: 12 dB Butterworth on the oscillators and the sub, not on the noise
            if (P.hpf > 0.0f) {
                const SvfCoef hc = svfCoef(P.hpf, 0.70710678f, sr_);
                for (int i = 0; i < n; ++i) b[i] = c.hp.tick(b[i], hc, 2);
            }
            if (P.gritOn && P.gritLoopErr > 0.0f) {   // LOOP ERR: the wave's loop is a little off, so a delay grows through each cycle and snaps back at the seam: a buzz at the note
                const float inc = fminf(f[0] / sr_, 0.5f), amount = P.gritLoopErr * P.gritMix;
                float seam = c.seam, prev = c.seamPrev;
                for (int i = 0; i < n; ++i) {
                    const float x = b[i];
                    b[i] = x + amount * seam * (prev - x);
                    prev = x;
                    seam += inc;
                    if (seam >= 1.0f) seam -= 1.0f;
                }
                c.seam = seam;
                c.seamPrev = prev;
            }
            if (P.gritOn) {   // sample-and-hold at the converter rate, the bits and the companding at each capture, then the wet/dry mix; the clock drift's level wobble
                const float inc = P.gritRate / sr_;
                const float lh = exp2_(P.gritBits - 1.0f);
                const bool comp = P.gritCompand > 0.0f;
                const float mu = 255.0f * P.gritCompand, lmu = comp ? logf(1.0f + mu) : 0.0f;
                const float gd0 = c.gritGain, gd1 = driftGain, dg = (gd1 - gd0) / (float)n;
                float gg = gd0;
                for (int i = 0; i < n; ++i) {
                    const float x = b[i];
                    float y;
                    if (inc >= 1.0f) y = x;
                    else {
                        c.cvPhase += inc;
                        if (c.cvPhase >= 1.0f) {
                            c.cvPhase -= 1.0f;
                            float q = x;
                            if (comp) { const float ax = fabsf(q); q = (q < 0 ? -1.0f : 1.0f) * logf(1.0f + mu * ax) / lmu; }
                            if (P.gritBits < 15.5f) q = roundf(q * lh) / lh;
                            else if (comp) q = roundf(q * lh) / lh;
                            if (comp) { const float aq = fabsf(q); q = (q < 0 ? -1.0f : 1.0f) * expm1f(aq * lmu) / mu; }
                            c.cvHeld = q;
                        }
                        y = c.cvHeld;
                    }
                    if (inc >= 1.0f) {   // no rate reduction: the bits and the companding act on every sample
                        float q = x;
                        if (comp) { const float ax = fabsf(q); q = (q < 0 ? -1.0f : 1.0f) * logf(1.0f + mu * ax) / lmu; }
                        q = roundf(q * lh) / lh;
                        if (comp) { const float aq = fabsf(q); q = (q < 0 ? -1.0f : 1.0f) * expm1f(aq * lmu) / mu; }
                        y = q;
                    }
                    y = x + P.gritMix * (y - x);
                    b[i] = y * gg;
                    gg += dg;
                }
                c.gritGain = gd1;
            }
            if (P.noise > 0.0f) for (int i = 0; i < n; ++i) b[i] += nz[i];
            // ---- filter 1 (then filter 2, in series or in parallel)
            const float cutMod2 = cutoff;   // F1's modulated cutoff
            const float own2 = P.f2on && (P.envAmt2 > 0.0f || P.keyTrack2 > 0.0f) ? exp2_(4.0f * (P.envAmt2 * e2 + P.keyTrack2 * v.keyTrk)) : 1.0f;   // filter 2's own KEYTRK and ENV AMT, on top of following it
            if (P.f2on && P.f2route == 1) {   // parallel: both see the same input; the balance knob turns filter 1 down
                float tmp[kCtl];
                for (int i = 0; i < n; ++i) tmp[i] = b[i];
                runFilter(c.f1, P.model, cutMod2, P.q, P.drive, sr_, b, n);
                runFilter(c.f2, P.model2, P.cutoff2 * (cutMod2 / P.cutoff) * own2, P.q2, P.drive2, sr_, tmp, n);
                const float g1 = P.balance <= 0.0f ? 1.0f : 1.0f - P.balance;
                for (int i = 0; i < n; ++i) b[i] = g1 * b[i] + tmp[i];
            } else {
                runFilter(c.f1, P.model, cutMod2, P.q, P.drive, sr_, b, n);
                if (P.f2on) runFilter(c.f2, P.model2, P.cutoff2 * (cutMod2 / P.cutoff) * own2, P.q2, P.drive2, sr_, b, n);
            }

        };
        float bufR[kCtl];
        if (stereo) for (int i = 0; i < n; ++i) { bufR[i] = buf[i] - side[i]; buf[i] += side[i]; }
        runChain(v.ch[0], buf);
        if (stereo) runChain(v.ch[1], bufR);

        // ---- amp: the envelope per sample, velocity, level (with the LFO), pan
        const float g = P.level_ * ampMod * gain_;
        const float pl = sqrtf(0.5f * (1.0f - P.pan)) * g, pr = sqrtf(0.5f * (1.0f + P.pan)) * g;
        const float gClip = 1.0f + 6.0f * P.clip;
        float ev[kCtl];
        for (int i = 0; i < n; ++i) {
            ev[i] = v.amp;
            switch (v.stage) {
                case 0: v.amp += (1.3f - v.amp) * v.atkK; if (v.amp >= 1.0f) { v.amp = 1.0f; v.stage = 1; } break;
                case 1: { const float gap = (v.amp - P.sustain) * v.decC; v.amp = P.sustain + gap; if (fabsf(gap) <= 1e-6f) { v.amp = P.sustain; v.stage = 2; } break; }
                case 2: break;
                default: v.amp *= v.relC; break;
            }
        }
        for (int i = 0; i < n; ++i) {
            float y = buf[i] * ev[i] * v.velGain;
            if (P.clip > 0.0f) y = v.ch[0].clip.tick(gClip * y);
            outL[base + i] += y * pl;
            if (!stereo) outR[base + i] += y * pr;
        }
        if (stereo) for (int i = 0; i < n; ++i) {
            float y = bufR[i] * ev[i] * v.velGain;
            if (P.clip > 0.0f) y = v.ch[1].clip.tick(gClip * y);
            outR[base + i] += y * pr;
        }
        if (v.stage == 3 && v.amp <= 1e-5f) { v.amp = 0.0f; v.used = false; break; }
    }
}

}  // namespace lean
