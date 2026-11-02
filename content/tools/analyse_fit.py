#!/usr/bin/env python3
"""analyse_fit.py - Phase A: how well does each CATHODE preset fit the Drumboy panel, and which ~170 to bring.

Reads the 383 archived presets, converts every one with convert_presets.py in two variants (today's firmware: no wavetables; and
with the wavetable plan: every catalogue table available), scores them, picks the selection and writes
    content/out/fit.csv  content/out/fit_report.md  content/out/selection.json  content/data/wavetables/brought.json
"""
import collections, csv, json, os, statistics as st, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import convert_presets as C

OUT = os.path.join(C.REPO, "content", "out")
DATA = os.path.join(C.REPO, "content", "data")
# the first listening page (brief): 40 BASS, 20 LEAD, 20 POLY, 10 each of the rest
TARGET = {"BASS": 40, "LEAD": 20, "POLY": 20, "PAD": 10, "PLUCK": 10, "STAB": 10, "KEYS": 10, "BRASS": 10, "FX": 10, "SEQ": 10, "TEXTURE": 10, "BELL": 10}
DUP = 0.015   # mean normalised distance over the 72 knobs below which two presets are the same panel sound
WT_FRAMES = 32
POOL = 1.6    # candidates kept per category = POOL x target; the render figures pick the final set from them (refine_selection.py)


def vec(r, P):
    return [(r["pages"][g][i] - P.lo[g][i]) / max(1, P.hi[g][i] - P.lo[g][i]) for g in range(9) for i in range(8)]


