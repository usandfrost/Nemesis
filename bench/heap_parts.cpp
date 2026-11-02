// Per-component heap cost of prepare(), to see which parts dominate the 1.7 MB.
#include <nemesis/dsp/CathodeModulatedVoice.h>
#include <nemesis/dsp/CathodeOscCore.h>
#include <nemesis/dsp/CathodeOutputStage.h>
#include <nemesis/dsp/CathodeVoiceStage.h>
#include <nemesis/dsp/MacroOscillator.h>
#include <nemesis/dsp/Oversampler.h>
#include <nemesis/dsp/PhaseDistortionOscillator.h>
#include <nemesis/dsp/WavetableOscillator.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>

namespace { bool gTrack = false; size_t gBytes = 0, gCalls = 0; }
void* operator new(size_t n) { if (gTrack) { gBytes += n; ++gCalls; } return std::malloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static void measure(const char* name, const std::function<void()>& f) {
    gBytes = gCalls = 0;
    gTrack = true;
    f();
    gTrack = false;
    std::printf("%-34s %5zu allocs %9zu B\n", name, gCalls, gBytes);
}

using namespace nemesis::dsp;
int main() {
    const double sr = 44100.0;
    { PhaseDistortionOscillator o; measure("PhaseDistortionOscillator.prepare", [&] { o.prepare(sr, PhaseDistortionOscillator::kDefaultOversample); }); }
    { WavetableOscillator o; measure("WavetableOscillator.setSampleRate", [&] { o.setSampleRate(sr); }); }
    { MacroOscillator o; measure("MacroOscillator.prepare", [&] { o.prepare(sr); }); }
    { CathodeOscCore o; measure("CathodeOscCore.prepare (2 slots)", [&] { o.prepare(sr); }); }
    { CathodeVoiceStage o; measure("CathodeVoiceStage.prepare", [&] { o.prepare(sr); }); }
    { CathodeOutputStage o; measure("CathodeOutputStage.prepare", [&] { o.prepare(sr); }); }
    { CathodeModulatedVoice o; measure("CathodeModulatedVoice.prepare", [&] { o.prepare(sr); }); }
}
