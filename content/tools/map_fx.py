#!/usr/bin/env python3
"""map_fx.py - how each cut preset's ORIGINAL upstream effect chain lands on the Drumboy's effects section.

  map_fx.py [--out content/data/fxmap.json] [--cases FILE]

Reads content/data/cut.json and the original preset JSON (ref/upstream/docs/dev/sound-design/archive/presets, read only), turns
every `fx` row to real units with the upstream registry laws (sdk/core/src/UnitCurves.cpp, ParamRegistry.cpp), maps those onto the Drumboy
knobs of firmware/src/app/FxParams.cpp (read, not written) and writes one record per preset in Drumboy stored units (0..127, or a
list index). The rules and the judgement calls are in content/out/FX_MAPPING.md.

--cases FILE writes the (upstream params | Drumboy params) pairs the render calibration (content/tools/fxcal/) measures; the per-type send
gains in SEND_GAIN come from that run.
"""
import argparse
import collections
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fxknobs  # noqa: E402
from fxknobs import fx_table, knob, stored  # noqa: E402

REPO = fxknobs.REPO
ARCHIVE = os.path.join(REPO, "ref", "upstream", "docs", "dev", "sound-design", "archive", "presets")
CUT = os.path.join(REPO, "content", "data", "cut.json")
OUT = os.path.join(REPO, "content", "data", "fxmap.json")

T = fx_table()
BPM_REF = 120.0   # the tempo a tempo-synced delay is turned to milliseconds at (the upstream atlas measures at 120)

# ----------------------------------------------------------------------------------------------------------------- upstream laws
# Defaults of the rows a preset file leaves out (sdk/core/src/ParamRegistry.cpp: a short block loads the missing row at its default).
DEF = {
    "delay": dict(type=0, sync=2, time=48, fbk=23, tone=74, width=90, mod=29, mix=30),
    "reverb": dict(type=1, decay=64, predly=18, damp=58, size=88, diffuse=70, mod=36, mix=34, erlate=60, shimmer=0),
    "chorus": dict(type=0, mode=2, depth=48, rate=0, width=70, fbk=7, tone=64, voices=3, mix=40, hp=0),
    "drive": dict(type=0, drive=44, tone=60, filter=80, level=96, mix=100),
    "gamp": dict(model=1, gain=54, bass=62, mid=52, treble=60, presence=46, cab=0, mic=0, level=84, mix=100),
    "comp": dict(type=0, thresh=74, ratio=40, attack=103, release=127, makeup=24),
    "phaser": dict(rate=68, depth=127, fbk=64, mix=64),
    "vintage": dict(model=3, mix=40),
}
K_DEPTH, K_FB, K_SPACE = 1.7369655941662063, 0.6520766965796932, 1.3219280948873622   # UnitCurves.h calibration exponents


def pct(v):           # kPercent
    return v / 127.0 * 100.0


def depth_pct(v):     # kDepthPct (Power)
    return 100.0 * (v / 127.0) ** K_DEPTH


def space_pct(v):     # kSpacePct (Power)
    return 100.0 * (v / 127.0) ** K_SPACE


def fb_pct(v):        # kFeedbackPct (Power, 0..110 %)
    return 110.0 * (v / 127.0) ** K_FB


def delay_ms(v):      # kDelayMs, Exp 1..4000
    return 4000.0 ** (v / 127.0)


def rev_decay_ms(v):  # kRevDecayMs, Exp 100..20000
    return 100.0 * 200.0 ** (v / 127.0)


def predly_ms(v):     # kPreDelayMs, Linear 0..250
    return v / 127.0 * 250.0


def chorus_rate_hz(v):   # kChorusRateHz, Exp 0.05..8
    return 0.05 * 160.0 ** (v / 127.0)


def hp_corner_hz(v):  # kHpCornerHz, Exp 20..2000 (chorus.hp)
    return 20.0 * 100.0 ** (v / 127.0)


def drive_db(v):      # kDriveDb, Linear 0..36
    return v / 127.0 * 36.0


def level_db(v):      # kLevelDb, CurveKind::Level (UnitCurves §22.8): square law below 100, linear above to +12 dB
    return 40.0 * math.log10(max(v, 1e-9) / 100.0) if v <= 100 else 12.0 * (v - 100) / 27.0


