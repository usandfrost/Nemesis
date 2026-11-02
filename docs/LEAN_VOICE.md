# The lean voice

The replacement for the CATHODE voice (`firmware/dsp`, generated from the upstream project) that plays the **same patches** (`audio::Patch`, the panel's nine pages, the `.PRS` files) with far fewer cycles per voice. Goal: 8–12 voices sounding at once on the Drumboy instead of 3–5, with a part still at 1–5 voices. The voice budget, not the voices of a part, is what is lifted.

Status: the firmware's default voice, measured on the board (below). The board has only this voice since it went to seven parts; the host tests and the web build keep the CATHODE engine for comparison.

## Why

A board profile of the committed firmware (PC sampling, audio ISR only, plain voice "Sub Pressure") showed ~3,700 cycles/sample per voice, of which only ~1,100 is the signal path proper; the rest is plumbing (modulation matrix, glue, oversamplers at 2×–4×, double-buffered stages). The budget is 12,472 cycles/sample at 44.1 kHz; the governor sheds at ~85 %.

Stop rule: the lean voice must measure ≤ 700 cycles/sample on the board for the presets it matches, or the work stops. At 700 the voice budget (~9,000 cycles after effects and system) is ~12 voices; at 900 ~10; at 1,100 ~8.

## Design

- Float only, **no oversampling**, and no libm in the voice's loops: the per-block exp2, log2, sin and tan are short polynomials (`fastExp2`, `fastLog2`, `fastSinCycles`, `fastTanPi` in `LeanDsp.h`, errors 1e-7 to 4e-6, checked by `tests/lean/fastmath_check.cpp`); the pitch ratios of the semitone offsets and of the unison members are made once per patch (`Params::ratio`, `uniRatio`). Anti-aliasing is done where a stage needs it: ADAA soft clip (first-order antiderivative, Parker et al., DAFx 2016), polyBLEP at the sync reset (the usual two-point residual), a 2× phase-distortion oscillator with a 19-tap Kaiser half-band decimator, two-mip crossfaded table reads.
- Voice-major block rendering in `kCtl = 16` sample control blocks: the modulation (LFO, Env2, cutoff, pitch, position, PD amount) is computed once per block and the filter coefficients are ramped across it. A voice's state stays in the cache while it renders its block.
- Filters: Simper's TPT state-variable filter for LP/BP/HP/NOTCH (and SOVIET); a closed-form n-pole TPT ladder (Zavalishin) for LADDER, ACID (3 poles, saturation in the feedback) and CLASSIC (4 stages with per-stage soft clip).
- Per-voice chain after the oscillators (run twice for a width/stereo voice): HPF → converter (bits, mu-law, sample-and-hold rate, drift) → noise → filter 1 (→ filter 2, series or parallel) → amp envelope × velocity → output clip → level → pan.
- The laws (levels, envelopes, filter mappings, unison, pan) are those of the old voice, measured on it: see `LEAN_SPEC.md`-style notes in the session scratch; they are not re-derived from any other synth.

## Files

| file | what |
|---|---|
| `firmware/src/lean/LeanDsp.h` | primitives: table reads, SVF, ladder, soft clips, PD oscillator, slow noise |
| `firmware/src/lean/LeanPart.h/.cpp` | `lean::Params` (a Patch in physical units, `makeParams`) and `lean::Part` (the voices) |
| `firmware/src/audio/Instrument.*` | hooks behind `#if DBOY_LEAN` (note start/end, patch changes, rendering, voice count) |
| `firmware/CMakeLists.txt` | `DBOY_LEAN` (build it in), `DBOY_LEAN_DEFAULT` (parts start on it) |
| `tests/lean/` | `lean_bench` (times a preset), `ref_render` (renders a preset through the old voice or the lean one, for grading) |
| `web/` | the web bench: the real firmware code in WebAssembly with both engines, a preset page with approve/reject verdicts (`web/feedback/verdicts.json`) and a virtual Drumboy |

## Grading against the old voice

A waveform match is the wrong test across different algorithms, so the grade is phase-independent: level (dB), a 256-sample envelope error, and third-octave band powers smoothed over 6 frames, per preset and note (40 and 64). A: level ≤ 1 dB, envelope worst ≤ 2.5 dB, spectrum mean ≤ 1.5 dB, p95 ≤ 4 dB. B: 2.5 / 5 / 3 / 8. Otherwise C. This is a host measure; **sound is judged by ear in the web bench.**

Status of the library (200 presets × 2 notes = 400 renders): A 308, B 59, C 33 (2026-10-30).

## Known gaps

- Converter "loop error" is not implemented (two presets).
- Noise-driven presets cannot match sample for sample (random sequences differ); drift is random per voice, as in the old voice but with different seeds.
- SOVIET's asymmetric clip and CLASSIC's large-signal behaviour are approximations; a few presets stay C.
- The effects are still the old ones, at half rate: the reverb and the amp are the largest single loads.

## Licence and provenance

Written for this project from the old voice's own behaviour and public techniques. No code was taken from other synthesisers. (The author of this code had read the source of a GPL-3.0 synth, OberVave, earlier in the project; this is therefore not a clean-room implementation, and its ideas — a leaner voice-major design — are credited as inspiration only.)

## Measured on the board

`DBOY_LEAN` and `DBOY_LEAN_DEFAULT` are on by default: the firmware boots on the lean voice. `-DDBOY_LEAN_DEFAULT=OFF` no longer fits the board (see Seven parts: CMake stops it); it boots on CATHODE with CATHODE's cost table (the debug event below still switches), and `-DDBOY_LEAN=OFF` leaves the lean voice out; both keep the lean voice's ITCM list, so they are comparison builds, not releases.

**The stop rule passed.** Over the whole library (200 presets, median rise per added note): a voice is 549 cycles/sample, 140 of 200 at or under 700, the 10th to 90th percentile 312 to 923.

| | old engine | lean voice |
|---|---|---|
| default project on PLAY, average load (governor off) | 100% | 15.8% |
| the same, worst block | 243% | 40% |
| the same, overruns in 8 s | 6,821 | 0 |
| first note of a preset, median | 53% | 6.5% |

Polyphony: 15 voices held of a light preset (effects 7%) 65% average, worst block 66%; with the usual reverb (effects 15%) 14 voices reach the 85% the voice budget is drawn to, and all 15 play at 89%, worst block 92%, with no overruns. With the sequencer, two parts at 5 voices and the drums, governor on: a light pair 61% average, worst block 86%, degrade 0; a typical pair 65%, worst block 94%, degrade 1; a pair with amps 48%, worst block 84%, degrade 5, none of these overrunning. An expensive pair (SOVIET with drive and unison x4) reaches degrade 5 and loses its effects, with 0 to 3 overruns in 12 s from run to run, worst block up to 110%. Waking the amp and effects on the first note costs nothing that shows: the worst block of a three-note chord is within a point of the same chord with them already running. Loading a preset takes one block of about 300%, which can click. The old engine's per-voice figures cannot be compared: it sounded only 1 or 2 of the 3 or 4 notes held, for a reason not found.

**Where the cycles were.** Moving the code into the ITCM saved about 1%: the I-cache already held the loops. The gains were all in data:

- The voices' state moved out of SDRAM into DTCM (1.5 KB each, the pool in `LeanPart.cpp`): 6-7% at 4 voices, because the voices stop fighting the wavetables and the effects' lines for the D-cache.
- The reverb's eight lines were written at the same index, a power of two apart, so all eight fell in one set of the 4-way D-cache and evicted each other on every sample. Fork patch B12 spaces them: 22% to 15% of the block on a typical preset, and the output is bit-identical.
- The sub is a pure sine, so it is now a rotation restarted from the phase every control block, not a table read: 201 cycles to about 32 (under -100 dB of error up to 2 kHz).
- A table position sitting on a frame (a basic SIN oscillator) reads that one frame, not two blended to nothing.

- The amp runs at 1x instead of 2x: half its cost, 16-17% of a block down to about 8. Aliasing against 2x, measured without a cabinet: 3-8 dB more on notes up to 1 kHz, 13-19 dB more at 4 kHz. Its soft clips are ADAA, so they already band-limit themselves. This is the one change here that alters the sound.
- An amp rests after 500 ms with nothing coming in, and an effect sleeps once nothing comes in and its tail has gone, whatever its send. A preset with an amp and sends, no notes playing: effects 17.5% down to 1.5%, the whole board 21.5% to 6.1%. This helps whenever a part rests, not the worst case.

The largest single loads left are the reverb (about 11% in the fit, 15% on a typical preset) and an amp (about 8%).

**The cost table** (`cost/LeanCostTable.h`, `DBOY_COST_LEAN`, on with `DBOY_LEAN_DEFAULT`) is that fit: every preset's rise per note, solved for what each option adds to a plain voice of 249 cycles, under the same conditions as `renderVoice` (a stereo voice pays for its chain twice, a live ring or sync collapses the stack, PD runs per stack member). The second oscillator (+259) and a unison member (+137) are the big ones; the filter models differ by at most 162, so the lean voice has no heavy filters and the one-heavy-filter rule is off. On the ladder the width goes before the unison, CompandOff is the whole converter (`converter off`), and XmodOff takes the sync and leaves the ring, which costs nothing. The budget is drawn from a 15-voice run, because voices cost about 1.20 times their sum when they sound together. Re-run it with:

    tools/board_bench.py /tmp/all.json --lean on --voices 4 --hold 1.6       # about two hours
    tools/fit_lean_cost.py /tmp/all.json --table firmware/src/cost/LeanCostTable.h

The ITCM list is `tools/board_profile.py` twice (notes held over the library, and `--play` for the drums and the sequencer) into `tools/gen_itcm.py --samples A --samples B`, then a rebuild.

`tools/board_script.py` has `lean on|off` (debug event 12) to change engine without a reflash, and `tools/board_bench.py --lean on|off` takes it. Since the board dropped the CATHODE engine `lean off` does nothing there, so these apply to a build that has both. The switch itself is one block of about 900%: measure a worst block on a build that boots on the engine in question.

## Voices beyond 5

Decided against (owner, 2026-10-30): a part keeps 1–5 voices. The panel, the preset format (`kPresetVoicesMax`) and `PatchMap`'s per-voice caches stay as they are; `lean::kMaxVoices` is 5 to match (`Instrument` already clamps a part to `CathodePart::kVoices`). The cost model now charges the lean voice's own measured prices (above).

## Seven parts

A project has seven parts, A to G: six synths and the sampler that plays the drums (C, where it has always been, so an older project plays as it did). Any part can be either. A new project's lanes use A, B and C as before; PLAYED BY moves a lane to another part.

The voice budget is what limits them, not the part count: seven parts × 5 voices is 35 voices, far past what the CPU plays. The budget and the degrade ladder count only the parts a lane plays (`Project::partsIdle`), so a part nobody uses cannot make a project look over budget.

The six synths need what three did not, so:

- **Lean voice only on the board.** Seven CATHODE engines would not fit the SDRAM, so a build with `DBOY_LEAN_DEFAULT` leaves that engine out (`DBOY_OLD_ENGINE=0`). Flash went from 856 KB to 439 KB, and the board's `lean off` does nothing. The web build and the host tests keep the old engine for comparison.
- **Six wavetables in memory**, up from three. 41% of the presets use one, and a table is about 1 MB. They come out of the SDRAM the old engines left free.
- **Voices in two places.** A part's first three voices are in DTCM; the fourth and fifth in internal RAM, which shares the D-cache. A part takes its lowest free voice first, so this only touches a part playing four or five notes.
- **Silent parts cost little.** The effects section skips a part with nothing in its buffers, and with no signal to protect a send lands at once.
- **MIXER has four views:** LEVELS, VOICES, PRIORITY, CPU. In the first three each knob is one part, A on T1 to G on B3.

Files from before version 6 load with their three parts and effects as they were, and the new parts free.

Measured on the board with six synths on six lanes and the drums, two voices each, playing the default pattern for 12 s with the governor on:

| Six presets | Average | Worst block | Overruns | What the governor gave up |
|---|---:|---:|---:|---|
| The same light preset | 71% | 97% | 0 | nothing |
| A typical mix | 64% | 91% | 0 | 3 steps: a voice on G, the reverb, the delay |
| Three amp presets, twice | 54% | 96% | 0 | 15 steps |
| The dearest preset | 62% | 91% | 0 | 13 steps |

The typical mix asks for 42 of the 33 units the budget has; with the governor off it averages 82% and overruns (27 in 12 s, worst block 109%). Idle went from 10% to 4% of a block when a part's description and patch stopped being worked out on every block. A rebuild of the ladder takes more time than that, but 500 of them (a knob turned under this load) gave no overrun, worst block 99%.

A part with no lanes that is played live joins the budget while it sounds and for ten seconds after, so the ladder does not change with each note.