def dist(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def feat_class(f):
    if f.startswith("mod:"):
        src, dest = f[4:].split(">", 1)
        return "mod " + src + " > " + dest
    if f.startswith("fx:"):
        return "fx"
    return f.split(":")[0] + (":" + f.split(":")[1] if f.startswith("osc") and ":" in f else "")


def main():
    P = C.panel()
    ps = C.load_presets()
    now = C.convert_all(ps, brought=set())
    wt = C.convert_all(ps, brought=None)
    cat = C.catalogue()
    n = len(ps)
    rows = []
    for p, a, b in zip(ps, now, wt):
        rows.append(dict(name=p["name"], source=p["_file"], cat=p["category"], genre=p.get("genre", ""), score_now=a["score"], score_wt=b["score"],
                         cost=b["cost"], voices=b["voices"], mono=int(b["mono"]), dropped=" ".join(b["dropped"]), approx=" ".join(b["approx"]),
                         tables=" ".join(map(str, b["tables"]))))
    # ---- selection: best score first within each category, cost must allow the category's voices, no near duplicates
    V = [vec(r, P) for r in wt]
    order = sorted(range(n), key=lambda i: -wt[i]["score"])
    chosen, per, pool = [], collections.Counter(), []
    pcount = collections.Counter()
    skipped_dup = skipped_cost = 0
    for i in order:
        c = wt[i]["cat"]
        if c not in TARGET or pcount[c] >= int(POOL * TARGET[c]):
            continue
        if wt[i]["fit_voices"] < C.CAT_NEED.get(c, 2):
            skipped_cost += 1
            continue
        if any(dist(V[i], V[j]) < DUP for j in pool):
            skipped_dup += 1
            continue
        pool.append(i)
        pcount[c] += 1
        if per[c] < TARGET[c]:
            chosen.append(i)
            per[c] += 1
    sel_tables = tables_of(chosen, wt, ps)
    pool_tables = tables_of(pool, wt, ps)
    # ---- write fit.csv
    os.makedirs(OUT, exist_ok=True)
    chosen_names = {wt[i]["name"] for i in chosen}
    with open(os.path.join(OUT, "fit.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "source", "category", "genre", "score_now", "score_wt", "cost_per_voice", "voices", "mono", "selected", "dropped", "approximated", "wavetables"])
        for r in sorted(rows, key=lambda r: (r["cat"], -r["score_wt"])):
            w.writerow([r["name"], r["source"], r["cat"], r["genre"], r["score_now"], r["score_wt"], r["cost"], r["voices"], r["mono"],
                        int(r["name"] in chosen_names), r["dropped"], r["approx"], r["tables"]])
    ent = lambda ix: [dict(index=i, name=wt[i]["name"], cat=wt[i]["cat"], file=ps[i]["_file"], score=wt[i]["score"]) for i in ix]
    json.dump(dict(target=TARGET, chosen=ent(chosen), pool=ent(pool), tables=sel_tables, poolTables=pool_tables),
              open(os.path.join(OUT, "selection.json"), "w"), indent=1)
    json.dump(sel_tables, open(os.path.join(DATA, "wavetables", "brought.json"), "w"))
    write_report(ps, now, wt, rows, chosen, sel_tables, cat, skipped_dup, skipped_cost)
    print("selected", len(chosen), dict(per), "tables", len(sel_tables), "skipped dup", skipped_dup, "cost", skipped_cost)


def tables_of(ix, wt, ps):
    cnt = collections.Counter(t for i in ix for t in wt[i]["tables"] if _audible_wt(ps[i], t))
    return sorted(cnt, key=lambda t: (-cnt[t], t))   # most used first: a table's position here is its slot k (WAVE = 4 + k)


def _audible_wt(p, t):
    v = p["vals"]
    for k in (1, 2):
        if v.get(f"osc{k}.mode", 0) == 0 and v.get(f"osc{k}.table", 0) == t and v.get(f"osc{k}.on", 1) and v.get(f"osc{k}.level", 64) >= 8:
            return True
    return False


def write_report(ps, now, wt, rows, chosen, sel_tables, cat, skipped_dup, skipped_cost):
    n = len(ps)
    L = []
    A = L.append
    cats = collections.Counter(r["cat"] for r in rows)
    A("# Fit of the original CATHODE presets to the Drumboy panel\n")
    A(f"Corpus: {n} presets (`ref/upstream/docs/dev/sound-design/archive/presets/*/`, the T-CAL-55 cut archive; the shipped `Resources/presets/` set is "
      f"not in the clone, and the brief's 403 is not found: 383 JSON files, one preset each). 8 banks: "
      + ", ".join(f"{b} {c}" for b, c in collections.Counter(p["_bank"] for p in ps).most_common()) + ".\n")
    A("## Rule\n")
    A("Every source parameter is converted to the panel's units through the registry laws (`cathode-registry.tsv`) and classified exact / approximated / "
      "dropped. Each carries a loss in points; **score = 100 - sum(loss)**, floor 0. Budgets: oscillator layers 30 (split by audible level, times "
      "1 - fidelity of the layer's mode), filter 8 + filter envelope 4, amp envelope clipping 2.5 each, mod-matrix rows 3.0 x importance(dest) x "
      "min(1, depth/32) when dropped (cap 28), FX chain 5-7 per reverb/delay/chorus scaled by mix (cap 22), fold/resonator/FM/punch 3-4 each, per-oscillator "
      "envelopes 2.5. Cost: the firmware's own `voiceCost` (via `panel_tool cost`); a preset whose voice is too dear for its category "
      "(BASS/LEAD 2 voices, chords 3) is degraded step by step (width, companding, drive, ring, sync, filter 2, heavy filter, unison) and the loss is charged. "
      "Two variants: **now** (WAVE is SIN/TRI/SAW/SQR only) and **wt** (a wavetable carries WT-mode oscillators; the table is on the device). "
      "Fidelity is 1.0 only where the panel has the same thing; the rest (PD types other than Saw, ALGO engines, Bitmask, Pulse width, "
      "Reed/Scream filters, comb/formant) take fixed fidelities in `convert_presets.py`.\n")
    A("## Distribution (score 100 = nothing lost)\n")
    A("| variant | mean | median | >=80 | >=70 | >=60 | <50 |")
    A("|---|---|---|---|---|---|---|")
    for nm, r in (("now", now), ("wt", wt)):
        s = [x["score"] for x in r]
        A(f"| {nm} | {st.mean(s):.1f} | {st.median(s):.1f} | {sum(x >= 80 for x in s)} | {sum(x >= 70 for x in s)} | {sum(x >= 60 for x in s)} | {sum(x < 50 for x in s)} |")
    A("\n| category | presets | mean (wt) | >=70 (wt) | best | cost/voice mean | mono |")
    A("|---|---|---|---|---|---|---|")
    for c, k in cats.most_common():
        s = [wt[i]["score"] for i in range(n) if wt[i]["cat"] == c]
        cs = [wt[i]["cost"] for i in range(n) if wt[i]["cat"] == c]
        mono = sum(wt[i]["mono"] for i in range(n) if wt[i]["cat"] == c)
        A(f"| {c} | {k} | {st.mean(s):.0f} | {sum(x >= 70 for x in s)} | {max(s):.0f} | {st.mean(cs):.2f} | {mono} |")
    A("\n## Why presets fit badly (wt variant: points lost summed over all 383, and how many presets are hit)\n")
    agg, hit = collections.Counter(), collections.Counter()
    for x in wt:
        seen = set()
        for it in x["items"]:
            k = feat_class(it["feat"])
            if it["loss"] > 0:
                agg[k] += it["loss"]
                if k not in seen:
                    hit[k] += 1
                    seen.add(k)
    A("| feature | total points | presets | note |")
    A("|---|---|---|---|")
    notes = {"fx": "no effects on the device (reverb, delay, chorus, comp, ...)", "mod Vel > filt.cutoff": "velocity to cutoff: folded into the base cutoff at velocity 0.75; the soft-to-hard change is lost (the panel has VEL SENS for amp only)",
             "step-mod": "", "osc:sub": "sub is a sine, one octave down", "osc:osc1": "PD types other than Saw, ALGO engines, Bitmask, Pulse width, tables not on the device",
             "osc:osc2": "same", "osc-env:osc2": "per-oscillator envelopes (layers with their own shape)", "osc-env:sub": "", "osc-env:osc1": "", "filter-env": "Env2 shape is fixed"}
    for k, v in agg.most_common(14):
        A(f"| {k} | {v:.0f} | {hit[k]} | {notes.get(k, '')} |")
    A("\n## Wavetables\n")
    byc = collections.Counter(b["category"] for b in cat)
    names = ["Basic", "Vocal", "Metallic", "Harmonic", "Chip", "Complex"]
    frames = sum(b["frames"] for b in cat)
    A(f"The catalogue has **{len(cat)} factory banks**, all generated by procedural kernels (no stored data in the upstream tree): "
      + ", ".join(f"{names[k]} {byc[k]}" for k in sorted(byc)) + f". Frames per bank: " + ", ".join(f"{k} frames x {v}" for k, v in sorted(collections.Counter(b['frames'] for b in cat).items())) + f"; each frame 2048 samples. "
      f"All {len(cat)} banks as 16-bit PCM: {frames * 2048 * 2 / 1e6:.1f} MB ({frames} frames). Resident in the engine each frame is expanded to a "
      "band-limited mip pyramid (11 levels x 2048 floats = 90 KB per frame, 2.9 MB per 32-frame bank), so residency, not storage, is the limit; "
      "`FIRMWARE_INTEGRATION.md` proposes decimated mips (about 4.1 k samples per frame instead of 22.5 k).\n")
    use = collections.Counter()
    nwt = 0
    for i, p in enumerate(ps):
        t = [t for t in range(1, 200) if _audible_wt(p, t)]
        if t:
            nwt += 1
        use.update(t)
    nb = sum(1 for p in ps if _audible_wt(p, 0))
    A(f"{nwt} of {n} presets run an audible oscillator on a table other than Basics (bank 0, which the firmware already has as its four WAVE shapes; {nb} presets use that one); "
      f"they draw on {len(use)} distinct banks (top: "
      + ", ".join(f"{t} {cat[t]['id'].replace('factory.', '')} x{c}" for t, c in use.most_common(6)) + ").\n")
    sel_use = collections.Counter()
    for i in chosen:
        for t in wt[i]["tables"]:
            if _audible_wt(ps[i], t):
                sel_use[t] += 1
    A(f"## Selection ({len(chosen)} presets)\n")
    per = collections.Counter(wt[i]["cat"] for i in chosen)
    A("Best wt-score first inside each category, per-voice cost must let the category play its voices, and no two chosen presets closer than "
      f"{DUP} (mean normalised distance over the 72 knobs; {skipped_dup} candidates were skipped as near duplicates, {skipped_cost} for cost). "
      "Counts: " + ", ".join(f"{c} {per[c]}/{TARGET[c]}" for c in TARGET) + ".\n")
    s = [wt[i]["score"] for i in chosen]
    A(f"Selected scores: mean {st.mean(s):.1f}, min {min(s):.1f}, max {max(s):.1f}; per-voice cost mean {st.mean(wt[i]['cost'] for i in chosen):.2f}, max {max(wt[i]['cost'] for i in chosen):.2f}. "
      f"They need {len(sel_tables)} wavetable banks ({len(sel_tables) * WT_FRAMES * 4096 / 1e6:.1f} MB at <= {WT_FRAMES} frames, 16-bit; banks of 16 frames are smaller).\n")
    A("## What fits and what does not\n")
    A(SUMMARY.strip() + "\n")
    open(os.path.join(OUT, "fit_report.md"), "w").write("\n".join(L))


SUMMARY = """
The panel's 72 knobs carry the core of a CATHODE patch faithfully: oscillator level/octave/semitone/fine, a PD oscillator with the Saw type, a
sine sub, noise, ring mod, unison (1-4) with detune, 8 filter models, filter 2, HPF, the 7 grit rows, the amp ADSR (up to 2.5 s) and the
Env2 filter sweep, one LFO route, wheel and aftertouch to cutoff. Bass and lead patches built on a PD or saw core with a Classic/Acid/Ladder
filter lose the least. What does not fit is the part the designers wrote for: the whole FX chain (reverb, delay, chorus: in almost every preset), velocity
to cutoff / position / level (the panel has only amp velocity), per-oscillator envelopes, second and third LFO routes, the step modulator, FM, fold,
the resonator, the punch transient, ALGO (Plaits) and Bitmask oscillators, PD types other than Saw, pulse width modulation and glide. Presets whose
character is a wavetable sweep need the tables and a position control. So POLY/PAD material fits worse than BASS because it leans on FX and
motion, and BELL, TEXTURE and STAB carry the most dropped features. Worth bringing: the banks the best-fitting presets actually use (the
`brought` list), every one a procedural bank, 16-bit, 16-32 frames: a few MB on the card and, with the decimated-mip plan, well under 1 MB resident
per table. Basic and Harmonic banks cover most WT oscillators, Vocal/Metallic/Chip/Complex banks supply the unmistakable textures; the
choice is by use in the selection, not by catalogue order. Quality still needs listening: these scores measure what is representable, not what
sounds good.
"""

if __name__ == "__main__":
    main()
