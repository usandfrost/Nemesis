# Recording

REC opens the REC screen. It takes what comes in at the line input, or what the Drumboy itself is playing, and turns it into a sample.

| Knob | What it does |
|---|---|
| T1 SOURCE | LINE IN, or OUTPUT: the Drumboy's own sound, effects and all. |
| T2 GAIN | The line input's gain, 1.5 dB a step, up to +22.5 dB. |
| T3 CAPTURE | MANUAL starts on REC. AUTO waits for the sound. |
| T4 THRESHOLD | The level, -60 to -6 dB, shown as a white line on the meter. |
| B1 INPUT | L+R, LEFT or RIGHT. A take is mono. |
| B2 MONITOR | How much of the line input goes on to the outputs. |
| B3 AUTO STOP | End the take after this long below the threshold, and leave that silence out. |
| B4 ROOM | Time left in memory while a take is running. |

**REC** starts, or arms when CAPTURE is AUTO. Pressing it again stops a take, or gives up waiting. On a finished take it asks first, and a second REC drops the take and starts over: AUTO STOP may have ended it while you were reaching for the key.

**OK** keeps the take on the selected lane, if that lane is on a sampler. Otherwise it goes to FILE, under REC. **○** drops it. Leaving the screen keeps a finished take, so one is never lost by walking off.

The sample list holds 3,072 samples. When it is full REC says so and does not start: a take that could not be kept is never recorded. **●** and MUTE do nothing on this screen, so a slip of the finger does not change the project.

AUTO needs the input over the threshold for several samples, so one tick of noise does not start it. The 50 ms before that moment stay in the take, so the attack is not cut off. Keep the threshold above the noise of the source: the meter shows where that is.

A take is mono, 16-bit, at 44.1 kHz. A take that hits full scale says so when it ends.

In the browser a drum loop stands in for the line input, and a take stays in memory until the page goes.

## Where it goes

A take is saved as `Sample/REC/RECnnnn.wav` and read back from the card to check it before it counts as saved. If that fails the message says why, and the take stays in memory only, named UNSAV01 and on: it lasts until the next project load or until the power goes, so a project saved with it cannot bring it back.

If the card's folder is written with other capitals, `Sample/Rec` say, the take goes into that folder. FAT does not tell them apart, and a project finds a sample whatever its capitals, so the take's key `REC/REC0001.wav` still finds the file after a restart.

After a restart a take over about 12 seconds cannot be previewed in FILE or on a sample knob: the screen says TOO LONG TO PREVIEW. It still plays on a lane.

## Memory

Samples and wavetables share one pool (`audio/MemoryPool.h`). Samples fill it from the bottom and wavetables from the top, about 1 MB each and only while a part plays them. A recording goes in the free memory between them, so the room for a take is what the project leaves: about 108 seconds on an empty project, less with each wavetable and sample.

Until a take is kept or dropped the free memory is lent out: nothing new loads, and no sample or wavetable moves.

When the pool is full a wavetable is refused with WAVETABLE MEMORY FULL and a sample with SAMPLE MEMORY FULL. Samples the project no longer uses are dropped first when a sample needs the room. A take does not do that: kept takes hold their memory until the project is loaded again, and so does what a take finds in the way.

The audition buffers stay outside the pool, so browsing never fills it.
