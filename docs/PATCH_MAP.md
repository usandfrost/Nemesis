# The panel's sound pages on CATHODE

What each knob of a synth part's nine pages drives in the CATHODE engine (`firmware/src/audio/PatchMap.cpp`). Values are the panel's
0..127 (or their list index) unless said otherwise. A sampler part's pages are the mockup's `playSample`, in `audio/Sampler.cpp`.

Every voice keeps what it was last given: a change reaches the engine as the setters that differ. A p-locked step's values go to the
voice that plays that note (all voices of its unison stack) and stay until the voice's next note; when a knob moves meanwhile, that
voice gets the note's locks over the part's new pages, so its locked values stay. The CPU governor's bypasses (cost/Degrade.h) are
applied on top, the same way, to every voice.

| Page | Knob | CATHODE | Notes |
|---|---|---|---|
| OSC 1/2 | WAVE | SIN: phase-distortion oscillator (PD type Saw). TRI, SAW, SQR: wavetable mode on a frame of CATHODE's own saw/square/triangle/sine table | measured: SIN pure, TRI odd harmonics 1/n², SAW all 1/n, SQR odd 1/n |
| | PD SHAPE, PD AMT | `setPdShape`, the PD amount's modulation base | only with WAVE = SIN (as the mockup's picture); PD AMT 0 = a clean sine |
| | OCT, SEMI | `setOctave`, `setSemitones` | |
| | FINE | `setFine` (cents) + GLOBAL TUNE | |
| | LEVEL | `setLevel` per slot | OSC 2 LEVEL 0 = slot 2 silent (and skipped by the CPU) |
| | (slot 8, retired) | was OSC 1 UNI DET | the panel no longer shows it; a file that has a value there is migrated on load (`migratePages`): SUB UNI DET takes the larger of the two, as the voice always did |
| | OSC 2 SYNC | the sync amount's modulation base; osc 1 is hard-synced to osc 2 (osc 2 is the master and sets the heard pitch) | |
| SUB / UNISON | SUB LVL, SUB OCT | `setSubLevel`, `setSubOctave` (-1, -2) | the sub is a sine |
| | NOISE | the noise level's modulation base | |
| | RING | ring modulation depth | bypassed by the degrade step "cross-mod off" |
| | UNISON | stack size of slot 1 (1..4) | bypassed by "unison off" |
| | UNI DET, UNI BLND | slot 1 detune (see OSC 1), blend | |
| | WIDTH | stereo width of the stacks (only with UNISON > 1) | bypassed by "unison width off" |
| FILTER 1 | MODEL | LP, BP, HP, NOTCH, LADDER (24 dB), ACID, SOVIET (LP), CLASSIC (LP 24) | one heavy model per lane on the CATHODE engine, none on the lean voice (cost model); "heavy filter to light" swaps a heavy model for its light twin |
| | CUTOFF, RESO | the cutoff's base: 30 Hz .. 16 kHz, Q 0.7 .. ~16 | |
| | DRIVE | filter drive | bypassed by "filter drive off" |
| | KEYTRK | KeyTrack → cutoff route, full = 0.8 octave per octave | |
| | ENV AMT | Env2 → cutoff, scaled to the mockup's sweep (peak = cutoff × (1 + 6 × amount)) | Env2: 1 ms attack, decay = AMP DECAY × 0.8 + 20 ms, no sustain, release = AMP RELEASE |
| | HPF | the series high-pass at cutoff(v) / 8; 0 = off | |
| | MIX F1/F2 | filter balance: 0 = both filters at full level, 127 = filter 2 alone | CATHODE's balance is a mixer, not a crossfade; "filter 1 alone" is FILTER 2 ACTIVE off |
| FILTER 2 | MODEL, CUTOFF, RESO, DRIVE | filter 2's model, cutoff/Q, drive | CUTOFF and RESO go to the modulated voice's F2 bases (`setF2Cutoff`, `setF2Reso`); until 2026-10-27 they were written to the voice stage, which the voice then overwrote with its defaults (1360 Hz, Q 1), so only the model, route, on/off and DRIVE had any effect. F2 follows F1's cutoff modulation (key tracking, envelope, LFO, mod wheel, aftertouch) from its own base |
| | ROUTE | series / parallel | |
| | ACTIVE | filter 2 on/off | bypassed by "filter 2 off" |
| | KEYTRK, ENV AMT | **not mapped**: CATHODE's filter 2 has no key tracking or envelope of its own | the knobs stay on the page, as in the mockup |
| GRIT | BITS, RATE, DRIFT, LOOP ERR | the converter: bit depth, rate 1 .. 44 kHz, drift, loop error | |
| | COMPAND | μ-law companding | bypassed by "companding off" |
| | WOW | **not mapped** (no CATHODE counterpart; DRIFT is the closest) | |
| | CLIP | the output stage's drive | |
| | MIX | grit amount (dry/converted) | |
| AMP ENV | ATTACK, DECAY, SUSTAIN, RELEASE | the amp envelope (2 ms .. 2.5 s) | |
| | VEL SENS | velocity sensitivity | |
| | LEVEL, PAN | the level and pan bases | |
| | VEL>ATK | per note: attack × (1 − amount × velocity × 0.9) | as the mockup |
| MOD | LFO RATE, SHAPE | LFO 1: 0.2 .. 12.2 Hz; sine, triangle, saw, square | |
| | LFO DEPTH, DEST | LFO 1 → cutoff (±1 oct), pitch (±0.9 semitone), amp (±0.5), PD amount | matrix slot 1 |
| | ENV>PD | Env2 → PD amount | slot 2 |
| | ENV AMT+ (was ENV>CUT) | added to FILTER 1's ENV AMT route (up to 2 more octaves). Kept as a knob: 28 library presets run ENV AMT at its top and use this for the rest of the sweep, so it cannot be folded into ENV AMT | |
| | MOD WHL | mod wheel → cutoff, up to 2 octaves | slot 3 |
| | AFTERT | aftertouch → cutoff, up to 2 octaves | slot 4 |
| GLOBAL | MASTER | the part's output gain | |
| | BEND RNG | the engine's bend range | |
| | TUNE | added to both oscillators' FINE | |
| | MIDI CH, CLOCK, PREVIEW | MIDI CH: read-only, the lanes have the channels (PROJECT > MIDI); CLOCK, PREVIEW: the instrument | CLOCK: internal only so far |

Levels: one CATHODE voice at full settings peaks about where the mockup's synth voice did (0.15 of full scale after the master); the
mix runs through a soft limiter, as the mockup's master ran through a compressor.
