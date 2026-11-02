# Voice pads, batch 1: ids 700-749 (only ever append, a patch's position is its id). tools/genre_presets.py builds them (--set voice).
# Why these exist: the choir/voice wavetable banks give no voice (each frame is one cycle with its vowel peaks at fixed harmonic NUMBERS, so the vowel moves with the note;
# user's verdict "No voice" on 7 of 7). A voice has its formants at fixed FREQUENCIES whatever the pitch. Here the source is plain saws (a unison stack, which is what an
# ensemble of singers is) and the vowel is made by the two filters: FILTER 1 and FILTER 2 are both band-pass, in PARALLEL, with KEYTRK 0 and no envelope, one on the first
# formant and one on the second. Formant frequencies are the usual adult values (Peterson & Barney): ah 730/1090, oo 300/870, ee 270/2290, oh 570/840 (the vowel of 'aw'), ay 400/2000 (close to 'ay', not a table value).
# The balance knob (MIX F1/F2) stays at 0 (both filters at unity). Measured on the dry render in a bass register with FILTER 2's cutoff working (it did not before
# 2026-10-27, see docs/PATCH_MAP.md): F2 comes out about 6-9 dB under F1 for oo, ee and ay and about 2 dB for ah. MOD WHL stays at the panel's 64, so on the device the
# wheel moves both formants together (F2 follows F1's modulation); the offline render has the wheel at rest. A little noise before the filters gives the breath, GRIT DRIFT the slight instability, a chorus and a large reverb the room.
# Engine facts: only two formants fit (two filters); the LFO is free-running and LFO -> cutoff moves both filters together by the same ratio, so the "drift" pad slides the
# whole vowel up and down (it passes through neighbouring vowels), it does not morph F1 and F2 separately. Levels start at the top and calibrate_levels.py lowers them.

BASE_ID = 700

def vowel(f1, f2, q1=60, q2=60, mix=0):
    """FILTER 1 and 2 as the two formants: band-pass, parallel, fixed in Hz (no key tracking, no envelope)."""
    return dict(
        FLT1=dict(MODEL=BP, CUTOFF=hz(f1), RESO=pct(q1), DRIVE=pct(0), KEYTRK=pct(0), ENV_AMT=pct(0), MIX_F1_F2=pct(mix)),
        FLT2=dict(MODEL=BP, CUTOFF=hz(f2), RESO=pct(q2), DRIVE=pct(0), ROUTE=PARALLEL, ACTIVE=1, KEYTRK=pct(0), ENV_AMT=pct(0)))

