#!/usr/bin/env python3
"""calibrate_levels.py - keep new presets as loud as the library's, per category.

render_listen reports `gainDb` for every preset: how much the render had to be amplified to reach the standard loudness (so a NEGATIVE number is a patch that is loud
raw). The library's median per category is the yardstick (content/data/level_medians.json, made from the 131 first-round presets).

  calibrate_levels.py medians STATS.jsonl        write content/data/level_medians.json from a render of the library
  calibrate_levels.py report STATS.jsonl         list the presets more than 4 dB from their category's median, trimmed voices, dropped notes, clipping, DC
  calibrate_levels.py apply BATCH.py STATS.jsonl lower AMP LEVEL of the patches of BATCH.py that are more than 4 dB too loud (a quiet one is only listed:
                                                 its level is already at the top, change its sound). The stats must come from a render of the file AS IT IS NOW:
                                                 apply refuses if the batch file is newer than the stats (it changes the file, so run it once per render).
                                                 A level written as anything but a plain number (LEVEL=pct(50)) is left alone and reported.
"""
import json, os, re, statistics, sys
HERE = os.path.dirname(os.path.abspath(__file__))
MED = os.path.join(HERE, "..", "data", "level_medians.json")
LIMIT = 4.0


def rows(path):
    return [json.loads(l) for l in open(path) if l.strip()]


def dev(r, med):
    return r["gainDb"] - med[r["cat"]] if r["cat"] in med else 0.0


def main(a):
    if len(a) < 3:
        sys.exit(__doc__)
    if a[1] == "medians":
        by = {}
        for r in rows(a[2]):
            by.setdefault(r["cat"], []).append(r["gainDb"])
        med = {c: round(statistics.median(v), 1) for c, v in sorted(by.items())}
        # the library's FX presets are drones normalised to the same loudness as anything else but made of very quiet noise: not a yardstick for a riser or a hit.
        # They get the median of everything else.
        med["FX"] = round(statistics.median([g for c, v in by.items() if c != "FX" for g in v]), 1)
        json.dump(med, open(MED, "w"), indent=1)
        print("wrote", MED)
        return
    med = json.load(open(MED))
    st = rows(a[-1])
    if a[1] == "report":
        n = 0
        for r in sorted(st, key=lambda r: r["i"]):
            d = dev(r, med)
            why = []
            if abs(d) > LIMIT: why.append(f"{d:+.1f} dB from the {r['cat']} median")
            if r["voices"] < r["voicesAsked"]: why.append(f"voices {r['voicesAsked']}->{r['voices']}")
            if r["notesSilent"]: why.append(f"{r['notesSilent']} notes dropped")
            if r["fx"]["clipped"] or r["dry"]["clipped"]: why.append("clipped")
            if r["dry"]["silent"] or r["fx"]["silent"]: why.append("silent")
            if abs(r["dry"]["dc"]) > 0.01: why.append(f"DC {r['dry']['dc']:.3f}")
            if why:
                n += 1
                print(f"{r['i']} {r['name']:22s}{r['cat']:8s} gain {r['gainDb']:6.1f}  " + "; ".join(why))
        print(f"{n} of {len(st)} to look at")
    elif a[1] == "apply":
        path = a[2]
        if os.path.getmtime(path) > os.path.getmtime(a[-1]):
            sys.exit(f"calibrate_levels.py: {path} was edited after {a[-1]} was rendered; render it again first (a second apply on old stats would lower the levels twice)")
        src = open(path).read()
        for r in st:
            d = dev(r, med)
            if d >= -LIMIT:
                continue
            m = re.search(r'patch\("%s"' % re.escape(r["name"]), src)
            if not m:
                continue
            nxt = src.find('\n    patch("', m.end())
            blk = src[m.start():nxt if nxt > 0 else len(src)]
            old_blk = blk
            hit = re.search(r"AMP=(dict|adsr)\(", blk)
            if not hit:
                continue
            o, depth = hit.end() - 1, 0
            for k in range(o, len(blk)):
                depth += blk[k] == "("
                depth -= blk[k] == ")"
                if depth == 0:
                    c = k
                    break
            amp = blk[o:c + 1]
            key = "LEVEL" if hit.group(1) == "dict" else "level"   # AMP=dict(... LEVEL=n) or AMP=adsr(..., level=n)
            mm = re.search(key + r"=(\d+)(?=\s*[,)])", amp)
            if re.search(key + r"=", amp) and not mm:
                print(f"{r['name']}: its AMP level is not a plain number: left alone ({d:+.1f} dB)")
                continue
            cur = int(mm.group(1)) if mm else (100 if key == "LEVEL" else 127)
            new = max(8, round(cur * 10 ** (d / 20)))
            amp2 = re.sub(key + r"=\d+", f"{key}={new}", amp) if mm else amp[:-1] + f", {key}={new})"
            blk = blk[:o] + amp2 + blk[c + 1:]
            src = src[:m.start()] + blk + src[m.start() + len(old_blk):]
            print(f"{r['name']}: AMP LEVEL {cur} -> {new} ({d:+.1f} dB)")
        open(path, "w").write(src)
        for r in st:
            if dev(r, med) > LIMIT:
                print(f"quiet: {r['name']} is {dev(r, med):+.1f} dB under the {r['cat']} median (AMP LEVEL cannot raise it enough: change its sound)")


main(sys.argv)
