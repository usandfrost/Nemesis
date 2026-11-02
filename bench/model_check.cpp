// Does the cost model (firmware/src/cost) predict what the engine really costs? For a set of patches, time one voice on the host with the same
// CPU savings the firmware uses, and compare the measured cost (relative to the plain voice) with voiceCost().
#include <nemesis/dsp/CathodeVoiceEngine.h>
#include <nemesis/dsp/ForkConfig.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "cost/CostModel.h"

using namespace nemesis::dsp;
using OM = CathodeVoiceStage::OscMode;
using FM = CathodeVoiceStage::FilterMode;
using FR = CathodeVoiceStage::FilterRoute;

struct Case { std::string name; cost::VoiceDesc d; };

static void apply(CathodeVoiceStage& t, const cost::VoiceDesc& d) {
    t.setOscMode(0, OM::PhaseDistortion); t.setPdShape(0, 0.3f); t.setPdAmount(0, 0.6f); t.setLevel(0, 0.7f);
    t.setOscMode(1, OM::PhaseDistortion); t.setPdShape(1, 0.5f); t.setPdAmount(1, 0.5f); t.setSemitones(1, 7); t.setLevel(1, d.slot[1].audible ? 0.7f : 0.0f);
    t.setUnisonVoices(0, d.slot[0].unison); t.setUnisonVoices(1, d.slot[1].unison);
    if (d.slot[0].unison > 1 || d.slot[1].unison > 1) { t.setUnisonDetune(0, 0.3f); t.setUnisonDetune(1, 0.3f); }
    t.setUnisonWidth(0, d.width); t.setUnisonWidth(1, d.width);
    t.setFilterMode(static_cast<FM>(d.f1)); t.setCutoff(2000.0f, 3.0f);
    if (d.f2On) { t.setFilter2On(true); t.setFilterRoute(static_cast<FR>(d.route)); t.setFilter2CutoffReso(2500.0f, 2.0f); }
    t.setDrive(d.drive); t.setXmodFm(d.fm); t.setXmodRing(d.ring); t.setSyncAmount(d.sync); t.setFoldAmount(d.fold); t.setCompanding(d.companding);
    if (d.companding > 0) t.setBitDepth(10.0f);
}

static double measure(const cost::VoiceDesc& d) {
    double best = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        auto* eng = new CathodeVoiceEngine; eng->prepare(44100.0); eng->setVoiceCount(1); eng->setLaneSitesForTest(0u);
        auto& mv = eng->voiceAt(0); apply(mv.outputStage().voice(), d);
        mv.outputStage().amp().setAttackMs(2.0f); mv.outputStage().amp().setSustain(1.0f);
        eng->reset(); eng->noteOn(48, 0.9f);
        float sink = 0; for (int i = 0; i < 4410; ++i) sink += eng->processSample().left;
        const int n = 44100;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) sink += eng->processSample().left;
        const auto t1 = std::chrono::steady_clock::now();
        best = std::fmin(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
        if (sink == 1.2345f) std::puts("");
        delete eng;
    }
    return best;
}

