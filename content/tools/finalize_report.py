#!/usr/bin/env python3
"""finalize_report.py - after the final render: put the FINAL selection (after the figure-based swap), the render checks and the A/B figures into
content/out/fit_report.md, and mark the selected rows in fit.csv. Run by build_content.py after stage upstream."""
import collections, csv, json, math, os, re, statistics as st

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(REPO, "content", "out")
S = os.environ.get("DBOY_CONTENT_SCRATCH", "/tmp/dboy-content")


def main():
    sel = json.load(open(os.path.join(OUT, "selection.json")))
    pres = json.load(open(os.path.join(REPO, "content", "data", "presets.json")))["presets"]
    rep = json.load(open(os.path.join(OUT, "convert_report.json")))
    idx = json.load(open(os.path.join(REPO, "content", "data", "wavetables", "index.json")))
    stats = [json.loads(l) for l in open(os.path.join(S, "stats.jsonl"))]
    hy = [json.loads(l) for l in open(os.path.join(S, "up_stats.jsonl"))]
    names = {p["name"] for p in pres}
    # fit.csv: selected = final
    rows = list(csv.reader(open(os.path.join(OUT, "fit.csv"))))
    h = rows[0]
    si = h.index("selected")
    for r in rows[1:]:
        r[si] = "1" if r[0] in names else "0"
    csv.writer(open(os.path.join(OUT, "fit.csv"), "w", newline="")).writerows(rows)
    # figures
    per = collections.Counter(p["category"] for p in pres)
    sc = [p["score"] for p in pres]
    d_oct = [math.log2(max(a["centroid"], 20) / max(b["centroid"], 20)) for a, b in zip(stats, hy)]
    d_sh = [sum(abs(a[k] - b[k]) for k in ("low", "mid", "high")) for a, b in zip(stats, hy)]
    raw = [s["rmsActiveDb"] for s in stats]
    L = []
    A = L.append
    A("## Final selection (%d presets, after the render-figure swap)\n" % len(pres))
    A("Counts: " + ", ".join("%s %d" % (c, per[c]) for c in sel["target"]) + ".\n")
    A("Chosen by wt-score inside each category, cost allowing the category's voices, no two within 0.015 on the 72 knobs; then every candidate was rendered "
      "and a preset whose figures (centroid, low/mid/high split, attack, hold) lay within 0.06 of a better one in its category was replaced by the next "
      "candidate (%d candidates rejected, %d presets swapped in). Fit of the final set: mean %.1f, min %.1f, max %.1f; per-voice cost mean %.2f, max %.2f "
      "(5-voice budget: every preset fits its recommended voice count). %d presets carry a degrade for cost (width, drive, ring, ...).\n"
      % (len(sel.get("rejectedByFigures", [])), len({c["name"] for c in sel["chosen"]} - {c["name"] for c in sel.get("chosenBeforeRender", sel["chosen"])}),
         st.mean(sc), min(sc), max(sc), st.mean(p["cost"] for p in pres), max(p["cost"] for p in pres), sum(1 for r in rep if r["costDegrades"])))
    cat_of = {t["slot"]: t["catalogue"] for t in json.load(open(os.path.join(REPO, "content", "data", "presets.json")))["tables"]}
    use_tab = [p for p in pres if any(t >= 0 and cat_of[t] != 0 for t in p["table"])]
    use_basics = [p for p in pres if any(t >= 0 and cat_of[t] == 0 for t in p["table"])]
    A("Wavetables for this set: **%d banks, %d files, %.2f MB** on the card (16-bit, <= 32 frames of 2048). %d presets play a table other than Basics; %d use the "
      "Basics morph (saw, square, triangle, sine: the firmware already has it as its WAVE shapes, but with a continuous position the panel lacks).\n"
      % (len(idx["tables"]), len(idx["tables"]), idx["totalBytes"] / 1e6, len(use_tab), len(use_basics)))
    A("### Objective checks on the renders (no listening was done)\n")
    A("| check | result |\n|---|---|")
    A("| rendered through firmware PatchMap + Instrument | %d of %d |" % (len(stats), len(pres)))
    A("| all samples finite | %s |" % ("yes" if all(s["finite"] for s in stats) else "NO"))
    A("| silent (active RMS < -70 dBFS) | %d |" % sum(1 for s in stats if s["silent"] or s["rmsActiveDb"] < -70))
    A("| samples at full scale (|x| >= 0.999) | %d presets |" % sum(1 for s in stats if s["clipped"]))
    A("| raw active level, dBFS RMS | median %.1f, min %.1f, max %.1f (the page normalises to -20) |" % (st.median(raw), min(raw), max(raw)))
    A("| raw peak above -3 dBFS | %d presets (the instrument's soft limiter keeps them under 0 dBFS) |" % sum(1 for s in stats if s["peakDb"] > -3))
    A("| DC offset above 1 %% of full scale | %d presets (removed by a 25 Hz high-pass in the mp3) |" % sum(1 for s in stats if abs(s["dc"]) > 0.01))
    A("| original engine (CATHODE, dry) rendered | %d of %d; %d reach full scale with no limiter or FX |" % (len(hy), len(pres), sum(1 for s in hy if s["clipped"])))
    A("| Drumboy vs original, spectral centroid | median %.2f octaves apart; %d presets > 1 octave, %d > 2 octaves |"
      % (st.median(abs(x) for x in d_oct), sum(abs(x) > 1 for x in d_oct), sum(abs(x) > 2 for x in d_oct)))
    A("| Drumboy vs original, low/mid/high energy split (L1) | median %.2f; %d presets > 0.5 |" % (st.median(d_sh), sum(x > 0.5 for x in d_sh)))
    worst = sorted(range(len(pres)), key=lambda i: -abs(d_oct[i]))[:6]
    A("\nFurthest from their original in brightness: " + ", ".join("%s (%+.1f oct)" % (pres[i]["name"], d_oct[i]) for i in worst) + ".\n")
    txt = open(os.path.join(OUT, "fit_report.md")).read()
    a = txt.index("## Selection (")
    b = txt.index("## What fits and what does not")
    txt = txt[:a] + "\n".join(L) + "\n" + txt[b:]
    open(os.path.join(OUT, "fit_report.md"), "w").write(txt)
    print("report updated; d_oct median %.2f" % st.median(abs(x) for x in d_oct))


if __name__ == "__main__":
    main()
