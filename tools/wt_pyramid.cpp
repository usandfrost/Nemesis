// Host tool: turns the exported wavetable files (content/data/wavetables/wtNNN.wt, DBWT v1: int16 frames) into the files the Drumboy loads
// (wtNNN.wtp, DBWP v1): the band-limited, decimated mip pyramid already built, as 16-bit samples and a scale. The board then reads a file and
// runs no FFT. The pyramid is built by the fork's own Wavetable::build (decimated), so what the board plays is what this tool made.
//
//   wt_pyramid <in dir> <out dir> [catalogue index ...]      (no indices: every wtNNN.wt in the directory)
//
// DBWP v1, little-endian: 0 "DBWP", 4 u16 version 1, 6 u16 flags (bit 0: decimated), 8 u16 frames, 10 u16 frame length, 12 u16 catalogue index,
// 14 u16 mips, 16 f32 scale (sample = int16 * scale), 20 u32 payload bytes, 24 u32 CRC-32 of the payload, 28 u32 reserved, 32 char[32] bank id,
// 64 payload: frames x frameSamples int16, frame-major, each frame its mips from the full-band one down (lengths: Wavetable::mipLength).
#include <nemesis/dsp/WavetableOscillator.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

static uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; ++i) { c ^= p[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
static void put16(uint8_t* h, int o, uint32_t v) { h[o] = (uint8_t)v; h[o + 1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* h, int o, uint32_t v) { for (int i = 0; i < 4; ++i) h[o + i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get16(const uint8_t* h, int o) { return h[o] | (h[o + 1] << 8); }
static uint32_t get32(const uint8_t* h, int o) { return get16(h, o) | (get16(h, o + 2) << 16); }

static bool convert(const std::string& in, const std::string& out) {
    FILE* f = fopen(in.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    if (b.size() < 64 || memcmp(b.data(), "DBWT", 4) != 0 || get16(b.data(), 4) != 1 || get16(b.data(), 6) != 1) { fprintf(stderr, "%s: not a DBWT v1 file\n", in.c_str()); return false; }
    const int frames = (int)get16(b.data(), 8), L = (int)get16(b.data(), 10), cat = (int)get16(b.data(), 14);
    if ((size_t)frames * L * 2 + 64 != b.size() || (get32(b.data(), 20) != (uint32_t)(frames * L * 2))) { fprintf(stderr, "%s: size mismatch\n", in.c_str()); return false; }
    std::vector<float> fr((size_t)frames * L);
    for (size_t i = 0; i < fr.size(); ++i) fr[i] = (float)(int16_t)get16(b.data() + 64, (int)(2 * i)) / 32768.0f;
    nemesis::dsp::Wavetable wt;
    wt.build(fr.data(), frames, L, true);
    float mx = 0;
    for (size_t i = 0; i < wt.rawSize(); ++i) mx = fmaxf(mx, fabsf((float)wt.raw()[i]));
    const float scale = mx > 0 ? mx / 32767.0f : 1.0f;
    std::vector<uint8_t> o(64 + wt.rawSize() * 2, 0);
    memcpy(o.data(), "DBWP", 4);
    put16(o.data(), 4, 1);
    put16(o.data(), 6, 1);
    put16(o.data(), 8, (uint32_t)frames);
    put16(o.data(), 10, (uint32_t)L);
    put16(o.data(), 12, (uint32_t)cat);
    put16(o.data(), 14, (uint32_t)wt.numMips());
    memcpy(o.data() + 16, &scale, 4);
    put32(o.data(), 20, (uint32_t)(wt.rawSize() * 2));
    memcpy(o.data() + 32, b.data() + 32, 32);   // the bank id
    for (size_t i = 0; i < wt.rawSize(); ++i) {
        long q = lrintf((float)wt.raw()[i] / scale);
        q = q > 32767 ? 32767 : q < -32768 ? -32768 : q;
        put16(o.data() + 64, (int)(2 * i), (uint32_t)(int16_t)q);
    }
    put32(o.data(), 24, crc32(o.data() + 64, wt.rawSize() * 2));
    FILE* w = fopen(out.c_str(), "wb");
    if (!w) return false;
    const bool ok = fwrite(o.data(), 1, o.size(), w) == o.size();
    fclose(w);
    printf("%s -> %s: %d frames x %zu samples, %zu bytes (scale %g)\n", in.c_str(), out.c_str(), frames, wt.frameSamples(), o.size(), scale);
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: wt_pyramid <in dir> <out dir> [catalogue index ...]\n"); return 2; }
    int fails = 0, done = 0;
    for (int i = 0; i < 200; ++i) {
        bool want = argc == 3;
        for (int a = 3; a < argc; ++a) want = want || atoi(argv[a]) == i;
        if (!want) continue;
        char n[32];
        snprintf(n, sizeof n, "wt%03d", i);
        FILE* t = fopen((std::string(argv[1]) + "/" + n + ".wt").c_str(), "rb");
        if (!t) { if (argc > 3) { fprintf(stderr, "no %s.wt\n", n); ++fails; } continue; }
        fclose(t);
        if (convert(std::string(argv[1]) + "/" + n + ".wt", std::string(argv[2]) + "/" + n + ".wtp")) ++done; else ++fails;
    }
    printf("%d converted, %d failed\n", done, fails);
    return fails ? 1 : 0;
}
