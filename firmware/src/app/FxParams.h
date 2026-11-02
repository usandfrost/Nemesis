#ifndef __APP_FX_PARAMS_H
#define __APP_FX_PARAMS_H

#include <stdint.h>

#include "app/Fmt.h"

// The effects' knobs (docs/FX_ANALYSIS.md): DELAY, REVERB and CHORUS are shared effects with a send level per part; AMP is an insert per
// part; COMP is on the master bus. Every parameter is a number 0..127 (a list: its index) that this file turns into real units, the same
// way for the screen, the sound and the tests.
namespace app {

constexpr int kFxParams = 8;
constexpr int kSends = 4;        // DELAY, REVERB, CHORUS, and one kept for later (always 0)
constexpr int kSendsUsed = 3;
constexpr int kFxParts = 7;      // = kParts (checked in Model.h)

enum FxView : uint8_t { FX_DELAY, FX_REVERB, FX_CHORUS, FX_COMP, FX_AMP, FX_SENDS, FX_VIEWS };

enum class FxUnit : uint8_t { Pct, Ms, Sec, Hz, Db, List };

struct FxParamDef {
    const char* name;
    FxUnit unit;
    bool exp;                 // real = a * (b / a)^(v/127)  (else linear a + (b - a) * v/127)
    double a, b;              // the real value at 0 and at 127
    double defReal;           // the default, in real units (a list: its index)
    const char* const* list;  // a list: its names; the value is the index
    int8_t nList;
};

const char* fxViewName(int view);
const FxParamDef& fxDef(int view, int i);   // view 0..4 (FX_SENDS: i 0..2 are DELAY, REVERB, CHORUS)
int fxCount(int view);                      // knobs in the view that hold a value
inline int fxMax(const FxParamDef& d) { return d.list ? d.nList - 1 : 127; }
int fxDefault(int view, int i);             // the default as the stored number
double fxReal(const FxParamDef& d, int v);  // the value in real units (a list: the index)
void formatFx(Text& out, const FxParamDef& d, int v);

// DELAY: its MODE list is FREE then note lengths. The delay time in ms for the project tempo.
constexpr int kDelayModes = 8;
double delayMs(int mode, int timeValue, int bpm);

struct FxSettings {
    int16_t delay[kFxParams], reverb[kFxParams], chorus[kFxParams], comp[kFxParams];
    int16_t amp[kFxParts][kFxParams];
    int16_t send[kFxParts][kSends];
};
void defaultFx(FxSettings& fx);
void sanitizeFx(FxSettings& fx);
// The stored array and length of a view's values for part `part` (AMP and SENDS are per part).
int16_t* fxValues(FxSettings& fx, int view, int part);
const int16_t* fxValues(const FxSettings& fx, int view, int part);

// The comp's ratio list entry as a number (the last, "INF", is 100).
double compRatio(int index);

}  // namespace app

#endif
