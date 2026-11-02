# Techno and trance, batch 1: fifty patches (ids 300-349; only ever append, a patch's position is its id). tools/genre_presets.py builds them (--set techno).
# Engine facts (docs/PATCH_MAP.md): the filter envelope decays with the amp decay and has no sustain; FINE is the detune; UNISON sits on the SUB page and costs voices;
# no glide, no pitch envelope (so no kick drum, no laser zaps); the LFO PD destination moves one table position for both oscillators; OSC 2's pitch is always
# written out (the panel's default is a fifth up). Noise-only sounds use SUB NOISE with both oscillators at level 0. Drums are not synthesized here: a hat, a
# clap and a tick are short noise or metallic-bank bursts for a sequencer lane. Only banks with a converted file (genre_presets.py banks).
# Levels start at the top (AMP LEVEL 127) and content/tools/calibrate_levels.py lowers the loud ones to the library's level.

BASE_ID = 300

PATCHES = [
    # ================= basses (9): one voice
    patch("Rumble Low", "BASS", voices=1,
          OSC1=o1(TRI, level=100, OCT=-1), OSC2=o2(SIN, 0, 0, 60, OCT=-1), SUB=dict(SUB_LVL=pct(50), SUB_OCT=-1),
          FLT1=filt(LP, 160, 10, 35, 30, 20), GRIT=dict(CLIP=pct(30)), AMP=adsr(2, 260, 45, 110, level=38),
          sends=dict(reverb=15), reverb=rev(60, 2.0, 1800, 10, low=30)),
    patch("Offbeat Roll", "BASS", voices=1,
          OSC1=o1(SAW, level=100, OCT=-1), OSC2=o2(SAW, 0, 6, 70, OCT=-1),
          FLT1=filt(LP, 380, 25, 0, 50, 55), AMP=adsr(2, 110, 0, 60)),
    patch("Acid Squelch", "BASS", voices=1,
          OSC1=o1(SAW, level=100, OCT=-1), OSC2=o2(SAW, 0, 4, 90, OCT=-1), SUB=dict(SUB_LVL=pct(45), SUB_OCT=-1),
          FLT1=filt(ACID, 1100, 35, 55, 40, 40), GRIT=dict(CLIP=pct(50)), GLOB=dict(MASTER=127), AMP=adsr(2, 200, 40, 70),
          sends=dict(delay=30), delay=dly(DOT16, 35, 4000)),
    patch("Acid Grind", "BASS", voices=1,
          OSC1=o1(SQR, level=100, OCT=-1),
          FLT1=filt(ACID, 500, 50, 60, 40, 60), GRIT=dict(CLIP=pct(40)), AMP=adsr(2, 160, 40, 60)),
    patch("Warehouse Sub", "BASS", voices=1,
          OSC1=o1(SIN, level=100), SUB=dict(SUB_LVL=pct(70), SUB_OCT=-1), GRIT=dict(CLIP=pct(40)), GLOB=dict(MASTER=127),
          FLT1=filt(LP, 300, 8, 0, 30, 15), AMP=adsr(3, 400, 85, 150, level=25)),
    patch("Industrial Fold", "BASS", voices=1, banks=(152, None),       # 152 = FOLD
          OSC1=ob(50, level=100, OCT=-1), OSC2=o2(SAW, 0, 5, 30, OCT=-1),
          FLT1=filt(LP, 900, 25, 30, 50, 35), GRIT=dict(CLIP=pct(25)), AMP=adsr(2, 200, 45, 90, level=41)),
    patch("Pumping Square", "BASS", voices=1,
          OSC1=o1(SQR, level=100, OCT=-1), OSC2=o2(SAW, 0, 5, 60, OCT=-1), SUB=dict(SUB_LVL=pct(30), SUB_OCT=-1),
          FLT1=filt(LADDER, 480, 30, 0, 50, 45), AMP=adsr(2, 150, 50, 60)),
    patch("Rave Bass Stab", "BASS", voices=1,
          OSC1=o1(SAW, level=100, OCT=-1), OSC2=o2(SAW, 7, 0, 50),
          FLT1=filt(LP, 900, 15, 0, 55, 60), AMP=adsr(2, 180, 0, 80),
          sends=dict(delay=25), delay=dly(DOT16, 35, 4500)),
    patch("Dub Techno Bass", "BASS", voices=1,
          OSC1=o1(TRI, level=100), OSC2=o2(SIN, 0, 0, 70, OCT=-1),
          FLT1=filt(LP, 260, 12, 0, 40, 20), AMP=adsr(4, 500, 60, 300, level=50),
          sends=dict(delay=35, reverb=25), delay=dly(DOT8, 55, 2800), reverb=rev(65, 3.0, 2500)),

    # ================= sequence and arp voices (6) and plucks and noise hits (5): short envelopes, two voices so tails overlap
    patch("Trance Pluck Arp", "SEQ", voices=2,
          OSC1=o1(SAW, -7, 100), OSC2=o2(SAW, 0, 7, 80),
          FLT1=filt(LP, 1700, 25, 0, 55, 60), AMP=adsr(2, 130, 0, 90, vel=70),
          sends=dict(delay=30, reverb=30), delay=dly(DOT8, 50, 4500), reverb=rev(70, 3.0)),
    patch("Gated Arp", "SEQ", voices=2,
          OSC1=o1(SQR, -5, 100), OSC2=o2(SAW, 12, 5, 40),
          FLT1=filt(LP, 2200, 20, 0, 55, 45), AMP=adsr(2, 90, 0, 60, level=76),
          sends=dict(delay=28, reverb=20), delay=dly(E8, 45, 4500), reverb=rev(55, 2.2)),
    patch("Hypno Pulse Seq", "SEQ", voices=2, banks=(7, None),         # 7 = PWM NARROW; the LFO sweeps the width
          OSC1=ob(40, level=100),
          FLT1=filt(LP, 2000, 25, 0, 55, 40), AMP=adsr(2, 140, 0, 90),
          MOD=dict(LFO_RATE=lfo_hz(0.8), LFO_DEPTH=pct(25), LFO_DEST=LFO_PD, LFO_SHAPE=TRI),
          sends=dict(delay=15, reverb=25), delay=dly(DOT8, 45, 5000), reverb=rev(60, 2.5)),
    patch("Minimal Click", "SEQ", voices=2,
          OSC1=o1(SIN, level=100), OSC2=o2(SIN, 0, 0, 30, OCT=1),
          FLT1=filt(LP, 6000, 8, 0, 60, 20), AMP=adsr(2, 45, 0, 40),
          sends=dict(delay=16, reverb=25), delay=dly(T8, 40, 5000), reverb=rev(55, 2.0)),
    patch("Detroit Seq", "SEQ", voices=2, banks=(135, None),           # 135 = FM 1:1
          OSC1=ob(50, level=100), OSC2=o2(SIN, 0, 0, 25, OCT=1),
          FLT1=filt(LP, 5000, 10, 0, 60, 30), AMP=adsr(2, 150, 0, 100),
          sends=dict(delay=22, reverb=30, chorus=30), delay=dly(DOT8, 45, 5000), reverb=rev(65, 2.8), chorus=cho(0.6, 35)),
    patch("Chip Trance Arp", "SEQ", voices=2, banks=(130, None),       # 130 = CHIP
          OSC1=ob(40, level=100), OSC2=o2(SQR, 0, 0, 35, OCT=1),
          FLT1=filt(LP, 4500, 10, 0, 55, 35), AMP=adsr(2, 110, 0, 70),
          sends=dict(delay=25, reverb=20), delay=dly(DOT8, 45, 5000), reverb=rev(55, 2.2)),
    patch("Metal Tick", "PLUCK", voices=2, banks=(76, None),           # 76 = FM CLANG
          OSC1=ob(30, level=100),
          FLT1=filt(HP, 800, 15, 0, 40, 0), AMP=adsr(1, 70, 0, 50),
          sends=dict(reverb=25), reverb=rev(50, 1.8, 6000, 10)),
    patch("Ring Pluck", "PLUCK", voices=2, banks=(70, None),           # 70 = RING WIDE
          OSC1=ob(30, level=100),
          FLT1=filt(LP, 3000, 20, 0, 55, 50), AMP=adsr(2, 180, 0, 120),
          sends=dict(delay=35, reverb=35), delay=dly(DOT8, 40, 4500), reverb=rev(65, 3.0)),
    patch("Shatter Seq", "PLUCK", voices=2, banks=(166, None),         # 166 = SHATTER FIB
          OSC1=ob(45, level=100),
          FLT1=filt(LP, 3500, 15, 0, 55, 40), AMP=adsr(2, 120, 0, 80),
          sends=dict(delay=25, reverb=25), delay=dly(DOT16, 45, 5000), reverb=rev(60, 2.5)),
    patch("Acid Zap", "PLUCK", voices=1,
          OSC1=o1(SAW, level=100),
          FLT1=filt(LP, 500, 55, 0, 50, 85), AMP=adsr(2, 100, 0, 60, level=59),
          sends=dict(delay=16), delay=dly(DOT16, 40, 4500)),
    patch("Noise Hat", "PLUCK", voices=2,
          OSC1=dict(LEVEL=0), SUB=dict(NOISE=pct(100)),
          FLT1=filt(HP, 7000, 10, 0, 0, 0), AMP=adsr(1, 45, 0, 25, level=77),
          sends=dict(reverb=15), reverb=rev(40, 1.0, 8000, 5)),

    # ================= leads (8)
    patch("Vocal Chop", "STAB", voices=2, banks=(31, None),         # 31 = FORMANT
          OSC1=ob(55, level=100),
          FLT1=filt(LP, 4000, 10, 0, 55, 25), AMP=adsr(2, 180, 0, 120),
          sends=dict(delay=35, reverb=35), delay=dly(DOT8, 40, 4500), reverb=rev(65, 3.0)),
    patch("Anthem Supersaw", "LEAD", voices=2,
          OSC1=o1(SAW, level=100), SUB=dict(UNISON=3, UNI_DET=pct(45), UNI_BLND=pct(70), WIDTH=pct(80)),
          FLT1=filt(LP, 4500, 8, 0, 60, 20), AMP=adsr(6, 500, 85, 260, level=46),
          sends=dict(delay=45, reverb=40, chorus=25), delay=dly(DOT8, 40, 5000), reverb=rev(75, 3.5), chorus=cho(0.5, 35)),
    patch("Trance Pluck Lead", "LEAD", voices=2,
          OSC1=o1(SAW, -6, 100), OSC2=o2(SQR, 0, 6, 60),
          FLT1=filt(LP, 3000, 20, 0, 65, 55), AMP=adsr(2, 260, 35, 160, level=66),
          sends=dict(delay=55, reverb=30), delay=dly(DOT8, 45, 5000), reverb=rev(60, 2.5)),
    patch("Acid Lead", "LEAD", voices=1,
          OSC1=o1(SAW, level=100), OSC2=o2(SAW, 0, 6, 90),
          FLT1=filt(ACID, 1800, 40, 40, 60, 40), GRIT=dict(CLIP=pct(25)), AMP=adsr(3, 300, 70, 120),
          sends=dict(delay=40, reverb=20), delay=dly(E8, 40, 4500), reverb=rev(50, 2.0)),
    patch("Sync Trance Lead", "LEAD", voices=1,
          OSC1=o1(SAW, level=100, SEMI=7), OSC2=o2(SAW, 0, 0, 20, SYNC=pct(80)),   # OSC 2 is the sync MASTER (sets the pitch, quiet), OSC 1 the slave (its pitch sets the timbre)
          FLT1=filt(LP, 4500, 15, 0, 70, 30), AMP=adsr(3, 300, 85, 160, level=34),
          MOD=dict(LFO_RATE=lfo_hz(5.0), LFO_DEPTH=pct(25), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=45, reverb=25), delay=dly(DOT8, 40, 5000), reverb=rev(55, 2.5)),
    patch("Square Anthem", "LEAD", voices=2,
          OSC1=o1(SQR, -7, 90), OSC2=o2(SQR, 0, 7, 90),
          FLT1=filt(LADDER, 3200, 22, 0, 70, 25), AMP=adsr(4, 350, 80, 200, level=77),
          sends=dict(delay=50, reverb=30, chorus=25), delay=dly(DOT8, 40, 4500), reverb=rev(60, 2.8), chorus=cho(0.7, 30)),
    patch("Hoover Lead", "LEAD", voices=2, banks=(12, None),           # 12 = SYNC SAW
          OSC1=ob(40, level=100), OSC2=o2(SAW, 0, 12, 60),
          SUB=dict(UNISON=2, UNI_DET=pct(35), UNI_BLND=pct(60), WIDTH=pct(60)),
          FLT1=filt(LP, 5000, 20, 20, 60, 20), AMP=adsr(8, 300, 85, 200, level=50),
          MOD=dict(LFO_RATE=lfo_hz(4.0), LFO_DEPTH=pct(35), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=35, reverb=30), delay=dly(DOT8, 40, 4500), reverb=rev(60, 2.8)),
    patch("Voice Lead", "LEAD", voices=1, banks=(32, None),            # 32 = CHOIR
          OSC1=ob(60, level=100), OSC2=o2(SAW, 0, 6, 25),
          FLT1=filt(LP, 4000, 10, 0, 60, 15), AMP=adsr(40, 400, 85, 300),
          sends=dict(delay=35, reverb=55, chorus=40), delay=dly(DOT8, 40, 4000), reverb=rev(80, 4.5), chorus=cho(0.4, 40)),

    # ================= stabs (7): two or three voices
    patch("Rave Stab", "STAB", voices=3,
          OSC1=o1(SAW, -7, 100), OSC2=o2(SAW, 7, 7, 80),
          FLT1=filt(LP, 1100, 20, 0, 55, 65), AMP=adsr(2, 220, 0, 110),
          sends=dict(delay=19, reverb=30), delay=dly(DOT8, 40, 4500), reverb=rev(60, 2.5)),
    patch("Hoover Stab", "STAB", voices=3, banks=(13, None),           # 13 = SYNC WIDE
          OSC1=ob(50, level=100), OSC2=o2(SAW, 0, 10, 50),
          FLT1=filt(LP, 2800, 25, 25, 55, 50), AMP=adsr(3, 260, 0, 130),
          sends=dict(delay=30, reverb=35), delay=dly(DOT8, 40, 4500), reverb=rev(65, 3.0)),
    patch("Dub Chord", "STAB", voices=3, banks=(92, None),             # 92 = CHORD
          OSC1=ob(70, level=100), OSC2=o2(SAW, 0, 6, 20),
          FLT1=filt(LP, 1400, 22, 0, 50, 45), AMP=adsr(4, 380, 0, 200),
          sends=dict(delay=65, reverb=45), delay=dly(DOT8, 65, 3500), reverb=rev(75, 4.0, 3500)),
    patch("Detroit Stab", "STAB", voices=4, banks=(136, None),         # 136 = FM 1:3
          OSC1=ob(40, level=100), OSC2=o2(SAW, 0, 5, 20),
          FLT1=filt(LP, 3500, 10, 0, 55, 40), AMP=adsr(2, 260, 0, 150),
          sends=dict(reverb=40, chorus=35), reverb=rev(70, 3.2), chorus=cho(0.6, 40)),
    patch("Minor Stab", "STAB", voices=3,
          OSC1=o1(SAW, -8, 100), OSC2=o2(SAW, 3, 8, 80),
          FLT1=filt(LP, 1300, 18, 0, 55, 60), AMP=adsr(2, 230, 0, 120),
          sends=dict(delay=19, reverb=30), delay=dly(DOT8, 40, 4500), reverb=rev(60, 2.5)),
    patch("Organ Stab", "STAB", voices=4, banks=(93, None),         # 93 = ORGAN JAZZ
          OSC1=ob(60, level=100), OSC2=o2(SQR, 0, 0, 20, OCT=1),
          FLT1=filt(LP, 5000, 8, 0, 55, 25), AMP=adsr(2, 200, 0, 100),
          sends=dict(delay=16, chorus=40), delay=dly(E8, 35, 4500), chorus=cho(0.7, 35)),
    patch("Offbeat Square Stab", "STAB", voices=2,
          OSC1=o1(SQR, -5, 100), OSC2=o2(SAW, 0, 5, 70),
          FLT1=filt(LP, 1000, 15, 0, 50, 70), AMP=adsr(2, 120, 0, 70, level=77),
          sends=dict(delay=25, reverb=20), delay=dly(E8, 35, 4000), reverb=rev(50, 1.8)),

    # ================= pads (6)
    patch("Trance Strings", "PAD", voices=3,
          OSC1=o1(SAW, -12, 80), OSC2=o2(SAW, 0, 12, 80),
          FLT1=filt(LP, 2500, 6, 0, 40, 0), GRIT=dict(DRIFT=pct(20)), AMP=adsr(700, 700, 90, 1500, vel=30, level=49),
          sends=dict(reverb=50, chorus=85), reverb=rev(85, 5.0, 4500, 30), chorus=cho(0.45, 65, 14)),
    patch("Dark Warehouse Pad", "PAD", voices=3,
          OSC1=o1(SAW, -14, 80), OSC2=o2(SAW, 0, 14, 70, OCT=-1),
          FLT1=filt(LP, 700, 20, 0, 35, 0), AMP=adsr(1200, 900, 90, 2000, vel=30, level=47),
          MOD=dict(LFO_RATE=lfo_hz(0.2), LFO_DEPTH=pct(35), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(delay=20, reverb=60, chorus=40), delay=dly(Q4, 45, 2500), reverb=rev(90, 6.5, 3000, 40), chorus=cho(0.3, 50)),
    patch("Evolving Pad", "PAD", voices=3,
          OSC1=o1(SAW, -8, 80), OSC2=o2(SQR, 12, 8, 50),
          FLT1=filt(LP, 1200, 40, 0, 30, 0), AMP=adsr(1600, 1000, 90, 2300, vel=30, level=41),
          MOD=dict(LFO_RATE=lfo_hz(0.2), LFO_DEPTH=pct(60), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(delay=35, reverb=60, chorus=55), delay=dly(Q4, 45, 3500), reverb=rev(88, 6.0, 4000, 35), chorus=cho(0.35, 50)),
    patch("Detroit Pad", "PAD", voices=4, banks=(18, None),            # 18 = SPREAD 5
          OSC1=ob(50, level=100), OSC2=o2(SAW, 0, 8, 30),
          FLT1=filt(LP, 3500, 8, 0, 45, 0), AMP=adsr(800, 800, 90, 1800, vel=30),
          sends=dict(reverb=60, chorus=70), reverb=rev(85, 5.0, 5000, 30), chorus=cho(0.5, 60)),
    patch("Choir Pad Trance", "PAD", voices=4, banks=(48, None),       # 48 = CHOIR WIDE
          OSC1=ob(50, level=100),
          FLT1=filt(LP, 5000, 6, 0, 50, 0), AMP=adsr(600, 900, 90, 2000, vel=30),
          sends=dict(reverb=70, chorus=55), reverb=rev(92, 7.0, 4000, 40), chorus=cho(0.35, 50)),
    patch("Glass Pad Techno", "PAD", voices=3, banks=(64, None),       # 64 = GLASS
          OSC1=ob(40, level=100), OSC2=o2(SIN, 0, 0, 25, OCT=1),
          FLT1=filt(LP, 7000, 5, 0, 60, 0), AMP=adsr(500, 900, 85, 1800, vel=30, level=58),
          sends=dict(delay=30, reverb=75), delay=dly(DOT8, 40, 5000), reverb=rev(90, 6.0, 7000, 35)),

    # ================= chords (3): the stack costs voices
    patch("Supersaw Stab", "STAB", voices=2,
          OSC1=o1(SAW, level=100), SUB=dict(UNISON=3, UNI_DET=pct(50), UNI_BLND=pct(70), WIDTH=pct(80)),
          FLT1=filt(LP, 1800, 12, 0, 55, 60), AMP=adsr(2, 280, 0, 160),
          sends=dict(delay=19, reverb=30), delay=dly(DOT8, 40, 4500), reverb=rev(65, 2.8)),
    patch("Uplift Chords", "POLY", voices=2,
          OSC1=o1(SAW, -9, 90), OSC2=o2(SAW, 0, 9, 90),
          SUB=dict(UNISON=2, UNI_DET=pct(20), UNI_BLND=pct(60), WIDTH=pct(60)),
          FLT1=filt(LP, 3000, 8, 0, 55, 20), AMP=adsr(8, 500, 80, 350, level=36),
          sends=dict(delay=30, reverb=40, chorus=30), delay=dly(DOT8, 40, 4500), reverb=rev(70, 3.5), chorus=cho(0.5, 40)),
    patch("Open Hat", "PLUCK", voices=2,
          OSC1=dict(LEVEL=0), SUB=dict(NOISE=pct(100)),
          FLT1=filt(HP, 6500, 10, 0, 0, 0), AMP=adsr(2, 380, 0, 250, vel=60, level=71),
          sends=dict(reverb=15), reverb=rev(40, 1.0, 8000, 5)),
    # ================= effects and textures (5)
    patch("Noise Downlifter", "FX", voices=1,     # a falling sweep is deterministic (Env2 restarts on every note); a rising one would depend on a free-running LFO
          OSC1=dict(LEVEL=0), SUB=dict(NOISE=pct(100)),
          FLT1=filt(LP, 300, 25, 0, 0, 100), MOD=dict(ENV_AMT=pct(100)), AMP=adsr(2, 2500, 0, 800, vel=0),
          sends=dict(reverb=45), reverb=rev(85, 4.0, 4500, 30)),
    patch("Swell Up", "FX", voices=1,
          OSC1=o1(SAW, -10, 80), OSC2=o2(SAW, 0, 10, 80), SUB=dict(NOISE=pct(20)),
          FLT1=filt(LP, 1200, 30, 0, 0, 0), AMP=adsr(2500, 800, 100, 600, vel=0, level=51),
          sends=dict(delay=25, reverb=45), delay=dly(DOT8, 45, 4500), reverb=rev(75, 3.5)),
    patch("Impact Boom", "FX", voices=1,
          OSC1=o1(SIN, level=100, OCT=-1), SUB=dict(SUB_LVL=pct(80), SUB_OCT=-1, NOISE=pct(30)),
          FLT1=filt(LP, 400, 10, 0, 0, 40), AMP=adsr(2, 1500, 0, 900, level=45),
          sends=dict(reverb=85), reverb=rev(95, 6.0, 3000, 20)),
    patch("Clap Burst", "FX", voices=2,
          OSC1=dict(LEVEL=0), SUB=dict(NOISE=pct(100)),
          FLT1=filt(BP, 1300, 30, 0, 0, 0), AMP=adsr(1, 110, 0, 70),
          sends=dict(reverb=45), reverb=rev(60, 1.2, 6000, 10)),
    patch("Metal Drone", "TEXTURE", voices=2, banks=(83, None),        # 83 = DENSE METAL
          OSC1=ob(40, level=100), OSC2=o2(SAW, 0, 7, 80, OCT=-1),
          FLT1=filt(LP, 2500, 30, 0, 30, 0), AMP=adsr(1500, 1000, 90, 2500, vel=20),
          MOD=dict(LFO_RATE=lfo_hz(0.3), LFO_DEPTH=pct(40), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(delay=25, reverb=70), delay=dly(Q4, 50, 3000), reverb=rev(90, 6.5, 3500, 30)),

    # ================= bell (1)
    patch("Trance Bell", "BELL", voices=2, banks=(74, None),           # 74 = FM BELL
          OSC1=ob(60, level=100), OSC2=o2(SIN, 0, 0, 15, OCT=2),
          FLT1=filt(LP, 9000, 5, 0, 60, 10), AMP=adsr(2, 1300, 0, 1100, level=56),
          sends=dict(delay=40, reverb=65), delay=dly(DOT8, 45, 6000), reverb=rev(80, 5.0, 7000, 30)),
]
