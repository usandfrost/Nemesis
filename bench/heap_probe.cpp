// Counts heap bytes requested while a CathodeVoiceEngine is prepared, so the firmware's heap can be sized.
// Uses a fixed table (not std::map) because it records from inside operator new.
#include <nemesis/dsp/CathodeVoiceEngine.h>
#include <nemesis/dsp/CathodeOscCore.h>
#include <nemesis/dsp/ForkConfig.h>

#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
struct Slot { size_t size = 0, count = 0; };
Slot gSlots[256];
bool gTrack = false;
size_t gBytes = 0, gCalls = 0;

void record(size_t n) {
    ++gCalls;
    gBytes += n;
    for (Slot& s : gSlots) {
        if (s.count && s.size == n) { ++s.count; return; }
        if (!s.count) { s.size = n; s.count = 1; return; }
    }
}
}

void* operator new(size_t n) { if (gTrack) record(n); return std::malloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

int main() {
#ifdef DSP_IS_FORK
    if (std::getenv("HEAP_NO_TWIN")) nemesis::dsp::fork::Config::stereoTwin = false;
    nemesis::dsp::fork::Config::lanes = false;
#endif
    auto* eng = new nemesis::dsp::CathodeVoiceEngine;
    std::printf("sizeof(CathodeVoiceEngine) = %zu bytes\n", sizeof(*eng));
#ifdef DSP_IS_FORK
    nemesis::dsp::CathodeOscCore::setLazyMacro(LAZY);
#endif
    gTrack = true;
    eng->prepare(44100.0);
    gTrack = false;
    for (const Slot& s : gSlots)
        if (s.count && (s.count >= 8 || s.size >= 4096))
            std::printf("  size %8zu x %4zu = %9zu B\n", s.size, s.count, s.size * s.count);
    std::printf("prepare(): %zu allocations, %zu bytes total\n", gCalls, gBytes);
}
