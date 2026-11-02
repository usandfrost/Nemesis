// Per-feature CPU cost of one CATHODE voice. A baseline voice (one phase-distortion oscillator, low-pass filter) is timed, then
// each feature is switched on ALONE and timed again, so the table shows what each feature adds. One voice, held note.
// Output: ns per sample, cost relative to the baseline, sorted within each group. Host numbers: use them as RELATIVE costs.
#include <nemesis/dsp/CathodeVoiceEngine.h>
#include <nemesis/dsp/ForkConfig.h>
#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace nemesis::dsp;
using OM = CathodeVoiceStage::OscMode;
using FM = CathodeVoiceStage::FilterMode;
using ME = MacroOscillator::Engine;
using FR = CathodeVoiceStage::FilterRoute;

namespace {
Wavetable gWt;
std::vector<float> gWtData;

void base(CathodeVoiceStage& t) {
    t.setOscMode(0, OM::PhaseDistortion); t.setPdShape(0, 0.3f); t.setPdAmount(0, 0.6f); t.setLevel(0, 1.0f);
    t.setOscMode(1, OM::PhaseDistortion); t.setLevel(1, 0.0f);
    t.setFilterMode(FM::LowPass); t.setCutoff(2000.0f, 3.0f);
}

double run(const std::function<void(CathodeVoiceStage&)>& cfg, int voices = 1) {
    auto* eng = new CathodeVoiceEngine;
    eng->prepare(44100.0);
    eng->setVoiceCount(voices);
    eng->setLaneSitesForTest(0u);   // as the firmware: every voice scalar
    for (int v = 0; v < CathodeVoiceEngine::kMaxVoices; ++v) {
        auto& st = eng->voiceAt(v).outputStage();
        base(st.voice());
        cfg(st.voice());
        st.amp().setAttackMs(2.0f); st.amp().setDecayMs(20.0f); st.amp().setSustain(1.0f); st.amp().setReleaseMs(100.0f);
    }
    eng->reset();
    for (int v = 0; v < voices; ++v) eng->noteOn(48 + 5 * v, 0.9f);
    float sink = 0;
    for (int i = 0; i < 4410; ++i) sink += eng->processSample().left;      // warm-up, lets the envelope settle
    double best = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
        const int n = 44100;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) sink += eng->processSample().left;
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
    }
    if (sink == 1234.5f) std::puts("");
    delete eng;
    return best;
}

struct Case { std::string group, name; std::function<void(CathodeVoiceStage&)> cfg; double ns = 0; };
}

