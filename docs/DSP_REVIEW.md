# DSP review: improvements for the M7 (2026-10-26)

## Status: implemented (2026-10-26)

Everything below is implemented except where the table says otherwise. All DSP changes are patches in `tools/fork_patches.py`
(sections 9-12), behind build switches, so the default build is still bit-identical to upstream.

| Switch | What | Proven by |
|---|---|---|
| `NEMESIS_FORK_EXACT` | B5, E1-E10, E12-E15 | `tools/check_fork.sh`: bit-identical to upstream in double and to the plain float build in float, on `bench/render_cov` (69 patches covering every changed path, including mid-note switches) and `render_ref` (22 patches) |
| `NEMESIS_FORK_APPROX` | N1-N5 | `cov_diff`: no NaN, worst spectral deviation 1.9 dB in one bin of one patch; on the drive patches closer to the double reference than before (63 -> 66 dB SNR). `alias_check_fw`: alias floors unchanged (within 0.1 dB) |
| `NEMESIS_FORK_MAX_VOICES` (8 in the firmware) | P2 | renders identical to the 32-voice build on all 69 patches |
| `fork::Config::lanes / driveOversample / classicOversample / stereoTwin` | E1b, N5, B4, P3 | run-time, set in `main.cpp`; defaults = upstream |
| `NEMESIS_FORK_FAST_DATA` + linker script + startup | P4 | builds: ITCM 60 KB of 64 (68 hottest functions, `tools/gen_itcm.py`), DTCM 14 KB (FIR pool, PD sine table) |

The tests were themselves tested: 10 deliberately broken versions of the optimizations; 9 are caught by `check_fork.sh`, the tenth is an
equivalent mutant (a cache the twin never reads). Three were only caught after adding coverage patches (a static vowel, a route change on
its own, a stereo patch with detune: without detune no unison stack and no stereo twin runs). Bugs B1-B3 and the ten `/code-review` findings in the cost model and renderer are fixed;
`tests/cost` 27,187 checks, mockup `t.js` 190 and `t_cost.js` 1,894 checks pass.

Host result (`bench/cost_bench`, float, same settings before and after): every one of the 86 cases is faster, -42% in total; a plain voice
-46% (279 -> 151 ns), filter drive -55%, FM -55%, the heavy cores -44 to -54%. Heap after `prepare()` 5.96 MB -> 1.36 MB (8 voices, shared
tables, no lane groups). None of this has run on the board yet.

Not done, and why:
- **P1** D-cache on: needs the board (the MPU code is behind `DBOY_DCACHE`).
- **P5** voice-major rendering: a structural change; decide from DWT numbers.
- **E8c** one mask for the 16 named-destination tests: a few cycles, and one missed ungated statement in that block would change sound.
- **E11** evaluating the silent-slot test once per sample: its inputs (cross-mod fades) change within the sample, so caching is not exact.
  Its counter is now diagnostic-only (`NEMESIS_FORK_COUNT`).
- **E14b** skipping the exact-zero last upsampler tap in the exact build: `fma(0, x, -0)` can flip the sign of a zero; done in N1 instead.
- **E13b** caching unison member increments: `phase + inc * mult` is one fused multiply-add, a cached product rounds differently.
- **N6** LFO phase per block: with E8 an unread LFO only advances its phase (one add, one compare), so little is left to gain.
- **B4**: kept Classic at x2 (top cutoff ~15.5 kHz at full resonance); `DBOY_CLASSIC_OVERSAMPLE=4` restores ~31 kHz.

Found while doing it: CATHODE quantises the oscillator to 8 bits by default, so changes of ~1e-7 in the oscillator path often vanish
entirely (renders stay bit-identical); the D-cache and placement questions matter more than any single DSP change here.

---

How this was made: three read-only reviews of `firmware/dsp/` (oscillators, filters and drive, voice/modulation/engine) plus static analysis
of the firmware ELF (`tools/fp_profile.py`, `tools/libm_callers.py`, per-function division and load/store counts, linker map). Nothing here
has been measured on the board. Cycle figures are estimates from operation counts (vdiv.f32 ~14 cycles, vdiv.f64 ~30, a serial FMA chain
~3 cycles per tap) and **leave out memory latency**, which is the biggest unknown (section 1).

