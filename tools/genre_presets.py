#!/usr/bin/env python3
"""Builds preset files (.PRS) from patch descriptions written with the panel's knob names and real units, for one genre set: content/synthwave/, content/techno/, content/synthpop/, content/trance/, content/ebm/, content/royksopp/, content/delerium/ or content/voice/ (--set NAME, default synthwave).

  genre_presets.py build [--set NAME] [BATCH.py ...] [-d DIR]   write the ACCEPTED presets of the given batch files (default: every content/NAME/batch*.py) into DIR
                                                (default content/NAME): rated 3 or more in content/NAME/ratings.json, and not changed since they were
                                                rated. A note in the rating that names a panel category files the preset there. Nothing is touched unless every
                                                patch is valid. Only .PRS files in the set\'s id window (synthwave 141-299, techno 300-349, synthpop 350-399, trance 400-449, ebm 450-499, royksopp 600-649, delerium 650-699, voice 700-749) are replaced; others in DIR stay.
  genre_presets.py build --pending -d DIR           the patches that wait for a rating (new, or changed since they were rated and rated 3+ before): a scratch folder
                                                for a listening page (content/tools/run_batch.sh DIR out.html)
  genre_presets.py build --all -d DIR               every patch, whatever its rating
  genre_presets.py check [BATCH.py ...]             validate the batches (and their ids and names against the library), count accepted / pending / left out
  genre_presets.py banks                            the wavetable banks that have a converted file (the only ones a patch may use), with their names

A batch file defines BASE_ID (its first preset's id; ids are BASE_ID + the patch's position, so only ever append to a batch) and PATCHES, a list of
patch() calls:
    BASE_ID = 200
    PATCHES = [
        patch("Name", "PAD", voices=4, banks=(None, 131),
              OSC1=dict(WAVE=SAW, FINE=-8), FLT1=dict(MODEL=LADDER, CUTOFF=hz(900)), AMP=dict(DECAY=ms(250), SUSTAIN=pct(0)),
              sends=dict(delay=50, reverb=35, chorus=25),            # percent
              delay=dict(MODE=DOT8, FEEDBACK=45), reverb=dict(DECAY=3.2), chorus=dict(RATE=0.6), amp=dict(MODEL=CLEAN)),
    ]
A knob left out keeps the panel's default (read from firmware/src/app/Params.cpp and FxParams.cpp, so it cannot drift from the firmware); a name that is not
a knob of that page is an error, and so is a value outside the knob's range. Pages: OSC1 OSC2 SUB FLT1 FLT2 GRIT AMP MOD GLOB. Knob names are written with
underscores (ENV_AMT, VEL_SENS, PRE_DLY). An oscillator's WAVE knob here is only SIN TRI SAW SQR; a wavetable bank is given with banks=(osc1, osc2), and
then that oscillator's PD_SHAPE is the table position. UNISON, UNI_DET, UNI_BLND and WIDTH are on the SUB page (they act on OSC 1). The effects are given
in the units the FX screen shows (ms, Hz, seconds, percent, dB); a patch that gives none leaves the shared effects alone.
"""
import json, math, os, re, sys, traceback
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
import prs

# the ids a set may use: above the library's 10-140, below the second round's 500-596 (content/presets2; royksopp and delerium sit above it) and the 1000 where the panel's own saves start
SETS = {"synthwave": (141, 299), "techno": (300, 349), "synthpop": (350, 399), "trance": (400, 449), "ebm": (450, 499), "royksopp": (600, 649), "delerium": (650, 699), "voice": (700, 749)}
SETNAME = "synthwave"
def window(): return SETS[SETNAME]
def setdir(name=None): return os.path.join(ROOT, "content", name or SETNAME)
PAGES = prs.PAGES
WAVE_MAX = 4 + prs.BANKS - 1
FACTORY_NAMES = ["Saw Stack", "Algo Knock", "Iron Squelch", "Sub Pressure", "Glass Wide", "Saw Choir", "Pluck Fifth", "Tape Warble", "Soft Sheen", "Dimension Bloom", "Bare Slab"]