def chain_hz(v):      # the band edges `lo`/`hi` of an fx entry: 20 * 1000^(v/127)  (ATLAS: 33 = 120 Hz, 46 = 244 Hz, 97 = 3.9 kHz)
    return 20.0 * 1000.0 ** (v / 127.0)


def cutoff_hz(v):     # kCutoffHz, ExpZero 20..20000 with noon 700 Hz (drive.filter)
    b = (700.0 ** 2 - 20.0 * 20000.0) / (20.0 + 20000.0 - 2.0 * 700.0)
    return (20.0 + b) * ((20000.0 + b) / (20.0 + b)) ** (v / 127.0) - b


DLY_TYPES = ["Digital", "Analog", "Tape", "Ping-pong", "Multi-tap", "Reverse"]
DLY_SYNC_BEATS = {0: 1.0, 1: 1.5, 2: 0.5, 3: 0.75, 4: 1.0 / 3.0, 5: 0.25, 6: 0.75, 7: 1.0 / 6.0}   # kDlySyncs, 8 = Free
DLY_SYNC_NAME = ["1/4", "1/4.", "1/8", "1/8.", "1/8T", "1/16", "3/16", "1/16T", "Free"]
# upstream sync index -> Drumboy MODE index (FREE 1/4 1/8. 1/8 1/16. 1/16 1/8T 1/16T); 1/4. has none (-> FREE at the time it has at BPM_REF)
SYNC_TO_MODE = {0: 1, 1: 0, 2: 3, 3: 2, 4: 6, 5: 5, 6: 2, 7: 7}
REV_TYPES = ["Room", "Hall", "Plate", "Spring", "Shimmer", "Cathedral"]
# per-type decay band (ms), pre-delay band (ms) - ReverbEffect.h
REV_DECAY = [(200, 1200), (1000, 6000), (500, 3000), (500, 3000), (1500, 8000), (3000, 12000)]
REV_PRE = [(0, 20), (0, 80), (0, 40), (0, 0), (0, 80), (30, 150)]
CHO_TYPES = ["Chorus", "Flanger", "Ensemble", "Vibrato"]
CHO_GEOM = {0: (20.0, 8.0), 1: (3.5, 3.0), 2: (20.0, 8.0), 3: (12.0, 10.0)}   # (base ms, swing ms) ChorusEffect.cpp geometryFor
DRV_TYPES = ["Overdrive", "Distortion", "Fuzz", "Bit crush", "Sample rate", "Saturate"]
AMP_MODELS = ["Clean", "Crunch", "Lead", "Hi-gain", "Bass", "Acoustic"]
CAB_TYPES = ["4x12", "2x12", "1x12", "1x10", "8x10", "Direct"]
COMP_TYPES = ["VCA", "FET", "Opto", "Bus"]
SAMPLERS = ["Emulator I", "Emulator II", "Akai S900", "Akai S1000", "SP-1200", "Fairlight", "Mellotron"]

# ----------------------------------------------------------------------------------------------------------------- calibration
# Wet-level correction of a SEND, per original effect type: measured by rendering the upstream effect at 100 % mix and the Drumboy effect
# (send 1, return 100 %) on the same input with the mapped settings, as sqrt(E_nemesis / E_drumboy) - see content/tools/fxcal/ and
# FX_MAPPING.md. 1.0 = the Drumboy return is as loud as the upstream wet at the same send. Filled in from fxcal/cal_result.json.
CAL_FILE = os.path.join(HERE, "fxcal", "cal_result.json")


def load_cal():
    try:
        return json.load(open(CAL_FILE))
    except (OSError, ValueError):
        return {"delay": {}, "reverb": {}, "chorus": {}}


CAL = load_cal()


def send_from_mix(mix127, gain):
    """the original is a dry/wet insert: out = dry*(1-m) + wet*m. The Drumboy part keeps its full dry and adds send*wet, so the
    same wet-to-dry balance needs send = m/(1-m) (times the wet level correction), clamped to 1.0."""
    m = min(max(mix127 / 127.0, 0.0), 1.0)
    if m >= 0.999:
        return 127, True
    s = m / (1.0 - m) * gain
    return int(min(127, max(0, math.floor(s * 127.0 + 0.5)))), s > 1.0 + 0.5 / 127.0


