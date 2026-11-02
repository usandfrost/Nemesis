// ref_render: renders the Drumboy firmware's CATHODE voice (part A) for one library preset, dry, on the host.
//
//   ref_render OUT_PREFIX PRESET_ID NOTE VEL HOLD_SECONDS TAIL_SECONDS [VOICES_OVERRIDE] [--chord N1,N2,..] [--fit] [--verify] [--via-instrument]
//   ref_render --list
//
// How the sound is made. The default path drives audio::CathodePart (PatchMap.cpp) exactly as audio::Instrument does for a live note
// (Instrument::syncParts / startCathode / endCathode / renderCathode), but WITHOUT the sequencer, the effects section, the master gain and
// the soft limiter:   out = engine.processSample() x (part.level/127 x GLOBAL level/127) x kCathodeHeadroom (2.0)
// which is what Instrument::renderCathode adds to the part's buffer before FxSection.
// The CPU governor (loadFn_) is never present, so the degrade ladder stays at level 0: no voice is cut, no bypass applied.
// --verify renders the same note through the real audio::Instrument (master 127, no effects) and prints the largest difference to the direct
// path after applying the master gain (0.7) and soft limiter to the direct one.
// --via-instrument writes the Instrument's output instead (post master 0.7 and limiter).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <nemesis/dsp/CathodeOscCore.h>
#include <nemesis/dsp/ForkConfig.h>

#include "PresetHost.h"
#include "app/CostBridge.h"
#include "app/Model.h"
#include "app/SampleLib.h"
#include "app/WaveLib.h"
#include "audio/Instrument.h"
#include "audio/PatchMap.h"
#include "audio/WaveStore.h"
#include "lean/LeanPart.h"

using namespace app;

namespace {

constexpr double kSr = 44100.0;
constexpr int kBlock = 32;
constexpr double kNoteOnAt = 0.05;       // seconds
constexpr float kCathodeHeadroom = 2.0f;  // mirror of Instrument.cpp (anonymous namespace there); --verify guards against drift
constexpr float kMasterScale = 0.7f;      // mirror of Instrument.cpp

inline float softLimit(float x) {  // mirror of Instrument.cpp
    const float a = x < 0 ? -x : x;
    if (a <= 0.8f) return x;
    const float y = 0.8f + (a - 0.8f) / (1.0f + (a - 0.8f) * 5.0f);
    return x < 0 ? -(y > 1 ? 1 : y) : (y > 1 ? 1 : y);
}

std::string jsonStr(const char* s) {
    std::string o = "\"";
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') { o += '\\'; o += *s; }
        else if ((unsigned char)*s < 32) o += ' ';
        else o += *s;
    }
    return o + "\"";
}

struct StdioWaves : audio::WaveFile {
    std::string dir;
    FILE* f = nullptr;
    bool open(int bank) override {
        char n[64];
        snprintf(n, sizeof n, "/wt%03d.wtp", bank);
        f = fopen((dir + n).c_str(), "rb");
        return f != nullptr;
    }
    bool read(void* dst, size_t bytes) override { return f && fread(dst, 1, bytes, f) == bytes; }
    void close() override { if (f) fclose(f); f = nullptr; }
};

void configureDsp() {
    // the firmware's DSP settings (main.cpp), as render_song.cpp has them
    nemesis::dsp::CathodeOscCore::setLazyMacro(true);
    nemesis::dsp::fork::Config::idleMatrixSkip = true;
    nemesis::dsp::fork::Config::skipSilentSlots = true;
    nemesis::dsp::fork::Config::filterOversample = 2;
    nemesis::dsp::fork::Config::pdOversample = 4;
    nemesis::dsp::fork::Config::driveOversample = 2;
    nemesis::dsp::fork::Config::classicOversample = 0;
    nemesis::dsp::fork::Config::stereoTwin = true;
    nemesis::dsp::fork::Config::lanes = false;
}

struct Ev { long at; bool on; int note; };   // a sample index (from the start of the render)

struct Result {
    std::vector<float> L, R;
    double activeNs = 0;
    long activeSamples = 0;
    std::vector<double> blockUs;   // us per sample of each full 32-sample segment that rendered
    int voicesUsed = 0;
};

