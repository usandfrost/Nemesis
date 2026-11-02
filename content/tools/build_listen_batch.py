#!/usr/bin/env python3
"""build_listen_batch.py - a listening page for a batch of NEW presets (content/synthwave), in the look of the library page (build_listen2.py's template).

    build_listen_batch.py <wav dir> <stats.jsonl> <out.html> [title] [--hy <dir of NNN.wav> <up_stats.jsonl>]

The wav dir and stats come from render_listen run on the batch (DBOY_PRESET_SRC=<folder of the .PRS files>; see content/tools/run_batch.sh). Each preset has
WITH FX and DRY, and ORIGINAL (the upstream engine, dry) when --hy gives the renders of the originals (upstream_render, one NNN.wav per preset id). Ratings are kept under their own key in the browser and exported as JSON like the library page's."""
import concurrent.futures as cf, html as htmllib, importlib.util, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("bl2", os.path.join(HERE, "build_listen2.py"))
bl2 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bl2)


def main(wavdir, stats_path, out_html, title="Drumboy Synthwave Listening", hydir=None, hystats=None):
    stats = sorted((json.loads(l) for l in open(stats_path)), key=lambda r: r["i"])
    mp = os.path.join(os.path.dirname(os.path.abspath(wavdir)), "mp3")   # next to the renders, in the scratch folder
    os.makedirs(mp, exist_ok=True)
    hy = set()
    for l in (open(hystats) if hystats else []):
        r = json.loads(l)
        if "error" in r or not os.path.exists("%s/%03d.wav" % (hydir, r["i"])):
            sys.exit("build_listen_batch.py: the original of preset %s was not rendered (%s)" % (r.get("i"), r.get("error", "no wav")))
        hy.add(r["i"])
    jobs = []
    for r in stats:
        i = r["i"]
        jobs.append(("%s/fx_%03d.wav" % (wavdir, i), "%s/fx_%03d.mp3" % (mp, i), 2, bl2.KB["fx"]))
        jobs.append(("%s/dry_%03d.wav" % (wavdir, i), "%s/dry_%03d.mp3" % (mp, i), 1, bl2.KB["dry"]))
        if i in hy:
            jobs.append(("%s/%03d.wav" % (hydir, i), "%s/hy_%03d.mp3" % (mp, i), 1, bl2.KB["hy"]))
    with cf.ThreadPoolExecutor(8) as ex:
        info = {dst: (dur, tot) for dst, dur, tot in ex.map(bl2.enc, jobs)}
    items, audio, raw = [], [], 0
    for r in stats:
        i = r["i"]
        fxp, dp = "%s/fx_%03d.mp3" % (mp, i), "%s/dry_%03d.mp3" % (mp, i)
        hp = "%s/hy_%03d.mp3" % (mp, i)
        has_hy = i in hy
        raw += os.path.getsize(fxp) + os.path.getsize(dp) + (os.path.getsize(hp) if has_hy else 0)
        diff = r["diffDb"]
        items.append(dict(n=r["name"], c=r["cat"], i=i, v=r["voices"], va=r["voicesAsked"], b=[x for x in r["bankNames"] if x], sd=r["sends"],
                          amp=r["ampName"] if r["ampModel"] > 0 else "", dr=r["ampDrive"] if r["ampModel"] > 0 else 0, pr=0, was="", ph=r["phrase"], hy=1 if has_hy else 0,
                          fxd=None if diff < -100 else round(diff), tail=r["fx"]["tail60S"], tailDry=r["dry"]["tail60S"], miss=r["notesSilent"], nn=r["notes"],
                          dur=[round(info[fxp][0], 2), round(info[dp][0], 2)]))
        audio.append([bl2.uri(fxp), bl2.uri(dp), bl2.uri(hp) if has_hy else ""])
    withfx = [r for r in stats if r["diffDb"] > -100]
    summary = dict(
        renders=len(stats) * 2, finite=all(r["finiteDry"] and r["finiteFx"] for r in stats),
        clipped=sum(r["dry"]["clipped"] + r["fx"]["clipped"] for r in stats), silent=sum(r["dry"]["silent"] + r["fx"]["silent"] for r in stats),
        withEffects=len(withfx), unchanged=len(stats) - len(withfx),
        layerOver=dict((str(t), sum(1 for r in stats if r["diffDb"] > t)) for t in (-40, -26, -12, -6, 0)),
        voicesTrimmed=sum(1 for r in stats if r["voices"] < r["voicesAsked"]), droppedNotes=[r["name"] for r in stats if r["notesSilent"]],
        gainLimitedByFx=sum(1 for r in stats if r["gainLimitedByFx"]))
    order = [c for c in bl2.CATS if any(x["c"] == c for x in items)]
    order += sorted({x["c"] for x in items} - set(order))
    key = "dboy-" + re.sub(r"[^A-Za-z0-9_-]", "_", os.path.splitext(os.path.basename(out_html))[0])
    html = (bl2.TEMPLATE.replace("/*ITEMS*/", json.dumps(items, separators=(",", ":"))).replace("/*CATS*/", json.dumps(order))
            .replace("/*AUDIO*/", json.dumps(audio, separators=(",", ":"))).replace("/*SUMMARY*/", json.dumps(summary))
            .replace("__KBPS__", "%d / %d / %d" % (bl2.KB["fx"], bl2.KB["dry"], bl2.KB["hy"])).replace("__N__", str(len(items)))
            .replace("Drumboy Library Listening", htmllib.escape(title)).replace("dboy-listen-v2", key).replace("drumboy-listen-v2", key))
    html, n = re.subn(r"const prev = .*?;\n", 'const prev = "";\n', html, count=1, flags=re.S)   # the library page's "1st rating" chip: nothing to show for new presets
    assert n == 1, "build_listen2.py's template changed: the 'prev' chip line was not found"
    lede_old = html[html.index('<p class="lede">'):html.index("</p>", html.index('<p class="lede">')) + 4]
    lede_new = ('<p class="lede"><b>%d new presets</b>, rendered through the firmware\'s own code (the preset loader, the wavetable banks and the effects section), loaded the way BROWSE '
                'loads a preset. <b>WITH FX</b> is what you hear after loading it. <b>DRY</b> is the same sound with every send at 0 and the amp off, at the same gain, so the difference '
                'you hear is the effects alone.' + (' <b>ORIGINAL</b> is the original CATHODE playing the preset it was converted from, without its effects.' if hy else '') +
                ' Nothing here has run on the board yet. Rate 1-5 (3 = keep, 5 = a favourite); a note can name what to change, or the category it belongs in.</p>') % len(items)
    html = html.replace(lede_old, lede_new)
    open(out_html, "w").write(html)
    print("%d presets, mp3 %.2f MB, %s %.2f MB" % (len(items), raw / 1e6, out_html, os.path.getsize(out_html) / 1e6))
    print(json.dumps(summary))


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("wavdir"); ap.add_argument("stats"); ap.add_argument("out_html"); ap.add_argument("title", nargs="?", default="Drumboy Synthwave Listening")
    ap.add_argument("--hy", nargs=2, metavar=("DIR", "STATS"), help="the renders of the originals (NNN.wav) and upstream_render's stats")
    a = ap.parse_args()
    main(a.wavdir, a.stats, a.out_html, a.title, *(a.hy or []))
