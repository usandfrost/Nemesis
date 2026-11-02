# Bring-up: what to check first on the board

Nothing in the panel firmware has run on a Drumboy yet. The app core, the screen, the sequencer, the CATHODE parts and the sampler are
tested on the host (`tests/`: `app_test`, `audio_test`, `render_screens`, `render_song`); the board layer (`firmware/src/board/`) is
written against the stock firmware's drivers and pin names but unproven. ## Installing without an ST-Link

The stock bootloader (flash sector 0, never touched by us) installs firmware from the SD card at every power-on:
1. `python3 tools/make_sd_firmware.py firmware/build/dboy_cathode.bin Drumboy_Pro_Fw_CATHODE.bin` (the packer reproduces the stock
   package byte for byte, so the format is the bootloader's).
2. Copy that file to the card's **root**, power on. The bootloader checks the CRC, flashes, re-reads the flash against the CRC, and
   empties `System/Firmware` and moves the file there (so it installs once, and the file that was there is gone). A name over 31
   characters is skipped with nothing on screen: the bootloader's FatFs has `_MAX_LFN` 32 and then sees only the short name.
3. **Back to stock:** copy the stock package (`ref/02 Software/Firmware/Drumboy_Pro_Fw_01_01.bin`) to the root and power on. This works
   even if our firmware hangs at start-up, because the bootloader runs first.

This path was tried on 2026-10-31 with a release build (`-DDBOY_DEBUG_HOOK=OFF`): about 20 s, and the flash read back identical to the image. `tools/make_install_zip.sh` builds what a user gets, and `docs/INSTALL.txt` is what they are told.

Without a debugger, the only signs are the three LEDs (LED2 blinks: the main loop runs; LED0: an audio block over budget; LED1: an I2S
error) and the screen. A hang before the screen comes up needs the ST-Link to find.

## Driving the board from the computer (no one at the buttons)

With an ST-Link on the SWD header, `tools/board_script.py` presses keys, turns knobs, reads what the firmware is doing and saves a PNG of the
screen, from a script. The firmware side is a mailbox in RAM (`src/board/DebugHook.h`, CMake option `DBOY_DEBUG_HOOK`, on while bringing up): the
script writes events, the main loop feeds them to the same `Ui::keyDown/keyUp/layerDown/layerUp/turn` that `Input::poll` calls, and publishes mode,
part, playing, the audio load and the degrade level; a screen strip is rasterised on request from the last frame drawn. Nothing listens on any port.

    tools/board_script.py tools/board/smoke.txt                  # boots, answers PLAY, fits its audio budget, two screenshots
    tools/board_script.py -c "key PLAY; wait 3; status; snap play.png"

It needs OpenOCD (the copy in STM32CubeIDE is found; it is started and stopped by the script) and the ELF of the build on the board (`board::debugMail`
is found there). Flash with STM32CubeProgrammer first, with the CPU held in reset (`mode=UR reset=HWrst`); a hot-plug erase failed on the first try.
`board_script.py -h` lists the commands. Limits: the screenshot is taken strip by strip, so a frame that changes while it is read is torn;
it shows the display list the main loop last drew, not the panel's pixels.

## Checks

Check in this order; each step names what to change if it fails.

1. **Boot and audio.** The heartbeat LED (LED2) blinks; LED0 lights on a block over budget, LED1 on an I2S error. Press PLAY: the default
   project plays. If silent: `Dac::initialize`, then `Instrument::init` (it allocates ~5 MB of SDRAM: `memory/SdramHeap`).
2. **Display.** `board/Lcd.cpp` runs the stock NT35510 start-up sequence (`Nt35510Init.h`, copied from the stock `Lcd::initialize`)
   and the stock landscape mode (MADCTL 0xA0). Upside down or mirrored: change the MADCTL value in `Lcd::init`. Garbled colours: the
   pixel format command (0x3A00 = 0x55, 16 bit). `Lcd::lastPresentUs` and `tilesSent` say how long a frame took.
3. **Keys.** Each key should do what its stock legend's function is mapped to (`board/Input.cpp`, `kLeftKeys`, `kRightKeys`; the
   stock enums give the bit order). Note: the left 3x3 keeps the stock order: its top row prints **● + ○** (ADD, UP, CLEAR) and its bottom row **C − P** (− is
   DOWN), so + − ◁ ▷ are a cross around OK. `web/test/site.mjs` holds the web panel to this table. The bottom-left key (stock SONG) is unassigned.