// The banks the part plays: loaded into `tables` through the firmware's own WaveStore. `missing` lists banks whose file is not there.
struct Banks {
    StdioWaves files;
    std::vector<nemesis::dsp::Real> arena;
    std::vector<int16_t> staging;
    audio::WaveStore store;
    std::vector<int> missing;
    void load(const Project& pj, audio::SharedTables* tables) {
        const char* d = getenv("DBOY_WTP_DIR");
        files.dir = d ? d : DBOY_WTP_DIR;
        uint8_t bits[(kWaveBanks + 7) / 8];
        audio::neededBanks(pj, bits);
        WaveSet& ws = waveSet();
        for (int b = 0; b < kWaveBanks; ++b) ws.present[b] = false;
        bool any = false;
        for (int b = 0; b < kWaveBanks; ++b) {
            if (!(bits[b >> 3] & (1u << (b & 7)))) continue;
            any = true;
            char n[64];
            snprintf(n, sizeof n, "/wt%03d.wtp", b);
            FILE* t = fopen((files.dir + n).c_str(), "rb");
            if (t) { fclose(t); ws.present[b] = true; }
            else missing.push_back(b);
        }
        if (!any) return;
        const size_t slotN = audio::WaveStore::slotSamples();
        arena.assign(slotN * 2, 0);
        staging.assign(slotN, 0);
        store.init(arena.data(), 2, staging.data(), tables, &files);
        store.want(bits, 1000);
    }
};