# ---- the firmware's own knob tables
def _read(path):
    return open(os.path.join(ROOT, "firmware/src/app", path)).read()

def _num(s):
    s = s.strip()
    return WAVE_MAX if s == "kWaveMax" else int(s)

def load_pages():
    """{page: [(name, default, lo, hi, is_list)]} for the nine synth pages, from Params.cpp's kPages."""
    src = _read("Params.cpp")
    body = src[src.index("static const PageDef kPages[PG_COUNT] = {"):]
    body = body[:body.index('{"SAMPLE"')]
    ent = re.findall(r'(?:\b(P|L)\("([^"]+)", *(-?\w+), *(-?\w+), *(-?\w+)|\b(NONE|RETIRED))', body)
    rows = []
    for kind, name, v, lo, hi, none in ent:
        rows.append(("-", 0, 0, 0, False) if none else (name, _num(v), _num(lo), _num(hi), kind == "L"))
    assert len(rows) == 72, len(rows)
    return {PAGES[i]: rows[i * 8:(i + 1) * 8] for i in range(9)}

def load_fx():
    """{view: [(name, default value 0..127, list size or 0, lo, hi, exp)]} for DELAY REVERB CHORUS AMP, from FxParams.cpp."""
    src = _read("FxParams.cpp")
    lists = {m.group(1): len(re.findall(r'"[^"]*"', m.group(2))) for m in re.finditer(r'static const char\* const (\w+)\[\] = \{([^}]*)\}', src)}
    out = {}
    for view, var in (("delay", "kDelay"), ("reverb", "kReverb"), ("chorus", "kChorus"), ("amp", "kAmp")):
        blk = src[src.index(f"static const FxParamDef {var}[kFxParams] = {{"):]
        blk = blk[:blk.index("};")]
        rows = []
        for kind, name, args in re.findall(r'\b(LIN|EXP|LST)\("([^"]+)", *([^)]*)\)', blk):
            a = [x.strip() for x in args.split(",")]
            if kind == "LST":
                rows.append((name, int(a[-1]), lists[a[0]], 0, 0, False))
            else:
                lo, hi, d = float(a[1]), float(a[2]), float(a[3])
                rows.append((name, _fx_value(kind == "EXP", lo, hi, d), 0, lo, hi, kind == "EXP"))
        assert len(rows) == 8, (view, len(rows))
        out[view] = rows
    return out

def _fx_value(exp, lo, hi, real):
    t = math.log(real / lo) / math.log(hi / lo) if exp else (real - lo) / (hi - lo)
    return max(0, min(127, int(round(127 * t))))

def load_bank_names():
    return [m.group(1) for m in re.finditer(r'^\{"([^"]+)", \d, \d\},', _read("WaveNames.inc"), re.M)]

def available_banks():
    """The catalogue numbers that have a converted wavetable file (content/data/wavetables/wtNNN.wt): the ones the card can be given."""
    d = os.path.join(ROOT, "content/data/wavetables")
    return sorted(int(m.group(1)) for f in (os.listdir(d) if os.path.isdir(d) else []) if (m := re.fullmatch(r"wt(\d{3})\.wt", f)))

PAGE_TABLE = load_pages()
FX_TABLE = load_fx()
BANK_NAMES = load_bank_names()


# ---- panel units -> knob values (the inverses of app/Params.cpp's formatParam); a value outside the knob's range is an error, not a clamp
def _dom(what, x, lo, hi):
    if isinstance(x, bool) or not isinstance(x, (int, float)) or not (lo <= x <= hi):
        raise prs.PresetError(f"{what}({x!r}): the panel's range is {lo}..{hi}")
    return x

