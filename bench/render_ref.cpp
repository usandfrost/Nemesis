// Renders a fixed set of CATHODE patches to a raw float32 file (interleaved L/R). The same source is built against
// upstream and against the fork, in double and in float, so outputs can be compared bit-for-bit or by error.
//   render_ref out.f32   -> writes patches back to back; prints one line per patch (name, frames).
#include <nemesis/dsp/CathodeVoiceEngine.h>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace nemesis::dsp;
using OM = CathodeVoiceStage::OscMode;
using FM = CathodeVoiceStage::FilterMode;
using ME = MacroOscillator::Engine;

namespace {
void common(CathodeVoiceStage& t, FM f, float cutoff = 2000.0f, float q = 3.0f) {
    t.setLevel(0, 0.5f);   // the voice has per-slot levels now (was a single osc mix)
    t.setLevel(1, 0.5f);
    t.setFilterMode(f);
    t.setCutoff(cutoff, q);
}
void pd(CathodeVoiceStage& t, FM f, float cutoff = 2000.0f, float q = 3.0f) {
    for (int s = 0; s < 2; ++s) {
        t.setOscMode(s, OM::PhaseDistortion);
        t.setPdShape(s, 0.3f + 0.2f * s);
        t.setPdAmount(s, 0.6f);
    }
    t.setSemitones(1, 7);
    common(t, f, cutoff, q);
}
void macro(CathodeVoiceStage& t, ME e, FM f) {
    for (int s = 0; s < 2; ++s) {
        t.setOscMode(s, OM::Macro);
        t.setMacroEngine(s, e);
    }
    t.setSemitones(1, 7);
    common(t, f);
}
void mask(CathodeVoiceStage& t, FM f) {
    for (int s = 0; s < 2; ++s) {
        t.setOscMode(s, OM::Bitmask);
        t.setMask(s, 77 + 100 * s);
    }
    common(t, f);
}
void crush(CathodeVoiceStage& t) {   // converter / companding / drift: the nonlinear and libm-heavy parts
    pd(t, FM::LowPass);
    t.setBitDepth(10.0f);
    t.setCompanding(0.5f);
    t.setDrift(0.6f);
}
struct Patch { std::string name; std::function<void(CathodeVoiceStage&)> setup; };
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::vector<Patch> patches = {
        {"pd-lowpass", [](auto& t) { pd(t, FM::LowPass); }},
        {"pd-bandpass", [](auto& t) { pd(t, FM::BandPass); }},
        {"pd-highpass", [](auto& t) { pd(t, FM::HighPass); }},
        {"pd-notch", [](auto& t) { pd(t, FM::Notch); }},
        {"pd-formant", [](auto& t) { pd(t, FM::Formant); }},
        {"pd-comb", [](auto& t) { pd(t, FM::Comb); }},
        {"pd-ladder24", [](auto& t) { pd(t, FM::Ladder24); }},
        {"pd-ladder12", [](auto& t) { pd(t, FM::Ladder12); }},
        {"pd-screamlp", [](auto& t) { pd(t, FM::ScreamLp); }},
        {"pd-acid", [](auto& t) { pd(t, FM::Acid); }},
        {"pd-sovietlp", [](auto& t) { pd(t, FM::SovietLp); }},
        {"pd-classiclp24", [](auto& t) { pd(t, FM::ClassicLp24); }},
        {"pd-reedlp", [](auto& t) { pd(t, FM::ReedLp); }},
        {"pd-statelp", [](auto& t) { pd(t, FM::StateLp); }},
        {"pd-lowcutoff", [](auto& t) { pd(t, FM::Ladder24, 40.0f, 6.0f); }},
        {"pd-highq", [](auto& t) { pd(t, FM::LowPass, 800.0f, 12.0f); }},
        {"crush", [](auto& t) { crush(t); }},
        {"mask-lowpass", [](auto& t) { mask(t, FM::LowPass); }},
        {"macro-analog", [](auto& t) { macro(t, ME::Analog, FM::LowPass); }},
        {"macro-swarm", [](auto& t) { macro(t, ME::Swarm, FM::LowPass); }},
        {"macro-modal", [](auto& t) { macro(t, ME::Modal, FM::Ladder24); }},
        {"macro-chords", [](auto& t) { macro(t, ME::Chords, FM::Formant); }},
    };
    FILE* fp = std::fopen(argv[1], "wb");
    if (!fp) return 3;
#ifdef DSP_IS_FORK
    CathodeOscCore::setLazyMacro(false);
#endif
    for (const auto& p : patches) {
        auto* eng = new CathodeVoiceEngine;
        eng->prepare(44100.0);
        eng->setVoiceCount(5);
        for (int v = 0; v < CathodeVoiceEngine::kMaxVoices; ++v) {
            auto& stage = eng->voiceAt(v).outputStage();
            p.setup(stage.voice());
            stage.amp().setAttackMs(5.0f);
            stage.amp().setDecayMs(30.0f);
            stage.amp().setSustain(0.8f);
            stage.amp().setReleaseMs(200.0f);
        }
        eng->reset();
        for (int n : {36, 48, 55, 64, 76}) eng->noteOn(n, 0.8f);   // low to high so every octave is covered
        const int frames = 44100;                                   // one second held
        std::vector<float> buf(2 * frames);
        for (int i = 0; i < frames; ++i) { auto s = eng->processSample(); buf[2 * i] = s.left; buf[2 * i + 1] = s.right; }
        std::fwrite(buf.data(), sizeof(float), buf.size(), fp);
        std::printf("%s %d\n", p.name.c_str(), frames);
        delete eng;
    }
    std::fclose(fp);
}
