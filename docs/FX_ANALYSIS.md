# FX analysis: DELAY, REVERB, AMP, CHORUS, COMPRESSOR (2026-10-26)

Analysis only: nothing under `firmware/`, `tests/`, `tools/`, `content/` was changed. New: this file and `fx_bench/` (scripts, benchmarks, `fx_bench/results/*.txt`).
Tags: **[M]** measured here, **[C]** read from code, **[E]** estimated. I cannot listen: nothing below says how anything sounds.
The tree already has the parameter side (`app/FxParams.h`, `Project::fx`, `ProjectFile` v4, `Mode::Fx`); section 6 checks it against the engines. The audio side does not exist yet.

## 1. How it was measured

| Item | Value |
|---|---|
| Unit | **voice equivalent (ve)** = one plain CATHODE voice (PD osc + low-pass), firmware switches, same build: 150 ns/sample float, 143 ns double [M] (the unit of `cost::voiceCost`) |
| Method | ns per stereo sample over 32-sample blocks on an 8 s drum-like loop, best of 3, run-to-run +-3% [M]. Float build (g++-16, firmware flags) shown; the double build (clang) agrees within ~30% (`results/bench_*.txt`) |
| Sources | upstream FX converted `double`->`Real` with `sync_dsp.convert_text` (`make_tree.py`); stock loops transcribed from `Controller.cpp` into `stock_fx.h` (mono: stereo = 2 instances); "sketch" = small new code in `proto_fx.h`, **cost only, not reviewed for sound**. Host libm is faster than newlib, so libm-heavy code looks cheaper than it will on the M7 |
| Board cycles | **[E]** 1 ve = 1,746 cycles/sample, the repo's assumption (budget 5.0 = 70% of 12,472). Nothing is measured on the M7. Cross-check, static M7 instruction counts per sample (`results/m7_instruction_counts.txt`): stereo delay 115, Freeverb 114, stereo chorus 201, linked compressor 118 |

## 2. Candidates per effect

State = bytes allocated by `prepare()` [M]. Flash = growth of an M7 link (`--gc-sections`) over the engine-only link [M]. In all upstream classes `process()` allocates nothing [M] and has no per-sample virtual call or atomic (per block only); `std::vector` only in `prepare` [C]. Licence: stock = MIT (`firmware/LICENSE.stock-mit`, keep the notice); upstream = your own code.

### DELAY
| | upstream `DelayEffect` | stock `Delay` | sketch |
|---|---|---|---|
| What | 6 types. Digital = 2x `DelayCore`: cubic read, 20 ms crossfade on a time change, loop = DC block + HP + LP + ADAA clip, feedback cap 0.95; ping-pong; MOD = varispeed; ms only (no tempo input) [C] | mono, integer delay (no interpolation), one-pole damping; tempo = 6 fixed fractions; a time change hides behind a ~0.5 s fade-out/in [C] | stereo, glided time, linear read |
| Quality [M] | TIME 300->450 ms: no step (jump ratio 0.99); with MOD it is a pitch glide (3.6); FEEDBACK/TONE moves smooth (1.05/1.10); **MIX unsmoothed (15x step)**; MIX 0 = bit-exact, 0 latency. Finite at max feedback (peak 2.6 for +6 dBFS in). Tail to -90 dBFS: 3.4 s (fb 40%), 9.4 s (80%), 21 s (95%) | | |
| Cost | Digital **0.15**, ping-pong 0.14, tape 0.34 | 0.03 | 0.04 |
| State / flash | **9.5 MB** (all six types are allocated, 4 s rings; 18.3 MB double) / +23.9 KB; `DelayCore` x2 for 1.5-2 s alone: **1.0 MB** (power-of-two rings, 2.0 s = 1.5 s) / +2.6 KB | 0.77 MB (2 x 96,000 floats); flash: the 7 stock effects together +5.3 KB | 1.0 MB |

