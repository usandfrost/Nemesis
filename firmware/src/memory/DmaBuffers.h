#ifndef __DMA_BUFFERS_H
#define __DMA_BUFFERS_H

#include <stdint.h>

#include "Global.h"

// The I2S DMA buffers, in ONE 1 KB block aligned to 1 KB. That is exactly what one MPU region needs to mark
// them non-cacheable (a region must be a power-of-two size, aligned to its size). With the D-cache on, a
// cacheable DMA buffer means the CPU reads stale audio / the DMA reads stale output, so this block is the
// one place in AXI SRAM that must stay uncached. memory::mpuConfigure() maps it.
//
// 2 buffers x I2S_TOTAL_BUFFER_SIZE words x 4 B: 1 KB at the default 32-frame block, 2 KB at 64, 4 KB at 128 (always a power of two).
constexpr unsigned kDmaBufferBytes = 2u * I2S_TOTAL_BUFFER_SIZE * 4u;
struct alignas(kDmaBufferBytes) DmaBuffers {
    uint32_t rx[I2S_TOTAL_BUFFER_SIZE];
    uint32_t tx[I2S_TOTAL_BUFFER_SIZE];
};
static_assert(sizeof(DmaBuffers) == kDmaBufferBytes && (kDmaBufferBytes & (kDmaBufferBytes - 1)) == 0,
              "DmaBuffers must be exactly one MPU region: a power-of-two size, aligned to its size (memory::mpuConfigure sizes the region from it)");

extern DmaBuffers dmaBuffers;

// Existing code (codec driver, audio engine) refers to the buffers by these names.
#define i2s_rxBuffer (dmaBuffers.rx)
#define i2s_txBuffer (dmaBuffers.tx)

#endif
