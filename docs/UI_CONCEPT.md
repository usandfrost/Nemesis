# Drumboy CATHODE: interaction concept (draft 1)

Status: a proposal to argue with, not a spec. Facts marked **[stock]** come from the Randomwaves firmware/hardware repo,
**[render]** from `ref/01 Hardware/Drawing/Drumboy Pro.png`. Anything marked **❓** I could not verify and need you to confirm.

## 1. What the panel gives us

| Element | Count | Notes |
|---|---|---|
| Encoders | 8 | 4 above the screen (**T1-T4**), 4 below (**B1-B4**). Only A/B lines in the stock pin map: **no push switch** [stock]. |
| Layer keys | 10 | The translucent row along the bottom, each with an LED [render]. |
| Left block | 16 | Top trio ▢ ▷ ○; a 3x3 (● + ○ / ◁ ‖‖ ▷ / C − P); bottom trio M F S [render]. Stock names: RESET, PLAY/STOP, REC, ADD, UP, CLEAR, LEFT, CENTER, RIGHT, COPY, DOWN, PASTE, MUTE, FILL, STYLE, SONG. |
| Right block | 16 | Top trio F D M; a 3x3 of icons (sine, tuning fork, dotted circle / two filter curves, sliders / three circles); bottom trio L I S; one grid key [render]. Stock names: FILE, DRUMKIT, MIXER, RHYTHM, METRONOME, IO, FILTER_0, FILTER_1, EQ, EFFECT_0, EFFECT_1, REVERB, LFO, LAYER_INST, LAYER_SONG, KNOB. |
| Screen | 960 x 540 landscape | A 4-cell header sits above T1-T4 and a 4-cell footer above B1-B4 [render]; the stock main view is a 10-row lane list with a step timeline. |
| LEDs | 42 | Layer-key LEDs, 12-segment EQ meters, 3 status LEDs [stock]. |

**Keycap legends** (decided 2026-11-01, on the board): the 3x3 keeps the stock wiring, so + − ◁ ▷ are a cross around ‖‖ (OK): + is UP, − is DOWN, ● is ADD and ○ is CLEAR.
This document uses those names (NAV-UP/DOWN/LEFT/RIGHT, OK, ADD, CLEAR, COPY, PASTE).

## 2. Principles

1. **A knob always says what it does.** The screen cell next to each encoder shows its parameter name and live value. No hidden state.
2. **Few modes, always visible.** Four modes (PLAY, STEP, SOUND, BROWSE); the current one is always shown in the same screen corner and
   by which key LEDs are lit. A held step key adds a fifth, temporary mode: LOCK.
3. **Sound never stops for editing.** The sequencer keeps running; edits land on the next step or block.
4. **One key, one place.** A key keeps one meaning per mode. Hold-for-secondary is used only for MUTE and COPY/PASTE.
5. **Turning is cheap, committing is not needed.** Parameter changes are live and shown as they happen; nothing to confirm except saving a preset.

## 3. The model (your "Mixed" answer)

```
Project
 ├─ Part A  (patch, N voices)   lanes 1-3
 ├─ Part B  (patch, M voices)   lanes 4-7
 └─ Part C  (patch, K voices)   lanes 8-10        N + M + K <= total voices (target 5, set by measurement)
        Lane = notes of one rhythm layer: steps (length 1-32), Euclid generator, mute
        Step = on/off, note (offset from the lane root), velocity, gate length, probability
```

- 2 to 4 parts, each a separate CATHODE engine with its own patch; a part's lanes all play that patch.
- The 10 layer keys always mean "lane 1-10", in PLAY mode. In STEP mode the same keys become steps (section 4).
- **Engineering consequence:** each part is its own engine object. The fork must prepare only the voices a part actually has
  and share read-only tables, or four engines would not fit in SDRAM. That work is not done yet (see plan, item 3.0).

## 4. Modes and keys

