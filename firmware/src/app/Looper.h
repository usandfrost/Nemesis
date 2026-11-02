#ifndef __APP_LOOPER_H
#define __APP_LOOPER_H

#include <stdint.h>

#include "app/Model.h"

// A sample's loop: where it runs, and how the end melts into the start. The region is worked out the same way for the sound and the screen; finding the
// points is for the main loop (it reads the sample for some tens of ms), never the audio interrupt.
namespace app {

struct LoopRegion {
    double start = 0, end = 0;   // frames
    double fade = 0;             // frames of crossfade, at most half the loop
};

// From a lane's SAMPLE page (`sm`) and, for a sample that loops, its LOOP page (`lp`, or nullptr).
void loopRegion(const int16_t* sm, const int16_t* lp, uint32_t length, uint32_t rate, LoopRegion& out);
double loopFadeFrames(const int16_t* lp, double loopFrames, uint32_t rate);

// Looks near s0 and e0 for better loop points. AUTO_AMP: two rising zero crossings where the waves on both sides match, so the join needs no fade at all.
// AUTO_POWER: two steady places where the sound is equally loud over the fade, then lined up as well as the wave allows. `corr` 0..1: how alike the two ends are
// at the points it ended on, which is what CURVE MATCH fades by. `search` 1..127. False: nothing to go on (silence, or too short); the points are as they were.
// Main loop only: about 50 ms on the board with the audio idle, several times that with it busy.
bool findLoopPoints(const int16_t* data, uint32_t frames, uint32_t rate, uint32_t s0, uint32_t e0, int algo, int search, double fade, uint32_t& outStart, uint32_t& outEnd, float& corr);

}  // namespace app

#endif
