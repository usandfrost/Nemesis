// wt_export: the upstream wavetable catalogue (200 factory banks, all procedural kernels) as data for the Drumboy.
//   wt_export index <catalogue.json>                       one entry per bank: id, label, category, frames, spectral probes
//   wt_export write <outdir> <frames> <idx> [<idx> ...]    one .wt file per listed bank (format: content/data/wavetables/FORMAT.md)
// Built against upstream's sdk/dsp sources (the same kernels the plugin runs); never edits upstream. The catalogue order is
// sdk/core/src/WavetableCatalogue.cpp buildCatalogue(): Basic, Vocal, Metallic, Harmonic, Chip, Mask-complex (3), Complex.
#include <nemesis/dsp/BasicTableGenerator.h>
#include <nemesis/dsp/ChipTableGenerator.h>
#include <nemesis/dsp/ComplexTableGenerator.h>
#include <nemesis/dsp/HarmonicTableGenerator.h>
#include <nemesis/dsp/MaskTableGenerator.h>
#include <nemesis/dsp/MetallicTableGenerator.h>
#include <nemesis/dsp/VocalTableGenerator.h>
#include <nemesis/dsp/WavetableBank.h>
#include <nemesis/dsp/WavetableRecipe.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <functional>
#include <string>
#include <vector>

using namespace nemesis::dsp;

