#!/usr/bin/env python3
"""export_library.py - the whole preset library in ONE JSON file, for someone who does not have the Drumboy firmware: every preset with every parameter, as the panel's
knob number AND as the panel shows it (so "CUTOFF 103" is also "4.9k Hz"), the effects, where the preset came from, and the documentation of what each knob drives in CATHODE.

    export_library.py [--panel-tool PATH] [OUT.json]       default OUT: content/out/sdcard/Drumboy_CATHODE_presets.json

Reads the presets from the folders tools/make_sd_content.py puts on the card (content/presets, presets2, synthwave, techno, synthpop, trance, ebm, royksopp, delerium, voice)
plus the factory presets in the firmware. The displayed values come from the firmware's own formatting code (content/tools/panel_tool.cpp `format` and `factory`), built with
    cmake -S content/tools -B <scratch>/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build <scratch>/build --target panel_tool
(DBOY_PANEL_TOOL or --panel-tool names the binary). Nothing is written to the repository except the output file."""
import datetime, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import prs                                                                    # noqa: E402
import genre_presets as gp                                                    # noqa: E402

FOLDERS = ("presets", "presets2", "synthwave", "techno", "synthpop", "trance", "ebm", "royksopp", "delerium", "voice")
SETS = [(0, 9, "factory", "built into the firmware"), (10, 140, "round 1", "converted from CATHODE presets"), (141, 299, "synthwave", "written for the Drumboy"),
        (300, 349, "techno", "written for the Drumboy"), (350, 399, "synth pop", "written for the Drumboy"), (400, 449, "trance", "written for the Drumboy"),
        (450, 499, "EBM", "written for the Drumboy"), (500, 599, "round 2", "converted from CATHODE presets"), (600, 649, "Roeyksopp style", "written for the Drumboy"),
        (650, 699, "Delerium style", "written for the Drumboy"), (700, 749, "voice pads", "written for the Drumboy")]
FX_VIEWS = ["delay", "reverb", "chorus", "comp", "amp"]


def set_of(i):
    for a, b, n, how in SETS:
        if a <= i <= b:
            return n, how
    return "other", ""


def ratings():
    """{name: rating}: the user's 1-5 on the listening pages (by the name the preset has now)."""
    out = {}
    def put(name, r):
        if isinstance(r, int) and 1 <= r <= 5:
            out[name] = out[prs.renamed(name)] = r
    for dirpath, _, files in os.walk(os.path.join(ROOT, "content")):
        if "ratings.json" in files:
            rs = json.load(open(os.path.join(dirpath, "ratings.json")))["ratings"]
            for n, v in (rs.items() if isinstance(rs, dict) else [(x["name"], x) for x in rs]):
                put(n, v.get("rating"))
    return out


def origins():
    """{id: dict} for the converted presets: the upstream preset each was made from."""
    out = {}
    cut = json.load(open(os.path.join(ROOT, "content/data/cut.json")))["presets"]
    chosen = {c["name"]: c for c in json.load(open(os.path.join(ROOT, "content/out/selection.json")))["chosen"]}
    for p in cut:   # round 1 (ids 10-140): matched by name (gen_presets.py numbers them in its own order); a " 2" is added to a name the library already had
        c = chosen.get(p["name"], {})
        out[("name", prs.renamed(p["name"]))] = dict(nemesisName=p["name"], nemesisFile=c.get("file"), nemesisBank=p.get("bank"), nemesisCategory=p.get("listenCategory"))
    for i, p in json.load(open(os.path.join(ROOT, "content/data/more/ids.json"))).items():
        out[int(i)] = dict(nemesisName=p["name"], nemesisFile=p.get("file"), nemesisCategory=p.get("nemesisCategory"))
    return out


class Panel:
    def __init__(self, exe):
        self.exe = exe

    def run(self, mode, lines):
        r = subprocess.run([self.exe, mode], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True)
        return r.stdout.strip().split("\n")

    def format(self, jobs):
        """jobs: [("P"|"F", index, [8 values])] -> [[8 texts]]"""
        out = self.run("format", ["%s %d %s" % (k, i, " ".join(map(str, v))) for k, i, v in jobs])
        if len(out) != len(jobs) or any(o == "ERR" for o in out):
            sys.exit("export_library.py: panel_tool format failed")
        return [json.loads(o) for o in out]


