# FX mapping: the original upstream chain -> the Drumboy effects section

Data: `content/data/fxmap.json` (131 records, Drumboy stored units: 0..127, or a list index). Generator: `content/tools/map_fx.py`.
Checks: `content/tools/test_fxmap.py`. Level/RT60 render calibration: `content/tools/fxcal/` (`fx_cal.cpp`, `analyse_cal.py`, `cal_result.json`).

Units. A preset file stores registry plain values 0..127. They are turned to real units with the upstream laws of `sdk/core/src/UnitCurves.cpp`
(kPercent linear, kDepthPct `100*(v/127)^1.737`, kFeedbackPct `110*(v/127)^0.652`, kDelayMs `4000^(v/127)` ms, kRevDecayMs `100*200^(v/127)` ms,
kPreDelayMs 0..250 ms, kChorusRateHz `0.05*160^(v/127)`, kDriveDb 0..36 dB, kLevelDb: `40 log10(v/100)` below 100, linear to +12 dB above),
then to the Drumboy knob by the inverse of `fxReal` (linear or exponential a..b, clamped, round half up). "mix 64" is therefore 50 %, not 64 %.
A row a file leaves out is at the registry default (ParamRegistry.cpp), as the loader does.

## Sends (delay / reverb / chorus)

The original is an insert: `out = dry (1-m) + wet m`. The Drumboy part keeps its whole dry and the shared effect (return 100 % wet) adds
`send * wet`. Rule: keep the original wet-to-dry balance:

    send = clamp( m / (1 - m) * G(type) , 0..1 ) * 127          m = mix / 127

`G` is the wet-level correction of the original effect type, **measured**: the same noise burst through the upstream effect at 100 % mix and
the Drumboy effect at send 1 (reverb with its return trim 1.4), mapped settings, `sqrt(E_nemesis / E_drumboy)`, median over the presets.

| type | G | | type | G | | type | G |
|---|---|---|---|---|---|---|---|
| delay Digital | 0.99 | | reverb Room | 0.71 | | chorus Chorus | 0.58 |
| delay Analog | 0.81 | | reverb Hall | 0.71 | | chorus Ensemble | 0.57 |
| delay Tape | 0.68 | | reverb Plate | 1.33 | | chorus Vibrato | 0.99 |
| delay Ping-pong | 0.61 | | reverb Spring | 0.90 | | | |
| | | | reverb Shimmer | 0.71 | | | |
| | | | reverb Cathedral | 0.46 | | | |

Mixes in the cut reach 50 % at most (stored 64), so `m/(1-m)` reaches 1.0; one send (Folded Chime reverb) saturates at 127 (noted).
Judgement: the ratio rule makes the part about `1/(1-m)` louder than the original (the dry is not ducked); the plain `send = m` would
halve the wet-to-dry balance at 50 % mix. If the user prefers the latter, change `send_from_mix`.

## DELAY (upstream delay -> DELAY view, 7 knobs + the send)

| original | Drumboy | rule |
|---|---|---|
| `sync` (1/4 1/4. 1/8 1/8. 1/8T 1/16 3/16 1/16T Free) | MODE | 1/4 -> 1/4, 1/8 -> 1/8, 1/8. and 3/16 -> 1/8., 1/16 -> 1/16, 1/8T, 1/16T same; **1/4. has no mode -> FREE at 750 ms** (= at 120 BPM, no longer follows tempo; 4 presets) |
| `time` (Free only; `4000^(v/127)` ms, per type clamp Analog 20-600, Tape 50-1500) | TIME 20..1000 ms exp | the played time. For a synced delay TIME holds the same time at 120 BPM so switching to FREE keeps it. Nothing exceeds 1000 ms (max 785), the clamp note exists but never fires |
| `fbk` (0..110 %, power law) | FEEDBACK 0..95 % | the percent, clamped to 95 |
| `tone`, Digital / Ping-pong | TONE 500..12000 Hz | the loop low-pass `1000 * 20^(tone)` Hz |
| `tone`, Tape | TONE | tape AGE: `16000 * (800/16000)^tone` |
| `tone`, Analog (37 of the 65 delays) | TONE | BBD bandwidth `N / (4 D)`, `N = 4096 * 4^(tone - 0.5)`, D the played time (ATLAS: 20 kHz at 50 ms) |
| `width` (default 90 -> 71 %) | SPREAD | the same law: right line at `1 - 0.25 width` of the left. Ping-pong -> 100 % (stands in for the crossed feedback) |
| `delay.mod` (default 29 -> 7.7 %) | MOD | percent |
| (none; upstream loop HP 30 Hz) | HPF | 30 Hz |
| band edges `lo 46 / hi 97` (11 presets: 244 Hz - 3.9 kHz around the delay, `20*1000^(v/127)`) | HPF / TONE | HPF = 244 Hz, TONE = min(tone, 3.9 kHz) |

Not mapped: Analog companding / drift / saturation, Tape heads / wow / saturation (notes), duck.

## REVERB (-> REVERB view)

