# Loops

A sample whose PLAY is LOOP plays from START to END and round again. On a sampler lane SUB opens the LOOP page, which says how the end joins the start. The page is dim until PLAY is LOOP. A lane in SYNC plays slices, not a loop, so the page is dim there too.

| Knob | What it does |
|---|---|
| T1 AUTO | OFF plays the knobs' points. EQ AMP and EQ POWER look for better ones (below). |
| T2 XFADE | How long the end melts into the start. Lockable on a step. |
| T3 UNIT | MS, up to 2 s, or % of the loop, up to 50%. A fade is never more than half the loop. |
| T4 CURVE | LINEAR, EQ POWER, LOG or MATCH (below). |
| B1 S FINE, B2 E FINE | Move the points by small steps inside one of START's and END's. Not used while AUTO is on. |
| B3 SEARCH | How far from START and END AUTO may look. |

The picture shows the loop, with the fade shaded at both ends, and the footer gives the time of one lap.

## The fade and the lap

What fades in is the sound leading up to START, so a fade does not change the lap: a one-bar loop stays one bar. That needs sample before START. With START at the very beginning there is nothing before it, so the end goes back to a point the fade's length in, and the lap is shorter by that much. Move START in if the lap has to be exact.

## Curves

| Curve | Use it for |
|---|---|
| LINEAR | Two ends in step, such as a held note on matching points. The level stays. On unrelated sound it dips by about 3 dB. |
| EQ POWER | Two ends with nothing in common, such as noise or a texture. The power stays. On sound in step it swells by about 3 dB. |
| LOG | The new sound arrives early and the old leaves late, close to EQ POWER. |
| MATCH | Works out the gains from how alike AUTO measured the ends to be, so neither case moves the level. With AUTO off it behaves like EQ POWER. |

## AUTO

The search runs when the knobs have stopped, for each lane on an AUTO loop, whatever pattern it is in. Until it lands the lane plays the knobs' points, and a sound already looping moves to the new points when it next starts.

**EQ AMP** looks for two rising zero crossings where the wave on both sides of the join is most alike. It often needs no fade at all: with XFADE off it was the best setting on most material tested. Use it on anything with a pitch.

**EQ POWER** looks for two steady places where the sound is equally loud over the length of the fade, then lines them up as well as the wave allows. Use it where you want a real fade.

Turning AUTO sets CURVE to MATCH; turn CURVE afterwards to choose another. A step that locks START or END plays the knobs' points, with no search.

No setting makes a decaying note steady: a loop in a piano's decay still steps in level at each lap.

## What to expect

Measured on seven kinds of synthesized sound (pad, string, piano, bass, breath, drums, texture), over 22,000 loops, against the knobs' own points. These are sample steps at the join, not listening.

- With XFADE off, EQ AMP takes 5 to 27 dB off the click, and is better 71 to 96% of the time. EQ POWER gains up to 13 dB on drums and bass, and about nothing on breath.
- With a fade, the points matter much less: both searches gain about 1 dB or less at 50 ms, a little more on drums and bass at 5 ms.
- MATCH holds the level best of the four curves: the middle of the worst dip or swell is 1.5 dB, against 2.2 to 2.7 dB for the others.

So: a sound with a pitch wants EQ AMP and no fade, and a sound without one wants a fade of 50 ms or more.

## Slices

A slice whose lane has PLAY on LOOP loops inside itself, with the same fade, and % means a share of the slice.

## How it is kept

Found points go with the sample in `Project::loops`: 32 entries, one for each sample, START, END, AUTO, SEARCH and fade in use. With all 32 in use a further lane stays on its knobs' points. Points are for a file of a given length, and are found again if the file changes. A sample missing from the card loses them.

`findLoopPoints` (`app/Looper.cpp`) runs in the main loop. It has been estimated at about 50 ms with the audio idle, several times that with the audio busy, and the panel waits while it runs. The audio does not. It has not been measured on the board.
