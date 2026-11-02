#ifndef __SDRAM_HEAP_H
#define __SDRAM_HEAP_H

#include <stddef.h>
#include <stdint.h>

// C++ heap in the external 16 MB SDRAM (0xC0000000). CATHODE's prepare() allocates ~1.7 MB, far more than
// the ~180 KB of internal RAM left after .bss, so every `new` made after sdramHeapEnable() lands here.
//
// It is a bump allocator and delete() of an SDRAM block is a no-op. That is deliberate: the engine allocates
// everything in prepare() and nothing afterwards (measured on the host: 0 allocations while rendering), so
// there is nothing to reuse and no fragmentation to manage. If that ever changes, bytesUsed() will show it.
//
// Performance warning: with the D-cache off (current state) every access to this memory goes out over the
// 16-bit FMC bus. The first on-target cycle counts will therefore be pessimistic; see docs/OPTIMIZATION_PLAN.md
// Phase 3 (MPU + D-cache, shared tables, hot state in DTCM).
namespace memory {

void sdramHeapEnable();               // call after Sdram_Initialize()
size_t sdramHeapBytesUsed();
size_t sdramHeapBytesFree();
bool inSdram(const void* p);

}  // namespace memory

#endif
