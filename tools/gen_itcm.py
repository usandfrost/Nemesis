#!/usr/bin/env python3
"""Choose the DSP functions that run from ITCM (docs/DSP_REVIEW.md P4) and write firmware/platform/itcm_functions.ld.

    gen_itcm.py ELF --objdump arm-none-eabi-objdump --nm arm-none-eabi-nm [--budget 61440] [--out PATH] [--samples PROFILE.json ...]

Walks the call graph from the audio render (CathodeRenderer::render) over direct calls, drops what only runs at note-on,
parameter change or prepare time (by name, the same rules as tools/fp_profile.py), and fills the ITCM budget with what is left:
first the voice loop's own passes and the per-sample DSP (by name), then everything else reachable, smallest first (more
functions per byte). Needs -ffunction-sections (each function in .text.<mangled name>). Two-pass: build, run this, build again.
Static analysis alone knows what CAN run per sample, not how often. With --samples (tools/board_profile.py: DWT program-counter samples from
the board over a spread of presets) the functions that were actually hot go in first, densest first (samples per byte); the static ranking fills
what is left, so an engine or filter the sample set did not use is still covered. Measured on the board 2026-10-28: with the static list alone,
80% of the CPU samples ran from flash, including 9% in modulateSample (6.7 KB, too big to be reached by the smallest-first fill).
"""
import argparse
import json
import re
import subprocess

COLD = re.compile(r"prepare|noteOn|noteOff|assign|setNoteHz|Table|Generator|gateOn|gateOff|landLayout|landMorph|startMorph|latch|Pending|"
                  r"resolve|recompute|trigger|[Ss]et[A-Z]|Steal|plaits|panic|reset|Reset|reshape|Request|request|bind|Bind|seed|Seed|"
                  r"^__|Error_Handler|HAL_|rem_pio|^cos|^sin|^pow|^exp|^log|^tan|memcpy|memset|init|Init|Lanes|lanes")
HOT_FIRST = re.compile(r"processPoly|processMono|midPass|ampPass|processSample|chainPre|filterStage|foldPair|modulateSample|processCore|"
                       r"processPost|finishBody|finishSample|slotArm|singleArm|stackSample|fmLeg|ringLeg|decimate|Oversampler|AdaaSoftClip|"
                       r"coreStep|drivenStep|linearStep|crossStep|refreshLive|tickModEnv|shapeValue|driveScalar|render")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--nm", required=True)
    ap.add_argument("--root", action="append", help="a function the walk starts from (default: the Instrument's and the CathodeRenderer's render)")
    ap.add_argument("--samples", action="append", help="a profile from tools/board_profile.py (more than one are added up): its hot functions are placed first")
    ap.add_argument("--budget", type=int, default=60 * 1024)   # of 64 KB: room for the linker's long-branch veneers
    ap.add_argument("--out", default="firmware/platform/itcm_functions.ld")
    a = ap.parse_args()

    dis = subprocess.run([a.objdump, "-d", "--no-show-raw-insn", a.elf], capture_output=True, text=True, check=True).stdout
    calls, cur = {}, None
    call = re.compile(r"\b(?:bl|b\.w|b)\s+[0-9a-f]+ <([^>+]+)")
    for line in dis.splitlines():
        m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
        if m:
            cur = m.group(1)
            calls[cur] = set()
        elif cur:
            c = call.search(line)
            if c:
                n = c.group(1)
                calls[cur].add(n[2:-7] if n.startswith("__") and n.endswith("_veneer") else n)   # follow ld's long-branch veneers
    size = {}
    for line in subprocess.run([a.nm, "-S", a.elf], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) >= 4 and p[2] in "tTwW":
            size[p[3]] = int(p[1], 16)

    seen, stack = set(), list(a.root or ["_ZN5audio10Instrument6renderEPfS1_i", "_ZN15CathodeRenderer6renderEPfS0_i"])
    while stack:
        f = stack.pop()
        if f in seen or f not in calls:
            continue
        seen.add(f)
        stack.extend(calls[f])
    demangled = dict(zip(sorted(seen), subprocess.run(["c++filt"], input="\n".join(sorted(seen)), capture_output=True, text=True).stdout.split("\n")))
    hot = [f for f in seen if f in size and not COLD.search(demangled.get(f, f).split("(")[0].split("::")[-1]) and not f.startswith("plaits")
           and "plaits" not in demangled.get(f, f)]
    first = sorted([f for f in hot if HOT_FIRST.search(demangled.get(f, f))], key=lambda f: size[f])
    rest = sorted([f for f in hot if f not in first], key=lambda f: size[f])
    chosen, used, measured = [], 0, []
    if a.samples:
        prof = {}
        for path in a.samples:
            for f, d in json.load(open(path))["functions"].items():
                e = prof.setdefault(f, dict(size=d["size"], samples=0))
                e["samples"] += d["samples"]
        names = subprocess.run(["c++filt"], input="\n".join(prof), capture_output=True, text=True).stdout.split("\n")
        for f, dn in zip(prof, names):
            engine = dn.startswith(("nemesis::", "fm::", "audio::", "fx::", "lean::")) or "upstream" in dn
            if f in size and prof[f]["samples"] >= 3 and (f in seen or engine) and "plaits" not in dn and "_veneer" not in f:
                measured.append(f)
        measured.sort(key=lambda f: -prof[f]["samples"] / (size[f] + 8))   # samples per byte of ITCM
        for f in measured:
            if used + size[f] + 8 > a.budget:
                continue
            chosen.append(f)
            used += size[f] + 8
    for f in first + rest:
        if f in chosen:
            continue
        if used + size[f] + 8 > a.budget:   # + alignment slack
            continue
        chosen.append(f)
        used += size[f] + 8
    with open(a.out, "w") as o:
        o.write("/* generated by tools/gen_itcm.py from %s: %d functions, ~%d bytes of %d reachable per-sample bytes%s. */\n"
                % (a.elf.split("/")[-1], len(chosen), used, sum(size[f] for f in hot),
                   ", %d of them measured hot (%s)" % (len([f for f in chosen if f in measured]), ", ".join(p.split("/")[-1] for p in a.samples)) if a.samples else ""))
        for f in sorted(chosen):
            o.write("*(.text.%s)\n" % f)
    print("%d of %d per-sample functions, %d bytes into ITCM (%d bytes reachable per sample)" % (len(chosen), len(hot), used, sum(size[f] for f in hot)))


if __name__ == "__main__":
    main()
