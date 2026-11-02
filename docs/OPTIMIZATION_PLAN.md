# Drumboy Pro / CATHODE: optimization plan

Target: CATHODE synth (from upstream `dsp/`) running on the Drumboy Pro (STM32H723, Cortex-M7 @ 550 MHz,
FPv5 double-precision FPU, 128 KB DTCM, 64 KB ITCM, 320 KB AXI SRAM, 16 MB SDRAM, SGTL5000 codec).

Goal: 5 voices, no audio dropouts, display and controls responsive, with a CPU meter the player can trust.

## Ground rules

- **Measure before and after every step.** From Phase 1 on, each change records cycles per sample (DWT counter)
  for the benchmark patches. A change with no number does not merge.
- **Budget:** worst-case audio render <= 70% of the sample period. At 48 kHz that is 14.5 us per sample; at 32 kHz
  it is 21.9 us per sample.
- **Known unknowns (do not treat as facts until measured on the board):** the M7 slowdown vs the Apple core
  (guessed 15-40x), the cost of the stereo width > 0 path (location not yet found), SDRAM latency impact.
- **Goldens:** upstream tests compare against stored output. Anything that changes bits (single precision, fast math,
  reordered sums) is a deliberate step with regenerated or toleranced goldens, never a silent side effect.
  Bit-identical steps are marked below.
- Do not touch the upstream repo. The Drumboy firmware vendors or links the DSP read-only; optimized variants live here.

## Findings since this plan was written

- **Codec rate is 44.1 kHz, not 48 kHz** (stock firmware, `AUDIO_SAMPLE_RATE 44100`, 32-sample blocks = 0.73 ms).
  Budget per sample at 550 MHz is 12,472 cycles (22.7 us); 70% = 8,730 cycles. The clean upsampling ratio from
  44.1 kHz is 2:3 (engine at 29.4 kHz), or 1:2 (22.05 kHz). Read "32 kHz" below as 29.4 kHz.
- **Memory is the bigger problem than first thought.** `CathodeVoiceEngine::prepare()` makes 1,120 heap
  allocations totalling 1.71 MB (measured on the host, `bench/heap_probe.cpp`), plus 119 KB of object state.
  Internal RAM left after .bss is about 180 KB. Breakdown: 1.16 MB in 48 blocks of 2-32 KB (Plaits scratch) and
  0.55 MB in ~1,070 small blocks (tables, filter and oversampler state). It prepares all 16 voices x 2 oscillator
  slots even when 5 are used, and most of the tables look like identical per-voice copies. Current stop-gap:
  `firmware/src/memory/SdramHeap` puts the heap in SDRAM. With the D-cache off that will be slow.
  New work item **3.0**: prepare only the active voices and share read-only tables between voices (needs a fork of
  the DSP in this repo, not edits in upstream), then enable MPU + D-cache with SDRAM cacheable and DMA buffers not.
- Plaits keeps RNG state in a `thread_local`; the firmware builds with `-Dthread_local=` (single audio context).

## Status: DSP review implemented (docs/DSP_REVIEW.md), host-verified, not run on the board

