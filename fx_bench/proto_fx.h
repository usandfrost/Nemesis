// SKETCHES of "write small new" candidates, for cost measurement only (not product code, not reviewed for sound). They show what a
// firmware-native FX would cost if written for the M7: plain structs, no virtuals, no atomics, no heap in the audio path, Real = float on
// the board, fm:: fast math (firmware/dsp FastMath.h) where a transcendental is needed per sample.
#pragma once
#include <nemesis/dsp/FastMath.h>
#include <nemesis/dsp/Real.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace proto {
using nemesis::dsp::Real;

inline Real onePole(Real hz, Real sr) { return Real(1) - nemesis::dsp::fm::exp2(Real(-1.4426950409) * Real(6.2831853) * hz / sr); }

// ------------------------------------------------------------------------------------------------ DELAY: stereo, ms time glided (varispeed)
// Ring buffers are separate per channel, power of two, linear interpolation, feedback through LP + DC-block + cheap soft clip.
// A time change glides (one pole, ~60 ms): no click, but the repeats swoop in pitch while it moves (that is the tape/BBD behaviour).
struct Delay {
    std::vector<Real> bl, br;
    int mask = 0, w = 0;
    Real sr = 44100, cur = 0, target = 0, glide = 0, fb = 0, lpA = 1, pp = 0;
    Real lpL = 0, lpR = 0, dcL = 0, dcR = 0, pl = 0, pr = 0;
    void init(Real fs, Real maxSec) {
        sr = fs;
        int n = 1;
        while (n < (int)(fs * maxSec) + 4) n <<= 1;
        bl.assign(n, 0); br.assign(n, 0);
        mask = n - 1;
        glide = onePole(Real(8), fs * Real(1)) ;   // ~20 ms time constant at 8 Hz corner
    }
    size_t bytes() const { return (bl.size() + br.size()) * sizeof(Real); }
    void set(Real ms, Real feedback, Real lpHz, Real pingpong) {
        target = ms * Real(0.001) * sr;
        fb = feedback;
        lpA = onePole(lpHz, sr);
        pp = pingpong;
        if (cur == 0) cur = target;
    }
    static Real clip(Real x) {   // soft limit of the loop: x - x^3/3 inside +-1, flat outside
        x = x > Real(1.5) ? Real(1.5) : (x < Real(-1.5) ? Real(-1.5) : x);
        return x - x * x * x * Real(0.148148);   // 1.5 maps to the plateau (x - 4/27 x^3 peaks at 1)
    }
    Real tap(const std::vector<Real>& b, Real d) const {
        const Real rp = Real(w + mask + 1) - d;
        const int i = (int)rp;
        const Real f = rp - Real(i);
        return b[i & mask] + f * (b[(i + 1) & mask] - b[i & mask]);
    }
    void process(float inL, float inR, float& oL, float& oR) {
        cur += glide * (target - cur);
        const Real yl = tap(bl, cur), yr = tap(br, cur);
        // loop: LP, DC block, clip; ping-pong crosses the channels (mono-summed input goes in on the left)
        lpL += lpA * (yl - lpL);
        lpR += lpA * (yr - lpR);
        Real fl = lpL - dcL, fr = lpR - dcR;
        dcL += Real(0.0005) * fl;
        dcR += Real(0.0005) * fr;
        fl = clip(fl) * fb;
        fr = clip(fr) * fb;
        if (pp > 0) {   // ping-pong: the mono-summed input enters the left line, each repeat hops to the other side
            bl[w] = Real(0.5) * (inL + inR) + fr;
            br[w] = fl;
        } else {
            bl[w] = inL + fl;
            br[w] = inR + fr;
        }
        w = (w + 1) & mask;
        oL = yl;
        oR = yr;
    }
};

// ------------------------------------------------------------------------------------------------ CHORUS: stereo, 2 voices per channel
// Two sine LFOs per side via coupled-form oscillators (no sin per sample), Hermite (cubic) read, 1 ring per channel of 2048 samples.
struct Chorus {
    std::vector<Real> bl, br;
    int mask = 0, w = 0;
    Real sr = 44100, base = 0, depth = 0, u[4] = {1, 0, 1, 0}, v[4] = {0, 1, 0, -1}, c = 1, s = 0, mix = 0.5f;
    void init(Real fs) {
        sr = fs;
        bl.assign(2048, 0); br.assign(2048, 0);
        mask = 2047;
        base = Real(0.018) * fs;
        v[0] = 0; u[0] = 1;
        // four oscillators: L voice a, L voice b, R voice a, R voice b, 90 degrees apart
        const Real ph[4] = {0, Real(1.5707963), Real(3.1415926), Real(4.712389)};
        for (int i = 0; i < 4; ++i) { u[i] = std::cos(ph[i]); v[i] = std::sin(ph[i]); }
    }
    size_t bytes() const { return (bl.size() + br.size()) * sizeof(Real); }
    void set(Real rateHz, Real depthMs, Real m) {
        const Real a = Real(6.2831853) * rateHz / sr;
        c = std::cos(a); s = std::sin(a);
        depth = depthMs * Real(0.001) * sr;
        mix = m;
    }
    static Real herm(const std::vector<Real>& b, int mask, int w, Real d) {
        const Real rp = Real(w + mask + 1) - d;
        const int i = (int)rp;
        const Real f = rp - Real(i);
        const Real xm1 = b[(i - 1) & mask], x0 = b[i & mask], x1 = b[(i + 1) & mask], x2 = b[(i + 2) & mask];
        const Real cc = (x1 - xm1) * Real(0.5);
        const Real vv = x0 - x1;
        const Real ww = cc + vv;
        const Real aa = ww + vv + (x2 - x0) * Real(0.5);
        const Real bb = ww + aa;
        return ((aa * f - bb) * f + cc) * f + x0;
    }
    void process(float inL, float inR, float& oL, float& oR) {
        for (int i = 0; i < 4; ++i) { const Real nu = c * u[i] - s * v[i]; v[i] = s * u[i] + c * v[i]; u[i] = nu; }
        bl[w] = inL; br[w] = inR;
        const Real wl = Real(0.5) * (herm(bl, mask, w, base + depth * v[0]) + herm(bl, mask, w, base * Real(1.3) + depth * v[1]));
        const Real wr = Real(0.5) * (herm(br, mask, w, base + depth * v[2]) + herm(br, mask, w, base * Real(1.3) + depth * v[3]));
        w = (w + 1) & mask;
        oL = inL + mix * (wl - inL);
        oR = inR + mix * (wr - inR);
    }
};

