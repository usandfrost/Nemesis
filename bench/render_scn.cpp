// Feature-by-feature scenarios to find where the float build departs from the double reference. One second each.
#include <nemesis/dsp/CathodeVoiceEngine.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace nemesis::dsp;
using OM = CathodeVoiceStage::OscMode;
using FM = CathodeVoiceStage::FilterMode;

struct Scn { const char* name; int voices; float pdAmt; float lvl0, lvl1; float cutoff, q; bool env; };

int main(int argc, char** argv) {
    const std::vector<Scn> scns = {
        {"1v sine open",        1, 0.0f, 1.0f, 0.0f, 19000.0f, 0.7f, false},
        {"1v pd0.6 open",       1, 0.6f, 1.0f, 0.0f, 19000.0f, 0.7f, false},
        {"1v pd0.6 2osc open",  1, 0.6f, 0.5f, 0.5f, 19000.0f, 0.7f, false},
        {"1v pd0.6 lp2k q3",    1, 0.6f, 1.0f, 0.0f, 2000.0f, 3.0f, false},
        {"1v sine lp2k q3",     1, 0.0f, 1.0f, 0.0f, 2000.0f, 3.0f, false},
        {"5v pd0.6 open",       5, 0.6f, 1.0f, 0.0f, 19000.0f, 0.7f, false},
        {"5v pd0.6 lp2k q3",    5, 0.6f, 1.0f, 0.0f, 2000.0f, 3.0f, false},
    };
    FILE* fp = std::fopen(argv[1], "wb");
#ifdef DSP_IS_FORK
    CathodeOscCore::setLazyMacro(false);
#endif
    for (const auto& sc : scns) {
        auto* eng = new CathodeVoiceEngine;
        eng->prepare(44100.0);
        eng->setVoiceCount(sc.voices);
        for (int v = 0; v < CathodeVoiceEngine::kMaxVoices; ++v) {
            auto& st = eng->voiceAt(v).outputStage();
            auto& t = st.voice();
            for (int s = 0; s < 2; ++s) {
                t.setOscMode(s, OM::PhaseDistortion);
                t.setPdShape(s, 0.3f);
                t.setPdAmount(s, sc.pdAmt);
            }
            t.setLevel(0, sc.lvl0);
            t.setLevel(1, sc.lvl1);
            t.setFilterMode(FM::LowPass);
            t.setCutoff(sc.cutoff, sc.q);
            st.amp().setAttackMs(5.0f); st.amp().setDecayMs(30.0f); st.amp().setSustain(0.8f); st.amp().setReleaseMs(200.0f);
        }
        eng->reset();
        const int notes[5] = {48, 52, 55, 59, 62};
        for (int i = 0; i < sc.voices; ++i) eng->noteOn(notes[i], 0.8f);
        std::vector<float> buf(2 * 44100);
        for (int i = 0; i < 44100; ++i) { auto s = eng->processSample(); buf[2 * i] = s.left; buf[2 * i + 1] = s.right; }
        std::fwrite(buf.data(), 4, buf.size(), fp);
        std::printf("%s\n", sc.name);
        delete eng;
    }
    std::fclose(fp);
}
