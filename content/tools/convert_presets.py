#!/usr/bin/env python3
"""convert_presets.py - CATHODE preset JSON -> Drumboy panel pages, with a per-feature loss report and a fit score.

The registry laws (Registry.to_unit / from_unit) are the ones in ref/upstream/docs/dev/sound-design/tools/convert_patch.py,
which mirror core/src/UnitCurves.cpp; ranges, curves and defaults come from docs/dev/cathode-registry.tsv, as
docs/dev/cathode-preset-mapping.md asks. The Drumboy side (knob ranges, the cost model) is read from the firmware sources through
`panel_tool` (content/tools/panel_tool.cpp), never copied by hand.

    convert_presets.py convert  [--set selection.json] [--tables brought.json] [--out content/data]
    (library use: analyse_fit.py imports load_presets / Converter)

A conversion returns a dict:
    pages    9 x 8 ints in the Drumboy's ranges (WAVE clamped to 0..3: today's firmware)
    ext      what the firmware cannot hold yet: osc1/osc2 wavetable (catalogue index, position), recommended voices
    items    one record per source feature: {feat, status exact|approx|dropped, loss, note}
    loss     sum of the losses; score = max(0, 100 - loss)
"""
import collections, csv, glob, json, math, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
HYB = os.path.join(REPO, "ref", "upstream")
REG_TSV = os.path.join(HYB, "docs", "dev", "cathode-registry.tsv")
ARCHIVE = os.path.join(HYB, "docs", "dev", "sound-design", "archive", "presets")
SCRATCH = os.environ.get("DBOY_CONTENT_SCRATCH", "/tmp/dboy-content")
PANEL_TOOL = os.environ.get("DBOY_PANEL_TOOL", os.path.join(SCRATCH, "build", "panel_tool"))
CATALOGUE = os.path.join(REPO, "content", "data", "wavetables", "catalogue.json")

PG = {n: i for i, n in enumerate(["OSC1", "OSC2", "SUB", "FLT1", "FLT2", "GRIT", "AMP", "MOD", "GLOB"])}
SRC_NAME = ["Env1", "Env2", "Env3", "Lfo1", "Lfo2", "Lfo3", "Step", "S&H", "Vel", "Key", "Wheel", "ConvDr", "AT", "Slide", "DD1", "DD2",
            "Drift1", "Drift2", "Drift3", "MGrit", "MMot", "MPos", "MPd"]


# ------------------------------------------------------------------------------------------------------------ registry laws
class Registry:
    def __init__(self, path=REG_TSV):
        self.rows = {}
        with open(path, newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f, delimiter="\t"):
                if r["machine"] == "cathode":
                    self.rows[r["token"][6:] if r["token"].startswith("synth.") else r["token"]] = r

    def default(self, tok):
        return int(self.rows[tok]["default"])

    def vrange(self, tok):
        r = self.rows[tok]
        return int(r["min"]), int(r["max"])

    def labels(self, tok):
        return (self.rows[tok]["enumLabels"] or "").split("|")

    def _c(self, tok):
        r = self.rows[tok]
        if r["curveKind"] in ("-", ""):
            return None
        f = lambda k: float(r[k]) if r[k] not in ("-", "") else 0.0
        return dict(kind=r["curveKind"], vLo=f("vLo"), vHi=f("vHi"), lo=f("unitLo"), hi=f("unitHi"), shape=f("shape"), off=r["offAtZero"] == "1")

    def unit(self, tok, v):
        c = self._c(tok)
        if c is None:
            return float(v)
        if c["off"] and v <= 0:
            return 0.0
        vc = min(max(float(v), c["vLo"]), c["vHi"])
        t = (vc - c["vLo"]) / (c["vHi"] - c["vLo"])
        k = c["kind"]
        if k == "Linear":
            return c["lo"] + (c["hi"] - c["lo"]) * t
        if k == "Exp":
            return c["lo"] * (c["hi"] / c["lo"]) ** t
        if k == "ExpZero":
            b = c["shape"]
            return (c["lo"] + b) * ((c["hi"] + b) / (c["lo"] + b)) ** t - b
        if k == "Level":   # dB
            return 40 * math.log10(max(vc, 1e-9) / 100.0) if vc <= 100 else c["hi"] * (vc - 100) / (c["vHi"] - 100)
        if k == "CentredLinear":
            if vc < 64:
                return c["shape"] + (vc - 64) * (c["shape"] - c["lo"]) / (64 - c["vLo"])
            return c["shape"] + (vc - 64) * (c["hi"] - c["shape"]) / (c["vHi"] - 64)
        if k == "BipolarPercent":
            return vc * c["lo"] / c["vLo"] if vc < 0 else vc * c["hi"] / c["vHi"]
        if k == "Raw":
            return vc
        raise ValueError(k)


REG = Registry()


# ------------------------------------------------------------------------------------------------------------ Drumboy panel
class Panel:
    """Knob ranges from the firmware's own Params.cpp (panel_tool ranges)."""

    def __init__(self):
        path = os.path.join(SCRATCH, "ranges.json")
        if not os.path.exists(path):
            os.makedirs(SCRATCH, exist_ok=True)
            open(path, "w").write(subprocess.check_output([PANEL_TOOL, "ranges"], text=True))
        d = json.load(open(path))
        self.pages = d["pages"]
        self.cats = d["cats"]
        self.budget = d["budget"]
        self.lo = [[p["min"] for p in g["p"]] for g in self.pages]
        self.hi = [[p["max"] for p in g["p"]] for g in self.pages]
        self.df = [[p["def"] for p in g["p"]] for g in self.pages]

    def clamp(self, pg, i, v):
        return int(min(max(round(v), self.lo[pg][i]), self.hi[pg][i]))


PANEL = None


def panel():
    global PANEL
    if PANEL is None:
        PANEL = Panel()
    return PANEL


