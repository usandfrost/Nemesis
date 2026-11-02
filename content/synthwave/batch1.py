# Synthwave, batch 1: ten patches across the types (tools/genre_presets.py builds them; ids 200-209: only ever append to a batch, a patch's position is its id).
# Engine facts these lean on (docs/PATCH_MAP.md): the filter envelope has a 1 ms attack, decays over AMP DECAY x 0.8 + 20 ms and has no sustain, so a
# held pad or brass opens at the note's start and then settles; a slow swell comes from the AMP ATTACK and the LFO. FINE on OSC 1 / OSC 2 is the
# detune that stands in for analog drift. UNISON (SUB page, acts on OSC 1, up to x4) costs voices. No glide, no phaser. A wavetable bank must have a
# converted file (tools/genre_presets.py banks); the table position is PD_SHAPE and the LFO's PD destination moves it for BOTH oscillators.

BASE_ID = 200
PATCHES = [
    # ---- arps and plucks: one note at a time, so two voices let the release of one overlap the next
    patch("Sunset Arp", "SEQ", voices=2,
          OSC1=dict(WAVE=SAW, FINE=-8, LEVEL=127), OSC2=dict(WAVE=SAW, SEMI=0, FINE=8, LEVEL=pct(90)),
          FLT1=dict(MODEL=LADDER, CUTOFF=hz(950), RESO=pct(25), KEYTRK=pct(60), ENV_AMT=pct(65)),
          AMP=dict(ATTACK=ms(2), DECAY=ms(260), SUSTAIN=pct(0), RELEASE=ms(160), VEL_SENS=pct(70), LEVEL=127),
          sends=dict(delay=22, reverb=18, chorus=25),   # delay send like Detroit Seq's (it was 50), reverb send halved (it was 35)
          delay=dict(MODE=DOT8, FEEDBACK=45, TONE=4000, SPREAD=80, HPF=200),
          reverb=dict(SIZE=70, DECAY=3.2, DAMP=5000, PRE_DLY=25),
          chorus=dict(RATE=0.6, DEPTH=40)),

    patch("Pluck Gate", "PLUCK", voices=2, banks=(86, None),     # bank 86 = STRING BRIGHT
          OSC1=dict(PD_SHAPE=40, LEVEL=127),                       # the table position
          OSC2=dict(WAVE=SAW, SEMI=12, FINE=0, LEVEL=pct(45)),
          FLT1=dict(MODEL=LP, CUTOFF=hz(2600), RESO=pct(20), KEYTRK=pct(50), ENV_AMT=pct(60)),
          AMP=dict(ATTACK=ms(2), DECAY=ms(160), SUSTAIN=pct(0), RELEASE=ms(90), LEVEL=127),
          sends=dict(delay=40, reverb=25),
          delay=dict(MODE=DOT16, FEEDBACK=40, TONE=5000),
          reverb=dict(SIZE=55, DECAY=2.0)),

    # ---- pads
    patch("Chrome Pad", "PAD", voices=4,
          OSC1=dict(WAVE=SAW, FINE=-10, LEVEL=pct(65)), OSC2=dict(WAVE=SAW, SEMI=0, FINE=10, LEVEL=pct(65)),
          FLT1=dict(MODEL=LP, CUTOFF=hz(1400), RESO=pct(8), KEYTRK=pct(40), ENV_AMT=pct(20)),
          AMP=dict(ATTACK=ms(700), DECAY=ms(600), SUSTAIN=pct(85), RELEASE=ms(1400), VEL_SENS=pct(40)),
          MOD=dict(LFO_RATE=lfo_hz(0.5), LFO_DEPTH=pct(12), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(delay=20, reverb=55, chorus=85),
          delay=dict(MODE=Q4, FEEDBACK=35, TONE=3000),
          reverb=dict(SIZE=85, DECAY=5.0, DAMP=4500, PRE_DLY=30),
          chorus=dict(RATE=0.5, DEPTH=60, DELAY=14)),

    patch("Neon Wash", "TEXTURE", voices=3,
          OSC1=dict(WAVE=SAW, LEVEL=pct(55)), OSC2=dict(WAVE=SQR, SEMI=7, FINE=-6, LEVEL=pct(35)),
          SUB=dict(NOISE=pct(18)),
          FLT1=dict(MODEL=BP, CUTOFF=hz(1200), RESO=pct(35), KEYTRK=pct(30), ENV_AMT=pct(10)),
          AMP=dict(ATTACK=ms(1800), DECAY=ms(1200), SUSTAIN=pct(90), RELEASE=ms(2400), VEL_SENS=pct(20)),
          MOD=dict(LFO_RATE=lfo_hz(0.3), LFO_DEPTH=pct(45), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(delay=30, reverb=75, chorus=60),
          delay=dict(MODE=Q4, FEEDBACK=50, TONE=2500),
          reverb=dict(SIZE=95, DECAY=8.0, DAMP=3500, PRE_DLY=40),
          chorus=dict(RATE=0.3, DEPTH=70)),

    # ---- bass
    patch("Dark Pulse Bass", "BASS", voices=1,
          OSC1=dict(WAVE=SQR, OCT=-1), OSC2=dict(WAVE=SAW, SEMI=0, OCT=-1, FINE=6, LEVEL=pct(55)),
          SUB=dict(SUB_LVL=pct(45), SUB_OCT=-1),
          FLT1=dict(MODEL=LADDER, CUTOFF=hz(420), RESO=pct(28), DRIVE=pct(22), KEYTRK=pct(55), ENV_AMT=pct(45)),
          AMP=dict(ATTACK=ms(2), DECAY=ms(170), SUSTAIN=pct(55), RELEASE=ms(90))),

    # ---- lead
    patch("Lasergrid Lead", "LEAD", voices=2, banks=(131, 131),     # bank 131 = PWM on both oscillators: the LFO moves one position for both
          OSC1=dict(PD_SHAPE=30), OSC2=dict(PD_SHAPE=30, SEMI=0, FINE=7, LEVEL=pct(70)),
          FLT1=dict(MODEL=LP, CUTOFF=hz(3000), RESO=pct(18), KEYTRK=pct(70), ENV_AMT=pct(25)),
          AMP=dict(ATTACK=ms(4), DECAY=ms(400), SUSTAIN=pct(80), RELEASE=ms(220)),
          MOD=dict(LFO_RATE=lfo_hz(4), LFO_DEPTH=pct(35), LFO_DEST=LFO_PD, LFO_SHAPE=TRI),
          sends=dict(delay=55, reverb=30, chorus=25),
          delay=dict(MODE=DOT8, FEEDBACK=40, TONE=4500),
          reverb=dict(SIZE=60, DECAY=2.8),
          chorus=dict(RATE=0.7, DEPTH=35)),

    # ---- brass stab
    patch("Highway Stab", "STAB", voices=3,
          OSC1=dict(WAVE=SAW, FINE=-6, LEVEL=pct(70)), OSC2=dict(WAVE=SAW, SEMI=0, FINE=7, LEVEL=pct(70)),
          FLT1=dict(MODEL=LP, CUTOFF=hz(650), RESO=pct(10), KEYTRK=pct(50), ENV_AMT=pct(70)),
          AMP=dict(ATTACK=ms(18), DECAY=ms(350), SUSTAIN=pct(40), RELEASE=ms(150), VEL_SENS=pct(60)),
          sends=dict(reverb=30, chorus=30),
          reverb=dict(SIZE=60, DECAY=2.2),
          chorus=dict(RATE=0.8, DEPTH=30)),

    # ---- supersaw: the stack costs voices, the budget will trim it
    patch("Supersaw Skyline", "POLY", voices=2,
          OSC1=dict(WAVE=SAW, LEVEL=pct(100)), OSC2=dict(LEVEL=0),
          SUB=dict(UNISON=4, UNI_DET=pct(55), UNI_BLND=pct(70), WIDTH=pct(80)),
          FLT1=dict(MODEL=LP, CUTOFF=hz(2200), RESO=pct(5), KEYTRK=pct(50), ENV_AMT=pct(15)),
          AMP=dict(ATTACK=ms(12), DECAY=ms(500), SUSTAIN=pct(80), RELEASE=ms(450)),
          sends=dict(delay=15, reverb=45, chorus=35),
          delay=dict(MODE=DOT8, FEEDBACK=35),
          reverb=dict(SIZE=80, DECAY=4.0),
          chorus=dict(RATE=0.6, DEPTH=45)),

    # ---- keys
    patch("Midnight EP", "KEYS", voices=3, banks=(135, None),      # bank 135 = FM 1:1
          OSC1=dict(PD_SHAPE=30),
          OSC2=dict(WAVE=SIN, OCT=1, SEMI=0, FINE=0, LEVEL=pct(25)),
          FLT1=dict(MODEL=LP, CUTOFF=hz(3500), RESO=pct(5), KEYTRK=pct(60), ENV_AMT=pct(30)),
          AMP=dict(ATTACK=ms(2), DECAY=ms(700), SUSTAIN=pct(20), RELEASE=ms(350), VEL_SENS=pct(90)),
          sends=dict(reverb=25, chorus=70),
          reverb=dict(SIZE=55, DECAY=2.4),
          chorus=dict(RATE=0.9, DEPTH=50)),

    # ---- bell
    patch("Glass Chime", "BELL", voices=2, banks=(75, None),       # bank 75 = FM GLASS
          OSC1=dict(PD_SHAPE=64),
          OSC2=dict(WAVE=SIN, OCT=1, SEMI=0, FINE=0, LEVEL=pct(20)),
          FLT1=dict(MODEL=LP, CUTOFF=hz(8000), RESO=pct(5), KEYTRK=pct(60), ENV_AMT=pct(10)),
          AMP=dict(ATTACK=ms(2), DECAY=ms(1400), SUSTAIN=pct(0), RELEASE=ms(1200)),
          sends=dict(delay=35, reverb=60),
          delay=dict(MODE=DOT8, FEEDBACK=45, TONE=6000),
          reverb=dict(SIZE=75, DECAY=4.5, DAMP=7000)),
]