def pct(p): return int(round(_dom("pct", p, 0, 100) * 1.27))
def hz(f): return int(round(127 * math.log(_dom("hz", f, 30, 16000) / 30.0) / math.log(16000.0 / 30.0)))
def ms(m): return int(round(127 * (max(_dom("ms", m, 0, 2500) - 2.0, 0.0) / 2500.0) ** (1 / 2.2)))
def lfo_hz(f): return int(round((_dom("lfo_hz", f, 0.2, 12.2) - 0.2) / 12.0 * 127))
def hpf_hz(f): return 0 if f == 0 else hz(_dom("hpf_hz", f, 3.9, 2000) * 8)     # the HPF knob: 0 = off, else cutoff / 8
def pan(p):                                                     # -100 (L) .. +100 (R), as the panel shows it
    _dom("pan", p, -100, 100)
    return int(round(64 + p * (0.64 if p < 0 else 0.63)))

# names for the list knobs
SIN, TRI, SAW, SQR = 0, 1, 2, 3
LP, BP, HP, NOTCH, LADDER, ACID, SOVIET, CLASSIC = range(8)
SERIES, PARALLEL = 0, 1
LFO_CUT, LFO_PITCH, LFO_AMP, LFO_PD = range(4)       # "PD AMT" is the wavetable position where the oscillator plays a bank
FREE, Q4, DOT8, E8, DOT16, S16, T8, T16 = range(8)    # delay MODE
AMP_OFF, CLEAN, CRUNCH, LEAD, BASS = range(5)         # amp MODEL
CAB_OFF, CAB_1X12, CAB_2X12, CAB_4X12 = range(4)

# short forms for the batch files: an amp envelope, a filter, the oscillators and the effects, in real units
def adsr(a, d, s, r, vel=None, level=127):
    """AMP page: attack, decay, release in ms, sustain in percent; vel = VEL SENS percent (the panel's 80 when left out)."""
    k = dict(ATTACK=ms(a), DECAY=ms(d), SUSTAIN=pct(s), RELEASE=ms(r), LEVEL=level)
    if vel is not None:
        k["VEL_SENS"] = pct(vel)
    return k

def filt(model, cut, reso=10, drive=0, key=50, env=30):
    """FILTER 1: cutoff in Hz; reso, drive, key tracking and envelope amount in percent."""
    return dict(MODEL=model, CUTOFF=hz(cut), RESO=pct(reso), DRIVE=pct(drive), KEYTRK=pct(key), ENV_AMT=pct(env))

def o1(wave=SAW, fine=0, level=100, **kw):
    """OSC 1 on a basic wave (extra knobs: OCT, SEMI ...)."""
    return dict(WAVE=wave, FINE=fine, LEVEL=pct(level), **kw)

def o2(wave=SAW, semi=0, fine=0, level=100, **kw):
    """OSC 2: the pitch is always given (the panel's default is a fifth up)."""
    return dict(WAVE=wave, SEMI=semi, FINE=fine, LEVEL=pct(level), **kw)

def ob(pos, fine=0, level=100, **kw):
    """An oscillator on a wavetable bank (banks=); pos is the table position 0-127."""
    return dict(PD_SHAPE=pos, FINE=fine, LEVEL=pct(level), **kw)

def ob2(pos, semi=0, fine=0, level=100, **kw):
    return dict(PD_SHAPE=pos, SEMI=semi, FINE=fine, LEVEL=pct(level), **kw)

def dly(mode=DOT8, fb=45, tone=4500, spread=80, hpf=200): return dict(MODE=mode, FEEDBACK=fb, TONE=tone, SPREAD=spread, HPF=hpf)
def rev(size=70, decay=3.5, damp=5000, pre=25, low=None):
    """Reverb settings; low = LOW CUT in Hz (the panel's 120 Hz default removes the low end from the reverb: lower it for bass and sub sounds)."""
    k = dict(SIZE=size, DECAY=decay, DAMP=damp, PRE_DLY=pre)
    if low is not None:
        k["LOW_CUT"] = low
    return k
