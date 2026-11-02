// Which matrix slots are active in a default voice, and which destinations does that make live?
#include <nemesis/dsp/CathodeVoiceEngine.h>
#include <cstdio>
using namespace nemesis::dsp;
int main() {
    auto* eng = new CathodeVoiceEngine;
    eng->prepare(44100.0);
    eng->noteOn(60, 0.8f);
    for (int i = 0; i < 100; ++i) (void)eng->processSample();
    auto& m = eng->voiceAt(0).matrix();
    int active = 0;
    for (int i = 0; i < 40; ++i) if (m.slotActive(i)) { ++active; std::printf("slot %d active: dest %d depth %g\n", i, (int)m.slotDest(i), (double)m.slotDepth(i)); }
    std::printf("active slots in a default voice: %d\n", active);
}
