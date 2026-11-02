#include "audio/TestTone.h"

#include <math.h>

void TestTone::init(float baseHz, float sampleRate) {
    for (int v = 0; v < kMaxVoices; ++v) {
        const float hz = baseHz * (1.0f + 0.0035f * v) * (1.0f + 0.5f * (v % 3));
        const float w = 2.0f * 3.14159265f * hz / sampleRate;
        ph_[v].cosW = cosf(w);
        ph_[v].sinW = sinf(w);
        ph_[v].c = 1.0f;
        ph_[v].s = 0.0f;
    }
}

void TestTone::render(float* left, float* right, int frames) {
    const float gain = level_ / (float)voices_;
    for (int i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (int v = 0; v < voices_; ++v) {
            Phasor& p = ph_[v];
            const float c = p.c * p.cosW - p.s * p.sinW;
            const float s = p.s * p.cosW + p.c * p.sinW;
            p.c = c;
            p.s = s;
            sum += s;
        }
        left[i] = right[i] = sum * gain;
    }
    // Phasors drift in magnitude over time; renormalise once per block (cheap, voices_ sqrt-free).
    for (int v = 0; v < voices_; ++v) {
        Phasor& p = ph_[v];
        const float n = 1.5f - 0.5f * (p.c * p.c + p.s * p.s);
        p.c *= n;
        p.s *= n;
    }
}