def cho(rate=0.6, depth=45, delay=12): return dict(RATE=rate, DEPTH=depth, DELAY=delay)

def _norm(s):
    """A knob's name as a Python keyword: ENV AMT -> ENV_AMT, PRE-DLY -> PRE_DLY, VEL>ATK -> VELATK (case and punctuation do not matter)."""
    return re.sub(r"[^A-Z0-9]", "", s.upper())

def _need(x, kind, what):
    if not isinstance(x, kind):
        raise prs.PresetError(f"{what} must be {'a dict' if kind is dict else kind.__name__}, not {x!r}")

def _page(page, given):
    _need(given, dict, f"{page}=")
    rows = PAGE_TABLE[page]
    vals = [r[1] for r in rows]
    names = [_norm(r[0]) for r in rows]
    for k, v in given.items():
        if _norm(str(k)) not in names or _norm(str(k)) == "":
            raise prs.PresetError(f"{page} has no knob {k!r} (it has {', '.join(r[0] for r in rows if r[0] != '-')})")
        i = names.index(_norm(k))
        n, d, lo, hi, _ = rows[i]
        if page in ("OSC1", "OSC2") and i == 0:
            hi = 3   # a bank is given with banks=, never through WAVE (the firmware takes WAVE 0-3 from the file and the bank from the bank field)
        if isinstance(v, bool) or not isinstance(v, int) or not (lo <= v <= hi):
            raise prs.PresetError(f"{page} {k}={v!r}: a whole number {lo}..{hi} is needed" + (" (a wavetable bank goes in banks=)" if hi == 3 else ""))
        vals[i] = v
    return vals

def _fx(view, given):
    _need(given, dict, f"{view}=")
    rows = FX_TABLE[view]
    vals = [r[1] for r in rows]
    names = [_norm(r[0]) for r in rows]
    for k, v in given.items():
        if _norm(str(k)) not in names:
            raise prs.PresetError(f"{view} has no knob {k!r} (it has {', '.join(r[0] for r in rows)})")
        i = names.index(_norm(k))
        n, d, size, lo, hi, exp = rows[i]
        if isinstance(v, bool) or not isinstance(v, (int, float)):
            raise prs.PresetError(f"{view} {k}={v!r}: a number is needed")
        if size:
            if v != int(v) or not (0 <= v < size):
                raise prs.PresetError(f"{view} {k}={v!r}: a list position 0..{size - 1} is needed")
            vals[i] = int(v)
        else:
            if not (lo - 1e-9 <= v <= hi + 1e-9):
                raise prs.PresetError(f"{view} {k}={v!r} is outside {lo}..{hi}")
            vals[i] = _fx_value(exp, lo, hi, v)
    return vals

