# Delerium, batch 1: fifty patches (ids 650-699; only ever append, a patch's position is its id). tools/genre_presets.py builds them (--set delerium).
# Source: the upstream project's notes on Delerium (ref/upstream/docs/dev/sound-design/REFERENCE-RECIPES.md 1.12 and PAD-3 "Church Choir Breath"): a real choir recorded in a
# church (here the choir banks, with a slow attack, a little noise for the air and a very large, dark reverb), long cathedral reverbs, breathy vocal pads, slow-filtered
# analogue-style pads, ethnic percussion and world instruments, and the trance-pop side ("Silence" in its remix: a gated pluck arp and a pumping pad at about 135 BPM;
# "Euphoria (Firefly)": downtempo at about 100 BPM). Recreations from those notes, not studio settings.
# Engine facts (docs/PATCH_MAP.md): the filter envelope decays with the amp decay and has no sustain; FINE is the detune; UNISON sits on the SUB page and acts on OSC 1; no
# glide, no pitch envelope; the LFO is free-running, its slowest rate is 0.2 Hz, LFO -> pitch stays under a semitone (vibrato, not sirens) and the LFO PD destination moves
# one table position for both oscillators; chord and pad patches keep voices at 3 with WIDTH 0 so the CPU budget does not trim them (the chorus makes them wide). Percussion
# is a filter ping on a sine or noise for a sequencer lane (the engine has no pitch envelope: a drum is a ring, not a thump). OSC 2's pitch is always written out.
# Levels start at the top (AMP LEVEL 127) and content/tools/calibrate_levels.py lowers the loud ones to the library's level.

BASE_ID = 650