4. **Encoders.** `Input::kKnobOfEncoder` (which encoder is T1..B4), `kDirection` (+1/-1 per knob) and `kEventsPerDetent` (2; the
   stock firmware uses 2 to 6 per step) are guesses until a knob is turned.
5. **LEDs.** Layer LEDs: selected lane lit, flicker on hits; in STEP the lit steps, the cursor blinking. The level bars follow the
   output.
   **Boot image.** `Lcd::showBootImage` (from `main`, straight after `Lcd::init`) draws the picture in flash (`gfx/BootImage.*`, data from `tools/gen_boot_image.py`, source
   `content/art/Nemesis.png`). It has not been seen on a board. Expect the picture to wipe down the panel in a few hundredths of a second, right way up, and to stay at least 2 s
   (`kBootImageMs`), or until the first key or knob or a toast (a warning must not expire behind it). Nothing waits for it: the card, the audio and the keys start behind it.
   `Lcd::lastPresentUs` holds the time it took until the first frame replaces it. A script's `snap` during those 2 s shows black: `tools/board_script.py` keys go straight to the UI, so they
   do not end the picture, and the display list is not drawn until it ends. Wait 2 s after a reset before a `snap`.
6. **SD card.** With the stock card: `/Sample/<folder>/*.wav` is indexed at boot (2,500 files) and the boot project's drum lanes pick
   the first KICK, SNARE and HHCLOSED. Projects go to `/CATHODE/PROJn.DBP`. If the card does not mount: `Card::mount` (detect pin PC13
   low = card in; `hsd2` with the stock ClockDiv 32).
7. **CPU.** MIXER view 2 shows the measured load. Re-run the cost benchmark on the board (`docs/CATHODE_COST.md`) and regenerate
   `cost/CostTable.h`; then `tools/gen_itcm.py` for the new hot code (the instrument, the sampler) and rebuild.
8. **Known build warning to look at:** GCC reports array-bounds warnings inside the CATHODE engine's spread code
   (`CathodeVoiceEngine::appliedOff`, `centreVoice`) with the 8-voice pool. They look like GCC not seeing the loop bound; confirm with a
   run where the voice count is at its maximum.

9. **SD transfers.** The SD driver copy (`platform/FatFs/Target/sd_diskio.c`) was changed from stock: unaligned buffers go through its
   aligned scratch block (the SDMMC's DMA ignores address bits 1:0, which would shift every 24-bit WAV), the timeout is 1 s instead of
   30, and long file names are kept up to 255 characters (`ffconf.h`). With the D-cache on (`DBOY_DCACHE`, the default) unaligned writes must clean the scratch block before the DMA reads it, which stock does not do: FatFs's own buffers are unaligned, so without that every directory and table write loses its data. Its cache
   maintenance must be turned on too, with 32-byte alignment.
10. **MIDI.** In on USART1 (DIN in), out on USART6 (DIN out, `midi/MidiOut.cpp`: an interrupt-driven byte ring, the stock pin and
   mode). Each lane has a channel and a note (PROJECT, INFO again for view 2; default lanes 1-7 on channels 1-7, the drum lanes 8-10
   sharing channel 10 on notes 36/38/42). In: a note on a lane's channel plays that lane (a shared channel: the lane whose note it
   is); a channel no lane uses plays the part on screen. Out: every note a lane plays goes out on its channel, with its note-off; STOP
   sends the note-offs of everything still held. To check: a keyboard on channel 1 plays lane 1's part; a drum machine on 10 plays the
   kick on 36; the sequencer's notes reach a synth on the out socket. If nothing goes out: `huart6.TxISR` must be the hook
   (`midi::startOut`), and USART6's interrupt must be enabled (`usart.c`, priority 2). `midi::outStats.dropped` counts messages that
   did not fit the 1 KB ring. MIDI in is now taken by the audio interrupt at the start of each block (no main-loop delay).

