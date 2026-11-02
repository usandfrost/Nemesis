#ifndef __TEST_TONE_H
#define __TEST_TONE_H

#include "audio/AudioEngine.h"

// Milestone-1 sanity check: a few detuned sine "voices" from a rotating-phasor recurrence (no libm
// in the audio path). The voice count doubles as a crude load generator for the CPU meter.
class TestTone : public AudioRenderer {
   public:
    static const int kMaxVoices = 16;

    void setVoices(int n) { voices_ = n < 1 ? 1 : (n > kMaxVoices ? kMaxVoices : n); }
    void setLevel(float l) { level_ = l; }
    void init(float baseHz, float sampleRate);

    void render(float* left, float* right, int frames) override;

   private:
    struct Phasor {
        float c = 1.0f, s = 0.0f;       // current rotation state
        float cosW = 1.0f, sinW = 0.0f; // per-sample rotation
    };
    Phasor ph_[kMaxVoices];
    int voices_ = 5;
    float level_ = 0.1f;
};

#endif
