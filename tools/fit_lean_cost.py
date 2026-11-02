#!/usr/bin/env python3
"""fit_lean_cost.py - what each option of a lean voice costs on the board: a least-squares fit of a tools/board_bench.py run over the library.

    board_bench.py BENCH.json --lean on --voices 3
    fit_lean_cost.py BENCH.json [--residuals N] [--table firmware/src/cost/LeanCostTable.h]

Each preset's `stepPct` (the rise per added note, which leaves out what the part costs with no voice sounding) is one equation: the plain voice plus the
options the preset uses. The library has no preset for each option alone, so a fit is the way to separate them. Prints cycles/sample per option, the same in
voice units (the plain voice = 1.0), and how well it predicts the presets; `--residuals N` lists the N presets it misses most. An option that fits below zero is
taken out and the fit run again (nothing gets cheaper for being switched on). The effects come the same way from each preset's `fxLoadPct`. --table writes the
table the firmware builds with (cost/CostModel.h, DBOY_COST_LEAN). Needs only the standard library."""
import json, os, statistics, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import prs

ROOT = os.path.abspath(os.path.join(HERE, ".."))
CYCLES_PER_PCT = 124.72   # 1% of the block budget in cycles per sample (12,472 cycles a sample at 44.1 kHz)
FOLDERS = ("presets", "presets2", "synthwave", "techno", "synthpop", "trance", "ebm", "royksopp", "delerium", "voice")
PG = {n: i for i, n in enumerate(prs.PAGES)}
IDLE_PCT = 3.9            # the board with nothing sounding (the sequencer, the codec, the bookkeeping)
# Voices cost more together than one at a time (they share the data cache), so the budget and the effects (which are real percentages) are scaled to the sum's units: the
# meter reads full where the board reads BUDGET_PCT. The scale comes from a run on the board: VOICES_HELD of preset HELD_ID, with no effects to speak of, took HELD_PCT of the block.
HELD_ID, VOICES_HELD, HELD_PCT = 98, 15, 53.9
BUDGET_PCT = 85.0         # the governor's enter threshold: what the voice budget is drawn up to
SAMPLER_VOICE_CYCLES = 213   # a sampler hit: 4.1% with 2.4 hits sounding on the default project, measured on the board
AMPS = ("off", "clean", "crunch", "lead", "bass")


def library():
    out = {}
    for folder in FOLDERS:
        d = os.path.join(ROOT, "content", folder)
        for f in sorted(os.listdir(d)):
            if f.upper().endswith(".PRS"):
                r = prs.decode(open(os.path.join(d, f), "rb").read())
                out[r["id"]] = r
    return out


def features(p):
    """The options a voice of this preset runs, as firmware/src/lean/LeanPart.cpp's makeParams reads the knobs. Model numbers as in CostBridge's kModelCost."""
    v = p["v"]
    g = lambda page, i: v[PG[page] * 8 + i]
    bank = [p["banks"][0] != 255, p["banks"][1] != 255]   # the file keeps a bank apart from WAVE; loading it writes WAVE = 4 + bank (Params.cpp)
    osc1, osc2 = g("OSC1", 6) > 0, g("OSC2", 6) > 0
    lfo_dest, lfo_depth, env2 = g("MOD", 2) & 3, g("MOD", 1), g("MOD", 4)
    pd_moves = not any(bank) and (env2 > 0 or (lfo_dest == 3 and lfo_depth > 0))
    pd1 = osc1 and not bank[0] and g("OSC1", 0) == 0 and (g("OSC1", 2) > 0 or pd_moves)
    pd2 = osc2 and not bank[1] and g("OSC2", 0) == 0 and (g("OSC2", 2) > 0 or pd_moves)
    ring, sync = osc1 and osc2 and g("SUB", 3) > 0, osc1 and g("OSC2", 7) > 0 and not pd1
    stack = max(1, min(4, g("SUB", 4))) if osc1 and max(g("OSC1", 7), g("SUB", 5)) > 0 and not (ring or sync) else 1   # as renderVoice: a live ring or sync collapses it
    wide = stack > 1 and g("SUB", 7) > 0
    chain = 2.0 if wide else 1.0   # a stereo voice runs everything after the oscillators twice
    m1, m2 = g("FLT1", 0), g("FLT2", 0)
    f2 = g("FLT2", 5) != 0
    grit = g("GRIT", 7) > 0 and (g("GRIT", 0) < 16 or g("GRIT", 1) > 0 or g("GRIT", 2) < 127 or g("GRIT", 3) > 0)
    model = lambda m: chain * (float(m1 == m) + float(f2 and m2 == m))   # the same price in either filter
    return dict(
        osc2=float(osc1 and osc2),
        bank=float(osc1 and bank[0]) + float(osc2 and bank[1]),
        pd=float(stack if pd1 else 0) + float(pd2),
        unison=float(stack - 1),
        width=float(wide),
        sub=float(g("SUB", 0) > 0),
        noise=chain * (g("SUB", 2) > 0),
        ladder=model(4),
        acid=model(5),
        soviet=model(6),
        classic=model(7),
        drive=chain * (g("FLT1", 3) > 0),
        hpf=chain * (g("FLT1", 6) > 0),
        f2=chain * f2,
        ring=float(ring),
        sync=float(sync),
        grit=chain * grit,
        clip=chain * (g("GRIT", 6) > 0),
    )