Items marked **verified** were re-checked in the code or reproduced; the rest come from one reviewer's reading, with file:line, and should be
confirmed when implemented. "Exact" = bit-identical in the float and double builds. All changes go through `tools/fork_patches.py`.

## 1. Platform: memory decides more than the maths

| # | Finding | Proposal |
|---|---|---|
| P1 | **Verified.** D-cache off, the whole engine (object + ~6 MB of `prepare()` allocations) on the SDRAM heap. 25-33% of the instructions in the hot functions are loads/stores (`modulateSample` 734 of 2,210, `chainPre` 420 of 1,462, `processCore` 366 of 1,447). Uncached SDRAM costs tens of cycles per access, so even one voice may not fit. | Treat `DBOY_DCACHE` (MPU code exists, unverified) as a prerequisite for bring-up, and measure with DWT both ways. |
| P2 | **Verified.** `kMaxVoices` is 32 (not 16). `sizeof(CathodeVoiceEngine)` = 578 KB, of which 527 KB are 32 voices x 16.5 KB; `prepare()` prepares all 32 (~186 KB heap each). | Fork patch `kMaxVoices = 8`: object ~150 KB (fits in AXI SRAM, cacheable), heap ~1.5 MB, and every all-voices loop (incl. `applyVoiceDesc` in the audio interrupt) 4x cheaper. Exact as long as voices x unison <= 8. |
| P3 | The stereo right twin (two filter banks + HPF, SRR, loop error, drift, resonator, noise) is prepared for every voice (`CathodeVoiceStage.h:326-335`). | Prepare only when stereo is allowed, or a small pool for the N loudest voices. Measure the bytes with `bench/heap_probe`. |
| P4 | **Verified.** ITCM (64 KB) and DTCM (128 KB) are empty: the linker script has the regions but no output sections. Render-reachable code is ~115 KB (excluding note-on/prepare/libm/Plaits) against a 32 KB I-cache. | Add `.itcm` / `.dtcm` sections. DTCM: shared FIR coefficient tables (F2), the 8 KB PD sine table, the active voices' decimator histories. ITCM: the ~60 KB hottest functions (`modulateSample` 7.7 KB, `chainPre` 5.2, `processCore` 5.1, `midPass` 4.2, ...). |
| P5 | The engine renders sample-major: per sample, all voices in turn. With a 32 KB D-cache and several KB of hot state per voice, five voices will evict each other every sample. | Structural option: voice-major rendering (one voice for the 32-sample block, then the next). Big change; decide after P1/P2 are measured. |

## 2. Bugs and behaviour changes

