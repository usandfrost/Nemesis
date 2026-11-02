#!/usr/bin/env python3
"""Build the FX-candidate tree for the host benchmarks, OUTSIDE the repo (nothing under firmware/ or tools/ is touched).

    make_tree.py OUT_DIR

Copies the upstream FX headers/sources that firmware/dsp does not have (the closure of the FX roots below) from ref/upstream/sdk/dsp,
converts `double` -> `Real` with the same rules as tools/sync_dsp.py (imported read-only), and writes
    OUT_DIR/inc/nemesis/dsp/*.h   OUT_DIR/src/*.cpp
Compile them together with firmware/dsp (Real.h, Oversampler.h, Svf.h, Waveshaper.h ... come from the fork). With NEMESIS_DSP_FLOAT unset
Real is double and the result equals upstream.
"""
import os, re, shutil, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import sync_dsp  # noqa: E402  (read-only import: convert_text, add_include)

UP = f"{REPO}/ref/upstream/sdk/dsp"
FORK = f"{REPO}/firmware/dsp"
ROOTS = ["DelayEffect.h", "ReverbEffect.h", "GampEffect.h", "ChorusEffect.h", "CompEffect.h", "DriveEffect.h", "PhaserEffect.h"]


def read(p):
    with open(p) as f:
        return f.read()


def closure(roots):
    seen, st = [], list(roots)
    while st:
        h = st.pop()
        if h in seen or not os.path.exists(f"{UP}/include/nemesis/dsp/{h}"):
            continue
        seen.append(h)
        st += re.findall(r'#include <nemesis/dsp/([A-Za-z0-9_]+\.h)>', read(f"{UP}/include/nemesis/dsp/{h}"))
        cpp = f"{UP}/src/{h[:-2]}.cpp"
        if os.path.exists(cpp):
            st += re.findall(r'#include <nemesis/dsp/([A-Za-z0-9_]+\.h)>', read(cpp))
    return seen


def main():
    out = os.path.abspath(sys.argv[1])
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(f"{out}/inc/nemesis/dsp")
    os.makedirs(f"{out}/src")
    new = [h for h in closure(ROOTS) if not os.path.exists(f"{FORK}/include/nemesis/dsp/{h}")]
    for h in new:
        for rel_in, rel_out in ((f"include/nemesis/dsp/{h}", f"inc/nemesis/dsp/{h}"), (f"src/{h[:-2]}.cpp", f"src/{h[:-2]}.cpp")):
            p = f"{UP}/{rel_in}"
            if not os.path.exists(p):
                continue
            s = read(p)
            n = sync_dsp.convert_text(s, False)
            if n != s:
                n = sync_dsp.add_include(n, "Real.h")
            with open(f"{out}/{rel_out}", "w") as f:
                f.write(n)
    print("headers taken from upstream (not in the fork):", " ".join(new))


if __name__ == "__main__":
    main()
