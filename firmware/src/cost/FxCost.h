#ifndef __COST_FX_COST_H
#define __COST_FX_COST_H

#include <stdint.h>

#include "cost/LeanCostTable.h"

// What the effects cost, in the cost model's own unit (one plain voice = 1.0). For the CATHODE engine, measured on the host as ratios against a voice in the same build
// (fx_bench/results/bench_float*.txt, docs/FX_ANALYSIS.md section 3); for the lean voice, on the board (LeanCostTable.h). The governor reads real cycles, so a wrong
// figure only mis-orders the degrade ladder. The amp is a mono insert with no oversampling (its models other than Crunch are scaled from the Crunch measurement).
namespace cost {

enum FxBit : uint8_t { kFxDelay = 1, kFxReverb = 2, kFxChorus = 4, kFxComp = 8 };

// (the wrappers as written, measured by the FX stress test on the host: ratios against one plain voice in the same build)
#if DBOY_COST_LEAN   // measured on the board, in lean voices
using lean_table::kCostDelay;
using lean_table::kCostReverb;
using lean_table::kCostChorus;
using lean_table::kCostComp;
using lean_table::kCostAmp;
#else
constexpr float kCostDelay = 0.16f;
constexpr float kCostReverb = 0.48f;
constexpr float kCostChorus = 0.07f;
constexpr float kCostComp = 0.19f;
constexpr float kCostAmp[5] = {0.0f, 0.90f, 1.10f, 1.33f, 1.33f};   // off, clean, crunch, lead, bass
#endif

inline float fxCost(uint8_t mask) {
    return ((mask & kFxDelay) ? kCostDelay : 0.0f) + ((mask & kFxReverb) ? kCostReverb : 0.0f) + ((mask & kFxChorus) ? kCostChorus : 0.0f) +
           ((mask & kFxComp) ? kCostComp : 0.0f);
}
inline float ampCost(uint8_t model) { return kCostAmp[model > 4 ? 4 : model]; }

}  // namespace cost

#endif
