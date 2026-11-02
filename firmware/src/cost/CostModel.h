#ifndef __COST_MODEL_H
#define __COST_MODEL_H

#include <stdint.h>

#include "cost/CostTable.h"
#include "cost/LeanCostTable.h"

// CPU cost model for one voice, in VOICE UNITS (a plain voice = 1.0). Pure arithmetic over a small description of the voice, so it
// runs identically on the host (tests/cost) and on the M7, and it never touches the DSP. The numbers are generated: CostTable.h for the CATHODE
// engine, LeanCostTable.h for the lean voice (DBOY_COST_LEAN, built from a run on the board).
//
// Used for three things: the mixer's cost meter, the one-heavy-filter rule (p-locks included), and the order in which the load
// governor degrades a part when the CPU is about to run out (cost/Degrade.h).
namespace cost {

enum class Filter : uint8_t {   // same order as nemesis::dsp::CathodeVoiceStage::FilterMode; CathodeRenderer.cpp static_asserts it
    LowPass, BandPass, HighPass, Notch, Formant, Comb, Ladder24, Ladder12, ScreamLp, ScreamHp, Acid, SovietLp, SovietBp, SovietHp,
    ClassicLp24, ClassicLp12, ClassicBp, ClassicHp, ReedLp, ReedBp, ReedHp, StateLp, StateBp, StateHp, StateNotch, StateAp, StateMorph, ReedMorph,
    Count
};
enum class Osc : uint8_t { PD, Wavetable, Bitmask, Pulse, Macro };
enum class Route : uint8_t { Series, Parallel, Stereo };
enum class FilterSlot : uint8_t { F1, F2 };

// What the firmware actually runs; the table's filter extras are for the upstream 4x oversampling and scale with the factor.
struct Settings {
    int filterOversample = 4;
    int driveOversample = 4;          // the filter drive's own factor (ForkConfig driveOversample); its extra scales with it
    bool skipSilentSlots = false;     // the firmware's skip of a silent, uncoupled oscillator slot (ForkConfig.h)
};

struct SlotDesc {
    Osc kind = Osc::PD;
    bool audible = true;        // level > 0
    uint8_t unison = 1;         // 1..8
    bool macroHeavy = false;    // the Plaits TwoOpFm and Particles engines
};

struct VoiceDesc {
    SlotDesc slot[2] = { { Osc::PD, true, 1, false }, { Osc::PD, false, 1, false } };
    Filter f1 = Filter::LowPass;
    Filter f2 = Filter::LowPass;
    bool f2On = false;
    Route route = Route::Series;
    float drive = 0.0f, fm = 0.0f, ring = 0.0f, sync = 0.0f;   // 0 = off
    float width = 0.0f;                                         // unison stereo width; > 0 engages the stereo right chain
    float companding = 0.0f, fold = 0.0f;
    bool pd1 = false, pd2 = false, sub = false, noise = false, hpf = false, clip = false;   // what only the lean voice's table charges for: phase distortion on an oscillator, the sub, noise, the HPF, GRIT CLIP
};

// Cost of one voice of a sampler part (an estimate, to be replaced by a measurement of the sampler voice).
#if DBOY_COST_LEAN
constexpr float kSamplerVoiceCost = lean_table::kSamplerVoice;
#else
constexpr float kSamplerVoiceCost = 0.25f;
#endif
// Unison on the second slot relative to the first: measured (table::kUnisonSlot1Factor; it was 2 before the DSP review's changes, ~1 after).
constexpr float kUnisonSlot1Factor = table::kUnisonSlot1Factor;
// A lane's sound may use at most this many heavy filter models at once (F1 plus F2, a p-lock included).
constexpr int kMaxHeavyFiltersPerLane = 1;

// The non-linear cores run inside their own oversampler (filterOversample); their measured extra scales with the factor.
constexpr bool oversampledCore(Filter f) {
    return f == Filter::ScreamLp || f == Filter::ScreamHp || f == Filter::Acid || f == Filter::SovietLp || f == Filter::SovietBp || f == Filter::SovietHp
           || f == Filter::ClassicLp24 || f == Filter::ClassicLp12 || f == Filter::ClassicBp || f == Filter::ClassicHp || f == Filter::ReedLp
           || f == Filter::ReedBp || f == Filter::ReedHp || f == Filter::ReedMorph;
}
// HEAVY = an extra of kHeavyThreshold voices or more at x4, the scale the one-heavy-filter rule was defined on, whatever factor the table
// was measured at (tools/gen_cost_table.py classifies the same way). A property of the model, not of the run's oversampling.
constexpr float extraAtX4(Filter f) {
    return table::kFilterExtra[static_cast<int>(f)] * (oversampledCore(f) ? 4.0f / static_cast<float>(table::kTableOversample) : 1.0f);
}
// The lean voice has no heavy models: no core is oversampled, and on the board the dearest (CLASSIC) adds about a third of a typical voice.
constexpr bool isHeavy(Filter f) { return !DBOY_COST_LEAN && extraAtX4(f) >= table::kHeavyThreshold; }

// A heavy model's nearest light model with the same response. A light model is its own fallback. Mirrors LIGHT_TWIN in
// tools/gen_cost_table.py, which refuses to generate a table where a heavy model has no light twin; should one slip through anyway,
// it falls back to the plain LowPass rather than to itself (a "fallback" to itself would leave two heavy filters running).
constexpr Filter lightTwin(Filter f) {
    switch (f) {
        case Filter::ScreamLp: case Filter::Acid: case Filter::ClassicLp24: return Filter::Ladder24;
        case Filter::SovietLp: case Filter::ClassicLp12: return Filter::Ladder12;
        case Filter::ScreamHp: case Filter::SovietHp: case Filter::ClassicHp: return Filter::HighPass;
        case Filter::SovietBp: case Filter::ClassicBp: return Filter::BandPass;
        case Filter::ReedLp: return Filter::StateLp;
        case Filter::ReedBp: return Filter::StateBp;
        case Filter::ReedHp: return Filter::StateHp;
        case Filter::ReedMorph: return Filter::StateMorph;
        default: return f;
    }
}
constexpr Filter lightFallback(Filter f) {
    if (!isHeavy(f)) return f;
    const Filter t = lightTwin(f);
    return isHeavy(t) || t == f ? Filter::LowPass : t;
}

inline float filterExtra(Filter f, const Settings& s) {
    const float e = table::kFilterExtra[static_cast<int>(f)];
    return oversampledCore(f) ? e * static_cast<float>(s.filterOversample) / static_cast<float>(table::kTableOversample) : e;   // ~linear in the factor
}

inline float unisonExtra(int n) {
    if (n <= 1) return 0.0f;
    return n == 2 ? table::kUnison2 : (n <= 4 ? table::kUnison4 : table::kUnison8);
}

inline float oscKindDelta(const SlotDesc& s) {
    switch (s.kind) {
        case Osc::Wavetable: return table::kOscWavetable;
        case Osc::Bitmask: return table::kOscBitmask;
        case Osc::Pulse: return table::kOscPulse;
        case Osc::Macro: return table::kOscMacro + (s.macroHeavy ? table::kOscMacroHeavy : 0.0f);
        default: return 0.0f;
    }
}

#if DBOY_COST_LEAN
inline float leanFilterExtra(Filter f) {
    using namespace lean_table;
    switch (f) {
        case Filter::Ladder24: case Filter::Ladder12: return kLadder;
        case Filter::Acid: return kAcid;
        case Filter::SovietLp: case Filter::SovietBp: case Filter::SovietHp: return kSoviet;
        case Filter::ClassicLp24: case Filter::ClassicLp12: case Filter::ClassicBp: case Filter::ClassicHp: return kClassic;
        default: return 0.0f;
    }
}

// The lean voice's sum, the same conditions as lean::Part::renderVoice: ring and sync need osc 1 sounding (and ring osc 2, and sync no PD on osc 1), either collapses osc 1's
// unison stack, PD runs per stack member, and a stereo (width) voice runs the chain after the oscillators twice. tools/fit_lean_cost.py features() reads the knobs the same way.
inline float voiceCost(const VoiceDesc& d, const Settings& = Settings()) {
    using namespace lean_table;
    const bool osc1 = d.slot[0].audible, osc2 = d.slot[1].audible;
    const bool ring = osc1 && osc2 && d.ring > 0.0f, sync = osc1 && d.sync > 0.0f && !d.pd1;
    const int stack = osc1 && !ring && !sync ? d.slot[0].unison : 1;
    const bool wide = stack > 1 && d.width > 0.0f;
    float c = 1.0f + static_cast<float>(stack - 1) * kUnison;
    if (osc1 && osc2) c += kOsc2;
    for (int i = 0; i < 2; ++i) if (d.slot[i].audible && d.slot[i].kind == Osc::Wavetable) c += kBank;
    if (d.pd1 && osc1) c += static_cast<float>(stack) * kPd;
    if (d.pd2 && osc2) c += kPd;
    if (d.sub) c += kSub;
    if (ring) c += kRing;
    if (sync) c += kSync;
    float chain = leanFilterExtra(d.f1);
    if (d.noise) chain += kNoise;
    if (d.hpf) chain += kHpf;
    if (d.clip) chain += kClip;
    if (d.drive > 0.0f) chain += kDrive;
    if (d.f2On) chain += kF2 + leanFilterExtra(d.f2);
    if (d.companding > 0.0f) chain += kGrit;
    return c + chain * (wide ? 2.0f : 1.0f) + (wide ? kWidth : 0.0f);
}
#else
inline float voiceCost(const VoiceDesc& d, const Settings& s = Settings()) {
    float c = 1.0f;
    const bool coupled = d.fm > 0.0f || d.ring > 0.0f || d.sync > 0.0f;       // a silent slot that feeds the other one is still computed
    // FM, ring mod and hard sync make slot 0 collapse its unison stack to one oscillator, so its unison (and the stereo width, which needs a
    // stack) costs nothing while they are on. Measured: ring alone 1.65, ring + unison x4 1.67, ring + unison x4 + width 1.68; the same for FM and sync.
    const bool stack0Collapsed = coupled;
    const bool stackLive = (!stack0Collapsed && d.slot[0].unison > 1) || d.slot[1].unison > 1;
    int computed = 0;
    for (int i = 0; i < 2; ++i) {
        const SlotDesc& sl = d.slot[i];
        const float uni = (i == 0 && stack0Collapsed) ? 0.0f : unisonExtra(sl.unison) * (i == 1 ? kUnisonSlot1Factor : 1.0f);
        c += uni;
        const bool skipped = !sl.audible && s.skipSilentSlots && !coupled && sl.unison <= 1;
        if (!skipped) {                                                         // a skipped slot is not computed, whatever its kind
            ++computed;
            c += oscKindDelta(sl);
        }
    }
    c += static_cast<float>(computed - 1) * table::kSlotCost;                // the plain voice contains exactly one computed slot
    c += filterExtra(d.f1, s);
    if (d.f2On) {
        c += (d.route == Route::Series ? table::kF2Series : (d.route == Route::Parallel ? table::kF2Parallel : table::kF2Stereo)) + filterExtra(d.f2, s);
    }
    if (d.width > 0.0f && stackLive)                                            // the stereo right chain re-runs the filter(s)
        c += table::kWidthFixed + filterExtra(d.f1, s) + (d.f2On ? filterExtra(d.f2, s) : 0.0f);
    // The heavy cores already run their own oversampled drive stage, so the filter drive adds nothing on top of them (measured:
    // Classic alone 1.27, Classic + drive 1.28). On a light model the drive is a separate oversampled waveshaper.
    if (d.drive > 0.0f && !isHeavy(d.f1)) c += table::kDrive * static_cast<float>(s.driveOversample) / static_cast<float>(table::kTableOversample);   // oversampled like the cores
    if (d.ring > 0.0f) c += table::kRing;
    if (d.fm > 0.0f) c += table::kFm;
    if (d.sync > 0.0f) c += table::kSync;
    if (d.companding > 0.0f) c += table::kCompanding;
    if (d.fold > 0.0f) c += table::kFold;
    return c < 0.3f ? 0.3f : c;
}
#endif

// ---- the one-heavy-filter rule ---------------------------------------------------------------------------------------------------------

inline int heavyCount(const VoiceDesc& d) { return (isHeavy(d.f1) ? 1 : 0) + (d.f2On && isHeavy(d.f2) ? 1 : 0); }
inline bool heavyRuleOk(const VoiceDesc& d) { return heavyCount(d) <= kMaxHeavyFiltersPerLane; }

// Set a filter model. The one just chosen wins: if that leaves two heavy filters, the OTHER one falls back to its light model.
// Returns true when the other filter was displaced (so the UI can say so).
inline bool setFilter(VoiceDesc& d, FilterSlot which, Filter f) {
    (which == FilterSlot::F1 ? d.f1 : d.f2) = f;
    if (heavyRuleOk(d)) return false;
    Filter& other = which == FilterSlot::F1 ? d.f2 : d.f1;
    other = lightFallback(other);
    return true;
}
// Turning F2 on can also create a second heavy filter; F2 is the newcomer, so IT is demoted.
inline bool setFilter2On(VoiceDesc& d, bool on) {
    d.f2On = on;
    if (heavyRuleOk(d)) return false;
    d.f2 = lightFallback(d.f2);
    return true;
}
// A p-lock may change a step's filter model for that step only. The lane's base keeps its own filters, so a lock that would make a second
// heavy filter is changed to its light fallback instead (the lock, not the base, is the newcomer).
inline Filter allowedLock(const VoiceDesc& base, FilterSlot which, Filter locked) {
    VoiceDesc t = base;
    (which == FilterSlot::F1 ? t.f1 : t.f2) = locked;
    if (heavyRuleOk(t)) return locked;
    return lightFallback(locked);
}

}  // namespace cost

#endif