# ----------------------------------------------------------------------------------------------------------------- loading
def load_originals():
    idx = {}
    for bank in sorted(os.listdir(ARCHIVE)):
        d = os.path.join(ARCHIVE, bank)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".json"):
                j = json.load(open(os.path.join(d, fn)))
                for p in j["machines"]["cathode"]:
                    idx[(bank, p["name"][:23])] = dict(p, _file=os.path.join(bank, fn))
    return idx


def fx_rows(p):
    """the original chain as [(tid, vals-with-defaults, lo, hi)] in chain order"""
    out = []
    for e in p.get("fx", []):
        v = dict(DEF.get(e["tid"], {}))
        v.update(e.get("vals", {}))
        out.append((e["tid"], v, e.get("lo"), e.get("hi"), set(e.get("vals", {}))))
    return out


# ----------------------------------------------------------------------------------------------------------------- knob helpers
def S(view, name, r):
    return stored(knob(T, view, name), r)


def defaults(view):
    return {k.name: fxknobs.default_stored(k) for k in T[view]}


# ----------------------------------------------------------------------------------------------------------------- delay
def map_delay(v, lo, hi, notes, case=None):
    typ = int(v["type"])
    sync = int(v["sync"])
    tname = DLY_TYPES[typ] if typ < 6 else "?"
    # played time: the upstream `time` row (only when sync is Free), the per-type clamp of DelayEffect (Analog 20-600, Tape 50-1500 ms)
    if sync == 8:
        ms = delay_ms(v["time"])
        lo_ms, hi_ms = {1: (20.0, 600.0), 2: (50.0, 1500.0)}.get(typ, (1.0, 4000.0))
        ms = min(max(ms, lo_ms), hi_ms)
        mode = 0
    else:
        ms = DLY_SYNC_BEATS[sync] * 60000.0 / BPM_REF
        mode = SYNC_TO_MODE[sync]
        if sync == 1:
            notes.append("delay 1/4. has no Drumboy note mode: FREE at %.0f ms (= 1/4. at %d BPM; does not follow tempo)" % (ms, BPM_REF))
    if ms > 1000.0:
        notes.append("delay time %.0f ms clamped to the 1000 ms TIME limit" % ms)
    time = S("DELAY", "TIME", ms)
    fb = min(fb_pct(v["fbk"]), 95.0)
    if fb_pct(v["fbk"]) > 95.0:
        notes.append("delay feedback %.0f%% clamped to 95%%" % fb_pct(v["fbk"]))
    t = pct(v["tone"]) / 100.0
    if typ == 2:       # Tape: tone is AGE, the darker the higher
        tone_hz = 16000.0 * (800.0 / 16000.0) ** t
    elif typ == 1:     # Analog (BBD): the reconstruction low-pass follows the delay time, N/(4 D), N = 4096 * 4^(tone-0.5)
        tone_hz = 4096.0 * 4.0 ** (t - 0.5) / (4.0 * ms / 1000.0)
    else:              # Digital, Ping-pong: 1k..20k exponential
        tone_hz = 1000.0 * 20.0 ** t
    hpf_hz = 30.0      # DelayEffect kToneHpDefaultHz (a preset file never sets tonehp)
    free_tone, free_hpf = tone_hz, hpf_hz
    if hi is not None:
        tone_hz = min(tone_hz, chain_hz(hi))
    if lo is not None:
        hpf_hz = chain_hz(lo)
    width = pct(v["width"])
    if typ == 3:
        width = 100.0
    mod = depth_pct(v["mod"])
    rec = dict(mode=mode, time=time, feedback=S("DELAY", "FEEDBACK", fb), tone=S("DELAY", "TONE", tone_hz),
               spread=S("DELAY", "SPREAD", width), mod=S("DELAY", "MOD", mod), hpf=S("DELAY", "HPF", hpf_hz))
    if typ != 0:
        extra = {1: "Analog: no BBD companding/drift/saturation, TONE set from the BBD bandwidth law N/(4*time) at %d BPM" % BPM_REF,
                 2: "Tape: no heads/saturation/wow, TONE = the age low-pass", 3: "Ping-pong: no crossed feedback, SPREAD 100% stands in"}.get(typ)
        if extra:
            notes.append("delay " + extra)
    if case is not None:
        case.append(("D", typ, ms, fb_pct(v["fbk"]), pct(v["tone"]), pct(v["width"]), depth_pct(v["mod"]), ms,
                     fxknobs.real(knob(T, "DELAY", "FEEDBACK"), rec["feedback"]) / 100.0, free_tone, width / 100.0, mod / 100.0, free_hpf))
    return rec, typ


