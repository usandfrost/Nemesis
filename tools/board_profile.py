#!/usr/bin/env python3
"""board_profile.py - where the CPU really spends its time while presets sound: PC samples from the Cortex-M7's DWT (no halting) over a spread of presets.

    board_profile.py OUT.json [--ids 10,11,300-310 | --every N] [--seconds 4] [--note 57] [--elf PATH] [--play]

For each preset: load it on part A, hold one note, read the program counter (DWT_PCSR) as fast as OpenOCD answers (~1,700 a second), and count the
samples per function. OUT.json is {function: {size, samples}} summed over all presets, plus the run's settings. tools/gen_itcm.py --samples OUT.json
uses it to fill the ITCM with what is hot, not what might be. The ELF must be the build on the board. The degrade governor is off meanwhile.
--play samples the project as it is, playing from the sequencer (the drums, the sequencer, a full mix), instead of a note held on each preset."""
import collections, json, os, re, subprocess, sys, time, bisect
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import board_script as bs
import board_bench

ROOT = os.path.abspath(os.path.join(HERE, ".."))


def main(argv):
    args = argv[1:]
    if not args or args[0].startswith("-"):
        print(__doc__); return 2
    out_path, seconds, note, elf, every, ids, play = args[0], 4.0, 57, os.path.join(ROOT, "firmware/build/dboy_cathode.elf"), 7, None, False
    lib = board_bench.library()
    i = 1
    while i < len(args):
        if args[i] == "--ids": ids = board_bench.ids_arg(args[i + 1], lib); i += 2
        elif args[i] == "--every": every = int(args[i + 1]); i += 2
        elif args[i] == "--seconds": seconds = float(args[i + 1]); i += 2
        elif args[i] == "--note": note = int(args[i + 1]); i += 2
        elif args[i] == "--elf": elf = args[i + 1]; i += 2
        elif args[i] == "--play": play = True; i += 1
        else: print(__doc__); return 2
    ids = [None] if play else ids or sorted(lib)[::every]
    funcs = []
    for l in subprocess.run(["arm-none-eabi-nm", "-S", elf], capture_output=True, text=True).stdout.splitlines():
        f = l.split(None, 3)
        if len(f) == 4 and f[2] in "tTwW": funcs.append((int(f[0], 16), int(f[1], 16), f[3]))
    funcs.sort()
    starts = [f[0] for f in funcs]
    counts, sizes, total = collections.Counter(), {}, 0
    bd = bs.Board(elf)
    try:
        for n in ("governor off", "key PLAY") if play else ("governor off", "solo A"):
            bs.run_line(bd, n, 0, ".")
        bd.ocd.cmd("mww 0xE000EDFC 0x01000000")   # DEMCR.TRCENA: the DWT answers
        for k, pid in enumerate(ids):
            if not play:
                bd.send(bs.EV["note"], note, 0)
                bd.send(bs.EV["preset"], 0, pid)
                time.sleep(0.5)
                bd.send(bs.EV["note"], note, 100)
                time.sleep(0.7)
            t0, got = time.time(), 0
            while time.time() - t0 < seconds:
                m = re.search(r": ([0-9a-f]{8})", bd.ocd.cmd("mdw 0xE000101C 1"))
                if not m: continue
                p = int(m.group(1), 16) & ~1
                j = bisect.bisect_right(starts, p) - 1
                if j >= 0 and p < funcs[j][0] + funcs[j][1]:
                    counts[funcs[j][2]] += 1
                    sizes[funcs[j][2]] = funcs[j][1]
                got += 1
            total += got
            print("%3d/%d  %-31s %d samples" % (k + 1, len(ids), "the project, playing" if play else "%4d %s" % (pid, lib[pid][0][:26]), got), flush=True)
        bs.run_line(bd, "key STOP", 0, ".") if play else bd.send(bs.EV["note"], note, 0)
        bs.run_line(bd, "governor on", 0, ".")
    finally:
        bd.ocd.close()
    json.dump(dict(elf=os.path.basename(elf), presets=ids, seconds=seconds, note=note, total=total,
                   functions={n: dict(size=sizes[n], samples=c) for n, c in counts.items()}), open(out_path, "w"), indent=1)
    print("saved", out_path, "-", total, "samples,", len(counts), "functions")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
