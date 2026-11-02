#include "memory/SdramHeap.h"

#include <malloc.h>
#include <new>
#include <stdlib.h>

extern "C" void Error_Handler(void);

namespace {

constexpr uintptr_t kBase = 0xC0000000u;
constexpr size_t kSize = 16u * 1024u * 1024u;
constexpr size_t kAlign = 32;                 // one Cortex-M7 cache line

volatile bool gEnabled = false;
uintptr_t gNext = kBase;

void* sdramAlloc(size_t n, size_t align = kAlign) {
    if (align < kAlign) align = kAlign;
    uintptr_t p = (gNext + (align - 1)) & ~(uintptr_t)(align - 1);
    if (p + n > kBase + kSize) Error_Handler();   // out of SDRAM: fail loudly at start-up, never at run time
    gNext = p + n;
    return (void*)p;
}

}  // namespace

namespace memory {

void sdramHeapEnable() { gEnabled = true; }
size_t sdramHeapBytesUsed() { return gNext - kBase; }
size_t sdramHeapBytesFree() { return kSize - (gNext - kBase); }
bool inSdram(const void* p) { return (uintptr_t)p >= kBase && (uintptr_t)p < kBase + kSize; }

}  // namespace memory

// Before sdramHeapEnable() (static constructors run before SDRAM exists) fall back to the newlib heap.
void* operator new(size_t n) {
    if (n == 0) n = 1;
    if (gEnabled) return sdramAlloc(n);
    void* p = malloc(n);
    if (!p) Error_Handler();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { if (p && !memory::inSdram(p)) free(p); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }

// The aligned forms (a type with alignas(16) members, such as CathodePart's lanes) are separate replaceable
// functions: left alone they go to newlib's memalign, which only has the small internal heap, and a 160 KB
// object makes it return null (then the library aborts). Found on the first run on the board.
void* operator new(size_t n, std::align_val_t al) {
    if (n == 0) n = 1;
    if (gEnabled) return sdramAlloc(n, (size_t)al);
    void* p = memalign((size_t)al, n);
    if (!p) Error_Handler();
    return p;
}
void* operator new[](size_t n, std::align_val_t al) { return operator new(n, al); }
void* operator new(size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return operator new(n, al); }
void* operator new[](size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return operator new(n, al); }
void operator delete(void* p, std::align_val_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::align_val_t) noexcept { operator delete(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { operator delete(p); }
