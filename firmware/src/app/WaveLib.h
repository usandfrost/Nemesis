#ifndef __APP_WAVE_LIB_H
#define __APP_WAVE_LIB_H

#include <stdint.h>

#include "app/Fmt.h"

// The wavetables the OSC pages can pick (docs/PRESETS_AND_WAVETABLES.md). An oscillator's WAVE knob holds 0..3 (SIN, TRI, SAW, SQR: computed at
// boot) or 4 + a catalogue number 0..199 (a factory bank of the original CATHODE: a file on the SD card). The number is stable: a project that
// names bank 77 means bank 77 on any card. A bank whose file is not on the card shows its name with a "?" and plays the basic wave it is nearest to.
namespace app {

constexpr int kBasicWaves = 4;
constexpr int kWaveBanks = 200;
constexpr int kWaveMax = kBasicWaves + kWaveBanks - 1;   // the WAVE knob's top value

const char* waveBankName(int bank);          // "TRI CIRCLE"
int waveBankNearest(int bank);               // the basic wave 0..3 it plays when its file is missing
int waveBankCategory(int bank);              // 0 basic, 1 vocal, 2 metallic, 3 harmonic, 4 chip, 5 complex
const char* waveCategoryName(int cat);

// Which banks the card holds (set at boot from the card's index, and by tests).
struct WaveSet {
    bool present[kWaveBanks] = {};
    int count() const { int n = 0; for (bool b : present) n += b; return n; }
};
WaveSet& waveSet();

// WAVE knob value -> text ("SAW", "TRI CIRCLE", "TRI CIRCLE ?"), and the step through what the card has (basic waves first, then the banks present).
void formatWave(Text& out, int v);
int stepWave(int v, int dir);                // one step up (dir > 0) or down; stays put at the ends; a value on a missing bank steps to a present one
bool waveIsBank(int v);                      // v >= 4
inline int waveBankOf(int v) { return v - kBasicWaves; }

}  // namespace app

#endif
