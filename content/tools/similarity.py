#!/usr/bin/env python3
"""similarity.py - which presets sound alike? Clusters the library inside each category.

  similarity.py PRS_DIR RENDER_DIR OUT.json [--threshold T]

PRS_DIR     every preset to compare (.PRS files)
RENDER_DIR  the folder content/tools/run_batch.sh renders in (wav/dry_<id>.wav and stats.jsonl); the same listening phrase is played for every preset of a category
OUT.json    clusters, members, distances, ratings found in content/*/ratings.json

A preset is described by what it sounds like (70 %, from the dry render) and how it is built (30 %):
  sound : the long-term spectrum in 32 log-spaced bands (level removed), how noisy it is (spectral flatness), the loudness over time in 24 bins (percussive or sustained), and the brightness over time
  build : waves and banks, filter model and cutoff, envelope times, detune, unison, sync, LFO use, effect sends
Presets are only compared inside their category (the phrases differ between categories). Average-linkage clustering; --threshold is the distance at which two
groups still merge (default 0.25: the distance scale is set per category so that the typical pair is 0.5; at 0.20 only the closest cousins group, at 0.30 the groups get loose). Needs numpy and scipy. The script writes nothing but OUT.json.
"""
import json, os, sys

import numpy as np
from scipy.io import wavfile
from scipy.cluster.hierarchy import linkage, fcluster
from scipy.spatial.distance import squareform

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import prs  # noqa: E402

SR = 44100
BANDS = np.geomspace(40, 16000, 33)
NT = 24