def solve(rows, ys):
    """Least squares by the normal equations and Gaussian elimination, with a whisper of ridge so an option no preset uses stays at 0."""
    n = len(rows[0])
    a = [[sum(r[i] * r[j] for r in rows) + (1e-6 if i == j else 0.0) for j in range(n)] + [sum(r[i] * y for r, y in zip(rows, ys))] for i in range(n)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        for r in range(n):
            if r != c:
                f = a[r][c] / a[c][c]
                a[r] = [x - f * z for x, z in zip(a[r], a[c])]
    return [a[i][n] / a[i][i] for i in range(n)]


def fx_features(p):
    """What a preset alone on part A has the effects section run."""
    amp = p["amp"][0] if p["mask"] & 8 else 0
    return dict(delay=float(p["sends"][0] > 0), reverb=float(p["sends"][1] > 0), chorus=float(p["sends"][2] > 0),
                **{a: float(amp == k) for k, a in enumerate(AMPS) if k})


def fit(rows, ys, names, keep):
    """Least squares with the options that fit below zero taken out, worst first. `keep` (the intercept) may stay."""
    live = list(range(len(names)))
    while True:
        x = solve([[r[k] for k in live] for r in rows], ys)
        neg = [(v, k) for v, k in zip(x, live) if v < 0 and names[k] not in keep]
        if not neg:
            return {names[k]: v for k, v in zip(live, x)}
        live.remove(min(neg)[1])


def write_table(path, x, fx, runs, lib):
    u = lambda n: x.get(n, 0.0) / x["plain"]
    held = sum(x.get(n, 0.0) * f for n, f in features(lib[HELD_ID]).items()) + x["plain"]   # cycles/sample the sum gives one voice of the held preset
    factor = HELD_PCT / (VOICES_HELD * held / CYCLES_PER_PCT)
    print("voices together cost x %.2f of their sum (%d of preset %d)" % (factor, VOICES_HELD, HELD_ID))
    fu = lambda n: fx.get(n, 0.0) * CYCLES_PER_PCT / x["plain"] / factor
    budget = int((BUDGET_PCT - IDLE_PCT) * CYCLES_PER_PCT / x["plain"] / factor)
    with open(path, "w") as o:
        o.write("// GENERATED by tools/fit_lean_cost.py from a board_bench.py run on the board (%d presets, --lean on). Do not edit: re-run the bench and the fit.\n" % len(runs))
        o.write("// Units: a plain lean voice (one oscillator, a state-variable low-pass, nothing else) = 1.0.\n#pragma once\n\n")
        o.write("// 1: the cost model describes the lean voice (cost/CostModel.h), 0: the CATHODE engine.\n#ifndef DBOY_COST_LEAN\n#define DBOY_COST_LEAN 0\n#endif\n\n")
        o.write("namespace cost::lean_table {\n\nconstexpr int kPlainCycles = %.0f;   // cycles/sample of the plain voice on the board, for reference only\n" % x["plain"])
        o.write("constexpr int kVoiceBudget = %d;     // plain voices that bring the board from idle (%.1f%%) to the governor's %.0f%%, sounding together (x %.2f)\n\n" % (budget, IDLE_PCT, BUDGET_PCT, factor))
        for name, key in (("kOsc2", "osc2"), ("kBank", "bank"), ("kPd", "pd"), ("kUnison", "unison"), ("kWidth", "width"), ("kSub", "sub"), ("kNoise", "noise"), ("kLadder", "ladder"),
                          ("kAcid", "acid"), ("kSoviet", "soviet"), ("kClassic", "classic"), ("kDrive", "drive"), ("kHpf", "hpf"), ("kF2", "f2"), ("kRing", "ring"),
                          ("kSync", "sync"), ("kGrit", "grit"), ("kClip", "clip")):
            o.write("constexpr float %s = %.3ff;\n" % (name, u(key)))
        o.write("constexpr float kSamplerVoice = %.3ff;\n\n" % (SAMPLER_VOICE_CYCLES / x["plain"] / factor))
        for name, key in (("kCostDelay", "delay"), ("kCostReverb", "reverb"), ("kCostChorus", "chorus")):
            o.write("constexpr float %s = %.3ff;\n" % (name, fu(key)))
        o.write("constexpr float kCostComp = %.3ff;   // not in the bench's presets: the host's ratio to the reverb\n" % (fu("reverb") * 0.19 / 0.48))
        # (no preset in the library runs CLEAN or BASS alone enough to fit: the host's ratios to the models that are, FxCost.h)
        amps = [fu("crunch") * 0.90 / 1.10, fu("crunch"), fu("lead"), fu("lead")]
        o.write("constexpr float kCostAmp[5] = {0.0f, %s};   // off, clean, crunch, lead, bass\n\n}  // namespace cost::lean_table\n" % ", ".join("%.3ff" % a for a in amps))


def main(argv):
    if len(argv) < 2 or argv[1].startswith("-"):
        print(__doc__); return 2
    show = int(argv[argv.index("--residuals") + 1]) if "--residuals" in argv else 0
    lib = library()
    runs = [r for r in json.load(open(argv[1]))["results"] if r.get("stepPct") is not None and r["id"] in lib]
    names = ["plain"] + list(features(next(iter(lib.values()))))
    rows = [[1.0] + list(features(lib[r["id"]]).values()) for r in runs]
    ys = [r["stepPct"] * CYCLES_PER_PCT for r in runs]
    fit_x = fit(rows, ys, names, ("plain",))
    x = [fit_x.get(n, 0.0) for n in names]
    pred = [sum(a * b for a, b in zip(r, x)) for r in rows]
    err = [p - y for p, y in zip(pred, ys)]
    print("%d presets; plain voice %.0f cycles/sample" % (len(runs), x[0]))
    print("%-8s %8s %8s %8s" % ("option", "cycles", "units", "presets"))
    for k, name in enumerate(names):
        print("%-8s %8.0f %8.3f %8d" % (name, x[k], x[k] / x[0], sum(1 for r in rows if r[k])))
    print("error: median |%.0f| cycles, 90%% within %.0f, worst %.0f" % (statistics.median(abs(e) for e in err), sorted(abs(e) for e in err)[int(len(err) * 0.9)], max(abs(e) for e in err)))
    if show:
        for k in sorted(range(len(runs)), key=lambda i: -abs(err[i]))[:show]:
            on = [n for n, f in zip(names[1:], rows[k][1:]) if f]
            print("  %4d %-24s measured %5.0f predicted %5.0f  %s" % (runs[k]["id"], runs[k]["name"][:24], ys[k], pred[k], " ".join(on)))
    fx_names = ["base"] + list(fx_features(next(iter(lib.values()))))
    fx_x = fit([[1.0] + list(fx_features(lib[r["id"]]).values()) for r in runs], [r["fxLoadPct"] for r in runs], fx_names, ("base",))
    print("effects, %% of the block:", " ".join("%s %.1f" % (n, v) for n, v in fx_x.items()))
    if "--table" in argv:
        write_table(argv[argv.index("--table") + 1], fit_x, fx_x, runs, lib)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