| Mode | How you get there | Layer keys (10) | T1-T4 / B1-B4 |
|---|---|---|---|
| **PLAY** (default) | power-on, or BACK from any mode | select lane 1-10 (LED: lit = selected, dim = has steps, flash = hit) | tempo, swing, part select, pattern / selected lane's note, level, Euclid n, Euclid k |
| **STEP** | STEP key (❓ which cap) | steps 1-8 of the current page; keys 9-10 = page back / forward; hold a step key + a sound page = p-lock | lane root, length, ... / step note, velocity, gate, probability |
| **SOUND** | any sound-page key | lane select (still works) | 8 labelled parameters of that page |
| **BROWSE** | FILE | unused (dim) | list scroll, bank, category, preview |

Why 8 steps per page and not 10: 16- and 32-step patterns then line up with bar boundaries (2 and 4 pages). Keys 9-10 are free for paging, so
the 10-key row is fully used. If you prefer 10 steps per page, say so; bars then straddle pages.

Global keys (same in every mode, except that CLEAR also removes the locks of a latched step): PLAY/STOP, REC, RESET; MUTE (hold + layer key = mute that lane); COPY / PASTE (hold COPY, press a
lane or step, press PASTE on the target); CLEAR (clears the step under the last-pressed step key, or the lane when held).
NAV keys move the cursor in the grid, and OK toggles the step under the cursor, so a step can be edited without the layer keys too.

## 5. Screens

All sketches are 960 x 540 and show which encoder drives which cell.

```
PLAY                                     [PLAY]   PART A  ♩=120
 T1 TEMPO  | T2 SWING | T3 PART | T4 PATTERN
 ----------------------------------------------------------------
 L1  bass   M  x . . x . . x . | x . . . x . . .     <- 16..32 steps, playhead
 L2  bass   M  . . x . . . x . | . . x . . . . x
 L3  bass      ...
 L4  lead   ...                                       (10 rows, lane colour = part)
 ----------------------------------------------------------------
 B1 NOTE   | B2 LEVEL | B3 EUCLID n | B4 EUCLID k      (selected lane)
```

```
STEP (lane 2, page 1 of 2)               [STEP]   L2  steps 1-8
 T1 ROOT   | T2 LENGTH | T3 SWING | T4 EUCLID r
 ----------------------------------------------------------------
        1    2    3    4    5    6    7    8      <- keys 1-8
 on     x    .    x    .    x    .    .    x
 note   0    .   +7    .   +3    .    .   -5      cursor/last-pressed step is highlighted
 vel   100   .   80    .  127    .    .   60
 gate   50%  .  25%    .  75%    .    .   50%
 ----------------------------------------------------------------
 B1 NOTE   | B2 VELOCITY | B3 GATE | B4 PROBABILITY    (the highlighted step)
```

```
SOUND page (e.g. FILTER 1)               [SOUND]  PART A  FILTER 1
 T1 MODEL  | T2 CUTOFF | T3 RESO   | T4 DRIVE
 ----------------------------------------------------------------
        (filter response curve, drawn live)
 ----------------------------------------------------------------
 B1 KEYTRK | B2 ENV AMT | B3 HPF    | B4 MIX F1/F2
```

```
BROWSE (presets)                         [BROWSE] PART A
 T1 BANK   | T2 CATEGORY | T3 PREVIEW VOL | T4 (unused)
 ----------------------------------------------------------------
   > Soft Sheen        PAD
     Tape Warble       PAD
     Dimension Bloom   PAD                          OK = load into this part, BACK = cancel
 ----------------------------------------------------------------
 B1 SCROLL (fine) | B2 SCROLL (page) | B3 | B4
```

## 6. Euclidean generator

Per lane: **n** (steps, 1-32), **k** (hits, 0-n), **r** (rotation, 0..n-1), shown in PLAY (B3/B4) and STEP (T4).

