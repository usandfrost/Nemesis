#!/usr/bin/env python3
"""Makes the local reference copy of the upstream checkout that the tools read (tools/sync_dsp.py, bench/, fx_bench/, content/tools/).

    make_upstream_view.py CHECKOUT ref/upstream

The copy has the upstream project's name taken out, the same way everywhere, so nothing in this repository has to spell it: folders
and code say `nemesis`, prose says `upstream`. Only the names change, so a build from the copy sounds like one from the checkout. The
name is read from the checkout's include folder. ref/ is not in git.
"""
import os, re, shutil, sys

TEXT = (".h", ".hpp", ".c", ".cc", ".cpp", ".mm", ".inc", ".cmake", ".txt", ".md", ".json", ".py", ".sh", ".tsv")
SKIP = {".git", "build", "Build", "logs", "node_modules"}


def neutral(s, name):
    """`name` out of a text: `nemesis` in code, `upstream` in prose."""
    cap, big = name.capitalize(), name.upper()
    for pat, new in (
        (rf"\b[Tt]he {name} CATHODE", "the original CATHODE"),
        (rf"\b{name} CATHODE", "CATHODE"),
        (rf"ref/{name}-main", "ref/upstream"),
        (rf"{name}-main", "upstream"),
        (rf"{name}_(ref|render|build)", r"upstream_\1"),
        (rf"(?<=[A-Z_]){len(name)}{name}", f"{len('nemesis')}nemesis"),   # (a mangled name counts its letters)
        (rf"{big}_", "NEMESIS_"),
        (rf"{name}(?=/|::|_|[A-Z])", "nemesis"),
        (rf"(?<=namespace ){name}", "nemesis"),
        (rf"(?<=[_/]){name}\b", "nemesis"),
        (rf"\b[Aa] {name}\b", "an upstream"),
        (rf"\b{name}\b", "upstream"),
        (rf"\b{cap}\b", "Upstream"),
        (rf"\b{big}\b", "UPSTREAM"),
        (cap, "Nemesis"),
        (r"\b[Uu]pstream upstream\b", "upstream"),
    ):
        s = re.sub(pat, new, s)
    return s


def project_name(src):
    for dsp in ("sdk/dsp", "dsp"):
        inc = f"{src}/{dsp}/include"
        if os.path.isdir(inc):
            return next(d for d in sorted(os.listdir(inc)) if os.path.isdir(f"{inc}/{d}"))
    raise SystemExit(f"{src} has no sdk/dsp/include or dsp/include")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    name = project_name(src)
    if os.path.exists(dst):
        shutil.rmtree(dst)
    count = 0
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in SKIP]
        rel = os.path.relpath(root, src)
        out = os.path.normpath(f"{dst}/{'/'.join('nemesis' if p == name else p for p in rel.split('/'))}")
        os.makedirs(out, exist_ok=True)
        for f in files:
            to = f"{out}/{neutral(f, name)}"
            if f.endswith(TEXT):
                with open(f"{root}/{f}", encoding="utf-8", errors="surrogateescape", newline="") as h:
                    t = h.read()
                with open(to, "w", encoding="utf-8", errors="surrogateescape", newline="") as h:
                    h.write(neutral(t, name))
            else:
                shutil.copy(f"{root}/{f}", to)
            count += 1
    print(f"{count} files in {dst}")


if __name__ == "__main__":
    main()
