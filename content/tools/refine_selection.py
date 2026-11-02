#!/usr/bin/env python3
"""refine_selection.py - pick the final set from the candidate pool using the Drumboy RENDER figures, so two presets of a category that would sound the same are not both kept.

upstream's preset-pipeline asks for "no near duplicates". The pool (analyse_fit.py: 1.6 x the target per category, best fit first, already
distinct on the panel's 72 knobs) is rendered with the category's phrase; a candidate is kept only if its figures (spectral centroid, the
low/mid/high split, attack, how long it holds) differ from every preset already kept in the category by at least THRESH. Figures describe
the render, not taste; a pair that passes can still sound alike, and a pair that fails is usually the same patch with another name.
    refine_selection.py <pool_stats.jsonl> [--thresh 0.06]
"""
import argparse, collections, json, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(REPO, "content", "out")


def feat(r):
    return [math.log2(max(r["centroid"], 20)) / 3.0, r["low"], r["mid"], r["high"], math.log2(r["attackMs"] + 50) / 6.0, r["activeFrames"] / 90.0]


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stats")
    ap.add_argument("--thresh", type=float, default=0.06)
    a = ap.parse_args()
    sel = json.load(open(os.path.join(OUT, "selection.json")))
    stats = {}
    for line in open(a.stats):
        r = json.loads(line)
        stats[r["i"]] = r
    pool = sel["pool"]
    target = sel["target"]
    kept = collections.defaultdict(list)
    final, rejected = [], []
    for i, c in enumerate(pool):   # pool is in best-score-first order per the selection loop
        r = stats.get(i)
        cat = c["cat"]
        if r is None or not r["finite"] or r["silent"]:
            rejected.append((c["name"], "silent or non-finite"))
            continue
        if len(kept[cat]) >= target[cat]:
            continue
        f = feat(r)
        near = min(((dist(f, g), n) for g, n in kept[cat]), default=(9, ""))
        if near[0] < a.thresh:
            rejected.append((c["name"], "figures within %.3f of %s" % near))
            continue
        kept[cat].append((f, c["name"]))
        final.append(c)
    sel["chosenBeforeRender"] = sel["chosen"]
    sel["chosen"] = final
    sel["rejectedByFigures"] = rejected
    json.dump(sel, open(os.path.join(OUT, "selection.json"), "w"), indent=1)
    print("kept %d, rejected %d: " % (len(final), len(rejected)) + ", ".join("%s %d/%d" % (c, len(kept[c]), target[c]) for c in target))
    changed = {c["name"] for c in final} - {c["name"] for c in sel["chosenBeforeRender"]}
    print("swapped in %d presets" % len(changed))


if __name__ == "__main__":
    main()
