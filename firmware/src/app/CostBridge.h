#ifndef __APP_COST_BRIDGE_H
#define __APP_COST_BRIDGE_H

#include "app/Model.h"
#include "app/Fmt.h"
#include "cost/Degrade.h"

// The cost model (firmware/src/cost) read from a part's pages, as the mockup's descOf/partDesc/costBadge/enforceHeavy/lockedValue do.
// The UI uses it for the MIXER's cost meter and the knob badges; the instrument builds its degrade ladder from the same descriptions, so
// the ladder the MIXER shows is the one the governor walks.
namespace app {

// What the firmware runs (FW_COST in the mockup): cores and drives at x2, silent slots skipped.
constexpr cost::Settings kFwCost = {2, 2, true};

// The MODEL knob in order, as cost filters.
cost::Filter modelFilter(int menu);
int menuOf(cost::Filter f);            // a light fallback that is not on the menu (Ladder12) shows as LADDER

// A part's voice as the cost model sees it; `step` (may be null) supplies p-locked values.
cost::VoiceDesc descOf(const Project& pj, int part, const Step* step = nullptr);
cost::PartDesc partDesc(const Project& pj, int part, uint8_t priorityOverride = 0xFF);
cost::PartDesc fxDesc(const Project& pj);   // the shared effects, the ladder's last part (index kParts)
float fxCostOf(const Project& pj);
// Takes voices off a part until the part (its amp included) and the shared effects fit the voice budget on their own (a part keeps at least one).
// Returns the voices it ends with. Used when a preset is loaded: its recommended voices plus its effects can ask for more than the CPU has.
int fitVoicesToBudget(Project& pj, int part);
// The shared effects some effects settings use (cost::FxBit mask): any part's send to the delay, reverb, chorus, or the compressor on.
uint8_t fxMaskOf(const FxSettings& fx, uint32_t idleParts = 0);   // (`idleParts`: a bit per part whose sends do not count)
float partCost(const Project& pj, int part);
float voiceTotal(const Project& pj);        // in voice units, as set (before any degrade); a part no lane plays adds nothing

// The degrade ladder over the parts, and a part with the first `level` steps applied.
struct Ladder {
    cost::Step steps[cost::kMaxSteps];
    int count = 0;
};
void buildLadder(const Project& pj, Ladder& out);
cost::PartDesc degradedPart(const Project& pj, int part, const Ladder& l, int level);
float effTotal(const Project& pj, const Ladder& l, int level);
void describeStep(Text& out, const cost::Step& s);   // "A: unison off (-0.23)"
extern const char* const kActionNames[];

// The knob badge: what this setting adds to the voice while it is on ("+.27"), empty otherwise.
void costBadge(Str<16>& out, const Project& pj, int part, int page, int param);
bool menuHeavy(int menu);

// The one-heavy-filter rule on the knobs: the filter just chosen wins, the other falls back to its light twin. Returns true (and the toast
// text) if the other filter was changed.
bool enforceHeavy(Project& pj, int part, int page, int param, Text& toast);
// The same rule for a p-lock: a locked value that would make a second heavy filter becomes its light fallback.
int lockedValue(const Project& pj, int part, uint8_t key, int value, const Step& step, Text& toast);

}  // namespace app

#endif
