# Röyksopp, batch 1: fifty patches (ids 600-649; only ever append, a patch's position is its id). tools/genre_presets.py builds them (--set royksopp).
# Source: the owner's Röyksopp notes and the gear research in the upstream project (ref/upstream/docs/dev/sound-design/REFERENCES.md and REFERENCE-RECIPES.md 1.15, 4.x):
# the band's own favourites are the Korg MS-20 (rough, resonant: here the SOVIET filter), the Juno-106 (rounded, with a slow chorus), the Mellotron (tape strings, choir,
# flute: here the string/choir/breath banks with the converter at 12 bits / 28 kHz and DRIFT), the Yamaha CS-80 (brass and chords); the Akai S1000 crunch (GRIT bits and
# rate), a Space Echo (a dark, tempo-synced delay with a high-passed feedback) and a plate or hall behind it all. These are recreations from those notes, not studio settings.
# Engine facts (docs/PATCH_MAP.md): the filter envelope decays with the amp decay and has no sustain; FINE is the detune; UNISON sits on the SUB page and acts on OSC 1; no
# glide, no pitch envelope, no phaser; the LFO is free-running, its slowest rate is 0.2 Hz and LFO -> pitch stays under a semitone (vibrato, not sirens); WOW has no
# counterpart (DRIFT does the slow instability); chord and pad patches keep voices at 3 with WIDTH 0 so the CPU budget does not trim them (the chorus makes them wide).
# OSC 2's pitch is always written out. Levels start at the top (AMP LEVEL 127) and content/tools/calibrate_levels.py lowers the loud ones to the library's level.

BASE_ID = 600

# the "sampler/tape grain" of the Mellotron and Akai patches: 12 bits at about 28 kHz (GRIT RATE 80), mild
def grain(drift=25, bits=12, rate=80):
    return dict(BITS=bits, RATE=rate, DRIFT=pct(drift))

