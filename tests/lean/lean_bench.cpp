// The lean voice on the host: plays a library preset's notes through lean::Part and times it (ns per sample per voice), writing a 16-bit WAV.
//   lean_bench PRESET_ID NOTES(comma) SECONDS OUT.wav [VOICE_CAP]
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include <nemesis/dsp/CathodeOscCore.h>
#include "PresetHost.h"
#include "app/Model.h"
#include "audio/PatchMap.h"
#include "lean/LeanPart.h"

static void writeWav(const char* path, const std::vector<float>& l, const std::vector<float>& r) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    const uint32_t n = (uint32_t)l.size(), bytes = n * 4;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2); u32(44100); u32(44100 * 4); u16(4); u16(16);
    fwrite("data", 1, 4, f); u32(bytes);
    for (uint32_t i = 0; i < n; ++i) {
        const int16_t a = (int16_t)lrintf(fmaxf(-1.f, fminf(1.f, l[i])) * 32767.f), b = (int16_t)lrintf(fmaxf(-1.f, fminf(1.f, r[i])) * 32767.f);
        fwrite(&a, 2, 1, f); fwrite(&b, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: lean_bench PRESET_ID NOTES SECONDS OUT.wav [CAP]\n"); return 2; }
    shippedPresets();
    const int id = atoi(argv[1]);
    std::vector<int> notes;
    for (char* t = strtok(argv[2], ","); t; t = strtok(nullptr, ",")) notes.push_back(atoi(t));
    const double seconds = atof(argv[3]);
    static audio::SharedTables tables;
    tables.build();
    static app::Part part;
    memset(&part, 0, sizeof part);
    app::defaultSynthPages(part.pages);
    if (id >= 0) { if (!app::presetLoad(id) || !app::applyPresetIndex(part.pages, id)) { fprintf(stderr, "no preset %d\n", id); return 1; } }
    audio::Patch patch;
    audio::makePatch(part, nullptr, 0, 0, patch);
    lean::Params prm;
    lean::makeParams(patch, tables, prm);
    lean::Part lp;
    lp.init(44100.0f, tables);
    if (argc > 5) lp.setVoices(atoi(argv[5]));
    for (int n : notes) lp.noteOn(n, 0.8f, prm);
    const int blocks = (int)(seconds * 44100.0 / 32);
    std::vector<float> L, R;
    float l[32], r[32];
    double totalNs = 0, voiceSamples = 0;
    for (int b = 0; b < blocks; ++b) {
        if (b == blocks * 3 / 4) for (int n : notes) lp.noteOff(n);
        memset(l, 0, sizeof l); memset(r, 0, sizeof r);
        const auto t0 = std::chrono::steady_clock::now();
        const int act = lp.activeVoices();
        lp.render(l, r, 32);
        totalNs += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        voiceSamples += 32.0 * act;
        for (int i = 0; i < 32; ++i) { L.push_back(l[i]); R.push_back(r[i]); }
    }
    double peak = 0, sum = 0;
    for (size_t i = 0; i < L.size(); ++i) { peak = fmax(peak, fmax(fabs(L[i]), fabs(R[i]))); sum += L[i] * L[i] + R[i] * R[i]; }
    printf("preset %d, %zu notes: peak %.3f rms %.4f | %.1f ns per voice-sample (%.0f voice-samples)\n", id, notes.size(), peak, sqrt(sum / (2 * L.size())), voiceSamples > 0 ? totalNs / voiceSamples : 0.0, voiceSamples);
    writeWav(argv[4], L, R);
    return 0;
}
