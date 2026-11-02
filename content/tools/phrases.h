// Shared by render_presets (the Drumboy instrument) and upstream_render (the original CATHODE machine): the same phrase per category, the same
// figures, the same loudness normalisation, so an A/B pair differs only by the engine that played it.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <complex>
#include <initializer_list>
#include <string>
#include <vector>

namespace phr {
constexpr double kSr = 44100.0;
// ---- phrases: (start s, MIDI note, duration s, velocity 0..1)
struct Note { double t; int n; double d; float v; };
struct Phrase { std::vector<Note> notes; double len; const char* name; };

inline Phrase phraseFor(const char* cat) {
    Phrase p;
    auto add = [&](double t, int n, double d, float v) { p.notes.push_back({t, n, d, v}); };
    auto chord = [&](double t, std::initializer_list<int> ns, double d, float v) { int k = 0; for (int n : ns) add(t + 0.004 * k++, n, d, v); };
    const std::string c = cat;
    if (c == "BASS") {   // a riff: root, ghost notes, a fifth, a long note
        p.name = "bass line";
        add(0.00, 36, 0.35, 0.95); add(0.45, 36, 0.12, 0.6); add(0.60, 36, 0.12, 0.75); add(0.90, 43, 0.25, 0.9); add(1.25, 41, 0.25, 0.85);
        add(1.60, 36, 0.12, 0.5); add(1.80, 39, 0.35, 1.0); add(2.30, 34, 0.30, 0.8); add(2.70, 36, 1.00, 0.95);
        p.len = 4.6;
    } else if (c == "LEAD") {
        p.name = "lead line";
        add(0.00, 67, 0.40, 0.8); add(0.45, 72, 0.25, 0.9); add(0.75, 71, 0.25, 0.7); add(1.05, 67, 0.40, 0.85); add(1.55, 69, 0.20, 0.75);
        add(1.80, 72, 0.20, 0.95); add(2.05, 74, 1.30, 1.0);
        p.len = 4.6;
    } else if (c == "PAD" || c == "TEXTURE") {
        p.name = "held chord";
        chord(0.0, {48, 55, 63, 70}, 3.0, 0.8);
        p.len = 5.0;
    } else if (c == "POLY") {
        p.name = "chord progression";
        chord(0.0, {48, 55, 63, 70}, 1.0, 0.85); chord(1.2, {44, 51, 60, 67}, 1.0, 0.85); chord(2.4, {46, 53, 62, 65}, 1.4, 0.9);
        p.len = 4.8;
    } else if (c == "KEYS") {
        p.name = "keys phrase";
        chord(0.0, {48, 52, 55, 59}, 0.6, 0.8); add(0.8, 67, 0.3, 0.9); add(1.1, 71, 0.3, 0.7); add(1.4, 72, 0.5, 0.85);
        chord(2.2, {45, 52, 57, 60}, 0.6, 0.8); add(3.0, 64, 0.3, 0.8); add(3.3, 60, 0.9, 0.9);
        p.len = 4.8;
    } else if (c == "PLUCK") {
        p.name = "pluck arpeggio";
        const int seq[8] = {60, 64, 67, 71, 72, 71, 67, 64};
        for (int i = 0; i < 16; ++i) add(0.17 * i, seq[i % 8] + (i >= 8 ? 5 : 0), 0.10, 0.6f + 0.35f * ((i % 4) == 0));
        p.len = 4.2;
    } else if (c == "STAB") {
        p.name = "chord stabs";
        chord(0.0, {48, 55, 63, 70}, 0.14, 0.95); chord(0.5, {48, 55, 63, 70}, 0.14, 0.7); chord(1.0, {51, 58, 65, 70}, 0.14, 0.95);
        chord(1.5, {48, 55, 63, 70}, 0.14, 0.8); chord(2.25, {46, 53, 62, 65}, 0.14, 1.0); chord(2.75, {48, 55, 63, 70}, 0.3, 0.9);
        p.len = 4.0;
    } else if (c == "BRASS") {
        p.name = "brass stabs and a long note";
        chord(0.0, {53, 60, 65, 69}, 0.45, 0.9); chord(0.7, {55, 62, 67, 70}, 0.45, 0.95); chord(1.5, {53, 60, 65, 69}, 1.8, 0.9);
        p.len = 4.4;
    } else if (c == "BELL") {
        p.name = "bell notes";
        add(0.0, 72, 1.2, 0.8); add(0.9, 76, 1.0, 0.7); add(1.7, 79, 1.0, 0.8); add(2.6, 84, 1.0, 0.65);
        p.len = 5.0;
    } else if (c == "SEQ") {
        p.name = "sequence";
        const int seq[8] = {48, 48, 60, 48, 51, 63, 55, 58};
        for (int i = 0; i < 24; ++i) add(0.125 * i, seq[i % 8], 0.08, 0.6f + 0.3f * ((i % 4) == 0));
        p.len = 3.8;
    } else {   // FX and anything else: a held note, then a low hit
        p.name = "held note and a hit";
        add(0.0, 60, 2.0, 0.9); add(2.6, 36, 0.4, 1.0);
        p.len = 4.6;
    }
    return p;
}

// ---- figures
inline void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / len;
        const std::complex<double> wl(cos(ang), sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

inline void writeWav(const char* path, const std::vector<float>& x) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    const uint32_t n = (uint32_t)x.size(), bytes = n * 2, rate = (uint32_t)kSr;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(bytes);
    for (float v : x) { const int16_t s = (int16_t)lrintf(fmaxf(-1.f, fminf(1.f, v)) * 32767.f); fwrite(&s, 2, 1, f); }
    fclose(f);
}


// Figures of a raw mono render and the normalised copy (-20 dBFS RMS over the active region, peak <= -1 dBFS): one JSON object body.
struct Figures { std::vector<float> norm; std::string json; };
inline Figures analyse(const std::vector<float>& mono, bool finite, int idx, const char* name, const char* cat, const char* phrase) {
    const int n = (int)mono.size();
    double sum = 0, pk = 0, dc = 0;
    int clipped = 0;
    for (float x : mono) { sum += (double)x * x; pk = fmax(pk, fabs(x)); dc += x; if (fabs(x) >= 0.999f) ++clipped; }
    const int fl = (int)(0.05 * kSr);
    std::vector<double> env;
    for (int s = 0; s + fl <= n; s += fl) { double q = 0; for (int k = 0; k < fl; ++k) q += (double)mono[s + k] * mono[s + k]; env.push_back(sqrt(q / fl)); }
    double emax = 1e-12;
    for (double v : env) emax = fmax(emax, v);
    double aSum = 0; int aN = 0;
    for (double v : env) if (v > emax * 0.0316) { aSum += v * v; ++aN; }
    const double rmsActive = aN ? sqrt(aSum / aN) : 0.0;
    int fAtt = 0; while (fAtt < (int)env.size() && env[fAtt] < 0.9 * emax) ++fAtt;
    size_t best = 0; double bv = -1;
    for (size_t s = 0; s + 8192 <= mono.size(); s += 2048) { double q = 0; for (int k = 0; k < 8192; ++k) q += (double)mono[s + k] * mono[s + k]; if (q > bv) { bv = q; best = s; } }
    std::vector<std::complex<double>> a(8192);
    for (int k = 0; k < 8192; ++k) a[k] = (best + k < mono.size() ? mono[best + k] : 0.f) * (0.5 - 0.5 * cos(2 * M_PI * k / 8192));
    fft(a);
    double cn = 0, cd = 1e-18, lo = 0, mid = 0, hi = 0, hz = kSr / 8192;
    for (int k = 2; k < 4096; ++k) {
        const double pw = std::norm(a[k]), f = k * hz;
        cn += f * pw; cd += pw;
        (f < 250 ? lo : f < 4000 ? mid : hi) += pw;
    }
    const double all = lo + mid + hi + 1e-18;
    const double rawDb = 20 * log10(rmsActive + 1e-12);
    double gain = pow(10.0, (-20.0 - rawDb) / 20.0);
    if (pk * gain > 0.89) gain = 0.89 / pk;
    if (gain > 31.6) gain = 31.6;
    Figures f;
    f.norm.resize(mono.size());
    for (size_t k = 0; k < mono.size(); ++k) f.norm[k] = (float)(mono[k] * gain);
    for (int k = 0; k < 441 && k < (int)f.norm.size(); ++k) f.norm[f.norm.size() - 1 - k] *= (float)k / 441.0f;
    char b[1024];
    snprintf(b, sizeof b, "{\"i\":%d,\"name\":\"%s\",\"cat\":\"%s\",\"phrase\":\"%s\",\"finite\":%d,\"peak\":%.4f,\"peakDb\":%.1f,\"rmsDb\":%.1f,\"rmsActiveDb\":%.1f,\"clipped\":%d,"
             "\"silent\":%d,\"dc\":%.5f,\"centroid\":%.0f,\"low\":%.3f,\"mid\":%.3f,\"high\":%.3f,\"attackMs\":%d,\"activeFrames\":%d,\"gainDb\":%.1f}",
             idx, name, cat, phrase, finite ? 1 : 0, pk, 20 * log10(pk + 1e-12), 20 * log10(sqrt(sum / n) + 1e-12), rawDb, clipped,
             rmsActive < 3e-4 ? 1 : 0, dc / n, cn / cd, lo / all, mid / all, hi / all, fAtt * 50, aN, 20 * log10(gain));
    f.json = b;
    return f;
}
}  // namespace phr
