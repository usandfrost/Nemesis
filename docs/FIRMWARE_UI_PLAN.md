# The panel mockup in the firmware: plan

The spec is the mockup, `docs/mockup/drumboy-panel.html` (version 17), and `docs/UI_CONCEPT.md`. This plan says how it becomes firmware and
what differs on the real hardware. Nothing below has run on the board yet (no ST-Link); everything that can be is tested on the host.

## Layers

| Layer | Where | Runs | Tested by |
|---|---|---|---|
| Model: project, 16 patterns x 10 lanes x 32 steps, 3 parts, 8 songs, p-locks | `firmware/src/app/Model.*` | anywhere | host |
| Music theory: Euclid, scales, chords, arpeggio | `app/Theory.h` | anywhere | host |
| Parameter pages, units, presets | `app/Params.*` | anywhere | host |
| UI state machine: modes, bindings, keys, undo, toasts | `app/Ui.*` | main loop | host (a port of the mockup's checks) |
| Sequencer: clock, swing, steps, chords, arpeggio, probability, patterns, songs | `app/Sequencer.*` | audio interrupt | host |
| Project file (binary, versioned) | `app/ProjectFile.*` | main loop | host round trip |
| Graphics: display list, strip rasteriser, anti-aliased fonts | `firmware/src/gfx/*` | main loop | host renders PNGs |
| Screen: every view of the mockup drawn from the UI state | `gfx/Screen.*` | main loop | host PNGs next to mockup screenshots |
| Sound: 3 parts, each a CATHODE engine or a sampler; pages mapped to CATHODE setters; p-locks per voice; one degrade ladder over all parts | `audio/Instrument*`, `audio/Sampler*`, `audio/PatchMap*` | audio interrupt | host renders |
| Board: LCD (stock NT35510 bus), keys (3 x MCP23017 on I2C3), encoders (EXTI), layer LEDs and VU bars (MCP23017 on I2C4), SD card (FatFs) | `firmware/src/board/*` | main loop + interrupts | on the board only |

The app core has no HAL in it: the board code feeds it key, encoder and time events, and it calls back through `app::Services` for
sound previews, the SD card and engine status. The same core runs in the host tests and the host screen renderer.

## Threads

- **Audio interrupt** (I2S DMA half/full): sequencer step processing, engines, governor. Never allocates, never waits.
- **Main loop**: key and encoder events, UI, drawing (only when something changed, at most ~30 frames/s), LEDs, SD card.
- The UI edits the model in place; the sequencer reads it. Every field is a byte or a short with a valid value at all times, so a read
  that interleaves with an edit sees an old or a new value, never garbage. Transport changes the UI asks for while playing (next
  pattern, song jump) are single-byte requests the sequencer takes at the end of a pattern, as in the mockup.

## What differs from the mockup on the hardware

- **LEDs are one colour, on or off** (one MCP23017 bit per layer key). The mockup's part colours, dim "has content" and white hit flash
  become: on = selected / trig, a short blink = hit or cursor.
- **The screen is drawn by the firmware**, not a browser: the same layout, colours and fonts (Chakra Petch and IBM Plex Mono, both OFL,
  converted to anti-aliased bitmaps at build time), but symbols such as the play triangle are drawn shapes.
- **Key positions**: the mockup's drawing is not the panel. Keys are bound by the stock firmware's key names (RESET = stop,
  PLAYSTOP = play, CENTER = OK, STYLE = step, KNOB = shift, LFO = L/back, LAYER INST = I/project, LAYER SONG = S/song). The left keypad's
  16th key (stock: SONG, bottom left; the mockup's power key) is unassigned.
- **Samples come from the SD card** (`/Sample/<category>/*.wav`, the stock card layout). With no card, the twelve samples the mockup
  synthesises are generated at boot, so the instrument always has drums.
- **The sound pages are the mockup's eight-knob pages**, mapped onto CATHODE's own setters (`audio/PatchMap`). Where the mockup used a
  stand-in (WAVE = SIN/TRI/SAW/SQR), the firmware uses CATHODE's real choice at that spot (the phase-distortion types); the mapping table
  is in `docs/PATCH_MAP.md`.

## Order

1. App core (model, theory, params, UI, sequencer, project file) with host tests.
2. Graphics and screen, host PNGs compared with the mockup.
3. Sound: instrument with three parts, patch map, sampler, p-locks, degrade over all parts.
4. Board drivers and the main loop.

## Status (2026-10-26)

All four steps are written; steps 1-3 are tested on the host, step 4 only builds (`firmware/build`: 790 KB of 896 KB flash).

| Step | State | Evidence |
|---|---|---|
| 1. App core | done | `tests/app/test_app.cpp`: 206 checks (the mockup's suite ported, plus regressions from the code review) |
| 2. Screen | done | `tests/app/render_screens.cpp`: 17 states drawn and compared with Chrome shots of the mockup (`docs/mockup`); the strip rasteriser was fuzzed under ASan/UBSan |
| 3. Sound | done | `tests/app/test_audio.cpp`: 39 checks; `render_song` WAVs: timing within 0.1 ms of the 16th grid with swing, no hanging notes over 60 s, WAVE spectra as expected |
| 4. Board | written, not run | `docs/BRINGUP.md`: what to check first |

Every phase was reviewed by a separate agent and tested by another; their findings are fixed and pinned by tests.

Added after the mockup (2026-10-26): **per-lane MIDI channels, in and out.** PROJECT has a second view (INFO again) with a table of
the lanes: T1 picks the lane, T2 its channel (OFF, 1-16), T3 its note, T4 shows how many lanes share the channel. A lane alone on its
channel plays every note (a sampler lane transposed from its BASE NOTE); lanes sharing a channel (the drums on 10) are each played by
their TRIG NOTE and send it. The part's GLOBAL MIDI CH is now a read-only pointer to this view. Projects are file version 3 (older
files load with the default table). Recording from MIDI is not in (decided: not now). Code: `app/MidiMap`, `audio/Instrument`
(`takeMidi`, `sendOn`), `midi/MidiOut`; tests: `testMidiMap` (app), `testMidi` (audio).

Deliberate differences from the mockup, beyond the hardware ones above:
- AMP VEL>ATK no longer turns red over the voice budget (a leftover from when VOICES sat on the AMP page).
- MIXER view 2: every ladder line in the same type (the mockup's CSS gave the first one the label font by accident).
- A step page without lit steps shows the lane strip under the grid, as the mockup's CSS does (it hides it when the rows are tall).
- The browser shows a sample's length only once it has been loaded from the card (2,500 files are indexed at boot, not read).
