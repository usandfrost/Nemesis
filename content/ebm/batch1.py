# EBM, batch 1: fifty patches (ids 450-499; only ever append, a patch's position is its id). tools/genre_presets.py builds them (--set ebm).
# The style: hard sequenced analog bass in 16ths, dry square and saw leads, short brass-like stabs, metallic and noise percussion, cold pads and alarms. Same engine facts as
# content/techno/batch1.py: the filter envelope decays with the amp decay and has no sustain; FINE is the detune; UNISON acts on OSC 1; no glide, no pitch envelope (so no
# kick drum; the toms are filter pings); the LFO is free-running; hard sync has OSC 2 as the master (sets the pitch) and OSC 1 as the slave; OSC 2's pitch is always written
# out. Percussion is noise or metallic-bank bursts for a sequencer lane. The SOVIET filter is the LP model meant for the bitey MS-20 style resonance.
# Levels start at the top (AMP LEVEL 127) and content/tools/calibrate_levels.py lowers the loud ones to the library's level.

BASE_ID = 450

PATCHES = [
    # ================= sequenced basses (14): one voice
    patch("Body Bass", "BASS", voices=1,
          OSC1=o1(SAW, level=100, OCT=-1), OSC2=o2(SQR, 0, 0, 70, OCT=-1),
          FLT1=filt(SOVIET, 750, 30, 0, 55, 40), AMP=adsr(1, 220, 40, 50)),
    patch("MS Bite Bass", "BASS", voices=1,
          OSC1=o1(SQR, level=100, OCT=-1),
          FLT1=filt(SOVIET, 520, 58, 0, 60, 60), GRIT=dict(CLIP=pct(20)), AMP=adsr(1, 130, 20, 40, level=62)),
    patch("Dry Square Seq", "BASS", voices=1,
          OSC1=o1(SQR, level=100),
          FLT1=filt(LP, 600, 20, 0, 50, 35), AMP=adsr(1, 250, 70, 40, level=32)),
    patch("Octave Pump Bass", "BASS", voices=1,
          OSC1=o1(SAW, level=100, OCT=-1), OSC2=o2(SAW, 0, 0, 55),
          FLT1=filt(CLASSIC, 950, 22, 0, 55, 45), AMP=adsr(1, 170, 30, 45)),
    patch("Stomp Bass", "BASS", voices=1,
          OSC1=o1(SAW, level=100, OCT=-1),
          FLT1=filt(LADDER, 380, 35, 0, 55, 70), GRIT=dict(CLIP=pct(30)), GLOB=dict(MASTER=127), AMP=adsr(1, 95, 0, 35)),
    patch("Pulse Machine Bass", "BASS", voices=1, banks=(7, None),     # 7 = PWM NARROW
          OSC1=ob(40, level=100, OCT=-1),
          FLT1=filt(LP, 700, 25, 0, 55, 40), AMP=adsr(1, 200, 50, 45, level=78)),
    patch("FM Iron Bass", "BASS", voices=1, banks=(135, None),         # 135 = FM 1:1
          OSC1=ob(70, level=100), OSC2=o2(SAW, 0, 4, 35),
          FLT1=filt(LP, 1300, 15, 0, 55, 35), AMP=adsr(1, 180, 40, 45, level=66)),
    patch("Fold Bass EBM", "BASS", voices=1, banks=(183, None),        # 183 = FOLD SLOW
          OSC1=ob(40, level=100, OCT=-1),
          FLT1=filt(LP, 900, 25, 20, 55, 45), AMP=adsr(1, 170, 35, 45)),
    patch("Sub Stomp", "BASS", voices=1,
          OSC1=o1(SIN, level=100), SUB=dict(SUB_LVL=pct(45), SUB_OCT=-1),
          FLT1=filt(LP, 420, 8, 0, 30, 25), GRIT=dict(CLIP=pct(35)), GLOB=dict(MASTER=127), AMP=adsr(1, 160, 30, 50, level=36)),
    patch("Raw Saw Bass", "BASS", voices=1,
          OSC1=o1(SAW, -8, 100), OSC2=o2(SAW, 0, 8, 100),
          FLT1=filt(LP, 1000, 15, 0, 55, 40), AMP=adsr(1, 260, 60, 50, level=45)),
    patch("Acid Hammer", "BASS", voices=1,
          OSC1=o1(SAW, level=100, OCT=-1), OSC2=o2(SQR, 0, 0, 100, OCT=-1),
          FLT1=filt(ACID, 800, 45, 70, 50, 55), GRIT=dict(CLIP=pct(65)), GLOB=dict(MASTER=127), AMP=adsr(1, 140, 20, 40)),
    patch("Long Square Bass", "BASS", voices=1,
          OSC1=o1(SQR, level=100), OSC2=o2(SQR, 0, 6, 60),
          FLT1=filt(LP, 700, 18, 0, 50, 15), AMP=adsr(4, 400, 90, 150, level=23)),
    patch("Hollow Bass", "BASS", voices=1, banks=(101, None),          # 101 = HOLLOW
          OSC1=ob(50, level=100),
          FLT1=filt(LP, 800, 20, 0, 55, 35), AMP=adsr(1, 200, 45, 50, level=58)),
    patch("Distorted Bass", "BASS", voices=1,
          OSC1=o1(SAW, level=100), OSC2=o2(SQR, 0, 0, 80),
          FLT1=filt(LP, 900, 20, 70, 55, 40), GRIT=dict(CLIP=pct(70)), GLOB=dict(MASTER=127), AMP=adsr(1, 190, 40, 50, level=28)),

    # ================= leads (8)
    patch("Cold Saw Lead", "LEAD", voices=1,
          OSC1=o1(SAW, -7, 100), OSC2=o2(SAW, 0, 7, 100),
          FLT1=filt(LP, 3200, 15, 0, 60, 20), AMP=adsr(2, 300, 80, 120, level=32),
          sends=dict(delay=20, reverb=15), delay=dly(DOT8, 35, 4000), reverb=rev(45, 1.8)),
    patch("Brute Square Lead", "LEAD", voices=1,
          OSC1=o1(SQR, level=100), OSC2=o2(SQR, 12, 0, 50),
          FLT1=filt(LP, 2600, 20, 0, 60, 25), AMP=adsr(2, 250, 85, 100, level=22)),
    patch("Sync Machine Lead", "LEAD", voices=1,
          OSC1=o1(SAW, level=85, SEMI=5), OSC2=o2(SAW, 0, 0, 15, SYNC=pct(70)),   # OSC 2 is the sync MASTER (sets the pitch, quiet), OSC 1 the slave
          FLT1=dict(filt(LP, 3800, 20, 0, 70, 25), HPF=hpf_hz(40)), AMP=adsr(2, 280, 85, 100, level=59)),   # the HPF takes the sync reset's low rumble out
    patch("Hard Pulse Lead", "LEAD", voices=1, banks=(131, None),      # 131 = PWM
          OSC1=ob(35, level=100),
          FLT1=filt(LP, 3000, 18, 0, 60, 20), AMP=adsr(2, 300, 80, 110, level=37)),
    patch("Chip Lead EBM", "LEAD", voices=1, banks=(130, None),        # 130 = CHIP
          OSC1=ob(60, level=100), OSC2=o2(SQR, 12, 0, 30),
          FLT1=filt(LP, 5000, 10, 0, 60, 15), AMP=adsr(2, 200, 85, 80, level=28)),
    patch("Fold Lead", "LEAD", voices=1, banks=(152, None),            # 152 = FOLD
          OSC1=ob(55, level=100),
          FLT1=filt(LP, 3500, 18, 20, 60, 25), AMP=adsr(2, 300, 80, 110, level=40)),
    patch("Wobble Lead", "LEAD", voices=1,
          OSC1=o1(SAW, level=100), OSC2=o2(SAW, 0, 8, 70),
          FLT1=filt(LP, 1800, 40, 0, 60, 15), AMP=adsr(8, 300, 90, 150, level=27),
          MOD=dict(LFO_RATE=lfo_hz(4.0), LFO_DEPTH=pct(60), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI)),   # the LFO moves the pitch by under a semitone at most: the cutoff wobbles instead
    patch("Formant Lead", "LEAD", voices=1, banks=(31, None),          # 31 = FORMANT
          OSC1=ob(50, level=100),
          FLT1=filt(LP, 5000, 10, 0, 60, 10), AMP=adsr(6, 300, 85, 150, level=49),
          sends=dict(reverb=20), reverb=rev(50, 2.0)),

    # ================= stabs (8)
    patch("Hard Brass Stab", "STAB", voices=4,
          OSC1=o1(SAW, -9, 100), OSC2=o2(SAW, 0, 9, 100),
          FLT1=filt(LP, 1500, 15, 0, 55, 65), AMP=adsr(4, 260, 0, 90, level=73)),
    patch("Octave Stab", "STAB", voices=2,
          OSC1=o1(SAW, level=100), OSC2=o2(SAW, 12, 0, 70),
          FLT1=filt(LP, 2000, 15, 0, 55, 60), AMP=adsr(2, 200, 0, 70)),
    patch("Pulse Chord Stab", "STAB", voices=4, banks=(131, None),    # 131 = PWM
          OSC1=ob(30, level=100), OSC2=o2(SQR, 0, 6, 50),
          FLT1=filt(LP, 2200, 18, 0, 55, 50), AMP=adsr(1, 130, 0, 40, level=78)),
    patch("Power Fifth Stab", "STAB", voices=2,
          OSC1=o1(SAW, level=100), OSC2=o2(SAW, 7, 0, 80),
          FLT1=filt(LP, 1700, 18, 0, 55, 60), AMP=adsr(2, 170, 0, 60)),
    patch("Clang Stab", "STAB", voices=3, banks=(76, None),            # 76 = FM CLANG
          OSC1=ob(55, level=100), OSC2=o2(SAW, 0, 7, 40),
          FLT1=filt(LP, 3000, 15, 0, 55, 45), AMP=adsr(1, 220, 0, 100),
          sends=dict(reverb=20), reverb=rev(50, 2.0)),
    patch("Noise Stab", "STAB", voices=3,
          OSC1=o1(SAW, level=100), SUB=dict(NOISE=pct(35)),
          FLT1=filt(LP, 2500, 20, 0, 55, 60), AMP=adsr(1, 150, 0, 60)),
    patch("Reed Stab", "STAB", voices=4, banks=(98, None),             # 98 = ODD ONLY
          OSC1=ob(50, level=100), OSC2=o2(SQR, 0, 5, 50),
          FLT1=filt(LP, 2200, 15, 0, 55, 45), AMP=adsr(2, 230, 0, 80)),
    patch("Fall Stab", "STAB", voices=3,
          OSC1=o1(SAW, -10, 100), OSC2=o2(SQR, 0, 10, 80),
          FLT1=filt(LADDER, 900, 30, 0, 55, 85), GRIT=dict(CLIP=pct(30)), GLOB=dict(MASTER=127), AMP=adsr(4, 520, 0, 250, level=76),
          sends=dict(reverb=25), reverb=rev(60, 2.5)),

    # ================= sequence and pluck voices (6): two voices so tails overlap
    patch("Blip Seq", "SEQ", voices=2,
          OSC1=o1(SQR, level=100),
          FLT1=filt(LP, 1800, 25, 0, 60, 55), AMP=adsr(1, 90, 0, 30, level=77)),
    patch("Resonant Blip", "SEQ", voices=2,
          OSC1=o1(SAW, level=100),
          FLT1=filt(SOVIET, 900, 70, 0, 60, 80), AMP=adsr(1, 110, 0, 30)),
    patch("Arp Machine", "SEQ", voices=2,
          OSC1=o1(SAW, level=100), OSC2=o2(SQR, 0, 5, 50),
          FLT1=filt(LADDER, 1300, 30, 0, 60, 60), GRIT=dict(CLIP=pct(35)), GLOB=dict(MASTER=127), AMP=adsr(1, 160, 0, 50),
          sends=dict(delay=25), delay=dly(DOT16, 35, 4000)),
    patch("Metal Seq", "SEQ", voices=2, banks=(83, None),              # 83 = DENSE METAL
          OSC1=ob(45, level=100),
          FLT1=filt(LP, 2500, 15, 0, 40, 20), AMP=adsr(1, 220, 0, 80)),
    patch("FM Bleep", "SEQ", voices=2, banks=(132, None),              # 132 = 2-OP
          OSC1=ob(55, level=100),
          FLT1=filt(LP, 5000, 10, 0, 60, 30), AMP=adsr(1, 100, 0, 40)),
    patch("Noise Pluck", "PLUCK", voices=2,
          OSC1=o1(SAW, level=100), SUB=dict(NOISE=pct(25)),
          FLT1=filt(LP, 2000, 25, 0, 55, 70), AMP=adsr(1, 120, 0, 40, level=67)),

    # ================= percussion (8): noise and metal for sequencer lanes; the toms are filter pings (no pitch envelope here)
    patch("Noise Snare", "PLUCK", voices=2,
          OSC1=o1(TRI, level=40), SUB=dict(NOISE=pct(90)),
          FLT1=filt(HP, 900, 15, 0, 30, 0), AMP=adsr(1, 170, 0, 90, vel=60),
          sends=dict(reverb=20), reverb=rev(40, 1.2, 6000, 5)),
    patch("Rim Shot", "PLUCK", voices=2,
          OSC1=o1(SQR, level=60), SUB=dict(NOISE=pct(40)),
          FLT1=filt(BP, 1800, 40, 0, 30, 0), AMP=adsr(1, 40, 0, 25, vel=60),
          sends=dict(reverb=15), reverb=rev(40, 1.2, 6000, 5)),
    patch("Ring Hat", "PLUCK", voices=2, banks=(83, None),             # 83 = DENSE METAL
          OSC1=ob(90, level=100), SUB=dict(NOISE=pct(40)),
          FLT1=filt(HP, 6000, 10, 0, 0, 0), AMP=adsr(1, 70, 0, 35, vel=60)),
    patch("Noise Crash", "FX", voices=2,
          OSC1=dict(LEVEL=0), SUB=dict(NOISE=pct(100)),
          FLT1=filt(HP, 3000, 10, 0, 0, 0), AMP=adsr(1, 1200, 0, 700, vel=60),
          sends=dict(reverb=35), reverb=rev(70, 3.0, 6000, 10)),
    patch("Metal Hit", "PLUCK", voices=2, banks=(76, None),            # 76 = FM CLANG
          OSC1=ob(80, level=100), SUB=dict(NOISE=pct(20)),
          FLT1=filt(HP, 400, 10, 0, 0, 0), AMP=adsr(1, 280, 0, 150, vel=50, level=76),
          sends=dict(reverb=25), reverb=rev(55, 2.0)),
    patch("Anvil Clang", "FX", voices=2, banks=(59, None),             # 59 = GONG
          OSC1=ob(70, level=100), SUB=dict(NOISE=pct(10)),
          FLT1=filt(HP, 250, 10, 0, 0, 0), AMP=adsr(1, 900, 0, 600, vel=50),
          sends=dict(reverb=45), reverb=rev(75, 3.5, 4500, 15)),
    patch("Low Filter Ping", "PLUCK", voices=2,
          OSC1=o1(SIN, level=100), SUB=dict(NOISE=pct(8)),
          FLT1=filt(LP, 130, 88, 0, 70, 80), AMP=adsr(1, 380, 0, 160, level=28),
          sends=dict(reverb=20), reverb=rev(50, 1.5, 3500, 10)),
    patch("High Filter Ping", "PLUCK", voices=2,
          OSC1=o1(SIN, level=100), SUB=dict(NOISE=pct(8)),
          FLT1=filt(LP, 260, 88, 0, 70, 80), AMP=adsr(1, 300, 0, 130, level=42),
          sends=dict(reverb=20), reverb=rev(50, 1.5, 3500, 10)),

    # ================= pads, textures and effects (6)
    patch("Cold Pad", "PAD", voices=3,
          OSC1=o1(SAW, -10, 80), OSC2=o2(SAW, 0, 10, 80),
          FLT1=filt(LP, 1800, 12, 0, 40, 0), AMP=adsr(500, 700, 90, 1200, vel=30, level=45),
          sends=dict(reverb=45, chorus=35), reverb=rev(75, 4.0, 3500, 30), chorus=cho(0.35, 40)),
    patch("Dark Machine Strings", "PAD", voices=3,
          OSC1=o1(SAW, -12, 80), OSC2=o2(SAW, 0, 12, 80, OCT=-1),
          FLT1=filt(LP, 1200, 20, 0, 40, 0), AMP=adsr(900, 800, 90, 1800, vel=30, level=46),
          sends=dict(reverb=50, chorus=45), reverb=rev(80, 5.0, 3500, 30), chorus=cho(0.3, 50)),
    patch("Machine Drone", "TEXTURE", voices=2,
          OSC1=o1(SAW, -14, 100, OCT=-1), OSC2=o2(SQR, 0, 14, 70, OCT=-1),
          FLT1=filt(LP, 600, 30, 0, 30, 0), AMP=adsr(600, 900, 100, 1500, vel=0, level=50),
          MOD=dict(LFO_RATE=lfo_hz(0.3), LFO_DEPTH=pct(50), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(reverb=40), reverb=rev(80, 4.5, 3000, 30)),
    patch("Noise Wash", "TEXTURE", voices=2,
          OSC1=dict(LEVEL=0), SUB=dict(NOISE=pct(100)),
          FLT1=filt(LP, 1500, 40, 0, 0, 0), AMP=adsr(900, 900, 100, 1500, vel=0),
          MOD=dict(LFO_RATE=lfo_hz(0.3), LFO_DEPTH=pct(70), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(reverb=50), reverb=rev(85, 5.0, 4000, 30)),
    patch("Alarm Tone", "FX", voices=1,
          OSC1=o1(SQR, level=100),
          FLT1=filt(LP, 2800, 15, 0, 50, 0), AMP=adsr(3, 200, 100, 150, level=22),
          MOD=dict(LFO_RATE=lfo_hz(2.5), LFO_DEPTH=pct(100), LFO_DEST=LFO_AMP, LFO_SHAPE=SQR),   # beeps: the LFO moves the pitch by under a semitone at most, so the alarm is gated instead
          sends=dict(reverb=20), reverb=rev(50, 1.8)),
    patch("Choir Stab", "STAB", voices=3, banks=(49, None),           # 49 = CHOIR MEN
          OSC1=ob(50, level=100),
          FLT1=filt(LP, 3500, 10, 0, 50, 25), AMP=adsr(8, 280, 0, 150),
          sends=dict(delay=25, reverb=30), delay=dly(DOT8, 40, 3500), reverb=rev(65, 2.8)),
]
