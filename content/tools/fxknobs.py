#!/usr/bin/env python3
"""The Drumboy effects knobs, read from firmware/src/app/FxParams.cpp (never written).

fx_table()  -> {"DELAY": [Knob x8], "REVERB": ..., "CHORUS": ..., "COMP": ..., "AMP": ...}
Knob: name, unit, exp, a, b, default (real units; a list: index), items (list names or None)
real(k, v) / stored(k, real) / default_stored(k) replicate fxReal / the inverse of it / fxDefault.
"""
import math
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
FXPARAMS = os.path.join(REPO, "firmware", "src", "app", "FxParams.cpp")


class Knob:
    def __init__(self, name, unit, exp, a, b, default, items=None):
        self.name, self.unit, self.exp, self.a, self.b, self.default, self.items = name, unit, exp, a, b, default, items

    @property
    def is_list(self):
        return self.items is not None

    @property
    def vmax(self):
        return len(self.items) - 1 if self.items else 127

    def __repr__(self):
        return "Knob(%s)" % self.name


def _num(s):
    return float(s.strip())


def fx_table(path=FXPARAMS):
    src = open(path, encoding="utf-8").read()
    lists = {}
    for m in re.finditer(r"static const char\* const (\w+)\[\]\s*=\s*\{([^}]*)\};", src):
        lists[m.group(1)] = re.findall(r'"([^"]*)"', m.group(2))
    tab = {}
    for m in re.finditer(r"static const FxParamDef (k\w+)\[[^\]]*\]\s*=\s*\{(.*?)\};", src, re.S):
        name = m.group(1)
        knobs = []
        for c in re.finditer(r"(LIN|EXP|LST)\(([^()]*)\)", m.group(2)):
            kind, args = c.group(1), [a.strip() for a in c.group(2).split(",")]
            if kind == "LST":
                knobs.append(Knob(args[0].strip('"'), "List", False, 0, 0, _num(args[2]), lists[args[1]]))
            else:
                knobs.append(Knob(args[0].strip('"'), args[1], kind == "EXP", _num(args[2]), _num(args[3]), _num(args[4])))
        tab[name] = knobs
    out = {"DELAY": tab["kDelay"], "REVERB": tab["kReverb"], "CHORUS": tab["kChorus"], "COMP": tab["kComp"], "AMP": tab["kAmp"],
           "SENDS": tab["kSend"]}
    return out


def real(k, v):
    """fxReal"""
    if k.is_list:
        return v
    v = min(max(v, 0), 127)
    t = v / 127.0
    return k.a * (k.b / k.a) ** t if k.exp else k.a + (k.b - k.a) * t


def stored(k, r):
    """the inverse: the stored 0..127 (a list: the index) nearest to the real value, clamped (round half up like C++ floor(x+0.5))"""
    if k.is_list:
        return int(min(max(round(r), 0), k.vmax))
    r = min(max(r, min(k.a, k.b)), max(k.a, k.b))
    t = math.log(r / k.a) / math.log(k.b / k.a) if k.exp else (r - k.a) / (k.b - k.a)
    return int(min(max(math.floor(t * 127.0 + 0.5), 0), 127))


def default_stored(k):
    """fxDefault"""
    if k.is_list:
        return int(k.default)
    return stored(k, k.default)


def knob(table, view, name):
    for k in table[view]:
        if k.name == name:
            return k
    raise KeyError((view, name))


if __name__ == "__main__":
    t = fx_table()
    for v, ks in t.items():
        print(v, [(k.name, k.unit, "exp" if k.exp else "lin", k.a, k.b, k.default, default_stored(k)) for k in ks])
