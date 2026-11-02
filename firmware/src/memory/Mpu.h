#ifndef __MPU_H
#define __MPU_H

#include <stddef.h>
#include <stdint.h>

// Memory protection / cache set-up for the STM32H723 (Cortex-M7, 32 KB I-cache + 32 KB D-cache).
//
// The D-cache is ON by default (CMake option DBOY_DCACHE; the SD driver's cache maintenance follows it). Measured on the board with the
// default project playing: 877% of the audio budget with the cache off (the GUI froze), 125-147% with it on. The MPU itself is ALWAYS
// configured, cache or not (mpuConfigure(false) with it off): region 4 makes the SDRAM Normal memory, because the default map treats
// 0xC0000000 as Device memory, where an unaligned access is a UsageFault (first run on the board).
//
// Map, highest region number wins where regions overlap:
//   region 1  dmaBuffers (1 KB, AXI SRAM)   Normal, NON-cacheable, shareable   <- I2S DMA
//   region 2  0x38000000 (16 KB, SRAM4)     Normal, NON-cacheable, shareable   <- BDMA / I2C scratch
//   region 3  0xC0000000 (256 MB)           NO ACCESS                          <- guard, see below
//   region 4  0xC0000000 (16 MB, SDRAM)     Normal; cacheable write-back write-allocate with the D-cache, else non-cacheable
// Everything else uses the default map (flash and AXI SRAM cacheable, TCM uncached, peripherals device).
//
// Why the guard: the Cortex-M7 may speculatively read any address in a cacheable region's neighbourhood.
// The FMC decodes 256 MB at 0xC0000000 but only 16 MB is populated; a speculative read of the rest can
// stall or fault the bus. Region 3 makes those addresses inaccessible, region 4 re-enables the real chip.
//
// RULE for anyone adding DMA that targets SDRAM (SD card samples, display): the DMA write bypasses the cache,
// so call memory::invalidateAfterDma(ptr, len) before the CPU reads it, and memory::cleanBeforeDma(ptr, len)
// before a DMA reads data the CPU wrote.
namespace memory {

// Configure the MPU and, if `dataCache`, enable the D-cache with the SDRAM cacheable. Call after Sdram_Initialize().
void mpuConfigure(bool dataCache);

void cleanBeforeDma(const void* p, size_t len);      // CPU wrote it, DMA will read it
void invalidateAfterDma(void* p, size_t len);        // DMA wrote it, CPU will read it

}  // namespace memory

#endif
