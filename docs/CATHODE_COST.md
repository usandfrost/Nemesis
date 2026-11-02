# What a CATHODE voice costs, and what to cut for the Drumboy

Measured with `bench/cost_bench.cpp` (one voice, held note, lanes off as on the M7) and `bench/base_loop.cpp` + macOS `sample`
(function-level profile). Host = Apple Silicon, double precision, `-O2`. **These are RELATIVE costs. The M7 has no NEON, does float in
hardware and has a much smaller cache, so absolute numbers will differ and the ranking may shift a little.** Re-run both on the board with
the DWT counter before cutting anything.

Baseline voice (one phase-distortion oscillator, low-pass filter, no modulation routed): **about 325-335 ns per sample on the host.**
Re-run: `cmake --build bench/build_new --target cost_bench_double && bench/build_new/cost_bench_double`.

## 1. Where one voice spends its time (share of the voice)

| Stage | Low-pass | Classic filter | Acid filter |
|---|---|---|---|
| Oscillators (PD generation at 4x, decimating FIR, cross-mod, unison) | 44% | 29% | 32% |
| Filter stage (F1 / F2, the non-linear cores) | 26% | 50% | 46% |
| Modulation (LFOs, drift, matrix, mod envelopes), running even when nothing is routed | 11% | 8% | 8% |
| Engine glue (voice loop, mid passes) | 9% | 7% | 7% |
| Filter drive (oversampled waveshaper) | 7% | 4% | 5% |
| Amp envelope, level, pan | 3% | 2% | 2% |
| Grit (bit depth, converter, loop error) | <1% | <1% | <1% |

**The single most expensive function** is the phase-distortion oscillator's **decimating FIR** (`Oversampler::decimateSample` for the PD
generator, 15% of a plain voice), followed by the generator itself and the per-slot oscillator work. With a heavy filter, the **non-linear
filter cores** take over.

## 2. What each feature adds to one voice (extra cost over the baseline)

| Feature | Extra | Notes |
|---|---|---|
| Cross-mod: ring modulation | +47% | second oscillator audible |
| Filter: Classic (LP24/LP12/BP/HP) | +44-50% | oversampled non-linear core |
| Filter drive | +33-36% | oversampled waveshaper |
| Filter: Acid | +34% | |
| Unison width > 0 (engages the stereo right chain) | +16% with a low-pass, +37% with Classic | re-runs the whole filter for the right channel |
| Filter: Reed | +30-33% | |
| Filter: Soviet, Scream | +26-29% | |
| Unison x8 / x4 / x2 | +25% / +16% / +12% | |
| Cross-mod: FM | +26% | |
| Macro engines TwoOpFm, Particles | +16-19% | all other 14 engines are within +/-5% |
| Companding | +14% | |
| Phase distortion vs wavetable / pulse | +10-27% | both slots on wavetable: 0.73x of both on PD |
| Fold | +7% | |
| Second filter F2 in series / parallel / stereo route | +4% / +8% / +7% | with a cheap F2 |
| Hard sync, HPF, sub oscillator, bit depth, converter rate, loop error, drift | 0-4% | essentially free |
| Filter models LP, BP, HP, Notch, Formant, Comb, Ladder 12/24, State (all 6) | 0% | cheap: the baseline already pays for the filter stage |

## 3. Things that cost nothing to remove (no feature lost)

1. **A silent oscillator slot is still computed.** Both slots on wavetable cost 237 ns whether slot 1 is audible or at level 0. Skipping an unused
   slot saves roughly 10-25% of a voice. This needs a small change in the fork (a `slot active` gate in `CathodeOscCore`).
2. **The PD oversampling factor** is 4 by default. Factor 2 saves **14%** of a voice (47 ns), factor 1 saves 22% but aliases. It is a
   `prepare()` argument already, so the firmware can set it without touching the DSP.
3. **Non-linear filter cores oversample x4 by default** (Classic, Acid, Reed, Soviet, Scream). Factor 2 halves what they add: Classic +150 -> +78 ns,
   Acid +123 -> +62 ns. Also a `prepare()` argument.
4. **The modulation system is about 10% of every voice even when idle.** An "idle" fast path that skips LFOs, drift and the matrix when no
   destination is routed would save most of it (fork change).

## 3b. The savings, implemented (`tools/fork_patches.py` step 8, switches in `ForkConfig.h`, verified by `bench/savings_check` + `bench/alias_check`)