**Use upstream `DelayCore` x2 (546 lines; `AdaaSoftClip` is already in the fork) inside our own ~150-line stereo wrapper** (ping-pong, spread, tempo ms from `delayMs()`, own mix/return smoothing). Not `DelayEffect`: 9.5 MB and five unused types. Ring must cover 1.5 s (1/4 at 40 BPM) plus margin.

### REVERB
| | upstream `ReverbEffect` | stock Freeverb | 
|---|---|---|
| What [C] | Room (4-line FDN + early taps), **Hall (8-line Hadamard FDN, 4 allpass diffusers, per-line damping + DC block, coupled-form LFO per line: no sin per sample)**, Plate (Dattorro), Spring, Shimmer, Cathedral (16 lines). Size/decay/pre-delay smoothed over 30 ms in the class; mono in, stereo out | 8 combs + 4 allpass (original Freeverb lengths), one-pole damping, no modulation, mono in, pseudo-stereo = 0-20 ms delay on R; on/off = 0.5 s fade, buffers freeze (a stale tail returns) [C] |
| Cost [M] | Room 0.17, **Hall 0.50**, **Plate 0.29** (0.27 without modulation), Cathedral 1.19, Shimmer 0.58, **Spring 4.7** | **0.09** |
| State [M] | Room 164 KB, Hall 368 KB, Plate 192 KB, Cathedral 1.15 MB, Spring 57 KB; the class allocates all: 2.0 MB | 72 KB |
| Flash [M] | class +20.5 KB; `ReverbCore` alone +5.0 KB; `PlateReverbCore` +5.1 KB | (in the +5.3 KB above) |
| Behaviour [M] | DECAY moves smooth (1.0-1.3); **SIZE move = 20x step** (line lengths jump); PRE-DELAY 2.1; MIX unsmoothed. Decay 2.4 s: -60 dBFS at 2.0 s (Hall) / 2.3 s (Plate), -90 dBFS at 3.1 / 3.7 s; max settings 7.0 s (Hall 6 s). Peak 6.2 for +6 dBFS noise at max Plate settings: the return needs a trim | -60 dBFS at 0.7 s (default), 7.5 s (max) |

**Use `ReverbCore` (8 lines, 940 lines with `Allpass`) directly**, not `ReverbEffect`: the effect's per-type bands (Hall 1-6 s, pre-delay 80 ms) are its own; the core takes any RT60, so the page's DECAY 0.2-12 s, PRE-DLY 0-200 ms, SIZE fit. Our wrapper adds the 30 ms smoothing, WIDTH (M/S) and LOW CUT (`setInputHpHz`). `PlateReverbCore` (cheaper, fixed size, decay <= 3 s) is the alternative voicing and the first reverb degrade step.

### AMP
| | upstream `GampEffect` | stock Overdrive / Distortion | sketch |
|---|---|---|---|
| What [C] | 6 models: 1-4 preamp stages (ADAA clip + filters) inside **one oversampler per model (x2/x4/x4/x8/x4/none)**, passive tone stack, power amp (clip, rail sag, crossover, transformer), presence, algorithmic cabinet (6 cabs x 3 mics, no IR). Float core; stereo (2 channels); 10 ms glides on every gain; aligned dry line | HP -> cubic clip (or atan table) -> biquad; mono; **no oversampling**, no tone stack, power amp or cabinet | mono-sum, crude 2x |
| Cost, stereo [M] | Clean 1.5, Crunch 3.4, Lead 4.2, Bass 4.1, **Hi-gain 8.9**, Acoustic 0.3. Oversample forced x2: Crunch 1.9, Hi-gain 2.8; x1: Crunch 0.9. **Cab "Direct" saves only ~3%**. While a tone knob glides: +23% | 0.06 / 0.10 | 0.10 |
| Cost, **mono-sum** [M, scratch-patched copy, right channel skipped] | Clean 0.78, Crunch 1.70, Lead 2.08, Bass 2.04, Hi-gain 4.4; **forced x2: Crunch 0.93, Hi-gain 1.36; x1: Crunch 0.51** | | |
| Behaviour [M] | 16 samples (0.36 ms) latency when oversampled; GAIN/BASS/PRESENCE/CAB moves glide, no step (1.0-2.6; the 2.6 is GAIN adding harmonics); **MODEL switch = 7x step**, MIX unsmoothed; finite at max gain (peak 5.3 at Hi-gain +12 dB level) | | |
| State / flash | 14 KB / +25.3 KB; 2,131 lines (+`EnvelopeFollower`, `OnePole`) | 0 | |

