#ifndef __APP_SLICER_H
#define __APP_SLICER_H

#include <stdint.h>

#include "app/Model.h"

// Finding where a sample's slices start. No HAL and no allocation worth the name: a sample in memory goes in, a slice table comes out. Main loop only
// (a long loop takes some tens of ms), never the audio interrupt.
namespace app {

// Fills `out` (sample and pitch settings are its defaults; `out.sample` is left as it was). `grid` 0: at the transients, `sens` 0..127 how small an attack
// counts; else that many equal parts, each moved to a zero crossing near it. Returns the slices found (at least 1: the whole sample).
int detectSlices(const int16_t* data, uint32_t frames, uint32_t rate, int sens, int grid, SliceMap& out);

// The index into kSliceBeats that puts a loop of this length nearest the project tempo.
int guessBeats(uint32_t frames, uint32_t rate, int bpm);

// A slice's own edits, in one place so the panel keeps the table in order. Both keep the slices sorted.
bool splitSlice(SliceMap& m, int slice, const int16_t* data);   // false: the table is full, or the slice is too short to halve
bool mergeSlice(SliceMap& m, int slice);                        // the slice joins the one before it. false: the first slice has none

}  // namespace app

#endif
