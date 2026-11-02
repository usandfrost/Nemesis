#include "app/SampleLib.h"

#include <math.h>
#include <string.h>

namespace app {

int SampleLib::addCategory(const char* name) {
    if (catCount == 0) { strcpy(cats[0], "ALL"); catCount = 1; }
    for (int i = 1; i < catCount; ++i) if (strcmp(cats[i], name) == 0) return i;
    if (strcmp(name, "REC") != 0 && catCount >= kMaxSampleCats - 1) return kMaxSampleCats - 2;   // the last place is kept for the takes
    strncpy(cats[catCount], name, sizeof(cats[0]) - 1);
    cats[catCount][sizeof(cats[0]) - 1] = 0;
    return catCount++;
}

void SampleLib::keyOf(int i, char* out, int size) const {
    if (size <= 0) return;
    out[0] = 0;
    if (i < 0 || i >= count) return;
    const SampleItem& it = items[i];
    if (it.path) { strncpy(out, it.path, (size_t)size - 1); out[size - 1] = 0; return; }
    out[0] = '@';
    strncpy(out + 1, it.name, (size_t)size - 2);
    out[size - 1] = 0;
}

size_t SampleLib::compactCache(int16_t* base, size_t used, const bool* keep) {
    // the cached samples in the order they sit in the block
    static int16_t order[kMaxSamples];
    int n = 0;
    for (int i = 0; i < count; ++i)
        if (items[i].data >= base && items[i].data < base + used) order[n++] = (int16_t)i;
    for (int a = 1; a < n; ++a)
        for (int b = a; b > 0 && items[order[b - 1]].data > items[order[b]].data; --b) { const int16_t t = order[b]; order[b] = order[b - 1]; order[b - 1] = t; }
    size_t at = 0;
    for (int k = 0; k < n; ++k) {
        SampleItem& it = items[order[k]];
        if (!keep[order[k]]) { it.data = nullptr; continue; }
        int16_t* to = base + at;
        if (to != it.data) memmove(to, it.data, it.length * sizeof(int16_t));
        it.data = to;
        at += it.length;
    }
    return at;
}

int SampleLib::find(const char* key) const {
    if (!key || !*key) return -1;
    if (key[0] == '@') {   // a built-in sample, by name
        for (int i = 0; i < count; ++i) if (!items[i].path && strcmp(items[i].name, key + 1) == 0) return i;
        return -1;
    }
    // the card's samples are one run sorted by path (Card::indexSamples): a binary search over it
    int lo = 0;
    while (lo < count && !items[lo].path) ++lo;
    const int run = sortedEnd > 0 ? sortedEnd : count;
    int hi = run - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int c = items[mid].path ? strcmp(items[mid].path, key) : -1;
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    for (int i = run; i < count; ++i) if (items[i].path && strcmp(items[i].path, key) == 0) return i;   // the takes kept since the start
    // Other capitals: FAT does not tell Rec/ from REC/, so a take kept as REC/ is the file a later index finds as Rec/
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; };
    for (int i = 0; i < count; ++i) {
        const char* a = items[i].path;
        if (!a) continue;
        const char* b = key;
        while (*a && lower(*a) == lower(*b)) { ++a; ++b; }
        if (!*a && !*b) return i;
    }
    return -1;
}

int SampleLib::catSize(int c) const {
    if (c == 0) return count;
    int n = 0;
    for (int i = 0; i < count; ++i) n += items[i].cat == c;
    return n;
}

int SampleLib::catPlace(int i) const {
    int n = 0;
    for (int k = 0; k < i; ++k) n += items[k].cat == items[i].cat;
    return n;
}

int SampleLib::catFirst(int c) const {
    for (int i = 0; i < count; ++i) if (c == 0 || items[i].cat == c) return i;
    return -1;
}

int SampleLib::catNext(int i, int dir) const {
    for (int k = i + dir; k >= 0 && k < count; k += dir) if (items[k].cat == items[i].cat) return k;
    return -1;
}

int SampleLib::catStep(int c, int dir, bool all) const {
    for (int k = c + dir; k >= (all ? 0 : 1) && k < catCount; k += dir) if (k == 0 || catFirst(k) >= 0) return k;
    return c;
}

int SampleLib::catsInUse(int c, int& place) const {
    bool has[kMaxSampleCats] = {};
    for (int i = 0; i < count; ++i) has[items[i].cat] = true;
    int n = 0;
    place = 0;
    for (int k = 1; k < catCount; ++k) {
        if (!has[k]) continue;
        if (k < c) ++place;
        ++n;
    }
    return n;
}

namespace {

struct Gen {
    SampleLib& lib;
    int16_t* storage;
    size_t cap, used = 0;
    uint32_t seed = 20251026u;

    double rnd() {   // the mockup's LCG: [-1, 1)
        seed = seed * 1664525u + 1013904223u;
        return (seed / 4294967296.0) * 2.0 - 1.0;
    }

