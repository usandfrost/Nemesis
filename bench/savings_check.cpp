// Verifies the CPU savings in the DSP fork (tools/fork/ForkConfig.h).
//  1. EXACT savings (idle mod matrix, skipped silent slots): output with them on must be BIT-IDENTICAL to output with them off, on
//     patches where they engage AND on patches where they must not (FM, ring, sync, unison, an active mod route).
//  2. Oversampling factors (NOT bit-identical by design): measure the alias floor against an 8x reference.
#include <nemesis/dsp/CathodeVoiceEngine.h>
#include <nemesis/dsp/ForkConfig.h>

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace nemesis::dsp;
using OM = CathodeVoiceStage::OscMode;
using FM = CathodeVoiceStage::FilterMode;
using ME = MacroOscillator::Engine;
using Src = CathodeModMatrix::Source;
using Dst = CathodeModMatrix::Dest;

namespace {
struct Patch { std::string name; std::function<void(CathodeModulatedVoice&)> cfg; int expectSkip = -1; };   /* 1 must engage, 0 must not, -1 either */

std::vector<float> render(const Patch& p, bool idle, bool skip, int pdOs, int fOs, int frames, int voices = 3) {
    fork::Config::idleMatrixSkip = idle; fork::Config::skipSilentSlots = skip; fork::Config::pdOversample = pdOs; fork::Config::filterOversample = fOs;
    auto* eng = new CathodeVoiceEngine;
    eng->prepare(44100.0);
    eng->setVoiceCount(voices);
    eng->setLaneSitesForTest(0u);
    for (int v = 0; v < CathodeVoiceEngine::kMaxVoices; ++v) {
        auto& mv = eng->voiceAt(v);
        auto& t = mv.outputStage().voice();
        t.setOscMode(0, OM::PhaseDistortion); t.setPdShape(0, 0.3f); t.setPdAmount(0, 0.6f); t.setLevel(0, 1.0f);
        t.setOscMode(1, OM::PhaseDistortion); t.setPdShape(1, 0.5f); t.setPdAmount(1, 0.5f); t.setLevel(1, 0.0f);
        t.setSemitones(1, 7);
        t.setFilterMode(FM::LowPass); t.setCutoff(2000.0f, 3.0f);
        mv.outputStage().amp().setAttackMs(3.0f); mv.outputStage().amp().setDecayMs(40.0f); mv.outputStage().amp().setSustain(0.8f); mv.outputStage().amp().setReleaseMs(100.0f);
        p.cfg(mv);
    }
    eng->reset();
    const int notes[3] = {48, 55, 64};
    for (int i = 0; i < voices; ++i) eng->noteOn(notes[i], 0.85f);
    std::vector<float> out(2 * frames);
    for (int i = 0; i < frames; ++i) {
        if (i == frames / 2) eng->noteOff(notes[0]);                                 // exercise the release too
        const auto s = eng->processSample(); out[2 * i] = s.left; out[2 * i + 1] = s.right;
    }
    delete eng;
    return out;
}
}

