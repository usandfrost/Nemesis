#!/usr/bin/env python3
"""Generate the cost table from the cost benchmark's output, so the numbers have ONE source.

    gen_cost_table.py bench/out_new/cost_double.txt firmware/src/cost/CostTable.h [docs/mockup/drumboy-panel.html]

Reads the rows `cost_bench` prints (group, feature, ns/sample, x baseline, extra ns). Everything is divided by the plain-voice baseline,
so the table is in VOICE UNITS (a plain voice = 1.0) and is independent of the machine it was measured on. Re-run the benchmark on the
board and regenerate to replace the host-relative numbers. If given a third argument, the same table is written into the mockup between the
/*COSTTABLE*/ markers, so the mockup and the firmware can never disagree.
"""
import json
import re
import sys

FILTERS = ["LowPass", "BandPass", "HighPass", "Notch", "Formant", "Comb", "Ladder24", "Ladder12", "ScreamLp", "ScreamHp", "Acid", "SovietLp",
           "SovietBp", "SovietHp", "ClassicLp24", "ClassicLp12", "ClassicBp", "ClassicHp", "ReedLp", "ReedBp", "ReedHp", "StateLp", "StateBp",
           "StateHp", "StateNotch", "StateAp", "StateMorph", "ReedMorph"]
# Every model that can measure heavy needs a LIGHT twin with the same response; firmware/src/cost/CostModel.h lightFallback mirrors it.
# The generator refuses a heavy model without one (a re-measure that pushes a light model over the threshold must be decided by a person).
LIGHT_TWIN = {"ScreamLp": "Ladder24", "Acid": "Ladder24", "ClassicLp24": "Ladder24", "SovietLp": "Ladder12", "ClassicLp12": "Ladder12",
              "ScreamHp": "HighPass", "SovietHp": "HighPass", "ClassicHp": "HighPass", "SovietBp": "BandPass", "ClassicBp": "BandPass",
              "ReedLp": "StateLp", "ReedBp": "StateBp", "ReedHp": "StateHp", "ReedMorph": "StateMorph"}
HEAVY_PREFIXES = ("Scream", "Soviet", "Classic", "Reed", "Acid")


def parse(path):
    rows = {}
    for ln in open(path):
        p = re.split(r"\s{2,}", ln.strip())
        if len(p) == 5 and p[3].endswith("x"):
            rows[(p[0], p[1])] = float(p[2])
    return rows