- Bugs fixed: the float port rounded the PD/Bitmask phase to float every step (-3 cents at 55 Hz; this was the "unexplained slow
  divergence" below: float vs double went from ~45 to ~70 dB SNR); renderer patch setters and steal fade; ten cost-model findings.
- `NEMESIS_FORK_EXACT` (bit-identical, proven by `tools/check_fork.sh`), `NEMESIS_FORK_APPROX` (~1e-8), 8-voice pool, shared FIR tables,
  ITCM/DTCM placement (`tools/gen_itcm.py`, two-pass build). Host: -42% over 86 benchmark cases, plain voice 279 -> 151 ns;
  heap 5.96 -> 1.36 MB.
- Still open from this plan: D-cache on the board (P1), voice-major rendering (P5), everything that needs DWT numbers.

## Status: CPU savings that remove no feature (`docs/CATHODE_COST.md` section 3b)

- Idle mod matrix and silent-slot skip: exact (bit-identical, proven with an engagement counter); filter cores at x2: ~1 dB alias floor; PD at x2: opt-in
  (5 dB). Together on the host: -14% plain voice, -11% Classic voice (float GCC), up to -27% / -36% with PD at x2.
- Modulation sources still tick while idle (documented behaviour); skipping them would save ~5% more but is not free.

## Status: MIDI in (firmware/src/midi/), done, host-tested, not run on the board

- `MidiParser` (running status, real-time bytes anywhere, sysex skipped, note-on vel 0 = off, channel filter), `SpscQueue` (UART
  interrupt produces, audio interrupt consumes, no lock), `Router` (sustain pedal CC64, CC1 mod wheel, CC123/120 all notes off,
  CC121 reset, pitch bend, channel pressure). `MidiIn.cpp` is the only file that touches the HAL: USART1 half-duplex, one byte per
  interrupt, same pin/mode as the stock firmware.
- The audio interrupt drains the queue in `CathodeRenderer::beginBlock()`, so every engine call is in the audio context.
- `tests/midi/test_midi.cpp` (2,068 checks incl. a 2-million-event two-thread ring test); three planted bugs (running status broken,
  real-time cancelling running status, sustain never releasing) are each caught. Run: `cmake -S tests -B tests/build && ctest`.
- A demo chord plays for 1.5 s at boot (inside the audio context), then MIDI takes over.
- Not done: MIDI out, program change, poly pressure, MIDI clock/transport, mapping CCs to patch parameters (needs the knob/UI work).
- Known limitation: `-DDBOY_FLOAT_DSP=OFF` does not link (the engine's `std::atomic<double>` needs 8-byte atomics, which the M7 lacks).

## Status: fork re-synced to current upstream (origin/main a2a457d85, 2026-10-26)

- `tools/sync_dsp.py ref/upstream/sdk/dsp firmware/dsp` rebuilds the fork from an upstream checkout and re-applies every
  FIREFORK change (`tools/fork_patches.py`, each anchor asserted; `tools/fork/` holds Real.h and FastMath.h). A new upstream
  snapshot is one command. It also PRUNES to what the CATHODE voice can reach (66 headers; FX chain and master bus dropped).
- Default build bit-identical to upstream: `bench/render_ref_upstream` vs `render_ref_fork_double`, 22 patches x 5 voices x 1 s.
- New upstream has: dual filter F1/F2, per-slot unison, sub/pulse oscillators, xmod/sync, HPF, the stereo RIGHT chain
  (`startRightChain()` copies HPF, SRR, loop error, drift, grit, F1 and F2 into a twin that runs per voice) and the NEON lane
  path (T-CMP-37/38). The firmware switches the lanes off (`setLaneSitesForTest(0)`): no NEON on the M7.
- **Memory is now the constraint.** `CathodeVoiceEngine` is 642 KB (float) and `prepare()` makes ~10,000 heap allocations:
  5.96 MB in float, 10.3 MB in double (lazy Plaits on). About 6.6 MB of the 16 MB SDRAM. The bulk is ~1,800 Oversampler objects
  (4 small vectors each: shareable coefficients + per-voice history) across the left chain, F1, F2 and the right twin, x16 voices.
  The engine object lives on the SDRAM heap (`CathodeRenderer`), static RAM is 33 KB. Next memory work: share identical
  Oversampler coefficient vectors, prepare the right twin only when stereo is on, prepare only the active voices.
- **clang ignores `-fsingle-precision-constant`** ("optimization flag not supported"). Every earlier host "float" run therefore used
  double literals and flattered the result. Use GCC for float checks: `brew install gcc`, `export SDKROOT=$(xcrun --show-sdk-path)`,
  configure `bench/` with `-DCMAKE_CXX_COMPILER=g++-16` into `build_gcc/`.
- Faithful float (GCC) vs the double reference, `bench/compare_spec.py` (spectral, insensitive to a ~1e-7 pitch offset): level within
  0.02 dB, no NaN, a handful of bins 50-60 dB below the peak differ by 1-4 dB near filter resonances; waveform SNR is only ~44 dB
  because the Wide pitch law is itself computed with float literals (`midiNoteToHz` is off by ~1e-7, 0.0002 cent). Not bit-matched;
  a listening A/B is the real test. TODO: render WAV pairs.
- Float-constant typing is per translation unit. Wide-context expressions with unsuffixed literals are float-precision in those
  files. Benign for the pitch law (above) but keep it in mind when adding Wide code.

## Status: float port (item 2.3), done on the host, unmeasured on the board

- `dsp/include/nemesis/dsp/Real.h`: `Real` is `double` by default and `float` with `NEMESIS_DSP_FLOAT`; `Wide` is always
  `double` and is used for phase accumulators, oscillator frequencies/increments and the whole control-rate pitch chain
  (note Hz, detune and spread multipliers, glide). 56 files / ~800 lines rewritten mechanically, then 3 files by hand.
- **Default (double) build is bit-identical to upstream**: `bench/render_ref_upstream` vs
  `render_ref_fork_double`, 22 patches x 1 s x 5 voices, `cmp` reports no difference.
- **Float build vs the double reference** (`bench/compare.py`): no NaN/Inf, level within 0.01 dB, and 102.6-103.3 dB SNR
  over a 50 ms window on every patch. Over the full second SNR is 64-96 dB; the cause of that slow divergence is not
  isolated (not oscillator phase or frequency, both are Wide now). Treat it as unexplained until someone listens.
- **Static M7 profile of the render path** (`tools/fp_profile.py`): double-precision FPU instructions 1,934 -> 501,
  single-precision 412 -> 1,545; static RAM -25 KB, flash -6 KB. What remains is mostly `pow` / `sin` / `cos` called from
  `prepare()` and table building (not per sample). It counts instructions, not cycles, and ignores loop trip counts.
- Not converted: the FX sources (compile in float mode but are unvalidated) and `MaskTableGenerator::segmentTap`.
- Switches: firmware `-DDBOY_FLOAT_DSP=ON|OFF` (default ON). Remember: in zsh, quote or array-split option lists.

## Status: per-sample math (item 2.2) and companding hoist (part of 2.1), done on the host

- `dsp/include/nemesis/dsp/FastMath.h`: `fm::exp2 / log2 / tan / log1p / expm1`. Taylor or atanh series with range reduction,
  so the error is known, not fitted. Forward to `std::` unless `NEMESIS_DSP_FAST_MATH` and `NEMESIS_DSP_FLOAT` are both set.
  `bench/fastmath_test` sweeps each against libm at the same float input: exp2 7.5e-8 relative, log2 6e-8 relative,
  tan 2e-7 relative up to 0.49 pi, log1p / expm1 under 5e-7 absolute (4.5e-7 of full scale after the mu-law divide).
- Call sites switched (all marked FIREFORK): filter prewarp `tan`, the four modulation `exp2` remaps, the sample-rate reducer's
  `exp2`/`log2`, companding `log1p`/`expm1`, and `setBitDepth`'s `exp2` (exact for integer bit depths).
- `BrokenConverter` no longer recomputes `ln(1+mu)` twice per sample; hoisted into `setCompanding`. Bit-identical.
- Result: the only libm calls still reachable from the render loop are at note-on or parameter-change (`setNoteHz`, `setPan`,
  `setVelocity`, Modal resonator, punch trigger). `tools/libm_callers.py` lists them.
- Double build still bit-identical to upstream. Float + fast math vs float: identical on 21 of 22 test patches, 134 dB (50 ms)
  on the companding patch. GAP: the 22 test patches do not exercise live modulation or a moving cutoff, so the `tan` / `exp2`
  call sites are covered by the unit sweeps only, not by a render. Add modulated patches when the mod-matrix API is scripted.

## Stereo right chain (T-CMP-38) and what it means for the Drumboy

- T-CMP-38 (upstream `process/TASKS.md`, on origin/main, not in the local checkout or in this fork's snapshot) is a DESKTOP card:
  float64x2 NEON lanes across voices for the Stereo-route / width > 0 RIGHT filter chain, double precision, bit-identical.
  Its baseline is 62.7-63.9 % of the block for 16 voices on the desktop (perf-budgets 3d / 3i). NEON does not exist on the
  Cortex-M7, so none of that code helps here; the lanes would run through their scalar fallback.
- This fork was snapshotted before the stereo route (and before T-CMP-37's lane headers and the sdk/ + cathode/ split) existed.
  So the stereo path has not been measured here at all.
- For the M7 the stereo right chain should be a build/patch-level choice, not a given. Options, cheapest first: (1) mono filter
  chain + pan (right = left) when width = 0 -- upstream may already do this; (2) right chain only for the N loudest voices;
  (3) a cheaper second-channel filter (one-pole / shared coefficients); (4) accept it at 3-4 voices.
  Decide after re-syncing the fork to the post-split upstream and measuring it with `bench/`.

## Phase 0: finish what is already started (host, no hardware)

| # | Task | Output | Bit-identical |
|---|------|--------|---------------|
| 0.1 | Locate the stereo route / width > 0 path (branch, file, card) and add it to `bench/` | Benchmark table with width > 0 | n/a |
| 0.2 | Cross-compile benchmark for `cortex-m7` (done for DSP sources: 58/58 + Plaits build, ~419 KB code) | Size report in CI-style script | n/a |
| 0.3 | Add 32 kHz mode + polyphase 3:2 upsampler to the host benchmark; render CATHODE presets at 32 and 48 kHz | WAV pairs for A/B listening | no |
| 0.4 | Profile the host build per function (Instruments / `perf`-style sampling) to rank real hot spots | Top-20 hot functions | n/a |

Exit: a ranked hot-spot list and a listening decision on 32 kHz.

## Phase 1: measurement scaffolding (needs board; ST-Link on order)

1. Board support skeleton: clocks (550 MHz), MPU/caches on, FMC SDRAM at `0xC0000000`, I2S3 + SGTL5000 over I2C3,
   linker script for the app at `0x08020000` (896 KB), `SCB->VTOR` relocation. Reuse CubeMX init from the open-source
   stock app (MIT).
2. DWT `CYCCNT` wrapper around the audio render callback: min / avg / max cycles per block, exposed over SWO or UART.
3. On-screen and LED CPU meter (peak-hold). This is also the player-facing safety valve.
4. Test tone out of the headphone jack (milestone 1 sanity check).
5. Run the benchmark patches on target; replace every guessed ratio in this document with a measured one.

Exit: real cycles-per-sample table for 1/5/8 voices, all benchmark patches.

## Phase 2: compute budget (biggest wins first)

| # | Optimization | Expected effect | Risk | Bit-identical |
|---|--------------|-----------------|------|---------------|
| 2.1 | Control-rate hoisting: coefficients, pitch-to-Hz, smoothing, mod-matrix targets computed per block, not per sample | Moderate; free of audible change if done carefully | Zipper noise if block too coarse | Mostly (verify with goldens) |
| 2.2 | Replace per-sample `sin / exp / tanh / pow` with polynomial or table versions | Large on M7 (newlib libm is slow) | Accuracy; aliasing in waveshapers | No |
| 2.3 | Single-precision (`float`) port of hot paths: oscillators, filters, envelopes, output stage; keep `double` only where numerically required (phase accumulators, long-time constants) | Large on M7 (DP unit is several times slower than SP) | Filter stability at low cutoffs, drift in long-running state | No (regenerate goldens or use tolerance with a written justification) |
| 2.4 | Stereo path: share work between channels; compute second channel only when width > 0 and only the stages that actually differ | Up to ~2x on that path | Behavior change if "sharing" alters decorrelation | Aim: yes for mono / width 0 |
| 2.5 | Cost-aware voice allocator: per-engine and per-filter cost table, steal voices when the meter passes the budget | Prevents dropouts instead of speeding up | Audible steals on heavy patches | n/a |
| 2.6 | 32 kHz engine rate + upsampler (default, if 0.3 listening test passes); 24 kHz option for the heaviest models | ~33% (32 kHz), ~50% (24 kHz) | Top-end loss, more aliasing in nonlinear stages | no |
| 2.7 | CMSIS-DSP for biquads, FIR (upsampler), buffer ops where the algorithm matches | Moderate | Algorithm mismatch | no |
| 2.8 | Optional: bit-identical NEON path for the Mac build only (batch voices) | Faster desktop tests; no M7 benefit | Maintenance cost | yes |

Order: 2.1 -> 2.3 -> 2.2 -> 2.4 -> 2.6 -> 2.5 -> 2.7. 2.8 is optional and independent.
Re-measure after each; stop when 5 heavy-ish voices fit in 70%.

## Phase 3: memory placement

1. Hot code (voice render loop, filters) in ITCM via a `.itcm` section, startup copy from flash.
2. Voice state and small tables in DTCM. Verify with the map file that nothing hot is left in AXI SRAM by accident.
3. Wavetables and delay lines in SDRAM; access sequentially or in bursts; measure with and without D-cache.
4. DMA buffers (I2S, SD, LCD) in a non-cacheable MPU region or with explicit cache maintenance. Write a test that
   catches stale-cache bugs.
5. Plaits tables (75 KB) in flash with cache on; move to TCM only if the meter shows a stall.

Exit: map-file report of where every hot symbol lives, plus before/after cycle counts.

## Phase 4: audio pipeline robustness

1. Double-buffered circular I2S DMA; block size chosen by measurement (start 32 samples at 32 kHz).
2. Audio render in the highest-priority interrupt. Display, key scan, MIDI parse, SD below it, so none can cause
   a dropout.
3. Underrun detector: count missed deadlines, show on the meter.
4. Fixed-ratio polyphase upsampler (32 -> 48 kHz) with a documented passband and stopband; unit test against a
   host reference.
5. Background sample/wavetable loader from SD into SDRAM; never called from the audio interrupt.

## Phase 5: non-audio load

1. Display (NT35510, 540x960, 16-bit parallel): dirty-rectangle redraw only, no full-screen refresh in the steady
   state; budget its CPU time explicitly.
2. Keys / encoders: use the MCP23017 interrupt pins and DMA-driven I2C instead of polling six expanders; coalesce
   reads.
3. MIDI in: UART DMA with idle-line detection; parse outside the audio interrupt.
4. LED updates batched; avoid per-LED I2C writes.

## Phase 6: build settings and regression guard

1. `-O2` baseline, try `-O3` per hot file; `-ffunction-sections -fdata-sections`, `--gc-sections`,
   `-fno-exceptions -fno-rtti`; evaluate LTO.
2. Compiler flags fixed in the CMake preset; `-ffast-math` only for files proven safe, never globally.
3. A script that builds, runs host tests/goldens, and (with the board attached) prints the cycle table. Any change
   that raises worst-case cycles by more than a set threshold fails.
4. SD-card firmware package builder (header `RW_DRUMBOY_PRO_FIRMWARE `, version, size, CRC-32) so updates do not
   need the ST-Link.

## Decision gates

- After Phase 0: is 32 kHz acceptable by ear? If not, drop 2.6 and rely on voice count + 2.3.
- After Phase 1: is the real M7 slowdown within 15-40x? If worse than 40x, plan for 3-4 voices or restricting engines.
- After 2.3: do goldens need regeneration, and does the owner accept the tolerance policy?
- After Phase 2: 5 voices at 70% for the "typical" patch? If not, apply 2.5 more aggressively before Phase 3.

## Open questions

1. Where is the stereo route / width > 0 implementation, and what was the 63% measured against?
2. Which engines, filters and effects must be available on day one, and which can be hidden behind cost limits?
3. Should the effects chain run at the engine rate (32 kHz) or after the upsampler (48 kHz)?
4. SWD (ST-Link) arrives when? SD-card updates are possible before then but make debugging slow.