11. **Effects.** The FX screen is the **bottom-left key of the left keypad** (the stock legend there says SONG; SONG is mapped to the right
   keypad here). Press it again, or use the up/down keys, for DELAY, REVERB, CHORUS, COMP, AMP, SENDS. Sends are 0 and the amp and compressor
   off in a new project: the sound is as without effects, and the effects section does not run at all (bit for bit the old summing).
   To check: turn up a part's REVERB send in SENDS and hear its tail; DELAY in a synced mode follows the tempo (PLAY, change BPM);
   the AMP on a bass part; COMP ON on the drums. On the board only: the CPU cost (MIXER view 2 CPU LOAD with each effect on; the shared
   effects are estimated at ~12% of a block, each amp 11-17%: docs/FX_ANALYSIS.md), the D-cache (`DBOY_DCACHE`, off today: the reverb and the
   delay lines make many SDRAM reads per sample), and that loading a project clears the old tails. The shared effects count in the MIXER's cost
   (an EFFECTS line on view 2; each amp is added to its part's line) and are the first things the degrade ladder gives up: reverb, delay, compressor,
   chorus off in that order, then an amp goes clean and off (a bypass, the settings stay). Not p-lockable yet.

12. **Presets and wavetables.** Copy the folder `tools/make_sd_content.py` makes (the firmware image at the card's root, `CATHODE/WT/wtNNN.wtp`).
   BROWSE (FILE key) lists 200 presets in 12 categories; the preview plays three notes, and a preset that plays a wavetable bank loads it first
   (tens of ms the first time). Check: the OSC 1 page, WAVE past SQR: bank names, POSITION on the second knob; a bank that is not on the card
   shows red with a "?". Check six banks at once and a seventh: the message WAVETABLE MEMORY FULL, no crash. See
   docs/PRESETS_AND_WAVETABLES.md for memory and what is unverified (load time, the uncached SDRAM reads).

13. **Preset effects.** Load a library preset in BROWSE (OK): the FX screen (bottom-left key, SENDS view) shows the part's sends and AMP shows its amp; the
   preview and the loaded part both play through them. Check the CPU load (MIXER view 2): a preset with reverb + delay + chorus + amp is the heaviest case; the
   voices are trimmed to fit and the degrade ladder gives the effects up first (docs/FX_ANALYSIS.md). Undo (SHIFT + ○) right after a load brings the old effects back.
14b. **FX locks.** Play a pattern with a reverb or delay that has a send of 0 on the part. Hold a step until it says its lock is open, press the FX key (SENDS), turn REVERB to full: that step alone throws the part into the reverb and the tail rings on after it. Then AMP (with an amp on): lock DRIVE on a step. Listen for clicks at the step edges (the send is smoothed, the amp knobs glide). STOP in the middle of a locked step: the send must not stay up. Watch the CPU load on a step that wakes an effect.
14. **Preset files.** Boot with the card from `tools/make_sd_content.py` (197 files in /CATHODE/PRESETS, no INDEX.PIX): BROWSE should list 200 presets. Time the
   boot with and without INDEX.PIX (a start after a change reads the 197 files, the next save writes the index, and the start after that should read only the folder listing and the index).
   Power-cycle once more and check that INDEX.PIX is not rewritten (its date stays). Scroll BROWSE and preview a preset: the first preview of a preset reads its file
   (should be a few ms; the audio must not glitch). Copy a new `.PRS` (tools/prs.py new) into the folder, restart: it appears last in its category. Put a damaged
   file there (change one byte): the boot toast says "1 PRESET FILE IGNORED". Pull the card while BROWSE is open and press OK: PRESET FILE CANNOT BE READ.
   **Saving:** load a library preset, go to a sound page, change the cutoff, SHIFT + OK, edit the name, OK: the toast says SAVED, the file is in /CATHODE/PRESETS, BROWSE lists it, and
   after a restart it is still there with no rebuild (INDEX.PIX date changed at the save, not at boot). Time the save (the screen says SAVING PRESET... while the index is written, 25 small card writes for 197 presets; if it takes more than a second, write only the changed entry).
   SHIFT + OK again, knob 4 to REPLACE, OK twice: the file is rewritten. Try a card with its write-protect on: THE CARD COULD NOT BE WRITTEN and nothing changes.

15. **Sample browser** (`docs/SAMPLE_BROWSER.md`). With the stock card, on a sampler lane: hold SHIFT and turn B1 through every folder (the order is the card's own, after the built-in six; report if it is not usable), let go and turn B1. Spin it fast with the pattern playing: the knob must feel free (a preview reads the card, so a spin is heard at its first detent and where it stops) and the audio must not glitch. Check on the hardware that SHIFT and a knob are comfortable together with one or two hands. Long names: the cell cuts them, the toast must show them whole. In STEP 2/3 give three steps a sample in another folder, undo the last, save, restart, load: the locks are there. REC, keep a take of 15 s, restart: on a sample knob its name and TOO LONG TO PREVIEW show, and it plays on a lane. With a folder `Sample/Rec` on the card: a take goes in there, and a project saved with it finds it after a restart.

16. **Tracker** (`docs/TRACKER.md`). Press STEP three times. Everything the host cannot tell:
   - **The bus.** Play a pattern at 240 BPM on the tracker and watch `Lcd::lastPresentUs` and `tilesSent`. The host counts 60 tiles a step (94 on PLAY); check the time for those, and for a frame with a popover, is well under the 62 ms a step has. The playhead must keep up and the audio must not glitch. If the row is too much, the row number alone would be 4 tiles.
   - **Reading it.** 16 px text in 90 px columns on the real panel at arm's length: are `C-4 100`, a sharp, and the dimmed repeats readable? Can the lock and chord marks be told apart?
   - **Hands.** Hold OK and press the arrows around it, on the left cluster, with one hand and with two. The keys are on port expanders, not a matrix, so any two should register: check that none is lost. A slow OK tap must still count, and the note must not move when letting go of OK just after an arrow.
   - **Knobs.** Spin B1 fast past the limit: the note stops at the limit and the popover says so. B2 at +15: nothing moves and it says STOPPED. A sampler lane: SHIFT + B3 with one hand or two, as in check 15.
   - **While it plays.** Enter notes with the pattern running, on a lane the playhead is on: the edit must be heard on the next pass with no click, and a chord must be whole. Undo a step while it is playing.
   - **Two pages.** A 32-step lane: step through page 2 while the playhead is on page 1, and turn a page (458 tiles, the one big redraw): see how long it takes.
   - **LEDs.** In the tracker the layer keys show lanes, not steps.
   - **Old projects.** A project saved before this build opens in the tracker unchanged. Save it, reload it: the same.
20. **Clock and sync.** Nothing here has met a real cable. GLOBAL CLOCK at MIDI with a sequencer sending clock: does PLAY wait, do START and STOP work, does the tempo on screen settle? At INT, does another box follow the clock Nemesis sends? SYNC out: is the pulse (5 ms, two to a quarter) what a Volca or a Pocket Operator wants, or does it need another level or length? SYNC in: `SYNC_IN_PULSE` is read at the start of each audio block as high; check the pulse really reads high on the pin, and that the jack's detect does not get in the way. A sustain pedal on a keyboard through MIDI in.
19. **Exporting** (`docs/EXPORT.md`), on a spare card. PROJECT, INFO twice, then load F2 first. SONG WAV of F2: time it, and open the file on a computer. How many seconds of song does a second of work make, and what is the speed of the card? STEMS: do they line up against the mix in an editor? MIDI FILE: open it in a sequencer. STOP halfway leaves no file. Does the audio come back cleanly afterwards, and is the board's load normal?
18. **Starting empty, and the factory projects.** Boot: nothing plays, and PLAY says why. On PROJECT, B1 walks F1 F2 F3 and on to slot 30. Load F2 and F3, play each song to its end and watch the load and the overruns: both use three synth voices with up to seven sample voices. Time how long PROJECT takes to open the first time with many slots full: thirty slots are read once. A project saved to slot 10 or above must show as `PROJ10.DBP` on a computer. Keys, levels and locks are judged by ear: the host render only proves they play and differ.
17. **Deleting a project.** On PROJECT, save to a free slot and press ○: the row asks for four seconds and nothing else has changed. Press ○ again: the slot is empty, and the card shows no `PROJn.DBP` or `PROJn.BAK` for it on a computer. SHIFT + ○ puts it back and it loads. How long is the wait on a large project, and does the audio carry on through it?

Decided (2026-10-26):
- **A project remembers its samples by path.** Project files are version 2: after the project comes a table of the samples it uses,
  by their path on the card ("Kick/Altair_1.wav", or "@Kick 808" for a built-in one). On load each is looked up on the card as it
  is now and every reference is renumbered; samples the card no longer has are counted ("PROJECT 3 LOADED · 2 SAMPLES MISSING") and
  their lanes play sample 0. Version 1 files still load, with their numbers as they were.
- **A full sample cache evicts what the project no longer uses.** The samplers fade out for a moment (their voices read memory that
  moves; the synth parts play on), the samples still used move down to close the gaps, and the new one loads. If it still does not
  fit, the screen says SAMPLE MEMORY FULL and that sample is not tried again until the next project load.

What the host cannot tell: real timing of the I2C reads and the display bus, the SD card's speed (a project save is ~230 KB; the
screen does not update while it is written), encoder feel, and the MIDI sockets themselves (the routing is tested on the host).
