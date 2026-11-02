#ifndef __APP_MIDI_MAP_H
#define __APP_MIDI_MAP_H

#include "app/Model.h"

// Which lane a MIDI note is, in and out (docs/UI_CONCEPT.md, the PROJECT screen's MIDI view). A lane has a channel (or none) and a
// trigger note. A lane alone on its channel is played by every note on it: a synth lane at the note's pitch, a sampler lane transposed
// by the note's distance from its trigger note. On a channel several lanes share (drum lanes on 10), a note plays the lane whose trigger
// note it is: a synth lane at its root, a sampler lane untransposed. Going out, a lane's notes are the same rule backwards.
namespace app {

int lanesOnChannel(const Project& pj, int ch);   // ch 1..16

struct MidiRoute {
    int lane = -1;
    int pitch = 0;      // a synth lane: the MIDI note to play; a sampler lane: semitones from the sample's own pitch
    bool sampler = false;
};
// An incoming note on `ch` (1..16) on the pattern playing: which lane it plays and how. False: no lane is that note.
bool routeIn(const Project& pj, int pattern, int ch, int note, MidiRoute& out);

// The note a lane sends for what it plays: `pitch` is the MIDI note of a synth lane, or a sampler lane's semitone offset.
int noteOut(const Project& pj, int lane, int pitch, bool sampler);

}  // namespace app

#endif
