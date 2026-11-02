#include "memory/Mpu.h"

#include "main.h"
#include "memory/DmaBuffers.h"

namespace {

// Cache line is 32 bytes; maintenance by address must cover whole lines.
constexpr uintptr_t kLine = 32;

// SIZE field = log2(bytes) - 1.
constexpr uint8_t sizeField(uint32_t bytes) {
    uint8_t n = 0;
    while ((1u << (n + 1)) < bytes) ++n;
    return n;
}

void setRegion(uint8_t number, uint32_t base, uint32_t bytes, uint8_t access, uint8_t typeExt, bool cacheable,
               bool bufferable, bool shareable, bool executeNever) {
    MPU_Region_InitTypeDef r = {};
    r.Enable = MPU_REGION_ENABLE;
    r.Number = number;
    r.BaseAddress = base;
    r.Size = sizeField(bytes);
    r.SubRegionDisable = 0x00;
    r.TypeExtField = typeExt;
    r.AccessPermission = access;
    r.DisableExec = executeNever ? MPU_INSTRUCTION_ACCESS_DISABLE : MPU_INSTRUCTION_ACCESS_ENABLE;
    r.IsShareable = shareable ? MPU_ACCESS_SHAREABLE : MPU_ACCESS_NOT_SHAREABLE;
    r.IsCacheable = cacheable ? MPU_ACCESS_CACHEABLE : MPU_ACCESS_NOT_CACHEABLE;
    r.IsBufferable = bufferable ? MPU_ACCESS_BUFFERABLE : MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&r);
}

}  // namespace

namespace memory {

void mpuConfigure(bool dataCache) {
    HAL_MPU_Disable();

    // 1. I2S DMA buffers: Normal memory, TEX=1 with C=0 B=0 = non-cacheable.
    setRegion(1, (uint32_t)&dmaBuffers, sizeof(DmaBuffers), MPU_REGION_FULL_ACCESS, MPU_TEX_LEVEL1, false, false,
              true, true);

    // 2. SRAM4 (D3): the BDMA / I2C scratch lives here. Non-cacheable for the same reason.
    setRegion(2, 0x38000000, 16 * 1024, MPU_REGION_FULL_ACCESS, MPU_TEX_LEVEL1, false, false, true, true);

    // 3. FMC SDRAM bank 1 address space: no access (guard against speculative reads of unpopulated space).
    setRegion(3, 0xC0000000, 256u * 1024u * 1024u, MPU_REGION_NO_ACCESS, MPU_TEX_LEVEL0, false, false, false, true);

    // 4. The 16 MB that exists: Normal memory, not shareable. With the data cache on it is write-back write-allocate
    //    (TEX=1, C=1, B=1); with it off, non-cacheable (TEX=1, C=0, B=0). Either way it is NORMAL memory, which the
    //    CPU may access unaligned. Without this region the SDRAM is Device memory (the default map for 0xC0000000)
    //    and an unaligned store (a packed struct member in SDRAM) is a UsageFault.
    setRegion(4, 0xC0000000, 16u * 1024u * 1024u, MPU_REGION_FULL_ACCESS, MPU_TEX_LEVEL1, dataCache, dataCache, false,
              true);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);   // keep the default map for everything not listed above

    if (dataCache) {
        SCB_InvalidateDCache();
        SCB_EnableDCache();
    }
}

void cleanBeforeDma(const void* p, size_t len) {
    const uintptr_t start = (uintptr_t)p & ~(kLine - 1);
    const uintptr_t end = ((uintptr_t)p + len + kLine - 1) & ~(kLine - 1);
    SCB_CleanDCache_by_Addr((uint32_t*)start, (int32_t)(end - start));
}

void invalidateAfterDma(void* p, size_t len) {
    const uintptr_t start = (uintptr_t)p & ~(kLine - 1);
    const uintptr_t end = ((uintptr_t)p + len + kLine - 1) & ~(kLine - 1);
    SCB_InvalidateDCache_by_Addr((uint32_t*)start, (int32_t)(end - start));
}

}  // namespace memory
