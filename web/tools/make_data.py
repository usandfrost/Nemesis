#!/usr/bin/env python3
"""make_data.py - the data the page loads, written to web/www/data:
     prs.bin    the preset library: for each file  u8 name length, name, u16 size, bytes  (little endian)
     info.json  per preset: tags (what it uses) and, if a grade file is given, how closely the lean voice matches the full engine
usage: make_data.py [--grades scores2.csv] [--census census.json]"""
import csv, json, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "web", "www", "data")
FOLDERS = ("presets", "presets2", "synthwave", "techno", "synthpop", "trance", "ebm", "royksopp", "delerium", "voice")
args = sys.argv[1:]
def opt(name):
    return args[args.index(name) + 1] if name in args else None
os.makedirs(OUT, exist_ok=True)
blob = bytearray(); n = 0
for f in FOLDERS:
    d = os.path.join(ROOT, "content", f)
    for name in sorted(os.listdir(d) if os.path.isdir(d) else []):
        if name.upper().endswith(".PRS") and not name.startswith("."):
            b = open(os.path.join(d, name), "rb").read(); nb = name.encode()
            blob += struct.pack("<B", len(nb)) + nb + struct.pack("<H", len(b)) + b; n += 1
open(os.path.join(OUT, "prs.bin"), "wb").write(blob)
print("prs.bin:", n, "files,", len(blob), "bytes")
info = {}
cen = opt("--census")
if cen:
    for p in json.load(open(cen))["presets"]:
        t = []
        t.append(p["f1"]["model"])
        if p["f1"]["drive"]: t.append("DRIVE")
        if p["f2"]["on"]: t.append("FILTER2")
        if p["unison"]["n"] > 1: t.append("UNISON x%d" % p["unison"]["n"])
        if p["unison"]["width_on"]: t.append("WIDTH")
        if p["o1"]["mode"].startswith("PD") or p["o2"]["mode"].startswith("PD"): t.append("PD")
        if "BANK" in p["o1"]["mode"] or "BANK" in p["o2"]["mode"]: t.append("WAVETABLE")
        if p["noise"]["on"]: t.append("NOISE")
        if p["sub"]["on"]: t.append("SUB")
        if p["ring"]["on"]: t.append("RING")
        if p["sync"]["on"]: t.append("SYNC")
        if p["grit"]["any_on"]: t.append("GRIT")
        if p["f1"]["hpf"]: t.append("HPF")
        info[str(p["id"])] = {"tags": t}
g = opt("--grades")
if g:
    for r in csv.DictReader(open(g)):
        pid, note = r["file"].replace(".f32", "").split("_")
        e = info.setdefault(pid, {"tags": []})
        e.setdefault("grade", {})[note] = r["verdict"]
        e.setdefault("spec", {})[note] = [float(r["level_db"]), float(r["spec_mean"]), float(r["spec_p95"])]
json.dump(info, open(os.path.join(OUT, "info.json"), "w"))
print("info.json:", len(info), "presets")