# ----------------------------------------------------------------------------------------------------------------- reverb
# Decay: the Drumboy network (the upstream Hall FDN) reaches RT60 = 0.8 x its DECAY setting, and so do the upstream Hall/Shimmer/Cathedral
# (rendered, fxcal/analyse_cal.py rt table: Hall 0.76-0.82, Shimmer 0.77-0.80, Cathedral 0.73-0.79 x nominal), so those keep the nominal
# decay; Plate measures 0.92-1.0 x nominal (-> DECAY x 1.2), Spring 0.75-0.87 (x 1.0). Room's EDC reads only 0.11-0.32 s because its
# tail is early reflections (ATLAS: its late tail decays at ~0.4-0.9 s, 8.5 dB under): half the nominal decay splits the two.
REV_DECAY_FACTOR = [0.5, 1.0, 1.2, 1.0, 1.0, 1.0]
REV_SIZE = {0: 0.0, 2: 35.0, 3: 25.0, 5: 100.0}          # fixed SIZE for the types whose geometry the Hall network cannot reach (Hall/Shimmer: the size row)
REV_WIDTH = {0: 60.0, 3: 70.0, 5: 70.0}                   # narrower than Hall/Plate in the atlas (corr 0.9 vs 0.0-0.3), the rest 100
REV_MOD_SCALE = {5: 0.5}                                  # Cathedral modulates 3 samples where Hall does 6


def map_reverb(v, lo, hi, notes, case=None):
    typ = int(v["type"])
    tname = REV_TYPES[typ]
    dlo, dhi = REV_DECAY[typ]
    nominal = rev_decay_ms(v["decay"])
    applied = min(max(nominal, dlo), dhi)
    decay_s = applied / 1000.0 * REV_DECAY_FACTOR[typ]
    d = pct(v["damp"]) / 100.0
    if typ == 5:
        damp_hz = 20000.0 * (1200.0 / 20000.0) ** d
    else:
        damp_hz = 20000.0 * (2000.0 / 20000.0) ** d
    plo, phi = REV_PRE[typ]
    pre = min(max(predly_ms(v["predly"]), plo), phi)
    size = REV_SIZE.get(typ, space_pct(v["size"]))
    width = REV_WIDTH.get(typ, 100.0)
    mod = depth_pct(v["mod"]) * REV_MOD_SCALE.get(typ, 1.0)
    lowcut = free_low = 30.0 if typ in (1, 4, 5) else 20.0
    if lo is not None:
        lowcut = chain_hz(lo)
    rec = dict(size=S("REVERB", "SIZE", size), decay=S("REVERB", "DECAY", decay_s), damp=S("REVERB", "DAMP", damp_hz),
               predelay=S("REVERB", "PRE-DLY", pre), width=S("REVERB", "WIDTH", width), mod=S("REVERB", "MOD", mod),
               lowcut=S("REVERB", "LOW CUT", lowcut))
    if typ != 1:
        notes.append("reverb %s -> the Drumboy's one hall-like network (size/decay/damp chosen for a similar tail)" % tname)
    if applied != nominal:
        notes.append("reverb decay %.1f s -> %.1f s (the %s band %.1f-%.1f s)" % (nominal / 1000.0, applied / 1000.0, tname, dlo / 1000.0, dhi / 1000.0))
    if decay_s > 12.0:
        notes.append("reverb decay clamped to 12 s")
    if typ == 0 and "erlate" in v:
        notes.append("reverb Room: early-reflection pattern not modelled (the Drumboy network has none)")
    if typ == 4:
        notes.append("reverb Shimmer: the octave-up feedback is not modelled (shimmer=%d)" % v["shimmer"])
    if typ == 3:
        notes.append("reverb Spring: springs/drip/boing not modelled")
    if case is not None:
        case.append(("R", typ, nominal, predly_ms(v["predly"]), pct(v["damp"]), space_pct(v["size"]), depth_pct(v["mod"]),
                     fxknobs.real(knob(T, "REVERB", "SIZE"), rec["size"]) / 100.0, fxknobs.real(knob(T, "REVERB", "DECAY"), rec["decay"]),
                     fxknobs.real(knob(T, "REVERB", "DAMP"), rec["damp"]), fxknobs.real(knob(T, "REVERB", "PRE-DLY"), rec["predelay"]),
                     fxknobs.real(knob(T, "REVERB", "WIDTH"), rec["width"]) / 100.0, fxknobs.real(knob(T, "REVERB", "MOD"), rec["mod"]) / 100.0,
                     free_low))
    return rec, typ


