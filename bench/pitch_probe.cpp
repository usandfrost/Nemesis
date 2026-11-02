// Relative error of the engine's note->Hz law, printed per note. Build it against double and float DSP, compare.
#include <nemesis/dsp/CathodeVoiceEngine.h>

#include <cstdio>

int main() {
    for (int n = 0; n <= 127; n += 7)
        std::printf("%d %.17g\n", n, (double)nemesis::dsp::CathodeVoiceEngine::midiNoteToHz(n));
}