The Drumboy reverb is one network (upstream's Hall FDN). Type has no equivalent; size and decay are chosen so the tail is similar, and every non-Hall
type gets a note.

| original | Drumboy | rule |
|---|---|---|
| `decay` (100 ms * 200^(v/127)), clamped to the type's band: Room 0.2-1.2, Hall 1-6, Plate 0.5-3, Spring 0.5-3, Shimmer 1.5-8, Cathedral 3-12 s | DECAY 0.2..12 s | the clamped value times a tail factor: Hall / Spring / Shimmer / Cathedral 1.0, **Plate 1.2**, **Room 0.5** (see below) |
| `size` (power law 1.32) | SIZE | Hall and Shimmer: the percent (identical line scale 0.7-1.5). Room 0, Plate 35, Spring 25, Cathedral 100: the types whose geometry the Hall network cannot reach, the nearest end |
| `damp` | DAMP 1..16 kHz | Hall-type law `20000 * (2000/20000)^damp` Hz; Cathedral `20000 * (1200/20000)^damp` (upstream Room / Plate / Spring use a non-monotonic coefficient mapping, ambiguous, not copied) |
| `predly` (0..250 ms) | PRE-DLY | the type's clamp (Room 20, Hall 80, Plate 40, Spring 0, Shimmer 80, Cathedral 30-150 ms) |
| (no upstream row; types differ) | WIDTH | Hall, Plate, Shimmer 100 %; Room 60, Spring 70, Cathedral 70 (atlas correlation) |
| `reverb.mod` (default 36 -> 11 %) | MOD | percent, halved for Cathedral |
| (upstream Hall input HP 30 Hz) | LOW CUT | 30 Hz (Hall, Shimmer, Cathedral), 20 Hz otherwise; a band edge `lo 33` (120 Hz) wins |

Render check (impulse, T20 extrapolated; `analyse_cal.py`): Hall upstream 2.72 s vs Drumboy 2.72 s, Plate 1.39 vs 1.42, Spring 1.08 vs 1.12,
Shimmer 5.68 vs 5.65, Cathedral 7.21 vs 6.70 (the 12 s DECAY limit). Room: the upstream EDC reads 0.11-0.32 s (early reflections) while the atlas
puts its late tail at 0.4-0.9 s; the 0.5 factor lands in between (0.41 s median). Not mapped: Room early reflections (`erlate`), Shimmer octave feedback
(2 presets), Spring boing, Plate / Cathedral character.

## CHORUS (-> CHORUS view)

| original | Drumboy | rule |
|---|---|---|
| `type` Chorus / Ensemble (+ `mode` I / II / I+II) / Vibrato | DELAY | base delay 20 ms (Chorus, Ensemble), 12 ms (Vibrato) |
| `rate` (0.05 * 160^(v/127)) | RATE | Ensemble mode II is x1.68, I+II x1.34 (upstream kModeIIRateMult) |
| `depth` | DEPTH | percent times mode depth (x1.25 / x1.125), scaled so the sweep is the same: upstream swing 8 ms (Vibrato 10) = `depth * delay / 2` |
| `fbk` (0..110 %) | FEEDBACK 0..70 | loop gain `fbk/110 * 0.95` |
| `width` (default 70) | WIDTH | x0.5: upstream offsets the right LFO by `width` quarter-cycles, the Drumboy by `width` half-cycles; Vibrato 0 |
| `tone` | TONE | `800 * 22.5^tone` Hz |
| `hp` (20 * 100^(v/127)) | HPF | Hz; a band edge `lo` raises it |

Not mapped: Ensemble voices (3) and colour, Vibrato's wet-only (the send adds dry + wet; the render shows the Ensemble wet is -5 dB under the
Drumboy's, hence G 0.57), `voices`.

## AMP (insert per part, a "drive" or "gamp" instance)

The FIRST drive or gamp instance is used; the others are noted. No amp/drive in the original: MODEL 0 and the other seven fields at the Drumboy defaults.

| original | Drumboy | rule |
|---|---|---|
| `gamp.model` Clean Crunch Lead Hi-gain Bass Acoustic | MODEL | CLEAN CRUNCH LEAD LEAD BASS CLEAN (Acoustic noted) |
| `drive.type` | MODEL | Saturate / Overdrive -> CRUNCH, Distortion / Fuzz or `drive >= 20 dB` -> LEAD; Bit crush / Sample rate: not mapped |
| `gain`, `drive.drive` (0..36 dB) | DRIVE 0..100 % | dB / 36, times `0.5 + 0.5 mix` when the original is a parallel blend (the Drumboy amp has no MIX) |
| `gamp.bass / mid / treble` (50 % = noon) | BASS / MID / TREBLE | `(p - 50) / 50 * 12` dB. A drive instance: 0 dB; its post low-pass `drive.filter` below 8 kHz -> TREBLE `-4 dB` per octave |
| `presence` | PRESENCE | percent (drive: Drumboy default 40) |
| `gamp.cab` 4x12 2x12 1x12 1x10 8x10 Direct | CAB | 4X12 2X12 1X12 1X12 4X12 OFF. A drive instance has no cab: OFF |
| `level` (kLevelDb) | LEVEL -24..+6 dB | the dB |
| drive band `lo 33` (bass left dry, 3 presets) | BASS | -6 dB (the Drumboy amp is full-band) |

Not mapped: mic (fixed dynamic), the Drumboy amp is mono out. One preset has a drive then a gamp: the drive is used.

## Not mapped (listed in `notes`)

comp (16 presets, the master COMP is global: its type / threshold are noted), eq (8), phaser (6), schain (3), vintage (3), plus per-effect details above.
Chain order is lost: 23 presets run the delay into the reverb in series, the sends are parallel (noted).

## Could not be verified

No audio was heard. The send gains and the reverb RT60 are measured by rendering the two implementations (noise burst, same input), not by ear. The amp /
drive mapping is by the stated rules only (upstream DriveEffect pre-gain `1 + range * amount` is not the amp's `10^(dB/20)`; the loudness of the amp stage was not rendered).
