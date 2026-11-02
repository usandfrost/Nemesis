# Synthwave, batch 3: three patches the user rated 4 that were swapped out of batch 2 by mistake while it was being diversified (ids 290-292). They come back as
# they were, with the review's fixes (OSC2 pitch explicit, voices, levels), so they are new sounds to rate again.

BASE_ID = 290

def dly(mode=DOT8, fb=45, tone=4500, spread=80, hpf=200): return dict(MODE=mode, FEEDBACK=fb, TONE=tone, SPREAD=spread, HPF=hpf)
def rev(size=70, decay=3.5, damp=5000, pre=25): return dict(SIZE=size, DECAY=decay, DAMP=damp, PRE_DLY=pre)
def cho(rate=0.6, depth=45, delay=12): return dict(RATE=rate, DEPTH=depth, DELAY=delay)

PATCHES = [
    patch("Pulse Dreams", "PAD", voices=4, banks=(8, 8),           # 8 = PWM REVERSE on both
          OSC1=dict(PD_SHAPE=30, FINE=-10, LEVEL=pct(80)), OSC2=dict(PD_SHAPE=30, SEMI=0, FINE=10, LEVEL=pct(80)),
          FLT1=dict(MODEL=LP, CUTOFF=hz(2400), RESO=pct(8), KEYTRK=pct(40), ENV_AMT=pct(0)),
          AMP=dict(ATTACK=ms(500), DECAY=ms(700), SUSTAIN=pct(90), RELEASE=ms(1300), VEL_SENS=pct(30), LEVEL=65),
          MOD=dict(LFO_RATE=lfo_hz(0.6), LFO_DEPTH=pct(25), LFO_DEST=LFO_PD, LFO_SHAPE=TRI),
          sends=dict(delay=20, reverb=50, chorus=70), delay=dly(DOT8, 40, 4000), reverb=rev(80, 4.5, 5000, 30), chorus=cho(0.55, 55)),
    patch("Pulse Pump", "BASS", voices=1, banks=(131, None),       # 131 = PWM
          OSC1=dict(PD_SHAPE=30, OCT=-1, LEVEL=127), OSC2=dict(WAVE=SAW, OCT=-1, SEMI=0, FINE=5, LEVEL=pct(40)),
          SUB=dict(SUB_LVL=pct(35), SUB_OCT=-1),
          FLT1=dict(MODEL=LADDER, CUTOFF=hz(450), RESO=pct(22), KEYTRK=pct(55), ENV_AMT=pct(45)),
          AMP=dict(ATTACK=ms(2), DECAY=ms(180), SUSTAIN=pct(55), RELEASE=ms(80), LEVEL=127)),
    patch("Tine Keys", "KEYS", voices=3, banks=(78, None),         # 78 = FM TINE
          OSC1=dict(PD_SHAPE=30, LEVEL=127), OSC2=dict(WAVE=SIN, SEMI=0, FINE=0, LEVEL=pct(20)),
          FLT1=dict(MODEL=LP, CUTOFF=hz(3200), RESO=pct(5), KEYTRK=pct(60), ENV_AMT=pct(25)),
          AMP=dict(ATTACK=ms(3), DECAY=ms(800), SUSTAIN=pct(15), RELEASE=ms(350), VEL_SENS=pct(90), LEVEL=45),
          sends=dict(reverb=30, chorus=80), reverb=rev(60, 2.8), chorus=cho(0.9, 55)),
]