def costs(pages_list):
    """per-voice cost, heavy-rule ok, voices that fit the 5-voice budget, from the firmware's cost model."""
    if not pages_list:
        return []
    inp = "\n".join(" ".join(str(int(x)) for g in pg for x in g) for pg in pages_list) + "\n"
    out = subprocess.run([PANEL_TOOL, "cost"], input=inp, capture_output=True, text=True, check=True).stdout.split("\n")
    res = []
    for line in out:
        if not line:
            continue
        c, ok, fit = line.split()
        res.append((float(c), ok == "1", int(fit)))
    return res


# ------------------------------------------------------------------------------------------------------------ source presets
def load_presets(root=ARCHIVE):
    out = []
    for f in sorted(glob.glob(os.path.join(root, "*", "*.json"))):
        d = json.load(open(f))
        for p in d["machines"]["cathode"]:
            out.append(dict(p, _bank=os.path.basename(os.path.dirname(f)), _file=os.path.relpath(f, root)))
    return out


_cat = None


def catalogue():
    global _cat
    if _cat is None:
        _cat = json.load(open(CATALOGUE)) if os.path.exists(CATALOGUE) else []
    return _cat


def probe(table, pos127):
    """the catalogue probe nearest to the position: best Drumboy shape and its spectral similarity."""
    c = catalogue()
    if not c or table >= len(c):
        return None
    pr = c[table]["probe"]
    return pr[min(range(len(pr)), key=lambda q: abs(q / (len(pr) - 1) - pos127 / 127.0))]


# ------------------------------------------------------------------------------------------------------------ conversion
SHAPE_WAVE = {"SIN": 0, "TRI": 1, "SAW": 2, "SQR": 3}
ALGO_FID = {0: ("SAW", .5), 1: ("SAW", .3), 2: ("SIN", .25), 3: ("SAW", .2), 4: ("SAW", .3), 5: ("SAW", .3), 6: ("SAW", .2), 7: ("SAW", .25),
            8: ("SAW", .3), 9: ("SAW", .1), 10: ("SAW", .1), 11: ("SAW", .35), 12: ("SIN", .25), 13: ("SIN", .3), 14: ("SQR", .1), 15: ("SQR", .1)}
ALGO_NAME = ["Analog", "Shaper", "2-Op FM", "Grains", "Harmonics", "Tables", "Chords", "Vox", "Swarm", "Noise", "Particles", "String", "Modal", "Kick", "Snare", "Hat"]
# (model, type) -> (Drumboy MODEL index [LP BP HP NOTCH LADDER ACID SOVIET CLASSIC], fidelity)
FM = {"LP": 0, "BP": 1, "HP": 2, "NOTCH": 3, "LADDER": 4, "ACID": 5, "SOVIET": 6, "CLASSIC": 7}
# FILTER types: 0 LP 1 LP12 2 LP24 3 BP 4 HP 5 Notch 6 AllPass 7 Morph 8 Comb 9 Formant
FX_BASE = {"reverb": 7.0, "delay": 5.0, "chorus": 5.0, "phaser": 4.0, "drive": 4.0, "comp": 2.0, "eq": 2.5, "gamp": 4.0, "vintage": 4.0,
           "era": 3.0, "schain": 3.0, "fltr": 4.0}
ROW_B = 3.0
CAP = {"mod": 28.0, "fx": 22.0}
IMP = {"synth.filt.cutoff": 1.0, "synth.osc.pos": 1.0, "synth.osc2.level": .7, "synth.osc1.level": .7, "synth.noise.level": .5,
       "synth.pitch": .6, "synth.filt.reso": .6, "synth.filt.vowel": .5, "synth.level": .6, "synth.sub.level": .5, "synth.osc.pdAmount": .9,
       "synth.conv.rate": .4, "synth.conv.bits": .4, "synth.lfo1.rate": .3, "synth.lfo1.depth": .3, "synth.fold.amount": .6, "synth.filt.drive": .4}
VEL_NOMINAL = 0.75
VEL_FOLD = {"synth.filt.cutoff": None, "synth.osc.pos": None, "synth.osc.pdAmount": None, "synth.osc2.level": ("OSC2", 6), "synth.osc1.level": ("OSC1", 6),
            "synth.sub.level": ("SUB", 0), "synth.noise.level": ("SUB", 2), "synth.filt.drive": ("FLT1", 3)}
CAT_NEED = {"BASS": 2, "LEAD": 2, "PLUCK": 2, "SEQ": 2, "FX": 2, "PERC": 2, "BELL": 2, "BRASS": 3, "TEXTURE": 3, "PAD": 3, "POLY": 3, "KEYS": 3, "STAB": 3}
CAT_VOICES = {"BASS": 2, "LEAD": 2, "PLUCK": 3, "SEQ": 2, "FX": 2, "PERC": 2, "BELL": 3, "BRASS": 3, "TEXTURE": 3, "PAD": 4, "POLY": 4, "KEYS": 4, "STAB": 4}


def hz_to_cut(hz):
    return 127.0 * math.log(max(hz, 1e-3) / 30.0) / math.log(16000.0 / 30.0)


def ms_to_env(ms):
    return 127.0 * (max(ms - 2.0, 0.0) / 2500.0) ** (1.0 / 2.2)


def env_ms(v):
    return 2.0 + (v / 127.0) ** 2.2 * 2500.0


def q_to_reso(q):
    return 127.0 * (max(q - 0.707, 0.0) / 15.3) ** (1.0 / 1.5)


