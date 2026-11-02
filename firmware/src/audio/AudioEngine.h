#ifndef __AUDIO_ENGINE_H
#define __AUDIO_ENGINE_H

#include <stdint.h>

#include "Global.h"
#include "audio/AudioRenderer.h"
#include "audio/Recorder.h"
#include "perf/CycleCounter.h"

// Owns the I2S DMA double buffer. The half/full-transfer interrupts each hand the engine one
// I2S_BLOCK_SIZE block; the engine renders it, converts to the codec's 32-bit format and records
// the cycles spent. Rendering happens in the interrupt, so nothing else in the firmware can starve it.
class AudioEngine {
   public:
    void setRenderer(AudioRenderer* r) { renderer_ = r; }
    void setRecorder(audio::Recorder* r) { recorder_ = r; }

    // Called from HAL_I2SEx_TxRxHalfCpltCallback / TxRxCpltCallback. `half` is 0 for the first
    // half-buffer, 1 for the second.
    void onBlock(int half);

    // Budget for one block in CPU cycles at the current core clock (the "100%" of the CPU meter).
    uint32_t budgetCycles() const;

    // CPU load of the last / worst block as a fraction of the budget.
    float loadLast() const { return (float)stats.last / (float)budgetCycles(); }
    float loadPeak() const { return (float)stats.max / (float)budgetCycles(); }

    perf::BlockStats stats;
    volatile uint32_t i2sOverruns = 0;
    volatile uint32_t i2sUnderruns = 0;

   private:
    AudioRenderer* renderer_ = nullptr;
    audio::Recorder* recorder_ = nullptr;
    float left_[I2S_BLOCK_SIZE];
    float right_[I2S_BLOCK_SIZE];
};

extern AudioEngine audioEngine;

#endif
