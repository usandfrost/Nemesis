#ifndef __COST_DEGRADE_H
#define __COST_DEGRADE_H

#include <stdint.h>

#include "cost/CostModel.h"
#include "cost/FxCost.h"

// What to give up, and in what order, when the CPU is about to run out. Nothing is deleted: a patch keeps all its settings, and each step only
// BYPASSES a costly option until the load drops again (the governor walks back up the ladder).
//
// The order is fixed and documented (the user's rule):
//   1. release one voice (the oldest) on the lowest-priority part that has more than one;
//   2. then, part by part from the LOWEST priority up, bypass that part's costly options, the one that saves the most first;
//   3. last resort: release voices down to one per part, lowest priority first.
namespace cost {

enum class Action : uint8_t { StealVoice, WidthOff, UnisonOff, DriveOff, XmodOff, FoldOff, CompandOff, HeavyToLight, F2Off,
                              AmpToClean, AmpOff, DelayOff, ReverbOff, ChorusOff, CompOff, Count };
static_assert(static_cast<int>(Action::Count) <= 16, "an action mask is 16 bits");

constexpr int kMaxParts = 8;     // the project's parts and the shared effects
constexpr int kMaxSteps = 96;

struct PartDesc {
    VoiceDesc voice;
    uint8_t voices = 4;
    uint8_t priority = 0;      // higher = kept longer. Equal priorities degrade the later part first.
    bool sampler = false;      // a sampler part has no costly options, only voices
    uint8_t amp = 0;           // the part's amp insert: 0 off, 1 clean .. 4 bass (a cost per part, whatever its voices)
    // The shared effects (delay, reverb, chorus, master compressor) are one more "part" in the ladder: no voices, the bits of what is in use.
    bool fxBus = false;
    uint8_t fx = 0;            // FxBit mask
};

struct Step {
    uint8_t part = 0;
    Action action = Action::StealVoice;
    float saved = 0.0f;        // cost saved when this step is applied, in voice units
};

inline float voiceUnit(const PartDesc& p, const Settings& s) { return p.sampler ? kSamplerVoiceCost : voiceCost(p.voice, s); }
inline float partCost(const PartDesc& p, const Settings& s) {
    if (p.fxBus) return fxCost(p.fx);
    return static_cast<float>(p.voices) * voiceUnit(p, s) + ampCost(p.amp);
}
inline float totalCost(const PartDesc* p, int n, const Settings& s) {
    float c = 0.0f;
    for (int i = 0; i < n; ++i) c += partCost(p[i], s);
    return c;
}

// Apply one action to a part. Returns true if it changed something (an action with nothing to bypass is skipped, never counted as a step).
inline bool apply(PartDesc& p, Action a) {
    VoiceDesc& v = p.voice;
    switch (a) {
        case Action::StealVoice: if (p.voices > 1) { --p.voices; return true; } return false;
        case Action::WidthOff: if (p.sampler || v.width <= 0.0f) return false; v.width = 0.0f; return true;
        case Action::UnisonOff: {
            if (p.sampler || (v.slot[0].unison <= 1 && v.slot[1].unison <= 1)) return false;
            v.slot[0].unison = v.slot[1].unison = 1; return true;
        }
        case Action::DriveOff: if (p.sampler || v.drive <= 0.0f) return false; v.drive = 0.0f; return true;
#if DBOY_COST_LEAN
        case Action::XmodOff: if (p.sampler || v.sync <= 0.0f) return false; v.sync = 0.0f; return true;   // ring costs nothing on the lean voice, so it stays (makePatch)
#else
        case Action::XmodOff: if (p.sampler || (v.fm <= 0.0f && v.ring <= 0.0f && v.sync <= 0.0f)) return false; v.fm = v.ring = v.sync = 0.0f; return true;
#endif
        case Action::FoldOff: if (p.sampler || v.fold <= 0.0f) return false; v.fold = 0.0f; return true;
        case Action::CompandOff: if (p.sampler || v.companding <= 0.0f) return false; v.companding = 0.0f; return true;
        case Action::HeavyToLight: {
            if (p.sampler) return false;
            bool ch = false;
            if (isHeavy(v.f1)) { v.f1 = lightFallback(v.f1); ch = true; }
            if (v.f2On && isHeavy(v.f2)) { v.f2 = lightFallback(v.f2); ch = true; }
            // A heavy core carries its own drive; on a light model the drive is a separate oversampled stage that costs MORE than the heavy
            // extra it replaces. So leaving the heavy model also bypasses the drive, or this step would raise the cost.
            if (ch && v.drive > 0.0f) v.drive = 0.0f;
            return ch;
        }
        case Action::F2Off: if (p.sampler || !v.f2On) return false; v.f2On = false; return true;
        case Action::AmpToClean: if (p.fxBus || p.amp <= 1) return false; p.amp = 1; return true;
        case Action::AmpOff: if (p.fxBus || p.amp == 0) return false; p.amp = 0; return true;
        case Action::DelayOff: if (!p.fxBus || !(p.fx & kFxDelay)) return false; p.fx &= static_cast<uint8_t>(~kFxDelay); return true;
        case Action::ReverbOff: if (!p.fxBus || !(p.fx & kFxReverb)) return false; p.fx &= static_cast<uint8_t>(~kFxReverb); return true;
        case Action::ChorusOff: if (!p.fxBus || !(p.fx & kFxChorus)) return false; p.fx &= static_cast<uint8_t>(~kFxChorus); return true;
        case Action::CompOff: if (!p.fxBus || !(p.fx & kFxComp)) return false; p.fx &= static_cast<uint8_t>(~kFxComp); return true;
        default: return false;
    }
}

// Build the whole ladder for the current set of parts. Returns the number of steps (<= cap).
inline int buildLadder(const PartDesc* parts, int n, const Settings& s, Step* out, int cap) {
    if (n > kMaxParts) n = kMaxParts;
    PartDesc work[kMaxParts];
    for (int i = 0; i < n; ++i) work[i] = parts[i];
    uint8_t order[kMaxParts];                      // parts by priority, lowest first; equal priority: later part first
    for (int i = 0; i < n; ++i) order[i] = static_cast<uint8_t>(i);
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0; --j) {
            const PartDesc& a = work[order[j - 1]]; const PartDesc& b = work[order[j]];
            if (b.priority < a.priority || (b.priority == a.priority && order[j] > order[j - 1])) { const uint8_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
            else break;
        }
    int count = 0;
    auto push = [&](int part, Action a, float saved) { if (count < cap) { out[count].part = static_cast<uint8_t>(part); out[count].action = a; out[count].saved = saved; ++count; } };

    for (int k = 0; k < n; ++k) {                                              // 1. release one voice, lowest priority first
        const int q = order[k];
        if (work[q].voices > 1) { const float sv = voiceUnit(work[q], s); apply(work[q], Action::StealVoice); push(q, Action::StealVoice, sv); break; }
    }
    for (int k = 0; k < n; ++k) {                                              // 2. bypass costly options, biggest saving first
        const int q = order[k];
        for (;;) {
            Action best = Action::Count; float bestSaved = 0.001f;
            for (int a = 1; a < static_cast<int>(Action::Count); ++a) {
#if DBOY_COST_LEAN
                // unison off also takes the width with it, so it always saves more and the gentler step would never be taken: the width goes first
                if (static_cast<Action>(a) == Action::UnisonOff && work[q].voice.width > 0.0f) continue;
#endif
                PartDesc t = work[q];
                if (!apply(t, static_cast<Action>(a))) continue;
                const float sv = partCost(work[q], s) - partCost(t, s);
                if (sv > bestSaved) { bestSaved = sv; best = static_cast<Action>(a); }
            }
            if (best == Action::Count) break;
            apply(work[q], best);
            push(q, best, bestSaved);
        }
    }
    for (int k = 0; k < n; ++k) {                                              // 3. last resort: down to one voice per part
        const int q = order[k];
        while (work[q].voices > 1) { const float sv = voiceUnit(work[q], s); apply(work[q], Action::StealVoice); push(q, Action::StealVoice, sv); }
    }
    return count;
}

// The part as it runs at ladder level `level` (the first `level` steps applied).
inline PartDesc degraded(const PartDesc& base, int partIndex, const Step* ladder, int steps, int level) {
    PartDesc p = base;
    for (int i = 0; i < level && i < steps; ++i)
        if (ladder[i].part == partIndex) apply(p, ladder[i].action);
    return p;
}

}  // namespace cost

#endif
