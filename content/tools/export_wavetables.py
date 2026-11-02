#!/usr/bin/env python3
"""export_wavetables.py - the wavetables the selected presets use, as .wt files for the SD card.

Runs content/tools/wt_export.cpp (upstream's own table kernels, built from ref/upstream/sdk/dsp, never modified) for every catalogue index in
content/data/wavetables/brought.json and writes wtNNN.wt (NNN = catalogue index), set.txt (slot k -> file; k is the position in brought.json
and what a preset's `table` field means), index.json / index.tsv and catalogue.json (all 200 banks with spectral probes).
    export_wavetables.py [--frames 32]
Format: content/data/wavetables/FORMAT.md
"""
import argparse, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
SCRATCH = os.environ.get("DBOY_CONTENT_SCRATCH", "/tmp/dboy-content")
BIN = os.path.join(SCRATCH, "wt_export")
WT = os.path.join(REPO, "content", "data", "wavetables")


def build():
    src = os.path.join(REPO, "ref", "upstream", "sdk", "dsp")
    gens = [os.path.join(src, "src", n + "TableGenerator.cpp") for n in ("Basic", "Chip", "Complex", "Harmonic", "Mask", "Metallic", "Vocal")]
    more = [os.path.join(src, "src", n + ".cpp") for n in ("WavetableKernels", "WavetableBank", "HarmonicFrame")]
    os.makedirs(SCRATCH, exist_ok=True)
    subprocess.check_call(["clang++", "-std=c++17", "-O2", "-I" + os.path.join(src, "include"), os.path.join(HERE, "wt_export.cpp")] + gens + more + ["-o", BIN])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=32)
    ap.add_argument("--brought", default=os.path.join(WT, "brought.json"))
    ap.add_argument("--out", default=WT)
    a = ap.parse_args()
    if not os.path.exists(BIN):
        build()
    brought = json.load(open(a.brought))
    out = a.out
    os.makedirs(out, exist_ok=True)
    cat_path = os.path.join(WT, "catalogue.json")
    if not os.path.exists(cat_path):
        subprocess.check_call([BIN, "index", cat_path])
    cat = json.load(open(cat_path))
    for f in os.listdir(out):
        if f.endswith(".wt"):
            os.remove(os.path.join(out, f))
    subprocess.check_call([BIN, "write", out, str(a.frames)] + [str(t) for t in brought])
    names = ["basic", "vocal", "metallic", "harmonic", "chip", "complex"]
    idx, total = [], 0
    for k, t in enumerate(brought):
        fn = "wt%03d.wt" % t
        size = os.path.getsize(os.path.join(out, fn))
        total += size
        b = cat[t]
        idx.append(dict(slot=k, catalogue=t, file=fn, id=b["id"], label=b["label"], category=names[b["category"]], frames=min(a.frames, b["frames"]),
                        sourceFrames=b["frames"], bytes=size))
    json.dump(dict(format="DBWT v1", frameLength=2048, sample="int16le", totalBytes=total, tables=idx), open(os.path.join(out, "index.json"), "w"), indent=1)
    with open(os.path.join(out, "index.tsv"), "w") as f:
        f.write("slot\tcatalogue\tfile\tid\tcategory\tframes\tsourceFrames\tbytes\n")
        for e in idx:
            f.write("%d\t%d\t%s\t%s\t%s\t%d\t%d\t%d\n" % (e["slot"], e["catalogue"], e["file"], e["id"], e["category"], e["frames"], e["sourceFrames"], e["bytes"]))
    with open(os.path.join(out, "set.txt"), "w") as f:   # for the host renderer: slot catalogue file
        for e in idx:
            f.write("%d %d %s\n" % (e["slot"], e["catalogue"], e["file"]))
    print("%d tables, %d files, %.2f MB total (%d frames max each)" % (len(idx), len(idx), total / 1e6, a.frames))


if __name__ == "__main__":
    main()