| # | Finding | Fix |
|---|---|---|
| B1 | **Verified, my float port.** `PhaseDistortionOscillator::wrap01(Real p)` (`:313`) and the same in `BitmaskOscillator` take a float, so the double phase accumulator is rounded to float every oversampled step. Simulated pitch error: **-3.1 cents at 55 Hz**, -0.26 at 110 Hz, -0.12 at 20 Hz, ~0 above 440 Hz. Bass notes detune against other oscillators. Likely cause of the "unexplained slow divergence" between float and double. | `static Wide wrapAcc(Wide p) { return p >= 1.0 ? p - 1.0 : p; }` for the accumulator steps (inc >= 0), keep the float wrap for the table read. Cost neutral; double build exact. Add a pitch test at 55 Hz to `bench/pitch_probe`. |
| B2 | `CathodeRenderer::configurePatch` sets cutoff and slot levels directly on the stage. After a FiltCutoff route is added and cleared, `refreshLive` restores the voice's own base (20 kHz) (`CathodeModulatedVoice.h:2106-2118`). | Use `setPatchCutoff` / `setPatchLevel` / `setPatchAmpEnv`. |
| B3 | Steal fade is 0 by default (`stealFadeSec_`, `h:2533`). With 5 voices the next note steals a voice whose tail still sounds: the pitch jumps mid-tail. | `setStealFade(0.003)` in `CathodeRenderer::init`. |
| B4 | Side effect of `DBOY_FILTER_OVERSAMPLE=2`: Classic's stability cap is on G, so its top cutoff halves. At reso 1 it now stops at 15.5 kHz (was ~31 kHz), at 0.75 at 18.2 kHz. Stable, but a high-resonance sweep audibly stops. | Accept, or keep Classic at x4 and others at x2. Document it in `CATHODE_COST.md` 3b. |
| B9 | **Also in upstream; memory safety.** Found by the firmware's end-to-end stress under ASan (2026-10-26). `LoopError::setNoteHz` can make the loop period shorter than the phase already reached; `processSample` subtracts the period once per sample, so `phase_ / period_` stays above 1 for many samples and the read offset runs past the 6-sample ring (heap overflow; on the board, reads of whatever SDRAM follows). Repro: `prepare(44100); setLoopErr(1); setNoteHz(40);` 1000 samples, `setNoteHz(2000);` one sample. | Wrap the phase into the new period in `setNoteHz` (`fmod`). Fixed in the fork (`tools/fork_patches.py`, B9); nothing changes where upstream is defined, `check_fork.sh` still bit-identical. |
| B10 | **Also in upstream; memory safety, a hard fault on the board.** Found by the firmware's end-to-end stress under ASan (2026-10-26). `WavetableOscillator::processSample` subtracts 1 from the phase once, so a frequency at or above the sample rate (`inc_ >= 1`) lets the phase grow without bound and `readAt` indexes ever further past the table. Reachable from the panel: MIDI note 127 with OSC OCT +2 and SEMI +12 is 8 times Nyquist. Repro: `Wavetable::build(sine, 1, 2048); setSampleRate(44100); setFrequency(50000); processSample()`. | Subtract `floor(phase_)` when the phase is 2 or more (the same subtraction below that). Fixed in the fork (`tools/fork_patches.py`, B10); `check_fork.sh` still bit-identical. |
| B5 | `startRightChain` (`CathodeVoiceStage.h:2035`) copies whole filter banks, including the Comb ring (2,209 floats) and every core's FIR history: ~6-7k floats SDRAM to SDRAM in one sample. | Copy the Comb ring only in Comb mode and only the active core's oversampler. |

The `/code-review` of the cost-model code found 10 more (filter 2 demotion not pushed to the engine, governor ignores panic during settle,
voice steal books the saving before the tail ends, `applyVoiceDesc` spike in the interrupt, enum drift with StateMorph/ReedMorph, ...).
They are in the firmware glue, not in the DSP, and are listed in that review.

## 3. Exact CPU savings (bit-identical)

