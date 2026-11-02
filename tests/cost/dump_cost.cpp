// Writes reference cases from the C++ cost model as JSON, so the JavaScript port in the mockup can be checked against it number for number.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "cost/CostModel.h"
#include "cost/Degrade.h"

using namespace cost;

static uint32_t st = 777;
static uint32_t rnd() { st = st * 1664525u + 1013904223u; return st >> 8; }
static float rnd01() { return (rnd() & 0xFFFF) / 65535.0f; }

static VoiceDesc randomVoice() {
    VoiceDesc d;
    d.slot[0].audible = true; d.slot[1].audible = rnd() % 2;
    d.slot[0].unison = 1 + rnd() % 8; d.slot[1].unison = rnd() % 3 ? 1 : 1 + rnd() % 4;
    d.f1 = static_cast<Filter>(rnd() % static_cast<int>(Filter::Count)); d.f2 = static_cast<Filter>(rnd() % static_cast<int>(Filter::Count));
    d.f2On = rnd() % 2; d.route = static_cast<Route>(rnd() % 3);
    d.drive = rnd() % 2 ? rnd01() : 0; d.fm = rnd() % 3 == 0 ? rnd01() : 0; d.ring = rnd() % 3 == 0 ? rnd01() : 0; d.sync = rnd() % 4 == 0 ? 1 : 0;
    d.width = rnd() % 2 ? rnd01() : 0; d.companding = rnd() % 3 == 0 ? rnd01() : 0; d.fold = rnd() % 3 == 0 ? rnd01() : 0;
    for (auto& sl : d.slot) { sl.kind = static_cast<Osc>(rnd() % 5); sl.macroHeavy = sl.kind == Osc::Macro && rnd() % 2; }
    return d;
}
static void descJson(const VoiceDesc& d) {
    std::printf("{\"k0\":%d,\"k1\":%d,\"mh0\":%d,\"mh1\":%d,\"a0\":%d,\"a1\":%d,\"u0\":%d,\"u1\":%d,\"f1\":%d,\"f2\":%d,\"f2On\":%d,\"route\":%d,\"drive\":%.4f,\"fm\":%.4f,\"ring\":%.4f,\"sync\":%.4f,\"width\":%.4f,\"comp\":%.4f,\"fold\":%.4f}",
                (int)d.slot[0].kind, (int)d.slot[1].kind, d.slot[0].macroHeavy, d.slot[1].macroHeavy, d.slot[0].audible, d.slot[1].audible, d.slot[0].unison, d.slot[1].unison, (int)d.f1, (int)d.f2, d.f2On, (int)d.route, d.drive, d.fm, d.ring, d.sync, d.width, d.companding, d.fold);
}

int main() {
    std::printf("{\"costs\":[");
    for (int i = 0; i < 400; ++i) {
        VoiceDesc d = randomVoice();
        Settings s; s.filterOversample = (rnd() % 2) ? 2 : 4; s.skipSilentSlots = rnd() % 2; s.driveOversample = (rnd() % 2) ? 2 : 4;
        std::printf("%s{\"d\":", i ? "," : ""); descJson(d);
        std::printf(",\"os\":%d,\"dos\":%d,\"skip\":%d,\"cost\":%.6f,\"heavy\":%d}", s.filterOversample, s.driveOversample, s.skipSilentSlots, voiceCost(d, s), heavyCount(d));
    }
    std::printf("],\"ladders\":[");
    for (int i = 0; i < 120; ++i) {
        const int n = 1 + rnd() % 3;
        PartDesc parts[3];
        for (int p = 0; p < n; ++p) {
            parts[p].voice = randomVoice(); parts[p].voices = 1 + rnd() % 5; parts[p].priority = rnd() % 3; parts[p].sampler = rnd() % 4 == 0;
            setFilter2On(parts[p].voice, parts[p].voice.f2On);
        }
        Settings s; s.filterOversample = 2; s.skipSilentSlots = true; s.driveOversample = 2;
        Step lad[kMaxSteps]; const int steps = buildLadder(parts, n, s, lad, kMaxSteps);
        std::printf("%s{\"parts\":[", i ? "," : "");
        for (int p = 0; p < n; ++p) { std::printf("%s{\"voices\":%d,\"priority\":%d,\"sampler\":%d,\"d\":", p ? "," : "", parts[p].voices, parts[p].priority, parts[p].sampler); descJson(parts[p].voice); std::printf("}"); }
        std::printf("],\"total\":%.6f,\"steps\":[", totalCost(parts, n, s));
        for (int k = 0; k < steps; ++k) std::printf("%s[%d,%d,%.6f]", k ? "," : "", lad[k].part, (int)lad[k].action, lad[k].saved);
        std::printf("]}");
    }
    std::printf("]}\n");
}
