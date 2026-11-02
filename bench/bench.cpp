// Host-side CPU estimate for the CATHODE voice engine (stage 1 of the Drumboy port benchmark).
// Reports ns/sample and the % of one core at 48 kHz for N held voices, per patch.
#include <nemesis/dsp/CathodeVoiceEngine.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace nemesis::dsp;
using OM = CathodeVoiceStage::OscMode;
using FM = CathodeVoiceStage::FilterMode;
using ME = MacroOscillator::Engine;

namespace
{
    constexpr double kSr = 48000.0;

    struct Patch
    {
        std::string name;
        std::function<void (CathodeVoiceStage&)> setup;
    };

    void pd (CathodeVoiceStage& t, FM f)
    {
        for (int s = 0; s < 2; ++s)
        {
            t.setOscMode (s, OM::PhaseDistortion);
            t.setPdShape (s, 0.3f + 0.2f * s);
            t.setPdAmount (s, 0.6f);
        }
        t.setSemitones (1, 7);
        t.setOscMix (0.5f);
        t.setFilterMode (f);
        t.setCutoff (2000.0f, 3.0f);
    }

    void macro (CathodeVoiceStage& t, ME e, FM f)
    {
        for (int s = 0; s < 2; ++s)
        {
            t.setOscMode (s, OM::Macro);
            t.setMacroEngine (s, e);
        }
        t.setSemitones (1, 7);
        t.setOscMix (0.5f);
        t.setFilterMode (f);
        t.setCutoff (2000.0f, 3.0f);
    }

    double run (const Patch& p, int voices, int seconds)
    {
        CathodeVoiceEngine eng;
        eng.prepare (kSr);
        eng.setVoiceCount (voices);
        for (int i = 0; i < CathodeVoiceEngine::kMaxVoices; ++i)
            p.setup (eng.voiceAt (i).outputStage().voice());
        eng.reset();
        for (int n = 0; n < voices; ++n)
            eng.noteOn (48 + n * 4, 0.8f);

        const long total = static_cast<long> (kSr) * seconds;
        float sink = 0.0f;
        for (long i = 0; i < 4800; ++i) // warm-up
            sink += eng.processSample().left;
        const auto t0 = std::chrono::steady_clock::now();
        for (long i = 0; i < total; ++i)
            sink += eng.processSample().left;
        const auto t1 = std::chrono::steady_clock::now();
        if (sink == 12345.678f)
            std::puts ("");
        return std::chrono::duration<double, std::nano> (t1 - t0).count() / static_cast<double> (total);
    }
}

int main()
{
    const std::vector<Patch> patches = {
        { "PD x2 / LowPass", [] (auto& t) { pd (t, FM::LowPass); } },
        { "PD x2 / Ladder24", [] (auto& t) { pd (t, FM::Ladder24); } },
        { "PD x2 / Acid", [] (auto& t) { pd (t, FM::Acid); } },
        { "Macro Analog x2 / LowPass", [] (auto& t) { macro (t, ME::Analog, FM::LowPass); } },
        { "Macro Tables x2 / LowPass", [] (auto& t) { macro (t, ME::Tables, FM::LowPass); } },
        { "Macro Swarm x2 / LowPass", [] (auto& t) { macro (t, ME::Swarm, FM::LowPass); } },
        { "Macro Grains x2 / LowPass", [] (auto& t) { macro (t, ME::Grains, FM::LowPass); } },
        { "Macro Modal x2 / Ladder24", [] (auto& t) { macro (t, ME::Modal, FM::Ladder24); } },
        { "Macro Chords x2 / Formant", [] (auto& t) { macro (t, ME::Chords, FM::Formant); } },
    };
    std::printf ("%-30s %7s %12s %10s\n", "patch", "voices", "ns/sample", "%core@48k");
    for (const auto& p : patches)
        for (int v : { 1, 8, 16 })
        {
            const double ns = run (p, v, 3);
            std::printf ("%-30s %7d %12.0f %9.1f%%\n", p.name.c_str(), v, ns, ns * kSr / 1e9 * 100.0);
        }
}
