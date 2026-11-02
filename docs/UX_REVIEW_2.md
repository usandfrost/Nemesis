# Usability review of the whole app (2026-10-30)

**Status (same day, after the owner's go):** done: 1 REC says `NOT IN THIS VERSION`; 2 corner names `FLT 1`, `FLT 2`, `SUB/UNI`; 3 dimming (PD off SIN, the unison stack at x1, filter 2 and its curve while off, the LFO cells at depth 0);
4 (the duplicate ENV knobs): **not folded** - all 28 library presets that use `ENV>CUT` run `ENV AMT` at its top and add the rest of the sweep with it (up to 4 octaves, ENV AMT alone stops at 2.8), so
folding would cut their range; the knob is now `ENV AMT+` (it says what it does) and stays; the unison detune is folded: OSC 1's slot is retired, files migrate on load (`migratePages`), exactly as the voice always used the larger;
5 BROWSE `OK LOADS IT` / `SCROLLING PLAYS IT`; 6 MIXER 2/2 leads with `WILL IT FIT` (a sentence and a bar); 7 locks show in the curve; 8 envelope segments at least 10 px; 9 names (`PROJECT`, `AMP SIM`);
10 a hold of `L` (0.6 s) goes home; 11 toasts: top under the bar on STEP and the sound pages, on PLAY the half the selected lane is not in; 12 `1 bar`; 13 the corner BPM is hidden where TEMPO is a knob;
14 `CLEAR AGAIN EMPTIES THE LANE`; 15 a hand-added step on PLAY is filled with a white underline; 17, 18, 22 on the web bench. Left: 19-21, 23 (compare the plate's glyphs with the device), 24.

Made by driving the real firmware UI (the web build runs the same `Ui`, `Screen` and `Instrument` code as the board) through every screen with
`tour.mjs` (scratchpad/review): PLAY, STEP 1 and 2, all nine SOUND pages, MIXER 1 and 2, BROWSE (scroll, load), SONG, FX, PROJECT 1 and 2, a step lock,
PLAY with the sequencer running, COPY, MUTE, CLEAR, REC. Then the web bench's presets page and the new Drumboy front plate. The screenshots are in
`scratchpad/review/shots1`, `shots2` and `web_presets.png`. The earlier review (docs/UX_REVIEW.md, all 20 items done) was not repeated: this one
looks for what is left, and for what the later features (FX, PROJECT/MIDI, lean voice, lanes on MIDI) added.

Judged against the concept's own rules (docs/UI_CONCEPT.md section 2): a knob always says what it does; no hidden state; one name per parameter.

## What already works (keep it)

- The fixed 8-knob layout with T1..B4 tags, the page colour in the corner and knob row, the part colour on every lane. You always know where you are.
- **One rule for "press the key again", and it holds:** a screen that shows tab chips (STEP, MIXER, PROJECT, FX) flips to its next view; every screen
  without chips (the sound pages, FILE, SONG) goes back. The toast says which view you are on.
- The lock flow (hold a step, pick a sound page, turn: yellow-green dashed step, `LOCK` tag on the cell, `L1 S2` in the corner, a toast that says what to do).
- Disabled cells are dimmed and say why (`ARP RATE` with the arp off; `MIDI CH` `IN PROJECT` with an `INFO` tag; `-` cells).
- Units everywhere (Hz, ms, %, st, ct, bars); the earlier raw 0-127 numbers are gone. `SHIFT + - UNDOES` is announced where it matters.
- PROJECT's SD CARD line explains itself (`B1 picks a slot, OK loads, SHIFT + OK saves`): the pattern to copy elsewhere.

## High: misleads, or does nothing without saying so

1. **REC does nothing, silently.** `Ui.cpp:889` returns for `Key::Rec`: no toast, no state on any screen. Every other key that cannot act says so
   (`OSC 2: NOT ON A SAMPLER PART`). Either build it, or say `REC: NOT YET` and dim the key legend; do not leave a dead key.
2. **Page names are cut off in the corner.** `SUB / UNISON` shows as `SUB / U`, and `FILTER 2` shows as `FILTER`, the same as filter 1: on the
   FLT 2 page nothing says it is the second filter (only B1 `ROUTE` differs). Fix the label width (or a short name: `SUB/UNI`, `FLT 2`) and put the filter
   number in the `PART A · IRON SQUELCH` line.
3. **Controls that do nothing look live.** The UI already dims what does not apply (the arp, the wavetable's `PD AMT`); the same is missing for:
   - `PD SHAPE` and `PD AMT` on OSC 1 and OSC 2 when WAVE is SAW, TRI or SQR (phase distortion only acts on SIN);
   - `UNI DET`, `UNI BLND`, `WIDTH` while UNISON is x1;
   - the whole FILTER 2 page, its response curve included, while `ACTIVE` is OFF (the curve is drawn as if it were in the signal);
   - `LFO DEPTH`-dependent cells and the flat LFO line while the depth is 0%.
4. **Two knobs for one thing.** FLT 1 `ENV AMT` and MOD `ENV>CUT` both set how far Env 2 moves the filter-1 cutoff (`PatchMap.cpp:170` adds them).
   Turn one and the other's contribution is invisible. Keep one (`ENV AMT` on the filter page) and give `ENV>CUT` another job, or label it `ENV>CUT +`.

## Medium: slows the player down or surprises

5. **BROWSE does not say how to load.** There is no `OK = LOAD · L = BACK` line, `B2 SCROLL X3` has no value, `BANK FACTORY` (dimmed, `INFO`) sits above a list
   of 200 presets that are not all the factory's. Preview exists (`Ui.cpp:1557`, after a pause) but nothing on the screen tells you. Add the one-line hint
   in the footer (as PROJECT does) and show `PREVIEWING` when it plays.
6. **MIXER 2/2 is a wall of jargon in small grey type.** `VOICE COST 5 of 5`, `COST 5.0/5` (5 what?), `HEAVY FILTERS A 0 B 0 C 0 (RULE: AT MOST 1 PER LANE)`,
   `prio keep`, `release oldest voice (-1.00)`. For the player the question is "will it drop notes, and which first". Lead with that sentence and a CPU bar;
   keep the ladder behind the INFO cells.
7. **A lock does not show the locked value in the picture.** With a lock on CUTOFF the cell reads `2.2k Hz` (lock colour) but the curve and the caption
   (`LADDER 1646 Hz`) still show the part's value. While a lock is open, draw and caption the locked value.
8. **The AMP envelope curve has a jagged spike at the attack** (a 2 ms attack and 3 ms decay are drawn on a scale of about a second; the line is
   sampled too coarsely). Scale the time axis with a floor on the first segment, or draw the segments analytically.
9. **Names that differ between key, screen and effect.** The key `I` (INFO) opens `PROJECT`; the key `L` (LANE) means `BACK` and, from PLAY, `HOME`; the FX
   tab `AMP` (an amp simulation) is also the name of the AMP envelope key; the bottom-left key's cap is a power symbol but it is the FX key (`Key::Spare`).
   One name per thing: `PROJECT` on the key or `INFO` on the screen; `AMP SIM` for the effect.
10. **Back goes through the history, not home.** From BROWSE (reached from MIXER 2) `L` returned to MIXER 2/2, not PLAY. The corner says `L BACK`,
    which is true, but from three levels deep the player needs `L` several times (or SHIFT + L). Say `L BACK  SHIFT+L HOME` once on the first deep screen, or make
    a long press of `L` go home.
11. **Toasts cover the step being edited.** The centred toast sits on the bottom rows (`PROB` in STEP, rows 9-10 in PLAY) and stays for a while; with
    the long ones (`IRON SQUELCH > PART A · SHIFT + - UNDOES`) the row you are working on is hidden. Put toasts in the strip between the grid and the footer,
    or at the top right under the transport state.

## Low

12. `1 bars` (SONG, one bar): singular. `7 bars` fine.
13. The corner `118 BPM` repeats `TEMPO 118` on PLAY and PROJECT (it is useful on the other pages; hide it where the knob shows it).
14. `CLEAR` on a lane says `EDITS CLEARED` but the Euclid pattern stays (by design: the concept's hold-to-turn-Euclid-off); a player who presses CLEAR to empty a
    lane sees the steps remain. Add what is left to the toast (`EDITS CLEARED · 4 HITS REMAIN`).
15. Steps on the PLAY grid that are on the Euclid base and steps added by hand look the same except for a faint inset: the earlier item 19 is better but still subtle.

## Web bench (presets page)

17. **The Play card and the piano are below the fold** (the side column: detail, verdict, engine, effects, play). On a 800 px window you scroll to play a
    preset or change register. Order the column: detail, play (with the engine switch), verdict; effects last, and shorten its paragraph.
18. `lean grade A` and the `A/B/C` chips mean "how close the lean voice is to the full one on the host", which the page never says; a tooltip or a
    one-line legend under the list.
19. The list has no column headings (id, name, category, voices, grade, verdict).
20. The verdict box says `Claude reads this`: fine for you, but write what it does (`saved to web/feedback/verdicts.json`) and keep that last line.
21. Reject has seven reason chips and a text box, Approve has nothing: a quick `too quiet` after listening is one click, good; approving with a comment
    (`good, but ...`) is not possible.

## Web bench (Drumboy tab: the front plate)

22. The computer keyboard does nothing on this tab (every key needs the mouse; a lock needs a held click). Map the keys: 1-0 to the layer keys, `Space`
    PLAY/STOP, `Shift` SHIFT, arrow keys to UP/DOWN/LEFT/RIGHT, `Enter` OK, hold-to-lock with the mouse stays.
23. The key glyphs are copied from the photograph (the ● + ○ / ◁ ||| ▷ / C − P cluster); what each key does in our firmware comes from `Ui.cpp`'s key table. The
    hover line names it. **Please compare the three clusters with the device once** (the UP/ADD/DOWN row and the right 3 x 3 in particular): if a glyph
    sits on a different key on your unit, it is one line in `panel.js` (`KEYS`).
24. Audio starts with the first press on the plate, but the header still says `audio off` for a moment; fine, but `Start audio` could be highlighted until then.

## Suggested order

1 (REC), 2 (names), 3 (dimming), 5 (BROWSE hint) first: all small, all in `Ui.cpp`/`Screen.cpp`/`Params.cpp`. Then 4 (the duplicate ENV knobs, a design
decision), 6 and 11 (MIXER text, toast position), 7 and 8 (drawing). The web items (17, 22) take an hour and make the bench quicker to use.
Nothing here needs the board.
