// upstream_render: the ORIGINAL CATHODE machine (ref/upstream/cathode/src/CathodeMachine.cpp, built unmodified) playing the same phrases as
// render_presets, for the A/B on the listening page. It is the synth engine only: the preset's FX chain (reverb, delay, chorus, ...) lives in the
// upstream app layer, which is not in the clone, so the original renders DRY. Per-stage state, voices, mod matrix, 16 slots, wavetables: all there.
//   stdin: file<TAB>preset name<TAB>category<TAB>index<TAB>out.wav     one per line (file = a preset JSON from the archive)
#include <nemesis/cathode/CathodeMachine.h>
#include <nemesis/core/MachineJson.h>
#include <nemesis/core/Presets.h>
#include <nemesis/machine/OwnedPatchSlot.h>

#include <fstream>
#include <iostream>
#include <sstream>

#include "../phrases.h"

using namespace nemesis;

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string t;
        while (std::getline(ss, t, '\t')) f.push_back(t);
        if (f.size() != 5) continue;
        std::ifstream in(f[0]);
        std::stringstream buf;
        buf << in.rdbuf();
        const auto r = core::loadFactoryBanks(buf.str());
        if (!r.banks) { printf("{\"i\":%s,\"error\":\"load: %s\"}\n", f[3].c_str(), r.message.c_str()); continue; }
        const core::Preset* pr = nullptr;
        for (const auto& m : r.banks->machines)
            if (m.first == "cathode")
                for (const auto& p : m.second)
                    if (p.name == f[1]) pr = &p;
        if (!pr) { printf("{\"i\":%s,\"error\":\"no preset\"}\n", f[3].c_str()); continue; }
        auto* machine = new app::CathodeMachine;   // fresh per preset
        machine::OwnedPatchSlot patch{app::CathodeMachine::machineTypeName()};
        machine->prepare(phr::kSr, &patch);
        patch.replace(core::resolveMachinePreset("cathode", *pr));
        patch.setPresetName(pr->name);
        machine->onDocumentLoaded(&patch, true);
        machine->reset();
        const phr::Phrase ph = phr::phraseFor(f[2].c_str());
        struct Ev { int at; bool on; size_t n; };
        std::vector<Ev> evs;
        for (size_t k = 0; k < ph.notes.size(); ++k) {
            evs.push_back({(int)(ph.notes[k].t * phr::kSr), true, k});
            evs.push_back({(int)((ph.notes[k].t + ph.notes[k].d) * phr::kSr), false, k});
        }
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
        const int block = 32, total = (int)(ph.len * phr::kSr) / block * block;
        std::vector<float> mono(total);
        size_t e = 0;
        bool finite = true;
        float l[block], rr[block];
        for (int b0 = 0; b0 < total; b0 += block) {
            machine->beginBlock();
            // notes land on a block boundary (<= 0.7 ms early/late), the chunk contract is one event per chunk
            while (e < evs.size() && evs[e].at < b0 + block) {
                const auto& ev = evs[e++];
                if (ev.on) machine->noteOn(ph.notes[ev.n].n, ph.notes[ev.n].v);
                else machine->noteOff(ph.notes[ev.n].n);
            }
            machine->advanceSmoothing(block);
            machine->renderChunk(l, rr, block);
            for (int k = 0; k < block; ++k) {
                if (!std::isfinite(l[k]) || !std::isfinite(rr[k])) { finite = false; l[k] = rr[k] = 0; }
                mono[b0 + k] = 0.5f * (l[k] + rr[k]);
            }
        }
        phr::Figures fg = phr::analyse(mono, finite, atoi(f[3].c_str()), f[1].c_str(), f[2].c_str(), ph.name);
        phr::writeWav(f[4].c_str(), fg.norm);
        printf("%s\n", fg.json.c_str());
        fflush(stdout);
    }
    return 0;
}
