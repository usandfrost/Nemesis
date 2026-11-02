# Nemesis

Free firmware for the Randomwaves Drumboy Pro, with a web version that runs the same code in a browser. Beta 2.

Nemesis is an independent project, not made or supported by Randomwaves.

It plays up to seven parts of CATHODE synth voices, sampler lanes and drums, with a step sequencer, a tracker, song mode, line-in recording, slicing, and export of a song to WAV, stems and MIDI.

## Install

Follow `docs/INSTALL.txt`. You need the release package and your Drumboy's SD card. Keep a backup of the card: the installer deletes the original firmware file from it.

## Build and check

`tools/check_all.sh` runs the host tests, the website tests in headless Chrome, and the ARM build. The firmware needs the Arm GNU toolchain (`ARM_GNU_ROOT`) and the web version needs Emscripten. See `docs/BRINGUP.md` for flashing and for what only the board can prove.

## Licence

GPL 3 or later: see `LICENSE`. Parts by other people keep their own licences: see `THIRD_PARTY_NOTICES.md`. There is no warranty of any kind.