| Saving | Switch (firmware default) | Result (one voice, host; float GCC build in brackets) | Exactness |
|---|---|---|---|
| Idle mod matrix: skip feeding and evaluating the matrix while it has no active slot | `DBOY_IDLE_MATRIX` (ON) | -5% (-4%) | **bit-identical**: with no active slot `evaluate()` is a no-op |
| Silent oscillator slot: do not generate or decimate a slot whose level is 0 and that nothing couples to the other slot (no FM, ring, sync, fade in flight, unison stack) | `DBOY_SKIP_SILENT_SLOTS` (ON) | -10% plain, -5% Classic (-9% / -8%) | **bit-identical** while the level stays 0; the skipped slot's phase freezes |
| Both exact savings together | | **-12% plain, -6% Classic (-14% / -11%)** | bit-identical on 12 patches, in double and float |
| Non-linear filter cores at 2x instead of 4x oversampling | `DBOY_FILTER_OVERSAMPLE` (2) | Classic voice -26% (alone), Acid -19% | alias floor: Classic -38.8 -> -38.0 dB, Acid unchanged (-50.9 dB) |
| Phase-distortion oscillator at 2x instead of 4x | `DBOY_PD_OVERSAMPLE` (**4**, opt-in) | plain voice -14% | alias floor -46.0 -> **-41.1 dB** on a bright 2.7 kHz tone (x1: -29.5 dB). A real quality cost, so it is off by default |
| All of the above with PD at 2x | | plain -27%, Classic -36% | |

How the exact ones are proven: `savings_check` renders 12 patches with the savings off and on and compares every sample with `memcmp`. Six of them
must engage (a counter, `fork::Config::slotSkips`, shows 63,354 skipped slot-samples) and six must NOT (FM, ring, hard sync, unison on the silent
slot, both slots audible). A first version of the slot skip never engaged, because `xmodFadeStep_ > 0` is the default and I had read it as "a fade
is running". The counter exists so a saving can never again pass its identity test by doing nothing.

Not done, and why: the modulation SOURCES (LFOs, drift, envelopes, step, S&H) still tick while the matrix is idle. They have to, because "an
un-bypassed LFO comes back in phase" is a documented property, and a tempo-synced LFO or a mod envelope would restart or lose its place if routed
later. Skipping them would save another ~5-6% but changes behaviour, so it is not a free saving and is not implemented.

## 4. Candidates to remove or restrict (a decision for you, not a recommendation)

| Candidate | Saves | What you lose |
|---|---|---|
| Ring mod and FM cross-mod | 26-47% on voices that use them | two classic timbres |
| Classic, Reed, Soviet, Scream, Acid filters | 26-50% on voices that use them | the "model zoo" character; keep one (Acid or Classic) as the signature non-linear filter |
| Filter drive | 33% | drive on the filter input |
| Unison width and the stereo right chain | 16-37% | true stereo voices; mono voice + pan stays |
| Unison > 4 | up to 9% | thick stacks |
| Macro engines TwoOpFm, Particles | 16-19% on voices using them | two of sixteen engines |
| Companding | 14% | one grit character |