class Conv:
    """one preset's conversion; `brought` = catalogue indices whose wavetable the device would have (None: all, set(): none)."""

    def __init__(self, p, brought=None):
        self.p, self.brought = p, brought
        self.v = {k: REG.default(k) for k in REG.rows}
        self.v.update(p["vals"])
        self.items = []
        self.ext = {}
        self.pg = [list(g) for g in panel().df]   # start from the panel's defaults, set everything the preset says
        self.tables = []
        self.mat = [m for m in p.get("matrix", []) if m.get("src") is not None and m.get("depth", 0) != 0 and m.get("on", True) and m.get("dest")]
        self.dropped_tags = []
        self.approx_tags = []

    # ---- bookkeeping
    def add(self, feat, status, loss, note=""):
        self.items.append(dict(feat=feat, status=status, loss=round(loss, 2), note=note))
        if status == "dropped" and loss >= 0.5:
            self.dropped_tags.append(feat)
        elif status == "approx" and loss >= 0.8:
            self.approx_tags.append(feat)

    def u(self, tok):
        return REG.unit(tok, self.v[tok])

    def setp(self, page, i, val):
        self.pg[PG[page]][i] = panel().clamp(PG[page], i, val)

    # ---- oscillators
    def osc(self, n):
        v, pre = self.v, f"osc{n}"
        page = f"OSC{n}"
        on = v[pre + ".on"] != 0
        lvl = v[pre + ".level"] if on else 0
        mode, fid, note, wave_ext = v[pre + ".mode"], 1.0, "", None
        wave, pdshape, pdamt = 0, panel().df[PG[page]][1], 0
        if mode == 0:   # wavetable
            tab, pos = v.get(pre + ".table", 0), v.get(pre + ".pos", 0)
            pr = probe(tab, pos)
            have = self.brought is None or tab in self.brought
            if lvl < 8:   # inaudible: no table, no dependency
                wave, pdshape, fid, note = 2, panel().df[PG[page]][1], 1.0, "off"
            elif have and pr is not None:
                wave, pdshape, fid = SHAPE_WAVE[pr["best"]], pos, 0.95   # WAVE keeps the nearest shape (today's firmware); ext carries the table
                self.ext[pre] = dict(table=tab, pos=pos)
                self.tables.append(tab)
                note = f"table {tab} {catalogue()[tab]['id'].replace('factory.', '')}"
                if lvl >= 8 and v.get(pre + ".scanOrder", 0):
                    fid -= 0.05
            else:
                sim = pr["sim"] if pr else 0.3
                wave = SHAPE_WAVE[pr["best"]] if pr else 2
                fid = 0.15 + 0.5 * sim
                note = f"table {tab} -> {pr['best'] if pr else 'SAW'} (similarity {sim:.2f})"
                self.tables.append(tab)   # still a dependency: counted so the report can say which tables would have carried it
                pdshape = panel().df[PG[page]][1]
        elif mode == 1:
            t = v[pre + ".pdType"]
            wave, pdshape, pdamt = 0, v[pre + ".pdShape"], v[pre + ".pdAmount"]
            fid = 1.0 if t == 0 else 0.45
            note = "PD " + REG.labels(pre + ".pdType")[t]
        elif mode == 2:
            sh, fid = ALGO_FID[v[pre + ".engine"]]
            wave = SHAPE_WAVE[sh]
            if wave == 0:
                pdamt = 40
            note = "ALGO " + ALGO_NAME[v[pre + ".engine"]]
        elif mode == 3:
            wave, fid, note = 3, 0.2, "Bitmask"
        else:
            pwm = v.get(pre + ".pwm", 0)
            wave = 3
            fid = 0.85 if (abs(v[pre + ".pw"] - 64) < 6 and pwm == 0) else 0.55
            note = "Pulse (PW/PWM dropped)"
        # pitch: octaves + semitones + fine as one offset; the panel has OCT -2..2, SEMI -12..12, FINE +-50
        st = v[pre + ".oct"] * 12 + v[pre + ".semi"] + v[pre + ".fine"] / 100.0
        pitch_fid = 1.0
        if abs(st) > 36:
            pitch_fid = 0.6
        o = int(round(max(-36, min(36, st)) / 12.0))
        o = max(-2, min(2, o))
        rem = max(-36, min(36, st)) - 12 * o
        semi = int(round(max(-12, min(12, rem))))
        fine = v[pre + ".fine"] if (abs(st) <= 36 and abs(v[pre + ".fine"]) <= 50 and abs(rem - semi) < 1) else (rem - semi) * 100
        self.setp(page, 0, wave)
        self.setp(page, 1, pdshape)
        self.setp(page, 2, pdamt)
        self.setp(page, 3, o)
        self.setp(page, 4, semi)
        self.setp(page, 5, fine)
        self.setp(page, 6, lvl)
        return dict(level=lvl / 127.0, fid=fid * pitch_fid, note=note, mode=mode, env=v.get(pre + ".env.on", 0) and lvl >= 8,
                    wave_ext=wave_ext)

    def build_osc(self):
        layers = []
        o1, o2 = self.osc(1), self.osc(2)
        self.setp("OSC2", 7, self.v["sync"])
        sublvl = self.v["sub.level"]
        sub_fid, sub_note = 1.0, "sine sub"
        sm = self.v["sub.mode"]
        if sm == 0:
            w = self.v["sub.wave"]
            if w != 1:
                sub_fid, sub_note = 0.55, "sub square/pulse -> sine"
        else:
            sub_fid, sub_note = 0.3, "sub mode " + REG.labels("sub.mode")[sm] + " -> sine"
        so = self.v["sub.oct"]
        if so == 0:
            sub_fid *= 0.5
            sub_note += ", sub at 0 oct -> -1"
        self.setp("SUB", 0, sublvl)
        self.setp("SUB", 1, so if so in (-1, -2) else -1)
        nz = self.v["noise.level"]
        self.setp("SUB", 2, nz)
        ring = self.v["xmod.ring"]
        self.setp("SUB", 3, ring)
        layers.append(("osc1", o1["level"], o1["fid"], o1["note"], o1["env"]))
        layers.append(("osc2", o2["level"], o2["fid"], o2["note"], o2["env"]))
        layers.append(("sub", 0.6 * sublvl / 127.0, sub_fid, sub_note, self.v["sub.env.on"] and sublvl >= 10))
        nz_fid = 1.0 if self.v["noise.type"] == 0 else 0.8
        layers.append(("noise", 0.4 * nz / 127.0, nz_fid, "noise type " + REG.labels("noise.type")[self.v["noise.type"]], False))
        live = [l for l in layers if l[1] * 127 >= 5]
        tot = sum(l[1] for l in live) or 1.0
        for name, lv, fid, note, env in live:
            w = lv / tot
            self.add("osc:" + name, "exact" if fid >= 0.99 else ("approx" if fid >= 0.3 else "dropped"), 30.0 * w * (1 - fid), note)
            if env:
                self.add("osc-env:" + name, "dropped", 2.5 * min(1.0, 0.5 + w), "own envelope")
        # unison
        uni = max(self.v["unison"], self.v["osc1.uni.voices"])
        det = self.v["unisonDetune"] if self.v["unison"] > 1 else (self.v["osc1.uni.detune"] if self.v["osc1.uni.voices"] > 1 else 0)
        if uni > 4:
            self.add("unison", "approx", 1.0, f"{uni} voices -> 4")
            uni = 4
        if self.v["osc2.uni.voices"] > 1:
            self.add("osc2-unison", "dropped", 1.5, "unison on oscillator 2")
        self.setp("SUB", 4, uni)
        self.setp("SUB", 5, det)
        self.setp("OSC1", 7, det)
        self.setp("SUB", 6, self.v["osc1.uni.blend"] if self.v["osc1.uni.voices"] > 1 else panel().df[PG["SUB"]][6])
        sp = max(self.v["voice.spread"], self.v["osc1.uni.width"])
        if uni > 1:
            self.setp("SUB", 7, sp if sp > 0 else 64)
        else:
            self.setp("SUB", 7, 0)
            if sp > 20:
                self.add("voice.spread", "dropped", 1.0, "stereo spread without unison")

    # ---- filters
    def fmap(self, model, typ):
        """-> (Drumboy MODEL index, fidelity, note)"""
        if typ == 8:
            return 0, 0.15, "comb"
        if typ == 9:
            return 0, 0.15, "formant"
        base = {0: "LP", 3: "BP", 4: "HP", 5: "NOTCH"}
        lp = typ in (0, 1, 2)
        if model == 0:
            if typ in base:
                return FM[base[typ]], 1.0, ""
            if lp:
                return 0, 1.0, ""
            return 0, 0.3, "allpass/morph -> LP"
        if model == 1:
            return (FM["LADDER"], 1.0 if lp else 0.3, "") if lp else (FM["LADDER"], 0.3, "ladder has no BP/HP")
        if model == 2:
            return (FM["SOVIET"], 0.5, "scream -> soviet") if lp else (FM[base.get(typ, "LP")], 0.5, "scream")
        if model == 3:
            return FM["ACID"], 1.0, ""
        if model == 4:
            return (FM["SOVIET"], 1.0, "") if lp else (FM[base.get(typ, "LP")], 0.6, "soviet BP/HP")
        if model == 5:
            if typ == 1:
                return FM["CLASSIC"], 0.85, "classic LP12 -> LP24"
            return (FM["CLASSIC"], 1.0, "") if lp else (FM[base.get(typ, "LP")], 0.6, "classic BP/HP")
        if model == 6:
            return FM[base.get(typ, "LP")], 0.5, "reed -> plain"
        return (FM[base.get(typ, "LP")], 0.9, "state") if typ in base or lp else (0, 0.3, "state allpass/morph -> LP")

    def build_filter(self):
        v = self.v
        m, fid, note = self.fmap(v["filt.model"], v["filt.type"])
        self.setp("FLT1", 0, m)
        self.add("filter", "exact" if fid >= .99 else ("approx" if fid >= .3 else "dropped"), 8.0 * (1 - fid),
                 f"{REG.labels('filt.model')[v['filt.model']]}/{REG.labels('filt.type')[v['filt.type']]} -> {self.pg[PG['FLT1']][0]}" + (f" ({note})" if note else ""))
        hz = self.u("filt.cutoff")
        self.setp("FLT1", 1, hz_to_cut(hz))
        if hz < 28 or hz > 17000:
            self.add("cutoff-range", "approx", 0.5, f"{hz:.0f} Hz clipped")
        q = self.u("filt.reso")
        self.setp("FLT1", 2, q_to_reso(q))
        self.setp("FLT1", 3, v["filt.drive"])
        self.setp("FLT1", 4, v["filt.keytrack"])
        hp = v["hpf"]
        self.setp("FLT1", 6, 0 if hp == 0 else hz_to_cut(8.0 * self.u("hpf")))
        # filter envelope: Env2 -> cutoff, hard-wire x env2.amt, plus Env2 matrix rows; 4 octaves at full depth
        d = self.u("filt.envAmt") / 100.0 * (self.u("env2.amt") / 100.0)
        self.env_d = d
        # filter 2
        if v["filt2.on"]:
            m2, f2, n2 = self.fmap(v["filt2.model"], v["filt2.type"])
            self.setp("FLT2", 0, m2)
            self.setp("FLT2", 5, 1)
            self.setp("FLT2", 1, hz_to_cut(self.u("filt2.cutoff")))
            self.setp("FLT2", 2, q_to_reso(self.u("filt2.reso")))
            self.setp("FLT2", 3, v["filt2.drive"])
            r = v["filt.route"]
            self.setp("FLT2", 4, 1 if r else 0)
            self.add("filter2", "exact" if f2 >= .99 else "approx", 3.0 * (1 - f2) + (0.8 if r == 2 else 0), n2 + (", stereo route -> parallel" if r == 2 else ""))
            bal = self.u("filt.balance")
            self.setp("FLT1", 7, max(0.0, bal) / 100.0 * 127.0)
            if bal < -5:
                self.add("filter-balance", "approx", 0.8, "F1-only balance not reachable")
        else:
            self.setp("FLT2", 5, 0)

    # ---- amp
    def build_amp(self):
        v = self.v
        ms = {k: self.u("amp." + k) for k in ("a", "d", "r")}
        for k, idx, nm in (("a", 0, "attack"), ("d", 1, "decay"), ("r", 3, "release")):
            self.setp("AMP", idx, ms_to_env(ms[k]))
            if ms[k] > 2600:
                fid = max(0.0, 1 - math.log(ms[k] / 2500.0) / math.log(4))
                self.add("amp-" + nm, "approx", 2.5 * (1 - fid), f"{ms[k]:.0f} ms clipped to 2.5 s")
        self.setp("AMP", 2, v["amp.s"])
        self.setp("AMP", 4, v["amp.vel"])
        g = 10 ** (self.u("level") / 20.0) if v["level"] > 0 else 0.0
        self.setp("AMP", 5, 100 * g)
        self.setp("AMP", 6, 64)
        self.amp_ms = ms
        for k in ("amp.h", "amp.draw", "amp.del"):
            if v.get(k, 0):
                self.add(k, "dropped", 1.0, "")
        # Env2 shape against the panel's fixed one: attack 1 ms, decay = 0.8 x AMP decay + 20 ms, no sustain, release = AMP release
        d = self.env_d
        mat_env = sum(m["depth"] / 63.0 for m in self.mat if m["src"] == 1 and m["dest"] == "synth.filt.cutoff")
        tot = d + mat_env
        self.env_total = tot
        if abs(tot) > 0.03:
            if tot < 0:
                self.add("filter-env", "dropped", 4.0, "negative envelope amount not reachable")
                tot = 0.0
            else:
                e_a, e_d, e_s = self.u("env2.a"), self.u("env2.d"), self.u("env2.s")
                fid = 1.0
                notes = []
                if e_a > 25:
                    fid *= 0.6
                    notes.append(f"attack {e_a:.0f} ms")
                if e_s > 12:
                    fid *= max(0.4, 1 - e_s / 140.0)
                    notes.append(f"sustain {e_s:.0f}%")
                amp_visible = v["amp.s"] < 105
                tie = 0.8 * ms["d"] + 20
                if not amp_visible and e_d > 1:
                    # amp decay is not audible with a full sustain: let it carry the filter envelope's decay
                    want = max(0.0, (e_d - 20.0) / 0.8)
                    self.setp("AMP", 1, ms_to_env(want))
                    tie = 0.8 * env_ms(self.pg[PG["AMP"]][1]) + 20
                ratio = max(e_d, 1) / max(tie, 1)
                if ratio > 2.0 or ratio < 0.5:
                    fid *= max(0.5, 1 - 0.25 * abs(math.log2(ratio)) / 1.5)
                    notes.append(f"decay {e_d:.0f} ms vs {tie:.0f} ms")
                self.add("filter-env", "exact" if fid > .93 else "approx", 4.0 * (1 - fid), ", ".join(notes))
                # amount: ENV AMT reaches 2.8 octaves, ENV>CUT adds 2 more
                fr = min(max(tot, 0.0), 1.2)
                if fr > 1.0:
                    self.add("filter-env", "approx", 0.5, "amount clipped")
                a = (2 ** (4 * min(fr, 0.7)) - 1) / 6.0
                a = min(a, 1.0)
                e = max(0.0, (fr - 0.7) / 0.5) if fr > 0.7 else 0.0
                self.setp("FLT1", 5, 127 * a)
                self.setp("MOD", 5, 127 * min(e, 1.0))
        else:
            self.setp("FLT1", 5, 0)

    # ---- grit
    def build_grit(self):
        v = self.v
        self.setp("GRIT", 0, self.u("conv.bits"))
        self.setp("GRIT", 1, v["conv.compand"])
        hz = self.u("conv.rate")
        self.setp("GRIT", 2, (hz / 1000.0 - 1.0) / 43.1 * 127.0)
        if hz < 1000:
            self.add("conv-rate", "approx", 0.8, f"{hz:.0f} Hz clipped to 1 kHz")
        self.setp("GRIT", 3, max(v["conv.drift"], 0))
        self.setp("GRIT", 4, v["conv.loopErr"])
        self.setp("GRIT", 7, v["grit.amount"])

    # ---- mod
    def build_mod(self):
        v = self.v
        pdmode = any(self.v[f"osc{n}.mode"] == 1 and v[f"osc{n}.level"] > 8 and v[f"osc{n}.on"] for n in (1, 2))
        tabmode = any(f"osc{n}" in self.ext for n in (1, 2))
        lfo_cands = []   # (score, src, dest-class, m_eff, row)
        used = set()
        wheel = at = vel_amp = 0.0
        key_add = 0.0
        env_pd = 0.0
        vel_fold = []
        for idx, m in enumerate(self.mat):
            src, dest, dep = m["src"], m["dest"], m["depth"]
            mm = dep / 63.0 if dep >= 0 else dep / 64.0
            imp = IMP.get(dest, 0.4)
            dfac = min(1.0, abs(dep) / 32.0)
            drifter = src in (16, 17, 18)
            if drifter:
                imp = 0.15
            label = f"{SRC_NAME[src]}>{dest.replace('synth.', '')}"
            if src in (3, 4, 5) and dest in ("synth.filt.cutoff", "synth.pitch", "synth.level", "synth.osc.pdAmount", "synth.osc.pos"):
                if dest == "synth.osc.pos" and not tabmode:
                    # no table on the device: a position sweep has no meaning on a basic shape
                    if self.v["osc1.mode"] == 0 or self.v["osc2.mode"] == 0:
                        self.add("mod:" + label, "dropped", ROW_B * imp * dfac, "wavetable not available")
                        continue
                    self.add("mod:" + label, "dropped", ROW_B * imp * dfac, "")
                    continue
                if dest == "synth.osc.pdAmount" and not pdmode:
                    self.add("mod:" + label, "dropped", ROW_B * imp * dfac, "no PD oscillator")
                    continue
                depth_lfo = self.u(f"lfo{src - 2}.depth") / 100.0
                lfo_cands.append((abs(mm) * depth_lfo * imp, src, dest, abs(mm) * depth_lfo, m, label, imp, dfac))
                continue
            if src == 1 and dest == "synth.filt.cutoff":
                continue   # folded into the filter envelope amount
            if src == 1 and dest in ("synth.osc.pdAmount", "synth.osc.pos") and ((dest == "synth.osc.pdAmount" and pdmode) or (dest == "synth.osc.pos" and tabmode)):
                env_pd = max(env_pd, abs(mm))
                self.add("mod:" + label, "exact", 0.0, "ENV>PD" if dest.endswith("pdAmount") else "ENV>PD acts on table position")
                continue
            if src == 10 and dest == "synth.filt.cutoff":
                oct_ = 4 * abs(mm)
                wheel = max(wheel, min(1.0, oct_ / 2.0))
                self.add("mod:" + label, "exact" if oct_ <= 2.05 else "approx", ROW_B * imp * dfac * (0 if oct_ <= 2.05 else 0.4), "")
                continue
            if src == 12 and dest == "synth.filt.cutoff":
                oct_ = 4 * abs(mm)
                at = max(at, min(1.0, oct_ / 2.0))
                self.add("mod:" + label, "exact" if oct_ <= 2.05 else "approx", ROW_B * imp * dfac * (0 if oct_ <= 2.05 else 0.4), "")
                continue
            if src == 9 and dest == "synth.filt.cutoff":
                key_add += abs(mm)
                self.add("mod:" + label, "exact", 0.0, "KEYTRK")
                continue
            if src == 8 and dest == "synth.level":
                vel_amp = max(vel_amp, abs(mm))
                self.add("mod:" + label, "exact", 0.0, "VEL SENS")
                continue
            if src == 8 and dest in VEL_FOLD:
                # the panel has no velocity routing to timbre: fold the effect at a nominal velocity (0.75) into the base value, so the
                # static timbre is what a normal hit would give; the dynamics (soft vs hard) are what is lost
                vel_fold.append((dest, mm * VEL_NOMINAL))
                self.add("mod:" + label, "approx", ROW_B * imp * dfac * 0.5, f"folded into the base at velocity {VEL_NOMINAL}")
                continue
            self.add("mod:" + label, "dropped", ROW_B * imp * dfac, "")
        for dest, d in vel_fold:
            self.fold(dest, d)
        if key_add:
            self.setp("FLT1", 4, self.pg[PG["FLT1"]][4] + 127 * key_add)
        if vel_amp:
            self.setp("AMP", 4, self.pg[PG["AMP"]][4] + 127 * vel_amp)
        self.setp("MOD", 4, 127 * min(1.0, env_pd))
        self.setp("MOD", 6, 127 * wheel)
        self.setp("MOD", 7, 127 * at)
        # the single LFO lane: the strongest LFO route wins, the others are dropped
        lfo_cands.sort(key=lambda c: -c[0])
        shape_fid = 1.0
        for k, (sc, src, dest, meff, m, label, imp, dfac) in enumerate(lfo_cands):
            if k > 0:
                self.add("mod:" + label, "dropped", ROW_B * imp * dfac, "second LFO route")
                continue
            reach = {"synth.filt.cutoff": 0.25, "synth.pitch": 0.3 * 0.0 + 0.3, "synth.level": 0.5, "synth.osc.pdAmount": 1.0, "synth.osc.pos": 1.0}[dest]
            if dest == "synth.pitch":
                st = 2 * meff + 10 * meff ** 3
                reach_st = 2 * 0.3 + 10 * 0.3 ** 3
                fid = min(1.0, reach_st / max(st, 1e-6)) ** 0.5
                knob = 127 * min(1.0, meff / 0.3)
            else:
                fid = min(1.0, reach / max(meff, 1e-6)) ** 0.5
                knob = 127 * min(1.0, meff / reach)
            dcls = {"synth.filt.cutoff": 0, "synth.pitch": 1, "synth.level": 2, "synth.osc.pdAmount": 3, "synth.osc.pos": 3}[dest]
            ln = src - 2
            sh = self.v[f"lfo{ln}.shape"]
            shape_map = {0: (0, 1.0), 1: (1, 1.0), 2: (2, 1.0), 3: (2, 0.8), 4: (3, 1.0), 5: (0, 0.2), 6: (0, 0.2)}
            ws, sf = shape_map[sh]
            fid *= sf
            self.setp("MOD", 0, hz_to_unit_lfo(self.u(f"lfo{ln}.rate")))
            self.setp("MOD", 1, knob)
            self.setp("MOD", 2, dcls)
            self.setp("MOD", 3, ws)
            self.add("mod:" + label, "exact" if fid > .95 else "approx", ROW_B * imp * dfac * (1 - fid), f"LFO lane (lfo{ln})")
        if not lfo_cands:
            self.setp("MOD", 1, 0)

    def fold(self, dest, d):
        """add a modulation amount d (fraction of the destination's full scale at the nominal velocity) to the base knob."""
        if dest == "synth.filt.cutoff":
            hz = self.u("filt.cutoff") * 2 ** max(-3.0, min(3.0, 4 * d))
            self.setp("FLT1", 1, hz_to_cut(hz))
        elif dest == "synth.osc.pos":
            for n in (1, 2):
                if f"osc{n}" in self.ext:
                    self.setp(f"OSC{n}", 1, self.pg[PG[f"OSC{n}"]][1] + 127 * d)
                    self.ext[f"osc{n}"]["pos"] = self.pg[PG[f"OSC{n}"]][1]
        elif dest == "synth.osc.pdAmount":
            for n in (1, 2):
                if self.v[f"osc{n}.mode"] == 1:
                    self.setp(f"OSC{n}", 2, self.pg[PG[f"OSC{n}"]][2] + 127 * d)
        else:
            page, i = VEL_FOLD[dest]
            self.setp(page, i, self.pg[PG[page]][i] + 127 * d)

    # ---- everything that has no counterpart
    def build_specials(self):
        v = self.v
        sp = [("fold.amount", "fold", 4.0, 60), ("reson.amount", "resonator", 4.0, 50), ("xmod.fm", "FM", 4.0, 50), ("punch.amount", "punch transient", 3.0, 60)]
        for tok, nm, w, full in sp:
            if v[tok] > 3:
                self.add(nm, "dropped", w * min(1.0, v[tok] / full), "")
        if v["glide"] > 0:
            self.add("glide", "dropped", 1.5, "")
        if v["drifter.depth1"] > 5:
            self.add("drifter", "approx", 0.5, "no organic drift")
        if v["voice.lfoPhaseSpread"] > 5:
            self.add("lfo-phase-spread", "dropped", 0.5, "")
        mono = v["voiceMode"] != 0
        self.mono = mono
        self.setp("GLOB", 0, 100)
        self.setp("GLOB", 1, v["bendRange"])
        # FX chain: dropped (the firmware has none); drive FX is the one with a near cousin (GRIT CLIP)
        fx_loss, fxl = 0.0, []
        for e in self.p.get("fx", []):
            tid, vals = e["tid"], e.get("vals", {})
            base = FX_BASE.get(tid, 3.0)
            s = 0.35 + 0.65 * vals["mix"] / 100.0 if "mix" in vals else 0.8
            loss = base * s
            if tid == "drive" and vals.get("drive", 0) > 20:
                self.setp("GRIT", 6, vals["drive"] * 0.5)
                loss *= 0.6
                self.add("fx:drive", "approx", min(loss, 5), "-> GRIT CLIP")
                continue
            fx_loss += loss
            fxl.append(tid)
        if fxl:
            self.add("fx:" + "+".join(fxl), "dropped", min(fx_loss, CAP["fx"]), "no effects on the device")

    def run(self):
        self.build_osc()
        self.build_filter()
        self.build_amp()
        self.build_grit()
        self.build_mod()
        self.build_specials()
        return self.result()

    def result(self):
        loss = sum(i["loss"] for i in self.items if not i["feat"].startswith("mod:"))
        loss += min(CAP["mod"], sum(i["loss"] for i in self.items if i["feat"].startswith("mod:")))
        return dict(name=self.p["name"], bank=self.p["_bank"], cat=self.p["category"], pages=self.pg, ext=self.ext, mono=self.mono,
                    items=self.items, loss=round(loss, 2), score=round(max(0.0, 100.0 - loss), 1),
                    dropped=sorted(set(self.dropped_tags)), approx=sorted(set(self.approx_tags)), tables=sorted(set(self.tables)))


