#!/usr/bin/env python3
"""board_bench.py - what each library preset costs on the board: load it into part A, hold a chord of the voices it is configured for, read the part's share of the audio block budget.

    board_bench.py OUT.json [--ids 10,11,300-310] [--note 57] [--hold 3.0] [--lean on|off] [--voices N]

For every preset (default: all in content/*/ .PRS files, plus the factory three) it loads the preset into part A (the firmware sets the part's VOICES from
the preset, trimmed to its CPU budget), plays that many notes at once (a chord from --note up), adds them one at a time (each held --hold seconds: the numbers are averages over the last second, so hold longer than one; a note is not added once the part passes 85% of the budget, which keeps the board from being overloaded with the governor off), and records the part's load and the effects' load as percentages of one audio block's cycle budget.
`perVoicePct` = (part load - the part's fixed share) / voices sounding; `stepPct` = the median rise per added note (from four notes up it is steadier than the slope: the first reading can
carry a bank being loaded). --voices N sets the part to N voices whatever the preset says, so a mono preset gets a step too. The degrade governor is off while it
runs. tools/board_script.py explains the board connection. About 5 seconds a preset."""
import json, os, statistics, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import board_script as bs
import prs

ROOT = os.path.abspath(os.path.join(HERE, ".."))
PART_FIXED_PCT = 5.8   # the part's share with no voice sounding but one configured (measured: tools/board_script.py profile, docs/BRINGUP.md)
FOLDERS = ("presets", "presets2", "synthwave", "techno", "synthpop", "trance", "ebm", "royksopp", "delerium", "voice")


def library():
    out = {0: ("Iron Squelch", 0), 1: ("Sub Pressure", 0), 2: ("Dimension Bloom", 0)}   # the factory three (voices are in the firmware)
    for folder in FOLDERS:
        d = os.path.join(ROOT, "content", folder)
        for f in sorted(os.listdir(d)):
            if f.upper().endswith(".PRS"):
                r = prs.decode(open(os.path.join(d, f), "rb").read())
                out[r["id"]] = (r["name"], r["voices"])
    return out


def ids_arg(text, lib):
    ids = []
    for part in text.split(","):
        a, _, b = part.partition("-")
        ids += range(int(a), int(b or a) + 1)
    return [i for i in ids if i in lib]


ABORT_PCT = 85.0   # stop adding notes when the part (plus effects) reaches this share of the block budget: the governor is off, so more would starve the main loop


