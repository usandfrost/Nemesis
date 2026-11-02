#!/usr/bin/env python3
"""Writes the library presets as files (content/presets/*.PRS, format in tools/prs.py and firmware/src/app/PresetFile.h) from the preset cut
(content/data/cut.json, made by content/tools/cut_presets.py from the user's listening-page ratings) and their effects (content/data/fxmap.json).
Each preset: name, category, the recommended voices, the wavetable bank of OSC 1 and OSC 2 (a catalogue number, 255 = none: the oscillator keeps
its basic wave), the 72 knob values (9 pages x 8; the table position in place of PD SHAPE where an oscillator plays a bank) and the effects. The id is
10 + the position in the sorted cut, as the numbers were when the presets were compiled in, so projects keep pointing at the same sounds.
Usage: gen_presets.py   (copy content/presets to /CATHODE/PRESETS on the card: tools/make_sd_content.py does)
       gen_presets.py --more [--ids FIRST] [--out DIR]   the second round (content/data/more: the 97 candidates that were not on the first listening page) as files in
                              content/candidates (ids from 500, in category / fit-score / name order): the page that rates them is made from those files, and the
                              ones rated 3 or more are moved on by tools/cut_more.py"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prs
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
CATS = ["ALL", "BASS", "LEAD", "PAD", "PLUCK", "STAB", "POLY", "KEYS", "FX", "SEQ", "TEXTURE", "BELL"]   # = kPresetCats; the first six were the factory's

# the factory's first ten (app/Params.cpp kPresets): a converted preset of the same name gets a " 2" so the list has no two alike
BASE = ["Saw Stack", "Algo Knock", "Iron Squelch", "Sub Pressure", "Glass Wide", "Saw Choir", "Pluck Fifth", "Tape Warble", "Soft Sheen", "Dimension Bloom", "Bare Slab"]

def main():
    more = "--more" in sys.argv
    known = {"--more", "--ids", "--out"}
    for i, a_ in enumerate(sys.argv[1:], 1):
        if a_.startswith("--") and a_ not in known:
            raise SystemExit(f"gen_presets.py: unknown option {a_}")
        if a_ in ("--ids", "--out") and (i + 1 >= len(sys.argv) or sys.argv[i + 1].startswith("--")):
            raise SystemExit(f"gen_presets.py: {a_} needs a value")
    if not more and ("--ids" in sys.argv or "--out" in sys.argv):
        raise SystemExit("gen_presets.py: --ids and --out belong to --more")
    arg = lambda k, d: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
    data = f"{ROOT}/content/data/more" if more else f"{ROOT}/content/data"
    id0 = int(arg("--ids", 500)) if more else 10
    pre = json.load(open(f"{data}/presets.json"))
    slot2cat = {t["slot"]: t["catalogue"] for t in pre["tables"]}
    if more:
        cut = [dict(p, listenCategory=p["category"], category={"BRASS": "STAB"}.get(p["category"], p["category"])) for p in pre["presets"]]   # (the panel has no BRASS)
        cut = sorted(cut, key=lambda p: (CATS.index(p["category"]) if p["category"] in CATS else 99, -p["score"], p["name"]))
    else:
        cut = json.load(open(f"{data}/cut.json"))["presets"]
        cut = sorted(cut, key=lambda p: (CATS.index(p["category"]) if p["category"] in CATS else 99, -p["rating"], p["name"]))
    fxfile = f"{data}/fxmap.json"
    assert os.path.exists(fxfile), f"{fxfile} is missing (content/tools/map_fx.py makes it)"
    raw = json.load(open(fxfile))
    fxmap = {r["name"]: r for r in (raw["presets"] if isinstance(raw, dict) else raw)}
    missing = [p["name"] for p in cut if p["name"] not in fxmap]
    assert not missing, f"fxmap.json lacks {missing[:3]}"
    order = {"delay": ["mode", "time", "feedback", "tone", "spread", "mod", "hpf"],
             "reverb": ["size", "decay", "damp", "predelay", "width", "mod", "lowcut"],
             "chorus": ["rate", "depth", "delay", "feedback", "width", "tone", "hpf"]}
    ret = 127   # RETURN: 100% (the last knob of each effect page)
    def row(kind, obj):
        r = [int(obj[k]) for k in order[kind]] + [ret]
        assert len(r) == 8 and all(0 <= v <= 127 for v in r), (kind, obj)
        return r
    outdir = arg("--out", f"{ROOT}/content/candidates") if more else f"{ROOT}/content/presets"
    if more and os.path.realpath(outdir) in (os.path.realpath(f"{ROOT}/content/presets"), os.path.realpath(f"{ROOT}/content/synthwave"), os.path.realpath(f"{ROOT}/content/presets2")):
        raise SystemExit(f"gen_presets.py: --out {outdir} holds presets of their own (this deletes every .PRS in it)")
    taken = set(BASE)
    if more:   # a name the library already has gets a " 2" too
        taken |= {prs.decode(open(f"{ROOT}/content/presets/{f}", "rb").read())["name"] for f in os.listdir(f"{ROOT}/content/presets") if f.upper().endswith(".PRS")}
    voted_out = {} if more else prs.library_cuts()   # (the candidates are only for listening; tools/cut_more.py applies the cut to the second round)
    skipped = 0
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.upper().endswith(".PRS"): os.remove(os.path.join(outdir, f))
    for idx, p in enumerate(cut):
        if p["category"] not in CATS:
            raise SystemExit(f"{p['name']}: category {p['category']} is not a preset category")
        pages = [list(pg) for pg in p["pages"]]
        bank = [255, 255]
        for s_ in range(2):
            t = p["table"][s_]
            if t >= 0:
                bank[s_] = slot2cat[t]
                pages[s_][1] = int(p["tablePos"][s_])
        flat = [v for pg in pages for v in pg]
        assert len(flat) == 72 and all(-128 <= v <= 127 for v in flat), p["name"]
        name = p["name"] + " 2" if p["name"] in taken else p["name"]
        if id0 + idx in voted_out:   # voted out on the similarity page or skipped on the golden page: the id stays unused so the others keep theirs
            assert prs.renamed(voted_out[id0 + idx]) == prs.renamed(name), f"content/data (similarity_cuts.json / golden_votes.json): id {id0 + idx} is {name!r}, not {voted_out[id0 + idx]!r}"
            skipped += 1
            continue
        f = fxmap[p["name"]]
        a_ = f.get("amp") or {}
        amp = [int(a_.get("model", 0)), int(a_.get("drive", 0)), int(a_.get("bass", 64)), int(a_.get("mid", 64)), int(a_.get("treble", 64)),
               int(a_.get("presence", 50)), int(a_.get("cab", 2)), int(a_.get("level", 100))]
        has_amp = amp[0] != 0   # no amp: the firmware keeps the knobs at their defaults (Params.cpp applyPresetFx)
        assert amp[0] <= 4 and amp[6] <= 3, name   # the lists: MODEL 0-4, CAB 0-3
        sends = [int(v) for v in f["sends"]]
        assert len(sends) == 3 and all(0 <= v <= 127 for v in sends) and all(0 <= v <= 127 for v in amp), name
        assert f.get("delay") is None or int(f["delay"]["mode"]) <= 7, name   # delay MODE list: 0-7
        z = [0] * 8
        rec = dict(id=id0 + idx, name=name if more else prs.renamed(name), cat=CATS.index(p["category"]), voices=max(1, min(5, p["voices"])), banks=bank, v=flat, sends=sends,
                   mask=(1 if f.get("delay") else 0) | (2 if f.get("reverb") else 0) | (4 if f.get("chorus") else 0) | (8 if has_amp else 0),
                   amp=amp if has_amp else z, delay=row("delay", f["delay"]) if f.get("delay") else z,
                   reverb=row("reverb", f["reverb"]) if f.get("reverb") else z, chorus=row("chorus", f["chorus"]) if f.get("chorus") else z)
        if not more:
            rec = prs.trimmed(p["name"], rec)   # content/data/level_trims.json
        open(os.path.join(outdir, prs.file_name(rec)), "wb").write(prs.encode(rec))
    if more:   # which upstream preset each id is (the A/B render of the original, the listening page)
        sel = json.load(open(f"{ROOT}/content/out/selection_more.json"))["chosen"]
        files = {c["name"]: c["file"] for c in sel}
        assert len(files) == len(sel) == len({p["name"] for p in cut}), "preset names are not unique: the id map is keyed by name"
        idsfile = f"{data}/ids.json" if "--out" not in sys.argv and "--ids" not in sys.argv else os.path.join(outdir, "ids.json")   # (a scratch run never touches the tracked map)
        json.dump({str(id0 + i): dict(name=p["name"], file=files[p["name"]], category=p["category"], nemesisCategory=p["listenCategory"], score=p["score"])
                   for i, p in enumerate(cut)}, open(idsfile, "w"), indent=1)
    import collections
    c = collections.Counter(p["category"] for p in cut)
    print(len(cut) - skipped, "presets:", dict(c), f"({skipped} left out: voted out as too similar or skipped on the golden page)" if skipped else "")

main()
