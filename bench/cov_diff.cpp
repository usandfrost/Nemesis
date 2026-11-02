// Compares two render_cov outputs patch by patch.
//   cov_diff ref.f32 test.f32 names.txt [--exact]
// Per patch: bit-identical or not, peak sample error, time-domain SNR, NaN/Inf count, and a spectral check that ignores a tiny
// pitch or phase offset: Hann frames of 4096 (hop 2048, left channel, from 0.1 s); "worst" = largest |dB difference| over bins
// within 60 dB of the frame peak, "floor" = energy change (dB) of the bins more than 60 dB down (aliasing, noise).
// With --exact the exit code is 1 unless every patch is bit-identical; otherwise 1 when any patch has NaN/Inf.
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
std::vector<float> load(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    const auto n = static_cast<size_t>(f.tellg());
    std::vector<float> v(n / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n));
    return v;
}
void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
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
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: cov_diff ref.f32 test.f32 names.txt [--exact]\n"); return 2; }
    const bool exact = argc > 4 && std::strcmp(argv[4], "--exact") == 0;
    const auto a = load(argv[1]), b = load(argv[2]);
    std::vector<std::pair<std::string, int>> names;
    {
        std::ifstream f(argv[3]);
        std::string line;
        while (std::getline(f, line)) {
            const auto sp = line.find_last_of(' ');
            if (sp == std::string::npos) continue;
            names.emplace_back(line.substr(0, sp), std::stoi(line.substr(sp + 1)));
        }
    }
    if (a.size() != b.size()) { std::printf("size mismatch %zu vs %zu\n", a.size(), b.size()); return 1; }
    constexpr int N = 4096;
    std::vector<double> hann(N);
    for (int i = 0; i < N; ++i) hann[i] = 0.5 - 0.5 * std::cos(2 * M_PI * i / N);
    size_t off = 0;
    int nonIdentical = 0, bad = 0;
    double worstAll = 0;
    std::printf("%-26s %-9s %10s %8s %8s %8s %5s\n", "patch", "identical", "peak err", "SNR dB", "worst", "floor", "nan");
    for (const auto& [name, frames] : names) {
        const size_t n = 2 * static_cast<size_t>(frames);
        const float* x = a.data() + off;
        const float* y = b.data() + off;
        off += n;
        const bool same = std::memcmp(x, y, n * 4) == 0;
        double sig = 0, err = 0, peak = 0;
        int nan = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!std::isfinite(y[i])) { ++nan; continue; }
            sig += double(x[i]) * x[i];
            const double d = double(x[i]) - y[i];
            err += d * d;
            peak = std::fmax(peak, std::fabs(d));
        }
        const double snr = err > 0 ? 10 * std::log10(sig / err) : 999;
        double worst = 0, qa = 0, qb = 0;
        if (!same) {
            for (int s = 4410; s + N <= frames; s += N / 2) {
                std::vector<std::complex<double>> fa(N), fb(N);
                for (int i = 0; i < N; ++i) { fa[i] = x[2 * (s + i)] * hann[i]; fb[i] = y[2 * (s + i)] * hann[i]; }
                fft(fa); fft(fb);
                double pk = 0;
                for (int k = 1; k < N / 2; ++k) pk = std::fmax(pk, std::abs(fa[k]));
                if (pk < 1e-9) continue;
                for (int k = 1; k < N / 2; ++k) {
                    const double ma = std::abs(fa[k]), mb = std::abs(fb[k]);
                    if (ma > pk * 1e-3) worst = std::fmax(worst, std::fabs(20 * std::log10((mb + 1e-30) / ma)));
                    else { qa += ma * ma; qb += mb * mb; }
                }
            }
        }
        const double floor = qa > 0 ? 10 * std::log10((qb + 1e-30) / qa) : 0;
        std::printf("%-26s %-9s %10.3g %8.1f %8.2f %8.2f %5d\n", name.c_str(), same ? "yes" : "NO", peak, snr, worst, floor, nan);
        nonIdentical += !same;
        bad += nan > 0;
        worstAll = std::fmax(worstAll, worst);
    }
    std::printf("%zu patches, %d not bit-identical, %d with NaN/Inf, worst spectral deviation %.2f dB\n", names.size(), nonIdentical, bad, worstAll);
    return (exact && nonIdentical) || bad ? 1 : 0;
}