- Turning n, k or r regenerates the lane's base pattern immediately and the grid redraws (Bjorklund distribution).
- **Manual edits are an overlay on the Euclid base**, not a replacement: toggling a step stores "flip this step", so changing k later
  keeps your hand-made changes. CLEAR on a lane removes the overlay, a second press also turns Euclid off, and turning k brings it back. Holding CLEAR for a
  second on PLAY or STEP empties every lane in the pattern (rhythm, tapped steps, locks, slice SYNC); the lanes keep their sounds, and one SHIFT + CLEAR brings it all back.
- Lane length is independent per lane, so lanes of different n run polyrhythmically against the shared clock.

## 6b. Parameter locks (p-locks)

A p-lock is a sound parameter value stored on ONE step. When that step fires, the lock replaces the part's value for that note only;
the next step plays the part's normal sound again.

- **Setting one.** In STEP view, hold a step key (about a third of a second; with a mouse a long press latches it), open any sound page
  and turn a knob. The cell turns amber and reads LOCK, the screen header shows the locked lane and step, and the value is stored on the
  step. A short tap on the same step key leaves lock mode; a tap on another step moves the lock there, which makes it quick to lock the
  same parameter across several steps.
- **Seeing them.** Steps with locks show an amber mark in the PLAY grid and `LKn` (n = how many) in the STEP view.
- **Clearing.** CLEAR while a step is latched removes that step's locks. CLEAR outside lock mode only removes manual step edits.
- **What is lockable.** Every per-voice parameter on the sound pages: oscillator shape and levels, filter cutoff / resonance / drive,
  envelope times, grit, pan, level. Not lockable: the voice count, and the GLOBAL page (master, bend range, MIDI channel, tuning, clock).
  Step note, velocity, gate and probability are already per-step and need no lock.
- **Storage.** A lock is (parameter id, value). A few per step is typical; budget about 8 per step, 32 steps x 10 lanes x 4 patterns,
  3 bytes each, about 30 KB. It lives in the project, not in the preset.
- **Engineering consequence.** The engine needs a way to apply per-voice values to the voice a note was just assigned. `noteOn` picks the
  voice internally, so the fork gets a small hook that returns the voice index (or applies a lock list during assignment), and the
  locked values are restored when that voice is next re-used. The upstream engine already has per-voice setters for every lockable
  parameter. This is the one UI feature that needs a change inside the DSP fork, not just on top of it.
- **Euclid interplay.** Locks follow the step, not the rhythm: retuning the Euclid k or rotation moves hits, but a lock stays on the step
  index it was set on, so it may end up on a step that is no longer a hit. Such a lock is kept (shown dimmed) and used again if the step
  becomes a hit.

## 6c. Chords and arpeggio

- **Chord per step.** Each step can carry a chord: type (maj, min, sus2, sus4, dim, aug, maj7, min7, dom7, min9, 5th, octave), inversion
  0-3, voicing (close or open) and strum (0-120 ms). The step's note is the chord root. Chord data is per step, edited on STEP view 2.
- **Arpeggio per lane.** Mode (off, up, down, up/down, random), rate (1, 2, 3, 4, 6 or 8 notes per step), octave range 1-4 and gate. The arp
  plays the chord's notes, or repeats a single note as a ratchet, spread evenly across the step. It restarts on every triggered step.
- **STEP has two views**, toggled by pressing STEP again. View 1: ROOT, LENGTH, HITS, ROTATE above; NOTE, VELOCITY, GATE, PROB below. View 2:
  ARP mode, rate, octaves, gate above; CHORD, INVERSION, VOICING, STRUM below. Steps with a chord are marked on the PLAY grid.
- **Cost.** A chord is several simultaneous notes, so it uses several voices. The STEP header warns when a chord needs more voices than the
  part has (the oldest voice is stolen). With an arp on, the notes are sequential and one voice is enough.
- **Sampler lanes** use the same chord and arp data, as pitched hits of the lane's sample.

## 6d. Sampler instrument and navigation