# ----------------------------------------------------------------------------------------------------------------- chorus
MODE_RATE = {0: 1.0, 1: 1.68, 2: (1.0 + 1.68) / 2}      # Ensemble `mode` I / II / I+II (ChorusEffect kModeIIRateMult)
MODE_DEPTH = {0: 1.0, 1: 1.25, 2: 1.125}


def map_chorus(v, lo, hi, explicit, notes, case=None):
    typ = int(v["type"])
    tname = CHO_TYPES[typ]
    base, swing = CHO_GEOM[typ]
    mode = int(v["mode"]) if typ == 2 else 0
    rate = chorus_rate_hz(v["rate"]) * MODE_RATE[mode]
    delay = 12.0 if typ == 3 else (max(base, 5.0))
    depth_h = pct(v["depth"]) / 100.0 * MODE_DEPTH[mode]
    depth = min(100.0, depth_h * swing / (0.5 * delay) * 100.0)
    fb = min(fb_pct(v["fbk"]) / 110.0 * 0.95 * 100.0, 70.0)
    width = 0.0 if typ == 3 else pct(v["width"]) * 0.5
    tone_hz = 800.0 * 22.5 ** (pct(v["tone"]) / 100.0)
    hp = free_hp = hp_corner_hz(v["hp"])
    if lo is not None:
        hp = max(hp, chain_hz(lo))
    rec = dict(rate=S("CHORUS", "RATE", rate), depth=S("CHORUS", "DEPTH", depth), delay=S("CHORUS", "DELAY", delay),
               feedback=S("CHORUS", "FEEDBACK", fb), width=S("CHORUS", "WIDTH", width), tone=S("CHORUS", "TONE", tone_hz),
               hpf=S("CHORUS", "HPF", hp))
    if typ == 2:
        notes.append("chorus Ensemble (mode %s, %d voices) -> one modulated line; the voice swirl is not modelled" % (["I", "II", "I+II"][mode], v["voices"]))
    if typ == 3:
        notes.append("chorus Vibrato is wet-only in the original; the send adds the wet to the dry (chorus-like, not a pure vibrato)")
    if depth_h * swing / (0.5 * delay) > 1.0 + 1e-9:
        notes.append("chorus depth clamped to 100%")
    if case is not None:
        case.append(("C", typ, mode, chorus_rate_hz(v["rate"]), pct(v["depth"]), pct(v["width"]), fb_pct(v["fbk"]), pct(v["tone"]),
                     int(v["voices"]), hp_corner_hz(v["hp"]),
                     fxknobs.real(knob(T, "CHORUS", "RATE"), rec["rate"]), fxknobs.real(knob(T, "CHORUS", "DEPTH"), rec["depth"]) / 100.0,
                     fxknobs.real(knob(T, "CHORUS", "DELAY"), rec["delay"]), fxknobs.real(knob(T, "CHORUS", "FEEDBACK"), rec["feedback"]) / 100.0,
                     fxknobs.real(knob(T, "CHORUS", "WIDTH"), rec["width"]) / 100.0, fxknobs.real(knob(T, "CHORUS", "TONE"), rec["tone"]),
                     free_hp))
    return rec, typ


# ----------------------------------------------------------------------------------------------------------------- amp / drive
AMP_MODEL_MAP = {0: 1, 1: 2, 2: 3, 3: 3, 4: 4, 5: 1}          # Clean Crunch Lead Hi-gain Bass Acoustic -> CLEAN CRUNCH LEAD LEAD BASS CLEAN
CAB_MAP = {0: 3, 1: 2, 2: 1, 3: 1, 4: 3, 5: 0}                # 4x12 2x12 1x12 1x10 8x10 Direct -> 4X12 2X12 1X12 1X12 4X12 OFF


def blend_scale(mix127):
    """a parallel (mix < 100 %) drive/amp: the Drumboy amp is a full insert with no MIX, so DRIVE is scaled 0.5 + 0.5 mix"""
    return 0.5 + 0.5 * min(max(mix127 / 127.0, 0.0), 1.0)