def patch(name, cat, voices, banks=(None, None), sends=None, delay=None, reverb=None, chorus=None, amp=None, **pages):
    if not isinstance(name, str):
        raise prs.PresetError(f"a patch name must be text, not {name!r}")
    bad = set(pages) - set(PAGES)
    if bad:
        raise prs.PresetError(f"{name}: unknown page {sorted(bad)} (pages: {', '.join(PAGES)})")
    sends = {} if sends is None else sends
    _need(sends, dict, f"{name}: sends=")
    if set(sends) - {"delay", "reverb", "chorus"}:
        raise prs.PresetError(f"{name}: sends are delay, reverb, chorus")
    if not isinstance(banks, (list, tuple)) or len(banks) != 2:
        raise prs.PresetError(f"{name}: banks= needs two entries (osc 1, osc 2), each a catalogue number or None")
    have = available_banks()
    for b in banks:
        if b is not None and (isinstance(b, bool) or not isinstance(b, int) or b not in have):
            nm = BANK_NAMES[b] if isinstance(b, int) and 0 <= b < len(BANK_NAMES) else "?"
            raise prs.PresetError(f"{name}: bank {b!r} ({nm}) has no converted wavetable file, so the card could not play it (genre_presets.py banks lists the ones that have)")
    for k, v in sends.items():
        if isinstance(v, bool) or not isinstance(v, (int, float)) or not (0 <= v <= 100):
            raise prs.PresetError(f"{name}: send {k}={v!r}: percent 0..100")
    built = {p: _page(p, pages.get(p, {})) for p in PAGES}
    for osc, b in (("OSC1", banks[0]), ("OSC2", banks[1])):
        given = {_norm(str(k)) for k in pages.get(osc, {})} if isinstance(pages.get(osc, {}), dict) else set()
        if "PDSHAPE" in given and b is None and "PDAMT" not in given:   # on a basic wave PD SHAPE is the phase-distortion shape and does nothing while PD AMT is 0
            raise prs.PresetError(f"{name}: {osc} sets PD_SHAPE (a table position) but has no bank in banks= (use o1/o2, or give banks)")
    given2 = {_norm(str(k)) for k in pages.get("OSC2", {})} if isinstance(pages.get("OSC2", {}), dict) else set()
    if built["OSC2"][6] > 0 and not {"SEMI", "FINE"} <= given2:   # the panel's OSC 2 defaults are +7 semitones and +6 cents: a fifth up, never what a patch wants by accident
        raise prs.PresetError(f"{name}: OSC2 is audible, so give its SEMI and FINE (the panel's defaults, +7 and +6, are a fifth up and slightly sharp)")
    spec = {"name": name, "category": cat, "voices": voices, "banks": list(banks),
            "pages": built,
            "fx": {"sends": [pct(sends.get(k, 0)) for k in ("delay", "reverb", "chorus")],
                   "delay": _fx("delay", delay) if delay is not None else None,
                   "reverb": _fx("reverb", reverb) if reverb is not None else None,
                   "chorus": _fx("chorus", chorus) if chorus is not None else None,
                   "amp": _fx("amp", amp) if amp is not None else None}}
    prs.from_spec(spec, window()[0])   # category, voices and the rest: the same checks the file format makes, now, with the patch's name in the message
    return spec


# ---- batches
def _natural(path):
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", os.path.basename(path))]

def default_batches(name=None):
    d = setdir(name)
    return sorted((os.path.join(d, f) for f in os.listdir(d) if re.fullmatch(r"batch\d+\.py", f)), key=_natural) if os.path.isdir(d) else []

def _run_batch(f):
    try:
        code = compile(open(f, encoding="utf-8").read(), f, "exec")
    except OSError as e:
        raise prs.PresetError(f"{f}: {e.strerror or e}")
    except UnicodeDecodeError as e:
        raise prs.PresetError(f"{f}: not UTF-8 text ({e.reason})")
    except (SyntaxError, ValueError) as e:
        raise prs.PresetError(f"{f}:{getattr(e, 'lineno', 0)}: {getattr(e, 'msg', e)}")
    g = {k: v for k, v in globals().items() if not k.startswith("__")}
    try:
        exec(code, g)
    except BaseException as e:   # a typo in a batch file (even a stray sys.exit): say where
        fr = [x for x in traceback.extract_tb(e.__traceback__) if x.filename == f]
        where = f"{f}:{fr[-1].lineno}" if fr else f
        raise prs.PresetError(f"{where}: {e}" if isinstance(e, prs.PresetError) else f"{where}: {type(e).__name__}: {e}")
    return g