- **A part is either CATHODE or SAMPLER** (set with the INSTRUMENT knob in the browser). A sampler part is drum-style: each of its lanes plays
  its own sample, with its own sample page (sample, start, end, play mode one-shot / gated / loop, pitch, fine, reverse, level), filter,
  amp envelope and choke group. A CATHODE part keeps its patch per part.
- **Sampler pages reuse the right-hand keys**: OSC 1 opens SAMPLE, FILTER 1 / AMP / MOD / GRIT / GLOBAL work as on a synth, OSC 2 opens SLICE, SUB opens LOOP, and the key
  with no sampler counterpart (FILTER 2) says so instead of doing nothing. The sound parameters are p-lockable, including START and
  SAMPLE, so a step can fire a different slice or a different drum; of SLICE and LOOP, a step can lock SLICE, RANDOM, RND REV and XFADE.
- **Browser.** For a sampler part the browser lists samples (by kick, snare, hat, perc, bass, fx) and previews them as you scroll; OK assigns
  the sample to the selected lane.
- **Voice budget in cost units.** A plain voice counts 1 and the options add to it; the lean voice's numbers are measured on the board (docs/LEAN_VOICE.md) (for the CATHODE engine, a sampler voice 0.25 is an estimate). The mixer
  shows the total against 5.0 and turns red when it is exceeded.
- **Getting back.** HOME (the L key) goes to the main lane screen from anywhere. ESC, or the top-left corner of the screen, goes back one step
  (to where you came from, with an open p-lock kept). Pressing the key of the page you are on also goes back, and so does OK after choosing a
  sample or preset. Moving between sound pages keeps the original back target, so one BACK always returns to where you started.

## 7. Presets and sound editing

- Presets live on the SD card in a compact binary format (a few hundred bytes), produced offline from the upstream JSON by a host tool.
  The device needs no JSON parser. The stock upstream values are 0-127 integers keyed by parameter name, so conversion is direct.
- FILE opens BROWSE for the current part. OK loads; the previous sound stays until OK, so browsing never interrupts a running part.
- **Sound pages** (proposal, 8 parameters each, drawn from the voice stage setters): OSC 1, OSC 2, MIX / SUB / UNISON, FILTER 1,
  FILTER 2 / HPF, GRIT (bit depth, companding, rate), AMP ENV, MOD (LFO / envelope amount). That is 64 parameters, plus the
  global page (tempo, swing, master, bend range, MIDI channel).
