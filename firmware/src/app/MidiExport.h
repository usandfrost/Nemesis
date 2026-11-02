#ifndef __APP_MIDIEXPORT_H
#define __APP_MIDIEXPORT_H

#include "app/Model.h"
#include "app/ProjectFile.h"
#include "app/SampleLib.h"

// A song as a Standard MIDI File, type 1: a tempo track, then one track for each lane that plays. The notes come from the real sequencer, run on its own and quicker than
// time, so what is in the file is what plays: Euclid rhythms, tapped steps, chords, strums, arpeggios, swing, the scale. A step with a PROB plays or not once, as it did
// on that run, the same each time the file is made. Each lane's notes are what its MIDI out sends (MidiMap.h noteOut): a lane on a shared channel is its trigger note.
// Nothing here touches what is playing.
namespace app {

constexpr int kMidiPpq = 480;   // (fine enough for a short arpeggio's notes)
constexpr int kMidiMostNotes = 24000;

// `work` is a copy of the project, which the export may change. False: the song has no rows, or `out` refused.
bool writeSongMidi(Project& work, int song, const SampleLib* lib, ByteSink& out);
bool midiTooManyNotes();   // the last writeSongMidi failed because the song has more than kMidiMostNotes
int laneFirstRow(const Project& pj, int song, int lane);   // the song's first row where the lane plays: what names it, in a track and in a stem
void midiExportInit();   // takes the memory it needs. At start-up on the board, where nothing can be had later

}  // namespace app

#endif