def tone_db(p):       # upstream tone percent (50 = noon) -> Drumboy +-12 dB
    return (pct(p) - 50.0) / 50.0 * 12.0


def amp_rec(model, drive, bass, mid, treble, presence, cab, level):
    return dict(model=model, drive=S("AMP", "DRIVE", drive), cab=cab, bass=S("AMP", "BASS", bass), mid=S("AMP", "MID", mid),
                treble=S("AMP", "TREBLE", treble), presence=S("AMP", "PRESENCE", presence), level=S("AMP", "LEVEL", level))


def amp_off():
    d = defaults("AMP")
    return dict(model=0, drive=d["DRIVE"], cab=d["CAB"], bass=d["BASS"], mid=d["MID"], treble=d["TREBLE"], presence=d["PRESENCE"], level=d["LEVEL"])


def map_gamp(v, lo, hi, notes):
    m = int(v["model"])
    model = AMP_MODEL_MAP[m]
    cab = CAB_MAP[int(v["cab"])]
    scale = blend_scale(v["mix"])
    drive = drive_db(v["gain"]) / 36.0 * 100.0 * scale
    rec = amp_rec(model, drive, tone_db(v["bass"]), tone_db(v["mid"]), tone_db(v["treble"]), pct(v["presence"]), cab, level_db(v["level"]))
    notes.append("amp %s (cab %s, mic %d) -> MODEL %s, CAB %s" % (AMP_MODELS[m], CAB_TYPES[int(v["cab"])], v["mic"], ["off", "CLEAN", "CRUNCH", "LEAD", "BASS"][model],
                                                                  ["OFF", "1X12", "2X12", "4X12"][cab]))
    if m == 5:
        notes.append("amp Acoustic has no Drumboy model: CLEAN")
    if v["mix"] < 127 * 0.999:
        notes.append("amp mix %.0f%% has no knob: DRIVE scaled x%.2f; the Drumboy amp is mono out" % (pct(v["mix"]), scale))
    return rec


def map_drive(v, lo, hi, notes):
    typ = int(v["type"])
    if typ in (3, 4):
        notes.append("drive %s (bit/sample-rate crusher) is not an amp: not mapped" % DRV_TYPES[typ])
        return None
    db = drive_db(v["drive"])
    heavy = typ in (1, 2) or db >= 20.0
    model = 3 if heavy else 2
    scale = blend_scale(v["mix"])
    drive = db / 36.0 * 100.0 * scale
    fhz = cutoff_hz(v["filter"])
    treble = 0.0 if fhz >= 8000.0 else max(-12.0, -4.0 * math.log2(8000.0 / fhz))
    bass = -6.0 if lo is not None else 0.0
    d = defaults("AMP")
    rec = amp_rec(model, drive, bass, 0.0, treble, 40.0, 0, level_db(v["level"]))
    notes.append("drive %s %.0f dB, mix %.0f%% -> MODEL %s, DRIVE %.0f%% (x%.2f for the parallel mix), CAB OFF" % (
        DRV_TYPES[typ], db, pct(v["mix"]), "LEAD" if heavy else "CRUNCH", drive, scale))
    if treble < 0:
        notes.append("drive post low-pass %.1f kHz -> TREBLE %.0f dB" % (fhz / 1000.0, treble))
    if lo is not None:
        notes.append("drive ran above %.0f Hz only (bass dry): BASS -6 dB instead, the amp is full-band" % chain_hz(lo))
    return rec