- Parameter smoothing is required on every encoder-driven value (the engine's setters change coefficients instantly); added in the
  parameter layer, not in the DSP.

## 8. Feedback and timing

- Layer-key LEDs show lane state; the 12-segment meters show part levels in PLAY and the audio CPU load in a service view.
- Sequencer clock runs inside the audio interrupt from the block counter: tick resolution 32 samples (0.73 ms). The step is placed
  inside the block by rendering in two chunks, so jitter is below one sample period. MIDI clock in/out later.
- Screen redraws only the cells that changed (the panel is driven over a CPU-driven 16-bit GPIO bus). Target 30 fps for the grid and
  playhead, ~5 ms for a parameter-cell update.
- UI to audio traffic uses the same single-producer / single-consumer queue as MIDI: encoder and key events are queued and applied at
  the start of the next audio block. The engine is never touched from the main loop.

## 9. Open questions

1. Keycap legends (section 1) and which cap is STEP, BACK, OK.
2. Voices per part: fixed split (for example 2 + 3) or dynamic stealing across parts? Dynamic is friendlier but needs the engine to
   share one voice pool, which the current engine does not.
3. Pattern memory: how many patterns per project, and is there a song/arrangement mode (the stock SONG and LAYER_SONG keys suggest the
   hardware expects one)?
4. Swing and shuffle: global only, or per lane?
5. Should REC (live record from MIDI or from the layer keys) exist in v1?

## 10. Build order

1. Sequencer core (steps, Euclid, overlay, clock, MIDI clock) with host tests, no UI.
2. Parameter table (the 64 parameters), binary preset format and the host converter; load a preset and play a pattern headless.
3. Display driver, key/encoder drivers from the stock project; the PLAY screen.
4. STEP and SOUND screens, BROWSE.
5. Multi-part engines (needs the per-voice prepare and shared tables). Sampler instrument behind an `Instrument` interface (see the sampler notes).
6. P-lock hook in the DSP fork (apply a lock list to the voice a note was assigned), then lock storage and playback.

## 6e. Project screen, key and scale, sample per step, SHIFT for setup moves (mockup version 9)

- **PROJECT** (the I key, was INFO): T1 KEY, T2 SCALE (off, major, minor, the five other church modes, harmonic and melodic minor,
  major and minor pentatonic, blues), T3 TEMPO, T4 SWING. Below: a 12-note strip with the scale notes lit and the key marked, then the
  voice cost, heavy filters and degrade ladder that INFO showed.
- **Scale lock**: with a scale set, every note a CATHODE part plays snaps to the nearest scale note (a tie goes down), chords and
  arpeggios note by note, so they become diatonic. Sampler (drum) lanes are never snapped. The STEP note row shows the note that will
  sound under each offset, and the lane header shows the key.
- **Sample per step** on drum lanes: STEP view 2 (press S again) is SAMPLE PER STEP instead of chord and arp, which mean nothing on a
  drum. Pick a step with the arrows or by holding its key; B1 STEP SAMPLE picks that trig's sample (turn fully left for the lane's own).
  It is stored as a sample lock, a p-lock on the sample number, so it plays through the lock path, shows LK on the step and is cleared
  with the other locks. One drum lane can then carry several sounds (kick, rim and clap on one lane), which frees lanes for other parts.
- **SHIFT + T3** moves a lane to another part. Without SHIFT the knob only says how to do it: a setup move must not happen by a stray turn.

## 6f. Decisions from the usability review (docs/UX_REVIEW.md), mockup version 11

| # | Decision |
|---|---|
| 1 | One name per parameter: ROOT, LENGTH, HITS, ROTATE everywhere; NOTE only for the step's note offset. |
| 2 | Lock colour lime and dashed; cursor solid white; playhead a white underline. |
| 3 | Pressing a step key selects it; hold + turn edits that step; tap toggles; hold + release without turning opens its lock. |
| 4 | One-level undo: SHIFT + ○. Undoable: clearing edits, emptying a lane (○ twice; one undo restores what was there before the first press), paste, loading a preset or a sample. Switching a part between synth and sampler (BROWSE T4) needs SHIFT. |
| 5 | The printed legends stay. FILL is a real fill: hold it and every Euclid lane plays twice its hits; ● keeps the re-roll. |
| 6 | Units everywhere: Hz for cutoff and HPF, ms / s for envelopes, % for amounts and levels, L/C/R for pan, st / ct / oct for pitch, kHz for the converter rate, Hz for the LFO rate, bit for bit depth. |
| 7 | Graph captions at the top right, clear of the PART label. |
| 8 | 16 px minimum for every text on the screen. To make room the STEP header drops what the knobs show, the PROJECT ladder shows 6 steps, PROJECT lost the "active now" row. |
| 9 | BOARD SPEED and WORST CASE moved off the instrument into a "CPU simulation" control above the mockup. |
| 10 | Views as tabs: STEP "NOTES / CHORD + ARP" (drums: "NOTES / SAMPLE PER STEP"), MIXER "LEVELS + VOICES / PRIORITY + DEGRADE"; a second press of the mode key switches. |
| 11 | Corners carry status: top left mode and "L BACK", top right transport and tempo (a red D-level badge when degraded), bottom left the pattern (always), bottom right the part. |
| 12 | Not a touch screen: corners are labels. L goes back one level, a second L goes home, SHIFT + L goes home at once. |
| 13 | BROWSE never tags a synth preset with a part that is now a sampler. |
| 14 | Scrolling to a synth preset plays three notes in the project key on a temporary copy of the preset; OK loads it. |
| 15 | GATE in steps with two decimals in the grid and on the knob. |
| 16 | On the chord view, INV and VOICING of steps without a chord are dark cells with a dash. |
| 17 | VOICES lives only in the MIXER (a part setting); AMP B4 is now VEL>ATK (a harder hit attacks faster). |
| 18 | On a sampler part FLT 2 is dark and says "NOT ON A SAMPLER PART". OSC 2 opens SLICE and SUB opens LOOP. |
| 19 | PLAY: Euclid hits filled, tapped-on steps hollow, tapped-off hits crossed. |
| 20 | SHIFT stays where it is; on value knobs it means coarse, on setup knobs (T3 PLAYED BY, T4 INSTRUMENT) "really do it", and those knobs are marked SHIFT. |

