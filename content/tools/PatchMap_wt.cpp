#include "audio/PatchMap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <nemesis/dsp/PulseOscillator.h>
#include <nemesis/dsp/SubOscillator.h>
#include <nemesis/dsp/WavetableBank.h>
#include <nemesis/dsp/WavetableKernels.h>
#include <nemesis/dsp/WavetableRecipe.h>

#include "app/CostBridge.h"

namespace audio {

using namespace nemesis::dsp;
using app::PG_OSC1; using app::PG_OSC2; using app::PG_SUB; using app::PG_FLT1; using app::PG_FLT2;
using app::PG_GRIT; using app::PG_AMP; using app::PG_MOD; using app::PG_GLOB;
using OscMode = CathodeVoiceStage::OscMode;
using FilterMode = CathodeVoiceStage::FilterMode;
using Src = CathodeModMatrix::Source;
using Dst = CathodeModMatrix::Dest;

namespace {

// CATHODE's basic shapes, in the order its own kernel morphs them: saw, square, triangle, sine (four frames, one per member)
constexpr WavetableRecipe kShapes{"dboy.shapes", "Basic Shapes", WavetableCategory::Basic, 4, WavetableKernel::ShapeMorph,
                                  kernel::ShapeMorph::make(kernel::ShapeSet::SawSquareTriSine), FrameOrdering::Morph};
// the WAVE knob (SIN, TRI, SAW, SQR) as a frame of that table; SIN is not read from it (it is the phase-distortion oscillator)
constexpr float kWavePos[4] = {1.0f, 2.0f / 3.0f, 0.0f, 1.0f / 3.0f};

inline float u01(int v) { return (float)v / 127.0f; }
inline float cutHz(int v) { return (float)app::cutHz(v); }
inline float envMs(int v) { return (float)app::envMs(v); }
// RESO 0..127 as the filters' Q: 0.7 (flat) .. ~16 (close to self-oscillation), more resolution low down
inline float resoQ(int v) { return 0.707f + powf(u01(v), 1.5f) * 15.3f; }

}  // namespace

void SharedTables::build() {
    float frames[4 * WavetableBank::kFrameLength];
    for (int f = 0; f < 4; ++f) WavetableKernels::render(kShapes, f, frames + f * WavetableBank::kFrameLength);
    shapes.build(frames, 4, WavetableBank::kFrameLength);
    PulseOscillator::buildTable(pulse);
    SubOscillator::buildTable(sub);
    if (const char* d = getenv("DBOY_WT_DIR")) loadSet(d);
}

ActionMask actionsAt(const cost::Step* ladder, int steps, int level, int part) {
    ActionMask m = 0;
    for (int i = 0; i < level && i < steps; ++i)
        if (ladder[i].part == part) m |= (ActionMask)(1u << (int)ladder[i].action);
    return m;
}

void makePatch(const app::Part& part, const app::Lock* locks, int nLocks, ActionMask degraded, Patch& out) {
    memcpy(out.p, part.pages, sizeof(out.p));
    for (int i = 0; i < nLocks; ++i) {
        const int pg = app::lockPage(locks[i].key);
        if (pg < app::kSynthPages && pg != PG_GLOB) out.p[pg][app::lockParam(locks[i].key)] = locks[i].value;
    }
    cost::Filter f1 = app::modelFilter(out.p[PG_FLT1][0]), f2 = app::modelFilter(out.p[PG_FLT2][0]);
    // the one-heavy-filter rule also for a locked note (the knobs keep it for the part; a lock may break it): filter 2 falls back
    if (cost::isHeavy(f1) && out.p[PG_FLT2][5] && cost::isHeavy(f2)) f2 = cost::lightFallback(f2);
    // what the CPU governor has bypassed on this part (cost/Degrade.h), the same features in the same way
    auto on = [&](cost::Action a) { return (degraded >> (int)a) & 1u; };
    if (on(cost::Action::WidthOff)) out.p[PG_SUB][7] = 0;
    if (on(cost::Action::UnisonOff)) out.p[PG_SUB][4] = 1;
    if (on(cost::Action::DriveOff)) out.p[PG_FLT1][3] = 0;
    if (on(cost::Action::XmodOff)) { out.p[PG_SUB][3] = 0; out.p[PG_OSC2][7] = 0; }
    if (on(cost::Action::CompandOff)) out.p[PG_GRIT][1] = 0;
    if (on(cost::Action::HeavyToLight)) {   // as cost::apply: a heavy model falls back, and with it the drive goes
        bool fell = false;
        if (cost::isHeavy(f1)) { f1 = cost::lightFallback(f1); fell = true; }
        if (out.p[PG_FLT2][5] && cost::isHeavy(f2)) { f2 = cost::lightFallback(f2); fell = true; }
        if (fell) out.p[PG_FLT1][3] = 0;
    }
    if (on(cost::Action::F2Off)) out.p[PG_FLT2][5] = 0;
    out.f1 = (uint8_t)f1;
    out.f2 = (uint8_t)f2;
}

// ---------------------------------------------------------------------------------------------------------------------------------

void SharedTables::loadSet(const char* d) {
    dir = d;
    FILE* f = fopen((dir + "/set.txt").c_str(), "r");
    if (!f) return;
    int k, cat;
    char fn[256];
    while (fscanf(f, "%d %d %255s", &k, &cat, fn) == 3 && k >= 0 && k < kExt) {
        extFile[k] = fn;
        extCatalogue[k] = cat;
        if (k + 1 > extCount) extCount = k + 1;
    }
    fclose(f);
}

// A .wt file (content/data/wavetables/FORMAT.md): 64-byte header, then frames x 2048 int16 little-endian.
const Wavetable* SharedTables::table(int slot) {
    if (slot < 0 || slot >= kExt || extFile[slot].empty()) return &shapes;
    if (ext[slot]) return ext[slot];
    FILE* f = fopen((dir + "/" + extFile[slot]).c_str(), "rb");
    if (!f) return &shapes;
    uint8_t h[64];
    if (fread(h, 1, 64, f) != 64 || memcmp(h, "DBWT", 4) != 0) { fclose(f); return &shapes; }
    const int frames = h[8] | (h[9] << 8), len = h[10] | (h[11] << 8);
    std::vector<int16_t> raw((size_t)frames * len);
    const size_t got = fread(raw.data(), 2, raw.size(), f);
    fclose(f);
    if (got != raw.size()) return &shapes;
    std::vector<float> fl(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) fl[i] = raw[i] / 32768.0f;
    Wavetable* t = new Wavetable;
    t->build(fl.data(), frames, len);
    ext[slot] = t;
    return t;
}

void CathodePart::init(double sampleRate, SharedTables& tables) {
    tables_ = &tables;
    for (int v = 0; v < kVoices; ++v) bound_[v][0] = bound_[v][1] = -1;
    engine_ = new CathodeVoiceEngine;   // SDRAM heap on the board
    engine_->prepare(sampleRate);
    engine_->setVoiceCount(kPoly);
    engine_->setStealFade(0.003f);
    engine_->setLaneSitesForTest(0u);   // no NEON on the M7: every voice scalar
    for (int v = 0; v < kVoices; ++v) {
        CathodeVoiceStage& t = engine_->voiceAt(v).outputStage().voice();
        for (int s = 0; s < 2; ++s) {
            t.setWavetable(s, tables.shapes);
            t.setPdType(s, PhaseDistortionOscillator::PdType::Saw);
        }
        t.setPulseWavetable(tables.pulse);
        t.setSubWavetable(tables.sub);
        t.setSubWave(SubOscillator::Wave::Sine);
        engine_->voiceAt(v).outputStage().amp().setDelayMs(0.0f);
        // the filter envelope (Env2): a fast attack and no sustain, its decay follows the AMP decay (set with it below)
        engine_->voiceAt(v).env2().setAttackMs(1.0f);
        engine_->voiceAt(v).env2().setSustain(0.0f);
        memset(applied_[v], 0x80, sizeof(applied_[v]));   // nothing applied yet: the first patch sets every parameter
        appliedF1_[v] = appliedF2_[v] = 0xFF;
        velAttack_[v] = false;
    }
    engine_->reset();
}

void CathodePart::setFilter1(int v, const Patch& P) {
    engine_->voiceAt(v).outputStage().voice().setFilterMode(static_cast<FilterMode>(P.f1));
}

// One parameter of the panel on voice v. Parameters that share a setter read their partners from P.
void CathodePart::set(int v, int key, const Patch& P) {
    CathodeModulatedVoice& mv = engine_->voiceAt(v);
    CathodeOutputStage& o = mv.outputStage();
    CathodeVoiceStage& t = o.voice();
    const auto& p = P.p;
    auto bindTable = [&](int s, int slot) {   // -1: the basic shapes (also undoes an earlier SD table on this voice)
        if (bound_[v][s] == slot) return;
        bound_[v][s] = (int8_t)slot;
        t.setWavetable(s, slot < 0 ? tables_->shapes : *tables_->table(slot));
    };
    auto oscWave = [&](int s, int pg) {
        const int w = p[pg][0];
        if (w >= 4) {   // an SD wavetable: PD SHAPE is its position
            bindTable(s, w - 4);
            t.setOscMode(s, OscMode::Wavetable);
            mv.setBaseOscPosition(s, u01(p[pg][1]));
        } else if (w == 0) {   // SIN: the phase-distortion oscillator; PD SHAPE and PD AMT bend it (0 = a clean sine)
            t.setOscMode(s, OscMode::PhaseDistortion);
            t.setPdShape(s, u01(p[pg][1]));
            mv.setBasePdAmount(s, u01(p[pg][2]));
        } else {        // TRI, SAW, SQR: a frame of the basic-shapes table
            bindTable(s, -1);
            t.setOscMode(s, OscMode::Wavetable);
            mv.setBaseOscPosition(s, kWavePos[w & 3]);
        }
    };
    auto fine = [&](int s, int pg) { t.setFine(s, (Wide)(p[pg][5] + p[PG_GLOB][3])); };
    auto detune = [&]() { const int d = p[PG_OSC1][7] > p[PG_SUB][5] ? p[PG_OSC1][7] : p[PG_SUB][5]; t.setUnisonDetune(0, u01(d)); };
    auto width = [&]() { const float w = p[PG_SUB][4] > 1 ? u01(p[PG_SUB][7]) : 0.0f; t.setUnisonWidth(0, w); t.setUnisonWidth(1, w); };
    auto envAmt = [&]() {
        // FILTER 1 ENV AMT and MOD ENV>CUT: one route, Env2 -> cutoff (full depth = 4 octaves). ENV AMT is scaled to the mockup's
        // sweep (the peak at cutoff x (1 + 6 x amount), up to ~2.8 octaves); ENV>CUT adds up to 2 octaves more.
        const float d = log2f(1.0f + 6.0f * u01(p[PG_FLT1][5])) / 4.0f + u01(p[PG_MOD][5]) * 0.5f;
        if (d > 0.0f) mv.setSlot(CathodeModMatrix::kEnvAmtSlot, Src::Env2, Dst::FiltCutoff, d > 1 ? 1 : d);
        else mv.clearSlot(CathodeModMatrix::kEnvAmtSlot);
    };
    auto lfo = [&]() {
        const bool tab = p[PG_OSC1][0] >= 4 || p[PG_OSC2][0] >= 4;   // with an SD table, "PD AMT" is the table position
        const Dst kDest[4] = {Dst::FiltCutoff, Dst::Pitch, Dst::Amp, tab ? Dst::OscPos : Dst::PdAmount};
        static const float kScale[4] = {0.25f, 0.3f, 0.5f, 1.0f};   // ~1 octave of cutoff, ~0.9 semitone, half the level, all of PD
        const int dest = p[PG_MOD][2] & 3;
        const float d = u01(p[PG_MOD][1]) * kScale[dest];
        if (d > 0.0f) mv.setSlot(0, Src::Lfo1, kDest[dest], d);
        else mv.clearSlot(0);
    };
    auto env2Times = [&]() {
        mv.env2().setDecayMs(envMs(p[PG_AMP][1]) * 0.8f + 20.0f);
        mv.env2().setReleaseMs(envMs(p[PG_AMP][3]));
    };
    switch (key) {
        // ---- OSC 1 / OSC 2 (slot 0 / 1)
        case PG_OSC1 * 8 + 0: case PG_OSC1 * 8 + 1: case PG_OSC1 * 8 + 2: oscWave(0, PG_OSC1); break;
        case PG_OSC2 * 8 + 0: case PG_OSC2 * 8 + 1: case PG_OSC2 * 8 + 2: oscWave(1, PG_OSC2); break;
        case PG_OSC1 * 8 + 3: t.setOctave(0, p[PG_OSC1][3]); break;
        case PG_OSC2 * 8 + 3: t.setOctave(1, p[PG_OSC2][3]); break;
        case PG_OSC1 * 8 + 4: t.setSemitones(0, p[PG_OSC1][4]); break;
        case PG_OSC2 * 8 + 4: t.setSemitones(1, p[PG_OSC2][4]); break;
        case PG_OSC1 * 8 + 5: fine(0, PG_OSC1); break;
        case PG_OSC2 * 8 + 5: fine(1, PG_OSC2); break;
        case PG_OSC1 * 8 + 6: t.setLevel(0, u01(p[PG_OSC1][6])); break;
        case PG_OSC2 * 8 + 6: t.setLevel(1, u01(p[PG_OSC2][6])); break;
        case PG_OSC1 * 8 + 7: detune(); break;                          // UNI DET (shared with SUB's: the larger wins)
        case PG_OSC2 * 8 + 7: mv.setBaseSyncAmount(u01(p[PG_OSC2][7])); break;   // SYNC: osc 2 hard-synced to osc 1
        // ---- SUB / UNISON
        case PG_SUB * 8 + 0: t.setSubLevel(u01(p[PG_SUB][0])); break;
        case PG_SUB * 8 + 1: t.setSubOctave(p[PG_SUB][1]); break;
        case PG_SUB * 8 + 2: mv.setBaseNoiseLevel(u01(p[PG_SUB][2])); break;
        case PG_SUB * 8 + 3: t.setXmodRing(u01(p[PG_SUB][3])); break;
        case PG_SUB * 8 + 4: t.setUnisonVoices(0, p[PG_SUB][4]); width(); break;
        case PG_SUB * 8 + 5: detune(); break;
        case PG_SUB * 8 + 6: t.setUnisonBlend(0, u01(p[PG_SUB][6])); break;
        case PG_SUB * 8 + 7: width(); break;
        // ---- FILTER 1
        case PG_FLT1 * 8 + 0: setFilter1(v, P); break;
        case PG_FLT1 * 8 + 1: case PG_FLT1 * 8 + 2: mv.setBaseCutoff(cutHz(p[PG_FLT1][1]), resoQ(p[PG_FLT1][2])); break;
        case PG_FLT1 * 8 + 3: t.setDrive(u01(p[PG_FLT1][3])); break;
        case PG_FLT1 * 8 + 4: {
            const float k = u01(p[PG_FLT1][4]);   // full = 0.8 octave per octave (KeyTrack source: 5 octaves = 1; cutoff: 4 octaves = 1)
            if (k > 0.0f) mv.setSlot(CathodeModMatrix::kKeyTrackSlot, Src::KeyTrack, Dst::FiltCutoff, k);
            else mv.clearSlot(CathodeModMatrix::kKeyTrackSlot);
            break;
        }
        case PG_FLT1 * 8 + 5: envAmt(); break;
        case PG_FLT1 * 8 + 6: t.setHpf(p[PG_FLT1][6] > 0 ? cutHz(p[PG_FLT1][6]) / 8.0f : 0.0f); break;
        // MIX F1/F2: CATHODE's balance is a mixer (0 = both at unity, +1 = filter 2 alone); the knob runs from both to filter 2 alone
        case PG_FLT1 * 8 + 7: t.setFilterBalance(u01(p[PG_FLT1][7])); break;
        // ---- FILTER 2 (KEYTRK and ENV AMT have no CATHODE counterpart for filter 2: docs/PATCH_MAP.md)
        case PG_FLT2 * 8 + 0:
            t.editFilter2([&](CathodeFilterBank& b) noexcept { b.setFilterMode(static_cast<FilterMode>(P.f2)); });
            mv.refreshF2();
            break;
        // F2's cutoff and Q are the modulated voice's own bases (it re-pushes them, with F1's cutoff modulation, whenever F1 moves): writing them straight to the
        // stage (t.setFilter2CutoffReso) was overwritten by that push with its defaults (1360 Hz, Q 1), so these two knobs did nothing until this was changed
        case PG_FLT2 * 8 + 1: mv.setF2Cutoff(cutHz(p[PG_FLT2][1])); break;
        case PG_FLT2 * 8 + 2: mv.setF2Reso(resoQ(p[PG_FLT2][2])); break;
        case PG_FLT2 * 8 + 3: t.setFilter2Drive(u01(p[PG_FLT2][3])); break;
        case PG_FLT2 * 8 + 4: t.setFilterRoute(p[PG_FLT2][4] ? CathodeVoiceStage::FilterRoute::Parallel : CathodeVoiceStage::FilterRoute::Series); break;
        case PG_FLT2 * 8 + 5: t.setFilter2On(p[PG_FLT2][5] != 0); break;
        // ---- GRIT (WOW has no CATHODE counterpart: the converter's DRIFT is the closest; docs/PATCH_MAP.md)
        case PG_GRIT * 8 + 0: mv.setBaseBitDepth((float)p[PG_GRIT][0]); break;
        case PG_GRIT * 8 + 1: t.setCompanding(u01(p[PG_GRIT][1])); break;
        case PG_GRIT * 8 + 2: mv.setBaseConverterRate((1.0f + u01(p[PG_GRIT][2]) * 43.1f) * 1000.0f); break;
        case PG_GRIT * 8 + 3: mv.setBaseDrift(u01(p[PG_GRIT][3])); break;
        case PG_GRIT * 8 + 4: mv.setBaseLoopError(u01(p[PG_GRIT][4])); break;
        case PG_GRIT * 8 + 6: o.setOutputDrive(u01(p[PG_GRIT][6])); break;
        case PG_GRIT * 8 + 7: t.setGritAmount(u01(p[PG_GRIT][7])); break;
        // ---- AMP
        case PG_AMP * 8 + 0: mv.amp().setAttackMs(envMs(p[PG_AMP][0])); break;
        case PG_AMP * 8 + 1: mv.amp().setDecayMs(envMs(p[PG_AMP][1])); env2Times(); break;
        case PG_AMP * 8 + 2: mv.amp().setSustain(u01(p[PG_AMP][2])); break;
        case PG_AMP * 8 + 3: mv.amp().setReleaseMs(envMs(p[PG_AMP][3])); env2Times(); break;
        case PG_AMP * 8 + 4: o.setVelSens(u01(p[PG_AMP][4])); break;
        case PG_AMP * 8 + 5: mv.setBaseLevel(u01(p[PG_AMP][5])); break;
        case PG_AMP * 8 + 6: mv.setBasePan((float)(p[PG_AMP][6] - 64) / 64.0f); break;
        // AMP 7 VEL>ATK is applied per note (applyVoice)
        // ---- MOD
        case PG_MOD * 8 + 0: mv.lfo1().setRateHz(0.2f + u01(p[PG_MOD][0]) * 12.0f); break;
        case PG_MOD * 8 + 1: case PG_MOD * 8 + 2: lfo(); break;
        case PG_MOD * 8 + 3: {
            static const Lfo::Shape kShape[4] = {Lfo::Shape::Sine, Lfo::Shape::Triangle, Lfo::Shape::SawUp, Lfo::Shape::Square};
            mv.lfo1().setShape(kShape[p[PG_MOD][3] & 3]);
            break;
        }
        case PG_MOD * 8 + 4: {
            const float d = u01(p[PG_MOD][4]);
            const bool tab = p[PG_OSC1][0] >= 4 || p[PG_OSC2][0] >= 4;
            if (d > 0.0f) mv.setSlot(1, Src::Env2, tab ? Dst::OscPos : Dst::PdAmount, d);
            else mv.clearSlot(1);
            break;
        }
        case PG_MOD * 8 + 5: envAmt(); break;
        case PG_MOD * 8 + 6: {
            const float d = u01(p[PG_MOD][6]) * 0.5f;   // the wheel opens the cutoff by up to 2 octaves
            if (d > 0.0f) mv.setSlot(2, Src::ModWheel, Dst::FiltCutoff, d);
            else mv.clearSlot(2);
            break;
        }
        case PG_MOD * 8 + 7: {
            const float d = u01(p[PG_MOD][7]) * 0.5f;
            if (d > 0.0f) mv.setSlot(3, Src::Aftertouch, Dst::FiltCutoff, d);
            else mv.clearSlot(3);
            break;
        }
        // ---- GLOBAL: TUNE moves both oscillators' fine tune; the rest are the part's, not a voice's (Instrument)
        case PG_GLOB * 8 + 3: fine(0, PG_OSC1); fine(1, PG_OSC2); break;
        default: break;
    }
}

void CathodePart::setBendRange(const Patch& base) {
    const float bend = (float)base.p[PG_GLOB][1];
    if (bend != bendRange_) { bendRange_ = bend; engine_->setBendRange(bend); }
}

void CathodePart::applyVoice(int v, const Patch& P, float vel01) {
    if (v < 0 || v >= kVoices) return;
    for (int pg = 0; pg < app::kSynthPages; ++pg)
        for (int i = 0; i < app::kParamsPerPage; ++i)
            if (applied_[v][pg][i] != P.p[pg][i]) {
                applied_[v][pg][i] = P.p[pg][i];
                set(v, pg * 8 + i, P);
            }
    if (appliedF1_[v] != P.f1) { appliedF1_[v] = P.f1; setFilter1(v, P); }
    if (appliedF2_[v] != P.f2) { appliedF2_[v] = P.f2; set(v, PG_FLT2 * 8 + 0, P); }
    // VEL>ATK: a harder hit attacks faster (the mockup: attack x (1 - amount x velocity x 0.9)); undone on the voice's next plain note
    const int va = P.p[PG_AMP][7];
    if (vel01 >= 0.0f && va > 0) {
        engine_->voiceAt(v).amp().setAttackMs(envMs(P.p[PG_AMP][0]) * (1.0f - u01(va) * vel01 * 0.9f));
        velAttack_[v] = true;
    } else if (velAttack_[v]) {
        engine_->voiceAt(v).amp().setAttackMs(envMs(P.p[PG_AMP][0]));
        velAttack_[v] = false;
    }
}

}  // namespace audio
