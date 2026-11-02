#!/usr/bin/env python3
"""Runs the wet-level render calibration and writes content/tools/fxcal/cal_result.json (read by map_fx.py).

  analyse_cal.py <build dir with fx_cal_hyb and fx_cal_dboy> [--work DIR]

1. map_fx.py --cases writes the (upstream params | Drumboy params) cases of every cut preset's delay / reverb / chorus,
2. both programs render the same 0.4 s noise burst through them (upstream effect at 100 % mix, Drumboy effect at send 1 / return 100 %,
   the reverb with its kReverbTrim 1.4) and print the wet energy,
3. the gain per original effect type is the MEDIAN over the presets of sqrt(E_nemesis / E_drumboy): the factor a send is multiplied by
   so that the Drumboy wet is as loud as the upstream wet at the same wet-to-dry ratio (clamped to 0.25 .. 2),
4. the reverb RT60 table (impulse, T20 extrapolated) checks the DECAY mapping.
"""
import argparse
import collections
import json
import math
import os
import statistics
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, ".."))
KIND = {"D": "delay", "R": "reverb", "C": "chorus"}
NAMES = {"D": ["Digital", "Analog", "Tape", "Ping-pong"], "R": ["Room", "Hall", "Plate", "Spring", "Shimmer", "Cathedral"],
         "C": ["Chorus", "Flanger", "Ensemble", "Vibrato"]}


def run(exe, cases, extra=()):
    out = subprocess.run([exe, cases, *extra], capture_output=True, text=True, check=True).stdout
    res = {}
    for line in out.splitlines():
        i, k, t, e = line.split()
        res[int(i)] = (k, int(t), float(e))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("--work", default=None)
    a = ap.parse_args()
    work = a.work or tempfile.mkdtemp(prefix="fxcal_")
    os.makedirs(work, exist_ok=True)
    cases = os.path.join(work, "cases.txt")
    subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "map_fx.py"), "--cases", cases, "--out", os.path.join(work, "fxmap_cal_run.json")],
                   check=True, capture_output=True)
    hyb = run(os.path.join(a.build, "fx_cal_hyb"), cases)
    dby = run(os.path.join(a.build, "fx_cal_dboy"), cases)
    by = collections.defaultdict(list)
    for i, (k, t, e) in hyb.items():
        if dby[i][2] > 0 and e > 0:
            by[(k, t)].append(math.sqrt(e / dby[i][2]))
    cal = {"delay": {}, "reverb": {}, "chorus": {}}
    print("wet level, sqrt(E_nemesis / E_drumboy) over the presets (send multiplier)")
    for (k, t), gs in sorted(by.items()):
        gs.sort()
        med = statistics.median(gs)
        cal[KIND[k]][str(t)] = round(min(2.0, max(0.25, med)), 3)
        print("  %-8s %-10s n=%2d  min %.3f  median %.3f (%+.1f dB)  max %.3f" % (KIND[k], NAMES[k][t], len(gs), gs[0], med, 20 * math.log10(med), gs[-1]))
    json.dump(cal, open(os.path.join(HERE, "cal_result.json"), "w"), indent=1)

    # RT60 table of the reverb mapping
    rc = os.path.join(work, "rcases.txt")
    with open(rc, "w") as f:
        f.writelines(l for l in open(cases) if l.startswith("R "))
    hr = run(os.path.join(a.build, "fx_cal_hyb"), rc, ["rt"])
    dr = run(os.path.join(a.build, "fx_cal_dboy"), rc, ["rt"])
    rows = {int(l.split()[1]): l.split() for l in open(rc)}
    print("reverb RT60 (T20 extrapolated, impulse): upstream at its nominal decay, Drumboy at the mapped DECAY")
    t_by = collections.defaultdict(list)
    for i, c in rows.items():
        t_by[int(c[2])].append((float(c[3]) / 1000.0, hr[i][2], float(c[9]), dr[i][2]))
    for t, rs in sorted(t_by.items()):
        h = statistics.median(r[1] / r[0] for r in rs)
        d = statistics.median(r[3] / r[2] for r in rs)
        print("  %-10s n=%2d  upstream RT60 / nominal %.2f   Drumboy RT60 / its DECAY %.2f   median upstream %.2f s vs Drumboy %.2f s" % (
            NAMES["R"][t], len(rs), h, d, statistics.median(r[1] for r in rs), statistics.median(r[3] for r in rs)))
    print("wrote", os.path.join(HERE, "cal_result.json"))


if __name__ == "__main__":
    main()