PATCHES = [
    # ================= choirs and voices (9): a slow attack, a little air, a huge reverb
    patch("Cathedral Choir", "PAD", voices=3, banks=(48, None),         # 48 = CHOIR WIDE
          OSC1=ob(50, level=100), OSC2=o2(SAW, 0, 6, 15), SUB=dict(NOISE=pct(4)),
          FLT1=filt(LP, 4500, 6, 0, 45, 0), AMP=adsr(700, 800, 95, 2500, vel=30),
          sends=dict(reverb=70, chorus=30), reverb=rev(95, 6.5, 4500, 45), chorus=cho(0.35, 40)),
    patch("Latin Choir Men", "PAD", voices=3, banks=(49, None),         # 49 = CHOIR MEN
          OSC1=ob(45, level=100), OSC2=o2(SAW, 0, 5, 10, OCT=-1),
          FLT1=filt(LP, 3200, 6, 0, 45, 0), AMP=adsr(600, 800, 95, 2200, vel=30, level=60),
          sends=dict(reverb=70, chorus=25), reverb=rev(92, 6.0, 4200, 45), chorus=cho(0.3, 35)),
    patch("Angel Choir High", "PAD", voices=3, banks=(50, None),        # 50 = CHOIR HIGH
          OSC1=ob(50, level=100), OSC2=o2(SIN, 0, 0, 15, OCT=1),
          FLT1=filt(LP, 7500, 5, 0, 55, 0), AMP=adsr(800, 800, 95, 2500, vel=30),
          sends=dict(delay=15, reverb=70), delay=dly(DOT8, 35, 5000), reverb=rev(95, 6.5, 6000, 45)),
    patch("Monk Choir Fifth", "PAD", voices=3, banks=(49, 49),          # 49 = CHOIR MEN on both, a fifth apart: a chant drone
          OSC1=ob(40, level=100), OSC2=ob2(40, 7, 4, 70),
          FLT1=filt(LP, 2800, 6, 0, 35, 0), AMP=adsr(900, 800, 100, 2400, vel=20, level=49),
          sends=dict(reverb=70, chorus=25), reverb=rev(94, 6.4, 4000, 45), chorus=cho(0.3, 35)),
    patch("Whisper Voices", "PAD", voices=3, banks=(45, None),          # 45 = WHISPER, mostly air
          OSC1=ob(50, level=100), SUB=dict(NOISE=pct(20)),
          FLT1=filt(LP, 3500, 14, 0, 45, 0), AMP=adsr(900, 800, 95, 2500, vel=30),
          sends=dict(delay=20, reverb=65), delay=dly(DOT8, 40, 4500), reverb=rev(92, 6.0, 5000, 40)),
    patch("Vowel Drift Choir", "PAD", voices=3, banks=(31, None),       # 31 = FORMANT: a slow LFO moves the table through the vowels
          OSC1=ob(50, level=100), OSC2=o2(SAW, 0, 6, 15),
          FLT1=filt(LP, 4500, 8, 0, 45, 0), AMP=adsr(700, 800, 95, 2300, vel=30, level=71),
          MOD=dict(LFO_RATE=lfo_hz(0.2), LFO_DEPTH=pct(40), LFO_DEST=LFO_PD, LFO_SHAPE=TRI),
          sends=dict(reverb=65, chorus=35), reverb=rev(90, 5.8, 4500, 40), chorus=cho(0.35, 40)),
    patch("Semantic Choir", "PAD", voices=3, banks=(32, 48),            # 32 = CHOIR, 48 = CHOIR WIDE, detuned against each other
          OSC1=ob(40, -6, 100), OSC2=ob2(40, 0, 8, 90),
          FLT1=filt(LP, 4800, 6, 0, 45, 0), AMP=adsr(750, 800, 95, 2400, vel=30, level=78),
          sends=dict(reverb=70, chorus=40), reverb=rev(94, 6.4, 4800, 45), chorus=cho(0.3, 45)),
    patch("Throat Chant", "TEXTURE", voices=2, banks=(43, None),        # 43 = THROAT, an octave down
          OSC1=ob(45, level=100, OCT=-1), SUB=dict(NOISE=pct(4)),
          FLT1=filt(LP, 1900, 10, 0, 35, 0), AMP=adsr(1000, 800, 100, 2000, vel=20, level=90),
          MOD=dict(LFO_RATE=lfo_hz(0.3), LFO_DEPTH=pct(30), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(reverb=60), reverb=rev(90, 5.5, 3500, 40)),
    patch("Choir Chop Arp", "SEQ", voices=2, banks=(32, None),          # 32 = CHOIR, short: a vocal-chop arp for the lane
          OSC1=ob(50, level=100),
          FLT1=filt(LP, 4500, 10, 0, 60, 40), AMP=adsr(2, 190, 0, 160),
          sends=dict(delay=40, reverb=45, chorus=20), delay=dly(DOT8, 45, 4000), reverb=rev(85, 4.2, 4500, 30), chorus=cho(0.4, 25)),

    # ================= ambient pads (9): slow filters, chorus, dark long reverb
    patch("Resonant Sweep Pad", "PAD", voices=3,             # a resonant ladder filter swept slowly by the LFO
          OSC1=o1(SAW, -7, 90), OSC2=o2(SAW, 0, 8, 80),
          FLT1=filt(LADDER, 1100, 55, 0, 40, 0), GRIT=dict(CLIP=pct(10)), GLOB=dict(MASTER=127), AMP=adsr(900, 800, 100, 2000, vel=30, level=55),
          MOD=dict(LFO_RATE=lfo_hz(0.2), LFO_DEPTH=pct(70), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(reverb=55, chorus=50), reverb=rev(88, 5.2, 4000, 40), chorus=cho(0.4, 45)),
    patch("Firefly Pad", "PAD", voices=3, banks=(18, None),             # 18 = SPREAD 5: Euphoria (Firefly), a downtempo pad, wide
          OSC1=ob(30, level=100), SUB=dict(SUB_LVL=pct(15), SUB_OCT=-1),
          FLT1=filt(LP, 3000, 10, 0, 45, 0), AMP=adsr(500, 700, 90, 1600, vel=30),
          sends=dict(delay=25, reverb=55, chorus=65), delay=dly(DOT8, 40, 4000), reverb=rev(85, 5.0, 4500, 35), chorus=cho(0.5, 55)),
    patch("Ring Air Pad", "PAD", voices=3, banks=(70, None),            # 70 = RING WIDE
          OSC1=ob(50, level=100), OSC2=o2(SIN, 0, 0, 20, OCT=1),
          FLT1=filt(LP, 6500, 6, 0, 55, 0), AMP=adsr(700, 800, 95, 2200, vel=30),
          sends=dict(delay=20, reverb=65), delay=dly(DOT8, 35, 5000), reverb=rev(92, 5.8, 6000, 40)),
    patch("Distant Thunder", "TEXTURE", voices=2,
          OSC1=dict(LEVEL=0), SUB=dict(NOISE=pct(100)),
          FLT1=filt(LP, 220, 25, 0, 0, 0), AMP=adsr(1500, 800, 100, 2500, vel=0, level=127),
          MOD=dict(LFO_RATE=lfo_hz(0.2), LFO_DEPTH=pct(60), LFO_DEST=LFO_AMP, LFO_SHAPE=TRI),
          sends=dict(reverb=50), reverb=rev(90, 5.5, 2500, 30, 40)),
    patch("Church Organ Pad", "PAD", voices=3, banks=(95, None),        # 95 = ORGAN TIBIA: the organ of the church the choir was recorded in
          OSC1=ob(45, level=100),
          FLT1=filt(LP, 4000, 4, 0, 40, 0), AMP=adsr(350, 600, 100, 1500, vel=20, level=42),
          sends=dict(reverb=70, chorus=20), reverb=rev(95, 6.5, 4500, 45), chorus=cho(0.3, 25)),
    patch("Soft Sine Pad", "PAD", voices=3,                  # almost pure: sines and a triangle, a touch of drift
          OSC1=o1(SIN, level=100), OSC2=o2(TRI, 0, 4, 40, OCT=1), SUB=dict(SUB_LVL=pct(15), SUB_OCT=-1),
          FLT1=filt(LP, 3000, 4, 0, 45, 0), GRIT=dict(DRIFT=pct(30)), AMP=adsr(800, 800, 100, 2000, vel=20, level=41),
          sends=dict(reverb=60, chorus=45), reverb=rev(90, 5.5, 4500, 40), chorus=cho(0.3, 45)),
    patch("Hollow Cloud Pad", "PAD", voices=3, banks=(101, None),       # 101 = HOLLOW
          OSC1=ob(50, level=100), OSC2=o2(SAW, 0, 6, 25),
          FLT1=filt(LP, 3500, 6, 0, 45, 0), AMP=adsr(800, 800, 95, 2200, vel=30, level=40),
          sends=dict(reverb=65, chorus=35), reverb=rev(92, 6.0, 4500, 40), chorus=cho(0.3, 40)),
    patch("Silk Wash Pad", "PAD", voices=3,
          OSC1=o1(SAW, level=100), SUB=dict(UNISON=4, UNI_DET=pct(35), UNI_BLND=pct(70), WIDTH=pct(0)),
          FLT1=filt(LADDER, 2400, 18, 0, 45, 0), GRIT=dict(CLIP=pct(10)), GLOB=dict(MASTER=127), AMP=adsr(1000, 800, 100, 2200, vel=30),
          sends=dict(reverb=60, chorus=60), reverb=rev(90, 5.5, 4500, 40), chorus=cho(0.4, 50)),
    patch("Himalayan Drone", "TEXTURE", voices=2,            # a held saw with its fifth and a low sub: it only ever breathes
          OSC1=o1(SAW, -5, 100), OSC2=o2(SAW, 7, 5, 55), SUB=dict(SUB_LVL=pct(40), SUB_OCT=-1),
          FLT1=filt(LP, 900, 20, 0, 30, 0), AMP=adsr(1500, 800, 100, 2500, vel=10, level=80),
          MOD=dict(LFO_RATE=lfo_hz(0.2), LFO_DEPTH=pct(45), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(reverb=55), reverb=rev(90, 5.5, 3000, 40, 80)),

    # ================= world instruments (9)
    patch("Tibetan Horn", "LEAD", voices=1,                  # a long horn: a low saw behind a closed filter, slow in and out
          OSC1=o1(SAW, level=100), OSC2=o2(SAW, 0, 6, 60),
          FLT1=filt(LP, 800, 22, 0, 45, 15), AMP=adsr(400, 600, 100, 900, level=28),
          MOD=dict(LFO_RATE=lfo_hz(4.2), LFO_DEPTH=pct(12), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=20, reverb=60), delay=dly(DOT8, 35, 3500), reverb=rev(90, 5.0, 3500, 40, 80)),
    patch("Didgeridoo", "TEXTURE", voices=1,                 # a drone with a rhythmic formant wobble
          OSC1=o1(SAW, level=100), OSC2=o2(SQR, 0, 0, 40),
          FLT1=filt(LP, 600, 55, 0, 30, 0), AMP=adsr(60, 600, 100, 300, level=100),
          MOD=dict(LFO_RATE=lfo_hz(2.4), LFO_DEPTH=pct(70), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(reverb=40), reverb=rev(70, 3.0, 3500, 25, 60)),
    patch("Sitar Buzz Pluck", "PLUCK", voices=2, banks=(12, None),      # 12 = SYNC SAW: the buzz of a sympathetic string
          OSC1=ob(60, level=100),
          FLT1=filt(LP, 3800, 12, 0, 60, 45), AMP=adsr(1, 700, 0, 450, level=70),
          sends=dict(delay=30, reverb=45, chorus=20), delay=dly(DOT8, 40, 4000), reverb=rev(80, 4.0), chorus=cho(0.4, 30)),
    patch("Mbira Tines", "PLUCK", voices=3, banks=(78, 63),             # 78 = FM TINE, 63 = BAR
          OSC1=ob(45, level=100), OSC2=ob2(50, 12, 0, 40),
          FLT1=filt(LP, 5500, 6, 0, 60, 30), AMP=adsr(1, 800, 0, 500, level=34),
          sends=dict(delay=30, reverb=45), delay=dly(DOT8, 40, 4500), reverb=rev(80, 4.0, 5000)),
    patch("Singing Bowl", "BELL", voices=2,                  # two sines a few cents apart beat against each other: the bowl's slow shimmer
          OSC1=o1(SIN, level=100), OSC2=o2(SIN, 0, 5, 80), SUB=dict(NOISE=pct(2)),
          FLT1=filt(LP, 5000, 6, 0, 55, 10), AMP=adsr(6, 2500, 0, 2500, level=31),
          sends=dict(reverb=55), reverb=rev(88, 5.0, 4500, 30)),
    patch("Temple Gong", "BELL", voices=1, banks=(59, None),            # 59 = GONG
          OSC1=ob(50, level=100),
          FLT1=filt(LP, 3500, 6, 0, 40, 10), AMP=adsr(8, 2500, 0, 2500, level=80),
          sends=dict(reverb=60), reverb=rev(92, 6.0, 3800, 35, 60)),
    patch("Log Drum", "PLUCK", voices=1, banks=(66, None),              # 66 = TUBE LONG: a hollow wooden knock
          OSC1=ob(60, level=100, OCT=-1),
          FLT1=filt(LP, 1300, 15, 0, 40, 40), AMP=adsr(1, 260, 0, 120, level=100),
          sends=dict(reverb=35), reverb=rev(60, 2.4, 4000, 20, 60)),
    patch("Finger Cymbals", "BELL", voices=3, banks=(73, None),         # 73 = RING THIN, high
          OSC1=ob(50, level=100),
          FLT1=filt(LP, 9000, 5, 0, 60, 0), AMP=adsr(1, 1300, 0, 1300, level=127),
          sends=dict(delay=20, reverb=45), delay=dly(DOT8, 35, 6000), reverb=rev(80, 4.0, 7000, 20)),
    patch("Bamboo Flute", "LEAD", voices=1, banks=(44, None),           # 44 = BREATH, an octave up
          OSC1=ob(55, level=100, OCT=1), SUB=dict(NOISE=pct(9)),
          FLT1=filt(LP, 5500, 8, 0, 60, 15), AMP=adsr(80, 450, 85, 450, level=75),
          MOD=dict(LFO_RATE=lfo_hz(5.5), LFO_DEPTH=pct(20), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=25, reverb=60), delay=dly(DOT8, 35, 4000), reverb=rev(88, 4.6, 5000, 40)),

    # ================= trance-pop and downtempo (7)
    patch("Silence Pluck Arp", "SEQ", voices=2,              # Silence (the remix): a gated, supersaw-ish pluck arp at about 135 BPM
          OSC1=o1(SAW, level=100), SUB=dict(UNISON=3, UNI_DET=pct(35), UNI_BLND=pct(65), WIDTH=pct(0)),
          FLT1=filt(LP, 2800, 14, 0, 55, 55), AMP=adsr(1, 190, 0, 100),
          sends=dict(delay=35, reverb=35, chorus=30), delay=dly(DOT8, 45, 4500), reverb=rev(70, 3.0), chorus=cho(0.5, 35)),
    patch("Bell Seq", "SEQ", voices=2, banks=(75, None),                # 75 = FM GLASS, short: a bell sequence for the lane
          OSC1=ob(30, level=100),
          FLT1=filt(LP, 6000, 6, 0, 60, 30), AMP=adsr(1, 420, 0, 250, level=77),
          sends=dict(delay=45, reverb=45), delay=dly(DOT8, 45, 4500), reverb=rev(85, 4.2, 5500)),
    patch("Euphoria Arp", "SEQ", voices=2, banks=(142, None),           # 142 = FEEDBACK SOFT; downtempo, about 100 BPM: round, the echo does the work
          OSC1=ob(35, level=100), OSC2=o2(TRI, 12, 4, 30),
          FLT1=filt(LP, 2600, 12, 0, 55, 45), AMP=adsr(2, 300, 0, 200),
          sends=dict(delay=45, reverb=50, chorus=30), delay=dly(DOT8, 45, 4000), reverb=rev(85, 4.5), chorus=cho(0.4, 35)),
    patch("FM Anthem Lead", "LEAD", voices=2, banks=(140, None),        # 140 = FM OCTAVE: a glassy, singing lead
          OSC1=ob(35, level=100), OSC2=o2(TRI, 0, 5, 30),
          FLT1=filt(LP, 5000, 8, 0, 60, 25), AMP=adsr(15, 400, 85, 300, level=50),
          MOD=dict(LFO_RATE=lfo_hz(5.0), LFO_DEPTH=pct(15), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=35, reverb=55, chorus=25), delay=dly(DOT8, 40, 4500), reverb=rev(85, 4.2), chorus=cho(0.5, 30)),
    patch("Downtempo Bass", "BASS", voices=1,                # Leeb's basslines: round and moving
          OSC1=o1(SAW, level=100), OSC2=o2(TRI, 0, 4, 60), SUB=dict(SUB_LVL=pct(40), SUB_OCT=-1),
          FLT1=filt(LP, 520, 22, 0, 45, 45), AMP=adsr(3, 320, 35, 100)),
    patch("Leeb Pulse Bass", "BASS", voices=1,               # the sequenced bass of the industrial side: a hard pulse, filter-accented
          OSC1=o1(SQR, level=100), OSC2=o2(SAW, 0, 6, 50),
          FLT1=filt(CLASSIC, 1000, 35, 15, 55, 55), GRIT=dict(CLIP=pct(25)), AMP=adsr(1, 190, 0, 60, level=74)),
    patch("Glass Chords", "STAB", voices=3, banks=(75, None),           # 75 = FM GLASS: chords that ring and fade
          OSC1=ob(40, level=100), OSC2=o2(SIN, 0, 0, 25, OCT=1),
          FLT1=filt(LP, 5500, 6, 0, 55, 20), AMP=adsr(12, 900, 0, 900),
          sends=dict(delay=30, reverb=60, chorus=30), delay=dly(DOT8, 40, 4500), reverb=rev(90, 5.0, 5500, 35), chorus=cho(0.4, 35)),

    # ================= leads (4)
    patch("Haunting Sine Lead", "LEAD", voices=1,
          OSC1=o1(SIN, level=100), OSC2=o2(TRI, 0, 3, 25, OCT=1), SUB=dict(NOISE=pct(3)),
          FLT1=filt(LP, 4500, 6, 0, 65, 10), AMP=adsr(40, 450, 85, 450, level=36),
          MOD=dict(LFO_RATE=lfo_hz(5.2), LFO_DEPTH=pct(22), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=40, reverb=60), delay=dly(DOT8, 40, 3800), reverb=rev(90, 5.0, 4500, 40)),
    patch("Morph Lead", "LEAD", voices=1, banks=(12, None),             # 12 = SYNC SAW: a slow LFO on the table position morphs the tone
          OSC1=ob(50, level=100),
          FLT1=filt(LP, 4000, 12, 0, 60, 20), AMP=adsr(40, 500, 85, 450, level=70),
          MOD=dict(LFO_RATE=lfo_hz(0.5), LFO_DEPTH=pct(40), LFO_DEST=LFO_PD, LFO_SHAPE=TRI),
          sends=dict(delay=35, reverb=55), delay=dly(DOT8, 40, 4000), reverb=rev(88, 4.8, 4500, 40)),
    patch("Soft Whistle Lead", "LEAD", voices=1, banks=(103, None),     # 103 = EVEN SOFT: a flute-organ whistle
          OSC1=ob(40, level=100),
          FLT1=filt(LP, 6500, 5, 0, 70, 10), AMP=adsr(20, 400, 85, 400, level=39),
          MOD=dict(LFO_RATE=lfo_hz(5.8), LFO_DEPTH=pct(20), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=35, reverb=55), delay=dly(DOT8, 40, 5000), reverb=rev(88, 4.6, 6000, 35)),
    patch("Violin Lead", "LEAD", voices=1, banks=(86, None),            # 86 = STRING BRIGHT, one bowed line
          OSC1=ob(55, level=100),
          FLT1=filt(LP, 4200, 8, 0, 65, 20), AMP=adsr(90, 500, 90, 400, vel=50, level=75),
          MOD=dict(LFO_RATE=lfo_hz(5.4), LFO_DEPTH=pct(25), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=25, reverb=55), delay=dly(DOT8, 35, 4000), reverb=rev(88, 4.6, 5000, 40)),

    # ================= textures (6)
    patch("Chime Tree", "BELL", voices=3, banks=(75, None),             # 75 = FM GLASS, high and long
          OSC1=ob(65, level=100, OCT=1),
          FLT1=filt(LP, 9000, 4, 0, 60, 10), GLOB=dict(MASTER=127), AMP=adsr(1, 1800, 0, 1800, level=127),
          sends=dict(delay=25, reverb=60), delay=dly(DOT8, 40, 6000), reverb=rev(92, 5.5, 7000, 30)),
    patch("Desert Wind", "TEXTURE", voices=2,
          OSC1=dict(LEVEL=0), SUB=dict(NOISE=pct(100)),
          FLT1=filt(BP, 900, 35, 0, 0, 0), GLOB=dict(MASTER=127), AMP=adsr(1500, 800, 100, 2500, vel=0, level=127),
          MOD=dict(LFO_RATE=lfo_hz(0.2), LFO_DEPTH=pct(70), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(reverb=55, chorus=30), reverb=rev(88, 5.0, 3500, 35), chorus=cho(0.3, 35)),
    patch("Rain Hiss", "TEXTURE", voices=2,
          OSC1=dict(LEVEL=0), SUB=dict(NOISE=pct(100)),
          FLT1=filt(HP, 4500, 5, 0, 0, 0), GLOB=dict(MASTER=127), AMP=adsr(1200, 800, 100, 2000, vel=0, level=59),
          sends=dict(reverb=40), reverb=rev(75, 3.5, 6000, 20)),
    patch("Deep Space Hum", "TEXTURE", voices=2,
          OSC1=o1(SIN, level=100, OCT=-1), OSC2=o2(TRI, 7, 3, 25, OCT=-1), SUB=dict(NOISE=pct(10)),
          FLT1=filt(LP, 700, 12, 0, 20, 0), AMP=adsr(1800, 800, 100, 2500, vel=0, level=90),
          sends=dict(reverb=55, chorus=40), reverb=rev(92, 6.0, 3000, 40, 60), chorus=cho(0.25, 40)),
    patch("Cathedral Bells", "BELL", voices=3, banks=(74, 58),          # 74 = FM BELL, 58 = BELL SOFT
          OSC1=ob(50, level=100), OSC2=ob2(60, 12, 0, 35),
          FLT1=filt(LP, 6000, 5, 0, 60, 15), AMP=adsr(2, 2000, 0, 2200, level=70),
          sends=dict(delay=20, reverb=70), delay=dly(DOT8, 40, 4500), reverb=rev(96, 6.5, 5000, 40)),
    patch("Spirit Whisper", "TEXTURE", voices=2, banks=(31, None),      # 31 = FORMANT over breath noise
          OSC1=ob(40, level=60), SUB=dict(NOISE=pct(35)),
          FLT1=filt(BP, 1800, 25, 0, 30, 0), AMP=adsr(1200, 800, 100, 2200, vel=0, level=70),
          MOD=dict(LFO_RATE=lfo_hz(0.3), LFO_DEPTH=pct(45), LFO_DEST=LFO_PD, LFO_SHAPE=TRI),
          sends=dict(delay=25, reverb=65), delay=dly(DOT8, 40, 3800), reverb=rev(92, 5.8, 4000, 40)),

    # ================= keys and plucks (6)
    patch("Crystal Keys", "KEYS", voices=3, banks=(75, None),           # 75 = FM GLASS
          OSC1=ob(40, level=100), OSC2=o2(TRI, 0, 3, 25),
          FLT1=filt(LP, 6000, 5, 0, 60, 20), AMP=adsr(2, 1000, 10, 500, vel=70, level=56),
          sends=dict(delay=25, reverb=50, chorus=30), delay=dly(DOT8, 40, 4500), reverb=rev(85, 4.5, 5500), chorus=cho(0.4, 30)),
    patch("Glass Harp", "KEYS", voices=3, banks=(64, None),             # 64 = GLASS, bowed: a slow attack and a held tone
          OSC1=ob(55, level=100),
          FLT1=filt(LP, 6000, 6, 0, 60, 0), AMP=adsr(180, 800, 90, 1000, vel=40, level=33),
          sends=dict(delay=20, reverb=60), delay=dly(DOT8, 40, 5000), reverb=rev(90, 5.0, 6000, 35)),
    patch("Harp Arpeggio", "PLUCK", voices=3, banks=(88, None),         # 88 = STIFF STRING
          OSC1=ob(45, level=100),
          FLT1=filt(LP, 4800, 8, 0, 65, 30), AMP=adsr(1, 900, 0, 600, level=70),
          sends=dict(delay=35, reverb=55, chorus=15), delay=dly(DOT8, 45, 4500), reverb=rev(88, 5.0, 5000, 35), chorus=cho(0.35, 25)),
    patch("Celesta Mist", "KEYS", voices=3, banks=(60, None),           # 60 = PLATE, struck
          OSC1=ob(45, level=100, OCT=1),
          FLT1=filt(LP, 7000, 5, 0, 60, 20), AMP=adsr(1, 900, 0, 800, level=70),
          sends=dict(delay=25, reverb=60), delay=dly(DOT8, 40, 5500), reverb=rev(90, 5.0, 6000, 35)),
    patch("Tuned Bars", "PLUCK", voices=3, banks=(68, None),            # 68 = BAR STACK
          OSC1=ob(50, level=100),
          FLT1=filt(LP, 5500, 6, 0, 60, 25), AMP=adsr(1, 650, 0, 500, level=70),
          sends=dict(delay=30, reverb=50), delay=dly(DOT8, 40, 4500), reverb=rev(85, 4.5, 5000, 30)),
    patch("Electro Piano Dark", "KEYS", voices=3, banks=(141, None),    # 141 = FEEDBACK: a dark, round electric piano
          OSC1=ob(25, level=100), OSC2=o2(TRI, 0, 3, 25),
          FLT1=filt(LP, 2800, 5, 0, 60, 25), AMP=adsr(2, 1200, 8, 600, vel=75, level=52),
          sends=dict(delay=25, reverb=55, chorus=25), delay=dly(DOT8, 40, 4000), reverb=rev(88, 4.8, 4500, 30), chorus=cho(0.4, 30)),
]