**Use `GampEffect`, but mono-sum and forced x2 (x1 since 2026-10-30: docs/LEAN_VOICE.md), models CLEAN/CRUNCH/LEAD/BASS only** (the page already has exactly these four; Hi-gain's x8 is the cost trap). The mono flag is a ~10-line patch in `tools/fork_patches.py` (I patched a scratch copy to measure it). Per active amp: **0.8-1.2 ve** (Lead and Bass at x2 are ~1.1 [E], scaled from Crunch), 14 KB. Per-sample libm runs while a tone knob glides (`resolveNetwork`: tan, sqrt, acos, cos) [C]; on the M7 that is the likely spike, so update those coefficients every 16 samples in the port. "No cabinet" is not a useful degrade step (~3%).

### CHORUS
| | upstream `ChorusEffect` | stock Chorus | sketch |
|---|---|---|---|
| What [C] | BBD lines (companding with sqrt+div per sample, linear read, integer `%`); Chorus/Flanger/Ensemble/Vibrato, 1-8 voices. **Base delay fixed per type** (no DELAY knob) | 2 voices (x0.75), triangle LFO, linear read, `%` wrap, mono; edits recompute the read pointer (jump) | 2 voices/channel, quadrature LFOs, cubic read |
| Cost [M] | 3 voices **0.41** (colour 0: 0.24), 2 voices 0.27, 1 voice 0.16, ensemble 8 x 2 banks 1.47 | 0.07 (2 instances, no L/R offset) | **0.06** |
| State / flash | 345 KB (all 32 lines always) / +5.8 KB | 40 KB | 16 KB |
| Behaviour [M] | RATE/DEPTH/VOICES moves smooth (1.0-1.3); MIX unsmoothed | | |

**Write the small stereo chorus** (~100 lines, from the sketch): 6x cheaper, 20x less memory, and it has the DELAY knob the page wants. Placement: **shared send effect** (as `FxParams.h` already has it): one instance, 0.06 ve. As a per-part insert upstream costs 1.2 ve, the sketch 0.18.

### COMPRESSOR
| | upstream `CompEffect` | stock Compressor | sketch |
|---|---|---|---|
| What [C] | VCA/FET/Opto/Bus; soft knee, program-dependent release, sidechain HP, **ADAA saturation**, parallel mix; stereo-linked; one `log2` + one `exp2` per sample; no look-ahead | mono peak detector, hard knee, dB-domain smoothing, no makeup/knee/sidechain; two instances = **unlinked** stereo | linked, soft knee |
| Cost [M] | VCA/FET/Bus **0.34-0.37**, Opto 0.52; **saturation off: 0.125** | 0.07 | 0.09 |
| Behaviour [M] | THRESH/RATIO moves smooth; **MAKEUP 0->12 dB = 7x step** (unsmoothed); MIX 6x step | | |
| State / flash | 352 B / +6.7 KB; 1,442 lines | 56 B | 40 B |

**Use `CompEffect` (Bus) on the master**, saturation off by default (it is 65% of the cost), `std::log2/exp2` -> `fm::` (on the host `fm::` is slower than Apple's libm, so that saving is only [E]), makeup/mix smoothed in the wrapper. One instance only; per-part compressors would need the sketch (0.09 each). "No look-ahead" is moot: neither source has one.

### Cheap extras the stock code already has
| | stock (stereo = 2x) | upstream |
|---|---|---|
| Flanger | 0.03 | `ChorusEffect` Flanger 0.17 |
| Phaser | 0.14 (2 biquad allpass, table sin, coefficients per sample) | 0.33 |
| Overdrive / Distortion | 0.06 / 0.10, no oversampling | 0.80 / 0.86 (x4 oversampled), Fuzz 1.38 |
| Bitcrusher | 0.03 | BitCrush 0.065 |
| Expander | like the compressor, ~0.07 [E] | not benchmarked |

Stock Bitcrusher fold loop `while (x > lim || x < -lim)` **never ends for |x| beyond about 1e7** (tested: 1e9, 1e30, inf) [M] and below that takes |x|/(2*lim) iterations [C]: one Inf upstream would lock the audio interrupt. Needs a bound if ported.

## 3. Cost and memory of the recommendation

| Block | ve [M] | Board cycles/sample [E] | Memory | Flash [M] |
|---|---|---|---|---|
| Delay (`DelayCore` x2) | 0.14 | 245 | 1.0 MB | +2.6 KB |
| Reverb (8-line `ReverbCore`; Plate 0.29) | 0.50 | 870 | 368 KB (Plate 192 KB) | +5.0 KB |
| Chorus (sketch) | 0.06 | 105 | 16 KB | ~1 KB |
| Master comp (sat. off; on 0.36) | 0.13 | 220 | 0.4 KB | +6.7 KB |
| Sends/returns/mix [E] | ~0.01 | 20 | 1 KB | - |
| **Shared total** | **~0.84** | ~1,460 (12% of a block) | ~1.4 MB | ~16 KB |
| Amp per part (mono, x2) | 0.8-1.2 each | 1,400-2,100 | 14 KB | +25.3 KB once |

Everything: ~1.5 MB SDRAM (the sample cache is ~6 MB after instrument, audition buffers and reserve **[E]**: it loses ~25%) and ~41 KB of the 90-110 KB free flash. This assumes the **D-cache on** (`DBOY_DCACHE`, off today): the FDN touches ~30 words per sample, and at "tens of cycles" per uncached access that alone is ~0.4 ve [E]. Free internal RAM: ~110 KB AXI + ~75 KB DTCM (ELF section sizes) for hot small state.

**Cost line for `CostBridge`**: `fxCost = sum(active shared effects) + sum(per-part amp)` in ve, taken from a generated table (extend `tools/gen_cost_table.py` with `fx_bench` output); the governor still reads real cycles, so a wrong table self-corrects.

## 4. Degrade steps (cheapest loss first)

| Effect | Steps, with the saving [M] |
|---|---|
| AMP (per part, belongs to the part's ladder) | oversample x4 -> x2 (-45%), x2 -> x1 (-51%; aliasing), model -> CLEAN, then amp bypass with a fade. Not "cab off" (~-3%) |
| REVERB | modulation off (-7%), then Plate (-42%; needs a short crossfade, both engines resident: 560 KB), then 4-line Room (0.17, -66%) |
| CHORUS | one voice per side, then off |
| COMP | saturation off (-65%), sidechain HP off, then bypass |
| DELAY | MOD off, cubic -> linear read (~-25% of 0.14 [E]); last: bypass. Not worth a step before the others |

Proposal: shared-FX steps join `cost::Step` through a 4th pseudo-part (`kMaxParts` is already 4), ordered after the low-priority parts' costly options and before the last-resort voice steals; amp steps belong to their part.

## 5. Architecture

```
CATHODE/sampler part p -> p_buf[p] (L,R x32) -> [AMP p: mono-sum, x2, own fade] -> x part gain
   -> dry bus
   -> x send[p][DELAY|REVERB|CHORUS] (smoothed) -> delay, reverb, chorus (100% wet) -> x RETURN
 dry + returns -> COMP (before MASTER) -> x MASTER -> existing softLimit -> meters
 audition stays on the dry bus.
```

- **Hook**: `Instrument::renderCathode` and the sampler loop write to per-part buffers (768 B); a new `mixFx(frames)` after the samplers does amp, sends, returns, comp, then the existing master code. Event-offset splitting is unchanged.
- **Parameters**: applied in `syncParts()` (control rate, like `base_`): compare `pj_->fx` with an applied copy, call setters. Sends, returns, makeup, mix, bypass are smoothed in our wrappers (the classes do not).
- **Block/latency**: 32-sample blocks (upstream control chunk 16 divides it); latency 0 except 16 samples on an oversampled amp (0.36 ms; add a 16-sample dry delay on the other parts if phase matters).
- **Tails**: effects keep running after a send drops; an effect sleeps (no processing, lines cleared) when its sends are 0 and its output has been < -100 dBFS for N blocks. Tails to -90 dBFS [M]: reverb 3.7 s at DECAY 2.4 s and 7 s at 6 s (so ~14 s at 12 s [E]), delay 21 s at max feedback: use an energy test, not a timer. STOP lets tails ring; project load clears them.
- **Tempo**: `delayMs(mode, time, bpm)` exists; evaluate per block; `DelayCore` crossfades the change (no click, [M]); with MOD > 0 a change becomes a pitch glide.
- **Fork**: add `DelayCore.h ReverbCore.h GampEffect.h CompEffect.h` to `ROOTS` in `tools/sync_dsp.py`. Checked on a scratch copy: all patches apply, existing fork files stay byte-identical, the new files come out converted, CMake's glob finds them. The mono Gamp patch goes in `tools/fork_patches.py`.
- **Project file**: done (v4, tail-append, defaults on older files, every send 0 so old projects sound as before).

## 6. The existing FX parameter pages against the engines

| Page | Fits | Gap |
|---|---|---|
| DELAY MODE/TIME/FEEDBACK/TONE/SPREAD/MOD/HPF/RETURN | `DelayCore`: feedback <= 95%, TONE = loop LP, HPF = loop HP, SPREAD = R time x0.75 at full | notes at 40 BPM reach 1.5 s, above TIME's 1 s: ring >= 1.5 s |
| REVERB SIZE/DECAY/DAMP/PRE-DLY/WIDTH/MOD/LOW CUT/RETURN | `ReverbCore` setters; size/decay/pre need our 30 ms smoothing | classes have no WIDTH; Plate has no SIZE and caps at 3 s |
| CHORUS RATE/DEPTH/DELAY/FEEDBACK/WIDTH/TONE/HPF/RETURN | the sketch | upstream has no DELAY knob |
| COMP ACTIVE/THRESH/RATIO/ATTACK/RELEASE/MAKEUP/MIX/SC HPF | all are `CompEffect` rows (attack min 0.05 ms) | ACTIVE needs a fade; MAKEUP/MIX need smoothing |
| AMP MODEL/DRIVE/BASS/MID/TREBLE/PRESENCE/CAB/LEVEL | `setModel` 0,1,2,4; DRIVE -> `setGainDb` 0-36; tone = percent with 50 = noon | tone dB range is per model (10-14 dB in the table), not a fixed +-12; no MIC knob (use Dynamic) |

### p-locks (key = page*8+param; FX pages would be ids 12-17, keys < 144)
| Lock freely [M: no step] | Risky |
|---|---|
| send levels (our smoothing), delay FEEDBACK/TONE, reverb DECAY/DAMP, chorus RATE/DEPTH, amp BASS/MID/TREBLE/PRESENCE/CAB, comp THRESH/RATIO/ATTACK/RELEASE | delay TIME (pitch glide if MOD > 0, tempo modes follow BPM anyway), reverb SIZE (20x step) and PRE-DLY (2x), amp MODEL (7x step), comp MAKEUP (7x unless smoothed), ACTIVE/MODEL OFF (needs a fade) |

**Implemented (2026-10-27): the three sends and the amp's knobs, per step.** A global-FX lock has no voice to live on: it is a **step-scoped override** (applied at the step's trigger with a ~5 ms glide, reverted at that lane's next step; last writer wins between lanes). A send lock must outlast the note's release or the tail is cut: revert after the part's amp release (`syncParts` already knows it).

What was built: keys `PG_FXSEND` (page 14: DELAY, REVERB, CHORUS send of the lane's part) and `PG_FXAMP` (page 15: the amp view's knobs, **not MODEL (0) and not CAB (6)**; an amp that is off has nothing to lock). Hold a step to open its lock, press the FX key (SENDS and AMP are the views a lock can use; PART is fixed to the lane's part), turn a knob; the step keeps the value, the knob shows LOCK. The sequencer sends the step's effects locks apart from its notes (`NoteSink::fxSet` at the step's time, `fxRelease` one step later, `fxReleaseAll` on STOP) with a token per set, so the end of one step cannot let go of the next; each lane keeps one set and the lane that set last wins where two lanes of one part lock the same knob. The instrument lays the locks over the project's effects settings for the block (the section already smooths the sends and the amp knobs glide), so the part's own values are back when the step ends; a reverb or delay tail the send fed goes on ringing. A lock is the step's alone: no hold across steps. The lock takes hold and lets go at block boundaries (32 samples: up to 0.7 ms late, set and release separately), hidden by the send smoothing. The shared effects' own settings (delay time and feedback, reverb size and decay, chorus rate, the returns, the compressor) are **not** lockable: all parts share them, and several jump (time, size) when moved. **Cost:** a lock that raises a send for an effect nobody else sends to wakes it for the step; the cost model counts effects by the project's own sends, so that CPU is not in the budget (the UI says so when the lock is made). The governor still sheds by measured load.

## 7. Work breakdown

1. **Fork** (0.5 d): ROOTS, mono Gamp patch, `check_fork.sh` (engine byte-identical), build, flash check.
2. **`firmware/src/fx/`** (3-4 d): wrappers for delay, reverb, chorus, comp, amp; no heap after init; host tests (template: `fx_quality.cpp`): finite, tail times, bypass transparency, **no step on any parameter move (jump ratio < 3)**, level.
3. **Instrument integration** (1-2 d): per-part buffers, sends, idle sleep. Regression test: sends 0, amps OFF, comp OFF renders **bit-identical** to today's `render_song`.
4. **Cost and degrade** (1-2 d): FX rows in `CostTable`, `PartDesc` FX fields, new `Action`s, `degrade_check`-style test against the engine, governor test.
5. **p-locks** for FX pages: sends and amp knobs done (see above); the shared settings are left out on purpose.
6. **Board** (the only place these can be settled): D-cache on, DWT cycles per effect, calibrate the table, DTCM placement, SDRAM budget.

## 8. Risks only the board settles

- **Uncached SDRAM**: FDN and delay lines make 10-30 accesses per sample; the D-cache must be on first.
- **M7 libm**: Gamp's glide path and the compressor's log2/exp2 are libm calls; the host cannot show their cost.
- **The unit**: 1 ve = 1,746 cycles is an assumption. If a real voice costs more, the 5-voice budget shrinks, and memory-bound FX need not scale with it.
- **Listening decisions**: amp aliasing at x2/x1, unison width lost in a mono-sum amp, reverb voicing.
- **Headroom**: shared FX ~12% of a block, each amp 11-17%; the sample cache shrinks ~25%; flash use is ~40-50% of what is left.

Re-run: `make_tree.py`, `cmake -S fx_bench` (float: g++-16 with `-DFX_FLOAT=ON`), `fx_bench`, `fx_quality`, `flash_size.sh`, `arm_count.sh`; `mono_gamp_patch.py` / `fm_patch.py` patch only the scratch copy.
