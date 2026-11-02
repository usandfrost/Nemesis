#ifndef __APP_FMT_H
#define __APP_FMT_H

#include <stdint.h>
#include <string.h>

// A fixed-capacity string for screen text. No heap, no printf (newlib-nano's printf has no floats unless linked in), and it never
// overflows: appends past the end are cut off.
namespace app {

template <int N>
struct Str {
    char s[N];
    int n = 0;

    Str() { s[0] = 0; }
    Str(const char* t) { s[0] = 0; add(t); }
    const char* c_str() const { return s; }
    int size() const { return n; }
    bool empty() const { return n == 0; }
    void clear() { n = 0; s[0] = 0; }

    Str& add(const char* t) {
        while (t && *t && n < N - 1) s[n++] = *t++;
        s[n] = 0;
        return *this;
    }
    Str& ch(char c) {
        if (n < N - 1) s[n++] = c;
        s[n] = 0;
        return *this;
    }
    // A signed integer in decimal.
    Str& num(long v) {
        char b[24];
        int k = 0;
        unsigned long u = v < 0 ? (unsigned long)(-(v + 1)) + 1u : (unsigned long)v;
        do { b[k++] = (char)('0' + u % 10u); u /= 10u; } while (u);
        if (v < 0) ch('-');
        while (k) ch(b[--k]);
        return *this;
    }
    // "+7", "0", "-3": the sign only when not zero, as the mockup's signed values.
    Str& sgn(long v) {
        if (v > 0) ch('+');
        return num(v);
    }
    // Zero-padded to `width` digits (non-negative values).
    Str& pad(long v, int width) {
        Str<24> t;
        t.num(v);
        for (int i = t.n; i < width; ++i) ch('0');
        return add(t.s);
    }
    // JavaScript's toFixed(d) for the values the screen shows (|v| < 1e9): rounds half away from zero.
    Str& fixed(double v, int d) {
        double m = 1.0;
        for (int i = 0; i < d; ++i) m *= 10.0;
        const bool neg = v < 0;
        // toFixed rounds the binary value, not its decimal spelling, so 1.005 gives "1.00" in both.
        const long long r = (long long)((neg ? -v : v) * m + 0.5);
        const long long ip = r / (long long)m, fp = r % (long long)m;
        if (neg && r != 0) ch('-');
        num((long)ip);
        if (d > 0) {
            ch('.');
            pad((long)fp, d);
        }
        return *this;
    }
    bool operator==(const char* t) const { return strcmp(s, t) == 0; }
};

using Text = Str<96>;   // the longest toast is 67 characters

// JavaScript Math.round: half up (towards +infinity).
inline long jsRound(double v) {
    double f = (double)(long long)v;
    if (f > v) f -= 1.0;   // floor
    return (long)(v - f >= 0.5 ? f + 1.0 : f);
}

}  // namespace app

#endif