def load_batches(files):
    """[(id, spec)] of every patch of the batch files; ids are BASE_ID + position."""
    if not files:
        raise prs.PresetError(f"no batch files (content/{SETNAME}/batch*.py)")
    out, used = [], {}
    for f in files:
        g = _run_batch(f)
        patches, base = g.get("PATCHES"), g.get("BASE_ID")
        if not isinstance(patches, list) or not patches or any(not isinstance(p, dict) for p in patches):
            raise prs.PresetError(f"{f}: PATCHES must be a list of patch(...) calls, and not empty")
        if isinstance(base, bool) or not isinstance(base, int) or base < window()[0] or base + len(patches) - 1 > window()[1]:
            raise prs.PresetError(f"{f}: BASE_ID must be a whole number with all its ids in {window()[0]}..{window()[1]} (the {SETNAME} set's window; this batch has {len(patches)} patches)")
        for i, s in enumerate(patches):
            pid = base + i
            if pid in used:
                raise prs.PresetError(f"{f}: id {pid} is also {used[pid]} (give the batch another BASE_ID)")
            used[pid] = f"{os.path.basename(f)} #{i + 1}"
            out.append((pid, s))
    names = [s["name"] for _, s in out]
    lowered = [n.lower() for n in names]
    dup_ci = sorted({n for n in names if lowered.count(n.lower()) > 1})
    if dup_ci:
        raise prs.PresetError(f"patch names used twice (case does not matter): {dup_ci}")
    others = set()
    for other in SETS:   # a name is used once across the genre sets as well
        if other != SETNAME:
            for f in default_batches(other):
                others |= {x["name"] for x in (_run_batch(f).get("PATCHES") or [])}
    dup = sorted({n for n in names if names.count(n) > 1})
    if dup:
        raise prs.PresetError(f"patch names used twice: {dup}")
    lib = set(FACTORY_NAMES)
    for d in ("presets", "presets2"):
        libdir = os.path.join(ROOT, "content", d)
        if os.path.isdir(libdir):
            lib |= {p["name"] for _, p in prs.load_dir(libdir, strict=False)}
    lib |= others
    lib |= {n for i, n in prs.library_cuts().items() if i < 141 or 500 <= i < 600}   # left out of round 1 or 2 (their files are gone): the name stays taken
    lib_ci = {n.lower() for n in lib}
    clash = sorted(n for n in names if n.lower() in lib_ci)
    if clash:
        raise prs.PresetError(f"names the library already has: {clash}")
    return out

def load_ratings():
    """{name: (rating, note)} from content/NAME/ratings.json, and the names whose sound changed after they were rated."""
    path = os.path.join(setdir(), "ratings.json")
    if not os.path.exists(path):
        return {}, set()
    where = f"content/{SETNAME}/ratings.json"
    try:
        d = json.load(open(path))
        rated = {}
        for r in d["ratings"]:
            if r["name"] in rated:
                raise prs.PresetError(f"{where}: {r['name']!r} is rated twice")
            rating = r["rating"]
            if isinstance(rating, bool) or not isinstance(rating, int) or not (0 <= rating <= 5):
                raise prs.PresetError(f"{where}: {r['name']!r} has the rating {rating!r} (0-5)")
            note = r.get("note") or ""
            if not isinstance(note, str):
                raise prs.PresetError(f"{where}: the note of {r['name']!r} is not text")
            rated[r["name"]] = (rating, note.strip())
        stale = d.get("stale", [])
        if not isinstance(stale, list) or any(not isinstance(n, str) for n in stale):
            raise prs.PresetError(f"{where}: 'stale' must be a list of names")
    except prs.PresetError:
        raise
    except (ValueError, KeyError, TypeError, AttributeError) as e:
        raise prs.PresetError(f"{where}: {type(e).__name__}: {e} (an export of the listening page has {{\"ratings\": [{{\"name\", \"rating\", \"note\"}}, ...]}})")
    return rated, set(stale)

def voted_out_names():
    """The names voted out on the similarity page or skipped on the Golden Presets page (content/data/similarity_cuts.json, golden_votes.json): out whatever their rating."""
    return prs.cut_names()

def status(name, rated, stale):
    """in: rated 3+ and unchanged since; pending: new, or changed since a 3+ rating; out: the rest (rating 0 means not rated) and the ones voted out as too similar or skipped on the golden page (content/data/similarity_cuts.json, golden_votes.json)."""
    if name in voted_out_names():
        return "out"
    if name not in rated:
        return "pending"
    rating, _ = rated[name]
    if rating == 0:
        return "out"
    if name in stale:
        return "pending" if rating >= 3 else "out"
    return "in" if rating >= 3 else "out"

