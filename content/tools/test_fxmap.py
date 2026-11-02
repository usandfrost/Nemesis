#!/usr/bin/env python3
"""Checks content/data/fxmap.json: every value inside the Drumboy knob ranges of firmware/src/app/FxParams.cpp, the generator's output
is current, spot values agree with the C++ numbers in tests/app/test_app.cpp, and prints the statistics. Exit code 1 on any failure.

  test_fxmap.py
"""
import collections
import json
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fxknobs  # noqa: E402
import map_fx  # noqa: E402
from fxknobs import fx_table, knob, real, stored, default_stored  # noqa: E402

REPO = fxknobs.REPO
FXMAP = os.path.join(REPO, "content", "data", "fxmap.json")
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)
        print("FAIL:", msg)
    return cond


T = fx_table()

# ---------------------------------------------------------------------------------------------------- 1. the knob table and the inversion
print("== knob table (parsed from FxParams.cpp) and the stored <-> real inversion")
check(knob(T, "DELAY", "MODE").items == ["FREE", "1/4", "1/8.", "1/8", "1/16.", "1/16", "1/8T", "1/16T"], "delay MODE list")
check(knob(T, "AMP", "MODEL").items == ["OFF", "CLEAN", "CRUNCH", "LEAD", "BASS"], "amp MODEL list")
check(knob(T, "AMP", "CAB").items == ["OFF", "1X12", "2X12", "4X12"], "amp CAB list")
check([len(T[v]) for v in ("DELAY", "REVERB", "CHORUS", "COMP", "AMP")] == [8] * 5 and len(T["SENDS"]) == 3, "8 knobs per view, 3 sends")
# the numbers tests/app/test_app.cpp pins: ends of ranges, formats, tempo-synced times
check(abs(real(knob(T, "DELAY", "TIME"), 127) - 1000.0) < 1e-6 and abs(real(knob(T, "COMP", "THRESH"), 0) + 60.0) < 1e-9, "ends of the ranges (test_app: 1000 ms, -60 dB)")
check(abs(real(knob(T, "REVERB", "DECAY"), 127) - 12.0) < 1e-9 and abs(real(knob(T, "DELAY", "TONE"), 127) - 12000.0) < 1e-6, "REVERB DECAY 127 = 12 s, DELAY TONE 127 = 12 kHz")
check(abs(real(knob(T, "AMP", "TREBLE"), 127) - 12.0) < 1e-9 and abs(real(knob(T, "AMP", "TREBLE"), default_stored(knob(T, "AMP", "TREBLE")))) < 12.0 / 127,
      "AMP TREBLE 127 = +12 dB, default ~0 dB")
src = open(fxknobs.FXPARAMS).read()
q = [float(x.strip()) for x in re.search(r"kQuarters\[kDelayModes\]\s*=\s*\{([^}]*)\}", src).group(1).replace("1.0 / 3.0", "0.3333333333333333").replace("1.0 / 6.0", "0.16666666666666666").split(",")]
for bpm, mode, ms in ((120, 3, 250.0), (120, 1, 500.0), (120, 2, 375.0), (120, 5, 125.0), (120, 7, 250.0 / 3), (120, 6, 500.0 / 3), (120, 4, 187.5)):
    check(abs(q[mode] * 60000.0 / bpm - ms) < 1e-6, "delayMs(mode %d, %d bpm) = %.2f (test_app)" % (mode, bpm, ms))
# every default is inside its range and maps back (the test_app tolerance: 6 % of an exponential default, 0.6 step for a linear one)
for v in ("DELAY", "REVERB", "CHORUS", "COMP", "AMP"):
    for k in T[v]:
        d = default_stored(k)
        ok = 0 <= d <= k.vmax
        if not k.is_list:
            tol = k.default * 0.06 if k.exp else (k.b - k.a) / 127.0 * 0.6 + 1e-9
            ok = ok and abs(real(k, d) - k.default) <= tol
        check(ok, "default of %s %s round-trips" % (v, k.name))
