#!/usr/bin/env python3
"""Prints the listening verdicts the web bench saved (web/feedback/verdicts.json).
   python3 web/tools/verdicts.py            open rejections and comments (not yet handled)
   python3 web/tools/verdicts.py --all      everything
   python3 web/tools/verdicts.py --done ID [ID...]   mark verdicts handled (after fixing)"""
import json, os, sys
FB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "feedback", "verdicts.json")
d = json.load(open(FB)) if os.path.exists(FB) else {}
a = sys.argv[1:]
if a[:1] == ["--done"]:
    for i in a[1:]:
        if i in d: d[i]["handled"] = True
    tmp = FB + ".tmp"; json.dump(d, open(tmp, "w"), indent=1, sort_keys=True); os.replace(tmp, FB); sys.exit(0)
rows = sorted(d.values(), key=lambda r: r["id"])
if "--all" not in a: rows = [r for r in rows if not r.get("handled") and (r["verdict"] == "rejected" or r["comment"].strip())]
for r in rows:
    print("#%04d %-28s %-8s engine=%-4s grade=%s mode=%s reg=%+d  %s%s" % (r["id"], r["name"], r["verdict"].upper(), r["engine"], r["grade"], r["mode"], r["register"], r["time"], "  (handled)" if r.get("handled") else ""))
    if r["reasons"]: print("      reasons: " + ", ".join(r["reasons"]))
    if r["comment"].strip(): print("      " + r["comment"].strip().replace("\n", "\n      "))
print("%d shown, %d approved, %d rejected in total" % (len(rows), sum(r["verdict"] == "approved" for r in d.values()), sum(r["verdict"] == "rejected" for r in d.values())))