def hz_to_unit_lfo(hz):
    return 127.0 * (hz - 0.2) / 12.0


# ------------------------------------------------------------------------------------------------------------ cost-aware pass
def degrade_steps(pg):
    """the cheapest-first list of things to bypass when a voice costs too much: (label, edit, loss)"""
    steps = []
    if pg[PG["SUB"]][7] > 0:
        steps.append(("unison width", lambda g: g[PG["SUB"]].__setitem__(7, 0), 1.0))
    if pg[PG["GRIT"]][1] > 0:
        steps.append(("companding", lambda g: g[PG["GRIT"]].__setitem__(1, 0), 0.6))
    if pg[PG["FLT1"]][3] > 0:
        steps.append(("filter drive", lambda g: g[PG["FLT1"]].__setitem__(3, 0), 1.5))
    if pg[PG["SUB"]][3] > 0:
        steps.append(("ring mod", lambda g: g[PG["SUB"]].__setitem__(3, 0), 2.5))
    if pg[PG["OSC2"]][7] > 0:
        steps.append(("osc sync", lambda g: g[PG["OSC2"]].__setitem__(7, 0), 2.0))
    if pg[PG["FLT2"]][5]:
        steps.append(("filter 2", lambda g: g[PG["FLT2"]].__setitem__(5, 0), 2.0))
    if pg[PG["FLT1"]][0] in (5, 7):
        steps.append(("heavy filter", lambda g: g[PG["FLT1"]].__setitem__(0, 4), 2.5))
    if pg[PG["SUB"]][4] > 1:
        steps.append(("unison", lambda g: (g[PG["SUB"]].__setitem__(4, 1), g[PG["SUB"]].__setitem__(7, 0)), 2.5))
    return steps


