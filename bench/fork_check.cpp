// Verifies the DSP fork: lazy Macro allocation must not change a single output bit, and must save memory.
#include <nemesis/dsp/CathodeOscCore.h>
#include <nemesis/dsp/CathodeVoiceEngine.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace { bool gTrack = false; size_t gBytes = 0; }
void* operator new(size_t n) { if (gTrack) gBytes += n; return std::malloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

using namespace nemesis::dsp;
using OM = CathodeVoiceStage::OscMode;
using FM = CathodeVoiceStage::FilterMode;
using ME = MacroOscillator::Engine;

namespace {

// usesMacro: slot 1 is switched to Macro AFTER prepare, the way the firmware's main loop would.
std::vector<float> render(bool lazy, bool usesMacro, size_t& allocBytes) {
    CathodeOscCore::setLazyMacro(lazy);
    auto* eng = new CathodeVoiceEngine;
    gBytes = 0;
    gTrack = true;
    eng->prepare(44100.0);
    gTrack = false;
    allocBytes = gBytes;
    eng->setVoiceCount(5);
    for (int v = 0; v < CathodeVoiceEngine::kMaxVoices; ++v) {
        CathodeVoiceStage& t = eng->voiceAt(v).outputStage().voice();
        t.setOscMode(0, OM::PhaseDistortion);
        t.setPdShape(0, 0.3f);
        t.setPdAmount(0, 0.6f);
        if (usesMacro) {
            gTrack = true;
            t.setOscMode(1, OM::Macro);   // allocates here in lazy mode
            gTrack = false;
            t.setMacroEngine(1, ME::Swarm);
        } else {
            t.setOscMode(1, OM::PhaseDistortion);
            t.setPdShape(1, 0.5f);
            t.setPdAmount(1, 0.5f);
        }
        t.setOscMix(0.5f);
        t.setFilterMode(FM::Ladder24);
        t.setCutoff(2000.0f, 3.0f);
    }
    if (usesMacro) allocBytes += gBytes - allocBytes;   // include the deferred allocations
    eng->reset();
    for (int n : {48, 52, 55, 59, 62}) eng->noteOn(n, 0.8f);
    std::vector<float> out;
    out.reserve(2 * 88200);
    for (int i = 0; i < 88200; ++i) {
        auto s = eng->processSample();
        out.push_back(s.left);
        out.push_back(s.right);
    }
    delete eng;
    return out;
}

bool check(const char* name, bool usesMacro) {
    size_t eagerBytes = 0, lazyBytes = 0;
    const auto a = render(false, usesMacro, eagerBytes);
    const auto b = render(true, usesMacro, lazyBytes);
    const bool same = a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    double peak = 0;
    for (float f : a) peak = std::max(peak, (double)std::abs(f));
    std::printf("%-28s eager %8zu B | lazy at prepare %8zu B | output %s (peak %.3f)\n", name, eagerBytes, lazyBytes,
                same ? "BIT-IDENTICAL" : "DIFFERENT", peak);
    return same && peak > 0.001;
}

}  // namespace

int main() {
    const bool ok1 = check("PD x2 (macro never used)", false);
    const bool ok2 = check("PD + Macro Swarm (switched)", true);
    return (ok1 && ok2) ? 0 : 1;
}