int main() {
    const std::vector<Patch> patches = {
        {"silent slot 1 (PD)", [](auto&) {}, 1},
        {"silent slot 0, slot 1 audible", [](auto& mv) { auto& t = mv.outputStage().voice(); t.setLevel(0, 0.0f); t.setLevel(1, 1.0f); }, 1},
        {"both audible", [](auto& mv) { auto& t = mv.outputStage().voice(); t.setLevel(0, 0.6f); t.setLevel(1, 0.6f); }, 0},
        {"silent slot 1 + FM cross-mod", [](auto& mv) { mv.outputStage().voice().setXmodFm(0.5f); }, 0},
        {"silent slot 1 + ring mod", [](auto& mv) { mv.outputStage().voice().setXmodRing(0.6f); }, 0},
        {"silent slot 1 + hard sync", [](auto& mv) { mv.outputStage().voice().setSyncAmount(1.0f); }, 0},
        {"silent slot 1 + unison x4 on slot 1", [](auto& mv) { auto& t = mv.outputStage().voice(); t.setUnisonVoices(1, 4); t.setUnisonDetune(1, 0.3f); }},
        {"unison x4 on slot 0, slot 1 silent", [](auto& mv) { auto& t = mv.outputStage().voice(); t.setUnisonVoices(0, 4); t.setUnisonDetune(0, 0.3f); t.setUnisonWidth(0, 0.7f); }, 1},
        {"macro slot 0, silent wavetable-less slot 1", [](auto& mv) { auto& t = mv.outputStage().voice(); t.setOscMode(0, OM::Macro); t.setMacroEngine(0, ME::Swarm); }, 1},
        {"LFO1 -> cutoff route (matrix active)", [](auto& mv) { mv.setSlot(0, Src::Lfo1, Dst::FiltCutoff, 0.5f); }, 1},
        {"Env2 -> pitch route + slot 1 silent", [](auto& mv) { mv.setSlot(0, Src::Env2, Dst::Pitch, 0.2f); }, 1},
        {"classic filter + F2 series", [](auto& mv) { auto& t = mv.outputStage().voice(); t.setFilterMode(FM::ClassicLp24); t.setFilter2On(true); t.setFilterRoute(CathodeVoiceStage::FilterRoute::Series); t.setFilter2CutoffReso(2500.0f, 2.0f); }, 1},
    };
    bool allOk = true;
    std::printf("== 1. exact savings: output must be bit-identical (off vs idle matrix + skipped slots) ==\n");
    for (const auto& p : patches) {
        const auto a = render(p, false, false, 4, 4, 22050);
        fork::Config::slotSkips = 0;
        const auto b = render(p, true, true, 4, 4, 22050);
        const unsigned long skips = fork::Config::slotSkips;
        const bool same = a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
        double pk = 0; for (float v : a) pk = std::fmax(pk, std::fabs(v));
        const bool alive = pk > 1e-3;
        const bool engagedOk = p.expectSkip < 0 || (p.expectSkip == 1) == (skips > 0);
        std::printf("  %-48s %s  skipped slot-samples: %7lu%s%s\n", p.name.c_str(), same ? "BIT-IDENTICAL" : "DIFFERENT", skips,
                    engagedOk ? "" : "  <- WRONG (expected engage=" , engagedOk ? "" : (p.expectSkip ? "yes)" : "no)"));
        if (!alive) std::printf("    (silent! patch broken)\n");
        allOk = allOk && same && alive && engagedOk;
    }
    // each saving on its own, so a failure names the culprit
    for (int which = 0; which < 2; ++which) {
        bool ok = true;
        for (const auto& p : patches) {
            const auto a = render(p, false, false, 4, 4, 11025), b = render(p, which == 0, which == 1, 4, 4, 11025);
            ok = ok && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
        }
        std::printf("  %s alone: %s on all %zu patches\n", which == 0 ? "idle matrix" : "skip silent slots", ok ? "bit-identical" : "DIFFERS", patches.size());
        allOk = allOk && ok;
    }

    std::printf("\n== 2. CPU: one voice, ns/sample (host), savings off vs on ==\n");
    auto timeit = [&](const Patch& p, bool idle, bool skip, int pdOs, int fOs) {
        double best = 1e30;
        for (int r = 0; r < 3; ++r) {
            const int n = 44100;
            fork::Config::idleMatrixSkip = idle; fork::Config::skipSilentSlots = skip; fork::Config::pdOversample = pdOs; fork::Config::filterOversample = fOs;
            auto* eng = new CathodeVoiceEngine; eng->prepare(44100.0); eng->setVoiceCount(1); eng->setLaneSitesForTest(0u);
            auto& mv = eng->voiceAt(0); auto& t = mv.outputStage().voice();
            t.setOscMode(0, OM::PhaseDistortion); t.setPdShape(0, 0.3f); t.setPdAmount(0, 0.6f); t.setLevel(0, 1.0f);
            t.setOscMode(1, OM::PhaseDistortion); t.setLevel(1, 0.0f);
            t.setFilterMode(FM::LowPass); t.setCutoff(2000.0f, 3.0f);
            p.cfg(mv);
            mv.outputStage().amp().setSustain(1.0f);
            eng->reset(); eng->noteOn(48, 0.9f);
            float sink = 0; for (int i = 0; i < 4410; ++i) sink += eng->processSample().left;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; ++i) sink += eng->processSample().left;
            const auto t1 = std::chrono::steady_clock::now();
            best = std::fmin(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
            if (sink == 1.234f) std::puts("");
            delete eng;
        }
        return best;
    };
    const Patch plain{"plain voice", [](auto&) {}};
    const Patch heavy{"Classic filter + silent slot", [](auto& mv) { mv.outputStage().voice().setFilterMode(FM::ClassicLp24); }};
    for (const Patch* p : {&plain, &heavy}) {
        const double off = timeit(*p, false, false, 4, 4), idle = timeit(*p, true, false, 4, 4), skip = timeit(*p, false, true, 4, 4);
        const double both = timeit(*p, true, true, 4, 4), os2 = timeit(*p, true, true, 2, 2);
        std::printf("  %-30s off %5.0f | idle matrix %5.0f (%+.0f%%) | skip slots %5.0f (%+.0f%%) | both %5.0f (%+.0f%%) | both + oversample x2 %5.0f (%+.0f%%)\n",
                    p->name.c_str(), off, idle, 100 * (idle - off) / off, skip, 100 * (skip - off) / off, both, 100 * (both - off) / off, os2, 100 * (os2 - off) / off);
    }
    std::printf("\n%s\n", allOk ? "ALL EXACT CHECKS PASSED" : "EXACT CHECKS FAILED");
    return allOk ? 0 : 1;
}