// ------------------------------------------------------------------------------------------------ COMPRESSOR: linked stereo, feed-forward
// Peak detector (max |L|,|R|) -> log2 -> soft-knee gain computer in dB -> attack/release one-pole on the gain in dB -> exp2. One log2 and
// one exp2 per sample (fm:: fast versions on the board). makeup is a smoothed gain. No look-ahead, no sidechain filter.
struct Comp {
    Real sr = 44100, thr = -20, ratioInv = 0.25f, knee = 6, atk = 0, rel = 0, gdb = 0, makeup = 1, makeupTarget = 1, det = 0;
    void init(Real fs) { sr = fs; }
    void set(Real thrDb, Real ratio, Real atkMs, Real relMs, Real makeupDb) {
        thr = thrDb;
        ratioInv = Real(1) / ratio;
        atk = Real(1) - nemesis::dsp::fm::exp2(Real(-1.4426950409) / (atkMs * Real(0.001) * sr));
        rel = Real(1) - nemesis::dsp::fm::exp2(Real(-1.4426950409) / (relMs * Real(0.001) * sr));
        makeupTarget = nemesis::dsp::fm::exp2(makeupDb * Real(0.16609640474));
    }
    void process(float l, float r, float& oL, float& oR) {
        const Real a = std::fabs(l) > std::fabs(r) ? std::fabs(l) : std::fabs(r);
        const Real db = Real(6.0206) * nemesis::dsp::fm::log2(a + Real(1e-9));
        const Real over = db - thr;
        Real gr;   // target gain change in dB (<= 0)
        if (over <= -knee * Real(0.5)) gr = 0;
        else if (over >= knee * Real(0.5)) gr = over * (ratioInv - 1);
        else { const Real x = over + knee * Real(0.5); gr = (ratioInv - 1) * x * x / (2 * knee); }
        gdb += (gr < gdb ? atk : rel) * (gr - gdb);
        makeup += Real(0.001) * (makeupTarget - makeup);
        const Real g = nemesis::dsp::fm::exp2(gdb * Real(0.16609640474)) * makeup;
        oL = l * g;
        oR = r * g;
    }
};

// ------------------------------------------------------------------------------------------------ AMP: sketch, 2x oversampled, mono-summed
// input HP -> gain -> asymmetric soft clip (x - x^3/3 with bias) at 2x via a 7-tap half-band -> LP -> 3-band tone (3 SVFs) -> cab (2 biquads)
// -> stereo copy. Everything one channel: a part's amp insert sums to mono first (a "mono amp, stereo after").
struct Amp {
    Real sr = 44100, drive = 6, bias = 0.1f, hpS = 0, hpA = 0.01f, lp1 = 0, lpA = 0.5f, up1 = 0, dn1 = 0, dn2 = 0, dn3 = 0;
    struct Bq { Real b0 = 0.3f, b1 = 0.6f, b2 = 0.3f, a1 = -0.2f, a2 = 0.1f, z1 = 0, z2 = 0;
        Real run(Real x) { const Real y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } } tone[3], cab[2];
    void init(Real fs) { sr = fs; hpA = onePole(Real(70), fs); lpA = onePole(Real(5000), fs); }
    static Real shape(Real x) { x = x > Real(1) ? Real(1) : (x < Real(-1) ? Real(-1) : x); return x - x * x * x * Real(0.33333333); }
    void process(float l, float r, float& oL, float& oR) {
        Real x = Real(0.5) * (l + r);
        hpS += hpA * (x - hpS);
        x = (x - hpS) * drive;
        // 2x: zero-stuff + 3-tap interpolation, clip both, 3-tap decimation (cheap half-band shape, not a design)
        const Real a = shape(x * Real(0.5) + up1 * Real(0.5) + bias), b = shape(x + bias);
        up1 = x;
        const Real y = Real(0.25) * dn1 + Real(0.5) * a + Real(0.25) * b;
        dn1 = b;
        lp1 += lpA * (y - lp1);
        Real z = lp1;
        for (auto& t : tone) z = t.run(z);
        for (auto& c : cab) z = c.run(z);
        oL = oR = z;
    }
};

}  // namespace proto
