# DBWT v1: wavetables for the Drumboy's SD card

One file per bank, `wtNNN.wt` where NNN is the upstream catalogue index (0..199, `sdk/core/src/WavetableCatalogue.cpp` order: Basic 30, Vocal 25,
Metallic 35, Harmonic 40, Chip 30, Mask-complex 3 + Complex 37). All 200 banks are procedural: upstream stores no table data, every frame is the output
of a kernel in `sdk/dsp` (`WavetableKernels.cpp`, `*TableGenerator.cpp`). `content/tools/wt_export.cpp` runs those kernels unmodified and writes the files.

## Layout (little-endian)

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `DBWT` |
| 4 | 2 | version = 1 |
| 6 | 2 | sample format: 1 = int16 PCM (the only one written) |
| 8 | 2 | frames stored (`n`, 16..32) |
| 10 | 2 | frame length in samples (2048; a power of two, one single-cycle period) |
| 12 | 2 | frames the source bank has (16, 32 or 64) |
| 14 | 2 | catalogue index |
| 16 | 1 | category 0 Basic, 1 Vocal, 2 Metallic, 3 Harmonic, 4 Chip, 5 Complex |
| 17 | 1 | mip policy = 0: nothing pre-computed, the loader builds the band-limited pyramid |
| 18 | 2 | reserved (0) |
| 20 | 4 | payload bytes (`n` x frame length x 2) |
| 24 | 4 | CRC-32 (zlib polynomial) of the payload |
| 28 | 4 | reserved |
| 32 | 32 | bank id, NUL padded (`factory.basic.tricircle`) |
| 64 | | payload: `n` frames of 2048 int16, frame-major, sample = float x 32767 |

* **Frames.** A bank with more than 32 frames (26 of 200 have 64) is stored with 32: source frames at evenly spaced indices, first and last kept
  (`k * (src-1) / (n-1)`). Banks of 16 keep 16. The oscillator interpolates linearly between frames, so a 64-frame morph loses half its resolution; a
  bank whose frames are unrelated (chaotic maps, `chip.and`) loses frames outright. Use `--frames 64` in `export_wavetables.py` for the full bank (26 banks double in size).
* **Band limiting.** Not stored. `Wavetable::build()` in the fork (`firmware/dsp/include/nemesis/dsp/WavetableOscillator.h`) takes `numFrames`, `frameLength` and a
  `float*` of frames, FFTs each frame and keeps harmonics `1..(frameLength/2 >> j)` for mip `j = 0..10`, one level per octave. `WavetableOscillator` picks the two
  mips bracketing the note and cross-fades them, and cross-fades the two frames bracketing `position` (0..1 across the frames). So a loader reads the int16, divides by 32768 and calls `build()`.
  Per frame that is 11 x 2048 floats (90 KB), per 32-frame bank 2.9 MB resident: see `content/out/FIRMWARE_INTEGRATION.md` for what that means on a 16 MB board.
* **Set.** `set.txt` / `index.tsv` / `index.json` list the files in slot order. The slot number is what `Presets_generated.inc` calls `table`; the WAVE knob would
  select slot `WAVE - 4`. Slot order is by use in the selected presets, most used first. `catalogue.json` describes all 200 banks (frames, five spectral probes each:
  nearest Drumboy shape, similarity, centroid), not only the exported ones.
* **Basics** (catalogue 0) is the morph saw, square, triangle, sine that the firmware already builds at boot as `SharedTables::shapes`; its file is only there so every slot is uniform.

Total for the current set: see `index.json` (`totalBytes`).
