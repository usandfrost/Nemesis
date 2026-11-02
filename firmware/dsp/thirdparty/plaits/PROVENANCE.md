# Vendored tree provenance — Plaits + stmlib subset (ADR-008)

Sanctioned single vendored MIT tree (ADR-008 §1/§7). Reached ONLY through the
first-party `MacroOscillator` L1 seam (ADR-008 §2, T-PLAITS-01b); no other L1
source includes a `plaits/` or `stmlib/` header (enforced: `tools/check-includes.sh` R3).
Carved out of clang-format/clang-tidy (ADR-008 §4 — `tools/gate.sh` scope +
`.clang-tidy` HeaderFilterRegex). MIT notices: `THIRD_PARTY_LICENSES` at the repo root.

## Upstream

| Component | Repository | Pinned commit |
|---|---|---|
| Plaits (`plaits/`) | https://github.com/pichenettes/eurorack | `08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4` (master, 2026-07-16 fetch) |
| stmlib (`stmlib/`) | https://github.com/pichenettes/stmlib | `e3bd7c9cc00e4364166f9905c0509b6ffd0535ec` (the eurorack submodule pin at the commit above) |

Upstream is frozen (Mutable Instruments wound down) — divergence risk ≈ nil (ADR-008 Context).

## What was taken (pruning by SELECTION — ADR-008 §1)

- `plaits/dsp/**` — the complete synthesis tree (all engines incl. `engine2/`; the v1
  EXPOSED roster is gated at the wrapper, not here).
- `plaits/resources.{h,cc}` — generated LUT/wavetable/formant/DX7-patch tables.
- `plaits/user_data.h` — referenced by `voice.h` + several engines (patched, below).
- `stmlib/stmlib.h`, `stmlib/dsp/{atan,cosine_oscillator,dsp,filter,hysteresis_quantizer,
  limiter,parameter_interpolator,polyblep,rsqrt,units}.h`, `stmlib/dsp/{atan,units}.cc`,
  `stmlib/utils/{buffer_allocator,dsp,random}.h`, `stmlib/utils/random.cc` — exactly the
  headers/TUs the Plaits DSP tree includes (transitively closed).

NOT taken: Eurorack firmware scaffolding (`plaits/{plaits.cc,ui.*,settings.*,drivers/,
bootloader/,hardware_design/,test/,user_data_receiver.*,pot_controller.h,resources/}`),
`stm_audio_bootloader`, the rest of stmlib. **License note:** `plaits/test/plaits_test.cc`
and `stmlib/test/` are GPL-3.0 upstream — they MUST NOT be vendored; everything vendored
here carries the MIT header (audited: zero "GNU General Public License" matches in-tree).

## Local patches (each marked `[UPSTREAM T-PLAITS-01a]` inline)

Upstream gates all Cortex-M/STM32 code behind `#ifndef TEST`; instead of defining a
global `TEST` macro for every consumer TU, the portable (upstream TEST) branches were
kept unconditionally and the STM32 branches pruned (ADR-008 §1 "prune Cortex-M
assumptions"):

1. `stmlib/stmlib.h` — `IN_RAM` linker-section attribute → empty define.
2. `stmlib/dsp/dsp.h` — `Clip16`/`ClipU16`/`Sqrt` thumb `ssat`/`usat`/`vsqrt` asm →
   portable C fallbacks.
3. `plaits/user_data.h` — STM32 flash backend (`<stm32f37x_conf.h>`) → the mock branch,
   with the debug `printf` hexdump stripped to silent no-ops; `ptr()` returns `NULL`
   (engines fall back to the default `resources.cc` tables).
4. `plaits/dsp/engine2/chiptune_engine.cc` — genuine upstream BUG FIX: `ChiptuneEngine::
   Init` initialised only `kChordNumNotes` (4) of the `kChordNumVoices` (5)
   `SuperSquareOscillator`s that `Render` uses. Latent upstream because the firmware
   `Voice` is static (zero-init BSS); a heap/stack-allocated Voice leaves `voice_[4]`
   with garbage phase state, and a NaN/huge `slave_phase_` never exits the polyblep
   transition loop — the first unclocked chiptune render (engine 7) spins forever.
   Init loop bound corrected to `kChordNumVoices`.

No other source lines were modified. Re-syncing with upstream (unlikely — frozen) means
re-applying exactly these three patches.

## Known desktop-port characteristics (wrapper concerns, T-PLAITS-01b)

- `plaits::kSampleRate` is hard-coded 48 kHz (`plaits/dsp/dsp.h`); host-rate handling
  belongs to the first-party wrapper.
- `stmlib::Random` is a GLOBAL LCG stream shared across instances — per-instance
  determinism is resolved at the wrapper seam.
- Output frames are 16-bit shorts in `kBlockSize`(=12)-frame blocks.