    struct St { double p = 0, l = 0, y = 0; };
    static double hp(St& st, double x, double a) { st.l = st.l * a + x * (1 - a); return x - st.l; }

    // Two passes over the same random sequence: the first finds the peak, the second writes the normalised 16-bit sample. No float
    // scratch buffer (the longest sample would need 248 KB of it).
    template <class Fn>
    void add(const char* name, const char* cat, double len, Fn fn) {
        const size_t n = (size_t)floor(len * kFactoryRate);
        if (used + n > cap || lib.count >= kMaxSamples) return;
        const uint32_t seed0 = seed;
        St st;
        double m = 0;
        for (size_t i = 0; i < n; ++i) { const double v = fabs((double)(float)fn((double)i / kFactoryRate, st)); m = v > m ? v : m; }
        const double norm = (m > 0 ? m : 1.0) * 1.05;
        seed = seed0;
        st = St();
        int16_t* d = storage + used;
        for (size_t i = 0; i < n; ++i) d[i] = (int16_t)lrint((float)fn((double)i / kFactoryRate, st) / norm * 32767.0);
        SampleItem& it = lib.items[lib.count++];
        strncpy(it.name, name, kSampleNameLen - 1);
        it.name[kSampleNameLen - 1] = 0;
        it.cat = (uint8_t)lib.addCategory(cat);
        it.length = (uint32_t)n;
        it.rate = kFactoryRate;
        it.data = d;
        it.path = nullptr;
        it.bad = 0;
        used += n;
    }
};

}  // namespace

void buildFactorySamples(SampleLib& lib, int16_t* storage, size_t capacity) {
    Gen g{lib, storage, capacity};
    // the mockup's category order, so the BROWSE categories match it
    static const char* const kCats[] = {"KICK", "SNARE", "HAT", "PERC", "BASS", "FX"};
    for (const char* c : kCats) lib.addCategory(c);
    const double TAU = 6.283185307179586, SR = kFactoryRate;
    using St = Gen::St;
    g.add("Kick 808", "KICK", 0.7, [&](double t, St& s) { s.p += (45 + 120 * exp(-t * 28)) / SR; return sin(TAU * s.p) * exp(-t * 4.5) + g.rnd() * exp(-t * 500) * 0.25; });
    g.add("Kick Punch", "KICK", 0.35, [&](double t, St& s) { s.p += (60 + 200 * exp(-t * 45)) / SR; return tanh(1.6 * sin(TAU * s.p)) * exp(-t * 11); });
    g.add("Snare Crack", "SNARE", 0.4, [&](double t, St& s) { s.p += (185 + 60 * exp(-t * 40)) / SR; return sin(TAU * s.p) * exp(-t * 22) * 0.7 + Gen::hp(s, g.rnd(), 0.6) * exp(-t * 16); });
    g.add("Clap Wide", "SNARE", 0.4, [&](double t, St& s) {
        double e = 0;
        for (int k = 0; k < 3; ++k) { const double u = t - k * 0.011; if (u > 0) e += exp(-u * 160); }
        const double tail = t - 0.033 > 0 ? t - 0.033 : 0;
        return Gen::hp(s, g.rnd(), 0.55) * (e * 0.6 + exp(-tail * 22) * (t > 0.033 ? 1 : 0));
    });
    g.add("Rim Shot", "PERC", 0.12, [&](double t, St&) { return sin(TAU * 1750 * t) * exp(-t * 180) * 0.6 + g.rnd() * exp(-t * 300) * 0.5; });
    g.add("Hat Closed", "HAT", 0.1, [&](double t, St& s) { return Gen::hp(s, g.rnd(), 0.9) * exp(-t * 70); });
    g.add("Hat Open", "HAT", 0.4, [&](double t, St& s) { return Gen::hp(s, g.rnd(), 0.9) * exp(-t * 10); });
    g.add("Crash", "HAT", 1.4, [&](double t, St& s) { return Gen::hp(s, g.rnd(), 0.85) * exp(-t * 3); });
    g.add("Conga", "PERC", 0.3, [&](double t, St& s) { s.p += (330 + 80 * exp(-t * 40)) / SR; return sin(TAU * s.p) * exp(-t * 14); });
    g.add("Tom Low", "PERC", 0.5, [&](double t, St& s) { s.p += (118 + 70 * exp(-t * 30)) / SR; return sin(TAU * s.p) * exp(-t * 7); });
    g.add("Bass Stab", "BASS", 0.7, [&](double t, St& s) {
        const double ph = fmod(55 * t, 1.0), x = 2 * ph - 1;
        s.y += (x - s.y) * (0.02 + 0.3 * exp(-t * 9));
        return s.y * exp(-t * 3.5);
    });
    g.add("FX Sweep", "FX", 1.1, [&](double t, St& s) {
        s.y += (g.rnd() - s.y) * (0.01 + 0.4 * t);
        const double w = sin(3.141592653589793 * (t / 1.1 < 1 ? t / 1.1 : 1));
        return s.y * w * w;
    });
}

}  // namespace app