// ---- the direct path: CathodePart driven like Instrument does
Result renderDirect(const Project& pj, const std::vector<int>& notes, int vel, double hold, double tail, std::vector<int>& missing) {
    const Part& part = pj.parts[0];
    audio::SharedTables* tables = new audio::SharedTables;
    tables->build();
    Banks* banks = new Banks;
    banks->load(pj, tables);
    missing = banks->missing;
    audio::CathodePart* cp = new audio::CathodePart;
    cp->init(kSr, *tables);
    nemesis::dsp::CathodeVoiceEngine& eng = cp->engine();
    // Instrument::syncParts
    eng.setVoiceCount(part.voices);
    audio::Patch base;
    audio::makePatch(part, nullptr, 0, 0, base);
    cp->invalidateTables();
    cp->setBendRange(base);
    for (int v = 0; v < audio::CathodePart::kVoices; ++v) cp->applyVoice(v, base, -1.0f);
    const int cap = part.voices;   // cost::degraded(...).voices at ladder level 0 (the governor is not present on the host)
    const float g = part.level / 127.0f * (part.pages[PG_GLOB][0] / 127.0f) * kCathodeHeadroom;

    const long onAt = (long)llround(kNoteOnAt * kSr), offAt = onAt + (long)llround(hold * kSr);
    long total = offAt + (long)llround(tail * kSr);
    total = (total + kBlock - 1) / kBlock * kBlock;
    std::vector<Ev> evs;
    for (int n : notes) { evs.push_back({onAt, true, n}); evs.push_back({offAt, false, n}); }

    struct Note { bool used = false; int midi = 0, voice = -1; uint64_t group = 0; uint32_t order = 0; };
    std::vector<Note> ns(notes.size());
    for (size_t k = 0; k < notes.size(); ++k) ns[k].midi = notes[k];
    uint32_t order = 0;
    const float vel01 = vel / 127.0f;

    Result res;
    res.L.assign((size_t)total, 0.f);
    res.R.assign((size_t)total, 0.f);

    auto endNote = [&](Note& n) {
        if (!n.used) return;
        n.used = false;
        if (n.voice >= 0) eng.noteOffVoice(n.voice, n.group, n.midi);
        n.voice = -1;
    };
    auto startNote = [&](Note& n) {   // Instrument::startCathode
        n.used = true; n.order = order++;
        for (;;) {
            int sounding = 0, oldest = -1;
            for (size_t k = 0; k < ns.size(); ++k) {
                const Note& o = ns[k];
                if (!o.used || o.voice < 0) continue;
                ++sounding;
                if (oldest < 0 || o.order < ns[(size_t)oldest].order) oldest = (int)k;
            }
            if (sounding < cap || oldest < 0) break;
            endNote(ns[(size_t)oldest]);
        }
        eng.noteOn(n.midi, vel01 > 1 ? 1 : vel01);
        const int v = eng.lastTriggeredVoice();
        if (v < 0) { n.used = false; return; }
        n.voice = v;
        n.group = eng.voiceGroupAt(v);
        for (int i = 0; i < audio::CathodePart::kVoices; ++i) {
            if (!eng.voiceHeld(i) || eng.voiceGroupAt(i) != n.group) continue;
            cp->applyVoice(i, base, vel01);
        }
    };
    auto renderSeg = [&](long from, long to) {   // Instrument::renderCathode
        if (to <= from) return;
        if (eng.activeVoiceCount() == 0) {
            bool fading = false;
            for (int v = 0; v < audio::CathodePart::kVoices && !fading; ++v) fading = eng.stealFading(v);
            if (!fading) return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (long i = from; i < to; ++i) {
            const auto s = eng.processSample();
            res.L[(size_t)i] += s.left * g;
            res.R[(size_t)i] += s.right * g;
        }
        const double ns_ = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        res.activeNs += ns_;
        res.activeSamples += to - from;
        if (to - from == kBlock) res.blockUs.push_back(ns_ / 1000.0 / kBlock);
    };

    for (long b0 = 0; b0 < total; b0 += kBlock) {
        std::vector<Ev> here;
        for (const Ev& e : evs) if (e.at >= b0 && e.at < b0 + kBlock) here.push_back(e);
        std::stable_sort(here.begin(), here.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
        long pos = b0;
        for (const Ev& e : here) {
            renderSeg(pos, e.at);
            pos = e.at;
            for (size_t k = 0; k < notes.size(); ++k) {
                if (notes[k] != e.note) continue;
                if (e.on) startNote(ns[k]); else endNote(ns[k]);
                break;
            }
        }
        renderSeg(pos, b0 + kBlock);
        res.voicesUsed = std::max(res.voicesUsed, eng.activeVoiceCount());
    }
    return res;
}

// ---- the lean voice (firmware/src/lean): the same note events, the same level chain
Result renderLean(const Project& pj, const std::vector<int>& notes, int vel, double hold, double tail, std::vector<int>& missing) {
    const Part& part = pj.parts[0];
    audio::SharedTables* tables = new audio::SharedTables;
    tables->build();
    Banks* banks = new Banks;
    banks->load(pj, tables);
    missing = banks->missing;
    audio::Patch base;
    audio::makePatch(part, nullptr, 0, 0, base);
    lean::Params prm;
    lean::makeParams(base, *tables, prm);
    lean::Part lp;
    lp.init((float)kSr, *tables);
    lp.setVoices(part.voices);
    lp.setBend(0.0f);
    const float g = part.level / 127.0f * (part.pages[PG_GLOB][0] / 127.0f) * kCathodeHeadroom;
    const long onAt = (long)llround(kNoteOnAt * kSr), offAt = onAt + (long)llround(hold * kSr);
    long total = offAt + (long)llround(tail * kSr);
    total = (total + kBlock - 1) / kBlock * kBlock;
    const float vel01 = vel / 127.0f;
    Result res;
    res.L.assign((size_t)total, 0.f);
    res.R.assign((size_t)total, 0.f);
    std::vector<float> l(kBlock), r(kBlock);
    auto renderSeg = [&](long from, long to) {
        if (to <= from) return;
        if (lp.activeVoices() == 0) return;
        std::fill(l.begin(), l.end(), 0.f);
        std::fill(r.begin(), r.end(), 0.f);
        const auto t0 = std::chrono::steady_clock::now();
        lp.render(l.data(), r.data(), (int)(to - from));
        const double ns_ = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        res.activeNs += ns_;
        res.activeSamples += to - from;
        if (to - from == kBlock) res.blockUs.push_back(ns_ / 1000.0 / kBlock);
        for (long i = from; i < to; ++i) { res.L[(size_t)i] += l[(size_t)(i - from)] * g; res.R[(size_t)i] += r[(size_t)(i - from)] * g; }
    };
    for (long b0 = 0; b0 < total; b0 += kBlock) {
        long pos = b0;
        const bool on = onAt >= b0 && onAt < b0 + kBlock, off = offAt >= b0 && offAt < b0 + kBlock;
        // note-on first when it comes first in the block, as in renderDirect
        if (on) { renderSeg(pos, onAt); pos = onAt; for (int n : notes) lp.noteOn(n, vel01, prm); }
        if (off) { renderSeg(pos, offAt); pos = offAt; for (int n : notes) lp.noteOff(n); }
        renderSeg(pos, b0 + kBlock);
        res.voicesUsed = std::max(res.voicesUsed, lp.activeVoices());
    }
    return res;
}

// ---- the real Instrument: master 127 (x0.7), no effects sends
Result renderInstrument(const Project& src, const std::vector<int>& notes, int vel, double hold, double tail, bool lean = false) {
    static Project pj;
    pj = src;
    pj.master = 127;
    for (Pattern& p : pj.patterns) for (Lane& l : p.lanes) l.mute = 1;
    static Transport tp;
    static SampleLib lib;
    static std::vector<int16_t> store(kFactoryFrames);
    buildFactorySamples(lib, store.data(), store.size());
    audio::Instrument* inst = new audio::Instrument;
    inst->init(kSr, pj, tp, lib);
    inst->setLean(lean);
    Banks* banks = new Banks;
    banks->load(pj, &inst->sharedTables());

    const long onAt = (long)llround(kNoteOnAt * kSr), offAt = onAt + (long)llround(hold * kSr);
    long total = offAt + (long)llround(tail * kSr);
    total = (total + kBlock - 1) / kBlock * kBlock;
    Result res;
    std::vector<int32_t> handle(notes.size(), -1);
    float l[kBlock], r[kBlock];
    for (long b0 = 0; b0 < total; b0 += kBlock) {
        inst->beginBlock();
        if (onAt >= b0 && onAt < b0 + kBlock)
            for (size_t k = 0; k < notes.size(); ++k) {
                NoteStart s{};
                s.part = 0; s.pitch = (int8_t)notes[k]; s.sampler = 0; s.vel01 = vel / 127.0f;
                handle[k] = inst->noteOn(s, (int)(onAt - b0));
            }
        if (offAt >= b0 && offAt < b0 + kBlock)
            for (size_t k = 0; k < notes.size(); ++k) inst->noteOff(0, handle[k], (int)(offAt - b0));
        inst->render(l, r, kBlock);
        res.L.insert(res.L.end(), l, l + kBlock);
        res.R.insert(res.R.end(), r, r + kBlock);
        res.voicesUsed = std::max(res.voicesUsed, inst->activeVoices(0));
    }
    return res;
}

int listLibrary() {
    shippedPresets();
    for (int k = 0; k < presetCount(); ++k) {
        const int id = presetIdAt(k);
        int b[2];
        presetBanks(id, b);
        std::string bs;
        for (int s = 0; s < 2; ++s) if (b[s] >= 0) bs += (bs.empty() ? "" : ",") + std::to_string(b[s]);
        if (bs.empty()) bs = "-";
        const int c = presetCat(id);
        printf("%d\t%s\t%d\t%s\t%s\n", id, presetName(id), presetVoices(id), c >= 0 && c < kPresetCatCount ? kPresetCats[c] : "?", bs.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> pos;
    std::string chord;
    bool verify = false, viaInst = false, fit = false, list = false, useLean = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--list") list = true;
        else if (a == "--verify") verify = true;
        else if (a == "--via-instrument") viaInst = true;
        else if (a == "--fit") fit = true;
        else if (a == "--lean") useLean = true;
        else if (a == "--chord" && i + 1 < argc) chord = argv[++i];
        else pos.push_back(a);
    }
    configureDsp();
    if (list) return listLibrary();
    if (pos.size() < 6) {
        fprintf(stderr,
                "usage: ref_render OUT_PREFIX PRESET_ID NOTE VEL HOLD_SECONDS TAIL_SECONDS [VOICES_OVERRIDE] [--chord N1,N2,..] [--fit] [--verify] [--via-instrument]\n"
                "       ref_render --list     (id, name, voices, category, banks; tab separated)\n");
        return 2;
    }
    shippedPresets();
    const std::string prefix = pos[0];
    const int id = atoi(pos[1].c_str());
    int vel = atoi(pos[3].c_str());
    const double hold = atof(pos[4].c_str()), tail = atof(pos[5].c_str());
    const int voicesOverride = pos.size() > 6 ? atoi(pos[6].c_str()) : 0;
    std::vector<int> notes;
    if (!chord.empty()) {
        size_t a = 0;
        while (a <= chord.size()) {
            size_t b = chord.find(',', a);
            if (b == std::string::npos) b = chord.size();
            if (b > a) notes.push_back(atoi(chord.substr(a, b - a).c_str()));
            a = b + 1;
        }
    } else notes.push_back(atoi(pos[2].c_str()));
    if (notes.empty() || vel < 1 || vel > 127) { fprintf(stderr, "bad notes or velocity\n"); return 2; }
    if (!presetExists(id) || !presetLoad(id)) { fprintf(stderr, "preset %d is not in the library or cannot be read\n", id); return 1; }

    // the project, with the preset on part A the way Ui.cpp loads it (pages, then the preset's voices)
    static Project pj;
    makeDefaultProject(pj);
    Part& part = pj.parts[0];
    part.type = PT_CATHODE;
    applyPresetIndex(part.pages, id);
    part.preset = (int16_t)id;
    const int presetV = presetVoices(id);
    if (presetV > 0) part.voices = (uint8_t)presetV;
    // what the UI's fitVoicesToBudget leaves with the preset's own effects loaded (reported; used only with --fit)
    int fitV = part.voices;
    {
        static Project t;
        t = pj;
        applyPresetFx(t.fx, 0, id);
        fitV = fitVoicesToBudget(t, 0);
    }
    if (fit) part.voices = (uint8_t)fitV;
    if (voicesOverride > 0) part.voices = (uint8_t)std::min(voicesOverride, (int)audio::CathodePart::kPoly);

    std::vector<int> missing;
    Result res = viaInst ? renderInstrument(pj, notes, vel, hold, tail, useLean) : (useLean ? renderLean(pj, notes, vel, hold, tail, missing) : renderDirect(pj, notes, vel, hold, tail, missing));

    if (verify) {
        Result ins = viaInst ? res : renderInstrument(pj, notes, vel, hold, tail);
        Result dir = viaInst ? renderDirect(pj, notes, vel, hold, tail, missing) : res;
        double maxd = 0, maxv = 0;
        for (size_t i = 0; i < dir.L.size() && i < ins.L.size(); ++i) {
            maxd = std::max(maxd, (double)fabsf(softLimit(dir.L[i] * kMasterScale) - ins.L[i]));
            maxd = std::max(maxd, (double)fabsf(softLimit(dir.R[i] * kMasterScale) - ins.R[i]));
            maxv = std::max(maxv, (double)fabsf(ins.L[i]));
        }
        printf("{\"verify\": true, \"preset\": %d, \"samples_direct\": %zu, \"samples_instrument\": %zu, \"max_abs_diff\": %.3g, \"instrument_peak\": %.4f, \"voices_direct\": %d, \"voices_instrument\": %d}\n",
               id, dir.L.size(), ins.L.size(), maxd, maxv, dir.voicesUsed, ins.voicesUsed);
        return maxd < 1e-6 ? 0 : 1;
    }

    const size_t n = res.L.size();
    std::vector<float> inter(n * 2);
    double peak = 0, sum = 0, sumHold = 0;
    const size_t h0 = (size_t)llround(kNoteOnAt * kSr), h1 = h0 + (size_t)llround(hold * kSr);
    size_t nh = 0;
    bool finite = true;
    for (size_t i = 0; i < n; ++i) {
        inter[2 * i] = res.L[i];
        inter[2 * i + 1] = res.R[i];
        if (!std::isfinite(res.L[i]) || !std::isfinite(res.R[i])) finite = false;
        peak = std::max(peak, (double)std::max(fabsf(res.L[i]), fabsf(res.R[i])));
        const double e = (double)res.L[i] * res.L[i] + (double)res.R[i] * res.R[i];
        sum += e;
        if (i >= h0 && i < h1) { sumHold += e; ++nh; }
    }
    const std::string path = prefix + ".f32";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return 1; }
    fwrite(inter.data(), sizeof(float), inter.size(), f);
    fclose(f);

    std::sort(res.blockUs.begin(), res.blockUs.end());
    const double usMean = res.activeSamples ? res.activeNs / 1000.0 / (double)res.activeSamples : 0;
    const double usP50 = res.blockUs.empty() ? usMean : res.blockUs[res.blockUs.size() / 2];
    const double usP95 = res.blockUs.empty() ? usMean : res.blockUs[(size_t)((double)res.blockUs.size() * 0.95)];
    const double usMax = res.blockUs.empty() ? usMean : res.blockUs.back();
    const int c = presetCat(id);
    printf("{\"preset\": %d, \"name\": %s, \"category\": %s, \"note\": ", id, jsonStr(presetName(id)).c_str(), jsonStr(c >= 0 && c < kPresetCatCount ? kPresetCats[c] : "?").c_str());
    if (notes.size() == 1) printf("%d", notes[0]);
    else { printf("["); for (size_t k = 0; k < notes.size(); ++k) printf("%s%d", k ? "," : "", notes[k]); printf("]"); }
    printf(", \"vel\": %d, \"hold\": %.3f, \"tail\": %.3f, \"frames\": %zu, \"peak\": %.6f, \"rms\": %.6f, \"rms_hold\": %.6f, "
           "\"host_us_per_sample\": %.4f, \"host_us_per_sample_p50\": %.4f, \"host_us_per_sample_p95\": %.4f, \"host_us_per_sample_max\": %.4f, "
           "\"active_samples\": %ld, \"voices_used\": %d, \"voices_cfg\": %d, \"voices_preset\": %d, \"voices_fit\": %d, \"finite\": %s, \"missing_banks\": [",
           vel, hold, tail, n, peak, sqrt(sum / (2.0 * (double)n)), nh ? sqrt(sumHold / (2.0 * (double)nh)) : 0.0, usMean, usP50, usP95, usMax,
           res.activeSamples, res.voicesUsed, (int)part.voices, presetV, fitV, finite ? "true" : "false");
    for (size_t k = 0; k < missing.size(); ++k) printf("%s%d", k ? "," : "", missing[k]);
    // the recipe in a few words (for grouping the host cost): oscillator waves (0..3 basic, 4+ = bank), unison voices, the filter models, drive, grit
    static const char* const kFilt[] = {"LowPass", "BandPass", "HighPass", "Notch", "Formant", "Comb", "Ladder24", "Ladder12", "ScreamLp", "ScreamHp", "Acid", "SovietLp", "SovietBp",
                                        "SovietHp", "ClassicLp24", "ClassicLp12", "ClassicBp", "ClassicHp", "ReedLp", "ReedBp", "ReedHp", "StateLp", "StateBp", "StateHp",
                                        "StateNotch", "StateAp", "StateMorph", "ReedMorph"};
    audio::Patch pp;
    audio::makePatch(part, nullptr, 0, 0, pp);
    const bool f2on = pp.p[PG_FLT2][5] != 0;
    printf("], \"recipe\": {\"wave1\": %d, \"wave2\": %d, \"level2\": %d, \"sub\": %d, \"unison\": %d, \"f1\": \"%s\", \"f2\": \"%s\", \"f1_heavy\": %s, \"f2_heavy\": %s, \"drive\": %d, "
           "\"grit\": %d, \"noise\": %d, \"sync\": %d, \"ring\": %d}",
           pp.p[PG_OSC1][0], pp.p[PG_OSC2][0], pp.p[PG_OSC2][6], pp.p[PG_SUB][0], pp.p[PG_SUB][4], pp.f1 < 28 ? kFilt[pp.f1] : "?", f2on && pp.f2 < 28 ? kFilt[pp.f2] : "-",
           cost::isHeavy((cost::Filter)pp.f1) ? "true" : "false", f2on && cost::isHeavy((cost::Filter)pp.f2) ? "true" : "false", pp.p[PG_FLT1][3], pp.p[PG_GRIT][7], pp.p[PG_SUB][2],
           pp.p[PG_OSC2][7], pp.p[PG_SUB][3]);
    printf(", \"path\": %s}\n", jsonStr(path.c_str()).c_str());
    return finite ? 0 : 1;
}