def sound_features(path):
    sr, x = wavfile.read(path)
    x = x.astype(np.float64) / 32768.0
    if x.ndim > 1:
        x = x.mean(axis=1)
    win = 2048
    hop = 1024
    n = max(1, (len(x) - win) // hop)
    w = np.hanning(win)
    freqs = np.fft.rfftfreq(win, 1.0 / sr)
    frames = np.stack([x[i * hop:i * hop + win] * w for i in range(n)])
    spec = np.abs(np.fft.rfft(frames, axis=1)) ** 2
    rms = np.sqrt(np.mean(frames ** 2, axis=1) + 1e-12)
    active = rms > rms.max() * 0.03
    if active.sum() < 2:
        active[:] = True
    # long-term spectrum in log bands, in dB, level removed
    avg = spec[active].mean(axis=0)
    band = np.array([avg[(freqs >= lo) & (freqs < hi)].sum() + 1e-12 for lo, hi in zip(BANDS[:-1], BANDS[1:])])
    db = 10 * np.log10(band)
    db = db - db.mean()
    # loudness over time, on the active stretch, in dB below the peak
    idx = np.where(active)[0]
    seg = rms[idx[0]:idx[-1] + 1]
    tb = np.array([seg[int(i * len(seg) / NT):max(int((i + 1) * len(seg) / NT), int(i * len(seg) / NT) + 1)].mean() for i in range(NT)])
    env = 20 * np.log10(tb / tb.max() + 1e-4)
    env = np.maximum(env, -40.0)
    # brightness over time (spectral centroid, in octaves above 100 Hz)
    cen = (spec * freqs).sum(axis=1) / (spec.sum(axis=1) + 1e-12)
    cseg = cen[idx[0]:idx[-1] + 1]
    cb = np.array([cseg[int(i * len(cseg) / 8):max(int((i + 1) * len(cseg) / 8), int(i * len(cseg) / 8) + 1)].mean() for i in range(8)])
    cb = np.log2(np.maximum(cb, 100.0) / 100.0)
    # how noisy: the spectral flatness of the active frames (0 dB = white noise, strongly negative = a few clean partials)
    band = (freqs >= 100) & (freqs < 16000)
    sp = spec[active][:, band] + 1e-12
    flat = 10 * np.log10(np.exp(np.log(sp).mean(axis=1)) / sp.mean(axis=1))
    return db, env, cb, np.array([flat.mean()])


def build_features(p):
    """Numbers 0..1 that say how the patch is built: oscillators, sub and unison, filter, amp envelope, LFO, sends. An oscillator at level 0 counts as absent."""
    v = p["v"]
    page = lambda n: v[n * 8:(n + 1) * 8]
    o1, o2, sub, f1, f2, grit, amp, mod, glob = (page(i) for i in range(9))
    f = lambda a: a / 127.0
    out = []
    for o, bank in ((o1, p["banks"][0]), (o2, p["banks"][1])):
        on = 1.0 if o[6] > 0 else 0.0
        out += [on * f(o[6]), on * (min(o[0], 3) / 3.0), on * (1.0 if bank != 255 else 0.0), on * (o[3] + 2) / 4.0, on * (o[4] + 12) / 24.0, on * abs(o[5]) / 50.0]
    out += [f(o2[7]) * (1.0 if o2[6] > 0 else 0.0)]                                       # sync
    out += [f(sub[0]), f(sub[2]), f(sub[3]), (sub[4] - 1) / 3.0, f(sub[5]) * (sub[4] > 1)]   # sub, noise, ring, unison
    fam = {0: 0, 6: 0, 7: 0, 4: 1, 5: 1, 1: 2, 2: 3, 3: 4}[min(f1[0], 7)]                 # LP-like (LP, SOVIET, CLASSIC), ladder-like (LADDER, ACID), BP, HP, NOTCH
    out += [1.0 if fam == k else 0.0 for k in range(5)]
    out += [f(f1[1]), f(f1[2]), f(f1[3]), f(f1[4]), f(f1[5]), f(f1[6])]                    # cutoff, reso, drive, key tracking, env amount, HPF
    out += [f(grit[6]), (16 - grit[0]) / 15.0]
    out += [f(amp[0]), f(amp[1]), f(amp[2]), f(amp[3])]                                  # amp envelope
    out += [f(mod[1]), mod[2] / 3.0 * (mod[1] > 0), f(mod[0]) * (mod[1] > 0), mod[3] / 3.0 * (mod[1] > 0), f(mod[5]), f(mod[4])]   # LFO depth, destination, rate, shape; env to cutoff and to PD
    out += [f(x) for x in p["sends"]]
    out += [p["voices"] / 5.0]
    return np.array(out, dtype=float)


SETS = [(0, 9, "factory"), (10, 140, "round 1"), (141, 299, "synthwave"), (300, 349, "techno"), (350, 399, "synth pop"), (400, 449, "trance extras"), (450, 499, "EBM"), (500, 599, "round 2"), (600, 649, "Röyksopp"), (650, 699, "Delerium"), (700, 749, "voice pads")]
WAVES = ["SIN", "TRI", "SAW", "SQR"]
MODELS = ["LP", "BP", "HP", "NOTCH", "LADDER", "ACID", "SOVIET", "CLASSIC"]


def set_of(i):
    for lo, hi, name in SETS:
        if lo <= i <= hi:
            return name
    return "saved"


def _ms(v):
    m = 2 + 2500 * (v / 127.0) ** 2.2
    return "%d ms" % round(m) if m < 1000 else "%.1f s" % (m / 1000)


def facts(p, bank_names):
    """A few plain facts about how a preset is built, for the page: oscillators, filter, amp envelope, voices, effects."""
    v = p["v"]
    page = lambda n: v[n * 8:(n + 1) * 8]
    o1, o2, sub, f1, f2, grit, amp, mod, glob = (page(i) for i in range(9))
    osc = []
    for o, b in ((o1, bank_names[0] if bank_names else ""), (o2, bank_names[1] if len(bank_names) > 1 else "")):
        if o[6] > 0:
            osc.append(b if b else WAVES[min(o[0], 3)])
    if sub[0] > 0:
        osc.append("sub")
    if sub[2] > 0:
        osc.append("noise")
    if sub[4] > 1:
        osc.append("unison %d" % sub[4])
    if o2[7] > 0 and o2[6] > 0:
        osc.append("sync")
    cutoff = 30 * (16000 / 30.0) ** (f1[1] / 127.0)
    flt = "%s %s Hz, reso %d%%" % (MODELS[min(f1[0], 7)], "%d" % round(cutoff), round(f1[2] / 1.27))
    env = "A %s, D %s, S %d%%, R %s" % (_ms(amp[0]), _ms(amp[1]), round(amp[2] / 1.27), _ms(amp[3]))
    fx = [n for n, x in zip(("delay", "reverb", "chorus"), p["sends"]) if x > 0]
    lfo = ["cut", "pitch", "amp", "wavetable"][min(mod[2], 3)] if mod[1] > 0 else ""
    return dict(osc=" + ".join(osc), flt=flt, env=env, voices=p["voices"], fx=", ".join(fx), lfo=lfo)


def ratings_by_name(root):
    """{lower-case preset name: rating} from every ratings.json under content/: the genre sets (a list), the first round (content/data) and the second (content/data/more) (a dict by name)."""
    got = {}
    files = [os.path.join(root, sub, "ratings.json") for sub in sorted(os.listdir(root))] + [os.path.join(root, "data", "more", "ratings.json")]
    for fn in files:
        if not os.path.isfile(fn):
            continue
        try:
            r = json.load(open(fn))["ratings"]
        except (ValueError, KeyError):
            continue
        items = r.items() if isinstance(r, dict) else ((x.get("name", ""), x) for x in r)
        for name, x in items:
            if isinstance(x, dict) and isinstance(x.get("rating"), int):
                got.setdefault(name.lower(), x["rating"])
    return got


def z(m):
    sd = m.std(axis=0)
    sd[sd < 1e-9] = 1.0
    return (m - m.mean(axis=0)) / sd


def main(a):
    thr = 0.25
    if "--threshold" in a:
        i = a.index("--threshold")
        thr = float(a[i + 1])
        del a[i:i + 2]
    if len(a) != 3:
        sys.exit(__doc__)
    prs_dir, rdir, out = a
    pres = {p["id"]: (f, p) for f, p in prs.load_dir(prs_dir)}
    rat = ratings_by_name(os.path.join(HERE, ".."))
    sounds = {}
    for i in pres:
        w = os.path.join(rdir, "wav", "dry_%03d.wav" % i)
        if os.path.isfile(w):
            sounds[i] = sound_features(w)
    stats = {}
    for l in open(os.path.join(rdir, "stats.jsonl")):
        if l.strip():
            d = json.loads(l)
            stats[d["i"]] = d
    cats = {}
    for i, (f, p) in pres.items():
        cats.setdefault(prs.CATS[p["cat"]], []).append(i)
    result = dict(threshold=thr, clusters=[], singles=[], presets={})
    for cat, ids in sorted(cats.items()):
        ids = sorted(ids)
        if len(ids) < 2:
            result["singles"] += [dict(id=i, cat=cat) for i in ids]
            continue
        S = np.array([sounds[i][0] for i in ids]); E = np.array([sounds[i][1] for i in ids]); C = np.array([sounds[i][2] for i in ids]); N = np.array([sounds[i][3] for i in ids])
        B = np.array([build_features(pres[i][1]) for i in ids])
        def dist(M):
            M = z(M)
            d = np.sqrt(((M[:, None, :] - M[None, :, :]) ** 2).mean(axis=2))
            return d
        dS, dE, dC, dN, dB = dist(S), dist(E), dist(C), dist(N), dist(B)
        sound = 0.45 * dS + 0.25 * dE + 0.12 * dC + 0.18 * dN
        D = 0.7 * sound + 0.3 * dB
        # scale to about 0..1 inside the category so one threshold means the same everywhere
        med = np.percentile(D[np.triu_indices(len(ids), 1)], 50)
        if len(ids) >= 6 and med > 1e-9:       # a small category keeps its raw distances
            D = D / med * 0.5
        np.fill_diagonal(D, 0)
        Z = linkage(squareform(D, checks=False), method="average")
        lab = fcluster(Z, t=thr, criterion="distance")
        groups = {}
        for k, g in enumerate(lab):
            groups.setdefault(g, []).append(k)
        for g, ks in groups.items():
            if len(ks) < 2:
                result["singles"].append(dict(id=ids[ks[0]], cat=cat))
                continue
            # the member closest to all the others leads the cluster
            ks = sorted(ks, key=lambda k: D[k, ks].mean())
            members = []
            for k in ks:
                i = ids[k]
                f, p = pres[i]
                members.append(dict(id=i, name=p["name"], cat=cat, set=set_of(i), facts=facts(p, [b for b in stats[i]["bankNames"]] if i in stats else []), rating=rat.get(p["name"].lower()),
                                    dist=round(float(D[k, ks[0]]), 3), file=f))
            avg = float(np.mean([D[a_, b_] for a_ in ks for b_ in ks if a_ < b_]))
            result["clusters"].append(dict(cat=cat, size=len(ks), spread=round(avg, 3), members=members))
    result["clusters"].sort(key=lambda c: (c["spread"], -c["size"]))
    for c in result["clusters"]:
        for m in c["members"]:
            result["presets"][str(m["id"])] = m
    json.dump(result, open(out, "w"), indent=1)
    n = sum(c["size"] for c in result["clusters"])
    print(f"{len(pres)} presets, {len(result['clusters'])} clusters holding {n}, {len(result['singles'])} on their own (threshold {thr})")


if __name__ == "__main__":
    main(sys.argv[1:])
