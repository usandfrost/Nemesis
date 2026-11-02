#!/usr/bin/env python3
"""Rebuild firmware/dsp/ from an upstream sdk/dsp checkout and re-apply every FIREFORK change.

    sync_dsp.py UPSTREAM_SDK_DSP DEST            e.g. ref/upstream/sdk/dsp firmware/dsp

Everything this repo changes in the DSP is done HERE, so a new upstream snapshot is one command, not an afternoon:

  1. copy include/ src/ thirdparty/ (no tests; Convolution.* is JUCE-only and dropped)
  2. add Real.h / FastMath.h  (templates in tools/fork/)
  3. mechanical `double` -> `Real` in the CATHODE voice closure, except identifiers that must stay `Wide` (phase
     accumulators, increments, frequencies, sample rate: see WIDE_RE)
  4. hand patches, each ASSERTING its anchor text exists. If upstream moved the code, the script stops and names the patch.

The default (Real = double) build must remain bit-identical to upstream: bench/render_ref_* proves it.
"""
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Headers (and their .cpp) converted to Real. Left double ON PURPOSE: the NEON/lane headers (the firmware turns lanes off),
# table builders and recipes (init time), MaskTableGenerator (its double index API; see plan), curve/draw tables.
KEEP_DOUBLE = {
    "ClassicFilterLanes.h", "FilterDriveLanes.h", "OversamplerLanes.h", "FirKernel.h", "WavetableRecipe.h",
    "MaskTableGenerator.h", "ScanOrderTableSet.h", "CurveTable.h", "DrawEnvelope.h", "ScanOrder.h",
}

# Only in these files may a declaration stay `Wide`: oscillators, modulator phases and the pitch chain. Everywhere else
# (filters, envelopes, drives...) every double becomes Real, or the filters would mix float and double arithmetic.
WIDE_FILES = {
    "PhaseDistortionOscillator", "BitmaskOscillator", "WavetableOscillator", "PulseOscillator", "SubOscillator", "MinBlep",
    "Lfo", "Drifter", "StepModulator", "MacroOscillator", "ModalResonator", "OrganicRandom", "CathodeOscCore",
    "CathodeVoiceStage", "CathodeModulatedVoice", "CathodeVoiceEngine", "CathodeOutputStage", "SampleRateReducer",
}

# A `double` declaration whose identifier looks like one of these stays `Wide` (always double).
#   phase / inc / freq / Hz / sampleRate / wrap : accumulators and the pitch chain; float detunes low notes.
#   cents / mult : pitch multipliers built from them.   rate : sample-rate-like control values.
WIDE_RE = re.compile(r"(?i)(phase|^inc|inc_|incOs|freq|hz\b|hz[A-Z_]|samplerate|sampleRate|wrap|cents|^mult|mult_|detune|glide|^rate|fs_)")

# `double [const] [&*] ident` where ident may be qualified (`Class::name`): the rule looks at the LAST component.
DECL = re.compile(r"\bdouble\b(\s+const\b)?(\s*[&*](?:\s*const\b)?)?\s*([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_~][A-Za-z0-9_]*)*)?")
# `std::array<double, N> ident` / `std::vector<double> ident`: the element type follows the same rule as a scalar.
CONTAINER = re.compile(r"\b(array|vector)<double((?:\s*,[^>]*)?)>(\s+const\b)?(\s*[&*](?:\s*const\b)?)?\s*([A-Za-z_][A-Za-z0-9_]*)?")


def read(p):
    with open(p) as f:
        return f.read()


def write(p, s):
    with open(p, "w") as f:
        f.write(s)


def closure(inc, roots):
    seen, st = [], list(roots)
    while st:
        h = st.pop()
        if h in seen or not os.path.exists(f"{inc}/{h}"):
            continue
        seen.append(h)
        st += re.findall(r'#include <nemesis/dsp/([A-Za-z0-9_]+\.h)>', read(f"{inc}/{h}"))
    return seen


def prune(dst, roots):
    """Delete every header/source the CATHODE voice cannot reach (FX chain, master bus, table recipes for other
    machines...). Follows includes in headers AND in the matching .cpp files. Add a root to keep more."""
    inc = f"{dst}/include/nemesis/dsp"
    keep_h, st = set(), list(roots)
    keep_cpp = set()
    while st:
        h = st.pop()
        if h in keep_h or not os.path.exists(f"{inc}/{h}"):
            continue
        keep_h.add(h)
        st += re.findall(r'#include <nemesis/dsp/([A-Za-z0-9_]+\.h)>', read(f"{inc}/{h}"))
        cpp = f"{dst}/src/{h[:-2]}.cpp"
        if os.path.exists(cpp):
            keep_cpp.add(os.path.basename(cpp))
            st += re.findall(r'#include <nemesis/dsp/([A-Za-z0-9_]+\.h)>', read(cpp))
    dropped = 0
    for f in os.listdir(inc):
        if f.endswith(".h") and f not in keep_h:
            os.remove(f"{inc}/{f}")
            dropped += 1
    for f in os.listdir(f"{dst}/src"):
        if f.endswith(".cpp") and f not in keep_cpp:
            os.remove(f"{dst}/src/{f}")
            dropped += 1
    return sorted(keep_h), dropped


