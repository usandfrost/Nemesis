# Trance and techno extras, batch 1 of the trance set: twenty patches (ids 400-419; only ever append, a patch's position is its id). tools/genre_presets.py builds them (--set trance).
# Asked for: trance gates, more supersaw, rolling basses and a few more techno sounds. Same engine facts as content/techno/batch1.py (filter envelope decays with the
# amp decay and has no sustain; FINE is the detune; UNISON sits on the SUB page and acts on OSC 1; no glide, no pitch envelope; OSC 2's pitch is always written out).
# Gates: the LFO is free-running (never restarted at a note) and its amp depth is at most about +-9.5 dB, so a gated pad PULSES, it does not cut to silence. The hard
# gates are the chops: sustain 0 and a short decay, so a sequencer or the arp plays the 16ths and the tempo-synced delay answers them.
# Rolling basses: one voice, short decay with sustain 0 and the filter envelope on top, meant to be played as running 16ths.
# Levels start at the top (AMP LEVEL 127) and content/tools/calibrate_levels.py lowers the loud ones to the library's level.

BASE_ID = 400

PATCHES = [
    # ================= trance gates (4)
    patch("Trance Gate Saw", "PAD", voices=3,
          OSC1=o1(SAW, level=100), SUB=dict(UNISON=4, UNI_DET=pct(40), UNI_BLND=pct(70), WIDTH=pct(0)),
          FLT1=filt(LP, 4800, 12, 0, 50, 0), AMP=adsr(10, 500, 100, 160, vel=30, level=42),
          MOD=dict(LFO_RATE=lfo_hz(9.2), LFO_DEPTH=pct(100), LFO_DEST=LFO_AMP, LFO_SHAPE=SQR),   # 9.2 Hz = 16ths at 138 BPM (the LFO is free-running: tempo is not locked)
          sends=dict(reverb=25, chorus=45), reverb=rev(50, 1.8), chorus=cho(0.6, 45)),
    patch("Gate Chord Chop", "STAB", voices=2,
          OSC1=o1(SAW, level=100), SUB=dict(UNISON=3, UNI_DET=pct(40), UNI_BLND=pct(70), WIDTH=pct(70)),
          FLT1=filt(LP, 3500, 10, 0, 55, 25), AMP=adsr(1, 95, 0, 35),
          sends=dict(delay=30, reverb=20), delay=dly(DOT8, 45, 5000), reverb=rev(55, 2.0)),
    patch("Gated Strings", "PAD", voices=3,
          OSC1=o1(SAW, -12, 80), OSC2=o2(SAW, 0, 12, 80),
          FLT1=filt(LP, 2600, 8, 0, 40, 0), GRIT=dict(DRIFT=pct(15)), AMP=adsr(120, 700, 100, 500, vel=30, level=41),
          MOD=dict(LFO_RATE=lfo_hz(4.6), LFO_DEPTH=pct(100), LFO_DEST=LFO_AMP, LFO_SHAPE=SQR),   # 4.6 Hz = 8ths at 138 BPM
          sends=dict(delay=20, reverb=30, chorus=60), delay=dly(DOT8, 40, 4500), reverb=rev(55, 2.0, 4500, 20), chorus=cho(0.4, 55)),
    patch("Sync Chop Stab", "STAB", voices=3, banks=(13, None),       # 13 = SYNC WIDE
          OSC1=ob(50, level=100),
          FLT1=filt(LP, 3500, 20, 0, 55, 40), AMP=adsr(1, 90, 0, 30),
          sends=dict(delay=25, reverb=15), delay=dly(DOT8, 45, 4500), reverb=rev(45, 1.5)),

    # ================= supersaw (6)
    patch("Supersaw Pad Wide", "PAD", voices=3,
          OSC1=o1(SAW, level=100), SUB=dict(UNISON=4, UNI_DET=pct(55), UNI_BLND=pct(75), WIDTH=pct(0)),   # width 0 so three voices fit the CPU budget; the chorus makes it wide
          FLT1=filt(LP, 3800, 8, 0, 50, 0), AMP=adsr(550, 700, 90, 1500, vel=30, level=53),
          sends=dict(reverb=55, chorus=60), reverb=rev(85, 4.5, 4500, 30), chorus=cho(0.4, 55)),
    patch("Big Room Saw Lead", "LEAD", voices=2,
          OSC1=o1(SAW, level=100), OSC2=o2(SAW, 12, 4, 60), SUB=dict(UNISON=3, UNI_DET=pct(50), UNI_BLND=pct(70), WIDTH=pct(80)),
          FLT1=filt(LP, 6000, 10, 0, 65, 15), AMP=adsr(4, 400, 85, 300, level=38),
          sends=dict(delay=40, reverb=40, chorus=20), delay=dly(DOT8, 40, 5000), reverb=rev(75, 3.5), chorus=cho(0.5, 30)),
    patch("Euphoric Saw Chords", "POLY", voices=3,
          OSC1=o1(SAW, level=100), SUB=dict(UNISON=4, UNI_DET=pct(35), UNI_BLND=pct(65), WIDTH=pct(0)),
          FLT1=filt(LP, 4200, 10, 0, 55, 20), AMP=adsr(10, 700, 80, 450, level=37),
          sends=dict(delay=35, reverb=45, chorus=45), delay=dly(DOT8, 45, 5000), reverb=rev(80, 4.0), chorus=cho(0.5, 40)),
    patch("Supersaw Pluck", "PLUCK", voices=2,
          OSC1=o1(SAW, level=100), SUB=dict(UNISON=4, UNI_DET=pct(45), UNI_BLND=pct(70), WIDTH=pct(70)),
          FLT1=filt(LP, 1400, 15, 0, 60, 70), AMP=adsr(1, 240, 0, 140, level=77),
          sends=dict(delay=25, reverb=30), delay=dly(DOT8, 45, 5000), reverb=rev(60, 2.5)),
    patch("Octave Saw Arp", "SEQ", voices=2,
          OSC1=o1(SAW, -5, 100), OSC2=o2(SAW, 12, 5, 70),
          FLT1=filt(LP, 2600, 12, 0, 60, 45), AMP=adsr(1, 150, 0, 90),
          sends=dict(delay=14, reverb=25), delay=dly(DOT8, 45, 5000), reverb=rev(60, 2.2)),
    patch("Warm Swell Chords", "POLY", voices=3,
          OSC1=o1(TRI, level=100), OSC2=o2(SAW, 0, 8, 60),
          FLT1=filt(LADDER, 2000, 14, 0, 50, 0), AMP=adsr(450, 800, 85, 900, vel=30),
          sends=dict(delay=25, reverb=55, chorus=50), delay=dly(DOT8, 40, 4500), reverb=rev(85, 4.5), chorus=cho(0.45, 45)),

    # ================= rolling basses (6): one voice, played as running 16ths
    patch("Rolling Trance Bass", "BASS", voices=1,
          OSC1=o1(SAW, level=100), OSC2=o2(SAW, 0, 5, 70),
          FLT1=filt(LP, 700, 22, 0, 55, 55), AMP=adsr(1, 130, 0, 45)),
    patch("Psy Rolling Bass", "BASS", voices=1,
          OSC1=o1(SAW, level=100, OCT=-1), OSC2=o2(SQR, 0, 0, 55, OCT=-1),
          FLT1=filt(LP, 800, 38, 0, 60, 75), GRIT=dict(CLIP=pct(25)), GLOB=dict(MASTER=127), AMP=adsr(1, 85, 0, 30)),
    patch("Rolling Sub Octaves", "BASS", voices=1,
          OSC1=o1(SAW, level=100), SUB=dict(SUB_LVL=pct(60), SUB_OCT=-1),   # the sub is an octave under the key whatever OSC OCT says: OSC 1 stays at the key
          FLT1=filt(LP, 600, 14, 0, 50, 45), AMP=adsr(2, 160, 0, 60)),
    patch("Rolling Ladder Bass", "BASS", voices=1,
          OSC1=o1(SQR, level=100), OSC2=o2(SQR, 0, 0, 60, OCT=-1),
          FLT1=filt(LADDER, 800, 45, 0, 55, 65), GRIT=dict(CLIP=pct(25)), GLOB=dict(MASTER=127), AMP=adsr(1, 120, 0, 40)),
    patch("Rolling FM Bass", "BASS", voices=1, banks=(143, None),     # 143 = FEEDBACK HARD
          OSC1=ob(40, level=100), OSC2=o2(SAW, 0, 4, 40),
          FLT1=filt(LP, 1500, 22, 0, 55, 40), AMP=adsr(1, 140, 0, 50)),
    patch("Tech Roll Bass", "BASS", voices=1,
          OSC1=o1(SAW, -4, 100), OSC2=o2(SAW, 0, 4, 80),
          FLT1=filt(ACID, 900, 30, 70, 50, 60), GRIT=dict(CLIP=pct(65)), GLOB=dict(MASTER=127), AMP=adsr(1, 100, 0, 35)),

    # ================= techno (4)
    patch("Hypno Techno Lead", "LEAD", voices=1,
          OSC1=o1(SQR, level=100), OSC2=o2(SQR, 0, 7, 60),
          FLT1=filt(LP, 1800, 35, 0, 55, 25), AMP=adsr(3, 300, 80, 150, level=21),
          MOD=dict(LFO_RATE=lfo_hz(0.4), LFO_DEPTH=pct(50), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(delay=40, reverb=25), delay=dly(DOT8, 55, 3500), reverb=rev(60, 2.5)),
    patch("Industrial Stab", "STAB", voices=2, banks=(152, None),      # 152 = FOLD
          OSC1=ob(60, level=100), OSC2=o2(SAW, 0, 8, 40),
          FLT1=filt(LP, 2200, 22, 20, 55, 55), GRIT=dict(CLIP=pct(25)), AMP=adsr(1, 150, 0, 90, level=75),
          sends=dict(delay=20, reverb=35), delay=dly(DOT16, 40, 4000), reverb=rev(65, 2.8)),
    patch("Sine Ping", "PLUCK", voices=2,
          OSC1=o1(SIN, level=100), SUB=dict(NOISE=pct(8)),
          FLT1=filt(LADDER, 200, 90, 0, 40, 80), AMP=adsr(1, 420, 0, 180, level=77),
          sends=dict(reverb=25), reverb=rev(55, 1.8, 3500, 10)),
    patch("Dark Techno Chord", "POLY", voices=3,
          OSC1=o1(SAW, -8, 80), OSC2=o2(SAW, 0, 9, 70),
          FLT1=filt(LP, 900, 25, 0, 45, 35), AMP=adsr(4, 650, 55, 500, level=39),
          sends=dict(delay=35, reverb=45), delay=dly(DOT8, 55, 2800), reverb=rev(80, 4.0, 3500, 30)),
]
