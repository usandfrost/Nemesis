# What the firmware needs to play the converted content

Written for the main session. Nothing under `firmware/`, `tests/`, `docs/` or `tools/` was changed; the working prototype of every item marked (P) is
`content/tools/PatchMap_wt.cpp` + `content/tools/shadow/audio/PatchMap.h`, a copy of the firmware's PatchMap with 62 changed lines that the renders use.

## 1. The WAVE knob and the table (P)

No new knob. OSC 1 and OSC 2 keep their eight knobs:

| knob | today | with tables |
|---|---|---|
| WAVE | SIN TRI SAW SQR (0..3) | 0..3 as now, then 4.. = wavetable slot `WAVE - 4` of the SD set (list shows the table name) |
| PD SHAPE | PD knee (WAVE = SIN only) | **POS**: position across the table's frames, 0..127 (the label changes with the WAVE value) |
| PD AMT | PD depth | unused with a table (shown as `-`) |
| MOD: LFO DEST "PD AMT", ENV>PD | PD amount | the table position, as the shadow does with `Dst::OscPos` instead of `Dst::PdAmount` |

`Params.cpp`: `WAVE.max` becomes `3 + tableCount` (a runtime value: `sanitizePages` clamps with `p.max`, so the clamp must read the installed count, or stay at 3 + 64
and let the player fall back to SIN for an empty slot). The WAVE list names come from the card's `index.tsv`. The converted `pages` keep WAVE at the nearest basic shape so that
today's firmware plays every preset (without the table); `table[]` and `tablePos[]` carry the real choice, and the firmware change is to write `4 + table` into WAVE and `tablePos` into PD SHAPE when it loads a record.

Position modulation by velocity/LFO/envelope works through the existing `OscPos` destination; one shared position moves both oscillators (engine fact, `cathode-preset-mapping.md` 1.5).

## 2. Loading tables from the card into SDRAM (P: load on first use)

* Format: `content/data/wavetables/FORMAT.md`. One file per bank, 64 byte header + 32 x 4 KB; 56 files, 7.3 MB for the current selection (all 200 banks: 28.5 MB at 32 frames).
* The loader reads int16, converts to float, calls `Wavetable::build()` (host prototype: `SharedTables::table(slot)`), keeps the result in SDRAM and binds it with `t.setWavetable(slot, tbl)` as `bindTable` does.
  Bind only on a change (setWavetable resets the oscillator phase), never from the audio interrupt: build the table in the main loop when a preset or the WAVE knob selects it, then hand the pointer over.