def convert_all(presets, brought=None, cost_aware=True):
    res = [Conv(p, brought).run() for p in presets]
    cs = costs([r["pages"] for r in res])
    for r, (c, ok, fit) in zip(res, cs):
        r["cost"], r["heavy_ok"], r["fit_voices"] = c, ok, fit
        r["cost_degrades"] = []
    if cost_aware:
        for _ in range(8):
            todo = [r for r in res if r["cost"] > 5.0 / CAT_NEED.get(r["cat"], 2) + 1e-6 or not r["heavy_ok"]]
            if not todo:
                break
            for r in todo:
                if not r["heavy_ok"] and r["pages"][PG["FLT2"]][5]:
                    r["pages"][PG["FLT2"]][0] = 4
                    r["heavy_ok"] = True
                    r["items"].append(dict(feat="cost:filter2-light", status="approx", loss=1.0, note="second heavy filter -> ladder"))
                    r["loss"] += 1.0
                    continue
                st = degrade_steps(r["pages"])
                if st:
                    lab, fn, ls = st[0]
                    fn(r["pages"])
                    r["items"].append(dict(feat="cost:" + lab + " off", status="dropped", loss=ls, note="per-voice cost over budget"))
                    r["loss"] = round(r["loss"] + ls, 2)
                    r["cost_degrades"].append(lab)
                    r["dropped"] = sorted(set(r["dropped"]) | {"cost:" + lab})
            cs = costs([r["pages"] for r in todo])
            for r, (c, ok, fit) in zip(todo, cs):
                r["cost"], r["heavy_ok"], r["fit_voices"] = c, ok, fit
    for r in res:
        r["score"] = round(max(0.0, 100.0 - r["loss"]), 1)
        need = CAT_VOICES.get(r["cat"], 2)
        r["voices"] = 1 if r["mono"] else max(1, min(need, r["fit_voices"]))
    return res


