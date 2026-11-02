#!/usr/bin/env python3
"""List libm call sites reachable from a root symbol, with the function that makes each call.
Functions that only run at prepare()/note-on/table-build time are annotated by name so the per-sample ones stand out."""
import argparse, re, subprocess, collections

ap = argparse.ArgumentParser()
ap.add_argument("elf"); ap.add_argument("--objdump", required=True)
ap.add_argument("--root", default="_ZN15CathodeRenderer6renderEPfS0_i")
a = ap.parse_args()
dis = subprocess.run([a.objdump, "-d", "--no-show-raw-insn", a.elf], capture_output=True, text=True, check=True).stdout
funcs, cur = {}, None
for line in dis.splitlines():
    m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
    if m: cur = m.group(1); funcs[cur] = []
    elif cur and line.startswith(" "): funcs[cur].append(line)
call = re.compile(r"\b(?:bl|b\.w)\s+[0-9a-f]+ <([^>+]+)")
unveneer = lambda n: n[2:-7] if n.startswith("__") and n.endswith("_veneer") else n   # calls into ITCM go through ld veneers
graph = {f: {unveneer(m.group(1)) for l in ls for m in [call.search(l)] if m} for f, ls in funcs.items()}
seen, st = set(), [a.root]
while st:
    f = st.pop()
    if f in seen or f not in funcs: continue
    seen.add(f); st.extend(graph[f])
libm = re.compile(r"^(sin|cos|tan|exp|exp2|expm1|log|log1p|log2|log10|pow|tanh|atan|atan2|sqrt|cbrt|fmod)f?$")
rows = collections.defaultdict(list)
for f in seen:
    for t in graph[f]:
        if libm.match(t): rows[f].append(t)
# demangle crudely
def nice(s):
    m = re.findall(r"\d+([A-Za-z_][A-Za-z0-9_]*)", s)
    return "::".join(m[:3]) if m else s
for f in sorted(rows, key=lambda x: -len(rows[x])):
    print(f"{len(rows[f]):3d}  {nice(f):<60} {' '.join(sorted(set(rows[f])))}")