# stored(real(v)) == v for every code of every knob (the inversion is exact)
for v in ("DELAY", "REVERB", "CHORUS", "COMP", "AMP"):
    for k in T[v]:
        if k.is_list:
            continue
        check(all(stored(k, real(k, c)) == c for c in range(128)), "stored(real(c)) == c for %s %s" % (v, k.name))
# the upstream sync -> MODE map reproduces the upstream tempo-synced time exactly (1/4. has none and goes FREE)
for sync, beats in map_fx.DLY_SYNC_BEATS.items():
    mode = map_fx.SYNC_TO_MODE[sync]
    if sync == 1:
        check(mode == 0, "1/4. has no mode -> FREE")
    else:
        check(abs(q[mode] * 60000.0 / 120.0 - beats * 500.0) < 1e-6, "upstream sync %s -> MODE %s same time" % (map_fx.DLY_SYNC_NAME[sync], knob(T, "DELAY", "MODE").items[mode]))

# ---------------------------------------------------------------------------------------------------- 2. the file
print("== fxmap.json")
doc = json.load(open(FXMAP))
recs = doc["presets"]
cut = json.load(open(map_fx.CUT))["presets"]
check([r["name"] for r in recs] == [c["name"] for c in cut], "one record per cut preset, in cut order (%d / %d)" % (len(recs), len(cut)))
fresh = json.loads(json.dumps(map_fx.build()))
check(fresh == recs, "fxmap.json is what map_fx.py generates now (run map_fx.py if this fails)")

VIEW = {"delay": ("DELAY", ["mode", "time", "feedback", "tone", "spread", "mod", "hpf"], ["MODE", "TIME", "FEEDBACK", "TONE", "SPREAD", "MOD", "HPF"]),
        "reverb": ("REVERB", ["size", "decay", "damp", "predelay", "width", "mod", "lowcut"], ["SIZE", "DECAY", "DAMP", "PRE-DLY", "WIDTH", "MOD", "LOW CUT"]),
        "chorus": ("CHORUS", ["rate", "depth", "delay", "feedback", "width", "tone", "hpf"], ["RATE", "DEPTH", "DELAY", "FEEDBACK", "WIDTH", "TONE", "HPF"])}
AMPK = [("model", "MODEL"), ("drive", "DRIVE"), ("cab", "CAB"), ("bass", "BASS"), ("mid", "MID"), ("treble", "TREBLE"), ("presence", "PRESENCE"), ("level", "LEVEL")]


def in_range(view, name, val, where):
    k = knob(T, view, name)
    return check(isinstance(val, int) and not isinstance(val, bool) and 0 <= val <= k.vmax, "%s: %s %s = %r outside 0..%d" % (where, view, name, val, k.vmax))


for r in recs:
    w = r["name"]
    check(set(r) == {"name", "sends", "amp", "delay", "reverb", "chorus", "notes"}, "%s: record keys %s" % (w, sorted(r)))
    check(len(r["sends"]) == 3, w + ": three sends")
    for i, s in enumerate(r["sends"]):
        check(isinstance(s, int) and 0 <= s <= 127, "%s: send %d = %r outside 0..127" % (w, i, s))
    for i, key in enumerate(("delay", "reverb", "chorus")):
        obj = r[key]
        check((obj is None) == (r["sends"][i] == 0), "%s: %s object and send agree (send %d)" % (w, key, r["sends"][i]))
        if obj is not None:
            view, keys, names = VIEW[key]
            check(list(obj) == keys, "%s: %s keys" % (w, key))
            for kk, nn in zip(keys, names):
                in_range(view, nn, obj[kk], w)
    check(list(r["amp"]) == [a for a, _ in AMPK], w + ": amp keys")
    for kk, nn in AMPK:
        in_range("AMP", nn, r["amp"][kk], w)
    if r["amp"]["model"] == 0:
        dflt = {nn: default_stored(knob(T, "AMP", nn)) for _, nn in AMPK}
        check(all(r["amp"][kk] == dflt[nn] for kk, nn in AMPK if kk != "model"), w + ": amp off carries the Drumboy defaults")
    check(isinstance(r["notes"], list) and all(isinstance(n, str) for n in r["notes"]), w + ": notes")

