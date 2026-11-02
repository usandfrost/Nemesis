#ifndef __APP_THEORY_H
#define __APP_THEORY_H

#include <stdint.h>

#include "app/Fmt.h"

// Notes, scales, chords, arpeggios and the Euclid rhythm: a line-for-line port of the mockup's model section.
namespace app {

static const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

inline int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline int mod(int a, int b) { const int m = a % b; return m < 0 ? m + b : m; }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int signOf(int v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

// "C4" = MIDI 60.
template <int N>
inline void noteName(Str<N>& out, int m) {
    out.add(kNoteNames[mod(m, 12)]).num(floorDiv(m, 12) - 1);
}

// The same note as the tracker prints it, three characters: "C-4", "C#4". (Below octave 0, which only a low root reaches: "C_1", "C#_1".)
template <int N>
inline void trackerNoteName(Str<N>& out, int m) {
    const char* n = kNoteNames[mod(m, 12)];
    const int o = floorDiv(m, 12) - 1;
    out.add(n);
    if (o < 0) out.ch('_');
    else if (!n[1]) out.ch('-');
    out.num(o < 0 ? -o : o);
}

// ---- project key and scale: with a scale set, every note a CATHODE part plays snaps to the nearest scale note (the lower one on a tie).
struct Scale {
    const char* name;
    uint8_t count;
    uint8_t deg[7];
};
static const Scale kScales[] = {
    {"OFF", 0, {}},
    {"MAJOR", 7, {0, 2, 4, 5, 7, 9, 11}},
    {"MINOR", 7, {0, 2, 3, 5, 7, 8, 10}},
    {"DORIAN", 7, {0, 2, 3, 5, 7, 9, 10}},
    {"PHRYGIAN", 7, {0, 1, 3, 5, 7, 8, 10}},
    {"LYDIAN", 7, {0, 2, 4, 6, 7, 9, 11}},
    {"MIXOLYD", 7, {0, 2, 4, 5, 7, 9, 10}},
    {"LOCRIAN", 7, {0, 1, 3, 5, 6, 8, 10}},
    {"HARM MIN", 7, {0, 2, 3, 5, 7, 8, 11}},
    {"MEL MIN", 7, {0, 2, 3, 5, 7, 9, 11}},
    {"MAJ PENT", 5, {0, 2, 4, 7, 9}},
    {"MIN PENT", 5, {0, 3, 5, 7, 10}},
    {"BLUES", 6, {0, 3, 5, 6, 7, 10}},
};
constexpr int kScaleCount = sizeof(kScales) / sizeof(kScales[0]);

inline bool scaleHas(int scale, int degree) {
    const Scale& s = kScales[scale];
    for (int i = 0; i < s.count; ++i) if (s.deg[i] == degree) return true;
    return false;
}

inline int scaleNote(int m, int key, int scale) {
    if (scale <= 0 || scale >= kScaleCount) return m;
    const Scale& sc = kScales[scale];
    const int pc = mod(m - key, 12);
    int best = 99;
    auto absi = [](int v) { return v < 0 ? -v : v; };
    for (int i = 0; i < sc.count; ++i) {
        const int x = sc.deg[i];
        const int ds[3] = {x - pc, x - pc - 12, x - pc + 12};
        for (int d : ds)
            if (absi(d) < absi(best) || (absi(d) == absi(best) && d < best)) best = d;
    }
    return m + best;
}

// ---- chords per step, and the lane's arpeggio
struct Chord {
    const char* name;
    uint8_t count;
    uint8_t iv[5];
};
static const Chord kChords[] = {
    {"OFF", 1, {0}},          {"MAJ", 3, {0, 4, 7}},     {"MIN", 3, {0, 3, 7}},         {"SUS2", 3, {0, 2, 7}},
    {"SUS4", 3, {0, 5, 7}},   {"DIM", 3, {0, 3, 6}},     {"AUG", 3, {0, 4, 8}},         {"MAJ7", 4, {0, 4, 7, 11}},
    {"MIN7", 4, {0, 3, 7, 10}}, {"DOM7", 4, {0, 4, 7, 10}}, {"MIN9", 5, {0, 3, 7, 10, 14}}, {"5TH", 2, {0, 7}},
    {"OCT", 2, {0, 12}},
};
constexpr int kChordCount = sizeof(kChords) / sizeof(kChords[0]);

static const char* const kArpModes[] = {"OFF", "UP", "DOWN", "UP/DN", "RAND"};
constexpr int kArpModeCount = 5;
static const uint8_t kArpRates[] = {1, 2, 3, 4, 6, 8};   // notes per step
constexpr int kArpRateCount = 6;

constexpr int kMaxChordNotes = 5;
constexpr int kMaxArpNotes = 8;

// The notes of a chord on `root`, lowest first. Inversion lifts the lowest note an octave; open voicing lifts every other note.
inline int chordNotes(int root, int chord, int inv, int spread, int* out) {
    const Chord& c = kChords[chord];
    int n = c.count;
    int v[kMaxChordNotes];
    for (int i = 0; i < n; ++i) v[i] = c.iv[i];
    for (int i = 0; i < inv && n > 1; ++i) {
        const int lo = v[0];
        for (int j = 0; j + 1 < n; ++j) v[j] = v[j + 1];
        v[n - 1] = lo + 12;
    }
    if (spread)
        for (int i = 0; i < n; ++i) if (i % 2) v[i] += 12;
    for (int i = 1; i < n; ++i)   // sort ascending (insertion)
        for (int j = i; j > 0 && v[j - 1] > v[j]; --j) { const int t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    for (int i = 0; i < n; ++i) out[i] = root + v[i];
    return n;
}

// `count` notes of the arpeggio over `notes` across `oct` octaves. `rnd` returns [0, 1).
template <class Rnd>
inline void arpSeq(const int* notes, int n, int mode, int oct, int count, Rnd&& rnd, int* out) {
    int pool[kMaxChordNotes * 4];
    int pn = 0;
    for (int o = 0; o < oct; ++o)
        for (int i = 0; i < n; ++i) pool[pn++] = notes[i] + 12 * o;
    int order[kMaxChordNotes * 8];
    int on = 0;
    if (mode == 2) {
        for (int i = pn - 1; i >= 0; --i) order[on++] = pool[i];
    } else if (mode == 3) {
        for (int i = 0; i < pn; ++i) order[on++] = pool[i];
        for (int i = pn - 2; i >= 1; --i) order[on++] = pool[i];
    } else {
        for (int i = 0; i < pn; ++i) order[on++] = pool[i];
    }
    if (on == 0) { for (int i = 0; i < pn; ++i) order[on++] = pool[i]; }
    for (int i = 0; i < count; ++i) out[i] = mode == 4 ? pool[(int)(rnd() * pn)] : order[i % on];
}

// Euclid: hit i of n when (i * k) mod n < k, rotated right by r. E(3,8) = x..x..x.
inline bool euclidHit(int n, int k, int r, int i) {
    if (k <= 0 || n <= 0) return false;
    return (mod(i - r, n) * k) % n < k;
}

}  // namespace app

#endif