def select(specs, which):
    """The (id, spec) pairs of one kind ('in', 'pending', 'all'); an accepted preset's rating note, when it names a panel category, files it there."""
    rated, stale = load_ratings()
    out = []
    for pid, s in specs:
        st = status(s["name"], rated, stale)
        if which != "all" and st != which:
            continue
        note = rated.get(s["name"], (0, ""))[1].upper()
        if st == "in" and note in prs.CATS[1:] and note != s["category"]:
            s = dict(s, category=note)
        out.append((pid, s))
    return out

def counts(specs):
    rated, stale = load_ratings()
    c = {"in": 0, "pending": 0, "out": 0}
    for _, s in specs:
        c[status(s["name"], rated, stale)] += 1
    return c

def encode_all(specs):
    """[(file name, bytes)], every patch encoded (so any error is found before a file is touched)."""
    out = []
    for pid, s in specs:
        try:
            p = prs.from_spec(dict(s, name=prs.renamed(s["name"])), pid)   # content/data/renames.json
            out.append((prs.file_name(p), prs.encode(p)))
        except prs.PresetError as e:
            raise prs.PresetError(f"{s.get('name', '?')}: {e}")
    return out

def write_all(files, outdir):
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):   # the files an earlier build made (our id range); a preset of yours or the library's is left alone
        path = os.path.join(outdir, f)
        if f.upper().endswith(".PRS") and os.path.isfile(path):
            try:
                pid = prs.decode(open(path, "rb").read())["id"]
            except (prs.PresetError, OSError):
                continue
            if window()[0] <= pid <= window()[1]:
                os.remove(path)
    for name, data in files:
        open(os.path.join(outdir, name), "wb").write(data)


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    cmd, args = argv[1], argv[2:]
    try:
        if cmd == "banks":
            have = available_banks()
            print(f"{len(have)} banks have a converted file:")
            print("  " + ", ".join(f"{b}:{BANK_NAMES[b]}" for b in have))
            return 0
        if cmd not in ("build", "check"):
            print(f"genre_presets.py: unknown command {cmd!r} (build, check, banks)\n", file=sys.stderr)
            print(__doc__)
            return 2
        global SETNAME
        if "--set" in args:
            i = args.index("--set")
            if i + 1 >= len(args) or args[i + 1] not in SETS:
                raise prs.PresetError(f"--set needs one of {', '.join(SETS)}")
            SETNAME = args[i + 1]
            del args[i:i + 2]
        outdir = setdir()
        if "-d" in args:
            i = args.index("-d")
            if i + 1 >= len(args):
                raise prs.PresetError("-d needs a folder")
            outdir = args[i + 1]
            del args[i:i + 2]
        which = "in"
        for flag, w in (("--pending", "pending"), ("--all", "all")):
            if flag in args:
                which = w
                args.remove(flag)
        bad = [a for a in args if a.startswith("-")]
        if bad:
            raise prs.PresetError(f"unknown option {bad[0]}")
        if which != "in" and outdir == setdir():
            raise prs.PresetError("--pending and --all need a scratch folder: -d DIR (content/" + SETNAME + " holds the accepted presets)")
        specs = load_batches(args or default_batches())
        encode_all(specs)   # (every patch must be valid, accepted or not)
        files = encode_all(select(specs, which))
        if cmd == "build":
            write_all(files, outdir)
            print(f"{len(files)} {which if which != 'in' else 'accepted'} presets in {outdir}")
        else:
            c = counts(specs)
            print(f"{len(specs)} patches ok (ids {specs[0][0]}..{specs[-1][0]}): {c['in']} accepted, {c['pending']} waiting for a rating, {c['out']} left out")
        return 0
    except (prs.PresetError, OSError) as e:
        print(f"genre_presets.py: {e}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main(sys.argv))
