#!/usr/bin/env python3
"""build_content.py - the whole content pipeline, start to finish (about two minutes on a laptop).

    1 analyse     analyse_fit.py: fit scores of all upstream presets, candidate pool (1.6 x target per category)
    2 pool        convert + export wavetables + render the whole pool through the Drumboy instrument
    3 refine      refine_selection.py: drop candidates whose render figures duplicate a better one in their category
    4 final       convert the final set, export its wavetables, write presets.json / Presets_generated.inc, run the checks, render
    5 upstream      render the same phrases through the original CATHODE machine (dry: no FX) for the A/B
    6 report      finalize_report.py: final selection, render checks and A/B figures into fit_report.md
    7 listen      build_listen.py: mp3s and content/out/listen.html
Everything is built OUTSIDE the repository (scratch), the repository gets content/data and content/out only.
    build_content.py [stage ...]    (default: all)
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
S = os.environ.get("DBOY_CONTENT_SCRATCH", "/tmp/dboy-content")
PY = sys.executable


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def build_tools(build, gen):
    run(["cmake", "-S", HERE, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DDBOY_GEN_DIR=" + gen], stdout=subprocess.DEVNULL)
    run(["cmake", "--build", build, "--target", "render_presets", "panel_tool"], stdout=subprocess.DEVNULL)


def render(build, wtdir, wavdir, stats, n=8):
    os.makedirs(wavdir, exist_ok=True)
    env = dict(os.environ, DBOY_WT_DIR=wtdir)
    procs = []
    for s in range(n):
        f = open(stats + ".%d" % s, "w")
        procs.append((subprocess.Popen([os.path.join(build, "render_presets"), "render", wavdir, str(s), str(n)], env=env, stdout=f), f))
    for p, f in procs:
        if p.wait() != 0:
            raise SystemExit("render shard failed")
        f.close()
    with open(stats, "w") as out:
        rows = []
        for s in range(n):
            rows += [json.loads(l) for l in open(stats + ".%d" % s)]
            os.remove(stats + ".%d" % s)
        for r in sorted(rows, key=lambda r: r["i"]):
            out.write(json.dumps(r) + "\n")


def stage_analyse():
    run([PY, os.path.join(HERE, "analyse_fit.py")])


def stage_pool():
    pool = os.path.join(S, "pool")
    os.makedirs(pool, exist_ok=True)
    run([PY, os.path.join(HERE, "convert_presets.py"), "--key", "pool", "--tables", "auto", "--brought-out", pool + "/brought.json", "--out", pool,
         "--report", pool + "/report.json"])
    run([PY, os.path.join(HERE, "export_wavetables.py"), "--brought", pool + "/brought.json", "--out", pool + "/wavetables"])
    build_tools(os.path.join(S, "build_pool"), pool)
    render(os.path.join(S, "build_pool"), pool + "/wavetables", pool + "/wav", pool + "/stats.jsonl")


def stage_refine():
    run([PY, os.path.join(HERE, "refine_selection.py"), os.path.join(S, "pool", "stats.jsonl")])


def stage_final():
    data = os.path.join(REPO, "content", "data")
    run([PY, os.path.join(HERE, "convert_presets.py"), "--key", "chosen", "--tables", "auto"])
    run([PY, os.path.join(HERE, "export_wavetables.py")])
    build_tools(os.path.join(S, "build"), data)
    chk = subprocess.run([os.path.join(S, "build", "render_presets"), "check"], capture_output=True, text=True)
    print(chk.stdout)
    open(os.path.join(REPO, "content", "out", "check.txt"), "w").write(chk.stdout)
    render(os.path.join(S, "build"), os.path.join(data, "wavetables"), os.path.join(S, "wav"), os.path.join(S, "stats.jsonl"))
    if chk.returncode != 0:
        raise SystemExit("check failed")


def stage_nemesis():
    hb = os.path.join(S, "upstream_build")
    if not os.path.exists(os.path.join(hb, "upstream_render")):
        run(["cmake", "-S", os.path.join(HERE, "upstream_ref"), "-B", hb, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"], stdout=subprocess.DEVNULL)
        run(["cmake", "--build", hb, "-j10"], stdout=subprocess.DEVNULL)
    sel = json.load(open(os.path.join(REPO, "content", "out", "selection.json")))
    arch = os.path.join(REPO, "ref", "upstream", "docs", "dev", "sound-design", "archive", "presets")
    os.makedirs(os.path.join(S, "hy"), exist_ok=True)
    lines = "".join("%s\t%s\t%s\t%d\t%s\n" % (os.path.join(arch, c["file"]), c["name"], c["cat"], i, os.path.join(S, "hy", "%03d.wav" % i)) for i, c in enumerate(sel["chosen"]))
    r = subprocess.run([os.path.join(hb, "upstream_render")], input=lines, capture_output=True, text=True, check=True)
    open(os.path.join(S, "up_stats.jsonl"), "w").write(r.stdout)
    print("upstream renders:", len(r.stdout.strip().split("\n")))


def stage_report():
    run([PY, os.path.join(HERE, "finalize_report.py")])


def stage_listen():
    run([PY, os.path.join(HERE, "build_listen.py")])


STAGES = [("analyse", stage_analyse), ("pool", stage_pool), ("refine", stage_refine), ("final", stage_final), ("upstream", stage_nemesis), ("report", stage_report), ("listen", stage_listen)]

if __name__ == "__main__":
    want = sys.argv[1:] or [n for n, _ in STAGES]
    for n, f in STAGES:
        if n in want:
            print("== stage", n, flush=True)
            f()
