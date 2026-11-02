// Does climbing the degrade ladder really make the REAL engine cheaper, by about what the model says? Builds a deliberately heavy 5-voice part,
// builds its ladder, applies each level with the same code the firmware uses (audio::applyVoiceDesc), and times the engine.
#include <nemesis/dsp/CathodeVoiceEngine.h>
#include <nemesis/dsp/ForkConfig.h>

#include <chrono>
#include <cmath>
#include <cstdio>

#include "audio/ApplyVoiceDesc.h"
#include "cost/Degrade.h"

using namespace nemesis::dsp;
using OM = CathodeVoiceStage::OscMode;
using FM = CathodeVoiceStage::FilterMode;

static double timeEngine(const cost::PartDesc& p) {
    double best = 1e30;
    for (int rep = 0; rep < 4; ++rep) {
        auto* eng = new CathodeVoiceEngine; eng->prepare(44100.0); eng->setVoiceCount(5); eng->setLaneSitesForTest(0u);
        for (int v = 0; v < CathodeVoiceEngine::kMaxVoices; ++v) {
            auto& st = eng->voiceAt(v).outputStage(); auto& t = st.voice();
            t.setOscMode(0, OM::PhaseDistortion); t.setPdShape(0, 0.3f); t.setPdAmount(0, 0.6f); t.setLevel(0, 0.7f);
            t.setOscMode(1, OM::PhaseDistortion); t.setPdShape(1, 0.5f); t.setPdAmount(1, 0.5f); t.setSemitones(1, 7); t.setLevel(1, p.voice.slot[1].audible ? 0.7f : 0.0f);
            t.setUnisonDetune(0, 0.3f); t.setUnisonDetune(1, 0.3f);
            t.setCutoff(2000.0f, 3.0f); t.setFilter2CutoffReso(2500.0f, 2.0f); t.setFilterRoute(CathodeVoiceStage::FilterRoute::Series);
            st.amp().setAttackMs(2.0f); st.amp().setSustain(1.0f);
        }
        audio::applyVoiceDesc(*eng, p.voice);
        eng->setVoiceCount(p.voices);
        eng->reset();
        for (int v = 0; v < p.voices; ++v) eng->noteOn(40 + 5 * v, 0.9f);
        float sink = 0; for (int i = 0; i < 4410; ++i) sink += eng->processSample().left;
        const int n = 22050;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) sink += eng->processSample().left;
        const auto t1 = std::chrono::steady_clock::now();
        best = std::fmin(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
        if (sink == 1.2345f) std::puts("");
        delete eng;
    }
    return best;
}

static const char* name(cost::Action a) {
    static const char* n[] = {"release oldest voice", "width off", "unison off", "drive off", "cross-mod off", "fold off", "companding off", "heavy filter -> light", "filter 2 off"};
    return n[static_cast<int>(a)];
}

int main() {
    fork::Config::idleMatrixSkip = true; fork::Config::skipSilentSlots = true; fork::Config::filterOversample = 2; fork::Config::pdOversample = 4;
    fork::Config::driveOversample = 2; fork::Config::lanes = false;   // the firmware configuration (main.cpp)
    cost::Settings s; s.filterOversample = 2; s.skipSilentSlots = true; s.driveOversample = 2;
    cost::PartDesc part;
    part.voices = 5; part.priority = 0;
    part.voice.f1 = cost::Filter::ClassicLp24; part.voice.f2On = true; part.voice.f2 = cost::Filter::LowPass; part.voice.drive = 0.6f;
    part.voice.slot[0].unison = 4; part.voice.width = 0.8f; part.voice.slot[1].audible = true; part.voice.fold = 0.6f; part.voice.companding = 0.5f;
    cost::Step lad[cost::kMaxSteps];
    const int steps = cost::buildLadder(&part, 1, s, lad, cost::kMaxSteps);
    const double base = timeEngine(part);
    const float baseCost = cost::partCost(part, s);
    std::printf("heavy 5-voice part: model cost %.2f voice units, measured %.0f ns/sample (all 5 voices)\n\n", baseCost, base);
    std::printf("%5s  %-24s %9s %9s %9s %9s\n", "level", "step applied", "model", "measured", "model %", "measured %");
    double worst = 0;
    for (int lv = 0; lv <= steps; ++lv) {
        const cost::PartDesc d = cost::degraded(part, 0, lad, steps, lv);
        const double ns = timeEngine(d);
        const float mc = cost::partCost(d, s);
        const double mp = 100.0 * mc / baseCost, ms = 100.0 * ns / base;
        worst = std::fmax(worst, std::fabs(mp - ms));
        std::printf("%5d  %-24s %9.2f %8.0fns %8.0f%% %9.0f%%\n", lv, lv == 0 ? "(none)" : name(lad[lv - 1].action), mc, ns, mp, ms);
    }
    std::printf("\nworst gap between model and measurement: %.0f percentage points\n", worst);
}