**Wording:** PATTERN means only the P1-P4 container (all 10 lanes and their steps). The rhythm a lane's LENGTH, HITS and ROTATE produce is
the **Euclid rhythm**. The view that edits one lane's steps is STEP (there is no "pattern mode").

## 7. Patterns, songs and the project file (mockup version 15)

Decisions: 16 patterns per project; a song row is a pattern + repeats; one project file holds everything; a pattern picked while
playing waits for the end of the current one.

- **Patterns P1-P16.** A pattern is all 10 lanes with their steps; it lasts as long as its longest lane. PLAY T4 PATTERN or SHIFT + layer
  key 1-10 picks one. Stopped: at once. Playing: it is queued (T4 shows `P1 ▸P5`, the bottom-left corner `▸P5`) and takes over at the
  end of the current pattern, on the beat. SHIFT + C / SHIFT + P copy and paste a whole pattern (undoable), also into another project,
  because the clipboard survives loading one.
- **SONG** (the S key, bottom right). Up to 8 songs; a song is a list of up to 64 rows, each a pattern and how often it repeats
  (x1-x16), and ends by looping or stopping. T1 song, T2 row, T3 the row's pattern, T4 its repeats; B1 loop/stop, B2 length in bars,
  B3 time at the current tempo. + and − pick a row, ● adds a copy after it, ○ removes it (undoable), C / P copy and paste a whole song.
  PLAY on the SONG screen plays the song from the selected row; OK on a row while it plays jumps there at the end of the current pattern.
  While a song plays it chooses the pattern (the PATTERN knob says so), the transport corner shows `▶ SONG`, and the playing row is
  marked with its repeat count.
- **Project file.** A project is one file on the SD card: 16 patterns (every lane, step, lock, chord and per-step sample), 8 songs, the
  seven parts' sounds, key, scale, tempo, swing and master level. The Drumboy starts with an empty project: the lanes and sounds are there, with nothing to play. PROJECT lists the firmware's three factory projects (F1 DEMO, F2 TECHNO, F3 MASCHINE) and then 30 card slots, in one scrolling list. B1 walks it; OK loads a row (undoable). A factory project loads as a copy to change and save to a slot; the original cannot be overwritten or deleted, and it keeps its name in its first slot. On a slot, SHIFT + OK
  saves the whole project there (overwriting is undoable too), and ○ twice deletes it (undoable until the power goes or the next undoable action). The mockup keeps the card in the browser; the firmware will write the
  same structure to the SD card.
- The CPU details (voice cost, heavy filters, degrade ladder) moved from PROJECT to MIXER view 2, next to the priorities they depend on.

**Settings vs information.** A knob cell is one of three kinds, and they look different: a *setting* (coloured label, its T/B tag, the
knob active); *information* (grey label, an INFO tag instead of the T/B tag, a faint hatch, the knob shown idle; turning it only says
"INFORMATION ONLY"); *unused* (empty and dimmed). Information cells today: SONG LENGTH and TIME, MIXER COST, CPU LOAD and DEGRADE,
BROWSE BANK. A test turns every cell that looks like a setting on every screen and fails if one changes nothing.