* **Memory is the problem, not the card.** Today's `Wavetable` keeps 11 full-length mips per frame as `Real` (float on the board): 32 frames = **2.88 MB per table**, 16 frames = 1.44 MB. Three parts with two
  tables each would need 17 MB; the engine already takes about 6.6 MB of the 16 MB (`docs/OPTIMIZATION_PLAN.md`). Options, cheapest first:
  1. Allow at most 2-3 bound tables at once (LRU, a part's oscillators sharing one) and use 16-frame tables: about 3-4 MB. No DSP change, but a preset with two different tables on two parts can refuse to load.
  2. **Decimated mips** (recommended; a fork patch in `tools/fork_patches.py`): mip `j` is band-limited to `1024 >> j` harmonics, so it needs only `2048 >> j` samples. A pyramid is then 4,095 samples per frame instead of 22,528: 32 frames = **0.52 MB float, 0.26 MB if stored int16**. The oscillator's inner loop indexes with a per-mip length mask instead of one `length_`. Not written or measured here.
  3. Frame length 1024 or 512 at export (zero DSP change, `build()` accepts any power of two): 1.3 MB / 0.6 MB per 32-frame table, but it loses content above harmonic 512 / 256. Not implemented in `wt_export` (needs a band-limited down-sampler).
* **Load time (unmeasured).** `Wavetable::build()` runs a radix-2 FFT with `cos`/`sin` computed in the butterfly: 32 frames x 12 FFTs of 2048 points. On the M7 without a twiddle table that is plausibly several seconds per table. Either patch the twiddles into a table (fork patch) or store the finished pyramid on the card (`mip policy` byte in the header is reserved for that). Measure with the DWT counter before choosing.
* Cost model: `descOf` builds slots as `Osc::PD`; a table slot would be `Osc::Wavetable` (`kOscWavetable` = -0.08 voice, `kOscPulse` -0.05), so the per-voice costs in this package (mean 1.20, max 1.71, all with PD slots) are slightly high for table presets. `CostBridge.cpp: descOf` should set `slot[s].kind` from WAVE.

## 3. Preset records, categories

* `app::Preset` holds 4 + 5 values. A converted preset sets all 72 knobs, so `content/data/Presets_generated.inc` defines `GenPreset { name; cat; voices; score; cost10; table[2]; tablePos[2]; int8_t pages[72]; }` (100 bytes + name). 170 presets = 41 KB of source, about 17 KB as const data in flash (91 KB free), or the same records as a file on the card (UI_CONCEPT section 7: "compact binary format, produced offline from the upstream JSON by a host tool"). Recommendation: the card; the `.inc` stays the host-test source. `applyPreset` then becomes a copy of 72 values plus the table binding, and `Part::voices = voices`.
* `kPresetCats` has 6 entries (ALL BASS LEAD PAD PLUCK STAB); `kGenCats` extends it without moving them: POLY KEYS BRASS FX SEQ TEXTURE BELL PERC (14 with ALL). POLY is new on the device (polyphonic chords, keys, organ-like); SEQ stands for upstream sequence patches (the device's ARP lane is the closest thing). `kPresetCatCount`, the BROWSE category cell (T2) and `Part::preset` (an index into `kPresets`, int8, -1 none: needs 170+ entries, so int16) change.
* The browse preview plays three notes in the project key; chord categories want the chord phrase (`phrases.h` has the ones used for the listening page).
* `ProjectFile` stores `Part::preset` and the pages; a table slot must be stored as WAVE >= 4 and survive `sanitize`, and a missing/short set must degrade to SIN, not crash.

## 4. What the conversion drops, in order of points lost over the 383-preset archive (`fit_report.md`)

| missing on the device | presets hit | where it would live |
|---|---|---|
| FX chain (reverb, delay, chorus, comp, drive) | 341 of 383 | a master FX send per part: not on the pages today |
| oscillator modes: PD types other than Saw, ALGO (Plaits), Bitmask, pulse width | 352 | PD type selector; the macro engines exist in the fork (`MacroOscillator`), `kOscMacro` costs -0.03 |
| sub as a non-sine, sub at 0 oct | 232 | SUB page has no spare knob |
| Velocity to cutoff (221 rows), to osc 2 level (101), to position (56) | 221+ | the converter folds them into the base at velocity 0.75; real velocity routing needs a VEL>CUT knob (the MOD page is full; GLOBAL has two unused cells, or a lock-style per-note route) |
| Env2 shape (attack, sustain) | 204 | fixed 1 ms attack, no sustain, decay tied to AMP DECAY |
| per-oscillator envelopes | 117 + 103 | |
| punch transient, glide | 80, 62 | |
| second and later LFO routes, step modulator, S&H, FM, fold, resonator | | one LFO lane on the MOD page |

## 5. Verified and not verified

Verified on the host with the fork build in double precision: the 170 records pass `app::sanitize` unchanged, the heavy-filter rule and cost x voices <= 5; all render finite and not silent through `PatchMap_wt` + `Instrument`
(`content/out/fit_report.md`, `python3 content/tools/test_content.py`: 14,630 checks). Not verified: anything on the board (load time, SDRAM, the float build), the decimated-mip oscillator, how any of it sounds.

## 6. Regenerate

`python3 content/tools/build_content.py` (about 70 s): fit analysis, candidate pool, render figures, final conversion, wavetable export, checks, renders (Drumboy and original), `content/out/listen.html`.
Build dirs live in the session scratch directory; the repository only receives `content/data` and `content/out`.
