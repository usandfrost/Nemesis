// Plays a project through the whole instrument on the host (the sequencer, the CATHODE parts, the sampler, the mix) and writes a
// 16-bit stereo WAV. Prints the level, the voices each part used, and how long a 32-sample block took to render here.
//   render_song <out.wav> [seconds] [solo part 0..2] [project.dbp]
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <vector>

#include <nemesis/dsp/CathodeOscCore.h>
#include <nemesis/dsp/ForkConfig.h>

#include "PresetHost.h"
#include "app/Model.h"
#include "app/ProjectFile.h"
#include "app/SampleLib.h"
#include "audio/Instrument.h"

using namespace app;

static void writeWav(const char* path, const std::vector<float>& l, const std::vector<float>& r, int rate) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    const uint32_t n = (uint32_t)l.size(), bytes = n * 4;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2); u32((uint32_t)rate);
    u32((uint32_t)rate * 4); u16(4); u16(16); fwrite("data", 1, 4, f); u32(bytes);
    for (uint32_t i = 0; i < n; ++i) {
        const int16_t a = (int16_t)lrintf(fmaxf(-1.f, fminf(1.f, l[i])) * 32767.f), b = (int16_t)lrintf(fmaxf(-1.f, fminf(1.f, r[i])) * 32767.f);
        fwrite(&a, 2, 1, f); fwrite(&b, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char** argv) {
    shippedPresets();   // the library, as the card would give it
    if (argc < 2) { fprintf(stderr, "usage: render_song <out.wav> [seconds] [solo part] [project.dbp]\n"); return 2; }
    const double seconds = argc > 2 ? atof(argv[2]) : 8.0;
    const int solo = argc > 3 ? atoi(argv[3]) : -1;
    const double sr = 44100.0;
    // the firmware's DSP settings (main.cpp)
    nemesis::dsp::CathodeOscCore::setLazyMacro(true);
    nemesis::dsp::fork::Config::idleMatrixSkip = true;
    nemesis::dsp::fork::Config::skipSilentSlots = true;
    nemesis::dsp::fork::Config::filterOversample = 2;
    nemesis::dsp::fork::Config::pdOversample = 4;
    nemesis::dsp::fork::Config::driveOversample = 2;
    nemesis::dsp::fork::Config::classicOversample = 0;
    nemesis::dsp::fork::Config::stereoTwin = true;
    nemesis::dsp::fork::Config::lanes = false;

    static Project pj;
    makeDefaultProject(pj);
    if (getenv("FACTORY")) makeFactoryProject(atoi(getenv("FACTORY")), pj);
    if (getenv("NOLOCKS")) {   // the same project with every step's locks taken off: what they do
        for (Pattern& p : pj.patterns)
            for (Lane& l : p.lanes)
                for (Step& st : l.st) st.nLocks = 0;
    }
    if (argc > 4) {   // a saved project file
        FILE* f = fopen(argv[4], "rb");
        if (!f) { perror(argv[4]); return 1; }
        std::vector<uint8_t> b;
        int c;
        while ((c = fgetc(f)) != EOF) b.push_back((uint8_t)c);
        fclose(f);
        struct Src : ByteSource { const std::vector<uint8_t>& b; size_t p = 0; explicit Src(const std::vector<uint8_t>& x) : b(x) {}
            bool read(void* d, size_t n) override { if (p + n > b.size()) return false; memcpy(d, b.data() + p, n); p += n; return true; } } src(b);
        static Project scratch;
        if (!readProject(src, scratch, pj)) { fprintf(stderr, "not a project file\n"); return 1; }
    }
    if (getenv("PLAIN")) {   // a probe: part A on its default pages, sustained, one long note per beat on lane 1, wave from PLAIN
        defaultSynthPages(pj.parts[0].pages);
        pj.parts[0].pages[PG_AMP][2] = 127;
        pj.parts[0].pages[PG_OSC1][0] = (int16_t)atoi(getenv("PLAIN"));
        pj.parts[0].pages[PG_FLT1][1] = 127;
        for (Lane& l : pj.patterns[0].lanes) l.mute = 1;
        Lane& a = pj.patterns[0].lanes[0];
        a.mute = 0; a.eu = 0; a.n = 4; a.root = 57;
        for (Step& st : a.st) { st.ov = 0; st.gate = 100; st.vel = 127; }
        a.st[0].ov = 1;
        a.level = 127;
    }
    if (solo >= 0)
        for (Pattern& p : pj.patterns)
            for (Lane& l : p.lanes) l.mute = l.part != solo;
    Transport tp;
    static SampleLib lib;
    std::vector<int16_t> store(kFactoryFrames);
    buildFactorySamples(lib, store.data(), store.size());

    static audio::Instrument inst;
    inst.init(sr, pj, tp, lib);
    tp.playing = 1;
    tp.playGen = 1;
    if (getenv("SONG")) {   // the project's first song from its first row, as SONG's PLAY does
        tp.songPlaying = 1;
        tp.songIdx = 0;
        tp.pat = pj.songs[0].rows[0].pat;
    }

    const int block = 32, blocks = (int)(seconds * sr / block);
    std::vector<float> L, R;
    L.reserve((size_t)blocks * block);
    R.reserve((size_t)blocks * block);
    float l[block], r[block];
    int maxActive[kParts] = {};
    double worstUs = 0, totalUs = 0;
    bool bad = false;
    for (int b = 0; b < blocks; ++b) {
        const auto t0 = std::chrono::steady_clock::now();
        inst.beginBlock();
        inst.render(l, r, block);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        totalUs += us;
        if (us > worstUs && b > 10) worstUs = us;
        for (int i = 0; i < block; ++i) {
            if (!std::isfinite(l[i]) || !std::isfinite(r[i])) bad = true;
            L.push_back(l[i]);
            R.push_back(r[i]);
        }
        for (int p = 0; p < kParts; ++p) if (inst.activeVoices(p) > maxActive[p]) maxActive[p] = inst.activeVoices(p);
    }
    double peak = 0, sum = 0;
    for (size_t i = 0; i < L.size(); ++i) {
        peak = fmax(peak, fmax(fabs(L[i]), fabs(R[i])));
        sum += L[i] * L[i] + R[i] * R[i];
    }
    const double rms = sqrt(sum / (2.0 * L.size()));
    writeWav(argv[1], L, R, (int)sr);
    printf("%s: %.1f s, peak %.3f (%.1f dBFS), rms %.4f (%.1f dBFS), %s\n", argv[1], seconds, peak, 20 * log10(peak + 1e-12), rms,
           20 * log10(rms + 1e-12), bad ? "NaN/Inf FOUND" : "all finite");
    printf("voices at most: A %d, B %d, C %d\n", maxActive[0], maxActive[1], maxActive[2]);
    printf("host render: %.1f us per 32-sample block on average, %.1f us worst (the block is %.0f us long)\n", totalUs / blocks, worstUs,
           block / sr * 1e6);
    return bad ? 1 : 0;
}
