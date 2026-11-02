// Runs one baseline voice for a while so a sampling profiler can attribute the time (macOS: `sample <pid>`).
#include <nemesis/dsp/CathodeVoiceEngine.h>
#include <cstdio>
#include <cstdlib>
using namespace nemesis::dsp;
int main(int argc, char** argv) {
    const int secs = argc > 1 ? std::atoi(argv[1]) : 8;
    const int voices = argc > 2 ? std::atoi(argv[2]) : 1;
    const int filt = argc > 3 ? std::atoi(argv[3]) : 0;
    auto* eng = new CathodeVoiceEngine;
    eng->prepare(44100.0);
    eng->setVoiceCount(voices);
    eng->setLaneSitesForTest(0u);   // the firmware runs every voice scalar (no NEON on the M7)
    for (int v = 0; v < CathodeVoiceEngine::kMaxVoices; ++v) {
        auto& st = eng->voiceAt(v).outputStage(); auto& t = st.voice();
        t.setOscMode(0, CathodeVoiceStage::OscMode::PhaseDistortion); t.setPdShape(0, 0.3f); t.setPdAmount(0, 0.6f); t.setLevel(0, 1.0f);
        t.setOscMode(1, CathodeVoiceStage::OscMode::PhaseDistortion); t.setLevel(1, 0.0f);
        t.setFilterMode(static_cast<CathodeVoiceStage::FilterMode>(filt)); t.setCutoff(2000.0f, 3.0f);
        st.amp().setAttackMs(2.0f); st.amp().setDecayMs(20.0f); st.amp().setSustain(1.0f); st.amp().setReleaseMs(100.0f);
    }
    eng->reset();
    for (int v = 0; v < voices; ++v) eng->noteOn(48 + 5 * v, 0.9f);
    float sink = 0;
    for (long i = 0; i < 44100L * secs; ++i) sink += eng->processSample().left;
    std::printf("%g\n", sink);
}