# ---------------------------------------------------------------------------------------------------- 3. an independent re-derivation of the sends and the delay times
print("== spot checks against the original files")
orig = map_fx.load_originals()
cutmap = {c["name"]: c for c in cut}
send_checked = time_checked = 0
for r in recs:
    c = cutmap[r["name"]]
    p = orig[(c["bank"], c["name"][:23])]
    for e in p.get("fx", []):
        tid = e["tid"]
        if tid not in ("delay", "reverb", "chorus"):
            continue
        v = e.get("vals", {})
        mix = v.get("mix", map_fx.DEF[tid]["mix"]) / 127.0
        i = {"delay": 0, "reverb": 1, "chorus": 2}[tid]
        typ = v.get("type", 1 if tid == "reverb" else 0)
        g = map_fx.CAL.get(tid, {}).get(str(typ), 1.0)
        want = min(127, int(math.floor(mix / (1.0 - mix) * g * 127.0 + 0.5))) if mix < 0.999 else 127
        if r["sends"][i] != want:
            check(False, "%s: %s send %d, independent derivation %d" % (r["name"], tid, r["sends"][i], want))
        send_checked += 1
        if tid == "delay" and v.get("sync", 2) == 8:
            typ_d = v.get("type", 0)
            ms = 4000.0 ** (v.get("time", 48) / 127.0)
            lo, hi = {1: (20.0, 600.0), 2: (50.0, 1500.0)}.get(typ_d, (1.0, 4000.0))
            ms = min(max(ms, lo, 20.0), min(hi, 1000.0))
            got = real(knob(T, "DELAY", "TIME"), r["delay"]["time"])
            check(r["delay"]["mode"] == 0 and abs(math.log(got / ms)) < math.log(1000.0 / 20.0) / 127.0 * 0.51, "%s: FREE delay %.0f ms -> TIME %.0f ms" % (r["name"], ms, got))
            time_checked += 1
        if tid == "delay" and v.get("sync", 2) != 8:
            mode = knob(T, "DELAY", "MODE").items[r["delay"]["mode"]]
            want_mode = {0: "1/4", 1: "FREE", 2: "1/8", 3: "1/8.", 4: "1/8T", 5: "1/16", 6: "1/8.", 7: "1/16T"}[v["sync"]]
            check(mode == want_mode, "%s: sync %d -> MODE %s (wanted %s)" % (r["name"], v["sync"], mode, want_mode))
print("   %d sends and %d free delay times re-derived from the raw files" % (send_checked, time_checked))

# the amp: gamp / drive instance -> model
for r in recs:
    c = cutmap[r["name"]]
    p = orig[(c["bank"], c["name"][:23])]
    first = next((e for e in p.get("fx", []) if e["tid"] in ("drive", "gamp")), None)
    want = 0
    if first is not None:
        v = first.get("vals", {})
        if first["tid"] == "gamp":
            want = {0: 1, 1: 2, 2: 3, 3: 3, 4: 4, 5: 1}[v.get("model", 1)]
        else:
            t = v.get("type", 0)
            if t not in (3, 4):
                db = v.get("drive", 44) / 127.0 * 36.0
                want = 3 if (t in (1, 2) or db >= 20.0) else 2
    check(r["amp"]["model"] == want, "%s: amp model %d, wanted %d" % (r["name"], r["amp"]["model"], want))

