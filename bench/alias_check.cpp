// How much does lowering the oversampling factor cost in quality? Measures the ALIAS FLOOR: energy that is not at a harmonic of the
// note, as a fraction of the total, for one voice playing a bright tone whose fundamental sits exactly on an FFT bin.
// Harmonics above Nyquist fold back to inharmonic frequencies (fs/f0 = 16.384 is not an integer), so anything between the harmonics
// is aliasing (plus the converter's own noise floor). Lower floor = cleaner.
#include <nemesis/dsp/CathodeVoiceEngine.h>
#include <nemesis/dsp/ForkConfig.h>

#include <cmath>
#include <cstdlib>
#include <complex>
#include <cstdio>
#include <vector>

using namespace nemesis::dsp;
using OM = CathodeVoiceStage::OscMode;
using FM = CathodeVoiceStage::FilterMode;

static void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / (double)len; const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) { std::complex<double> w(1); for (size_t k = 0; k < len / 2; ++k) { auto u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; } }
    }
}

// returns the alias floor in dB relative to the total energy
static int gDriveOs = 4;
static bool gBits24 = false;   // ALIAS_BITS24=1: converter at 24 bits, so the floor is the DSP's own, not the 8-bit grit
static double aliasFloor(int pdOs, int fOs, FM model, float cutoff, float q, float drive, int bin) {
    fork::Config::pdOversample = pdOs; fork::Config::filterOversample = fOs;
#ifdef NEMESIS_FORK_EXACT
    fork::Config::driveOversample = gDriveOs; fork::Config::lanes = false;
#endif
    const int N = 16384; const double fs = 44100.0, f0 = bin * fs / N;
    auto* eng = new CathodeVoiceEngine; eng->prepare(fs); eng->setVoiceCount(1); eng->setLaneSitesForTest(0u);
    auto& mv = eng->voiceAt(0); auto& t = mv.outputStage().voice();
    t.setOscMode(0, OM::PhaseDistortion); t.setPdShape(0, 0.12f); t.setPdAmount(0, 0.95f); t.setLevel(0, 1.0f);
    t.setOscMode(1, OM::PhaseDistortion); t.setLevel(1, 0.0f);
    t.setFilterMode(model); t.setCutoff(cutoff, q); if (drive > 0) t.setDrive(drive);
    if (gBits24) t.setBitDepth(24.0f);
    mv.outputStage().amp().setAttackMs(1.0f); mv.outputStage().amp().setSustain(1.0f);
    eng->reset(); eng->noteOn(60, 0.9f); mv.setNoteHz(f0);
    for (int i = 0; i < 8000; ++i) (void)eng->processSample();
    std::vector<std::complex<double>> x(N);
    for (int i = 0; i < N; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N); x[i] = w * eng->processSample().left; }
    delete eng;
    fft(x);
    std::vector<bool> harmonic(N / 2 + 1, false);
    for (int h = 1; h * bin <= N / 2; ++h) for (int d = -4; d <= 4; ++d) { const int k = h * bin + d; if (k >= 0 && k <= N / 2) harmonic[k] = true; }
    for (int k = 0; k <= 3; ++k) harmonic[k] = true;
    double eAll = 0, eAlias = 0;
    for (int k = 1; k <= N / 2; ++k) { const double e = std::norm(x[k]); eAll += e; if (!harmonic[k]) eAlias += e; }
    return 10 * std::log10(eAlias / eAll);
}

int main() {
    gBits24 = std::getenv("ALIAS_BITS24") != nullptr;
    const int bin = 1000;   // f0 = 2691.9 Hz: harmonics fold back inharmonically
    std::printf("alias floor, dB re total energy (lower = cleaner), tone at %.1f Hz\n\n", bin * 44100.0 / 16384);
    std::printf("phase-distortion oscillator, filter wide open:\n");
    for (int os : {8, 4, 2, 1}) std::printf("  PD oversample x%d : %7.1f dB\n", os, aliasFloor(os, 4, FM::LowPass, 19000.0f, 0.7f, 0.0f, bin));
    std::printf("\nClassic LP24 core (cutoff 6 kHz, resonance 6, drive 0.7), PD x4:\n");
    for (int os : {8, 4, 2}) std::printf("  filter oversample x%d : %7.1f dB\n", os, aliasFloor(4, os, FM::ClassicLp24, 6000.0f, 6.0f, 0.7f, bin));
    std::printf("\nAcid core (cutoff 5 kHz, resonance 6), PD x4:\n");
    for (int os : {8, 4, 2}) std::printf("  filter oversample x%d : %7.1f dB\n", os, aliasFloor(4, os, FM::Acid, 5000.0f, 6.0f, 0.0f, bin));
#ifdef NEMESIS_FORK_EXACT
    std::printf("\nLadder24 with pre drive 0.7 (cutoff 6 kHz, resonance 6), PD x4, Classic x2:\n");
    for (int os : {4, 2}) { gDriveOs = os; std::printf("  drive/ladder oversample x%d : %7.1f dB\n", os, aliasFloor(4, 2, FM::Ladder24, 6000.0f, 6.0f, 0.7f, bin)); }
    std::printf("\nLowPass with pre drive 0.7 (the separate FilterDrive), PD x4:\n");
    for (int os : {4, 2}) { gDriveOs = os; std::printf("  drive oversample x%d : %7.1f dB\n", os, aliasFloor(4, 2, FM::LowPass, 6000.0f, 3.0f, 0.7f, bin)); }
    gDriveOs = 4;
#endif
    std::printf("\nboth at x2 (what the firmware would run), Classic:  %7.1f dB   (upstream x4/x4: %7.1f dB)\n",
                aliasFloor(2, 2, FM::ClassicLp24, 6000.0f, 6.0f, 0.7f, bin), aliasFloor(4, 4, FM::ClassicLp24, 6000.0f, 6.0f, 0.7f, bin));
}
