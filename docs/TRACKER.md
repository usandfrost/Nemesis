# The tracker

STEP's third view. It is the same steps as the step editor, written as text: a row is a step, a column is a lane, and all ten lanes are on screen. It is not a second sequencer. Nothing new is stored and the project format has not changed, so every project opens in it as it is, and what is written here is in the other two views.

STEP (the S key on the left) goes round: the step editor, its second view, the tracker, and back. The top left corner says TRACKER; the view has no tabs because the rows need the height.

## The screen

Sixteen rows, ten columns of 90 px. A cell shows the note and the velocity, `C-4 100`, and `C-4` is the note the step editor calls C4. A sharp is `C#4`. An empty step is dashes. On a sampler lane the note is a pitch, `+03`. Below octave 0, which only a low root reaches, the sign can't be the dash: `C_1`, `C#_1`.

The note is the one that plays: with a project scale set, a synth lane shows the note after the scale has moved it, and a high root that runs out of notes shows the top one. It is the same on the NOTE knob.

Two marks stand at the right of a cell: lime on top for locks, white under it for a chord, or on a sampler lane for a sample or slice of the step's own.

The heads say what the step editor's header says, for all ten lanes at once: the lane, its part, its root or sample, M when it is muted. The selected lane is underlined.

**Rows and lanes.** Lanes run in step with each other, on the pattern's step: the longest lane gives the rows, and a shorter one repeats (a lane of 5 plays its step 3 on rows 3, 8, 13 and so on), which is what the sequencer does. Repeats are dim. A repeat is the step itself: an edit there changes every repeat of it.

**Pages.** Over 16 rows there are two pages, fixed, and a strip under the grid shows which one this is and where the playhead is. The pages do not follow the playhead, and nothing scrolls: the display bus is slow, so only the playhead row moves. When the playhead is on the other page only a mark in the strip moves.

## The keys

| Key | What it does here |
|---|---|
| Arrows | Move the cursor. Up and down: rows. Left and right: lanes. They go round at the ends |
| SHIFT + up, down | The same place on the other page |
| OK, tapped | An empty step: the lane's last note goes on it. A step with a note: the popover, and that note becomes the last note. Tapped again: closes it |
| OK held + left, right | Move the note a semitone. With the popover, on an empty step too |
| OK held + up, down | Move it an octave |
| CLEAR (the small ring) | Removes the step, all of it: locks, chord, velocity. With a lock open it clears the locks and leaves the note, as everywhere. Holding it does not empty the pattern here: it is the view's everyday key |
| SHIFT + CLEAR | Undo |
| Keys 1 to 10 | Pick a lane, all ten: 9 and 10 are not pages here. Held for a moment and let go: open the lock of that lane's step on this row. Held and turned: edit that step |
| The rest | As in the step editor: MUTE, FILL, COPY, PASTE, ADD and the others work on the selected lane, and L goes back |

OK acts when it comes up, because only then is it known to be a tap. Any other key or a knob while it is down means it was not one. That is the one change to what OK does, and it holds in this view only: the other two views still toggle the step on the press.

The last note is kept per lane, as an offset from the lane's root, because lanes have roots of their own: a note from the bass lane means nothing on the lead. A new lane starts on its root. It lasts until the power goes off; it is not in the project.

A step that is off in the Euclid rhythm and gets a note is switched on the way a tap does it, with `Step.ov`. A hit of the rhythm that CLEAR took off comes back the same way.

## The popover

It opens by itself beside the cell being edited, when OK or a knob touches the step, not when the cursor merely passes. It goes on the right while there is room and flips to the left near the edge, so it never covers the cell. It closes when you move on, change view or leave the screen, and OK closes it too.

| Knob | Synth lane | Sampler lane |
|---|---|---|
| T1, T2, T3 | VELOCITY, GATE, PROB | VELOCITY, GATE, PROB |
| T4 | IN SCALE | nothing |
| B1 | NOTE | PITCH |
| B2 | OCTAVE | OCTAVE |
| B3 | CHORD | STEP SAMPLE (SHIFT: the folder) |
| B4 | INVERSION | STEP SLICE, on a sliced lane |