PATCHES = [
    # ================= basses (8): one voice, dry, mono low end
    patch("MS-20 Snarl Bass", "BASS", voices=1,
          OSC1=o1(SAW, level=100), OSC2=o2(SQR, 0, 5, 70),
          FLT1=filt(SOVIET, 700, 58, 0, 50, 55), GRIT=dict(CLIP=pct(18)), AMP=adsr(1, 260, 35, 70, level=44)),
    patch("Rubber Reso Bass", "BASS", voices=1,       # Vision One: a resonant, rubbery saw bass, round and a little dirty (no glide on this engine)
          OSC1=o1(SAW, level=100), OSC2=o2(SAW, 0, 5, 80), SUB=dict(SUB_LVL=pct(35), SUB_OCT=-1),
          FLT1=filt(LP, 380, 45, 0, 40, 55), GRIT=dict(CLIP=pct(20)), AMP=adsr(3, 350, 75, 120, level=41)),
    patch("Octave Robot Bass", "BASS", voices=1,           # The Girl and the Robot: a punchy, bright octave bass
          OSC1=o1(SAW, level=100), OSC2=o2(SAW, 12, 0, 70),
          FLT1=filt(CLASSIC, 1700, 20, 0, 55, 40), AMP=adsr(1, 190, 45, 60)),
    patch("Dirty Mid Bass", "BASS", voices=1,              # Do It Again: the dirtiest, gritty mid-bass; sampler crunch on top
          OSC1=o1(SQR, level=100), OSC2=o2(SAW, 0, 6, 80),
          FLT1=filt(ACID, 900, 38, 40, 55, 50), GRIT=dict(CLIP=pct(55), BITS=12, RATE=92), GLOB=dict(MASTER=127), AMP=adsr(1, 200, 30, 50)),
    patch("Monument Sub", "BASS", voices=1,
          OSC1=o1(SIN, level=100), SUB=dict(SUB_LVL=pct(25), SUB_OCT=-1),
          FLT1=filt(LP, 500, 8, 0, 40, 20), AMP=adsr(6, 500, 70, 160, level=47)),
    patch("Forever Pulse Bass", "BASS", voices=1, banks=(131, None),     # 131 = PWM
          OSC1=ob(30, level=100), OSC2=o2(SAW, 0, 4, 40),
          FLT1=filt(SOVIET, 620, 20, 0, 50, 40), AMP=adsr(2, 320, 55, 90, level=45)),
    patch("Sync Pulse Bass", "BASS", voices=1, banks=(15, None),        # 15 = SYNC SLOW
          OSC1=ob(40, level=100), SUB=dict(SUB_LVL=pct(25), SUB_OCT=-1),
          FLT1=filt(LP, 900, 25, 0, 50, 45), AMP=adsr(1, 280, 20, 80)),
    patch("Filter Sweep Bass", "BASS", voices=1,           # a resonant, driven saw: the notes sweep with the decay
          OSC1=o1(SAW, level=100), OSC2=o2(SAW, 0, 7, 90), SUB=dict(SUB_LVL=pct(30), SUB_OCT=-1),
          FLT1=filt(ACID, 650, 60, 25, 45, 75), GRIT=dict(CLIP=pct(40)), GLOB=dict(MASTER=127), AMP=adsr(2, 380, 10, 90)),

    # ================= leads
    patch("Glass Robot Lead", "LEAD", voices=1, banks=(64, None),       # 64 = GLASS
          OSC1=ob(60, level=100), OSC2=o2(SIN, 0, 0, 25, OCT=1),
          FLT1=filt(LP, 6000, 10, 0, 60, 20), AMP=adsr(4, 350, 80, 200, level=60),
          sends=dict(delay=30, reverb=35), delay=dly(DOT8, 35, 4500), reverb=rev(65, 3.0)),
    patch("Icy Saw Lead", "LEAD", voices=2,                # What Else Is There?: an icy, detuned saw lead behind a dark plate
          OSC1=o1(SAW, -8, 100), OSC2=o2(SAW, 0, 8, 100),
          FLT1=filt(LP, 2600, 14, 0, 55, 25), AMP=adsr(40, 500, 80, 400, level=45),
          MOD=dict(LFO_RATE=lfo_hz(4.0), LFO_DEPTH=pct(14), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=35, reverb=45, chorus=30), delay=dly(DOT8, 35, 3500, 80, 220), reverb=rev(80, 3.8, 3800, 30), chorus=cho(0.5, 35)),
    patch("Monument Sax", "LEAD", voices=1, banks=(37, None),           # 37 = NASAL: the reed line over a steady pulse, deep in a long reverb
          OSC1=ob(55, level=100), SUB=dict(NOISE=pct(5)),
          FLT1=filt(LP, 2600, 14, 0, 60, 25), AMP=adsr(25, 450, 85, 400, level=62),
          MOD=dict(LFO_RATE=lfo_hz(5.2), LFO_DEPTH=pct(22), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=25, reverb=65), delay=dly(DOT8, 30, 3200, 80, 250), reverb=rev(85, 4.6, 3800, 35)),
    patch("Vocoder Robot Lead", "LEAD", voices=1, banks=(49, None),     # 49 = CHOIR MEN (the VP-330 vocoder voice)
          OSC1=ob(40, level=100), OSC2=o2(SAW, 0, 5, 40),
          FLT1=filt(LP, 3200, 18, 0, 60, 20), AMP=adsr(15, 400, 80, 220, level=60),
          sends=dict(delay=25, reverb=35, chorus=30), delay=dly(DOT8, 30, 3500), reverb=rev(60, 2.6), chorus=cho(0.6, 35)),
    patch("Fluttering Lead", "LEAD", voices=1,             # Do It Again: the fast, fluttering synth (a quick tremolo on the amplitude)
          OSC1=o1(TRI, level=100), OSC2=o2(SAW, 0, 6, 45),
          FLT1=filt(LP, 3800, 14, 0, 60, 25), AMP=adsr(5, 450, 85, 180, level=55),
          MOD=dict(LFO_RATE=lfo_hz(9.0), LFO_DEPTH=pct(40), LFO_DEST=LFO_AMP, LFO_SHAPE=TRI),
          sends=dict(delay=30, reverb=30), delay=dly(DOT8, 35, 4000), reverb=rev(60, 2.4)),
    patch("Echo Triangle Lead", "LEAD", voices=1,          # a plain lead into a Space Echo: every repeat darker
          OSC1=o1(TRI, level=100), OSC2=o2(SQR, 0, 0, 25, OCT=-1),
          FLT1=filt(LP, 3400, 10, 0, 60, 25), AMP=adsr(6, 350, 80, 200, level=43),
          sends=dict(delay=60, reverb=30), delay=dly(E8, 48, 2800, 90, 280), reverb=rev(60, 2.8, 3500)),

    # ================= pads (10): a slow attack, chorus, a long reverb
    patch("Juno Chorus Pad", "PAD", voices=3,
          OSC1=o1(SAW, -6, 100), OSC2=o2(SAW, 0, 6, 70), SUB=dict(SUB_LVL=pct(35), SUB_OCT=-1),
          FLT1=filt(LP, 2200, 10, 0, 45, 0), AMP=adsr(350, 600, 90, 1100, vel=30, level=41),
          sends=dict(reverb=40, chorus=80), reverb=rev(70, 3.4, 4500, 30), chorus=cho(0.6, 65)),
    patch("Mellotron Strings", "PAD", voices=3, banks=(86, None),       # 86 = STRING BRIGHT; tape grain and a little drift
          OSC1=ob(45, level=100), OSC2=o2(SAW, 0, 8, 30),
          FLT1=filt(LP, 2600, 6, 0, 45, 0), GRIT=grain(30), AMP=adsr(60, 500, 90, 600, vel=30),
          sends=dict(reverb=45, chorus=35), reverb=rev(75, 2.6, 4000, 25), chorus=cho(0.4, 40)),
    patch("Mellotron Choir", "PAD", voices=3, banks=(32, None),         # 32 = CHOIR
          OSC1=ob(45, level=100), OSC2=o2(SAW, 0, 7, 20),
          FLT1=filt(LP, 3200, 6, 0, 45, 0), GRIT=grain(30), AMP=adsr(70, 600, 90, 700, vel=30),
          sends=dict(reverb=50, chorus=30), reverb=rev(80, 3.0, 4000, 30), chorus=cho(0.35, 40)),
    patch("Mellotron Cello", "PAD", voices=2,
          OSC1=o1(SAW, level=100, OCT=-1), OSC2=o2(TRI, 0, 6, 70, OCT=-1),
          FLT1=filt(LP, 1300, 8, 0, 40, 0), GRIT=grain(30), AMP=adsr(80, 600, 90, 500, vel=30, level=51),
          sends=dict(reverb=40, chorus=25), reverb=rev(70, 2.4, 3500, 25), chorus=cho(0.4, 35)),
    patch("CS-80 Brass Swell", "PAD", voices=3,
          OSC1=o1(SAW, -7, 100), OSC2=o2(SAW, 0, 7, 100),
          FLT1=filt(LP, 1800, 22, 0, 55, 10), AMP=adsr(220, 700, 80, 500, vel=60, level=40),
          sends=dict(delay=15, reverb=45, chorus=30), delay=dly(DOT8, 30, 3500), reverb=rev(75, 3.2, 4200, 30), chorus=cho(0.5, 35)),
    patch("String Machine", "PAD", voices=3,        # the RS-505: plain saws, the ensemble chorus does the rest
          OSC1=o1(SAW, -5, 100), OSC2=o2(SAW, 0, 5, 100),
          FLT1=filt(LP, 4200, 4, 0, 40, 0), AMP=adsr(260, 600, 95, 800, vel=30, level=37),
          sends=dict(reverb=35, chorus=85), reverb=rev(65, 2.8, 5000, 25), chorus=cho(0.8, 70, 10)),
    patch("Forever Low Choir", "PAD", voices=3, banks=(49, None),       # 49 = CHOIR MEN under a low saw layer: the heavy pad of Forever
          OSC1=ob(45, level=90), OSC2=o2(SAW, 0, 6, 55, OCT=-1),
          FLT1=filt(LP, 2000, 6, 0, 35, 0), GRIT=grain(20), AMP=adsr(700, 800, 95, 1800, vel=30, level=55),
          sends=dict(reverb=60, chorus=45), reverb=rev(88, 4.6, 3600, 35), chorus=cho(0.35, 45)),
    patch("Breathing Pad", "PAD", voices=3,                  # Monument: a pad that breathes (a slow LFO on the cutoff)
          OSC1=o1(SAW, -6, 80), OSC2=o2(TRI, 0, 6, 80),
          FLT1=filt(LP, 1500, 18, 0, 40, 0), AMP=adsr(900, 800, 100, 1800, vel=30, level=43),
          MOD=dict(LFO_RATE=lfo_hz(0.2), LFO_DEPTH=pct(55), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(reverb=55, chorus=40), reverb=rev(85, 5.0, 4000, 40), chorus=cho(0.4, 40)),
    patch("Dark Plate Pad", "PAD", voices=3,
          OSC1=o1(SAW, -10, 80), OSC2=o2(SAW, 0, 10, 70),
          FLT1=filt(LP, 1100, 10, 0, 40, 0), AMP=adsr(500, 700, 95, 1500, vel=30, level=46),
          sends=dict(reverb=70, chorus=20), reverb=rev(80, 3.4, 3200, 25), chorus=cho(0.3, 30)),
    patch("Quartet Strings", "PAD", voices=3, banks=(86, None),         # Profound Mysteries: real-sounding strings, a long hall
          OSC1=ob(65, level=100), SUB=dict(NOISE=pct(3)),
          FLT1=filt(LP, 4000, 4, 0, 45, 0), AMP=adsr(380, 700, 90, 1100, vel=40),
          sends=dict(reverb=55, chorus=15), reverb=rev(85, 4.0, 5000, 35), chorus=cho(0.35, 25)),

    # ================= arps and sequences (7)
    patch("Narrow Pulse Arp", "SEQ", voices=2, banks=(131, None),       # 131 = PWM, a thin narrow pulse: a Juno-style pulse arp
          OSC1=ob(12, level=100), OSC2=o2(SAW, 0, 5, 30),
          FLT1=filt(LP, 3600, 12, 0, 60, 50), AMP=adsr(1, 200, 0, 110),
          sends=dict(delay=40, reverb=25, chorus=65), delay=dly(E8, 38, 3800, 80, 250), reverb=rev(55, 2.0), chorus=cho(0.6, 55)),
    patch("Glass Arp", "SEQ", voices=2, banks=(64, None),               # 64 = GLASS
          OSC1=ob(50, level=100), OSC2=o2(SIN, 0, 0, 20, OCT=1),
          FLT1=filt(LP, 6500, 8, 0, 60, 30), GLOB=dict(MASTER=127), AMP=adsr(1, 250, 0, 160, level=127),
          sends=dict(delay=40, reverb=40), delay=dly(DOT8, 40, 4500), reverb=rev(70, 3.0)),
    patch("Bubbling Arp", "SEQ", voices=2,                  # Here She Comes Again: bubbling blips, a resonant filter ping on a sine
          OSC1=o1(SIN, level=100), OSC2=o2(TRI, 12, 0, 30),
          FLT1=filt(LADDER, 700, 55, 0, 60, 70), GRIT=dict(CLIP=pct(20)), GLOB=dict(MASTER=127), AMP=adsr(1, 130, 0, 70),
          sends=dict(delay=35, reverb=30), delay=dly(E8, 38, 4000), reverb=rev(60, 2.2)),
    patch("Flutter Arp", "SEQ", voices=2,
          OSC1=o1(SAW, level=100), OSC2=o2(TRI, 12, 0, 50),
          FLT1=filt(LP, 3000, 18, 0, 55, 50), AMP=adsr(1, 380, 25, 150, level=66),
          MOD=dict(LFO_RATE=lfo_hz(8.0), LFO_DEPTH=pct(45), LFO_DEST=LFO_AMP, LFO_SHAPE=TRI),
          sends=dict(delay=30, reverb=25), delay=dly(DOT8, 35, 3800), reverb=rev(55, 2.0)),
    patch("Robot Blip Seq", "SEQ", voices=1, banks=(134, 134),          # 134 = DUTY 25
          OSC1=ob(40, level=100), OSC2=ob2(40, 12, 0, 50),
          FLT1=filt(LP, 4200, 15, 0, 60, 50), AMP=adsr(1, 110, 0, 50),
          sends=dict(delay=30, reverb=20), delay=dly(DOT8, 35, 4000), reverb=rev(45, 1.6)),
    patch("Crunch Arp", "SEQ", voices=2,                    # Do It Again: a gritty, driven arp
          OSC1=o1(SAW, level=100), OSC2=o2(SQR, 0, 6, 70),
          FLT1=filt(CLASSIC, 2000, 35, 25, 55, 55), GRIT=dict(CLIP=pct(40), BITS=12, RATE=88), GLOB=dict(MASTER=110), AMP=adsr(1, 190, 0, 70, level=62),
          sends=dict(delay=25, reverb=25), delay=dly(DOT8, 35, 3500), reverb=rev(50, 1.8)),
    patch("Inevitable Arp", "SEQ", voices=2,                # The Inevitable End: a soft, round arp, the chorus and the echo make it wide
          OSC1=o1(TRI, level=100), OSC2=o2(SAW, 7, 4, 30),
          FLT1=filt(LP, 1900, 10, 0, 55, 45), AMP=adsr(3, 420, 0, 260),
          sends=dict(delay=40, reverb=50, chorus=60), delay=dly(DOT8, 40, 3500), reverb=rev(85, 4.0, 4000), chorus=cho(0.45, 50)),

    # ================= keys and plucks
    patch("Eple Keys", "KEYS", voices=3, banks=(78, None),              # 78 = FM TINE: the sampled electric piano at the core, a quiet synth body decaying with it
          OSC1=ob(45, level=100), OSC2=o2(TRI, 0, 4, 30),
          FLT1=filt(LP, 3600, 6, 0, 60, 25), GRIT=dict(BITS=10, RATE=72, DRIFT=pct(20)), AMP=adsr(2, 1000, 15, 420, vel=70, level=43),
          sends=dict(delay=20, reverb=30, chorus=35), delay=dly(DOT8, 30, 3800), reverb=rev(65, 2.6), chorus=cho(0.5, 35)),
    patch("Juno Pluck", "PLUCK", voices=2, banks=(131, None),           # 131 = PWM, narrow
          OSC1=ob(18, level=100), SUB=dict(SUB_LVL=pct(15), SUB_OCT=-1),
          FLT1=filt(LP, 2200, 20, 0, 60, 60), AMP=adsr(1, 260, 0, 150, level=61),
          sends=dict(delay=25, reverb=25, chorus=70), delay=dly(DOT8, 35, 3800, 80, 250), reverb=rev(60, 2.2), chorus=cho(0.6, 60)),
    patch("MS-20 Wobble", "BASS", voices=1,                  # the MS-20's snarl with a slow wobble on the cutoff
          OSC1=o1(SAW, level=100), OSC2=o2(SQR, 0, 5, 50),
          FLT1=filt(SOVIET, 600, 62, 0, 45, 25), GRIT=dict(CLIP=pct(15)), AMP=adsr(2, 600, 85, 100, level=40),
          MOD=dict(LFO_RATE=lfo_hz(3.0), LFO_DEPTH=pct(65), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI)),
    patch("Soft Vibes", "PLUCK", voices=3, banks=(63, None),            # 63 = BAR, with the motor tremolo of a vibraphone
          OSC1=ob(50, level=100),
          FLT1=filt(LP, 4200, 6, 0, 60, 20), AMP=adsr(1, 1300, 0, 700, level=70),
          MOD=dict(LFO_RATE=lfo_hz(4.6), LFO_DEPTH=pct(30), LFO_DEST=LFO_AMP, LFO_SHAPE=SIN),
          sends=dict(delay=25, reverb=40, chorus=20), delay=dly(DOT8, 35, 4000), reverb=rev(70, 3.0), chorus=cho(0.4, 25)),
    patch("Dark Music Box", "BELL", voices=2, banks=(77, None),         # 77 = FM DEEP
          OSC1=ob(50, level=100, OCT=1),
          FLT1=filt(LP, 5500, 6, 0, 60, 20), AMP=adsr(1, 700, 0, 500, level=70),
          sends=dict(delay=30, reverb=45), delay=dly(DOT8, 35, 4000), reverb=rev(70, 3.2)),
    patch("Bell Pluck", "PLUCK", voices=2, banks=(74, None),            # 74 = FM BELL: a bell-piano melody voice
          OSC1=ob(30, level=100), OSC2=o2(TRI, 0, 4, 25),
          FLT1=filt(LP, 4800, 8, 0, 60, 30), GRIT=grain(15), AMP=adsr(1, 800, 0, 400, level=70),
          sends=dict(delay=30, reverb=40, chorus=20), delay=dly(DOT8, 35, 3800), reverb=rev(70, 3.0), chorus=cho(0.4, 25)),

    # ================= stabs and chords (4)
    patch("CS-80 Chord Brass", "STAB", voices=3,
          OSC1=o1(SAW, -7, 100), OSC2=o2(SAW, 0, 7, 100),
          FLT1=filt(LP, 2200, 20, 0, 55, 55), AMP=adsr(60, 450, 45, 300, vel=60, level=57),
          sends=dict(delay=20, reverb=35, chorus=25), delay=dly(DOT8, 30, 3500), reverb=rev(65, 2.8, 4200), chorus=cho(0.5, 35)),
    patch("Juno Chord Stab", "STAB", voices=3,
          OSC1=o1(SAW, -6, 90), OSC2=o2(SQR, 0, 6, 60),
          FLT1=filt(LP, 2400, 12, 0, 55, 45), AMP=adsr(2, 280, 0, 160),
          sends=dict(delay=25, reverb=30, chorus=65), delay=dly(DOT8, 35, 3800), reverb=rev(60, 2.4), chorus=cho(0.6, 55)),
    patch("Pizzicato Chords", "STAB", voices=3, banks=(88, None),       # 88 = STIFF STRING, plucked chords
          OSC1=ob(50, level=100), OSC2=o2(SAW, 0, 5, 20),
          FLT1=filt(LP, 3600, 8, 0, 60, 35), AMP=adsr(1, 300, 0, 200, level=70),
          sends=dict(delay=25, reverb=40, chorus=40), delay=dly(DOT8, 35, 3800), reverb=rev(70, 2.8), chorus=cho(0.5, 40)),
    patch("Choir Hit Chord", "STAB", voices=3, banks=(50, None),        # 50 = CHOIR HIGH
          OSC1=ob(50, level=100), OSC2=o2(SAW, 0, 6, 20),
          FLT1=filt(LP, 4500, 8, 0, 55, 25), GRIT=grain(25), AMP=adsr(20, 500, 0, 450, vel=40),
          sends=dict(delay=20, reverb=50, chorus=30), delay=dly(DOT8, 30, 3500), reverb=rev(80, 3.4, 4000), chorus=cho(0.4, 35)),

    # ================= textures and effects (4)
    patch("Space Echo Drone", "TEXTURE", voices=2,           # a held triangle in a Space Echo with a lot of feedback: the repeats stack up and darken
          OSC1=o1(TRI, level=100), OSC2=o2(SAW, 7, 4, 30),
          FLT1=filt(LP, 1800, 12, 0, 40, 0), AMP=adsr(400, 600, 100, 1500, vel=30, level=60),
          sends=dict(delay=85, reverb=45), delay=dly(E8, 72, 2500, 100, 260), reverb=rev(80, 4.0, 3500, 30)),
    patch("Tape Hiss Bed", "TEXTURE", voices=2,
          OSC1=dict(LEVEL=0), SUB=dict(NOISE=pct(70)),
          FLT1=filt(LP, 5500, 10, 0, 20, 0), GLOB=dict(MASTER=127), AMP=adsr(900, 800, 100, 1500, vel=0, level=127),
          MOD=dict(LFO_RATE=lfo_hz(0.2), LFO_DEPTH=pct(40), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(reverb=40, chorus=40), reverb=rev(80, 3.8, 3500), chorus=cho(0.3, 40)),
    patch("Bubble Machine", "FX", voices=2, banks=(140, None),          # 140 = FM OCTAVE, a fast LFO on the table position: bubbling
          OSC1=ob(40, level=100),
          FLT1=filt(LP, 3500, 20, 0, 60, 40), GLOB=dict(MASTER=127), AMP=adsr(2, 500, 40, 200, level=53),
          MOD=dict(LFO_RATE=lfo_hz(7.0), LFO_DEPTH=pct(70), LFO_DEST=LFO_PD, LFO_SHAPE=SIN),
          sends=dict(delay=35, reverb=35), delay=dly(DOT8, 40, 4000), reverb=rev(60, 2.4)),
    patch("Robot Voice Ring", "FX", voices=1,
          OSC1=o1(SAW, level=100), OSC2=o2(SAW, 6, 0, 0), SUB=dict(RING=pct(65)),      # the ring modulator is OSC 2: a tritone up, so the sidebands are inharmonic
          FLT1=filt(LP, 3200, 15, 0, 60, 30), GLOB=dict(MASTER=127), AMP=adsr(8, 400, 70, 250),
          sends=dict(delay=30, reverb=30), delay=dly(DOT8, 35, 3500), reverb=rev(55, 2.2)),

    # ================= S1000 percussion for a sequencer lane (3)
    patch("Crunch Rim", "PLUCK", voices=1,
          OSC1=dict(LEVEL=0), SUB=dict(NOISE=pct(100)),
          FLT1=filt(BP, 1800, 30, 0, 0, 80), GRIT=dict(BITS=10, RATE=70), AMP=adsr(1, 55, 0, 20, vel=70, level=65),
          sends=dict(reverb=25), reverb=rev(45, 1.4)),
    patch("Tape Shaker", "PLUCK", voices=1,
          OSC1=dict(LEVEL=0), SUB=dict(NOISE=pct(100)),
          FLT1=filt(BP, 6500, 20, 0, 0, 40), GRIT=dict(BITS=12, RATE=90), AMP=adsr(25, 90, 0, 40, vel=60, level=76),
          sends=dict(delay=15, reverb=20), delay=dly(DOT16, 30, 5000), reverb=rev(40, 1.2)),
    patch("Muffled Tom Ping", "PLUCK", voices=1,
          OSC1=o1(SIN, level=100), SUB=dict(NOISE=pct(6)),
          FLT1=filt(LADDER, 160, 85, 0, 40, 80), GRIT=dict(CLIP=pct(25), BITS=12, RATE=92), GLOB=dict(MASTER=127), AMP=adsr(1, 300, 0, 120, level=30),
          sends=dict(reverb=25), reverb=rev(50, 1.8, 3500, 15, 60)),

    # ================= and a few more
    patch("PWM Wash Pad", "PAD", voices=3, banks=(131, 131),            # 131 = PWM on both: one LFO moves the width of both pulses (the Juno's PWM pad)
          OSC1=ob(30, -6, 100), OSC2=ob2(30, 0, 7, 80),
          FLT1=filt(LP, 2800, 8, 0, 45, 0), AMP=adsr(450, 700, 95, 1400, vel=30, level=37),
          MOD=dict(LFO_RATE=lfo_hz(0.6), LFO_DEPTH=pct(45), LFO_DEST=LFO_PD, LFO_SHAPE=TRI),
          sends=dict(delay=15, reverb=50, chorus=65), delay=dly(DOT8, 30, 3500), reverb=rev(80, 3.8, 4000, 30), chorus=cho(0.5, 55)),
    patch("Vocoder Chords", "POLY", voices=3, banks=(31, None),         # 31 = FORMANT: the VP-330 vocoder voice playing chords
          OSC1=ob(35, level=100), OSC2=o2(SAW, 0, 6, 30),
          FLT1=filt(LP, 3500, 10, 0, 50, 25), AMP=adsr(40, 500, 75, 500, vel=45, level=39),
          sends=dict(delay=35, reverb=45, chorus=50), delay=dly(DOT8, 40, 3200, 90, 260), reverb=rev(75, 3.2, 3800, 30), chorus=cho(0.55, 45)),
]