int main() {
    if (const char* c = std::getenv("COST_CFG"); c && std::string(c) == "fw") {   // the firmware's exact savings; oversampling stays at 4 (the cost model scales it)
        fork::Config::idleMatrixSkip = true; fork::Config::skipSilentSlots = true;
#ifdef NEMESIS_FORK_EXACT
        fork::Config::lanes = false;
#endif
        if (const char* os = std::getenv("COST_OS")) {   // measure at the firmware's factors (the table records which)
            fork::Config::filterOversample = std::atoi(os); fork::Config::driveOversample = std::atoi(os);
            std::printf("(filter cores and drives at x%d)\n", fork::Config::filterOversample);
        }
        std::puts("(measured with idleMatrixSkip and skipSilentSlots ON, as the firmware runs)");
    }
    gWtData.resize(8 * 2048);
    for (int f = 0; f < 8; ++f) for (int i = 0; i < 2048; ++i) {
        const double p = i / 2048.0;
        gWtData[f * 2048 + i] = (float)((1.0 - f / 7.0) * std::sin(6.2831853 * p) + (f / 7.0) * (2.0 * p - 1.0)) * 0.8f;
    }
    gWt.build(gWtData.data(), 8, 2048);

    std::vector<Case> cs;
    auto add = [&](const char* g, const std::string& n, std::function<void(CathodeVoiceStage&)> f) { cs.push_back({ g, n, std::move(f) }); };

    // --- oscillator modes (slot 0 only)
    add("OSC", "Phase distortion (baseline)", [](auto&) {});
    add("OSC", "Wavetable", [](auto& t) { t.setOscMode(0, OM::Wavetable); t.setWavetable(0, gWt); t.setPosition(0, 0.4f); });
    add("OSC", "Bitmask", [](auto& t) { t.setOscMode(0, OM::Bitmask); t.setMask(0, 77); });
    add("OSC", "Pulse", [](auto& t) { t.setOscMode(0, OM::Pulse); t.setPulseWavetable(gWt); t.setPulseWidth(0, 0.3); });
    const char* me[16] = {"Analog","Shaper","TwoOpFm","Grains","Harmonics","Tables","Chords","Vox","Swarm","Noise","Particles","String","Modal","Kick","Snare","Hat"};
    for (int e = 0; e < 16; ++e)
        add("MACRO (Plaits engine)", me[e], [e](auto& t) { t.setOscMode(0, OM::Macro); t.setMacroEngine(0, static_cast<ME>(e)); });

    // --- the phase-distortion oscillator's oversampling factor (default 4; the stage is re-prepared with 2 or 1)
    add("PD OVERSAMPLING", "PD oversample x2", [](auto& t) { t.prepare(44100.0, 2); base(t); });
    add("PD OVERSAMPLING", "PD oversample x1 (aliasing!)", [](auto& t) { t.prepare(44100.0, 1); base(t); });

    for (int f : {4, 2}) {
        add("FILTER OVERSAMPLING", "Classic24 with its core oversampled x" + std::to_string(f), [f](auto& t) {
            t.prepare(44100.0, PhaseDistortionOscillator::kDefaultOversample, ScreamFilter::kDefaultOversample, AcidFilter::kDefaultOversample,
                      SovietFilter::kDefaultOversample, f, ReedFilter::kDefaultOversample);
            base(t); t.setFilterMode(FM::ClassicLp24); });
        add("FILTER OVERSAMPLING", "Acid with its core oversampled x" + std::to_string(f), [f](auto& t) {
            t.prepare(44100.0, PhaseDistortionOscillator::kDefaultOversample, ScreamFilter::kDefaultOversample, f,
                      SovietFilter::kDefaultOversample, ClassicFilter::kDefaultOversample, ReedFilter::kDefaultOversample);
            base(t); t.setFilterMode(FM::Acid); });
    }

    // --- what a silent / unused oscillator slot costs (slot 1 is at level 0 in every baseline)
    add("SLOT", "Slot 1 silent, set to Wavetable", [](auto& t) { t.setOscMode(1, OM::Wavetable); t.setWavetable(1, gWt); t.setLevel(1, 0.0f); });
    add("SLOT", "Slot 1 silent, set to Pulse", [](auto& t) { t.setOscMode(1, OM::Pulse); t.setPulseWavetable(gWt); t.setLevel(1, 0.0f); });
    add("SLOT", "Both slots Wavetable (slot 1 silent)", [](auto& t) { t.setOscMode(0, OM::Wavetable); t.setWavetable(0, gWt); t.setOscMode(1, OM::Wavetable); t.setWavetable(1, gWt); t.setLevel(1, 0.0f); });
    add("SLOT", "Both slots Wavetable, both audible", [](auto& t) { t.setOscMode(0, OM::Wavetable); t.setWavetable(0, gWt); t.setOscMode(1, OM::Wavetable); t.setWavetable(1, gWt); t.setLevel(0, 0.5f); t.setLevel(1, 0.5f); });

    // --- oscillator add-ons
    add("OSC ADD-ON", "Second oscillator (PD)", [](auto& t) { t.setLevel(1, 1.0f); t.setLevel(0, 0.5f); });
    add("OSC ADD-ON", "Second oscillator + FM cross-mod", [](auto& t) { t.setLevel(1, 1.0f); t.setLevel(0, 0.5f); t.setXmodFm(0.5f); });
    add("OSC ADD-ON", "Second oscillator + ring mod", [](auto& t) { t.setLevel(1, 1.0f); t.setLevel(0, 0.5f); t.setXmodRing(0.5f); });
    add("OSC ADD-ON", "Second oscillator + hard sync", [](auto& t) { t.setLevel(1, 1.0f); t.setLevel(0, 0.5f); t.setSyncAmount(1.0f); });
    add("OSC ADD-ON", "Unison x4 on both slots (both audible)", [](auto& t) { t.setLevel(1, 1.0f); t.setLevel(0, 0.5f); for (int k = 0; k < 2; ++k) { t.setUnisonVoices(k, 4); t.setUnisonDetune(k, 0.3f); } });
    add("OSC ADD-ON", "Sub oscillator", [](auto& t) { t.setSubLevel(0.8f); });
    for (int u : {2, 4, 8})
        add("OSC ADD-ON", "Unison x" + std::to_string(u), [u](auto& t) { t.setUnisonVoices(0, u); t.setUnisonDetune(0, 0.3f); });
    add("OSC ADD-ON", "Unison x4 + stereo width", [](auto& t) { t.setUnisonVoices(0, 4); t.setUnisonDetune(0, 0.3f); t.setUnisonWidth(0, 0.8f); });

    // --- the stereo right chain (needs width / the Stereo filter route to engage)
    auto uni = [](CathodeVoiceStage& t) { t.setUnisonVoices(0, 4); t.setUnisonDetune(0, 0.3f); t.setUnisonWidth(0, 0.8f); };
    add("STEREO", "Unison x4 + width, F2 series (no right chain?)", [uni](auto& t) { uni(t); t.setFilter2On(true); t.setFilterRoute(FR::Series); t.setFilter2CutoffReso(2500.0f, 2.0f); });
    add("STEREO", "Unison x4 + width, F2 STEREO route", [uni](auto& t) { uni(t); t.setFilter2On(true); t.setFilterRoute(FR::Stereo); t.setFilter2CutoffReso(2500.0f, 2.0f); });
    add("STEREO", "Classic24 + unison x4, NO width", [](auto& t) { t.setUnisonVoices(0, 4); t.setUnisonDetune(0, 0.3f); t.setFilterMode(FM::ClassicLp24); });
    add("STEREO", "Classic24 + unison x4 + width 0.8", [uni](auto& t) { uni(t); t.setFilterMode(FM::ClassicLp24); });
    add("STEREO", "Classic24 + unison x4 + width, F2 series", [uni](auto& t) { uni(t); t.setFilterMode(FM::ClassicLp24); t.setFilter2On(true); t.setFilterRoute(FR::Series); t.setFilter2CutoffReso(2500.0f, 2.0f); });
    add("STEREO", "Classic24 + unison x4 + width, F2 STEREO", [uni](auto& t) { uni(t); t.setFilterMode(FM::ClassicLp24); t.setFilter2On(true); t.setFilterRoute(FR::Stereo); t.setFilter2CutoffReso(2500.0f, 2.0f); });

    // --- filters (F1)
    const char* fm[] = {"LowPass","BandPass","HighPass","Notch","Formant","Comb","Ladder24","Ladder12","ScreamLp","ScreamHp","Acid","SovietLp","SovietBp","SovietHp","ClassicLp24","ClassicLp12","ClassicBp","ClassicHp","ReedLp","ReedBp","ReedHp","StateLp","StateBp","StateHp","StateNotch","StateAp","StateMorph","ReedMorph"};
    for (int m = 0; m < 28; ++m)
        add("FILTER", fm[m], [m](auto& t) { t.setFilterMode(static_cast<FM>(m)); t.setCutoff(2000.0f, 3.0f); });
    add("FILTER ADD-ON", "HPF on", [](auto& t) { t.setHpf(120.0f); });
    add("FILTER ADD-ON", "Filter drive", [](auto& t) { t.setDrive(0.6f); });
    add("FILTER ADD-ON", "F2 on, series (LowPass+LowPass)", [](auto& t) { t.setFilter2On(true); t.setFilterRoute(FR::Series); t.setFilter2CutoffReso(2500.0f, 2.0f); });
    add("FILTER ADD-ON", "F2 on, parallel", [](auto& t) { t.setFilter2On(true); t.setFilterRoute(FR::Parallel); t.setFilter2CutoffReso(2500.0f, 2.0f); });
    add("FILTER ADD-ON", "F2 on, STEREO (right chain)", [](auto& t) { t.setFilter2On(true); t.setFilterRoute(FR::Stereo); t.setFilter2CutoffReso(2500.0f, 2.0f); });
    add("FILTER ADD-ON", "Ladder24 + F2 Ladder24-class series", [](auto& t) { t.setFilterMode(FM::Ladder24); t.setFilter2On(true); t.setFilterRoute(FR::Series); t.setFilter2CutoffReso(2500.0f, 2.0f); });
    add("FILTER ADD-ON", "Acid + F2 series", [](auto& t) { t.setFilterMode(FM::Acid); t.setFilter2On(true); t.setFilterRoute(FR::Series); t.setFilter2CutoffReso(2500.0f, 2.0f); });

    // --- grit / converter / drift
    add("GRIT", "Bit depth 10", [](auto& t) { t.setBitDepth(10.0f); });
    add("GRIT", "Bit depth 10 + companding", [](auto& t) { t.setBitDepth(10.0f); t.setCompanding(0.5f); });
    add("GRIT", "Converter rate 12 kHz", [](auto& t) { t.setConverterRate(12000.0f); });
    add("GRIT", "Loop error", [](auto& t) { t.setLoopError(0.5f); });
    add("GRIT", "Drift", [](auto& t) { t.setDrift(0.6f); });
    add("GRIT", "Fold", [](auto& t) { t.setFoldAmount(0.6f); });

    for (auto& c : cs) c.ns = run(c.cfg);
    const double ref = cs[0].ns;
    std::printf("%-26s %-40s %9s %8s %8s\n", "group", "feature", "ns/sample", "x base", "extra ns");
    std::string g;
    std::vector<size_t> order(cs.size());
    for (size_t i = 0; i < cs.size(); ++i) order[i] = i;
    for (size_t i = 0; i < cs.size(); ++i) {
        const Case& c = cs[i];
        if (c.group != g) { g = c.group; std::puts(""); }
        std::printf("%-26s %-40s %9.0f %7.2fx %+8.0f\n", c.group.c_str(), c.name.c_str(), c.ns, c.ns / ref, c.ns - ref);
    }
    std::printf("\nbaseline voice: %.0f ns/sample\n", ref);
}
