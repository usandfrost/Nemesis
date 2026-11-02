# Slices

A sampler lane can cut its sample into slices and play them one by one. On a sampler lane OSC 2 opens the SLICE page; the key again goes to SLICE EDIT, one slice at a time; once more goes back.

## The SLICE page

| Knob | What it does |
|---|---|
| T1 MODE | OFF plays the sample as ever. STEP: a step that fires plays a slice. SYNC: the loop plays by itself at the tempo. |
| T2 SLICE | AUTO, or one slice. Lockable. |
| T3 RANDOM | The chance a hit plays any slice instead. Lockable. |
| T4 BEATS | How long the loop is. Guessed from its length and the tempo when MODE comes on; the screen shows the BPM that makes. |
| B1 FIT | At another tempo. SLICES: the pitch stays, so slices leave gaps or get cut. PITCH: the loop speeds up like a record. |
| B2 RND REV | The chance a hit plays backwards. Lockable. |
| B3 SENS | How small an attack counts as a new slice. |
| B4 GRID | AUTO cuts at the attacks; 4 to 32 cuts into equal parts. |

Turning SENS or GRID cuts the sample again, a moment after the knob stops. SHIFT + ○ undoes that, and hand edits come back with it.

## Playing

**STEP.** A step plays the slice of its place in the lane: step 3 plays slice 3, so the rhythm picks where the loop is chopped. In the step's second view STEP SLICE locks a slice. A step can lock RANDOM and RND REV too, and every other sampler knob: pitch, filter, level, reverse.

**SYNC.** The loop runs on the song's clock whatever the lane's rhythm. Each slice falls at its own place in the beats, so the feel stays at any tempo, and swing bends the steps it falls in. The lane's steps still work as an overlay: a step's locks reach the slices it holds, STEP SLICE replaces them, and PROB can drop them. STOP and MUTE end it.

**MIDI.** On a sliced lane alone on its channel the keys play the slices from the lane's note up. Lanes sharing a channel, as the drum lanes do on 10 by default, have one note each, so that lane plays its first slice: give it a channel of its own.

With PLAY on LOOP a STEP slice loops inside itself, with the LOOP page's fade (docs/LOOPS.md); SYNC slices play once.

A lane's slices cut each other, so a chopped loop is one voice. Notes of a chord start together and all sound.

## SLICE EDIT

| Knob | What it does |
|---|---|
| T1 SLICE | Picks a slice and plays it. |
| T2 START | Moves its start, between its neighbours. |
| T3 PITCH, T4 LEVEL | The slice's own. |
| B1 REVERSE, B2 PLAYS | The slice's own. A muted slice is skipped. |
| B3 SPLIT, B4 MERGE | Turn right. SPLIT halves it at a zero crossing; MERGE joins it to the one before. |

A run of edits is one undo.

## How it is kept

Slices belong to the sample, not the lane: every lane on that sample plays the same ones. A project holds the cuts of 16 samples, 32 slices each, in `Project::slices`. A cut is for a file of a given length. If the file changes the lane stays silent until the panel, which watches for this, has cut it again. A sample missing from the card loses its cut.

`detectSlices` (`app/Slicer.cpp`) runs in the main loop on what is in memory, never in the audio interrupt. It follows how fast the wave moves in 5 ms hops, picks the sudden rises over the last 30 ms, and moves each cut back to a zero crossing. When a loop has more attacks than the table holds the loudest stay.

A slice runs 6 ms past its end and fades there, and the next slice of the lane crossfades over its attack, so at the loop's own tempo the slices add up to the loop with no hole between them.
