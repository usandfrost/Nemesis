#include "audio/AudioEngine.h"

#include "main.h"
#include "memory/DmaBuffers.h"

extern uint32_t SystemCoreClock;

AudioEngine audioEngine;

uint32_t AudioEngine::budgetCycles() const {
    // A block must finish before the next one is due: frames / sampleRate seconds of core clock.
    return (uint32_t)((double)SystemCoreClock * I2S_BLOCK_SIZE / AUDIO_SAMPLE_RATE);
}

static inline int32_t toInt32(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int32_t)(v * 2147483520.0f);   // just under 2^31 so +1.0 never overflows
}

void AudioEngine::onBlock(int half) {
    const uint32_t t0 = perf::cycles();

    // The HAL leaves the I2S error interrupts disabled, so poll them here (same approach as the
    // stock firmware): an rx overrun can shift word alignment for good, so it must be counted.
    const uint32_t sr = SPI3->SR;
    if (sr & SPI_SR_OVR) {
        ++i2sOverruns;
        SPI3->IFCR = SPI_IFCR_OVRC;
    }
    if (sr & SPI_SR_UDR) {
        ++i2sUnderruns;
        SPI3->IFCR = SPI_IFCR_UDRC;
    }

    if (renderer_) {
        renderer_->beginBlock();
        renderer_->render(left_, right_, I2S_BLOCK_SIZE);
    } else {
        for (int i = 0; i < I2S_BLOCK_SIZE; ++i) left_[i] = right_[i] = 0.0f;
    }

    if (recorder_) recorder_->block(&i2s_rxBuffer[half * I2S_BLOCK_SIZE * I2S_CHANNEL_COUNT], left_, right_, I2S_BLOCK_SIZE);

    uint32_t* out = &i2s_txBuffer[half * I2S_BLOCK_SIZE * I2S_CHANNEL_COUNT];
    for (int i = 0; i < I2S_BLOCK_SIZE; ++i) {
        out[2 * i] = (uint32_t)toInt32(left_[i]);
        out[2 * i + 1] = (uint32_t)toInt32(right_[i]);
    }

    stats.add(perf::cycles() - t0, budgetCycles());
}