**Proposal.** Keep every oscillator, macro engine and cheap filter model (they cost the same as the baseline). Apply the four free items in
section 3. Make Classic or Acid the one heavy filter and gate the others behind a per-voice cost label. Treat ring mod, FM cross-mod, filter drive and
unison width as "costly" options that count extra against the voice budget (the mixer's cost meter already has the idea).

## 5. Not measured yet

Wavetable table lookups under cache pressure, the mod-matrix destinations when routed, the MIDI and sequencer overhead, F2 with a heavy model, and
every number on the M7 itself.

## 6. How to implement "count the costly options against the budget" and "one heavy filter"

**A cost model, data-driven.** Each feature gets a cost in voice units (a plain voice = 1.0), taken from section 2 and regenerated from
`cost_bench` (and later from the board). Per-voice cost = 1.0 + the sum of the enabled, audible extras:
ring mod +0.47, FM +0.26, filter drive +0.34, unison x2/x4/x8 +0.12/+0.16/+0.25, a heavy filter model (Classic +0.47, Acid +0.34, Reed +0.31,
Soviet / Scream +0.27; roughly half those with `DBOY_FILTER_OVERSAMPLE` 2), unison width = the active filter's extra again, and so on.
The table lives in one generated header (`CostTable.h`), so changing a number never touches code.

**Where it is computed.** At control rate only (preset load, a parameter change), never per sample. `partCost = voices x voiceCost(patch)`;
the mixer sums the parts and compares with the budget. The budget itself is measured: 70% of the sample period divided by the measured cycles of
a baseline voice. Until the board runs, it stays at 5.0 as in the mockup.

**What happens at the limit** (a decision for you, in increasing strength):
1. *Show it.* The meter and warning already in the mockup, plus a cost badge on every costly knob (the cell turns amber).
2. *Cap it.* Raising a costly option lowers the part's allowed voices so the total stays inside the budget ("voices 3 -> 2 because ring mod is on").
3. *Degrade at run time.* If the DWT cycle meter shows overruns, steal the oldest voice first, then drop the costliest option on the part with the
   lowest priority (unison to 1, drive off), in a fixed documented order. Nothing is removed from the patch, only bypassed until the load drops.

**One heavy filter.** Classify models as LIGHT (cost 0: LowPass, BandPass, HighPass, Notch, Formant, Comb, Ladder 12/24, State x6) and HEAVY
(Classic, Acid, Reed, Soviet, Scream). Two ways to restrict:
- *Build-time subset* (a CMake list, `DBOY_FILTER_MODELS=light+classic`): the other cores are not compiled in. That also saves flash (each
  non-linear core is several KB) and removes them from the model knob. Simplest and strongest.
- *Run-time rule*: all models stay, but at most one HEAVY model is allowed per part and the browser shows a cost class (LIGHT / MEDIUM / HEAVY) per preset.

**Order of work.** (1) the cost table and `voiceCost()` as a host-tested pure function; (2) feed the mixer meter from it; (3) the allowed-voices cap;
(4) the model subset switch; (5) the run-time degrade, which needs the real cycle meter on the board. The first two are about a day's work and
change no sound.

## 7. Implemented: cost model, degrade at the limit, one heavy filter per lane

| Piece | Where | Verified by |
|---|---|---|
| Cost table (generated from the benchmark) | `tools/gen_cost_table.py` -> `firmware/src/cost/CostTable.h`, and the `COSTTABLE` block of the mockup | regenerated from `out_new/cost_fw.txt` |
| `voiceCost()`, heavy/light classes, light fallbacks, one-heavy-filter rule, `allowedLock` | `firmware/src/cost/CostModel.h` (C++), `docs/mockup/drumboy-panel.html` (JS port) | `tests/cost/test_cost.cpp` 28,945 checks; model vs real engine: 36 patches, mean error 0.01 (`bench/model_check`); JS port equals C++ on 400 cases and 120 ladders (`mocktest/t_cost.js`) |
| Degrade ladder (release oldest voice on the lowest-priority part, then bypass the option with the biggest saving per part, last voices -> 1) | `cost/Degrade.h`, JS `buildLadder` | `bench/degrade_check`: worst gap to the real engine 10 percentage points |
| Governor (panic 0.97, enter 0.85 for 64 blocks, exit 0.60 for 1400 blocks, settle 48) | `cost/Governor.h`, `audio/CathodeRenderer.cpp` `beginBlock()` | unit tests; on the board it reads the DWT block cycles |
| Mockup UI | FILTER 2 has ACTIVE (was SPREAD); `+0.34`-style cost badges and `HEAVY` on knobs; MIXER view 2 (press MIXER again): PRIO A-C, AUTO DEGRADE, CPU LOAD, DEGRADE n/N, BOARD SPEED, WORST CASE; INFO lists the ladder; the bottom-left corner shows `Dn` while degraded | jsdom suite, 187 checks, mutation-tested |

**Interpretation of "one heavy filter per lane"** (say if you meant otherwise): the sound of a lane (the part patch plus the step's p-locks)
may use at most one heavy filter among F1 and F2 (an F2 that is OFF does not count). Choosing a second one sends the *other* filter to its
light twin (Acid/Classic/Soviet -> Ladder, Reed -> State), the filter you just chose wins. A p-lock that would create a second heavy filter
becomes that light twin instead. Nothing else is restricted: all 26 filter models still exist.

## 8. After the DSP review (docs/DSP_REVIEW.md), regenerated 2026-10-26

The table is now measured with the firmware's DSP configuration (`bench/cost_bench_fw`: float, fast math, `NEMESIS_FORK_EXACT` +
`NEMESIS_FORK_APPROX`, 8 voices, no lanes, `COST_CFG=fw COST_OS=2`: filter cores and drives at x2), and it records that factor
(`kTableOversample`). A plain voice is 151 ns on the host (was 279 with the same settings before the review).

What changed in the model:
- **Heavy** is decided on the x4-equivalent extra (`extraAtX4`), the scale the 0.2 threshold was defined on. With these numbers only
  **Acid and Classic** are heavy; Scream, Soviet and Reed measure under 0.06 of a voice at x2 and count as light. The rule stays; which
  models it covers comes from the table, and will change again when the board is measured.
- Every model has a light twin (`lightTwin`); `gen_cost_table.py` refuses a table where a heavy model has none, and `lightFallback` never
  returns a heavy model (the plain LowPass as a last resort). StateMorph and ReedMorph are in the enum and the table (28 models).
- A skipped silent slot no longer adds its oscillator kind's delta. The drive extra scales with `driveOversample`. Unison on slot 2 is
  measured (`kUnisonSlot1Factor` = 0.8; it was 2 before the review's changes).
- Model vs real engine (`bench/model_check_fw`, 36 patches): mean error 0.02 of a voice, worst 0.17 (Classic + F2 + unison + width,
  under-estimated). Degrade ladder vs engine (`degrade_check_fw`): worst gap 10 percentage points.
- The JS port in the mockup reproduces the C++ model on 400 random voices (now with oscillator kinds and drive factor) and 120 ladders.