def measure(bd, pid, name, voices, note, hold, silent=12.0, force=0):
    """Load the preset, then add one note at a time (up to the voices it is configured for) and read the part's load after each."""
    for attempt in range(3):   # a load reads the file from the card: check that the part really holds the preset, else ask again
        bd.send(bs.EV["preset"], 0, pid)
        time.sleep(1.0)
        s0 = bd.status()
        if s0["partPreset"][0] == pid:
            break
    else:
        raise bs.Fail("preset %d did not load (part A holds %d)" % (pid, s0["partPreset"][0]))
    if force:
        bd.send(bs.EV["voices"], 0, force)
        time.sleep(0.4)
        s0 = bd.status()
    cfg = max(1, s0["cfgVoices"][0])
    chord = [note + k * 3 + (k // 2) for k in range(cfg)]   # a stack of minor and major thirds, one note per configured voice
    loads, fx, sounding, on = [], 0.0, 0, 0.0
    held = []
    try:
        for n in chord:
            before = bd.status()
            bd.send(bs.EV["note"], n, 100)
            held.append(n)
            time.sleep(hold)
            s = bd.status()
            # a preset whose wavetable bank is still being read from the card plays silence: wait for it, note by note (up to ~10 s for the first note)
            for _ in range(7 if not loads else 0):
                if s["partLoad"][0] >= silent:
                    break
                bd.send(bs.EV["note"], n, 0)
                time.sleep(1.2)
                bd.send(bs.EV["note"], n, 100)
                time.sleep(hold)
                s = bd.status()
            loads.append(s["partLoad"][0])
            fx, sounding = s["fxLoad"], s["voices"][0]
            starts = s["starts"] - before["starts"]
            if starts:
                on = (s["startOnSumKc"] - before["startOnSumKc"]) / starts
            if s["partLoad"][0] + s["fxLoad"] + s["samplerLoad"] >= ABORT_PCT:
                break
    finally:
        for n in held:
            bd.send(bs.EV["note"], n, 0)
        time.sleep(0.5)
        # let the release tails and the effects' tails die away, or the next preset starts on a loaded board and its numbers (and its load) are wrong
        for _ in range(12):
            try:
                if bd.status()["loadAvg"] < 8.0 and bd.status()["load"] < 12.0:
                    break
            except bs.Fail:
                break
            time.sleep(0.7)
    # a voice: the slope between the first and the last measured count when there are two or more, else the single voice less the part's fixed share
    per = (loads[-1] - loads[0]) / (len(loads) - 1) if len(loads) > 1 else loads[0] - PART_FIXED_PCT
    step = statistics.median(loads[k + 1] - loads[k] for k in range(len(loads) - 1)) if len(loads) > 1 else None
    return dict(id=pid, name=name, recommendedVoices=voices, configuredVoices=cfg, notesPlayed=len(loads), soundingVoices=sounding, loadByVoices=loads,
                perVoicePct=per, stepPct=step, fxLoadPct=fx, noteOnKcycles=on, stoppedAtLimit=loads[-1] + fx >= ABORT_PCT)


def main(argv):
    args = argv[1:]
    if not args or args[0].startswith("-"):
        print(__doc__); return 2
    out_path, note, hold, ids, lean, force = args[0], 57, 2.3, None, None, 0
    lib = library()
    i = 1
    while i < len(args):
        if args[i] == "--ids": ids = ids_arg(args[i + 1], lib); i += 2
        elif args[i] == "--note": note = int(args[i + 1]); i += 2
        elif args[i] == "--hold": hold = float(args[i + 1]); i += 2
        elif args[i] == "--voices": force = int(args[i + 1]); i += 2
        elif args[i] == "--lean":
            lean = args[i + 1].lower(); i += 2
            if lean not in ("on", "off"): print("--lean takes on or off"); return 2   # on|off: the voice engine (firmware built with -DDBOY_LEAN=ON); the same run twice is the A/B; a firmware built without it ignores the switch, and the board stays on the engine chosen until it is reset
        else: print(__doc__); return 2
    ids = ids or sorted(lib)
    elf = os.path.join(ROOT, "firmware/build/dboy_cathode.elf")
    setup = ("governor off", "solo A") + (() if lean is None else ("lean " + lean,))
    bd = bs.Board(elf)
    results = []
    try:
        for n in setup:
            bs.run_line(bd, n, 0, ".")
        for k, pid in enumerate(ids):
            name, voices = lib[pid]
            r = None
            for attempt in (1, 2):
                try:
                    r = measure(bd, pid, name, voices, note, hold, 12.0 if lean == "off" else 1.2, force)   # the share that says a part really is sounding: 12% on CATHODE, a plain lean voice is 1.7%
                    break
                except bs.Fail as e:   # the board stalled (an overload with the governor off): reset it and try this preset once more, then give up on it
                    print("   %d: %s -> resetting" % (pid, e), flush=True)
                    bd.reset()
                    for n in setup:
                        bs.run_line(bd, n, 0, ".")
            if r is None:
                r = dict(id=pid, name=name, recommendedVoices=voices, error="the board stalled twice")
            results.append(r)
            if "error" in r:
                print("%3d/%d  %4d %-26s %s" % (k + 1, len(ids), pid, name[:26], r["error"]), flush=True)
            else:
                print("%3d/%d  %4d %-26s cfg %d  load by voices %s  fx %4.1f%%  a voice ~%4.1f%%  noteOn %3.0f kc" % (k + 1, len(ids), pid, name[:26], r["configuredVoices"],
                      "/".join("%.0f" % x for x in r["loadByVoices"]), r["fxLoadPct"], r["perVoicePct"], r["noteOnKcycles"]), flush=True)
            json.dump(dict(note=note, hold=hold, lean=lean, results=results), open(out_path, "w"), indent=1)
        bs.run_line(bd, "governor on", 0, ".")
        return 0
    except bs.Fail as e:
        print("board_bench: " + str(e), file=sys.stderr)
        return 1
    finally:
        try:   # leave the board as a player would have it
            bd.problem = None
            bd.send(bs.EV["governor"], 0, 1)
            bd.send(bs.EV["solo"], 255)
        except Exception:
            pass
        bd.ocd.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