def main(out_path, exe):
    panel = Panel(exe)
    ranges = json.loads(subprocess.run([exe, "ranges"], capture_output=True, text=True, check=True).stdout)
    pages_meta = {}
    for pg in ranges["pages"]:
        pages_meta[pg["key"]] = dict(title=pg["page"], knobs=[dict(name=k["name"], min=k["min"], max=k["max"], default=k["def"], **({"list": k["list"]} if "list" in k else {})) for k in pg["p"]])
    fx_meta = {}
    for name, view in (("DELAY", "delay"), ("REVERB", "reverb"), ("CHORUS", "chorus"), ("AMP", "amp")):
        fx_meta[name] = [dict(name=r[0], default=r[1], **({"listSize": r[2]} if r[2] else dict(realMin=r[3], realMax=r[4], curve="exponential" if r[5] else "linear"))) for r in gp.FX_TABLE[view]]
    cats = ranges["cats"]
    rec = []   # (id, name, category, voices, banks, v72, sends, fx rows or None, factory)
    for folder in FOLDERS:
        d = os.path.join(ROOT, "content", folder)
        for f in sorted(os.listdir(d)):
            if f.upper().endswith(".PRS"):
                rec.append(prs.decode(open(os.path.join(d, f), "rb").read()))
    rec.sort(key=lambda r: r["id"])
    fac = [list(map(int, l.split())) for l in panel.run("factory", [])]
    facnames = [("Iron Squelch", 1), ("Sub Pressure", 1), ("Dimension Bloom", 3)]
    facrec = [dict(id=i, name=n, cat=c, voices=0, banks=[255, 255], v=fac[i], sends=[0, 0, 0], mask=0, amp=[0] * 8, delay=[0] * 8, reverb=[0] * 8, chorus=[0] * 8) for i, (n, c) in enumerate(facnames)]
    allrec = facrec + rec
    # one batch through the firmware's formatter
    jobs = []
    for r in allrec:
        for g in range(9):
            jobs.append(("P", g, r["v"][g * 8:(g + 1) * 8]))
        for bit, key, view in ((1, "delay", 0), (2, "reverb", 1), (4, "chorus", 2), (8, "amp", 4)):
            if r["mask"] & bit:
                jobs.append(("F", view, r[key]))
    texts = panel.format(jobs)
    ti = iter(texts)
    org, rat = origins(), ratings()
    bank_names = gp.BANK_NAMES
    presets = []
    for r in allrec:
        sname, how = set_of(r["id"])
        pages = {}
        for g, key in enumerate(prs.PAGES):
            tx = next(ti)
            pages[key] = {k["name"]: dict(value=r["v"][g * 8 + i], text=tx[i]) for i, k in enumerate(pages_meta[key]["knobs"]) if k["name"] != "-"}
        fx = dict(sends={n: dict(value=r["sends"][i], percent=int(round(r["sends"][i] * 100 / 127))) for i, n in enumerate(("delay", "reverb", "chorus"))})
        for bit, key, label in ((1, "delay", "DELAY"), (2, "reverb", "REVERB"), (4, "chorus", "CHORUS"), (8, "amp", "AMP")):
            if r["mask"] & bit:
                tx = next(ti)
                fx[key] = {k["name"]: dict(value=r[key][i], text=tx[i]) for i, k in enumerate(fx_meta[label])}
            else:
                fx[key] = None
        banks = [dict(oscillator=o + 1, catalogueNumber=b, name=bank_names[b] if b < len(bank_names) else None) for o, b in enumerate(r["banks"]) if b != 255]
        p = dict(id=r["id"], name=r["name"], category=cats[r["cat"]], set=sname, origin=how, voices=r["voices"] or None, wavetableBanks=banks)
        o = org.get(r["id"]) if r["id"] >= 500 else org.get(("name", r["name"]))
        if o:
            p["upstream"] = o
        if r["id"] >= 10 and r["name"] in rat:   # (a factory preset can share a name with an upstream preset that was rated)
            p["listeningRating"] = rat[r["name"]]
        p["pages"] = pages
        p["effects"] = fx
        presets.append(p)
    assert next(ti, None) is None
    doc = open(os.path.join(ROOT, "docs/PATCH_MAP.md")).read()
    out = dict(
        about=("The Randomwaves Drumboy Pro CATHODE firmware's preset library: %d presets with every parameter. The Drumboy firmware is a fork of CATHODE (the upstream synth) with a 5-voice, "
               "9-page panel in front of it. Each preset is the panel's nine pages of eight knobs (OSC1 OSC2 SUB FLT1 FLT2 GRIT AMP MOD GLOB), the three effect sends, and, where the preset sets "
               "them, the settings of the shared DELAY, REVERB and CHORUS and its AMP insert. Every knob is given as the panel's number (`value`, 0..127 unless a list or a signed knob; see `knobs`) "
               "and as the panel shows it (`text`, from the firmware's own formatting code). `upstream` names the CATHODE preset a converted preset was made from; the others were written for "
               "the Drumboy. `listeningRating` is the author's 1-5 on the listening pages (3 or more was kept; every preset here was then kept again in a final keep/skip pass). Nothing here has "
               "run on a Drumboy yet: the sounds were judged from host renders through the firmware's own code. What each knob drives in CATHODE is in `documentation.patchMap`.") % len(presets),
        exported=datetime.date.today().isoformat(), source="github.com/usandfrost/Nemesis",
        counts=dict(presets=len(presets), factory=len(facrec), files=len(rec)),
        knobs=dict(pages=pages_meta, effects=fx_meta,
                   notes="`min`/`max`/`default` are the stored knob numbers. A list knob's value is its index in `list`. Effects knobs: `default` is the stored number 0..127, realMin..realMax the value at 0 and at 127."),
        presets=presets, documentation=dict(patchMap=doc))
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    json.dump(out, open(out_path, "w"), indent=1, ensure_ascii=False)
    print("%s: %d presets, %.2f MB" % (out_path, len(presets), os.path.getsize(out_path) / 1e6))


if __name__ == "__main__":
    args = sys.argv[1:]
    exe = os.environ.get("DBOY_PANEL_TOOL")
    if "--panel-tool" in args:
        i = args.index("--panel-tool"); exe = args[i + 1]; del args[i:i + 2]
    if not exe or not os.path.exists(exe):
        sys.exit(__doc__)
    main(args[0] if args else os.path.join(ROOT, "content/out/sdcard/Drumboy_CATHODE_presets.json"), exe)