namespace {
constexpr int kLen = WavetableBank::kFrameLength;   // 2048

struct Bank {
    std::string id, label;
    int category = 0, frames = 0;
    std::function<void(int, float*)> render;
};

template <class Gen>
void add(std::vector<Bank>& v) {
    for (int i = 0; i < Gen::kBankCount; ++i) {
        const auto b = static_cast<typename Gen::Bank>(i);
        const WavetableRecipe& r = Gen::recipeFor(b);
        v.push_back({std::string(r.id), std::string(r.label), (int)r.category, r.frames, [b](int f, float* o) { Gen::renderFrame(b, f, o); }});
    }
}

std::vector<Bank> catalogue() {
    std::vector<Bank> v;
    add<BasicTableGenerator>(v);
    add<VocalTableGenerator>(v);
    add<MetallicTableGenerator>(v);
    add<HarmonicTableGenerator>(v);
    add<ChipTableGenerator>(v);
    for (const MaskTableGenerator::Law law : {MaskTableGenerator::Law::Prism, MaskTableGenerator::Law::Shatter, MaskTableGenerator::Law::Weave}) {
        const WavetableRecipe* r = MaskTableGenerator::recipeFor(law);
        if (!r) continue;
        v.push_back({std::string(r->id), std::string(r->label), (int)r->category, r->frames, [law](int f, float* o) { MaskTableGenerator::renderFrame(law, f, o); }});
    }
    add<ComplexTableGenerator>(v);
    return v;
}

// magnitudes (dB re the strongest) of harmonics 1..N by direct DFT
constexpr int kH = 24;
void spectrum(const float* x, double* db) {
    double mag[kH];
    double mx = 1e-12;
    for (int h = 1; h <= kH; ++h) {
        double re = 0, im = 0;
        for (int n = 0; n < kLen; ++n) {
            const double a = 2.0 * M_PI * h * n / kLen;
            re += x[n] * cos(a);
            im += x[n] * sin(a);
        }
        mag[h - 1] = sqrt(re * re + im * im) / kLen;
        if (mag[h - 1] > mx) mx = mag[h - 1];
    }
    for (int h = 0; h < kH; ++h) db[h] = fmax(-60.0, 20.0 * log10(mag[h] / mx + 1e-12));
}

// the Drumboy's WAVE shapes as ideal spectra
void ideal(int shape, double* db) {   // 0 SAW 1 SQR 2 TRI 3 SIN
    for (int h = 1; h <= kH; ++h) {
        double m = 0;
        switch (shape) {
            case 0: m = 1.0 / h; break;
            case 1: m = (h & 1) ? 1.0 / h : 0; break;
            case 2: m = (h & 1) ? 1.0 / (h * h) : 0; break;
            default: m = h == 1 ? 1 : 0; break;
        }
        db[h - 1] = fmax(-60.0, 20.0 * log10(m + 1e-12));
    }
}

// shape distance (dB RMS after removing the mean offset) -> similarity 0..1
double sim(const double* a, const double* b) {
    double off = 0;
    for (int h = 0; h < kH; ++h) off += a[h] - b[h];
    off /= kH;
    double s = 0;
    for (int h = 0; h < kH; ++h) s += (a[h] - b[h] - off) * (a[h] - b[h] - off);
    return fmax(0.0, 1.0 - sqrt(s / kH) / 24.0);
}

double centroid(const float* x) {   // in harmonics, power weighted, over 1..64
    double num = 0, den = 1e-18;
    for (int h = 1; h <= 64; ++h) {
        double re = 0, im = 0;
        for (int n = 0; n < kLen; ++n) {
            const double a = 2.0 * M_PI * h * n / kLen;
            re += x[n] * cos(a);
            im += x[n] * sin(a);
        }
        const double p = re * re + im * im;
        num += h * p;
        den += p;
    }
    return num / den;
}

uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: wt_export index <out.json> | write <outdir> <frames> <idx>...\n"); return 2; }
    const std::vector<Bank> bank = catalogue();
    const std::string mode = argv[1];
    if (mode == "index") {
        FILE* f = fopen(argv[2], "w");
        fprintf(f, "[\n");
        static const char* const kShape[4] = {"SAW", "SQR", "TRI", "SIN"};
        std::vector<float> fr(kLen);
        for (size_t i = 0; i < bank.size(); ++i) {
            const Bank& b = bank[i];
            fprintf(f, " {\"index\":%zu,\"id\":\"%s\",\"label\":\"%s\",\"category\":%d,\"frames\":%d,\"probe\":[", i, b.id.c_str(), b.label.c_str(), b.category, b.frames);
            for (int q = 0; q < 5; ++q) {   // five positions across the bank
                const int frame = b.frames <= 1 ? 0 : q * (b.frames - 1) / 4;
                b.render(frame, fr.data());
                double db[kH], ref[kH];
                spectrum(fr.data(), db);
                int best = 0;
                double bs = -1;
                double sims[4];
                for (int s = 0; s < 4; ++s) {
                    ideal(s, ref);
                    sims[s] = sim(db, ref);
                    if (sims[s] > bs) { bs = sims[s]; best = s; }
                }
                double pk = 0;
                for (int n = 0; n < kLen; ++n) pk = fmax(pk, fabs(fr[n]));
                fprintf(f, "%s{\"frame\":%d,\"best\":\"%s\",\"sim\":%.3f,\"simSAW\":%.3f,\"simSQR\":%.3f,\"simTRI\":%.3f,\"simSIN\":%.3f,\"centroid\":%.2f,\"peak\":%.3f}", q ? "," : "",
                        frame, kShape[best], bs, sims[0], sims[1], sims[2], sims[3], centroid(fr.data()), pk);
            }
            fprintf(f, "]}%s\n", i + 1 < bank.size() ? "," : "");
        }
        fprintf(f, "]\n");
        fclose(f);
        printf("%zu banks\n", bank.size());
        return 0;
    }
    if (mode == "write" && argc >= 5) {
        const std::string dir = argv[2];
        const int want = atoi(argv[3]);
        for (int a = 4; a < argc; ++a) {
            const int idx = atoi(argv[a]);
            if (idx < 0 || idx >= (int)bank.size()) { fprintf(stderr, "bad index %d\n", idx); return 1; }
            const Bank& b = bank[idx];
            const int n = want < b.frames ? want : b.frames;   // never more frames than the bank has
            std::vector<int16_t> pay((size_t)n * kLen);
            std::vector<float> fr(kLen);
            for (int k = 0; k < n; ++k) {
                const int src = n <= 1 ? 0 : (int)lround((double)k * (b.frames - 1) / (n - 1));   // evenly spaced source frames, ends kept
                b.render(src, fr.data());
                for (int s = 0; s < kLen; ++s) {
                    const float v = fmaxf(-1.f, fminf(1.f, fr[s]));
                    pay[(size_t)k * kLen + s] = (int16_t)lrintf(v * 32767.f);
                }
            }
            uint8_t h[64] = {};
            memcpy(h, "DBWT", 4);
            auto u16 = [&](int o, uint16_t v) { h[o] = v & 255; h[o + 1] = v >> 8; };
            auto u32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; ++i) h[o + i] = (v >> (8 * i)) & 255; };
            u16(4, 1); u16(6, 1); u16(8, (uint16_t)n); u16(10, kLen); u16(12, (uint16_t)b.frames); u16(14, (uint16_t)idx);
            h[16] = (uint8_t)b.category; h[17] = 0;
            u32(20, (uint32_t)(pay.size() * 2));
            u32(24, crc32((const uint8_t*)pay.data(), pay.size() * 2));
            strncpy((char*)h + 32, b.id.c_str(), 31);
            char path[512];
            snprintf(path, sizeof path, "%s/wt%03d.wt", dir.c_str(), idx);
            FILE* f = fopen(path, "wb");
            if (!f) { perror(path); return 1; }
            fwrite(h, 1, 64, f);
            fwrite(pay.data(), 2, pay.size(), f);   // little-endian host (arm64 / x86)
            fclose(f);
        }
        return 0;
    }
    return 2;
}