PATCHES = [
    patch("Ah Pad", "PAD", voices=3,                          # ah: 730 / 1090 Hz
          OSC1=o1(SAW, -5, 100), SUB=dict(UNISON=3, UNI_DET=pct(25), UNI_BLND=pct(60), WIDTH=pct(0), NOISE=pct(5)),
          **vowel(730, 1090, 60, 60, 0), GRIT=dict(DRIFT=pct(20)), AMP=adsr(550, 800, 95, 1600, vel=30, level=43),
          sends=dict(reverb=55, chorus=55), reverb=rev(85, 4.5, 4500, 35), chorus=cho(0.4, 50)),
    patch("Oo Pad", "PAD", voices=3,                          # oo: 300 / 870 Hz
          OSC1=o1(SAW, -5, 100), SUB=dict(UNISON=3, UNI_DET=pct(25), UNI_BLND=pct(60), WIDTH=pct(0), NOISE=pct(5)),
          **vowel(300, 870, 60, 60, 0), GRIT=dict(DRIFT=pct(20)), AMP=adsr(600, 800, 95, 1700, vel=30, level=25),
          sends=dict(reverb=55, chorus=55), reverb=rev(85, 4.5, 4000, 35), chorus=cho(0.4, 50)),
    patch("Ee Pad", "PAD", voices=3,                          # ee: 270 / 2290 Hz
          OSC1=o1(SAW, -5, 100), SUB=dict(UNISON=3, UNI_DET=pct(25), UNI_BLND=pct(60), WIDTH=pct(0), NOISE=pct(5)),
          **vowel(270, 2290, 60, 60, 0), GRIT=dict(DRIFT=pct(20)), AMP=adsr(500, 800, 95, 1500, vel=30, level=29),
          sends=dict(reverb=55, chorus=55), reverb=rev(85, 4.5, 5500, 35), chorus=cho(0.4, 50)),
    patch("Oh Pad", "PAD", voices=3,                          # oh: 570 / 840 Hz
          OSC1=o1(SAW, -5, 100), SUB=dict(UNISON=3, UNI_DET=pct(25), UNI_BLND=pct(60), WIDTH=pct(0), NOISE=pct(5)),
          **vowel(570, 840, 60, 60, 0), GRIT=dict(DRIFT=pct(20)), AMP=adsr(550, 800, 95, 1600, vel=30, level=38),
          sends=dict(reverb=55, chorus=55), reverb=rev(85, 4.5, 4200, 35), chorus=cho(0.4, 50)),
    patch("Ay Pad", "PAD", voices=3,                          # ay: 400 / 2000 Hz
          OSC1=o1(SAW, -5, 100), SUB=dict(UNISON=3, UNI_DET=pct(25), UNI_BLND=pct(60), WIDTH=pct(0), NOISE=pct(5)),
          **vowel(400, 2000, 60, 60, 0), GRIT=dict(DRIFT=pct(20)), AMP=adsr(500, 800, 95, 1500, vel=30, level=26),
          sends=dict(reverb=55, chorus=55), reverb=rev(85, 4.5, 5500, 35), chorus=cho(0.4, 50)),
    patch("Vowel Drift Pad", "PAD", voices=3,                 # ah, slid slowly up and down by the LFO (both formants together)
          OSC1=o1(SAW, -5, 100), SUB=dict(UNISON=3, UNI_DET=pct(25), UNI_BLND=pct(60), WIDTH=pct(0), NOISE=pct(5)),
          **vowel(650, 1000, 60, 60, 0), GRIT=dict(DRIFT=pct(20)), AMP=adsr(700, 800, 100, 1800, vel=30, level=42),
          MOD=dict(LFO_RATE=lfo_hz(0.2), LFO_DEPTH=pct(40), LFO_DEST=LFO_CUT, LFO_SHAPE=TRI),
          sends=dict(reverb=60, chorus=55), reverb=rev(88, 5.0, 4500, 40), chorus=cho(0.4, 50)),
    patch("Ah Lead", "LEAD", voices=1,                        # one singer, a higher voice (ah: 850 / 1220 Hz), a little vibrato
          OSC1=o1(SAW, -4, 100), OSC2=o2(SAW, 0, 5, 70), SUB=dict(NOISE=pct(4)),
          **vowel(850, 1220, 60, 60, 0), GRIT=dict(DRIFT=pct(10)), AMP=adsr(70, 500, 90, 500, level=26),
          MOD=dict(LFO_RATE=lfo_hz(5.5), LFO_DEPTH=pct(22), LFO_DEST=LFO_PITCH, LFO_SHAPE=SIN),
          sends=dict(delay=25, reverb=50, chorus=25), delay=dly(DOT8, 35, 4000), reverb=rev(80, 3.8, 4500, 35), chorus=cho(0.4, 30)),
    patch("Breathy Ah Pad", "PAD", voices=3,                  # ah with a lot of breath: the noise goes through the same two formants
          OSC1=o1(SAW, -5, 60), SUB=dict(UNISON=3, UNI_DET=pct(25), UNI_BLND=pct(60), WIDTH=pct(0), NOISE=pct(28)),
          **vowel(730, 1090, 50, 50, 0), GRIT=dict(DRIFT=pct(20)), AMP=adsr(650, 800, 95, 1800, vel=30, level=66),
          sends=dict(reverb=60, chorus=45), reverb=rev(88, 5.0, 4500, 40), chorus=cho(0.35, 45)),
]
