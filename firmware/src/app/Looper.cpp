#include "app/Looper.h"

#include <math.h>

namespace app {

double loopFadeFrames(const int16_t* lp, double loopFrames, uint32_t rate) {
    const double v = lp[LP_XFADE] / 127.0;
    const double f = lp[LP_UNIT] == LOOP_PCT ? v * 0.5 * loopFrames : kLoopFadeMaxMs / 1000.0 * v * v * rate;
    return f < loopFrames * 0.5 ? f : loopFrames * 0.5;
}

void loopRegion(const int16_t* sm, const int16_t* lp, uint32_t length, uint32_t rate, LoopRegion& r) {
    // START and END as fractions of the sample, at least 4 ms (and two frames): a START at or past END moves START back, so the region is never empty or reversed
    const double len = length, last = len - 1, minLen = 0.004 * rate > 2 ? 0.004 * rate : 2;
    double s0 = sm[1] / 127.0 * len, s1 = sm[2] / 127.0 * len;
    if (lp && lp[LP_AUTO] == AUTO_OFF) {   // S FINE and E FINE: 63 steps inside one of START's and END's
        const double fine = len / (127.0 * 63.0);
        s0 += lp[LP_SFINE] * fine;
        s1 += lp[LP_EFINE] * fine;
        if (s0 < 0) s0 = 0;
    }
    if (s1 > last) s1 = last;
    if (s0 > last) s0 = last;
    if (s1 < s0 + minLen) s1 = s0 + minLen < last ? s0 + minLen : last;
    if (s1 - s0 < minLen) s0 = s1 - minLen > 0 ? s1 - minLen : 0;
    r.start = s0;
    r.end = s1;
    r.fade = lp ? loopFadeFrames(lp, s1 - s0, rate) : 0.0;
}

// Rising zero crossings around `c`, evenly over the whole reach however many there are
static constexpr int kCrossings = 96;
static int crossingsNear(const int16_t* d, uint32_t n, uint32_t c, uint32_t reach, uint32_t* out) {
    const uint32_t lo = c > reach ? c - reach : 1, hi = c + reach < n ? c + reach : n - 1;
    uint32_t all = 0;
    for (uint32_t i = lo; i < hi; ++i) all += d[i - 1] < 0 && d[i] >= 0;
    const uint32_t every = all / kCrossings + 1;
    int found = 0;
    uint32_t seen = 0;
    for (uint32_t i = lo; i < hi && found < kCrossings; ++i)
        if (d[i - 1] < 0 && d[i] >= 0 && seen++ % every == 0) out[found++] = i;
    return found;
}

static bool zeroMatch(const int16_t* d, uint32_t n, uint32_t s0, uint32_t e0, uint32_t reach, uint32_t& outS, uint32_t& outE) {
    uint32_t ss[kCrossings], es[kCrossings];
    const int ns = crossingsNear(d, n, s0, reach, ss), ne = crossingsNear(d, n, e0, reach, es);
    const int32_t half = 512;
    constexpr uint32_t kPoints = 256;
    const uint32_t shortest = (e0 - s0) / 2;
    double best = 1e30;
    bool any = false;
    for (int a = 0; a < ns; ++a)
        for (int b = 0; b < ne; ++b) {
            const uint32_t s = ss[a], e = es[b];
            if (e <= s || e - s < shortest) continue;
            uint64_t diff = 0;   // the waves either side of the join should be one wave
            uint32_t count = 0;
            for (int32_t i = -half; i < half && (double)diff < best * kPoints; i += 4) {
                const int64_t x = (int64_t)s + i, y = (int64_t)e + i;
                if (x < 0 || y >= (int64_t)n) continue;
                const int v = d[x] - d[y];
                diff += (uint64_t)(v < 0 ? -v : v);
                ++count;
            }
            if (count < kPoints / 2) continue;   // too near the sample's ends to tell (a short window must not win by being short)
            const double mean = (double)diff / count;
            if ((double)diff >= best * kPoints) continue;
            if (mean < best) { best = mean; outS = s; outE = e; any = true; }
        }
    return any;
}

static float energyOver(const int16_t* d, uint32_t n, uint32_t from, uint32_t len) {
    double sum = 0;
    const uint32_t to = from + len < n ? from + len : n, stride = len / 128 > 1 ? len / 128 : 1;
    uint32_t count = 0;
    for (uint32_t i = from; i < to; i += stride + ((i * 2654435761u) >> 29)) { sum += (double)d[i] * d[i]; ++count; }   // (uneven steps: an even one can land on a note's period)
    return (float)(count ? sum / count : 0.0);
}

// How alike the wave is over [point - before, point + after) at two points, -1..1
static float alikeAt(const int16_t* d, uint32_t n, uint32_t s, uint32_t e, uint32_t before, uint32_t after) {
    double ab = 0, aa = 0, bb = 0;
    const int32_t step = (int32_t)((before + after) / 512) + 1;
    for (int32_t i = -(int32_t)before; i < (int32_t)after; i += step) {
        const int64_t x = (int64_t)s + i, y = (int64_t)e + i;
        if (x < 0 || y < 0 || x >= (int64_t)n || y >= (int64_t)n) continue;
        const double p = d[x], q = d[y];
        ab += p * q;
        aa += p * p;
        bb += q * q;
    }
    return aa > 0 && bb > 0 ? (float)(ab / sqrt(aa * bb)) : 0.0f;
}

static bool powerMatch(const int16_t* d, uint32_t n, uint32_t rate, uint32_t s0, uint32_t e0, uint32_t reach, double fade, uint32_t& outS, uint32_t& outE) {
    constexpr int kPoints = 192;
    uint32_t w = (uint32_t)fade;
    if (w < rate / 50) w = rate / 50;
    if (w > (e0 - s0) / 2) w = (e0 - s0) / 2;
    if (w < 16) return false;
    const uint32_t hop = reach * 2 / kPoints + 1;
    const uint32_t sFrom = s0 > reach ? s0 - reach : 0, eFrom = e0 > reach ? e0 - reach : 0;
    float es[kPoints], ee[kPoints];
    for (int i = 0; i < kPoints; ++i) {
        const uint32_t s = sFrom + (uint32_t)i * hop, e = eFrom + (uint32_t)i * hop;
        es[i] = s >= w ? energyOver(d, n, s - w, w) : energyOver(d, n, s, w);   // what fades in comes from before the start, when there is any
        ee[i] = e >= w ? energyOver(d, n, e - w, w) : -1.0f;
    }
    const float tiny = 4.0f;   // of a 16-bit square: anything quieter is silence
    float best = 1e30f;
    int bs = -1, be = -1;
    for (int a = 0; a + 1 < kPoints; ++a) {
        if (es[a] < tiny) continue;
        const float la = logf(es[a]), steadyA = fabsf(logf(es[a + 1] + 1.0f) - la);   // not on an attack or the fall after one
        for (int b = 1; b < kPoints; ++b) {
            if (ee[b] < tiny) continue;
            const uint32_t s = sFrom + (uint32_t)a * hop, e = eFrom + (uint32_t)b * hop;
            if (e <= s + w * 2 || e >= n) continue;
            const float lb = logf(ee[b]), steadyB = fabsf(logf(ee[b - 1] + 1.0f) - lb);
            const float away = (float)(fabs((double)s - s0) + fabs((double)e - e0)) / (float)(reach * 2 + 1);
            const float score = fabsf(la - lb) + 0.5f * (steadyA + steadyB) + 0.05f * away;
            if (score < best) { best = score; bs = a; be = b; }
        }
    }
    if (bs < 0) return false;
    const uint32_t s = sFrom + (uint32_t)bs * hop;
    uint32_t e = eFrom + (uint32_t)be * hop;
    // In step with where the loop turns back to, by at most about a low note's period: the slide must not walk off the place the energy search chose.
    const uint32_t slide = rate / 60, turn = s > (uint32_t)fade ? s : (uint32_t)fade;
    float top = -2.0f;
    uint32_t bestE = e;
    for (uint32_t k = e > slide ? e - slide : 0; k <= e + slide && k < n; ++k) {
        if (k <= turn + w) continue;
        const float r = alikeAt(d, n, turn, k, 256, 256);
        if (r > top) { top = r; bestE = k; }
    }
    outS = s;
    outE = bestE;
    return true;
}

bool findLoopPoints(const int16_t* d, uint32_t n, uint32_t rate, uint32_t s0, uint32_t e0, int algo, int search, double fade, uint32_t& outS, uint32_t& outE, float& corr) {
    outS = s0;
    outE = e0;
    corr = 0.0f;
    if (!d || !rate || n < 64 || e0 <= s0 || e0 >= n) return false;
    if (search < 1) search = 1;
    if (search > 127) search = 127;
    uint32_t reach = (uint32_t)((uint64_t)(e0 - s0) * (uint32_t)search / 127u / 4u);
    if (reach < rate / 100) reach = rate / 100;
    if (reach > rate / 2) reach = rate / 2;
    const bool ok = algo == AUTO_POWER ? powerMatch(d, n, rate, s0, e0, reach, fade, outS, outE) : zeroMatch(d, n, s0, e0, reach, outS, outE);
    const uint32_t turn = outS > (uint32_t)fade ? outS : (uint32_t)fade;
    const float r = fade > 16.0 ? alikeAt(d, n, turn, outE, (uint32_t)fade, 0) : alikeAt(d, n, turn, outE, 256, 256);   // over what the fade will overlay
    corr = r < 0.0f ? 0.0f : r;
    return ok;
}

}  // namespace app
