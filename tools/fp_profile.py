#!/usr/bin/env python3
"""Static floating-point profile of the audio render path in a firmware ELF.

Walks the call graph from a root symbol (default: CathodeRenderer::render) over direct bl/b.w calls and
counts, across every reachable function, the single- vs double-precision FPU instructions and the calls to
libm / libgcc double routines. It is a proxy for M7 cost: .f32 ops are single-cycle throughput, .f64 ops are
several times slower, and a libm call is tens to hundreds of cycles. It does not weigh by loop trip counts.
"""
import argparse
import collections
import re
import subprocess
import sys


def unveneer(name):
    """`__X_veneer` (a GNU ld long-branch stub) -> X."""
    return name[2:-7] if name.startswith("__") and name.endswith("_veneer") else name


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--root", default="_ZN15CathodeRenderer6renderEPfS0_i")
    ap.add_argument("--top", type=int, default=12)
    a = ap.parse_args()

    dis = subprocess.run([a.objdump, "-d", "--no-show-raw-insn", a.elf], capture_output=True, text=True, check=True).stdout
    funcs, cur = {}, None
    for line in dis.splitlines():
        m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
        if m:
            cur = m.group(1)
            funcs[cur] = []
        elif cur and line.startswith(" "):
            funcs[cur].append(line.split("\t", 1)[1].strip() if "\t" in line else line.strip())

    def callees(name):
        out = set()
        for ins in funcs.get(name, []):
            m = re.match(r"(?:bl|b\.w|b)\s+[0-9a-f]+ <(.+?)(?:\+0x[0-9a-f]+)?>$", ins)
            t = unveneer(m.group(1)) if m else None   # a call into ITCM goes through a linker veneer: follow it to the function
            if t and t in funcs and t != name:
                out.add(t)
        return out

    seen, stack = set(), [a.root]
    if a.root not in funcs:
        sys.exit(f"root {a.root} not found")
    while stack:
        f = stack.pop()
        if f in seen:
            continue
        seen.add(f)
        stack.extend(callees(f))

    f32 = f64 = 0
    per_fn = collections.Counter()
    libcalls = collections.Counter()
    for f in seen:
        for ins in funcs[f]:
            op = ins.split()[0] if ins else ""
            if re.match(r"v(add|sub|mul|div|mla|mls|nmul|nmla|nmls|fma|fms|fnma|fnms|sqrt|cmp|cmpe|cvt|abs|neg|sel|max|min|rint[a-z]?)\.f32", op):
                f32 += 1
            elif re.match(r"v(add|sub|mul|div|mla|mls|nmul|nmla|nmls|fma|fms|fnma|fnms|sqrt|cmp|cmpe|cvt|abs|neg|sel|max|min|rint[a-z]?)\.f64", op) or op.endswith(".f64"):
                f64 += 1
                per_fn[f] += 1
            m = re.match(r"(?:bl|b\.w|b)\s+[0-9a-f]+ <(.+?)(?:\+0x[0-9a-f]+)?>$", ins)
            if m:
                t = m.group(1)
                if t not in funcs or re.match(r"^(sin|cos|tan|exp|exp2|log|log2|log10|pow|tanh|atan|atan2|sqrt|floor|ceil|fmod|round|fabs|cbrt|__aeabi_d|__aeabi_f|__aeabi_l2d|__aeabi_i2d|__aeabi_ui2d)[a-z0-9_]*f?$", t):
                    libcalls[t] += 1
    print(f"root: {a.root}")
    print(f"functions reachable: {len(seen)}")
    print(f"FPU single-precision instructions: {f32}")
    print(f"FPU double-precision instructions: {f64}")
    print("libm / libgcc calls (static call sites):")
    for k, v in libcalls.most_common(a.top):
        print(f"  {v:4d}  {k}")
    print("functions with most double-precision instructions:")
    for k, v in per_fn.most_common(a.top):
        print(f"  {v:4d}  {k[:100]}")


main()
