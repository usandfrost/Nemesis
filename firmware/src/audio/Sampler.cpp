#include "audio/Sampler.h"

#include "lean/LeanDsp.h"

#include <math.h>

#include "app/Looper.h"

namespace audio {

using namespace app;

static inline float u01(int v) { return (float)v / 127.0f; }
// log2 and 2^x, near enough for a converter's steps: these run in the audio interrupt, per sample
static inline float fastLog2(float x) {
    union { float f; uint32_t i; } u = {x};
    const int e = (int)((u.i >> 23) & 255) - 127;
    u.i = (u.i & 0x007FFFFFu) | 0x3F800000u;
    const float m = u.f;
    return (float)e + (-1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m);
}
static inline float fastExp2(float x) {
    const float fl = floorf(x), f = x - fl;
    union { uint32_t i; float f; } u;
    u.i = (uint32_t)((int)fl + 127) << 23;
    return u.f * (1.0f + f * (0.69606564f + f * (0.22449433f + f * 0.079440236f)));
}
static inline float triWave(float p) { return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p; }

static inline float softClip(float x) {   // tanh-like, cheap: x (27 + x^2) / (27 + 9 x^2), held at +-1
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// sin(x pi / 2) for x in 0..1, near enough for a gain: a sine law leaves the ends of a fade smoothly, where a square root turns a corner
static inline float quarterSine(float x) {
    const float z = x * x;
    return x * (1.5707963f + z * (-0.6459641f + z * (0.0796926f - z * 0.0046f)));
}

// The loop as it plays, once its points are known. What fades in is what comes before the point the end goes back to, so the lap stays end - start. With too little
// sample before START for the fade the turn moves in, and the lap is that much shorter.
static void settleLoop(SampleHit& h) {
    const double span = h.end - h.start;
    if (h.loopPct > 0.0) h.loopFade = h.loopPct * span;
    if (h.loopFade > span * 0.5) h.loopFade = span * 0.5;
    if (h.reverse) {
        const double top = (double)h.length - 1.0 - h.loopFade;
        h.loopTurn = h.end < top ? h.end : top;
        h.loopLen = h.loopTurn - h.start;
    } else {
        h.loopTurn = h.start > h.loopFade ? h.start : h.loopFade;
        h.loopLen = h.end - h.loopTurn;
    }
}

bool makeHit(const Lane& lane, const Lock* locks, int nLocks, int semis, float vel, float partLevel, const SampleLib& lib, double sr,
             SampleHit& h) {
    int16_t pg[kLanePages][kParamsPerPage];
    for (int g = 0; g < kLanePages; ++g)
        for (int i = 0; i < kParamsPerPage; ++i) pg[g][i] = lane.smp[g][i];
    for (int i = 0; i < nLocks; ++i) {   // p-locks (a sample lock, SMP:0, is the sample per step)
        const int li = lanePageIndex(lockPage(locks[i].key));
        if (li >= 0) pg[li][lockParam(locks[i].key)] = locks[i].value;
    }
    const int16_t* sm = pg[0];
    const int16_t* fl = pg[1];
    const int16_t* am = pg[2];
    const int16_t* gr = pg[4];
    const int16_t* md = pg[3];
    if (lib.count <= 0) return false;
    const SampleItem& it = lib.items[sm[0] < 0 ? 0 : sm[0] >= lib.count ? lib.count - 1 : sm[0]];
    if (it.length < 2 || !it.data) return false;
    h.smp = &it;
    h.data = it.data;
    h.length = it.length;
    const int mode = sm[3] < 0 ? 0 : sm[3] > 2 ? 2 : sm[3];
    const int16_t* lp = pg[lanePageIndex(PG_LOOP)];
    LoopRegion region;
    loopRegion(sm, mode == 2 ? lp : nullptr, it.length, it.rate, region);
    h.start = region.start;
    h.end = region.end;
    h.loopFade = region.fade;
    h.loopCurve = (uint8_t)(lp[LP_CURVE] < 0 ? 0 : lp[LP_CURVE] > CURVE_MATCH ? CURVE_MATCH : lp[LP_CURVE]);
    h.wantS = sm[1];
    h.wantE = sm[2];
    h.loopPct = mode == 2 && lp[LP_UNIT] == LOOP_PCT ? lp[LP_XFADE] / 127.0 * 0.5 : 0.0;
    h.autoLoop = (uint8_t)(mode == 2 ? lp[LP_AUTO] : 0);
    h.search = (uint8_t)lp[LP_SEARCH];
    h.fadeKey = loopFadeKey(lp);
    h.step = pow(2.0, (sm[4] + sm[5] / 100.0 + semis) / 12.0) * it.rate / sr;
    h.mode = (uint8_t)(sm[3] < 0 ? 0 : sm[3] > 2 ? 2 : sm[3]);
    h.reverse = sm[6] == 1;
    // the envelope, in the mockup's units (2 ms .. 2.5 s); decay and release are time constants (setTargetAtTime: tau = time / 3)
    h.attack = (float)(envMs(am[0]) / 1000.0);
    h.decayTau = (float)(envMs(am[1]) / 1000.0 / 3.0 + 0.001);
    h.sustain = u01(am[2]);
    h.releaseTau = (float)(envMs(am[3]) / 1000.0 / 3.0 + 0.005);
    // the level: velocity through VEL SENS, the sample's LEVEL, the AMP LEVEL, the part (as the mockup's playSample)
    const float vs = u01(am[4]), v = vel / 127.0f;
    h.level = v * (1.0f - vs * 0.6f + v * vs * 0.6f) * u01(sm[7]) * u01(am[5]) * partLevel * 0.9f;
    h.pan = (am[6] - 64) / 64.0f;
    if (h.pan < -1) h.pan = -1;
    if (h.pan > 1) h.pan = 1;
    h.choke = (uint8_t)am[7];
    h.model = (uint8_t)(fl[0] < 0 ? 0 : fl[0] > 2 ? 2 : fl[0]);
    const float cut = (float)cutHz(fl[1]);
    h.cutoff = cut < 40 ? 40 : cut > 18000 ? 18000 : cut;
    const float peak = h.cutoff * (1.0f + (u01(fl[4]) + u01(md[5])) * 6.0f);
    h.lfoHz = 0.2f + u01(md[0]) * 12.0f;
    h.lfoDepth = (md[2] & 3) == 3 ? 0.0f : u01(md[1]);   // (the panel stops at AMP on a sampler lane: there is no phase distortion here)
    h.lfoDest = (uint8_t)(md[2] & 3);
    h.lfoShape = (uint8_t)(md[3] & 3);
    h.wheelAmt = u01(md[6]) * 0.5f;
    h.pressAmt = u01(md[7]) * 0.5f;
    h.peak = peak > 18000 ? 18000 : peak;
    h.q = 0.5f + u01(fl[2]) * 14.0f;
    h.drive = u01(fl[3]);
    h.hpf = fl[5] > 0 ? (float)cutHz(fl[5]) / 8.0f : 0.0f;
    // GRIT: bits, the converter's rate, the mix (the rest of the page is CATHODE's)
    h.bits = gr[0] < 1 ? 1 : gr[0];
    h.srHz = gr[2] < 127 ? (1.0f + u01(gr[2]) * 43.1f) * 1000.0f : 0.0f;
    h.gritMix = u01(gr[7]);
    h.compand = u01(gr[1]);
    h.drift = u01(gr[3]) * h.gritMix;
    h.loopErr = u01(gr[4]) * h.gritMix;
    h.wow = u01(gr[5]) * h.gritMix;
    h.clip = u01(gr[6]);
    settleLoop(h);
    return true;
}

bool applySlice(SampleHit& h, const Lane& lane, const SliceMap& m, int slice, bool flip, int bpm, int laneIndex) {
    if (slice < 0 || slice >= m.count || m.frames != h.length || !h.smp || h.length < 2) return false;
    const Slice& s = m.s[slice];
    double end = slice + 1 < m.count ? m.s[slice + 1].start : m.frames - 1;
    if (end > h.length - 1) end = h.length - 1;
    if (s.start >= end) return false;
    h.step *= pow(2.0, s.pitch / 12.0);
    h.level *= s.level / 127.0f;
    h.reverse = h.reverse != ((s.flags & SLICE_REVERSE) != 0);
    if (flip) h.reverse = !h.reverse;
    if (lane.smpParam(PG_SLC, SLC_MODE) == SLICE_SYNC) h.mode = 0;   // the next slice ends it, not a gate
    h.sliceTail = h.mode == 2 ? 0.0 : 0.006 * h.smp->rate;           // (a slice that loops never reaches its end)
    h.start = h.reverse ? (s.start > h.sliceTail ? s.start - h.sliceTail : 0.0) : s.start;
    h.end = h.reverse ? end : (end + h.sliceTail < h.length - 1 ? end + h.sliceTail : h.length - 1);
    h.cutLane = (uint8_t)(laneIndex + 1);
    if (lane.smpParam(PG_SLC, SLC_FIT) == FIT_PITCH && h.smp->rate && bpm > 0) {   // the whole loop lasts its beats: faster or slower, like a record
        const int b = lane.smpParam(PG_SLC, SLC_BEATS);
        h.step *= ((double)m.frames / h.smp->rate) / (kSliceBeats[b < 0 ? 0 : b >= kSliceBeatChoices ? kSliceBeatChoices - 1 : b] * 60.0 / bpm);
    }
    settleLoop(h);
    return true;
}

void applyLoopFit(SampleHit& h, const Project& pj, int sample) {
    if (!h.autoLoop || h.mode != 2) return;
    const LoopFit* f = findLoopFit(pj, sample, h.wantS, h.wantE, h.autoLoop, h.search, h.fadeKey);
    if (!f || f->frames != h.length || f->end <= f->start || f->end >= h.length) return;
    h.start = f->start;
    h.end = f->end;
    h.loopCorr = f->corr / 100.0f;
    settleLoop(h);
}

void Sampler::init(double sampleRate) {
    sr_ = sampleRate;
    for (Voice& v : v_) v = Voice();
}

int Sampler::active() const {
    int n = 0;
    for (const Voice& v : v_) n += v.on;
    return n;
}

void Sampler::fade(Voice& v, float seconds) {
    if (!v.on) return;
    v.stage = 4;
    const float n = seconds * (float)sr_;
    v.fadeStep = v.env / (n > 1 ? n : 1);
}

void Sampler::cut(int cutLane) {
    for (Voice& v : v_) if (v.on && v.stage != 4 && v.h.cutLane == cutLane) fade(v, 0.004f);
}

void Sampler::stopAll() {
    for (Voice& v : v_) fade(v, 0.006f);
}

int32_t Sampler::start(const SampleHit& h, int maxVoices, int offset) {
    if (maxVoices < 1) maxVoices = 1;
    if (maxVoices > kVoices) maxVoices = kVoices;
    // a choke group: the new hit cuts the old ones in its group
    if (h.choke > 0)
        for (Voice& v : v_)
            if (v.on && v.stage != 4 && v.h.choke == h.choke) fade(v, 0.006f);
    if (h.cutLane > 0)   // the slice before it ends where this one starts. Not a hit starting at the same moment: that is a chord
        for (Voice& v : v_)
            if (v.on && v.stage != 4 && v.h.cutLane == h.cutLane && v.startIn < offset && v.cutIn < 0) {
                v.cutIn = offset;
                v.cutSecs = h.attack < 0.002f ? 0.002f : h.attack > 0.02f ? 0.02f : h.attack;
            }
    // out of voices: the oldest still sounding fades out (it keeps its slot for those 6 ms)
    for (;;) {
        int sounding = 0, oldest = -1;
        for (int i = 0; i < kVoices; ++i)
            if (v_[i].on && v_[i].stage != 4) {
                ++sounding;
                if (oldest < 0 || v_[i].age < v_[oldest].age) oldest = i;
            }
        if (sounding < maxVoices || oldest < 0) break;
        fade(v_[oldest], 0.006f);
    }
    int slot = -1;
    for (int i = 0; i < kVoices && slot < 0; ++i) if (!v_[i].on) slot = i;
    if (slot < 0) {   // every slot busy (some still fading): take the quietest fading one
        for (int i = 0; i < kVoices; ++i)
            if (v_[i].stage == 4 && (slot < 0 || v_[i].env < v_[slot].env)) slot = i;
        if (slot < 0) slot = 0;
    }
    Voice& v = v_[slot];
    v = Voice();
    v.on = true;
    v.gen = gen_++;
    v.age = age_++;
    v.h = h;
    v.startIn = offset < 0 ? 0 : offset;
    v.pos = h.reverse ? h.end : h.start;
    v.stage = 0;
    v.coefIn = 0;
    const float th = (h.pan + 1.0f) * 0.785398163f;   // equal-power pan (the Web Audio StereoPanner law for a mono input)
    v.gl = cosf(th);
    v.gr = sinf(th);
    v.hpA = h.hpf > 0 ? expf(-6.2831853f * h.hpf / (float)sr_) : 0.0f;
    // the per-sample loop's constants, once per hit
    const float dt = 1.0f / (float)sr_;
    v.attackStep = h.attack > 0.0005f ? dt / h.attack : 1.0f;
    v.decayK = 1.0f - expf(-dt / h.decayTau);
    v.releaseK = 1.0f - expf(-dt / h.releaseTau);
    v.oneShotK = 1.0f - expf(-dt / 0.005f);
    v.levels = h.bits >= 16 ? 0.0f : powf(2.0f, h.bits - 1.0f);
    v.moving = h.lfoDepth > 0.0f || h.drift > 0.0f || h.wow > 0.0f || h.wheelAmt > 0.0f || h.pressAmt > 0.0f;   // (the last two cost next to nothing until a wheel moves)
    v.modIn = 0;
    v.lfoPhase = 0.0f;
    v.wanderT = 0.0f;
    v.ampMod = 1.0f;
    v.cutMul = 1.0f;
    v.stepMul = 1.0;
    v.rng = 0x9E3779B9u;
    v.mu = 255.0f * h.compand;
    v.lmuInv = v.mu > 0.0f ? 1.0f / fastLog2(1.0f + v.mu) : 0.0f;
    v.holdInc = h.srHz > 0 ? h.srHz / (float)sr_ : 1.0f;
    const double tail0 = 0.012 * sr_ * h.step, region = h.end - h.start;   // the one-shot's 12 ms tail, at most half its region
    v.tail = tail0 < region * 0.5 ? tail0 : region * 0.5;
    return (int32_t)(((v.gen & 0xFFFFFu) << 3) | (uint32_t)slot);
}

void Sampler::release(int32_t handle, int offset) {
    if (handle < 0) return;
    Voice& v = v_[handle & 7];
    if (!v.on || ((v.gen & 0xFFFFFu) != ((uint32_t)handle >> 3))) return;   // a later hit has the slot now
    if (v.h.mode == 0) return;                                                 // a one-shot plays its region
    v.releaseIn = offset < 0 ? 0 : offset;
}

float Sampler::sampleAt(const Voice& v, double pos) const {
    const int16_t* d = v.h.data;
    const uint32_t n = v.h.length;
    const uint32_t i = (uint32_t)pos;
    const float f = (float)(pos - i);
    const float a = d[i < n ? i : n - 1], b = d[i + 1 < n ? i + 1 : n - 1];
    return (a + (b - a) * f) * (1.0f / 32768.0f);
}

void Sampler::updateFilter(Voice& v) {
    // the cutoff sweeps from the envelope peak down to the cutoff over 120 ms (exponentially, as the mockup's ramp)
    float fc = v.h.cutoff * v.cutMul;
    if (v.sweepT < 0.12f && v.h.peak > v.h.cutoff) fc *= powf(v.h.peak / v.h.cutoff, 1.0f - v.sweepT / 0.12f);
    else v.swept = true;
    const float nyq = (float)sr_ * 0.45f;
    if (fc > nyq) fc = nyq;
    v.g = tanf(3.14159265f * fc / (float)sr_);
    v.k = 1.0f / v.h.q;
    v.a1 = 1.0f / (1.0f + v.g * (v.g + v.k));
    v.a2 = v.g * v.a1;
    v.a3 = v.g * v.a2;
}

// What moves a hit while it plays: worked out every kModEvery samples
static constexpr int kModEvery = 32;
void Sampler::modulate(Voice& v) {
    const SampleHit& h = v.h;
    const float sec = kModEvery / (float)sr_;
    float semis = 0.0f, cut = wheel_ * h.wheelAmt + pressure_ * h.pressAmt, amp = 1.0f;
    if (h.lfoDepth > 0.0f) {
        v.lfoPhase += h.lfoHz * sec;
        if (v.lfoPhase >= 1.0f) v.lfoPhase -= 1.0f;
        const float p = v.lfoPhase;
        const float w = h.lfoShape == 0 ? lean::fastSinCycles(p) : h.lfoShape == 1 ? triWave(p) : h.lfoShape == 2 ? 2.0f * p - 1.0f : (p < 0.5f ? 1.0f : -1.0f);
        if (h.lfoDest == 0) cut += 0.25f * h.lfoDepth * w;
        else if (h.lfoDest == 1) semis += h.lfoDepth * w;
        else amp = 1.0f + 0.5f * h.lfoDepth * w;
    }
    if (h.drift > 0.0f || h.wow > 0.0f) {
        v.wanderT += sec;
        const float t = v.wanderT;
        const auto wave = [](float hz, float at) { float c = hz * at; c -= floorf(c); return lean::fastSinCycles(c); };
        semis += 0.8f * h.wow * wave(0.7f, t) + 0.15f * h.drift * (wave(1.3f, t) + 0.5f * wave(6.1f, t));
    }
    v.stepMul = semis != 0.0f ? (double)fastExp2(semis / 12.0f) : 1.0;
    v.ampMod = amp < 0.0f ? 0.0f : amp;
    const float m = cut != 0.0f ? fastExp2(4.0f * cut) : 1.0f;
    if (m != v.cutMul) { v.cutMul = m; v.swept = false; }
}

// -1..1, the same run for each hit, so a render is the same each time
float Sampler::jitter(Voice& v) {
    v.rng ^= v.rng << 13;
    v.rng ^= v.rng >> 17;
    v.rng ^= v.rng << 5;
    return (float)(int32_t)v.rng / 2147483648.0f;
}

void Sampler::render(float* left, float* right, int frames) {
    const float dt = 1.0f / (float)sr_;
    for (Voice& v : v_) {
        if (!v.on) continue;
        const SampleHit& h = v.h;
        const float attackStep = v.attackStep, decayK = v.decayK, releaseK = v.releaseK, oneShotK = v.oneShotK, levels = v.levels, holdInc = v.holdInc;
        const double tail = v.tail;   // a one-shot ends by itself: its release starts this far before the end of the region
        for (int i = 0; i < frames && v.on; ++i) {
            if (v.cutIn >= 0 && v.cutIn-- == 0) fade(v, v.cutSecs);
            if (v.stage == 0) {
                if (v.startIn > 0) { --v.startIn; if (v.releaseIn > 0) --v.releaseIn; continue; }
                v.stage = 1;
            }
            if (v.releaseIn == 0 && v.stage < 3) v.stage = 3;
            if (v.releaseIn >= 0) --v.releaseIn;
            // the envelope
            switch (v.stage) {
                case 1: v.env += attackStep; if (v.env >= 1.0f) { v.env = 1.0f; v.stage = 2; } break;
                case 2: v.env += (h.sustain - v.env) * decayK; break;
                case 3: v.env -= v.env * releaseK; if (v.env < 1e-4f) v.on = false; break;
                case 4: v.env -= v.fadeStep; if (v.env <= 1e-7f) { v.env = 0; v.on = false; } break;   // (not 0: a tiny env has a step too small to reach it)
            }
            if (!v.on) break;
            if (h.cutLane && h.mode != 2 && v.stage < 4) {   // a slice fades to nothing in its tail
                const double left_ = h.reverse ? v.pos - h.start : h.end - v.pos;
                if (left_ <= h.sliceTail) fade(v, (float)(left_ / (h.step * v.stepMul) / sr_));
            }
            else if (h.mode == 0 && v.stage < 3) {
                const double left_ = h.reverse ? v.pos - h.start : h.end - v.pos;
                if (left_ <= tail && v.stage == 2) v.stage = 5;   // the one-shot's own release (after its attack)
            }
            if (v.stage == 5) { v.env -= v.env * oneShotK; if (v.env < 1e-4f) { v.on = false; break; } }
            if (v.moving && --v.modIn <= 0) { modulate(v); v.modIn = kModEvery; }
            // the sample
            float x = sampleAt(v, v.pos);
            const double fadeLen = h.mode == 2 ? h.loopFade : 0.0;
            if (fadeLen > 0.0) {   // nearing the turn: the end fades out as what leads up to the turn fades in, so the jump lands between two of the same sample
                const double left_ = h.reverse ? v.pos - h.start : h.end - v.pos;
                if (left_ < fadeLen) {
                    const float t = 1.0f - (float)(left_ / fadeLen);
                    const float y = sampleAt(v, h.reverse ? v.pos + h.loopLen : v.pos - h.loopLen);
                    float a_ = 1.0f - t, b_ = t;                                                     // LINEAR, for sounds in step: the level stays
                    if (h.loopCurve == CURVE_POWER) { a_ = quarterSine(a_); b_ = quarterSine(b_); }  // for sounds out of step: the power stays
                    else if (h.loopCurve == CURVE_LOG) { a_ = 1.0f - t * t; b_ = t * (2.0f - t); }   // the new sound arrives early, the old leaves late
                    else if (h.loopCurve == CURVE_MATCH) {                                           // the power stays, however alike the two are
                        const float over = sqrtf(a_ * a_ + b_ * b_ + 2.0f * h.loopCorr * a_ * b_);
                        a_ /= over;
                        b_ /= over;
                    }
                    x = x * a_ + y * b_;
                }
            }
            if (h.reverse) {
                v.pos -= h.step * v.stepMul;
                if (v.pos < h.start) { if (h.mode == 2 && h.loopLen > 0) v.pos = fmax(0.0, h.loopTurn - fmod(h.start - v.pos, h.loopLen) - (h.loopErr > 0.0f && h.loopLen > 64.0 ? 0.02 * h.loopErr * h.loopLen * (0.5 + 0.5 * jitter(v)) : 0.0)); else { v.on = false; } }
            } else {
                v.pos += h.step * v.stepMul;
                if (v.pos > h.end) { if (h.mode == 2 && h.loopLen > 0) v.pos = h.loopTurn + fmod(v.pos - h.end, h.loopLen) + (h.loopErr > 0.0f && h.loopLen > 64.0 ? 0.02 * h.loopErr * h.loopLen * (0.5 + 0.5 * jitter(v)) : 0.0); else { v.on = false; } }
            }
            // drive, filter, high-pass
            if (h.drive > 0.0f) x = softClip(x * (1.0f + 4.0f * h.drive));
            if (!v.swept && --v.coefIn <= 0) { updateFilter(v); v.coefIn = 16; }
            v.sweepT += dt;
            const float k = v.k, a1 = v.a1, a2 = v.a2, a3 = v.a3;
            const float v3 = x - v.ic2, v1 = a1 * v.ic1 + a2 * v3, v2 = v.ic2 + a2 * v.ic1 + a3 * v3;
            v.ic1 = 2.0f * v1 - v.ic1;
            v.ic2 = 2.0f * v2 - v.ic2;
            float y = h.model == 0 ? v2 : h.model == 1 ? v1 : x - k * v1 - v2;
            if (h.hpf > 0.0f) { const float hp = v.hpA * (v.hpY + y - v.hpX); v.hpX = y; v.hpY = hp; y = hp; }
            // the converter: bits and rate, mixed with the clean signal
            if (levels > 0.0f || h.srHz > 0.0f) {
                float z = y;
                const auto quantise = [&](float in) {
                    if (levels <= 0.0f) return in;
                    if (v.mu <= 0.0f) return floorf(in * levels + 0.5f) / levels;
                    const float a = fabsf(in) > 1.0f ? 1.0f : fabsf(in);   // mu-law: the steps are fine where the sound is quiet
                    const float c = floorf(fastLog2(1.0f + v.mu * a) * v.lmuInv * levels + 0.5f) / levels;
                    return copysignf((fastExp2(c / v.lmuInv) - 1.0f) / v.mu, in);
                };
                if (h.srHz > 0.0f) {   // (the steps only change at a capture)
                    v.holdPhase += holdInc;
                    if (v.holdPhase >= 1.0f) { v.holdPhase -= 1.0f; v.holdVal = quantise(y); }
                    z = v.holdVal;
                } else z = quantise(y);
                y = y + (z - y) * h.gritMix;
            }
            if (h.clip > 0.0f) y = y + (softClip(y * (1.0f + 4.0f * h.clip)) - y) * h.gritMix;
            const float out = y * v.env * h.level * v.ampMod;
            left[i] += out * v.gl;
            right[i] += out * v.gr;
        }
    }
}

}  // namespace audio
