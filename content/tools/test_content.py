#!/usr/bin/env python3
"""test_content.py - checks of the converter output. Run after build_content.py:  python3 content/tools/test_content.py
 1 unit laws: the panel's own curves (Params.cpp cutHz / envMs, PatchMap resoQ) round-trip through the converter's inverses
 2 every converted preset: all 72 values inside the firmware's knob ranges, heavy-filter rule, cost x voices inside the 5-voice budget
 3 the firmware's own sanitiser (app::sanitize) leaves every preset unchanged                       (render_presets check, C++)
 4 renders: finite, not silent, no full-scale samples, level within a sane window
 5 the check itself can fail: a planted out-of-range value and an over-budget voice are both reported (separate build, scratch)
"""
import json, math, os, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import convert_presets as C

REPO = C.REPO
S = C.SCRATCH
fails = []
n = 0


def ok(cond, msg):
    global n
    n += 1
    if not cond:
        fails.append(msg)


# 1 laws
for v in range(0, 128):
    ok(abs(C.hz_to_cut(30.0 * (16000.0 / 30.0) ** (v / 127.0)) - v) < 1e-6, "cutoff law %d" % v)
    ok(abs(C.ms_to_env(2.0 + (v / 127.0) ** 2.2 * 2500.0) - v) < 1e-6, "env law %d" % v)
    ok(abs(C.q_to_reso(0.707 + (v / 127.0) ** 1.5 * 15.3) - v) < 1e-6, "reso law %d" % v)
    ok(abs(C.hz_to_unit_lfo(0.2 + v / 127.0 * 12.0) - v) < 1e-6, "lfo law %d" % v)
# registry spot checks against the TSV (cutoff v76 = 700 Hz noon, level v100 = 0 dB, bipolar edges)
ok(abs(C.REG.unit("filt.cutoff", 64) - 700) < 40, "registry cutoff noon (v64, doc: 700 Hz)")
ok(abs(C.REG.unit("level", 100)) < 1e-9 and abs(C.REG.unit("level", 127) - 12) < 1e-9, "registry level")
ok(C.REG.unit("filt.envAmt", -64) == -100 and C.REG.unit("filt.envAmt", 63) == 100, "registry bipolar")
ok(C.REG.unit("glide", 0) == 0, "offAtZero")

# 2 pages
P = C.panel()
d = json.load(open(os.path.join(REPO, "content", "data", "presets.json")))
ok(len(d["presets"]) >= 160, "preset count")
names = set()
for p in d["presets"]:
    ok(p["name"] not in names, "duplicate name " + p["name"])
    names.add(p["name"])
    ok(len(p["pages"]) == 9 and all(len(g) == 8 for g in p["pages"]), "shape " + p["name"])
    for g in range(9):
        for i in range(8):
            ok(P.lo[g][i] <= p["pages"][g][i] <= P.hi[g][i], "%s %s[%d]=%d outside %d..%d" % (p["name"], P.pages[g]["key"], i, p["pages"][g][i], P.lo[g][i], P.hi[g][i]))
    c, heavy_ok, fit = C.costs([p["pages"]])[0]
    ok(heavy_ok, "heavy rule " + p["name"])
    ok(c * p["voices"] <= P.budget + 1e-4, "budget " + p["name"])
    ok(1 <= p["voices"] <= 5, "voices " + p["name"])
    ok(abs(c - p["cost"]) < 1e-3, "cost drift " + p["name"])
    ok(all(-1 <= t < len(d["tables"]) for t in p["table"]), "table slot " + p["name"])

# 3 C++ check
r = subprocess.run([os.path.join(S, "build", "render_presets"), "check"], capture_output=True, text=True)
ok(r.returncode == 0, "sanitiser check: " + r.stdout[-300:])

# 4 renders
stats = [json.loads(l) for l in open(os.path.join(S, "stats.jsonl"))]
ok(len(stats) == len(d["presets"]), "render count")
for s in stats:
    ok(s["finite"], "non-finite " + s["name"])
    ok(not s["silent"] and s["rmsActiveDb"] > -70, "silent " + s["name"])
    ok(s["clipped"] == 0, "full scale " + s["name"])
    ok(s["rmsActiveDb"] < 0, "level " + s["name"])

# 5 the check can fail
plant = os.path.join(S, "plant")
os.makedirs(plant, exist_ok=True)
inc = open(os.path.join(REPO, "content", "data", "Presets_generated.inc")).read()
lines = inc.split("\n")
k = next(i for i, l in enumerate(lines) if l.startswith('    {"'))
# preset 0: OSC1 PD AMT -100 is below its range (the sanitiser must change it and the check must say FAIL 0); unison 4 + ring + Classic + drive added on top
row = lines[k]
vals = row[row.index("{", row.index("{") + 1):]
flat = vals[vals.rindex("{") + 1: vals.rindex("}")].split(",")
flat[2] = "-100"   # OSC1 PD AMT below its range
flat[8 * 2 + 0] = "2"; flat[8 * 2 + 4] = "4"; flat[8 * 2 + 7] = "127"; flat[8 * 2 + 3] = "127"   # SUB: ring, unison 4, width
flat[8 * 3 + 0] = "7"; flat[8 * 3 + 3] = "127"   # FILTER 1 Classic + drive
row2 = row[:row.rindex("{") + 1] + ",".join(flat) + row[row.rindex("}"):]
lines[k] = row2
open(os.path.join(plant, "Presets_generated.inc"), "w").write("\n".join(lines))
subprocess.run(["cmake", "-S", C.HERE, "-B", os.path.join(S, "build_plant"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DDBOY_GEN_DIR=" + plant], capture_output=True, check=True)
subprocess.run(["cmake", "--build", os.path.join(S, "build_plant"), "--target", "render_presets"], capture_output=True, check=True)
r = subprocess.run([os.path.join(S, "build_plant", "render_presets"), "check"], capture_output=True, text=True)
ok(r.returncode == 1 and "FAIL 0 " in r.stdout, "planted failure not reported: " + r.stdout[:300])
print(r.stdout.strip().split("\n")[0])

print("%d checks, %d failures" % (n, len(fails)))
for f in fails[:20]:
    print("  FAIL", f)
sys.exit(1 if fails else 0)
