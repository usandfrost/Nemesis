# Usability review of the panel mockup (version 9, 2026-10-26)

**Status (version 11):** all 20 items are decided and done (the decisions are in docs/UI_CONCEPT.md section 6f). Items 1, 2, 3, 7 in v10;
the rest in v11. The panel tests (259 checks) cover each change, and a planted bug in each is caught.

Made by rendering every screen (PLAY, STEP views 1 and 2 for a synth and a drum lane, LOCK, the SOUND pages, MIXER 1 and 2, BROWSE, the
sampler page, PROJECT, the key-legend overlay) in headless Chrome and reading the key map and bindings. Ranked by how badly a problem
misleads the player or loses work.

## High: misleads, or loses work

1. **FIXED (v10).** **One label, two meanings; one meaning, two labels.** PLAY B1 `NOTE` is the lane's root; STEP B1 `NOTE` is the selected step's
   offset. The other way round: PLAY `EUCL n` = STEP `LENGTH`, PLAY `EUCL k` = STEP `HITS k`, PLAY `NOTE` = STEP `ROOT`.
   Fix: one name per parameter everywhere (`ROOT`, `LENGTH`, `HITS`); `NOTE` only for the step offset.
2. **FIXED (v10: lock lime `#d7ff3c` and dashed, cursor white, playhead an underline).** **Cursor, lock and STEP mode share one colour.** `--lock` and the STEP accent are both `#ffb02e`, and part A is orange `#ff8a3d`. On a
   part-A lane the selected step, the locked step and the trigs are all orange; a cursor outline and a lock outline look the same.
   Fix: a lock colour no part uses (e.g. a light violet, plus a dashed outline), and the cursor as a white outline.
3. **FIXED (v10: pressing a step selects it; hold + turn edits it; hold + release without turning opens its lock).** **Selecting a step flips it.** In STEP a tap toggles the trig, so picking a step to edit its velocity, gate or (new) sample turns it
   off. The ways around it are the arrows, or a long press, which also opens a lock. Fix: hold a step key + turn a knob edits that step
   (the Elektron convention); a tap stays a toggle.
4. **Destructive actions with no undo or confirmation.** `−` twice empties a lane; BROWSE T4 `INSTRUMENT` turns a part from synth to
   sampler at once (changing what 3 to 4 lanes play); `OK` in BROWSE loads a preset over the part's edits. Fix: SHIFT for the first two
   (as T3 now), and a one-level undo (e.g. SHIFT + `−`).
5. **Duplicate and arbitrary key legends.** `F` is both BROWSE and FILL, `M` both MIXER and MUTE, `S` both STEP and SONG; PART is `D`.
   FILL and `+` do the same (re-roll Euclid), so one key is wasted. The legends are marked provisional; settle them before anything is
   printed.

## Medium: slows the player down

6. **Raw 0-127 numbers.** Cutoff `100`, attack `0`, release `28`, pan `64`, sample start/end `0`/`127`. The graph caption even says
   `BP 392 Hz` while the knob says `100`. Show units: Hz, ms, %, L/C/R.
7. **FIXED (v10: captions moved to the top right).** **Text overlap on the SOUND pages.** The graph caption (`A 2 ms D 3 ms ...`, `BP 392 Hz`, the GRIT and MOD captions) is drawn on top
   of the `PART A · ALGO KNOCK` label, so both are unreadable on FILTER, AMP, GRIT and MOD.
8. **Text too small for the device.** The lane header, mixer details, PROJECT ladder and step-view captions are 11 to 13 px on a
   960 x 540 panel of a few inches: about 1 mm tall, unreadable at arm's length. Keep everything that matters at 16 px or more.
9. **Mockup-only controls in the product UI.** MIXER view 2 `BOARD SPEED` and `WORST CASE` simulate the board; they do not belong on the
   instrument. Move them to a mockup-only panel.
10. **Hidden second views.** STEP and MIXER have a second view behind a second press of their key. STEP says `VIEW 1/2` in small type;
    MIXER says nothing. Show the view as tabs in the header.
11. **Status corners repeat the knobs.** `118 BPM` sits in the top-right corner on every screen and again on PLAY T1 and PROJECT T3;
    `P1 PAT` repeats PLAY T4. When the CPU degrades, `DEGRADE` replaces the pattern number. Use the corners for what no knob shows
    (playing / recording, the degrade state) and keep the pattern number visible.
12. **The "back" corner is a mouse affordance.** `◀ HOME` is clickable in the mockup. The hardware screen is not known to be touch; on the
    device only the L key goes back, and it goes all the way home, not back one level.
13. **BROWSE shows a stale tag.** `PART C` is shown next to the preset Soft Sheen, but part C is the sampler: the tag is left over from
    before the part became a sampler.
14. **No preview while browsing synth presets** (samples audition on scroll, presets do not).

## Low

15. `GATE` shows `0.6` in the step grid and `0.56` on the knob: one precision, please.
16. On synth view 2, the INV and VOICING cells of steps without a chord are blank but still filled in the trig colour: visual noise.
17. `VOICES` sits on the AMP page and in the MIXER. It is a part setting; the MIXER is enough.
18. On a sampler part, the OSC 2 and SUB keys silently open the SAMPLE page instead of saying they do not apply.
19. On PLAY, steps tapped on top of the Euclid rhythm have only a faint white inset; hard to see.
20. SHIFT sits in the bottom-right corner, far from T3, and means two things (coarse steps on every knob; "really move" on T3).

## What works well

The fixed 8-knob layout with T1-B4 tags, the mode colour carried by the corner and the knob row, one physical key per lane, the lane
colours, the chord-versus-voices warning, LOCK shown in the knob that is locked, the drum view's per-step sample row, and the PROJECT
key strip.