# ----------------------------------------------------------------------------------------------------------------- the whole chain
def map_preset(name, p, cases=None):
    rows = fx_rows(p)
    notes = []
    rec = dict(name=name, sends=[0, 0, 0], amp=amp_off(), delay=None, reverb=None, chorus=None, notes=notes)
    sat = []
    seen = collections.Counter()
    amp_done = False
    order = [r[0] for r in rows]
    for tid, v, lo, hi, explicit in rows:
        seen[tid] += 1
        first = seen[tid] == 1
        if tid in ("delay", "reverb", "chorus"):
            k = {"delay": 0, "reverb": 1, "chorus": 2}[tid]
            case = [] if cases is not None else None
            if not first:
                # a second instance: the louder mix wins, the settings of the first are kept
                m2 = v["mix"]
                if m2 > rec["_mix"][tid]:
                    rec["_mix"][tid] = m2
                    notes.append("second %s: louder mix %.0f%% wins for the send, settings of the first kept" % (tid, pct(m2)))
                else:
                    notes.append("second %s ignored (quieter mix)" % tid)
                continue
            rec.setdefault("_mix", {})[tid] = v["mix"]
            if tid == "delay":
                rec["delay"], typ = map_delay(v, lo, hi, notes, case)
            elif tid == "reverb":
                rec["reverb"], typ = map_reverb(v, lo, hi, notes, case)
            else:
                rec["chorus"], typ = map_chorus(v, lo, hi, explicit, notes, case)
            rec.setdefault("_type", {})[tid] = typ
            if case:
                cases.append(case[0])
        elif tid == "gamp" or tid == "drive":
            if amp_done:
                notes.append("second amp/drive instance ignored (%s)" % (
                    "gamp %s, cab %s, mix %.0f%%" % (AMP_MODELS[int(v["model"])], CAB_TYPES[int(v["cab"])], pct(v["mix"])) if tid == "gamp"
                    else "drive %s %.0f dB, mix %.0f%%" % (DRV_TYPES[int(v["type"])], drive_db(v["drive"]), pct(v["mix"]))))
                continue
            r = map_gamp(v, lo, hi, notes) if tid == "gamp" else map_drive(v, lo, hi, notes)
            if r is not None:
                rec["amp"] = r
                amp_done = True
        elif tid == "comp":
            notes.append("comp not mapped (master COMP is global): %s, thresh %.0f dB, ratio stored %d" % (COMP_TYPES[int(v["type"])], -60.0 * v["thresh"] / 127.0, v["ratio"]))
        elif tid == "phaser":
            notes.append("phaser not mapped (rate %.2f Hz, depth %.0f%%, mix %.0f%%)" % (0.01 * 1000.0 ** (v["rate"] / 127.0), pct(v["depth"]), pct(v["mix"])))
        elif tid == "vintage":
            notes.append("vintage not mapped (%s, mix %.0f%%)" % (SAMPLERS[int(v["model"])], pct(v["mix"])))
        elif tid == "eq":
            notes.append("eq not mapped (stored rows %s)" % ", ".join("%s=%s" % (k, v[k]) for k in sorted(explicit)) if explicit else "eq not mapped")
        elif tid == "schain":
            notes.append("schain (sidechain pump) not mapped")
        else:
            notes.append("%s not mapped" % tid)
    # sends from the mix of the (first) instance, with the wet-level correction of its type
    mixes = rec.pop("_mix", {})
    types = rec.pop("_type", {})
    for tid, k in (("delay", 0), ("reverb", 1), ("chorus", 2)):
        if tid in mixes:
            g = CAL.get(tid, {}).get(str(types[tid]), 1.0)
            s, clipped = send_from_mix(mixes[tid], g)
            rec["sends"][k] = s
            if clipped:
                notes.append("%s send saturated at 100%% (mix %.0f%% x level gain %.2f wants more)" % (tid, pct(mixes[tid]), g))
    if "delay" in order and "reverb" in order and order.index("delay") < order.index("reverb"):
        notes.append("delay -> reverb in series in the original (echoes also get the reverb); the sends are parallel")
    return rec


def build(cases=None, cut_file=None):
    cut = json.load(open(cut_file or CUT))["presets"]
    idx = load_originals()
    recs = []
    for c in cut:
        p = idx[(c["bank"], c["name"][:23])]
        recs.append(map_preset(c["name"], p, cases))
    return recs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--cases", help="write the calibration cases here")
    ap.add_argument("--cut", help="map this presets.json-style file's presets instead of content/data/cut.json (content/data/more/presets.json: the second round)")
    a = ap.parse_args()
    if a.cut and a.out == OUT:
        ap.error("--cut needs its own --out (the default is the first round's content/data/fxmap.json)")
    cases = [] if a.cases else None
    recs = build(cases, a.cut)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump(dict(format="drumboy-fxmap-1", presets=recs), open(a.out, "w"), indent=1)
    print("wrote", a.out, len(recs), "records")
    if a.cases:
        with open(a.cases, "w") as f:
            for i, c in enumerate(cases):
                f.write(" ".join([c[0], str(i)] + [("%.6g" % x) for x in c[1:]]) + "\n")
        print("wrote", a.cases, len(cases), "cases")


if __name__ == "__main__":
    main()
