# The sample browser

A card holds about 2,500 samples in about 20 folders. A sample is reached in two moves: a folder, then a sample in it.

Every knob that picks a sample works the same way. **Turn it** and it moves one sample inside the folder. **Hold SHIFT and turn it** and it moves one folder. SHIFT is the panel's big step on any knob; on a sample knob the big step is a folder.

| Where | Knob |
|---|---|
| PLAY, on a sampler lane | B1 |
| STEP, on a sampler lane | T1, the lane's sample |
| STEP 2/3, SAMPLE PER STEP | T1, the lane's sample. B1, the step's sample lock |
| STEP 3/3, TRACKER | B3, the step's sample lock |
| SAMPLE page (OSC 1) | T1. With a step's lock open it is the step's sample lock |

The knob's cell shows the folder, the place in the folder and the sample:

```
SNARE        12/88
Snare_010
```

While SHIFT is down the cell shows FOLDER, the folder's place among the folders and its name. Each turn also shows the folder, the place and all of the name in a toast, as a cell has no room for a long name. At the end of a folder the knob stops and says so.

## What the folder knob changes

Nothing that plays. The lane keeps its sample, and a step keeps its lock. The knob only says where the sample knob is.

The name on the cell turns amber: that sample is on show, not picked, and it is heard once when the knob stops. The first turn of the sample knob, either way, picks the sample on show. After that each turn moves one sample. To leave without picking, turn the folder knob back to the folder of the sample in use, or leave it: the lane plays on.

A pick is live, as the sample knob always was: the lane plays the new sample at its next hit. SHIFT + ○ undoes a whole run of turns in one go, for a lane or for a step. Moving the folder knob is nothing to undo. To listen without changing anything, use FILE: it plays the list and only OK loads.

## Where it starts

- A lane's knob starts in the folder of the lane's sample.
- If the folder knob has moved, it stays where it was put, for that lane, across screens and lanes, until a sample is picked or a project is loaded.
- A step with a lock starts in the folder of the lock.
- A step with no lock shows LANE, the knob's left stop. It starts in the folder of the last sample picked for a step of that lane, or the lane's own. The first turn right picks the sample last used in that folder, so one sample can go on several steps with one turn each. If that is the lane's own sample it takes the next one: a lock with the lane's sample would change nothing.
- Turning left past the first sample of a folder takes the lock off the step, and it plays the lane's sample again.
- A folder you come back to starts at the sample last picked in it, or at its first.

A folder with no samples is passed over. Takes are in the folder REC, like any other, at the end of it.

## FILE

FILE on a sampler part is the same list, on a whole screen. It opens in the folder the lane's knob is in, on the lane's sample. Its CATEGORY knob and LEFT and RIGHT move the same folder memory, so a folder chosen in FILE is the one the sample knob is in afterwards, and the other way round. CATEGORY also has ALL, the whole list. OK loads to the lane; a step's lock is picked with its knob.

## On the web panel

The web panel's knobs have stops, so they show a place: the sample's place in the folder, or with SHIFT down the folder's place among the folders. A step's knob has LANE at its left stop.

## Previews

A preview reads the card, tens of milliseconds, and the panel waits for it. A slow turn is heard at every detent. A fast turn is heard at its first detent and then where it stops, 140 ms later.

A sample over about 12 seconds does not fit the preview buffer. The screen says TOO LONG TO PREVIEW, and the sample can still be picked and played on a lane. A file that cannot be read says so too.

## In the code

`Ui::Pick` says what a knob picks for: a lane's own sample, or a step's lock. `pickTurn` is the sample knob, `pickFolderTurn` the folder knob, `pickCell` draws both knobs' cell, and `bindings` swaps a sample knob for `B::Folder` while SHIFT is down. `SampleLib`'s `cat*` functions walk a category: its samples need not sit together, as built-in samples and takes are elsewhere in the list.