| # | Where | Saving (estimate) |
|---|---|---|
| E1 | **Verified.** The lane scaffolding runs every sample although lanes are off: `runPdLanes<0/1>`, `runClassicLanes<false/true>`, `runDriveLanes` each walk 8 groups x 4 voices of flags (`CathodeVoiceEngine.h:1495-1509`, `1609`, `1723`, `1810`), plus `markPdReady`. Add `if (!site.on) return;` at the top (and in `processMono`), or a `DBOY_NO_LANES` switch that also drops the four sites (20 KB of the object) and 40 lane FIR `prepare()` calls. | ~1,000-2,600 instructions per **sample** (not per voice), ~160 flag loads |
| E2 | Cross-mod legs compute outputs nobody reads (`CathodeOscCore.h:2296, 2306, 2386, 2439`): `fmA_` decimation discarded, `fmB_.processSample(0, ...)` upsamples zeros, ring's `xmodA_` decimation discarded. Add `pushInput` / `upsampleOnly` to `Oversampler`. | FM ~133 MACs, ring ~65 (+65 with a side) per voice-sample |
| E3 | Cutoff modulated (every patch with a filter envelope): `pushFilterCutoffReso` (`CathodeModulatedVoice.h:816`) runs `TptSvf::setCutoff` (tan + 4 div) even in model modes, and the core's `setCutoff` recomputes reso-only terms (k, sqrt, comp gain). Set the SVF only in LP/BP/HP/Notch; recompute reso terms only when reso changes; early-out when nothing changed. | ~130 cycles per filter-sample |
| E4 | The right twin and F1/F2 recompute identical coefficients (`CathodeVoiceStage.h:774-786`). Compute once, copy (`ClassicFilter::Coefficients` exists). | ~250 cycles per filter-sample in stereo |
| E5 | `setVowel` (`CathodeVoiceStage.h:664`) recomputes 6 formant SVFs (tan + 4 div each) per sample when vowel is modulated, even outside Formant mode. Apply on entering Formant. | ~540 cycles per voice-sample when routed |
| E6 | Ladder decimates both the 12 and 24 dB taps and uses one. Add `Oversampler::pushOnly`. | 65 MACs at x4 |
| E7 | Vibrato (Pitch routed) retunes per sample: `CathodeOscCore::setNoteHz` has no unchanged-check, `resolveSawKnee` no early-out, the sub retunes while silent (`log2f` + div). Add the early-outs. | most of ~400-600 cycles per voice-sample while vibrato is on |
| E8 | Modulation: every LFO's value is computed per sample even when nothing reads it (`h:1606`); `evaluate()` walks 24 slots; `feedSources` writes 25 sources; 16 separate `destLive_` byte tests. Source mask, compact active-slot list (same order, so sums stay identical), one `uint32` live mask. Note: real presets always have hard-wired slots, so the idle-matrix skip rarely fires. | ~160-220 cycles per voice-sample |
| E9 | Request flags polled per sample: 11 loads per voice-sample plus 3 acquire loads (`ldr` + `dmb`) per sample. In this firmware every setter runs in `beginBlock()`, so latch once per block. | ~30-40 cycles per voice-sample cached, ~200 uncached |
| E10 | `processCore` dispatch is an if-chain (~25 compares for LowPass) before a switch (`CathodeFilterBank.h:755-864`). One switch. | ~20-40 per filter-sample (x2 with twin, x2 with F2) |
| E11 | `slotUncoupledAndSilent` evaluated 4x per sample (~11 loads each); `++fork::Config::slotSkips` in the hot path. Compute once in `beginSample`; counter only in a diagnostic build. | ~30 loads per voice-sample |
| E12 | PD generator reloads 6 members per oversampled step (aliasing with `ohist_` stores) and stages through `pdStage_` with no lane to read it. Locals + one store; `pdStaging=false` in ForkConfig. | ~30-40 SDRAM accesses per PD slot-sample |
| E13 | Unison: `freqHz / sampleRate_` per sample in `stackSample`; PD/Bitmask stack multiplies `incOs_ * incMult[k]` per member per OS step (32 vmul.f64 at 8 members). Cache on change. | small to ~100 cycles with unison |
| E14 | FIR coefficient vectors are per instance (~1,800 copies of the same few designs). Static per-factor tables. Skip the upsampler's last tap of phases >= 1 (exactly 0.0). | memory; halves coefficient traffic; 1 tap per phase |

## 4. Nearly exact (1 ulp to 1e-8; check with `compare_spec` and `alias_check`)