# ---------------------------------------------------------------------------------------------------- 4. statistics
print("== original effect types over the %d presets (instances / presets using it)" % len(recs))
inst, pres, vkeys = collections.Counter(), collections.Counter(), collections.defaultdict(collections.Counter)
for r in recs:
    c = cutmap[r["name"]]
    p = orig[(c["bank"], c["name"][:23])]
    seen = set()
    for e in p.get("fx", []):
        inst[e["tid"]] += 1
        seen.add(e["tid"])
        for k in e.get("vals", {}):
            vkeys[e["tid"]][k] += 1
    for t in seen:
        pres[t] += 1
no_fx = sum(1 for r in recs if not orig[(cutmap[r["name"]]["bank"], r["name"][:23])].get("fx"))
for t in sorted(inst, key=lambda t: -pres[t]):
    print("   %-8s %3d instances in %3d presets   keys: %s" % (t, inst[t], pres[t], ", ".join("%s(%d)" % (k, n) for k, n in sorted(vkeys[t].items()))))
print("   (no effects at all: %d presets)" % no_fx)

print("== what the 131 records get")
print("   delay send > 0:  %3d (delay object %d)" % (sum(1 for r in recs if r["sends"][0]), sum(1 for r in recs if r["delay"])))
print("   reverb send > 0: %3d (reverb object %d)" % (sum(1 for r in recs if r["sends"][1]), sum(1 for r in recs if r["reverb"])))
print("   chorus send > 0: %3d (chorus object %d)" % (sum(1 for r in recs if r["sends"][2]), sum(1 for r in recs if r["chorus"])))
models = collections.Counter(r["amp"]["model"] for r in recs)
print("   amp model:       " + ", ".join("%s %d" % (knob(T, "AMP", "MODEL").items[m], models[m]) for m in sorted(models)))
print("   no shared effect and no amp: %d presets" % sum(1 for r in recs if not any(r["sends"]) and r["amp"]["model"] == 0))
BINS = [(1, 16), (17, 32), (33, 48), (49, 64), (65, 80), (81, 96), (97, 126), (127, 127)]
for i, nm in enumerate(("delay", "reverb", "chorus")):
    vals = [r["sends"][i] for r in recs if r["sends"][i]]
    print("   %s send distribution (0..127): %s   min %d median %d max %d" % (nm, "  ".join("%d-%d:%d" % (a, b, sum(1 for x in vals if a <= x <= b)) for a, b in BINS),
                                                                                min(vals), sorted(vals)[len(vals) // 2], max(vals)))
print("   delay MODE:  " + ", ".join("%s %d" % (knob(T, "DELAY", "MODE").items[m], n) for m, n in sorted(collections.Counter(r["delay"]["mode"] for r in recs if r["delay"]).items())))
print("   amp CAB:     " + ", ".join("%s %d" % (knob(T, "AMP", "CAB").items[m], n) for m, n in sorted(collections.Counter(r["amp"]["cab"] for r in recs if r["amp"]["model"]).items())))

print("== notes (what could not be mapped or was approximated), by kind")
kind = collections.Counter()
ex = {}
for r in recs:
    for n in r["notes"]:
        key = re.sub(r"[-+]?\d+(\.\d+)?", "#", n)
        kind[key] += 1
        ex.setdefault(key, n)
for key, n in sorted(kind.items(), key=lambda kv: -kv[1]):
    print("   %3dx  %s" % (n, ex[key]))
print("   presets with at least one note: %d" % sum(1 for r in recs if r["notes"]))
unm = collections.Counter()
for r in recs:
    for n in r["notes"]:
        m = re.match(r"(comp|phaser|vintage|eq|schain)\b.*not mapped", n)
        if m:
            unm[m.group(1)] += 1
print("   not mapped at all: " + ", ".join("%s %d" % (k, v) for k, v in sorted(unm.items(), key=lambda kv: -kv[1])))

print()
if fails:
    print("%d FAILURE(S)" % len(fails))
    sys.exit(1)
print("ALL CHECKS PASSED")
