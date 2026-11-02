#include "app/CostBridge.h"
#include "app/WaveLib.h"

namespace app {

using cost::Filter;

static const Filter kModelCost[kModelCount] = {Filter::LowPass, Filter::BandPass, Filter::HighPass, Filter::Notch,
                                              Filter::Ladder24, Filter::Acid, Filter::SovietLp, Filter::ClassicLp24};

const char* const kActionNames[] = {"release oldest voice", "unison width off", "unison off", "filter drive off", "cross-mod off",
#if DBOY_COST_LEAN
                                    "fold off", "converter off", "heavy filter to light", "filter 2 off", "amp to clean", "amp off",
#else
                                    "fold off", "companding off", "heavy filter to light", "filter 2 off", "amp to clean", "amp off",
#endif
                                    "delay off", "reverb off", "chorus off", "compressor off"};

Filter modelFilter(int menu) { return kModelCost[clampi(menu, 0, kModelCount - 1)]; }

int menuOf(Filter f) {
    for (int i = 0; i < kModelCount; ++i) if (kModelCost[i] == f) return i;
    return 4;
}

cost::VoiceDesc descOf(const Project& pj, int part, const Step* step) {
    const Part& p = pj.parts[part];
    auto g = [&](int page, int i) -> int { return step ? step->lockValue(lockKey(page, i), p.pages[page][i]) : p.pages[page][i]; };
    const int u = g(PG_SUB, 4);
    cost::VoiceDesc d;
#if DBOY_COST_LEAN
    d.slot[0].audible = g(PG_OSC1, 6) > 0;
#else
    d.slot[0].audible = true;
#endif
    // an oscillator on a wavetable bank costs what a table oscillator does (cheaper than the phase-distortion one)
    if (g(PG_OSC1, 0) >= kBasicWaves) d.slot[0].kind = cost::Osc::Wavetable;
    if (g(PG_OSC2, 0) >= kBasicWaves) d.slot[1].kind = cost::Osc::Wavetable;
#if DBOY_COST_LEAN
    d.slot[0].unison = (uint8_t)(g(PG_OSC1, 7) > 0 || g(PG_SUB, 5) > 0 ? u : 1);   // a stack with no detune is one oscillator
#else
    d.slot[0].unison = (uint8_t)u;
#endif
    d.slot[1].audible = g(PG_OSC2, 6) > 0;
    d.slot[1].unison = 1;
    d.f1 = modelFilter(g(PG_FLT1, 0));
    d.f2 = modelFilter(g(PG_FLT2, 0));
    d.f2On = g(PG_FLT2, 5) > 0;
    d.route = g(PG_FLT2, 4) ? cost::Route::Parallel : cost::Route::Series;
    d.drive = g(PG_FLT1, 3) > 0 ? 1.0f : 0.0f;
    d.fm = 0.0f;
    d.ring = g(PG_SUB, 3) > 0 ? 1.0f : 0.0f;
    d.sync = g(PG_OSC2, 7) > 0 ? 1.0f : 0.0f;
    d.width = u > 1 && g(PG_SUB, 7) > 0 ? 1.0f : 0.0f;
#if DBOY_COST_LEAN
    // the lean voice's converter runs as a whole (makeParams' gritOn), and the ladder's CompandOff takes all of it (makePatch)
    d.companding = g(PG_GRIT, 7) > 0 && (g(PG_GRIT, 0) < 16 || g(PG_GRIT, 1) > 0 || g(PG_GRIT, 2) < 127 || g(PG_GRIT, 3) > 0 || g(PG_GRIT, 4) > 0 || g(PG_GRIT, 5) > 0) ? 1.0f : 0.0f;
#else
    d.companding = g(PG_GRIT, 1) > 0 ? 1.0f : 0.0f;
#endif
    d.fold = 0.0f;
    // a sine slot with PD AMT up, or with the PD amount moved (the same reading as lean::makeParams)
    const bool tab = g(PG_OSC1, 0) >= kBasicWaves || g(PG_OSC2, 0) >= kBasicWaves;
    const bool pdMoves = !tab && (g(PG_MOD, 4) > 0 || ((g(PG_MOD, 2) & 3) == 3 && g(PG_MOD, 1) > 0));
    d.pd1 = g(PG_OSC1, 0) == 0 && (g(PG_OSC1, 2) > 0 || pdMoves);
    d.pd2 = g(PG_OSC2, 0) == 0 && (g(PG_OSC2, 2) > 0 || pdMoves);
    d.sub = g(PG_SUB, 0) > 0;
    d.noise = g(PG_SUB, 2) > 0;
    d.hpf = g(PG_FLT1, 6) > 0;
    d.clip = g(PG_GRIT, 6) > 0;
    return d;
}

cost::PartDesc partDesc(const Project& pj, int part, uint8_t priorityOverride) {
    cost::PartDesc d;
    const bool smp = isSampler(pj, part);
    if (smp) {
        // NO_DESC: one audible slot, nothing else (the sampler's cost is its voice count)
        d.voice = cost::VoiceDesc();
    } else {
        d.voice = descOf(pj, part);
    }
    d.voices = pj.parts[part].voices;
    d.priority = priorityOverride != 0xFF ? priorityOverride : pj.parts[part].priority;
    d.sampler = smp;
    d.amp = (uint8_t)clampi(pj.fx.amp[part][0], 0, 4);
    return d;
}

// The shared effects as one more part of the ladder: what is in use is what some part sends to, or the compressor being on. It has the
// lowest priority, so (among equals the later part going first) the effects are given up before any part's own options.
uint8_t fxMaskOf(const FxSettings& fx, uint32_t idleParts) {
    uint8_t m = 0;
    for (int p = 0; p < kParts; ++p) {
        if ((idleParts >> p) & 1) continue;
        if (fx.send[p][0] > 0) m |= cost::kFxDelay;
        if (fx.send[p][1] > 0) m |= cost::kFxReverb;
        if (fx.send[p][2] > 0) m |= cost::kFxChorus;
    }
    if (fx.comp[0] != 0) m |= cost::kFxComp;
    return m;
}

cost::PartDesc fxDesc(const Project& pj) {
    cost::PartDesc d;
    d.fxBus = true;
    d.voices = 0;
    d.priority = 0;
    d.fx = fxMaskOf(pj.fx, pj.partsIdle);
    return d;
}

float partCost(const Project& pj, int part) { return cost::partCost(partDesc(pj, part), kFwCost); }

int fitVoicesToBudget(Project& pj, int part) {
    Part& p = pj.parts[part];
    while (p.voices > 1 && partCost(pj, part) + fxCostOf(pj) > (float)kVoiceBudget) --p.voices;
    return p.voices;
}

float fxCostOf(const Project& pj) { return cost::partCost(fxDesc(pj), kFwCost); }

float voiceTotal(const Project& pj) {
    float t = fxCostOf(pj);
    for (int i = 0; i < kParts; ++i) if (!isIdlePart(pj, i)) t += partCost(pj, i);
    return t;
}

void buildLadder(const Project& pj, Ladder& out) {
    cost::PartDesc parts[kParts + 1];
    for (int i = 0; i < kParts; ++i) {
        parts[i] = partDesc(pj, i);
        if (isIdlePart(pj, i)) { parts[i].voices = 1; parts[i].sampler = true; parts[i].amp = 0; }   // nothing to give up on it
    }
    parts[kParts] = fxDesc(pj);   // the shared effects: the ladder's last "part"
    out.count = cost::buildLadder(parts, kParts + 1, kFwCost, out.steps, cost::kMaxSteps);
}

cost::PartDesc degradedPart(const Project& pj, int part, const Ladder& l, int level) {
    return cost::degraded(part >= kParts ? fxDesc(pj) : partDesc(pj, part), part, l.steps, l.count, level);
}

float effTotal(const Project& pj, const Ladder& l, int level) {
    float t = 0;
    for (int i = 0; i <= kParts; ++i) if (i == kParts || !isIdlePart(pj, i)) t += cost::partCost(degradedPart(pj, i, l, level), kFwCost);
    return t;
}

void describeStep(Text& out, const cost::Step& s) {
    out.add(s.part >= kParts ? "FX" : kPartName[s.part]).add(": ").add(kActionNames[(int)s.action]).add(" (-").fixed(s.saved, 2).ch(')');
}

bool menuHeavy(int menu) { return cost::isHeavy(modelFilter(menu)); }

void costBadge(Str<16>& out, const Project& pj, int part, int page, int param) {
    if (isSampler(pj, part)) return;
    const cost::VoiceDesc d = descOf(pj, part);
    cost::VoiceDesc t = d;
    const int key = page * 8 + param;
    switch (key) {
        case PG_SUB * 8 + 0: t.sub = false; break;
        case PG_SUB * 8 + 2: t.noise = false; break;
        case PG_FLT1 * 8 + 6: t.hpf = false; break;
        case PG_GRIT * 8 + 6: t.clip = false; break;
        case PG_SUB * 8 + 3: t.ring = 0; break;
        case PG_SUB * 8 + 4: t.slot[0].unison = 1; break;
        case PG_SUB * 8 + 7: t.width = 0; break;
        case PG_OSC2 * 8 + 7: t.sync = 0; break;
        case PG_OSC2 * 8 + 6: t.slot[1].audible = false; break;
        case PG_FLT1 * 8 + 3: t.drive = 0; break;
        case PG_FLT2 * 8 + 5: t.f2On = false; break;
#if DBOY_COST_LEAN
        case PG_GRIT * 8 + 7: t.companding = 0; break;   // the lean voice's converter is one cost, and MIX is the knob that takes it out
        case PG_FLT1 * 8 + 0: t.f1 = Filter::LowPass; break;
        case PG_FLT2 * 8 + 0: t.f2 = Filter::LowPass; break;
#else
        case PG_GRIT * 8 + 1: t.companding = 0; break;
        case PG_FLT1 * 8 + 0: t.f1 = cost::lightFallback(t.f1); t.drive = d.drive; break;
        case PG_FLT2 * 8 + 0: t.f2 = cost::lightFallback(t.f2); break;
#endif
        default: return;
    }
    const float dl = cost::voiceCost(d, kFwCost) - cost::voiceCost(t, kFwCost);
    if (page == PG_FLT2 && param == 0 && !d.f2On) return;
    if (dl > 0.004f) {
        Text s;
        s.fixed(dl, 2);
        // "+.27": the leading zero dropped, as the mockup's .replace(/^0/, "")
        out.ch('+').add(s.c_str()[0] == '0' ? s.c_str() + 1 : s.c_str());
    }
}

bool enforceHeavy(Project& pj, int part, int page, int param, Text& toast) {
    int16_t(&pg)[kSynthPages][kParamsPerPage] = pj.parts[part].pages;
    const cost::VoiceDesc d = descOf(pj, part);
    if (cost::heavyRuleOk(d)) return false;
    toast.clear();
    if (page == PG_FLT1 || !(page == PG_FLT2 && param == 0)) {
        // FILTER 1 chosen (or FILTER 2 switched on): FILTER 2 falls back
        pg[PG_FLT2][0] = (int16_t)menuOf(cost::lightFallback(d.f2));
        toast.add("ONE HEAVY FILTER PER LANE: FILTER 2 > ").add(kModels[pg[PG_FLT2][0]]);
    } else {
        pg[PG_FLT1][0] = (int16_t)menuOf(cost::lightFallback(d.f1));
        toast.add("ONE HEAVY FILTER PER LANE: FILTER 1 > ").add(kModels[pg[PG_FLT1][0]]);
    }
    return true;
}

int lockedValue(const Project& pj, int part, uint8_t key, int value, const Step& step, Text& toast) {
    if (key != lockKey(PG_FLT1, 0) && key != lockKey(PG_FLT2, 0) && key != lockKey(PG_FLT2, 5)) return value;
    Step t = step;
    if (!t.setLock(key, (int16_t)value)) return value;
    if (cost::heavyRuleOk(descOf(pj, part, &t))) return value;
    toast.clear();
    if (key == lockKey(PG_FLT2, 5)) { toast.add("ONE HEAVY FILTER PER LANE: LOCK STAYS OFF"); return 0; }
    const int fb = menuOf(cost::lightFallback(modelFilter(value)));
    toast.add("ONE HEAVY FILTER PER LANE: LOCK > ").add(kModels[fb]);
    return fb;
}

}  // namespace app