# ------------------------------------------------------------------------------------------------------------ output
GEN_CATS = ["ALL", "BASS", "LEAD", "PAD", "PLUCK", "STAB", "POLY", "KEYS", "BRASS", "FX", "SEQ", "TEXTURE", "BELL", "PERC"]   # 0..5 = kPresetCats


def write_outputs(res, brought, outdir, report=None):
    slot = {t: k for k, t in enumerate(brought)}
    pres, rep = [], []
    for r in res:
        e = r["ext"]
        tab = [slot[e["osc%d" % n]["table"]] if "osc%d" % n in e and e["osc%d" % n]["table"] in slot else -1 for n in (1, 2)]
        pos = [e["osc%d" % n]["pos"] if "osc%d" % n in e else 0 for n in (1, 2)]
        pres.append(dict(name=r["name"][:23], category=r["cat"], bank=r["bank"], score=r["score"], cost=r["cost"], voices=r["voices"], table=tab, tablePos=pos,
                         pages=r["pages"], dropped=r["dropped"], approximated=r["approx"], tables=[slot[t] for t in r["tables"] if t in slot]))
        rep.append(dict(name=r["name"], category=r["cat"], score=r["score"], loss=r["loss"], cost=r["cost"], items=r["items"], costDegrades=r["cost_degrades"]))
    os.makedirs(outdir, exist_ok=True)
    json.dump(dict(format="drumboy-presets-1", pageOrder=list(PG), tables=[dict(slot=k, catalogue=t) for k, t in enumerate(brought)], presets=pres),
              open(os.path.join(outdir, "presets.json"), "w"), indent=1)
    json.dump(rep, open(report or os.path.join(REPO, "content", "out", "convert_report.json"), "w"), indent=1)
    L = ["// Generated by content/tools/convert_presets.py from the original CATHODE preset archive. Do not edit.",
         "// %d presets, %d wavetable slots. Include inside namespace app, after app/Params.h." % (len(pres), len(brought)),
         "//",
         "// The existing app::Preset holds only 4 + 5 values; a converted preset sets all 72 knobs, so it is its own flat struct:",
         "//   pages   the 9 synth pages x 8 knobs in Part::pages order (OSC1 OSC2 SUB FLT1 FLT2 GRIT AMP MOD GLOB), already inside the knob ranges",
         "//           of today's Params.cpp (a table oscillator keeps WAVE at the nearest SIN/TRI/SAW/SQR, so the table is optional).",
         "//   table   per oscillator, the wavetable slot (index into the SD set, content/data/wavetables/set.txt) or -1; its position is tablePos (0..127,",
         "//           the PD SHAPE value). The firmware change that uses it: WAVE >= 4 selects a table, PD SHAPE becomes POS (FIRMWARE_INTEGRATION.md).",
         "//   voices  the voice count to give the part (1: a mono source patch);  cost10: voice cost x 10 from the cost model;  score: fit 0..100.",
         "struct GenPreset {",
         "    const char* name;",
         "    uint8_t cat;        // index into kGenCats; 0..5 are kPresetCats",
         "    uint8_t voices;",
         "    uint8_t score;",
         "    uint8_t cost10;",
         "    int8_t table[2];    // -1: none",
         "    uint8_t tablePos[2];",
         "    int8_t pages[72];   // [page * 8 + knob]",
         "};",
         "static const char* const kGenCats[] = {" + ", ".join('"%s"' % c for c in GEN_CATS) + "};",
         "constexpr int kGenCatCount = %d;" % len(GEN_CATS),
         "constexpr int kGenTableSlots = %d;" % len(brought),
         "constexpr int kGenPresetCount = %d;" % len(pres),
         "static const GenPreset kGenPresets[kGenPresetCount] = {"]
    for p, r in zip(pres, res):
        nm = p["name"].replace("\\", "\\\\").replace('"', '\\"')
        flat = ",".join(str(x) for g in p["pages"] for x in g)
        L.append('    {"%s", %d, %d, %d, %d, {%d, %d}, {%d, %d}, {%s}},' % (nm, GEN_CATS.index(p["category"]), p["voices"], int(round(p["score"])),
                                                                        int(round(p["cost"] * 10)), p["table"][0], p["table"][1], p["tablePos"][0], p["tablePos"][1], flat))
    L.append("};")
    open(os.path.join(outdir, "Presets_generated.inc"), "w").write("\n".join(L) + "\n")
    return pres


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", default=os.path.join(REPO, "content", "out", "selection.json"))
    ap.add_argument("--key", default="chosen", help="selection.json list to convert: chosen | pool")
    ap.add_argument("--report", default=os.path.join(REPO, "content", "out", "convert_report.json"))
    ap.add_argument("--tables", default="auto", help="brought.json, or auto: the banks the converted presets actually use, most used first")
    ap.add_argument("--brought-out", default=os.path.join(REPO, "content", "data", "wavetables", "brought.json"))
    ap.add_argument("--out", default=os.path.join(REPO, "content", "data"))
    a = ap.parse_args()
    sel = json.load(open(a.set))
    allp = load_presets()
    byfile = {(p["_file"], p["name"]): p for p in allp}
    ps = [byfile[(c["file"], c["name"])] for c in sel[a.key]]
    if a.tables == "auto":
        use = collections.Counter()
        for r in convert_all(ps, brought=None, cost_aware=False):
            for k in ("osc1", "osc2"):
                if k in r["ext"]:
                    use[r["ext"][k]["table"]] += 1
        brought = sorted(use, key=lambda t: (-use[t], t))   # a table's position is its slot k: WAVE = 4 + k
        os.makedirs(os.path.dirname(a.brought_out), exist_ok=True)
        json.dump(brought, open(a.brought_out, "w"))
    else:
        brought = json.load(open(a.tables))
    res = convert_all(ps, brought=set(brought))
    pres = write_outputs(res, brought, a.out, a.report)
    s = [r["score"] for r in res]
    print("converted %d presets, score mean %.1f min %.1f, cost max %.2f, %d with cost degrades" % (len(res), sum(s) / len(s), min(s), max(r["cost"] for r in res),
                                                                                                    sum(1 for r in res if r["cost_degrades"])))


if __name__ == "__main__":
    main()
