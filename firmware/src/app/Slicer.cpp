#include "app/Slicer.h"

#include <math.h>
#include <string.h>

namespace app {

static constexpr int kHopsMax = 8192;     // a hop is 5 ms, longer for a sample past 40 s
static constexpr int kCandidates = 128;
static constexpr int kLookBack = 6;       // hops: 30 ms
static float* gOnset = nullptr;           // made at first use (the SDRAM heap on the board)

static void clearSlice(Slice& s, uint32_t start) {
    s.start = start;
    s.pitch = 0;
    s.level = 127;
    s.flags = 0;
    s.pad = 0;
}

// The nearest place at or before `at` where the wave crosses zero, within `reach` frames: a slice starting there does not click.
// How loud the sound after an attack is. When the table cannot hold every attack the loud ones stay: kicks and snares, not hats.
static float loudnessAt(const int16_t* d, uint32_t n, uint32_t at, uint32_t len) {
    float sum = 0.0f;
    const uint32_t end = at + len < n ? at + len : n;
    for (uint32_t i = at; i < end; i += 4) sum += (float)d[i] * (float)d[i];
    return sum;
}

static uint32_t zeroBefore(const int16_t* d, uint32_t at, uint32_t reach) {
    const uint32_t lo = at > reach ? at - reach : 1;
    for (uint32_t i = at; i >= lo && i > 0; --i)
        if ((d[i - 1] <= 0 && d[i] >= 0) || (d[i - 1] >= 0 && d[i] <= 0)) return i;
    return at;
}

int detectSlices(const int16_t* d, uint32_t n, uint32_t rate, int sens, int grid, SliceMap& out) {
    const int16_t keepSample = out.sample;
    memset(&out, 0, sizeof(out));
    out.sample = keepSample;
    out.frames = n;
    clearSlice(out.s[0], 0);
    out.count = 1;
    if (!d || n < 64 || !rate) return 1;
    const uint32_t reach = rate / 500 + 1;   // 2 ms
    if (grid > 0) {   // equal parts
        const int c = grid > kSlices ? kSlices : grid;
        for (int i = 1; i < c; ++i) {
            const uint32_t exact = (uint32_t)((uint64_t)n * (uint32_t)i / (uint32_t)c), last = out.s[out.count - 1].start;
            uint32_t s = zeroBefore(d, exact, reach);
            if (s <= last + 16) s = exact;       // two zero crossings in one place: the slices must stay apart
            if (s > last + 16) clearSlice(out.s[out.count++], s);
        }
        return out.count;
    }
    if (!gOnset) gOnset = new float[kHopsMax];
    uint32_t hop = rate / 200;
    if (hop < 16) hop = 16;
    if (n / hop >= kHopsMax) hop = n / (kHopsMax - 1) + 1;
    const int hops = (int)(n / hop);
    if (hops < 8) return 1;
    // how much the wave moves in each hop (the difference weighs the top end, where an attack shows), on a log scale, then its rise over the last hops
    float* e = gOnset;
    for (int h = 0; h < hops; ++h) {
        float sum = 0.0f;
        const uint32_t a = (uint32_t)h * hop, b = a + hop;
        for (uint32_t i = a > 0 ? a : 1; i < b; ++i) { const float v = (float)(d[i] - d[i - 1]); sum += v * v; }
        e[h] = logf(1.0f + sum / (float)hop);
    }
    // Against the loudest of the hops before the last: an attack is split over two hops, and a low note's period is longer than a hop, so a low note peaks hop after
    // hop. It takes more than one period's look back not to cut on every cycle
    float back[kLookBack + 2] = {};
    for (int h = 0; h < hops; ++h) {
        const float now = e[h];
        float ref = 0.0f;
        for (int k = 2; k < kLookBack + 2; ++k) if (back[k] > ref) ref = back[k];
        for (int k = kLookBack + 1; k > 0; --k) back[k] = back[k - 1];
        back[1] = now;
        e[h] = now > ref ? now - ref : 0.0f;
    }
    // a peak of that rise, well over what is around it. A quiet SENS asks for a big jump
    const float sq = 1.0f - (float)(sens < 0 ? 0 : sens > 127 ? 127 : sens) / 127.0f;
    const float delta = 0.3f + 2.7f * sq * sq;
    const int around = (int)(0.2f * (float)rate / (float)hop) + 1;
    const int minGap = (int)(0.04f * (float)rate / (float)hop) + 1;
    uint32_t pos[kCandidates];
    float strength[kCandidates];
    int found = 0, lastAt = -minGap;
    float lastRise = 0.0f;
    const uint32_t weigh = rate / 40;   // 25 ms
    for (int h = 1; h + 1 < hops; ++h) {
        const float o = e[h];
        if (o <= 0.0f || o < e[h - 1] || o < e[h + 1]) continue;
        const int a = h > around ? h - around : 0, b = h + around < hops ? h + around : hops;
        float mean = 0.0f;
        for (int k = a; k < b; ++k) mean += e[k];
        mean /= (float)(b - a);
        if (o < mean * 1.5f + delta) continue;
        if (h - lastAt < minGap) {   // the same attack: the stronger peak stands for it
            if (found > 0 && o > lastRise) { lastRise = o; pos[found - 1] = (uint32_t)h; lastAt = h; }
            continue;
        }
        if ((uint32_t)h * hop < rate / 50) continue;   // the first slice is there already
        const float loud = loudnessAt(d, n, (uint32_t)h * hop, weigh);
        if (found == kCandidates) {   // full: this one replaces the quietest if it beats it
            int w = 0;
            for (int k = 1; k < found; ++k) if (strength[k] < strength[w]) w = k;
            if (loud <= strength[w]) continue;
            for (int k = w; k + 1 < found; ++k) { strength[k] = strength[k + 1]; pos[k] = pos[k + 1]; }
            --found;
        }
        pos[found] = (uint32_t)h;
        strength[found++] = loud;
        lastAt = h;
        lastRise = o;
    }
    while (found > kSlices - 1) {   // the loudest that fit the table
        int w = 0;
        for (int k = 1; k < found; ++k) if (strength[k] < strength[w]) w = k;
        for (int k = w; k + 1 < found; ++k) { strength[k] = strength[k + 1]; pos[k] = pos[k + 1]; }
        --found;
    }
    for (int k = 0; k < found; ++k) {
        // inside the hops around the peak: where the wave first moves fast, then back to a zero crossing
        const uint32_t a = (pos[k] > 0 ? pos[k] - 1 : 0) * hop, b = (pos[k] + 1) * hop < n ? (pos[k] + 1) * hop : n;
        int biggest = 0;
        for (uint32_t i = a > 0 ? a : 1; i < b; ++i) { const int v = d[i] - d[i - 1]; const int m = v < 0 ? -v : v; if (m > biggest) biggest = m; }
        uint32_t at = a;
        for (uint32_t i = a > 0 ? a : 1; i < b; ++i) { const int v = d[i] - d[i - 1]; if ((v < 0 ? -v : v) * 4 >= biggest) { at = i; break; } }
        const uint32_t s = zeroBefore(d, at, reach);
        if (s > out.s[out.count - 1].start + reach) clearSlice(out.s[out.count++], s);
    }
    return out.count;
}

int guessBeats(uint32_t frames, uint32_t rate, int bpm) {
    if (!rate || !frames || bpm <= 0) return 3;
    const double secs = (double)frames / rate;
    int best = 0;
    double bestAway = 1e9;
    for (int i = 0; i < kSliceBeatChoices; ++i) {
        const double away = fabs(log2(kSliceBeats[i] * 60.0 / secs / bpm));
        if (away < bestAway) { bestAway = away; best = i; }
    }
    return best;
}

bool splitSlice(SliceMap& m, int k, const int16_t* d) {
    if (k < 0 || k >= m.count || m.count >= kSlices) return false;
    const uint32_t a = m.s[k].start, b = k + 1 < m.count ? m.s[k + 1].start : m.frames;
    if (b < a + 64) return false;
    uint32_t mid = a + (b - a) / 2;
    if (d) { const uint32_t z = zeroBefore(d, mid, (b - a) / 8); if (z > a + 16 && z < b - 16) mid = z; }
    for (int i = m.count; i > k + 1; --i) m.s[i] = m.s[i - 1];
    m.s[k + 1] = m.s[k];     // the two halves keep the slice's settings
    m.s[k + 1].start = mid;
    ++m.count;
    return true;
}

bool mergeSlice(SliceMap& m, int k) {
    if (k <= 0 || k >= m.count) return false;
    for (int i = k; i + 1 < m.count; ++i) m.s[i] = m.s[i + 1];
    --m.count;
    return true;
}

}  // namespace app
