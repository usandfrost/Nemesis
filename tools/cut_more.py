#!/usr/bin/env python3
"""The second round's cut: the user's ratings of content/candidates (content/data/more/ratings.json, the export of the listening page) decide which of the 97
converted upstream presets join the library. A sound needs 3 or more (unrated = 0 = left out). Its `note` in the export, when it names one of the panel's categories
(BASS LEAD PAD PLUCK STAB POLY KEYS FX SEQ TEXTURE BELL), is the category the user says it belongs in and replaces the converter's; any other note is a comment.
A kept preset keeps its id (500-596, the number it had as a candidate) and its file name follows its final name and goes to content/presets2/. Writes content/data/more/cut.json too.

  cut_more.py [--out DIR]       (default content/presets2; the folder's .PRS files are replaced, so --out is for scratch folders)
"""
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
import prs
MIN_RATING = 3
CATS = prs.CATS[1:]


def main(argv):
    out = argv[argv.index("--out") + 1] if "--out" in argv else os.path.join(ROOT, "content/presets2")
    if os.path.realpath(out) in {os.path.realpath(os.path.join(ROOT, "content", d)) for d in ("presets", "synthwave", "candidates")}:
        raise SystemExit(f"cut_more.py: {out} holds presets of their own (this deletes every .PRS in it)")
    rat = {}
    for r in json.load(open(os.path.join(ROOT, "content/data/more/ratings.json")))["ratings"]:
        if r["name"] in rat:
            raise SystemExit(f"cut_more.py: {r['name']!r} is rated twice in ratings.json")
        rat[r["name"]] = r
    cand = prs.load_dir(os.path.join(ROOT, "content/candidates"))
    unknown = sorted(set(rat) - {p["name"] for _, p in cand})
    if unknown:
        raise SystemExit(f"cut_more.py: ratings for presets that are not candidates: {unknown[:5]}")
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.upper().endswith(".PRS"):
            os.remove(os.path.join(out, f))
    kept, left, moved, comments = [], [], [], []
    voted_out = prs.library_cuts()
    for fname, p in cand:
        if p["id"] in voted_out:   # voted out on the similarity page or skipped on the Golden Presets page
            assert prs.renamed(voted_out[p["id"]]) == prs.renamed(p["name"]), f"content/data (similarity_cuts.json / golden_votes.json): id {p['id']} is {p['name']!r}, not {voted_out[p['id']]!r}"
            left.append((p["name"], "voted out (too similar, or skipped on the golden page)"))
            continue
        r = rat.get(p["name"])
        if r is None or r["rating"] < MIN_RATING:
            left.append((p["name"], "unrated" if r is None or r["rating"] == 0 else f"rated {r['rating']}"))
            continue
        note = (r.get("note") or "").strip()
        cat = p["cat"]
        if note.upper() in CATS:
            cat = prs.CATS.index(note.upper())
            if cat != p["cat"]:
                moved.append((p["name"], prs.CATS[p["cat"]], note.upper()))
        elif note:
            comments.append((p["name"], note))
        q = prs.fx_fixed(p["name"], prs.trimmed(p["name"], dict(p, cat=cat, name=prs.renamed(p["name"]))))   # content/data/renames.json, level_trims.json, fx_fixes.json
        open(os.path.join(out, prs.file_name(q)), "wb").write(prs.encode(q))   # (the file name follows the final name, see content/data/renames.json)
        kept.append(dict(id=p["id"], name=p["name"], rating=r["rating"], category=prs.CATS[cat], was=prs.CATS[p["cat"]], note=note))
    if out == os.path.join(ROOT, "content/presets2"):
        json.dump(dict(about="the second round's cut (tools/cut_more.py)", kept=kept, left=[dict(name=n, why=w) for n, w in left]), open(os.path.join(ROOT, "content/data/more/cut.json"), "w"), indent=1)
    import collections
    print(f"{len(kept)} of {len(cand)} kept (rated {MIN_RATING}+), {len(left)} left out; moved to another category: {len(moved)}")
    print("by category:", dict(collections.Counter(k["category"] for k in kept)))
    for n, a, b in moved:
        print(f"  moved {n}: {a} -> {b}")
    for n, c in comments:
        print(f"  note kept as a comment only: {n}: {c!r}")


if __name__ == "__main__":
    main(sys.argv)