def convert_text(s, allow_wide):
    """double -> Real, or Wide where the declared identifier says so. Comments are left alone."""
    out = []
    for ln in s.split("\n"):
        code, sep, cmt = ln.partition("//")

        def sub(m):
            ident = m.group(3) or ""
            wide = bool(ident) and WIDE_RE.search(ident) is not None
            return ("Wide" if wide else "Real") + (m.group(1) or "") + (m.group(2) or "") + (" " + ident if ident else "")

        # keep original spacing: only rewrite the type token (and keep the identifier text in place)
        def sub2(m):
            ident = (m.group(3) or "").split("::")[-1]
            wide = allow_wide and bool(ident) and WIDE_RE.search(ident) is not None
            tok = "Wide" if wide else "Real"
            return m.group(0).replace("double", tok, 1)

        def sub3(m):
            ident = m.group(5) or ""
            wide = allow_wide and bool(ident) and WIDE_RE.search(ident) is not None
            return m.group(0).replace("double", "Wide" if wide else "Real", 1)

        code = CONTAINER.sub(sub3, code)          # first: array<double,N> name
        out.append(DECL.sub(sub2, code) + sep + cmt)
    return "\n".join(out)


def add_include(s, header):
    if f"nemesis/dsp/{header}" in s:
        return s
    m = re.search(r"#include\s+[<\"][^\n]+\n", s)
    inc = f"#include <nemesis/dsp/{header}>\n"
    return s[:m.start()] + inc + s[m.start():] if m else inc + s


class Patcher:
    def __init__(self, root):
        self.root = root
        self.log = []

    def edit(self, rel, name, pairs, once=True):
        p = os.path.join(self.root, rel)
        s = read(p)
        for old, new in pairs:
            if old not in s:
                sys.exit(f"PATCH FAILED  [{name}]  anchor not found in {rel}:\n---\n{old}\n---\n"
                         f"Upstream changed this code. Update tools/sync_dsp.py.")
            s = s.replace(old, new, 1 if once else -1)
        write(p, s)
        self.log.append(f"  {name:<34} {rel}")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    up, dst = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    if os.path.exists(dst):
        shutil.rmtree(dst)
    for sub in ("include", "src", "thirdparty"):
        shutil.copytree(f"{up}/{sub}", f"{dst}/{sub}", ignore=shutil.ignore_patterns("Convolution.*", "tests", ".clang-tidy"))
    inc = f"{dst}/include/nemesis/dsp"

    # the CATHODE voice, and the effects the firmware's FX section uses (docs/FX_ANALYSIS.md): the delay and reverb cores (not the
    # effect classes around them: they allocate every type), the amp and the bus compressor
    ROOTS = ["CathodeVoiceEngine.h", "CathodeOutputStage.h", "CathodeModulatedVoice.h", "DelayCore.h", "ReverbCore.h", "GampEffect.h", "CompEffect.h"]
    kept, dropped = prune(dst, ROOTS)
    print(f"kept {len(kept)} headers reachable from the CATHODE voice, dropped {dropped} files (FX chain, master bus, ...)")
    shutil.copy(f"{HERE}/fork/Real.h", f"{inc}/Real.h")          # after the prune: nothing upstream includes them
    shutil.copy(f"{HERE}/fork/FastMath.h", f"{inc}/FastMath.h")
    shutil.copy(f"{HERE}/fork/ForkConfig.h", f"{inc}/ForkConfig.h")
    heads = closure(inc, ROOTS)
    conv = [h for h in heads if h not in KEEP_DOUBLE]
    files = []
    for h in conv:
        files.append(f"include/nemesis/dsp/{h}")
        if os.path.exists(f"{dst}/src/{h[:-2]}.cpp"):
            files.append(f"src/{h[:-2]}.cpp")
    changed = 0
    for rel in files:
        p = f"{dst}/{rel}"
        s = read(p)
        n = convert_text(s, os.path.basename(rel).rsplit('.', 1)[0] in WIDE_FILES)
        if n != s:
            changed += 1
            n = add_include(n, "Real.h")
        write(p, n)
    print(f"converted {changed} of {len(files)} files in the CATHODE closure ({len(heads)} headers, {len(KEEP_DOUBLE)} kept double)")

    import fork_patches  # noqa: E402  (tools/fork_patches.py)
    P = Patcher(dst)
    fork_patches.apply(P)
    print("\n".join(["patches applied:"] + P.log))


if __name__ == "__main__":
    sys.path.insert(0, HERE)
    main()