def main():
    rows = parse(sys.argv[1])
    ns = lambda g, n: rows[(g, n)]
    base = ns("OSC", "Phase distortion (baseline)")
    u = lambda a, b=base: round((a - b) / base, 3)

    t = {"baselineNs": round(base)}
    t["filter"] = {f: (u(ns("FILTER", f)) if u(ns("FILTER", f)) >= 0.06 else 0.0) for f in FILTERS}   # under 0.06 is measurement noise: a light model is exactly free
    m = re.search(r"filter cores and drives at x(\d+)", open(sys.argv[1]).read())
    t["tableOversample"] = int(m.group(1)) if m else 4   # the factor the filter and drive extras were measured at
    u4 = u(ns("OSC ADD-ON", "Unison x4"))
    s2 = ns("OSC ADD-ON", "Second oscillator (PD)")
    both = ns("OSC ADD-ON", "Unison x4 on both slots (both audible)")
    t["unisonSlot1Factor"] = round(max(0.5, ((both - s2) / base - u4) / u4), 2) if u4 > 0 else 1.0
    t["drive"] = u(ns("FILTER ADD-ON", "Filter drive"))
    s2 = ns("OSC ADD-ON", "Second oscillator (PD)")
    t["ring"] = u(ns("OSC ADD-ON", "Second oscillator + ring mod"), s2)
    t["fm"] = u(ns("OSC ADD-ON", "Second oscillator + FM cross-mod"), s2)
    t["sync"] = max(0.0, u(ns("OSC ADD-ON", "Second oscillator + hard sync"), s2))
    t["unison"] = {"2": u(ns("OSC ADD-ON", "Unison x2")), "4": u(ns("OSC ADD-ON", "Unison x4")), "8": u(ns("OSC ADD-ON", "Unison x8"))}
    # Width engages the stereo right chain, which re-runs the filter: a fixed part plus one more copy of the active filter's own extra.
    t["widthFixed"] = u(ns("OSC ADD-ON", "Unison x4 + stereo width"), ns("OSC ADD-ON", "Unison x4"))
    t["companding"] = u(ns("GRIT", "Bit depth 10 + companding"), ns("GRIT", "Bit depth 10"))
    t["fold"] = u(ns("GRIT", "Fold"))
    t["f2"] = {"series": u(ns("FILTER ADD-ON", "F2 on, series (LowPass+LowPass)")), "parallel": u(ns("FILTER ADD-ON", "F2 on, parallel")),
               "stereo": u(ns("FILTER ADD-ON", "F2 on, STEREO (right chain)"))}
    t["osc"] = {"PD": 0.0, "Wavetable": u(ns("OSC", "Wavetable")), "Bitmask": u(ns("OSC", "Bitmask")), "Pulse": u(ns("OSC", "Pulse")),
                "Macro": u(ns("MACRO (Plaits engine)", "Analog")),
                "MacroHeavy": u(ns("MACRO (Plaits engine)", "TwoOpFm"), ns("MACRO (Plaits engine)", "Analog"))}
    t["slotCost"] = u(s2)                  # one more computed oscillator slot (measured with the firmware savings: both slots audible vs one)
    t["heavyThreshold"] = 0.2
    # data-driven: heavy = an extra of 0.2 voices or more at x4 (the scale the rule was defined on), whatever factor it was measured at
    OVERSAMPLED = {f for f in FILTERS if f.startswith(("Scream", "Acid", "Soviet", "Classic", "Reed"))}
    x4 = lambda f: t["filter"][f] * (4 / t["tableOversample"] if f in OVERSAMPLED else 1)
    t["heavy"] = [f for f in FILTERS if x4(f) >= t["heavyThreshold"]]
    missing = [f for f in t["heavy"] if f not in LIGHT_TWIN or LIGHT_TWIN[f] in t["heavy"]]
    if missing:
        sys.exit(f"heavy model(s) without a light twin: {missing}. Add them to LIGHT_TWIN here and to cost::lightFallback, or raise the threshold.")

    h = ["// GENERATED by tools/gen_cost_table.py from the cost benchmark. Do not edit: re-run the benchmark and the generator.",
         "// Units: a plain voice (ONE audible phase-distortion oscillator, a low-pass, nothing else; the silent second slot is skipped) = 1.0.",
         "// Measured with the firmware's exact savings on (cost_bench with COST_CFG=fw), on the host, so these are",
         "// RELATIVE costs; regenerate from a run on the board. Filter and drive extras are for kTableOversample and are scaled at use.",
         "#pragma once", "", "namespace cost::table {", "",
         f"constexpr int kBaselineNs = {t['baselineNs']};   // the baseline voice on the measuring machine, for reference only", ""]
    h.append("constexpr float kFilterExtra[] = {   // order = cost::Filter")
    for f in FILTERS:
        h.append(f"    {t['filter'][f]:.3f}f,   // {f}")
    h += ["};", ""]
    for k, v in [("kDrive", t["drive"]), ("kRing", t["ring"]), ("kFm", t["fm"]), ("kSync", t["sync"]), ("kWidthFixed", t["widthFixed"]),
                 ("kCompanding", t["companding"]), ("kFold", t["fold"]), ("kSlotCost", t["slotCost"]),
                 ("kUnison2", t["unison"]["2"]), ("kUnison4", t["unison"]["4"]), ("kUnison8", t["unison"]["8"]),
                 ("kF2Series", t["f2"]["series"]), ("kF2Parallel", t["f2"]["parallel"]), ("kF2Stereo", t["f2"]["stereo"]),
                 ("kOscWavetable", t["osc"]["Wavetable"]), ("kOscBitmask", t["osc"]["Bitmask"]), ("kOscPulse", t["osc"]["Pulse"]),
                 ("kOscMacro", t["osc"]["Macro"]), ("kOscMacroHeavy", t["osc"]["MacroHeavy"]), ("kHeavyThreshold", t["heavyThreshold"]),
                 ("kUnisonSlot1Factor", t["unisonSlot1Factor"])]:
        h.append(f"constexpr float {k} = {v:.3f}f;")
    h.append(f"constexpr int kTableOversample = {t['tableOversample']};   // filter-core and drive extras above were measured at this factor")
    h += ["", "}  // namespace cost::table", ""]
    open(sys.argv[2], "w").write("\n".join(h))
    print(f"heavy (x4-equivalent >= {t['heavyThreshold']}): {', '.join(t['heavy']) or 'none'}")
    print(f"wrote {sys.argv[2]}: baseline {base:.0f} ns, heavy filters {', '.join(f'{f} {t['filter'][f]:.2f}' for f in ['Acid','ClassicLp24','ReedLp','SovietLp','ScreamLp'])}")

    if len(sys.argv) > 3:
        js = "const COST = " + json.dumps({"filterList": FILTERS, "filter": [t["filter"][f] for f in FILTERS], "heavyThreshold": t["heavyThreshold"], "drive": t["drive"], "ring": t["ring"], "fm": t["fm"], "sync": t["sync"], "widthFixed": t["widthFixed"],
                                          "companding": t["companding"], "fold": t["fold"], "unison": t["unison"], "f2": t["f2"], "slotCost": t["slotCost"],
                                          "osc": t["osc"], "lightTwin": LIGHT_TWIN,
                                          "tableOversample": t["tableOversample"], "unisonSlot1Factor": t["unisonSlot1Factor"]}, indent=None) + ";"
        html = open(sys.argv[3]).read()
        a, b = "/*COSTTABLE*/", "/*END COSTTABLE*/"
        if a in html:
            i, j = html.index(a), html.index(b)
            html = html[:i] + a + "\n" + js + "\n" + html[j:]
            open(sys.argv[3], "w").write(html)
            print(f"updated the cost table in {sys.argv[3]}")
        else:
            print("mockup has no /*COSTTABLE*/ markers yet; skipped")


main()
