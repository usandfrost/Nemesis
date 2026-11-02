// Coverage renders for the DSP-review optimizations (docs/DSP_REVIEW.md). Every patch exercises at least one optimized path:
// cross-mod legs, sync, unison, stereo route + width (right twin), modulated cutoff / pitch / vowel, F2, drive placements,
// output drive, PD pulse shapes, bitmask, voice stealing. Built against upstream and against the fork in several configs;
// tools compare the raw float32 outputs (interleaved L/R, patches back to back).
//   render_cov out.f32 [seconds]
#include <nemesis/dsp/CathodeVoiceEngine.h>
#ifdef DSP_IS_FORK
#include <nemesis/dsp/ForkConfig.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace nemesis::dsp;
using OM = CathodeVoiceStage::OscMode;
using FM = CathodeVoiceStage::FilterMode;
using Route = CathodeVoiceStage::FilterRoute;
using PdT = PhaseDistortionOscillator::PdType;
using Src = CathodeModMatrix::Source;
using Dst = CathodeModMatrix::Dest;

namespace {
Wavetable gWt;                       // 8 frames sine -> saw, as bench/cost_bench builds it
std::vector<float> gWtData;
void buildWt() {
    gWtData.resize(8 * 2048);
    for (int f = 0; f < 8; ++f)
        for (int i = 0; i < 2048; ++i) {
            const double p = i / 2048.0;
            gWtData[f * 2048 + i] = (float)((1.0 - f / 7.0) * std::sin(6.2831853 * p) + (f / 7.0) * (2.0 * p - 1.0)) * 0.8f;
        }
    gWt.build(gWtData.data(), 8, 2048);
}
void wt(CathodeModulatedVoice& mv, int slot);
using Cfg = std::function<void(CathodeModulatedVoice&)>;
struct Patch { std::string name; Cfg cfg; Cfg mid = nullptr; Cfg mid2 = nullptr; };   // `mid` / `mid2` run on every voice at 40% / 70%   // `mid` runs on every voice at 40% of the render

void base(CathodeModulatedVoice& mv) {
    auto& t = mv.outputStage().voice();
    for (int s = 0; s < 2; ++s) {
        t.setOscMode(s, OM::PhaseDistortion);
        t.setPdShape(s, 0.3f + 0.2f * s);
        t.setPdAmount(s, 0.6f);
    }
    t.setSemitones(1, 7);
    t.setLevel(0, 0.7f);
    t.setLevel(1, 0.0f);
    t.setFilterMode(FM::LowPass);
    t.setCutoff(2000.0f, 3.0f);
    mv.setBaseCutoff(2000.0f, 3.0f);
    auto& a = mv.outputStage().amp();
    a.setAttackMs(3.0f); a.setDecayMs(60.0f); a.setSustain(0.7f); a.setReleaseMs(120.0f);
}
CathodeVoiceStage& T(CathodeModulatedVoice& mv) { return mv.outputStage().voice(); }
void model(CathodeModulatedVoice& mv, FM f, float hz = 1500.0f, float q = 4.0f) {
    T(mv).setFilterMode(f); T(mv).setCutoff(hz, q); mv.setBaseCutoff(hz, q);
}
void cutoffEnv(CathodeModulatedVoice& mv) {   // Env2 sweeps the cutoff: coefficients recomputed per sample
    mv.env2().setAttackMs(2.0f); mv.env2().setDecayMs(150.0f); mv.env2().setSustain(0.2f); mv.env2().setReleaseMs(100.0f);
    mv.setSlot(0, Src::Env2, Dst::FiltCutoff, 0.6f);
}
void wt(CathodeModulatedVoice& mv, int slot) {
    mv.outputStage().voice().setOscMode(slot, OM::Wavetable);
    mv.outputStage().voice().setWavetable(slot, gWt);
    mv.outputStage().voice().setPosition(slot, 0.4f);
}
void pulse(CathodeModulatedVoice& mv, int slot) {
    mv.outputStage().voice().setOscMode(slot, OM::Pulse);
    mv.outputStage().voice().setPulseWavetable(gWt);
    mv.outputStage().voice().setPulseWidth(slot, 0.3);
}
void vibrato(CathodeModulatedVoice& mv) {
    mv.lfo1().setRateHz(5.5f); mv.lfo1().setDepth(1.0f);
    mv.setSlot(1, Src::Lfo1, Dst::Pitch, 0.05f);
}
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const double seconds = argc > 2 ? std::atof(argv[2]) : 1.0;
    const std::vector<Patch> patches = {
        {"plain pd lp", [](auto& mv) {}},
        {"both slots", [](auto& mv) { T(mv).setLevel(1, 0.5f); }},
        {"fm", [](auto& mv) { T(mv).setXmodFm(0.5f); }},
        {"fm both audible", [](auto& mv) { T(mv).setXmodFm(0.4f); T(mv).setLevel(1, 0.4f); }},
        {"ring", [](auto& mv) { T(mv).setXmodRing(0.6f); }},
        {"ring + fm", [](auto& mv) { T(mv).setXmodRing(0.5f); T(mv).setXmodFm(0.3f); T(mv).setLevel(1, 0.3f); }},
        {"sync", [](auto& mv) { T(mv).setSyncAmount(1.0f); T(mv).setLevel(1, 0.5f); }},
        {"unison x4 slot0", [](auto& mv) { T(mv).setUnisonVoices(0, 4); T(mv).setUnisonDetune(0, 0.3f); }},
        {"unison x8 width", [](auto& mv) { T(mv).setUnisonVoices(0, 8); T(mv).setUnisonDetune(0, 0.4f); T(mv).setUnisonWidth(0, 0.8f); }},
        {"unison width classic", [](auto& mv) { model(mv, FM::ClassicLp24); T(mv).setUnisonVoices(0, 3); T(mv).setUnisonDetune(0, 0.3f); T(mv).setUnisonWidth(0, 0.7f); cutoffEnv(mv); }},
        {"pd pulse", [](auto& mv) { T(mv).setPdType(0, PdT::Pulse); }},
        {"pd sawpulse", [](auto& mv) { T(mv).setPdType(0, PdT::SawPulse); T(mv).setPdType(1, PdT::Pulse); T(mv).setLevel(1, 0.4f); }},
        {"pd square reso", [](auto& mv) { T(mv).setPdType(0, PdT::Square); T(mv).setPdType(1, PdT::ResoII); T(mv).setLevel(1, 0.4f); }},
        {"bitmask", [](auto& mv) { for (int s = 0; s < 2; ++s) { T(mv).setOscMode(s, OM::Bitmask); T(mv).setMask(s, 77 + 100 * s); } T(mv).setLevel(1, 0.4f); }},
        {"bitmask unison", [](auto& mv) { T(mv).setOscMode(0, OM::Bitmask); T(mv).setMask(0, 123); T(mv).setUnisonVoices(0, 4); T(mv).setUnisonDetune(0, 0.25f); }},
        {"vibrato", [](auto& mv) { vibrato(mv); T(mv).setLevel(1, 0.4f); T(mv).setSubLevel(0.4f); }},
        {"vibrato unison", [](auto& mv) { vibrato(mv); T(mv).setUnisonVoices(0, 4); T(mv).setUnisonDetune(0, 0.3f); }},
        {"cutoff env lp", [](auto& mv) { cutoffEnv(mv); }},
        {"cutoff env classic", [](auto& mv) { model(mv, FM::ClassicLp24); cutoffEnv(mv); }},
        {"cutoff env acid", [](auto& mv) { model(mv, FM::Acid, 900.0f, 8.0f); cutoffEnv(mv); }},
        {"cutoff env soviet", [](auto& mv) { model(mv, FM::SovietLp); cutoffEnv(mv); }},
        {"cutoff env scream", [](auto& mv) { model(mv, FM::ScreamLp); cutoffEnv(mv); }},
        {"cutoff env reed", [](auto& mv) { model(mv, FM::ReedLp); cutoffEnv(mv); }},
        {"cutoff env ladder24", [](auto& mv) { model(mv, FM::Ladder24); cutoffEnv(mv); }},
        {"cutoff env ladder12", [](auto& mv) { model(mv, FM::Ladder12); cutoffEnv(mv); }},
        {"cutoff env state", [](auto& mv) { model(mv, FM::StateLp); cutoffEnv(mv); }},
        {"cutoff env comb", [](auto& mv) { model(mv, FM::Comb); cutoffEnv(mv); }},
        {"reso sweep classic", [](auto& mv) { model(mv, FM::ClassicLp24, 1200.0f, 12.0f); mv.lfo2().setRateHz(3.0f); mv.lfo2().setDepth(1.0f); mv.setSlot(2, Src::Lfo2, Dst::FiltReso, 0.5f); }},
        {"vowel lfo formant", [](auto& mv) { model(mv, FM::Formant); mv.lfo1().setRateHz(2.0f); mv.lfo1().setDepth(1.0f); mv.setSlot(0, Src::Lfo1, Dst::FiltVowel, 0.8f); }},
        {"vowel lfo lowpass", [](auto& mv) { mv.lfo1().setRateHz(2.0f); mv.lfo1().setDepth(1.0f); mv.setSlot(0, Src::Lfo1, Dst::FiltVowel, 0.8f); }},
        {"drive pre lp", [](auto& mv) { T(mv).setDrive(0.6f); }},
        {"drive pre ladder", [](auto& mv) { model(mv, FM::Ladder24); T(mv).setDrive(0.7f); cutoffEnv(mv); }},
        {"drive pre ladder12", [](auto& mv) { model(mv, FM::Ladder12, 1200.0f, 6.0f); T(mv).setDrive(0.6f); cutoffEnv(mv); }},
        {"drive post classic", [](auto& mv) { model(mv, FM::ClassicLp24); T(mv).setDrive(0.5f); T(mv).setDrivePlacement(CathodeFilterBank::DrivePlacement::PostFilter); }},
        {"classic drive", [](auto& mv) { model(mv, FM::ClassicLp24, 1800.0f, 10.0f); T(mv).setClassicDrive(0.7f); }},
        {"output drive", [](auto& mv) { mv.outputStage().setOutputDrive(0.6f); }},
        {"f2 series acid", [](auto& mv) { T(mv).setFilter2On(true); T(mv).setFilterRoute(Route::Series); T(mv).setFilter2CutoffReso(2500.0f, 2.0f); cutoffEnv(mv); }},
        {"f2 parallel classic", [](auto& mv) { model(mv, FM::ClassicLp24); T(mv).setFilter2On(true); T(mv).setFilterRoute(Route::Parallel); T(mv).setFilter2CutoffReso(900.0f, 4.0f); }},
        {"f2 stereo", [](auto& mv) { T(mv).setFilter2On(true); T(mv).setFilterRoute(Route::Stereo); T(mv).setFilter2CutoffReso(3000.0f, 3.0f); cutoffEnv(mv); }},
        {"comb stereo", [](auto& mv) { model(mv, FM::Comb); T(mv).setUnisonVoices(0, 2); T(mv).setUnisonDetune(0, 0.2f); T(mv).setUnisonWidth(0, 0.6f); }},
        {"crush", [](auto& mv) { T(mv).setBitDepth(10.0f); T(mv).setCompanding(0.5f); T(mv).setDrift(0.6f); }},
        {"fold + noise", [](auto& mv) { T(mv).setFoldAmount(0.5f); T(mv).setNoiseLevel(0.2f); }},
        {"wt fm", [](auto& mv) { wt(mv, 0); T(mv).setXmodFm(0.5f); T(mv).setLevel(1, 0.3f); }},
        {"wt ring unison", [](auto& mv) { wt(mv, 0); T(mv).setXmodRing(0.6f); T(mv).setUnisonVoices(0, 3); T(mv).setUnisonWidth(0, 0.5f); }},
        {"wt vibrato unison", [](auto& mv) { wt(mv, 0); vibrato(mv); T(mv).setUnisonVoices(0, 4); T(mv).setUnisonDetune(0, 0.3f); }},
        {"pulse fm", [](auto& mv) { pulse(mv, 0); T(mv).setXmodFm(0.4f); T(mv).setLevel(1, 0.3f); }},
        {"pulse unison", [](auto& mv) { pulse(mv, 0); T(mv).setUnisonVoices(0, 4); T(mv).setUnisonDetune(0, 0.3f); }},
        {"bitmask fm", [](auto& mv) { T(mv).setOscMode(0, OM::Bitmask); T(mv).setMask(0, 99); T(mv).setXmodFm(0.4f); T(mv).setLevel(1, 0.3f); }},
        {"switch ladder24->12 driven", [](auto& mv) { model(mv, FM::Ladder24); T(mv).setDrive(0.6f); }, [](auto& mv) { T(mv).setFilterMode(FM::Ladder12); }},
        {"switch lp->classic->formant", [](auto& mv) { cutoffEnv(mv); }, [](auto& mv) { T(mv).setFilterMode(FM::ClassicLp24); }},
        {"switch fm on", [](auto& mv) { T(mv).setLevel(1, 0.3f); }, [](auto& mv) { T(mv).setXmodFm(0.5f); T(mv).setXmodRing(0.4f); }},
        {"switch width on", [](auto& mv) { model(mv, FM::Comb); T(mv).setUnisonVoices(0, 3); T(mv).setUnisonDetune(0, 0.3f); cutoffEnv(mv); }, [](auto& mv) { T(mv).setUnisonWidth(0, 0.7f); }},
        {"switch formant + vowel", [](auto& mv) { mv.lfo1().setRateHz(2.0f); mv.lfo1().setDepth(1.0f); mv.setSlot(0, Src::Lfo1, Dst::FiltVowel, 0.8f); }, [](auto& mv) { T(mv).setFilterMode(FM::Formant); }},
        {"switch f2 on stereo", [](auto& mv) { model(mv, FM::Acid); cutoffEnv(mv); T(mv).setFilterRoute(Route::Stereo); T(mv).setFilter2CutoffReso(1500.0f, 3.0f); }, [](auto& mv) { T(mv).setFilter2On(true); }},
        {"switch draw envelopes", [](auto& mv) { cutoffEnv(mv); }, [](auto& mv) { mv.setEnvDraw(2, true); mv.setEnvDrawLengthMs(2, 400.0); mv.outputStage().setAmpDraw(true); }},
        {"switch balance + route", [](auto& mv) { T(mv).setFilter2On(true); T(mv).setFilter2CutoffReso(700.0f, 2.0f); }, [](auto& mv) { T(mv).setFilterRoute(Route::Parallel); T(mv).setFilterBalance(0.6f); }},
        {"switch formant static vowel", [](auto& mv) { mv.setBaseVowel(0.7f); }, [](auto& mv) { T(mv).setFilterMode(FM::Formant); }},
        {"switch lp static cutoff", [](auto& mv) { model(mv, FM::Acid, 700.0f, 5.0f); mv.setBaseCutoff(3500.0f, 6.0f); }, [](auto& mv) { T(mv).setFilterMode(FM::LowPass); }},
        {"switch route only", [](auto& mv) { T(mv).setFilter2On(true); T(mv).setFilter2CutoffReso(700.0f, 2.0f); }, [](auto& mv) { T(mv).setFilterRoute(Route::Parallel); }},
        {"switch f2 only", [](auto& mv) { T(mv).setFilter2CutoffReso(700.0f, 2.0f); }, [](auto& mv) { T(mv).setFilter2On(true); }},
        {"switch amp draw only", [](auto& mv) {}, [](auto& mv) { mv.outputStage().setAmpDraw(true); }},
        {"stereo reso sweep classic", [](auto& mv) { model(mv, FM::ClassicLp24, 1200.0f, 10.0f); T(mv).setUnisonVoices(0, 2); T(mv).setUnisonDetune(0, 0.25f); T(mv).setUnisonWidth(0, 0.8f); mv.lfo2().setRateHz(3.0f); mv.lfo2().setDepth(1.0f); mv.setSlot(2, Src::Lfo2, Dst::FiltReso, 0.5f); cutoffEnv(mv); }},
        {"stereo reso sweep scream", [](auto& mv) { model(mv, FM::ScreamLp, 1200.0f, 10.0f); T(mv).setUnisonVoices(0, 2); T(mv).setUnisonDetune(0, 0.25f); T(mv).setUnisonWidth(0, 0.8f); mv.lfo2().setRateHz(3.0f); mv.lfo2().setDepth(1.0f); mv.setSlot(2, Src::Lfo2, Dst::FiltReso, 0.9f); cutoffEnv(mv); }},
        {"switch fm carrier pd->wt", [](auto& mv) { T(mv).setXmodFm(0.5f); T(mv).setLevel(1, 0.3f); }, [](auto& mv) { wt(mv, 0); }},
        {"stereo f2 acid + ladder", [](auto& mv) { model(mv, FM::Ladder24, 900.0f, 6.0f); T(mv).setUnisonVoices(0, 3); T(mv).setUnisonDetune(0, 0.3f); T(mv).setUnisonWidth(0, 0.7f); T(mv).setFilter2On(true); T(mv).setFilterRoute(Route::Series); T(mv).setFilter2CutoffReso(2200.0f, 3.0f); cutoffEnv(mv); vibrato(mv); }},
        {"stereo drive soviet", [](auto& mv) { model(mv, FM::SovietLp, 1100.0f, 6.0f); T(mv).setDrive(0.5f); T(mv).setUnisonVoices(0, 2); T(mv).setUnisonDetune(0, 0.3f); T(mv).setUnisonWidth(0, 0.9f); cutoffEnv(mv); }},
        {"stereo then comb", [](auto& mv) { model(mv, FM::Acid, 900.0f, 4.0f); T(mv).setUnisonVoices(0, 3); T(mv).setUnisonDetune(0, 0.3f); T(mv).setUnisonWidth(0, 0.7f); }, [](auto& mv) { T(mv).setFilterMode(FM::Comb); }},
        {"comb, lp + stereo, comb again", [](auto& mv) { model(mv, FM::Comb); T(mv).setCombFeedback(0.8f); T(mv).setUnisonVoices(0, 3); T(mv).setUnisonDetune(0, 0.3f); },
            [](auto& mv) { T(mv).setFilterMode(FM::LowPass); T(mv).setUnisonWidth(0, 0.7f); }, [](auto& mv) { T(mv).setFilterMode(FM::Comb); }},
        {"lfos sh step routed", [](auto& mv) { mv.lfo3().setRateHz(7.0f); mv.lfo3().setDepth(0.7f); mv.setSlot(3, Src::Lfo3, Dst::Pan, 0.4f); mv.setSlot(4, Src::SampleHold, Dst::PdAmount, 0.3f); }},
    };
    buildWt();
    FILE* fp = std::fopen(argv[1], "wb");
    if (!fp) return 3;
#ifdef DSP_IS_FORK
    CathodeOscCore::setLazyMacro(false);
    fork::Config::lanes = false;   // as the firmware: no lane groups, every lane site off from prepare()
#endif
    const int frames = static_cast<int>(44100 * seconds);
    for (const auto& p : patches) {
        auto* eng = new CathodeVoiceEngine;
        eng->prepare(44100.0);
        eng->setVoiceCount(4);
        eng->setLaneSitesForTest(0u);
        for (int v = 0; v < CathodeVoiceEngine::kMaxVoices; ++v) { base(eng->voiceAt(v)); p.cfg(eng->voiceAt(v)); }
        eng->reset();
        // 33 = 55 Hz (float phase rounding is worst there); a 5th note steals; a note-off exercises the release.
        const int notes[5] = {33, 45, 52, 64, 71};
        for (int k = 0; k < 4; ++k) eng->noteOn(notes[k], 0.8f);
        std::vector<float> buf(2 * static_cast<size_t>(frames));
        for (int i = 0; i < frames; ++i) {
            if (i == frames / 3) eng->noteOff(notes[1]);
            if (i == frames / 2) eng->noteOn(notes[4], 0.7f);
            if (p.mid && i == frames * 2 / 5)
                for (int v = 0; v < CathodeVoiceEngine::kMaxVoices; ++v) p.mid(eng->voiceAt(v));
            if (p.mid2 && i == frames * 7 / 10)
                for (int v = 0; v < CathodeVoiceEngine::kMaxVoices; ++v) p.mid2(eng->voiceAt(v));
            const auto s = eng->processSample(); buf[2 * i] = s.left; buf[2 * i + 1] = s.right;
        }
        std::fwrite(buf.data(), sizeof(float), buf.size(), fp);
        std::printf("%s %d\n", p.name.c_str(), frames);
        delete eng;
    }
    std::fclose(fp);
}
