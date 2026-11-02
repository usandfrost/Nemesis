#include "app/WaveLib.h"

namespace app {

struct BankInfo { const char* name; uint8_t nearest, cat; };
static const BankInfo kBanks[kWaveBanks] = {
#include "app/WaveNames.inc"
};
static const char* const kBasic[kBasicWaves] = {"SIN", "TRI", "SAW", "SQR"};
static const char* const kCats[6] = {"BASIC", "VOCAL", "METALLIC", "HARMONIC", "CHIP", "COMPLEX"};

const char* waveBankName(int b) { return b >= 0 && b < kWaveBanks ? kBanks[b].name : ""; }
int waveBankNearest(int b) { return b >= 0 && b < kWaveBanks ? kBanks[b].nearest : 2; }
int waveBankCategory(int b) { return b >= 0 && b < kWaveBanks ? kBanks[b].cat : 0; }
const char* waveCategoryName(int c) { return c >= 0 && c < 6 ? kCats[c] : ""; }

WaveSet& waveSet() {
    static WaveSet s;
    return s;
}

bool waveIsBank(int v) { return v >= kBasicWaves; }

void formatWave(Text& out, int v) {
    if (v < 0) v = 0;
    if (v < kBasicWaves) { out.add(kBasic[v]); return; }
    const int b = v - kBasicWaves;
    out.add(waveBankName(b));
    if (b >= kWaveBanks || !waveSet().present[b]) out.add(" ?");
}

int stepWave(int v, int dir) {
    if (v < 0) v = 0;
    if (v > kWaveMax) v = kWaveMax;
    const int s = dir > 0 ? 1 : -1;
    int n = v + s;
    while (n >= kBasicWaves && n <= kWaveMax && !waveSet().present[n - kBasicWaves]) n += s;
    if (n < 0) return 0;
    if (n > kWaveMax) {   // past the last present bank: stay (a value on a missing bank steps back to the nearest present one)
        if (v >= kBasicWaves && waveSet().present[v - kBasicWaves]) return v;
        for (int k = kWaveMax; k >= kBasicWaves; --k) if (waveSet().present[k - kBasicWaves]) return k;
        return kBasicWaves - 1;
    }
    return n;
}

}  // namespace app
