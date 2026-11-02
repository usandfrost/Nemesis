// render_presets: the converted presets through the Drumboy's own instrument (fork DSP + PatchMap + Instrument), plus the checks.
//   render_presets check                       sanitiser + cost-budget check of every converted preset (exit 1 on a failure)
//   render_presets render <outdir> [shard n]   one phrase per preset -> <outdir>/NNN.wav (mono 16-bit, 44.1 kHz, loudness-normalised) and
//                                              a JSON line per preset on stdout: level, clipping, silence and spectral figures of the RAW render
// Wavetables: DBOY_WT_DIR=content/data/wavetables (set.txt + .wt files). Presets come from content/data/Presets_generated.inc.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include <nemesis/dsp/CathodeOscCore.h>
#include <nemesis/dsp/ForkConfig.h>

#include "app/CostBridge.h"
#include "app/Model.h"
#include "app/ProjectFile.h"
#include "app/SampleLib.h"
#include "audio/Instrument.h"

namespace app {
#include "Presets_generated.inc"
}
using namespace app;

#include "phrases.h"

namespace {
using phr::kSr; using phr::Phrase; using phr::phraseFor;

void setupDsp() {   // the firmware's DSP settings (main.cpp), as render_song.cpp
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

void loadPages(Project& pj, const GenPreset& g, bool withTables) {
    Part& p = pj.parts[0];
    for (int i = 0; i < 72; ++i) p.pages[i / 8][i % 8] = g.pages[i];
    if (withTables)
        for (int o = 0; o < 2; ++o)
            if (g.table[o] >= 0) p.pages[PG_OSC1 + o][0] = (int16_t)(4 + g.table[o]);   // the shadow PatchMap reads WAVE >= 4 as a table slot
    p.voices = g.voices;
}

int check() {
    int bad = 0, heavy = 0, over = 0;
    double worst = 0;
    static Project pj;
    for (int i = 0; i < kGenPresetCount; ++i) {
        const GenPreset& g = kGenPresets[i];
        makeDefaultProject(pj);
        loadPages(pj, g, false);
        static int16_t before[kSynthPages][kParamsPerPage];
        memcpy(before, pj.parts[0].pages, sizeof(before));
        sanitize(pj);
        bool changed = memcmp(before, pj.parts[0].pages, sizeof(before)) != 0;
        bool range = false;
        for (int pg = 0; pg < kSynthPages; ++pg)
            for (int k = 0; k < kParamsPerPage; ++k) {
                const ParamDef& d = pageDef(pg).p[k];
                if (g.pages[pg * 8 + k] < d.min || g.pages[pg * 8 + k] > d.max) range = true;
            }
        const cost::VoiceDesc d = descOf(pj, 0);
        const float c = cost::voiceCost(d, kFwCost);
        const bool ok = cost::heavyRuleOk(d);
        const bool budget = c * g.voices <= kVoiceBudget + 1e-4f;
        if (c > worst) worst = c;
        if (changed || range || !ok || !budget || g.voices < 1 || g.voices > 5 || g.cat >= kGenCatCount) {
            ++bad;
            printf("FAIL %d %s: sanitiser-changed %d out-of-range %d heavy-rule %d cost %.2f x %d voices\n", i, g.name, changed, range, ok, c, g.voices);
        }
        heavy += ok ? 0 : 1;
        over += budget ? 0 : 1;
    }
    printf("check: %d presets, %d failures (sanitiser / range / heavy-filter rule / voice budget); worst per-voice cost %.2f; budget %d voices\n", kGenPresetCount, bad,
           worst, kVoiceBudget);
    return bad ? 1 : 0;
}

int render(const char* outdir, int shard, int nshards) {
    setupDsp();
    static SampleLib lib;
    static std::vector<int16_t> store(kFactoryFrames);
    buildFactorySamples(lib, store.data(), store.size());
    for (int i = shard; i < kGenPresetCount; i += nshards) {
        const GenPreset& g = kGenPresets[i];
        static Project pj;
        static Transport tp;
        makeDefaultProject(pj);
        tp = Transport();
        loadPages(pj, g, true);
        pj.parts[0].level = 100;
        pj.master = 100;
        const Phrase ph = phraseFor(kGenCats[g.cat]);
        audio::Instrument* inst = new audio::Instrument;   // fresh per preset: no state, no LFO phase carried over (leaked: a shard renders ~10)
        inst->init(kSr, pj, tp, lib);
        struct Ev { int at; bool on; size_t n; };
        std::vector<Ev> evs;
        for (size_t k = 0; k < ph.notes.size(); ++k) {
            evs.push_back({(int)(ph.notes[k].t * kSr), true, k});
            evs.push_back({(int)((ph.notes[k].t + ph.notes[k].d) * kSr), false, k});
        }
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
        std::vector<int32_t> handle(ph.notes.size(), -1);
        const int block = 32, total = (int)(ph.len * kSr) / block * block;
        std::vector<float> mono(total);
        size_t e = 0;
        bool finite = true;
        double peakL = 0;
        float l[block], r[block];
        for (int b0 = 0; b0 < total; b0 += block) {
            inst->beginBlock();
            while (e < evs.size() && evs[e].at < b0 + block) {
                const Ev& ev = evs[e++];
                const int off = ev.at > b0 ? ev.at - b0 : 0;
                if (ev.on) {
                    NoteStart ns{};
                    ns.part = 0; ns.pitch = (int8_t)ph.notes[ev.n].n; ns.vel01 = ph.notes[ev.n].v;
                    handle[ev.n] = inst->noteOn(ns, off);
                } else {
                    inst->noteOff(0, handle[ev.n], off);
                }
            }
            inst->render(l, r, block);
            for (int k = 0; k < block; ++k) {
                if (!std::isfinite(l[k]) || !std::isfinite(r[k])) { finite = false; l[k] = r[k] = 0; }
                mono[b0 + k] = 0.5f * (l[k] + r[k]);
                peakL = fmax(peakL, fmax(fabs(l[k]), fabs(r[k])));
            }
        }
        phr::Figures fg = phr::analyse(mono, finite, i, g.name, kGenCats[g.cat], ph.name);
        char path[512];
        snprintf(path, sizeof path, "%s/%03d.wav", outdir, i);
        phr::writeWav(path, fg.norm);
        printf("%s\n", fg.json.c_str());
        fflush(stdout);
    }
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "check")) return check();
    if (argc >= 3 && !strcmp(argv[1], "render")) return render(argv[2], argc > 3 ? atoi(argv[3]) : 0, argc > 4 ? atoi(argv[4]) : 1);
    fprintf(stderr, "usage: render_presets check | render <outdir> [shard n]\n");
    return 2;
}