Any of these on an empty step puts the last note on it first, so there is one rule, and undo takes it all back. The knobs of an empty step are dimmed. Three do not: the folder knob picks nothing, so it adds nothing; INVERSION has nothing to invert until the step has a chord; IN SCALE is not about the step.

On a synth lane the popover shows the step and lane, the note with its offset, the octave, the chord, the inversion, and the chord's actual notes. Those come from `stepPitches()`, written to match the sequencer step for step and tested against it, so with a scale they are the notes after the scale. Its last line says what else matters: a chord with more notes than the part has voices, the arpeggio, or IN SCALE.

On a sampler lane it shows the sample's name and folder, pitch, octave and slice. The sample knob works as in the sample browser (docs/SAMPLE_BROWSER.md): SHIFT turns the folder and nothing is picked until the knob turns again, with the name in amber until then. A step with no sample of its own shows the lane's, unless the folder knob is elsewhere: then it shows, in amber, what the next turn takes. A chord on a sampler lane does play, so a step that has one says so, but it can't be set here.

## Limits

**±24.** A step's note stays within two octaves of the lane's root: the format and the NOTE knob hold it there. An octave move that would leave that does not happen at all: +15 stays +15, since stopping at +24 would change the note. A semitone stops at the limit. Either way the popover says STOPPED in red, and NOTE and OCTAVE turn red, until the next move that can be made. Octaves wider than that belong to the lane's ROOT.

**IN SCALE.** Offered on T4 once PROJECT has a scale, off by default. On, NOTE and OK with left and right walk the notes of the scale, so every detent is a note you can hear change, also from a note the scale has moved; without it a detent can land on a note the scale snaps back. It is how you are working, not something about the song, so it is not saved. Sampler lanes have none: the project scale is for synth parts.

## Undo and locks

Undo is SHIFT + CLEAR, one level, as elsewhere. Everything done to a step while its popover stays open is one undo, back to the step as it was, and CLEAR is one of its own. Like a sample pick elsewhere, an edit takes the place of whatever could be undone before it.

A lock open on a step still works: it opens from a held key 1 to 10, or arrives from the step editor, and its cell has the dashed lime outline. The sound pages lock it as ever. Up and down leave it open; another lane closes it, as a lane does in the step editor.

## On a computer

The web version is this same view through the firmware. On a computer keyboard:

- Enter is OK, so holding Enter with the arrows moves a note, which one mouse can't do. The Learn lesson shows that move in words for the same reason, and uses the knobs.
- In this view only, the letters are the piano of the bench page (A to K, with W E T Y U for the black keys): each key writes that note on the cursor's step and goes down a row. Z and X move the piano an octave. A note more than two octaves from the root says so and writes nothing. L leaves.

It goes through the firmware's own OK and NOTE knob (`wb_tracker_note` in `web/src/web_api.cpp`), so limits, IN SCALE and undo are the panel's. The firmware has no code for it.

## What it costs

Measured on the host with the frame the board would draw, counting the tiles that differ as `board/Lcd.cpp` does (tests/app/test_app.cpp, `testTrackerTiles`, on the default project; the screen is 810 tiles):

| Change | Tiles |
|---|---|
| The playhead moves a step | 60 (the old row and the new one: 7% of the screen), up to 65 on 32 rows with the strip under the grid |
| ... on the other page | 2 to 5 |
| For comparison: PLAY, a step, at worst | 94 |
| Cursor down | 90 (mostly the eight knob cells, whose values change) |
| Cursor to the next lane | 24 |
| A popover opens | 185 |
| A note turned in it | 11 |
| It closes | 112 |
| A page turn | 458 |

Rows are 20 px on a 20 px grid because the tiles are 32 x 20: a row on the grid is one row of tiles, so the playhead costs two rows of them. Off the grid it would cost three. That is why the heads share the 33 px above the grid and there are no tabs.

The display list is about 15 KB on the default project and 28 KB at its fullest, every cell a note with both marks and a popover on top, of the 64 KB arena. The view adds about 8 KB of flash and no static RAM: the UI lives in SDRAM.

## Not in it

- No sound on entry: a note is heard when the pattern plays it. Nothing in `Services` plays one note on a part, and it wasn't part of the design.
- Arrow keys do not repeat when held.
- OK on a filled step does not copy its chord: the last note is a note.