int main() {
    // the firmware's savings
    fork::Config::idleMatrixSkip = true; fork::Config::skipSilentSlots = true; fork::Config::filterOversample = 2; fork::Config::pdOversample = 4;
    fork::Config::driveOversample = 2; fork::Config::lanes = false;   // the firmware configuration (main.cpp)
    cost::Settings s; s.filterOversample = 2; s.skipSilentSlots = true; s.driveOversample = 2;
    std::vector<Case> cs;
    auto add = [&](const char* n, auto fn) { cost::VoiceDesc d; fn(d); cs.push_back({ n, d }); };
    add("plain (1 audible slot, LowPass)", [](auto&) {});
    add("both slots audible", [](auto& d) { d.slot[1].audible = true; });
    add("Ladder24 (light)", [](auto& d) { d.f1 = cost::Filter::Ladder24; });
    add("State LP (light)", [](auto& d) { d.f1 = cost::Filter::StateLp; });
    add("Classic LP24", [](auto& d) { d.f1 = cost::Filter::ClassicLp24; });
    add("Acid", [](auto& d) { d.f1 = cost::Filter::Acid; });
    add("Reed LP", [](auto& d) { d.f1 = cost::Filter::ReedLp; });
    add("Soviet LP", [](auto& d) { d.f1 = cost::Filter::SovietLp; });
    add("Scream LP", [](auto& d) { d.f1 = cost::Filter::ScreamLp; });
    add("filter drive", [](auto& d) { d.drive = 0.6f; });
    add("Classic + drive", [](auto& d) { d.f1 = cost::Filter::ClassicLp24; d.drive = 0.6f; });
    add("F2 series (light + light)", [](auto& d) { d.f2On = true; d.f2 = cost::Filter::LowPass; });
    add("Classic + F2 series (light)", [](auto& d) { d.f1 = cost::Filter::ClassicLp24; d.f2On = true; d.f2 = cost::Filter::LowPass; });
    add("unison x2", [](auto& d) { d.slot[0].unison = 2; });
    add("unison x4", [](auto& d) { d.slot[0].unison = 4; });
    add("unison x8", [](auto& d) { d.slot[0].unison = 8; });
    add("unison x4 + width", [](auto& d) { d.slot[0].unison = 4; d.width = 0.8f; });
    add("Classic + unison x4 + width", [](auto& d) { d.f1 = cost::Filter::ClassicLp24; d.slot[0].unison = 4; d.width = 0.8f; });
    add("ring mod", [](auto& d) { d.slot[1].audible = true; d.ring = 0.5f; });
    add("FM cross-mod", [](auto& d) { d.slot[1].audible = true; d.fm = 0.5f; });
    add("hard sync", [](auto& d) { d.slot[1].audible = true; d.sync = 1.0f; });
    add("companding + fold", [](auto& d) { d.companding = 0.5f; d.fold = 0.6f; });
    add("ring + Classic", [](auto& d) { d.slot[1].audible = true; d.ring = 0.5f; d.f1 = cost::Filter::ClassicLp24; });
    add("ring + unison x4", [](auto& d) { d.slot[1].audible = true; d.ring = 0.5f; d.slot[0].unison = 4; });
    add("ring + unison x4 + width", [](auto& d) { d.slot[1].audible = true; d.ring = 0.5f; d.slot[0].unison = 4; d.width = 0.8f; });
    add("FM + unison x4", [](auto& d) { d.slot[1].audible = true; d.fm = 0.5f; d.slot[0].unison = 4; });
    add("FM + unison x4 + width", [](auto& d) { d.slot[1].audible = true; d.fm = 0.5f; d.slot[0].unison = 4; d.width = 0.8f; });
    add("sync + unison x4", [](auto& d) { d.slot[1].audible = true; d.sync = 1.0f; d.slot[0].unison = 4; });
    add("sync + unison x4 + width", [](auto& d) { d.slot[1].audible = true; d.sync = 1.0f; d.slot[0].unison = 4; d.width = 0.8f; });
    add("unison x4 (slot 0) + unison x4 (slot 1), both audible", [](auto& d) { d.slot[1].audible = true; d.slot[0].unison = 4; d.slot[1].unison = 4; });
    add("ring + F2 series", [](auto& d) { d.slot[1].audible = true; d.ring = 0.5f; d.f2On = true; });
    add("ring + FM", [](auto& d) { d.slot[1].audible = true; d.ring = 0.5f; d.fm = 0.5f; });
    add("Classic + F2 + unison x4 + width", [](auto& d) { d.f1 = cost::Filter::ClassicLp24; d.f2On = true; d.slot[0].unison = 4; d.width = 0.8f; });
    add("everything: Classic, F2, drive, unison x4+width, ring", [](auto& d) { d.f1 = cost::Filter::ClassicLp24; d.f2On = true; d.drive = 0.6f; d.slot[0].unison = 4; d.width = 0.8f; d.slot[1].audible = true; d.ring = 0.5f; });

    const double plainNs = measure(cs[0].d);
    const float plainCost = cost::voiceCost(cs[0].d, s);
    std::printf("%-56s %9s %9s %9s %8s\n", "patch (firmware savings on)", "ns/sample", "measured", "model", "error");
    double worst = 0, sumAbs = 0; int n = 0;
    for (const auto& c : cs) {
        const double ns = measure(c.d), meas = ns / plainNs, model = cost::voiceCost(c.d, s) / plainCost, err = model - meas;
        std::printf("%-56s %9.0f %8.2fx %8.2fx %+7.2f\n", c.name.c_str(), ns, meas, model, err);
        worst = std::fmax(worst, std::fabs(err)); sumAbs += std::fabs(err); ++n;
    }
    std::printf("\nmean |error| %.2f, worst |error| %.2f (in units of the plain voice)\n", sumAbs / n, worst);
}
