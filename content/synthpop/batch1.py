# Synth pop, batch 1: fifty patches (ids 350-399; only ever append, a patch's position is its id). tools/genre_presets.py builds them (--set synthpop).
# Engine facts (docs/PATCH_MAP.md): the filter envelope decays with the amp decay and has no sustain; FINE is the detune; UNISON sits on the SUB page and costs voices;
# no glide (so no portamento leads), no phaser; the LFO has no delay (vibrato starts at full depth); the LFO PD destination moves one table position for both
# oscillators, so a patch using it puts the same bank on both; OSC 2's pitch is always written out. The DX-style keys and bells are the FM wavetable banks (135,
# 136, 78, 74), the E-piano with a quiet sine an octave up. Only banks with a converted file (genre_presets.py banks).
# Levels start at the top (AMP LEVEL 127) and content/tools/calibrate_levels.py lowers the loud ones to the library's level.

BASE_ID = 350

PATCHES = [
    # ================= basses (7): one voice, a dry low end
    patch("Moog Pop Bass", "BASS", voices=1,
          OSC1=o1(SAW, level=100, OCT=-1), FLT1=filt(LP, 520, 25, 0, 55, 55), GRIT=dict(CLIP=pct(25)), AMP=adsr(2, 250, 40, 100, level=51)),
    patch("FM Pop Bass", "BASS", voices=1, banks=(136, None),           # 136 = FM 1:3
          OSC1=ob(35, level=100, OCT=-1),
          FLT1=filt(LP, 1500, 12, 0, 55, 35), AMP=adsr(2, 200, 25, 90, vel=80)),
    patch("Sequencer Root", "BASS", voices=1,
          OSC1=o1(SQR, level=100, OCT=-1), OSC2=o2(SAW, 0, 5, 50, OCT=-1),
          FLT1=filt(LP, 700, 20, 0, 55, 60), AMP=adsr(2, 130, 0, 60)),
    patch("Digital Piano", "KEYS", voices=4, banks=(88, None),      # 88 = STIFF STRING
          OSC1=ob(30, level=100), OSC2=o2(SIN, 0, 0, 15, OCT=1),
          FLT1=filt(LP, 4000, 5, 0, 60, 30), AMP=adsr(2, 1200, 10, 500, vel=90, level=54),
          sends=dict(reverb=30, chorus=35), reverb=rev(60, 2.8), chorus=cho(0.7, 35)),
    patch("Soft Sub Pop", "BASS", voices=1,
          OSC1=o1(TRI, level=100), SUB=dict(SUB_LVL=pct(50), SUB_OCT=-1),
          FLT1=filt(LP, 400, 8, 0, 40, 20), AMP=adsr(3, 300, 70, 120, level=51)),
    patch("Bouncy Square Bass", "BASS", voices=1,
          OSC1=o1(SQR, level=100, OCT=-1), FLT1=filt(LP, 650, 18, 0, 55, 50), AMP=adsr(2, 140, 10, 60)),
    patch("Analogue Pump", "BASS", voices=1,
          OSC1=o1(SAW, -12, 90, OCT=-1), OSC2=o2(SAW, 0, 12, 90, OCT=-1),
          FLT1=filt(LADDER, 450, 25, 15, 55, 45), AMP=adsr(2, 200, 55, 90)),

    # ================= leads (9): vibrato from the LFO, delay and reverb sends
    patch("Sync Pop Lead", "LEAD", voices=1,
          OSC1=o1(SAW, level=100, SEMI=9), OSC2=o2(SAW, 0, 0, 20, SYNC=pct(85)),   # OSC 2 is the sync MASTER (sets the pitch, quiet), OSC 1 the slave
          FLT1=filt(LP, 4000, 15, 0, 70, 30), GRIT=dict(CLIP=pct(25)), AMP=adsr(3, 300, 85, 160, level=27),
          MOD=dict(LFO_RATE=lfo_hz(5.2), LFO_DEPTH=pct(25), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=40, reverb=25, chorus=30), delay=dly(E8, 40, 4500), reverb=rev(55, 2.5), chorus=cho(0.7, 30)),
    patch("Saw Hook Lead", "LEAD", voices=2,
          OSC1=o1(SAW, -6, 100), OSC2=o2(SAW, 0, 6, 100),
          FLT1=filt(LP, 3500, 15, 0, 70, 30), AMP=adsr(4, 350, 80, 180, level=33),
          MOD=dict(LFO_RATE=lfo_hz(5.5), LFO_DEPTH=pct(25), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=35, reverb=30, chorus=30), delay=dly(DOT8, 40, 4500), reverb=rev(60, 2.8), chorus=cho(0.6, 35)),
    patch("Square Hook Lead", "LEAD", voices=2,
          OSC1=o1(SQR, -5, 100), OSC2=o2(SQR, 0, 5, 100),
          FLT1=filt(LADDER, 3000, 20, 0, 70, 25), AMP=adsr(3, 300, 75, 150, level=72),
          MOD=dict(LFO_RATE=lfo_hz(5.8), LFO_DEPTH=pct(25), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=40, reverb=25), delay=dly(E8, 40, 4500), reverb=rev(55, 2.5)),
    patch("FM Bell Lead", "LEAD", voices=1, banks=(135, None),          # 135 = FM 1:1
          OSC1=ob(45), OSC2=o2(SIN, 0, 0, 20, OCT=1),
          FLT1=filt(LP, 6000, 10, 0, 65, 20), AMP=adsr(3, 350, 70, 200, level=47),
          MOD=dict(LFO_RATE=lfo_hz(5.0), LFO_DEPTH=pct(20), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=35, reverb=40), delay=dly(DOT8, 40, 5000), reverb=rev(65, 3.2)),
    patch("Mellotron Flute", "LEAD", voices=1, banks=(44, None),    # 44 = BREATH
          OSC1=ob(50, level=100), OSC2=o2(SIN, 0, 0, 20, OCT=1),
          FLT1=filt(LP, 4000, 8, 0, 60, 15), AMP=adsr(35, 400, 85, 250, level=66),
          MOD=dict(LFO_RATE=lfo_hz(5.0), LFO_DEPTH=pct(25), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(reverb=55, chorus=40), reverb=rev(75, 2.5), chorus=cho(0.5, 40)),
    patch("Brass Lead", "LEAD", voices=1,
          OSC1=o1(SAW, -5, 100), OSC2=o2(SAW, 0, 5, 100),
          FLT1=filt(LP, 1400, 12, 0, 60, 55), AMP=adsr(25, 350, 75, 160, level=35),
          sends=dict(delay=25, reverb=30), delay=dly(DOT8, 35, 4000), reverb=rev(55, 2.5)),
    patch("Reed Lead", "LEAD", voices=1, banks=(98, None),              # 98 = ODD ONLY
          OSC1=ob(60),
          FLT1=filt(LP, 3000, 12, 0, 65, 25), AMP=adsr(20, 400, 80, 200, level=42),
          MOD=dict(LFO_RATE=lfo_hz(5.0), LFO_DEPTH=pct(25), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=25, reverb=30), delay=dly(DOT8, 35, 4000), reverb=rev(55, 2.5)),
    patch("Pulse Wide Lead", "LEAD", voices=2, banks=(131, 131),        # 131 = PWM on both, one LFO sweeps the width
          OSC1=ob(35, -6, 100), OSC2=ob2(35, 0, 7, 70),
          FLT1=filt(LP, 3500, 15, 0, 65, 25), AMP=adsr(5, 400, 80, 200, level=32),
          MOD=dict(LFO_RATE=lfo_hz(0.9), LFO_DEPTH=pct(30), LFO_DEST=LFO_PD, LFO_SHAPE=TRI),
          sends=dict(delay=35, reverb=30, chorus=40), delay=dly(DOT8, 40, 4500), reverb=rev(60, 2.8), chorus=cho(0.6, 40)),
    patch("Vox Lead", "LEAD", voices=1, banks=(31, None),               # 31 = FORMANT
          OSC1=ob(55), OSC2=o2(SAW, 0, 5, 25),
          FLT1=filt(LP, 4500, 10, 0, 65, 15), AMP=adsr(30, 400, 80, 250, level=52),
          sends=dict(delay=30, reverb=40, chorus=40), delay=dly(DOT8, 35, 4500), reverb=rev(65, 3.2), chorus=cho(0.5, 40)),

    # ================= pads (7): a slow attack, chorus, a long reverb
    patch("Jupiter Strings", "PAD", voices=3,
          OSC1=o1(SAW, -10, 80), OSC2=o2(SAW, 0, 10, 80),
          FLT1=filt(LP, 2200, 6, 0, 40, 0), GRIT=dict(DRIFT=pct(20)), AMP=adsr(450, 600, 90, 1200, vel=30, level=47),
          sends=dict(reverb=45, chorus=90), reverb=rev(80, 4.5, 4500, 30), chorus=cho(0.5, 65, 14)),
    patch("Warm Poly Pad", "PAD", voices=3,
          OSC1=o1(SAW, -7, 80), OSC2=o2(SQR, 0, 7, 60),
          FLT1=filt(LP, 1100, 10, 0, 40, 0), AMP=adsr(500, 700, 90, 1400, vel=30, level=40),
          sends=dict(reverb=50, chorus=70), reverb=rev(80, 4.5, 4000, 30), chorus=cho(0.4, 55)),
    patch("Choir Pad Pop", "PAD", voices=4, banks=(32, None),           # 32 = CHOIR
          OSC1=ob(45), OSC2=o2(SAW, 0, 6, 20),
          FLT1=filt(LP, 4500, 6, 0, 50, 0), AMP=adsr(500, 800, 90, 1600, vel=30),
          sends=dict(reverb=60, chorus=40), reverb=rev(88, 5.5, 4000, 35), chorus=cho(0.35, 45)),
    patch("Glass Pad Pop", "PAD", voices=3, banks=(64, None),           # 64 = GLASS
          OSC1=ob(50), OSC2=o2(SIN, 0, 0, 20, OCT=1),
          FLT1=filt(LP, 7000, 5, 0, 60, 0), AMP=adsr(350, 800, 85, 1500, vel=30, level=57),
          sends=dict(delay=25, reverb=65), delay=dly(DOT8, 40, 5000), reverb=rev(85, 5.0, 7000, 30)),
    patch("Ensemble Pad", "PAD", voices=4, banks=(18, None),            # 18 = SPREAD 5
          OSC1=ob(40), OSC2=o2(SAW, 0, 8, 35),
          FLT1=filt(LP, 3000, 8, 0, 45, 0), AMP=adsr(600, 800, 90, 1600, vel=30),
          sends=dict(reverb=45, chorus=85), reverb=rev(80, 4.5, 5000, 30), chorus=cho(0.5, 65)),
    patch("Vowel Pad", "PAD", voices=4, banks=(31, 31),             # 31 = FORMANT on both: the LFO morphs the vowel
          OSC1=ob(40, -7, 100), OSC2=ob2(40, 0, 7, 70),
          FLT1=filt(LP, 4500, 8, 0, 45, 0), AMP=adsr(600, 800, 90, 1600, vel=30, level=45),
          MOD=dict(LFO_RATE=lfo_hz(0.3), LFO_DEPTH=pct(40), LFO_DEST=LFO_PD, LFO_SHAPE=TRI),
          sends=dict(reverb=60, chorus=60), reverb=rev(85, 5.0, 4500, 30), chorus=cho(0.4, 50)),
    patch("Bright Pad", "PAD", voices=3,
          OSC1=o1(SAW, -6, 80), OSC2=o2(SQR, 0, 6, 60),
          FLT1=filt(LP, 3800, 8, 0, 50, 0), AMP=adsr(250, 600, 90, 1000, vel=30, level=40),
          sends=dict(delay=15, reverb=40, chorus=60), delay=dly(DOT8, 35, 4500), reverb=rev(70, 3.8, 5500), chorus=cho(0.6, 50)),

    # ================= chords (4)
    patch("Juno Chords", "POLY", voices=3,
          OSC1=o1(SAW, -8, 90), OSC2=o2(SAW, 0, 8, 90), SUB=dict(SUB_LVL=pct(20), SUB_OCT=-1),
          FLT1=filt(LP, 2400, 8, 0, 55, 25), AMP=adsr(12, 500, 80, 350, level=30),
          sends=dict(reverb=30, chorus=95), reverb=rev(60, 2.8), chorus=cho(0.8, 70)),
    patch("Pop Chord Stack", "POLY", voices=2,
          OSC1=o1(SAW), OSC2=o2(SQR, 0, 7, 50),
          SUB=dict(UNISON=2, UNI_DET=pct(30), UNI_BLND=pct(60), WIDTH=pct(60)),
          FLT1=filt(LP, 3000, 8, 0, 55, 20), AMP=adsr(10, 450, 80, 300, level=33),
          sends=dict(reverb=35, chorus=50), reverb=rev(65, 3.0), chorus=cho(0.6, 40)),
    patch("Bright Poly", "POLY", voices=3,
          OSC1=o1(SAW, -7, 90), OSC2=o2(SAW, 0, 7, 90),
          FLT1=filt(LADDER, 3600, 12, 0, 60, 30), AMP=adsr(6, 400, 75, 250),
          sends=dict(delay=20, reverb=35, chorus=45), delay=dly(DOT8, 35, 4500), reverb=rev(65, 3.0), chorus=cho(0.6, 45)),
    patch("Dream Poly", "POLY", voices=3,
          OSC1=o1(SAW, -9, 90), OSC2=o2(SAW, 0, 9, 90),
          FLT1=filt(LP, 1800, 15, 0, 45, 15), AMP=adsr(200, 600, 85, 800, vel=40, level=27),
          MOD=dict(LFO_RATE=lfo_hz(0.5), LFO_DEPTH=pct(20), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(delay=25, reverb=55, chorus=70), delay=dly(DOT8, 40, 3500), reverb=rev(80, 4.5, 4500, 30), chorus=cho(0.5, 55)),

    # ================= keys (6)
    patch("DX E-Piano", "KEYS", voices=4, banks=(135, None),            # 135 = FM 1:1
          OSC1=ob(30), OSC2=o2(SIN, 0, 0, 20, OCT=1),
          FLT1=filt(LP, 4500, 5, 0, 60, 25), AMP=adsr(2, 900, 10, 400, vel=90, level=73),
          sends=dict(reverb=25, chorus=55), reverb=rev(55, 2.5), chorus=cho(0.7, 40)),
    patch("DX Tine Piano", "KEYS", voices=4, banks=(78, None),          # 78 = FM TINE
          OSC1=ob(35), OSC2=o2(SIN, 0, 0, 20),
          FLT1=filt(LP, 4000, 5, 0, 60, 20), AMP=adsr(2, 1100, 10, 500, vel=90, level=42),
          sends=dict(reverb=30, chorus=50), reverb=rev(60, 2.8), chorus=cho(0.7, 40)),
    patch("Rhodes Pop", "KEYS", voices=4, banks=(136, None),            # 136 = FM 1:3
          OSC1=ob(25), OSC2=o2(SIN, 0, 0, 15),
          FLT1=filt(LP, 3000, 5, 0, 60, 20), AMP=adsr(3, 1200, 15, 500, vel=90, level=48),
          MOD=dict(LFO_RATE=lfo_hz(4.5), LFO_DEPTH=pct(15), LFO_DEST=LFO_AMP, LFO_SHAPE=SIN),
          sends=dict(reverb=30, chorus=70), reverb=rev(60, 2.8), chorus=cho(0.8, 45)),
    patch("Clav Pop", "KEYS", voices=3,
          OSC1=o1(SAW), OSC2=o2(SQR, 0, 0, 20, OCT=1),
          FLT1=filt(LP, 3500, 25, 0, 60, 80), AMP=adsr(2, 200, 15, 70, vel=80),
          sends=dict(reverb=15), reverb=rev(45, 1.8)),
    patch("Pop Organ", "KEYS", voices=4, banks=(91, None),              # 91 = ORGAN
          OSC1=ob(50), OSC2=o2(SQR, 0, 0, 15, OCT=1),
          FLT1=filt(LP, 5000, 5, 0, 50, 5), AMP=adsr(5, 300, 100, 120, vel=10, level=22),
          MOD=dict(LFO_RATE=lfo_hz(6.0), LFO_DEPTH=pct(20), LFO_DEST=LFO_AMP, LFO_SHAPE=SIN),
          sends=dict(reverb=30, chorus=60), reverb=rev(60, 2.8), chorus=cho(0.8, 50)),
    patch("Harpsichord Pop", "KEYS", voices=4, banks=(86, None),        # 86 = STRING BRIGHT
          OSC1=ob(50),
          FLT1=filt(LP, 6000, 8, 0, 60, 30), AMP=adsr(2, 500, 0, 150),
          sends=dict(reverb=30), reverb=rev(60, 2.5)),

    # ================= brass and stabs (5)
    patch("Juno Brass", "STAB", voices=3,
          OSC1=o1(SAW, -6, 100), OSC2=o2(SAW, 0, 6, 100),
          FLT1=filt(LP, 900, 10, 0, 50, 70), AMP=adsr(25, 450, 60, 200, level=53),
          sends=dict(reverb=30, chorus=40), reverb=rev(60, 2.5), chorus=cho(0.6, 35)),
    patch("Orchestra Hit", "STAB", voices=3,
          OSC1=o1(SAW, -6, 100), OSC2=o2(SAW, 7, 6, 70),
          FLT1=filt(LP, 2000, 12, 0, 50, 60), AMP=adsr(2, 500, 0, 300),
          sends=dict(reverb=50), reverb=rev(80, 3.8)),
    patch("Horn Stab Pop", "STAB", voices=3,
          OSC1=o1(SAW, -5, 100), OSC2=o2(SQR, 0, 5, 60),
          FLT1=filt(LP, 1100, 12, 0, 50, 60), AMP=adsr(10, 300, 20, 150),
          sends=dict(reverb=30), reverb=rev(55, 2.2)),
    patch("Pop Stab", "STAB", voices=3,
          OSC1=o1(SQR, -5, 100), OSC2=o2(SAW, 0, 5, 100),
          FLT1=filt(LP, 1500, 15, 0, 55, 65), AMP=adsr(2, 180, 0, 100, level=62),
          sends=dict(delay=30, reverb=25), delay=dly(E8, 35, 4500), reverb=rev(50, 2.0)),
    patch("Brass Swell", "POLY", voices=3,
          OSC1=o1(SAW, -6, 100), OSC2=o2(SAW, 0, 6, 100),
          FLT1=filt(LP, 700, 12, 0, 50, 0), AMP=adsr(450, 400, 90, 500, level=26),
          sends=dict(reverb=35, chorus=35), reverb=rev(65, 3.0), chorus=cho(0.5, 35)),

    # ================= arps, sequences and plucks (8)
    patch("Arp Anthem", "SEQ", voices=2,
          OSC1=o1(SAW, -6, 100), OSC2=o2(SQR, 0, 6, 50),
          FLT1=filt(LP, 2400, 18, 0, 55, 45), AMP=adsr(2, 150, 0, 90),
          sends=dict(delay=28, reverb=30), delay=dly(E8, 40, 4500), reverb=rev(60, 2.8)),
    patch("Chip Arp 80s", "SEQ", voices=2, banks=(130, None),           # 130 = CHIP
          OSC1=ob(45),
          FLT1=filt(LP, 5000, 10, 0, 55, 35), AMP=adsr(2, 100, 0, 60),
          sends=dict(delay=35, reverb=20), delay=dly(DOT8, 40, 5000), reverb=rev(50, 2.0)),
    patch("Pluck Poly Pop", "PLUCK", voices=3,
          OSC1=o1(SAW, -6, 100), OSC2=o2(SAW, 0, 6, 100),
          FLT1=filt(LP, 2200, 15, 0, 55, 70), AMP=adsr(2, 260, 0, 150, level=55),
          sends=dict(delay=30, reverb=30, chorus=25), delay=dly(DOT8, 40, 4500), reverb=rev(60, 2.8), chorus=cho(0.6, 30)),
    patch("Marimba Pluck", "PLUCK", voices=2, banks=(63, None),         # 63 = BAR
          OSC1=ob(40),
          FLT1=filt(LP, 5000, 8, 0, 60, 25), AMP=adsr(2, 350, 0, 200),
          sends=dict(reverb=35), reverb=rev(60, 2.5)),
    patch("Bouncy Seq", "SEQ", voices=1,
          OSC1=o1(SQR), FLT1=filt(LP, 1400, 25, 0, 55, 70), AMP=adsr(2, 110, 0, 50),
          sends=dict(delay=35), delay=dly(DOT16, 40, 4500)),
    patch("Kalimba Pluck", "PLUCK", voices=2, banks=(78, None),         # 78 = FM TINE
          OSC1=ob(55),
          FLT1=filt(LP, 5000, 5, 0, 60, 20), AMP=adsr(2, 320, 0, 200, level=54),
          sends=dict(delay=30, reverb=35), delay=dly(DOT8, 40, 5000), reverb=rev(60, 2.8)),
    patch("Glassy Pluck", "PLUCK", voices=2, banks=(64, None),          # 64 = GLASS
          OSC1=ob(60),
          FLT1=filt(LP, 7000, 5, 0, 60, 15), AMP=adsr(2, 250, 0, 300),
          sends=dict(reverb=45, chorus=30), reverb=rev(70, 3.2, 7000), chorus=cho(0.6, 30)),
    patch("Echo Pluck", "PLUCK", voices=2,
          OSC1=o1(SAW, -5, 100), OSC2=o2(SAW, 0, 5, 80),
          FLT1=filt(LP, 1800, 15, 0, 55, 60), AMP=adsr(2, 200, 0, 120, level=57),
          sends=dict(delay=42, reverb=25), delay=dly(E8, 55, 4000), reverb=rev(50, 2.0)),

    # ================= bells and mallets (4)
    patch("Wurly EP", "KEYS", voices=4, banks=(99, None),           # 99 = EVEN ONLY
          OSC1=ob(45, level=100), OSC2=o2(SQR, 0, 0, 12, OCT=1),
          FLT1=filt(LP, 3200, 8, 0, 60, 35), AMP=adsr(2, 700, 20, 300, vel=90, level=72),
          MOD=dict(LFO_RATE=lfo_hz(5.5), LFO_DEPTH=pct(30), LFO_DEST=LFO_AMP, LFO_SHAPE=SIN),
          sends=dict(reverb=25, chorus=50), reverb=rev(55, 2.5), chorus=cho(0.7, 40)),
    patch("FM Bell Pop", "BELL", voices=2, banks=(74, None),            # 74 = FM BELL
          OSC1=ob(50),
          FLT1=filt(LP, 8000, 5, 0, 60, 10), AMP=adsr(2, 1400, 0, 1200, level=57),
          sends=dict(delay=30, reverb=60), delay=dly(DOT8, 40, 5000), reverb=rev(75, 4.5, 6500, 30)),
    patch("Vibes Pop", "BELL", voices=2, banks=(58, None),              # 58 = BELL SOFT
          OSC1=ob(40),
          FLT1=filt(LP, 6000, 5, 0, 60, 10), AMP=adsr(2, 1800, 0, 900, level=63),
          MOD=dict(LFO_RATE=lfo_hz(5.0), LFO_DEPTH=pct(25), LFO_DEST=LFO_AMP, LFO_SHAPE=SIN),
          sends=dict(reverb=40, chorus=25), reverb=rev(65, 3.2), chorus=cho(0.6, 30)),
    patch("Bar Stack Bell", "BELL", voices=2, banks=(68, None),         # 68 = BAR STACK
          OSC1=ob(35),
          FLT1=filt(LP, 5500, 5, 0, 60, 10), AMP=adsr(2, 700, 0, 400),
          sends=dict(reverb=35), reverb=rev(60, 2.5)),
]