| # | Where | Saving |
|---|---|---|
| N1 | **The biggest single lever. Verified design property.** The oversampler prototype is a windowed sinc at 1/L, so every L-th tap from the centre is a sinc zero (~1e-8 in float, not exactly 0): upsampler phase 0 is a pure delay, the decimator multiplies 16 taps by ~0. Today one serial `vfma` chain (latency-bound), 2 loads per tap. Compact the tables (drop sinc zeros), fold symmetric pairs (symmetrise at design), 4 accumulators, unroll per factor. Applies to every oversampler: PD, Bitmask, cross-mod, filter cores, drive, limiter. | Decimator L=4: 65 FMA -> ~25 FMA + 24 add; L=2: 33 -> ~9 + 8. Upsampler L=2: 34 -> 17. Roughly 100-130 cycles per oversampler-sample |
| N2 | Constant divisions in the saturators (`Waveshaper.h:71-86`: `/3`, `/12`, `2x/3`), `/kLimitDrive` in `SelfOscLimiter` (36 vdiv.f32 in `crossStep`), Soviet `/kNegKnee`. Multiply by the reciprocal; ADAA closed form when both inputs are inside the knee (no division, more accurate). | Classic x2 ~130 cycles, driven Ladder ~220, drive ~110, limiter ~110 per sample |
| N3 | Divisions by the sample rate in `setNoteHz` / `updateFrequency` (5 vdiv.f64 per PD slot per retune): cached reciprocals, `incOs_ = inc_ * invFactor_` (exact for 1/2/4/8). `fm::log2` in `WavetableOscillator::resolveMip`. `1/lenS_` in DrawEnvelope, `1/panGlideSamples_`. | with E7: vibrato almost free |
| N4 | PD Pulse/SawPulse call `powf` per oversampled step (`PhaseDistortionOscillator.h:369, 375`): 4 per slot-sample. Use `fm::exp2(powExp_ * fm::log2(ph))`. | ~250-600 cycles per slot-sample on those shapes |
| N5 | Filter drive and output drive ignore `filterOversample` (still x4, `CathodeFilterBank.h:212`, `CathodeOutputStage.h:141/144`); the Ladder too (`:229`). A driven Ladder is the heaviest core (198 FMA + 16 div) but costed "light" because the benchmark had drive 0. `fork::Config::driveOversample`. | ~67 FMA (drive), ~100 FMA + 8 div (Ladder) |
| N6 | LFO phase advanced per block (`phase += n*inc`, with one `crossBoundary()` per counted wrap so random draws stay in step). Keeps phase continuity. | ~25-35 cycles per voice-sample |

## 5. Suggested order

1. **Bugs B1-B3** (small, and B1 is audible). Add a 55 Hz pitch check so it stays fixed.
2. **Exact batch:** E1, E2, E6, E10, E11, E7, E3, E5 (fork patches, each proven with `savings_check`-style identity renders and an engagement counter where a path can be skipped). Re-run `cost_bench` and regenerate the cost table.
3. **Memory:** P2 (`kMaxVoices = 8`), P3, E14, then the DTCM/ITCM sections (P4).
4. **Nearly exact:** N1 first (largest, everywhere), then N2, N4, N3, N5, N6, each with `compare_spec` and `alias_check`.
5. **On the board:** P1 (D-cache on/off), then decide on P5 (voice-major rendering) and B4 from the DWT numbers.

## Firmware fix: notes lost on one-voice parts (found by the preset stress test)

`Instrument::renderCathode` skipped an engine whose voices were all idle. A note that arrived while the single voice was
still in its 3 ms steal fade was queued behind that fade, and since nothing rendered, the fade never finished: the note
was dropped (Acid Roll lost notes 1, 4 and 8 of a 9-note phrase). The engine is now also rendered while any voice is
`stealFading`. Regression test: `testMonoPhrase` in `tests/app/test_audio.cpp` (1, 2 and 3 voices, default patch,
Acid Roll, preset 12).

## Open: a chord is ~3 dB quieter after an earlier single note (upstream behaviour, not fixed)

Found by the preset stress test. On a part with VOICES 2, a two-note chord (60 + 67) measures RMS 0.0181 on a fresh
engine and after an earlier chord, but 0.0127 (-3 dB, about 1/sqrt(2)) after any earlier single note, whether a BROWSE
preview or a played note (preset 120 shows it; about 15 presets do). At VOICES 1 there is no difference. It is not an
effects or preview issue. It fits the stale per-voice `stackGain_` that `CathodeVoiceEngine.h` already documents (a reused
voice keeps the earlier stroke's 1/sqrt(n) factor). `firmware/dsp` is identical to upstream, so the fix belongs in upstream:
report it there (it moves the unison golden, as the header comment says) rather than patching the fork here.

Also known and accepted: after a BROWSE preview the effects follow the preview preset for 3 s after its last note, so FX
edits made in that window are heard only afterwards.
