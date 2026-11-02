// Transcription of the STOCK Drumboy Pro effects (MIT) into stand-alone, host-buildable structs, for CPU measurement only.
//
// Source: "ref/02 Software/Drumboy-Pro-App-H723/Core/Library/Controller/Controller.cpp", Controller::processAudioEffectHelper
// (case EF_DELAY ... EF_BITCRUSHER, lines ~22600-23300) and Controller::processAudioReverb (~22330-22450), with the parameter structs of
// Global/Global.h (Delay ~3209, Chorus ~3410, Flanger ~3599, Phaser ~3804, Compressor ~4043, Overdrive ~4446, Distortion ~4688,
// Reverb ~5730). The sample loops are copied statement for statement; only the Controller/transition plumbing is dropped. The stock code is
// MONO (one signal through filter -> effect 0 -> effect 1 -> reverb), so every cost here is for ONE channel; a stereo use needs two instances.
//
// Not linked against the original: if the original changes, this file does not follow.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <vector>

namespace stock {

constexpr float kSr = 44100.0f;

// Global.h 195-215: the stock "fast" dB helpers (bit-twiddling log2 / pow2)
inline float fast_log2(float x) {
    union { float f; uint32_t i; } vx = {x};
    union { uint32_t i; float f; } mx = {(vx.i & 0x007FFFFF) | 0x3F000000};
    float y = (float)vx.i;
    y *= 1.1920928955078125e-7f;
    return y - 124.22551499f - 1.498030302f * mx.f - 1.72587999f / (0.3520887068f + mx.f);
}
inline float fast_pow2(float val) {
    float clipp = (val < -126.0f) ? -126.0f : val;
    union { int32_t i; float f; } v = {(int32_t)((1 << 23) * (clipp + 126.94269504f))};
    return v.f;
}
inline float fast_lin2db(float lin) { return 6.0206f * fast_log2(lin); }
inline float fast_db2lin(float db) { return fast_pow2(db * 0.1660964f); }

// ---------------------------------------------------------------------------------------------------- DELAY (mono, 96000-sample buffer)
struct Delay {
    std::vector<float> buf;
    uint32_t play = 0, rec = 0, size = 96000;
    float level = 1.0f, feedback = 0.4f, dampingAlpha = 0.3f, last = 0.0f, dry = 1.0f, wet = 0.5f;
    void init(int lagSamples) {
        buf.assign(size, 0.0f);
        rec = lagSamples;
        play = 0;
        last = 0;
        const float freqFactor = (101.0f - 30.0f) * 0.01f;   // damping 30
        dampingAlpha = freqFactor * freqFactor;
    }
    float process(float input) {
        float delayed = buf[play];
        float fbTarget = input * level + delayed * feedback;
        float filtered = last + dampingAlpha * (fbTarget - last);
        last = filtered;
        if (fabsf(filtered) < 1.0e-18f) filtered = 0.0f;
        buf[rec] = filtered;
        float out = delayed * wet + input * dry;
        if (++play >= size) play = 0;
        if (++rec >= size) rec = 0;
        return out;
    }
};

// ---------------------------------------------------------------------------------------------------- CHORUS (2 voices, 5000-sample buffer)
struct Chorus {
    static constexpr int kBuf = 5000;
    std::vector<float> buf;
    struct V { float mix, shiftMax, shiftMin, shiftInterval, shiftInc; uint16_t playInterval; } v[2];
    uint16_t rec = 0;
    float feedback = 0.25f, dry = 0.75f, wet = 0.75f;
    void init(float time = 0.020f, float rate = 0.8f, float depth = 0.8f) {
        buf.assign(kBuf, 0.0f);
        const float coef[2] = {1.0f, 0.75f};
        for (int i = 0; i < 2; ++i) {
            float t = time * coef[i], d = depth * coef[i], r = rate * coef[i];
            uint16_t lag = (uint16_t)(t * kSr);
            int32_t pi = (int32_t)rec - lag;
            if (pi < 0) pi += kBuf;
            v[i].playInterval = (uint16_t)pi;
            v[i].mix = 0.5f;
            v[i].shiftMax = d * (float)lag - 2.0f;
            if (v[i].shiftMax < 1.0f) v[i].shiftMax = 1.0f;
            v[i].shiftMin = -v[i].shiftMax;
            v[i].shiftInc = ((v[i].shiftMax - v[i].shiftMin) * (r * 2.0f)) / kSr;
            v[i].shiftInterval = 0;
        }
        v[1].shiftInterval = v[1].shiftMax * 0.5f;
    }
    float process(float input) {
        const float bufSizeFloat = (float)kBuf;
        const int32_t bufSizeInt = kBuf;
        float acc = 0.0f;
        for (int i = 0; i < 2; ++i) {
            V& cD = v[i];
            cD.shiftInterval += cD.shiftInc;
            if (cD.shiftInterval <= cD.shiftMin) { cD.shiftInc = fabsf(cD.shiftInc); cD.shiftInterval = cD.shiftMin; }
            else if (cD.shiftInterval >= cD.shiftMax) { cD.shiftInc = -fabsf(cD.shiftInc); cD.shiftInterval = cD.shiftMax; }
            float readPos = (float)cD.playInterval + cD.shiftInterval;
            while (readPos < 0.0f) readPos += bufSizeFloat;
            while (readPos >= bufSizeFloat) readPos -= bufSizeFloat;
            int32_t a = (int32_t)readPos;
            int32_t b = (a + 1) % bufSizeInt;
            float frac = readPos - (float)a;
            float interp = buf[a] + ((buf[b] - buf[a]) * frac);
            acc += interp * cD.mix;
        }
        float fbs = input + (acc * feedback);
        if (fabsf(fbs) < 1.0e-15f) fbs = 0.0f;
        buf[rec] = fbs;
        float out = acc * wet + input * dry;
        rec = (rec + 1) % bufSizeInt;
        for (int i = 0; i < 2; ++i) v[i].playInterval = (v[i].playInterval + 1) % bufSizeInt;
        return out;
    }
};

// ---------------------------------------------------------------------------------------------------- FLANGER (250-sample buffer)
struct Flanger {
    static constexpr int kBuf = 250;
    float buf[kBuf] = {};
    uint16_t rec = 0, play = 0;
    float shiftInt = 0, shiftInc = 0, shiftMin = 0, shiftMax = 0, feedback = 0.25f, dry = 0.75f, wet = 0.75f;
    void init(float time = 0.003f, float rate = 0.5f, float width = 0.8f) {
        uint16_t lag = (uint16_t)(time * kSr);
        int32_t p = (int32_t)rec - lag;
        if (p < 0) p += kBuf;
        play = (uint16_t)p;
        shiftMax = width * (float)lag;
        if (shiftMax > (float)(kBuf - 2)) shiftMax = (float)(kBuf - 2);
        shiftInc = (shiftMax * (rate * 2.0f)) / kSr;
    }
    float process(float input) {
        const float bufSizeF = (float)kBuf;
        const int32_t bufSizeInt = kBuf;
        float readPos = (float)play - shiftInt;
        while (readPos < 0.0f) readPos += bufSizeF;
        while (readPos >= bufSizeF) readPos -= bufSizeF;
        int32_t a = (int32_t)readPos;
        int32_t b = a + 1;
        if (b >= bufSizeInt) b = 0;
        float frac = readPos - (float)a;
        float data = buf[a] + ((buf[b] - buf[a]) * frac);
        buf[rec] = input + data * feedback;
        float out = data * wet + input * dry;
        shiftInt += shiftInc;
        if (shiftInt <= shiftMin) { shiftInc = fabsf(shiftInc); shiftInt = shiftMin; }
        else if (shiftInt >= shiftMax) { shiftInc = -fabsf(shiftInc); shiftInt = shiftMax; }
        if (++play >= bufSizeInt) play = 0;
        if (++rec >= bufSizeInt) rec = 0;
        return out;
    }
};

// ---------------------------------------------------------------------------------------------------- COMPRESSOR
struct Compressor {
    float threshold = -18.0f, rate = 4.0f, attackAlpha, releaseAlpha, gainSmooth = 0.0f, dry = 0.0f, wet = 1.0f;
    void init(float attackS = 0.005f, float releaseS = 0.1f) {
        attackAlpha = expf(-2.19722f / (kSr * attackS));
        releaseAlpha = expf(-2.19722f / (kSr * releaseS));
    }
    float process(float input) {
        float absInput = fabsf(input);
        if (absInput < 1.0e-6f) absInput = 1.0e-6f;
        float in_dB = fast_lin2db(absInput);
        if (in_dB < -96.0f) in_dB = -96.0f;
        float gr = 0.0f;
        if (in_dB > threshold) {
            float diff = in_dB - threshold;
            float ratioInv = 1.0f / rate;
            gr = diff * (ratioInv - 1.0f);
        }
        float atten = 1.0f;
        if (gr >= -0.001f && gainSmooth >= -0.001f) {
            gainSmooth = 0.0f;
        } else {
            float alpha = (gr < gainSmooth) ? attackAlpha : releaseAlpha;
            gainSmooth = ((1.0f - alpha) * gr) + (alpha * gainSmooth);
            atten = fast_db2lin(gainSmooth);
        }
        return (input * atten * wet) + (input * dry);
    }
};

// ---------------------------------------------------------------------------------------------------- OVERDRIVE / DISTORTION
struct Biquad {
    float a0 = 0.2f, a1 = 0.4f, a2 = 0.2f, b1 = -0.3f, b2 = 0.1f, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    float run(float x0) {
        float y0 = (a0 * x0) + (a1 * x1) + (a2 * x2) - (b1 * y1) - (b2 * y2);
        x2 = x1; x1 = x0; y2 = y1; y1 = y0;
        return y0;
    }
};
struct Overdrive {
    float gain = 8.0f, threshold = 0.6f, alpha = 0.1f, hp_z1 = 0, dry = 0.25f, wet = 0.75f;
    Biquad lp;
    float process(float input) {
        float hp_output = input - hp_z1;
        hp_z1 = (input * alpha) + (hp_z1 * (1.0f - alpha));
        float drive = hp_output * gain, shaped;
        float th = (2.0f / 3.0f) * threshold;
        if (drive < -threshold) shaped = -th;
        else if (drive > threshold) shaped = th;
        else shaped = drive - ((drive * drive * drive) * 0.33333333f);
        float y0 = lp.run(shaped);
        return y0 * wet + input * dry;
    }
};
// stock atanFast() (Global.h ~246): 1024-entry table over [0,1], linear interpolation, atan(x) = pi/2 - atan(1/x) above 1
inline float atanFast(float in) {
    static float tab[1024];
    static bool built = false;
    if (!built) { for (int i = 0; i < 1024; ++i) tab[i] = atanf(i / 1024.0f); built = true; }
    bool neg = false, inv = false;
    if (in < 0.0f) { neg = true; in = -in; }
    if (in > 1.0f) { inv = true; in = 1.0f / in; }
    float f = in * 1024.0f;
    int32_t i = (int32_t)f;
    if (i >= 1023) i = 1022;
    float r = tab[i] + ((f - (float)i) * (tab[i + 1] - tab[i]));
    if (inv) r = 1.570796327f - r;
    return neg ? -r : r;
}
struct Distortion {
    float gain = 20.0f, threshold = 0.5f, bias = 0.1f, dry = 0.25f, wet = 0.75f;
    Biquad lp;
    float process(float input) {
        float drive = (input * gain) + bias, shaped;
        if (gain > 0.0001f) {
            shaped = ((2.0f / (float)M_PI) * atanFast(drive)) * threshold;
            shaped -= (bias * 0.5f);
        } else {
            shaped = input * threshold;
        }
        return lp.run(shaped) * wet + input * dry;
    }
};

// ---------------------------------------------------------------------------------------------------- REVERB (Freeverb: 8 combs, 4 allpass, surround delay)
struct Reverb {
    static constexpr uint16_t kPre = 4500, kSur = 900;
    static constexpr uint16_t combSize[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static constexpr uint16_t apSize[4] = {225, 556, 441, 341};
    std::vector<float> pre, sur, comb[8], ap[4];
    uint16_t preP = 0, preR = 0, surP = 0, surR = 0, ci[8] = {}, ai[4] = {};
    float cf[8] = {}, cfb, cd1, cd2, apfb = 0.35f, dry = 1.0f, wet = 0.5f;
    void init(int size = 25, int decay = 50, int damping = 30, float preDelayS = 0.005f, float surroundS = 0.005f) {
        pre.assign(kPre, 0); sur.assign(kSur, 0);
        for (int i = 0; i < 8; ++i) comb[i].assign(combSize[i], 0);
        for (int i = 0; i < 4; ++i) ap[i].assign(apSize[i], 0);
        cfb = 0.70f + (size / 100.0f * 0.15f) + (decay / 100.0f * 0.13f);
        cd1 = 0.05f + (damping / 100.0f * 0.45f);
        cd2 = 1.0f - cd1;
        preR = (uint16_t)(kSr * preDelayS);
        surR = (uint16_t)(kSr * surroundS);
    }
    static float comb1(std::vector<float>& b, uint16_t& idx, uint16_t size, float& fs, float in, float fb, float d1, float d2) {
        float out = b[idx];
        fs = (out * d2) + (fs * d1);
        b[idx] = in + (fs * fb);
        if (++idx >= size) idx = 0;
        return out;
    }
    static float allp(std::vector<float>& b, uint16_t& idx, uint16_t size, float in, float fb) {
        float bv = b[idx];
        float out = bv - in;
        b[idx] = in + (bv * fb);
        if (++idx >= size) idx = 0;
        return out;
    }
    void process(float in, float& outL, float& outR) {
        float x = in + 1.0e-9f;
        pre[preR] = x;
        float rin = pre[preP];
        if (++preP >= kPre) preP = 0;
        if (++preR >= kPre) preR = 0;
        float ic = rin * 0.015f, sum = 0;
        for (int i = 0; i < 8; ++i) sum += comb1(comb[i], ci[i], combSize[i], cf[i], ic, cfb, cd1, cd2);
        float a = sum;
        for (int i = 0; i < 4; ++i) a = allp(ap[i], ai[i], apSize[i], a, apfb);
        float mixed = a * wet + x * dry;
        sur[surR] = mixed;
        outL = mixed;
        outR = sur[surP];
        if (++surP >= kSur) surP = 0;
        if (++surR >= kSur) surR = 0;
    }
};

// ---------------------------------------------------------------------------------------------------- PHASER (2 biquad allpass, LFO by table sine)
inline float sinFast(float ph) {      // Global.h ~225: 1024-entry table, linear interpolation; the stock code wraps with while loops
    static float tab[1024];
    static bool built = false;
    if (!built) { for (int i = 0; i < 1024; ++i) tab[i] = sinf(6.2831853f * i / 1024.0f); built = true; }
    while (ph >= 1.0f) ph -= 1.0f;
    while (ph < 0.0f) ph += 1.0f;
    float pos = ph * 1024.0f;
    int a = (int)pos, b = (a + 1) & 1023;
    return tab[a] + ((pos - (float)a) * (tab[b] - tab[a]));
}
inline float cosFast(float ph) { return sinFast(ph + 0.25f); }
struct Phaser {
    float rate = 0.5f, feedback = 0.475f, center = 1000.0f, depth = 800.0f, dataX = 0, dataY = 0, Ts = 1.0f / kSr, Q = 0.5f, ff[4] = {}, fb[4] = {}, last = 0, dry = 0.75f, wet = 0.75f;
    float process(float input) {
        dataX += Ts;
        dataY = rate * dataX;
        if (dataY >= 1.0f) { dataX = 0.0f; dataY = 0.0f; }
        float lfoOsc = sinFast(dataY);
        float lfoFreq = (depth * lfoOsc) + center;
        float stage1Input = input + last * feedback;
        float normFreq = lfoFreq * (1.0f / kSr);
        float sinVal = sinFast(normFreq), cosVal = cosFast(normFreq);
        float alpha = sinVal / (2.0f * Q);
        float inv_a0 = 1.0f / (1.0f + alpha);
        float c_side = (1.0f - alpha) * inv_a0, c_mid = (-2.0f * cosVal) * inv_a0;
        float s1 = (c_side * stage1Input) + (c_mid * ff[0]) + ff[1] - (c_mid * fb[0]) - (c_side * fb[1]);
        ff[1] = ff[0]; ff[0] = stage1Input; fb[1] = fb[0]; fb[0] = s1;
        float s2 = (c_side * s1) + (c_mid * ff[2]) + ff[3] - (c_mid * fb[2]) - (c_side * fb[3]);
        ff[3] = ff[2]; ff[2] = s1; fb[3] = fb[2]; fb[2] = s2;
        if (fabsf(s2) < 1.0e-18f) s2 = 0.0f;
        last = s2;
        return s2 * wet + input * dry;
    }
};

// ---------------------------------------------------------------------------------------------------- BITCRUSHER (fold / clip, bit mask, sample-rate counter, smoothing LP)
struct Bitcrusher {
    uint32_t counter = 0, maxCount = 4, resMask = 0xFFF00000u;
    float sample = 0, limPos = 0.5f, limNeg = -0.5f, lastOut = 0, smoothAlpha = 0.5f, dry = 0.0f, wet = 1.0f;
    bool fold = true;
    float process(float input) {
        counter++;
        if (counter >= maxCount) {
            counter = 0;
            float p = input;
            if (fabsf(p) < 0.002f) {
                sample = 0.0f;
            } else {
                if (fold) {
                    while (p > limPos || p < limNeg) {
                        if (p > limPos) p = limPos - (p - limPos);
                        if (p < limNeg) p = limNeg - (p - limNeg);
                    }
                } else {
                    if (p > limPos) p = limPos; else if (p < limNeg) p = limNeg;
                }
                float n = (p + 1.0f) * 0.5f;
                uint32_t scaled = (uint32_t)(n * 16777215.0f);
                scaled &= resMask;
                sample = ((float)scaled * 5.96046448e-8f * 2.0f) - 1.0f;
            }
        }
        float f = lastOut + smoothAlpha * (sample - lastOut);
        lastOut = f;
        return f * wet + input * dry;
    }
};

}  // namespace stock
