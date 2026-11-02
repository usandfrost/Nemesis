#!/usr/bin/env python3
"""The preset cut: the listening page's ratings (data/ratings.json, the user's export) decide what becomes a firmware preset.
A sound needs 3 or more to make the cut; its `note` in the export is the CATEGORY THE USER SAYS IT BELONGS TO (the listening page filed it by the
converter's guess), so it replaces the converter's category. Writes data/cut.json and prints the counts.
Usage: cut_presets.py"""
import json, os, collections
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data")
MIN_RATING = 3
CATS = ["BASS", "LEAD", "POLY", "PAD", "PLUCK", "STAB", "KEYS", "BRASS", "FX", "SEQ", "TEXTURE", "BELL"]

def main():
    r = json.load(open(f"{DATA}/ratings.json"))["ratings"]
    pre = json.load(open(f"{DATA}/presets.json"))
    cut, moved, dropped = [], [], []
    for p in pre["presets"]:
        e = r.get(p["name"])
        if e is None:
            dropped.append((p["name"], "unrated")); continue
        if e["rating"] < MIN_RATING:
            dropped.append((p["name"], f"rated {e['rating']}")); continue
        cat = (e.get("note") or "").strip().upper() or p["category"]
        if cat not in CATS:
            raise SystemExit(f"{p['name']}: unknown category note {e['note']!r}")
        if cat != p["category"]: moved.append((p["name"], p["category"], cat))
        q = dict(p)
        q["listenCategory"] = p["category"]
        q["category"] = cat
        q["rating"] = e["rating"]
        cut.append(q)
    json.dump({"format": "drumboy-preset-cut-1", "minRating": MIN_RATING, "presets": cut}, open(f"{DATA}/cut.json", "w"), indent=1)
    c = collections.Counter(p["category"] for p in cut)
    print(f"{len(cut)} presets make the cut ({MIN_RATING}+), {len(dropped)} do not, {len(moved)} moved to the category the user gave")
    for k in CATS: print(f"  {k:8s} {c[k]}")
    tab = [p for p in cut if any(t >= 0 for t in p["table"])]
    print(f"{len(tab)} of them use a wavetable ({len({t for p in tab for t in p['table'] if t >= 0})} banks); {len(cut) - len(tab)} need none")
    return cut, moved

if __name__ == "__main__":
    main()
