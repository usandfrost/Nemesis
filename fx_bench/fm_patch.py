#!/usr/bin/env python3
"""SCRATCH-ONLY experiment: in the converted copy of CompEffect.cpp (scratch tree, never in the repo) replace the per-sample std::log2 / std::exp2 by
the firmware's fm::log2 / fm::exp2 (firmware/dsp FastMath.h), to measure what the compressor costs once the libm calls are gone.
Usage: fm_patch.py TREE_DIR"""
import sys
p = sys.argv[1] + "/src/CompEffect.cpp"
s = open(p).read()
assert "std::exp2" in s and "std::log2" in s
s = s.replace("std::exp2", "nemesis::dsp::fm::exp2").replace("std::log2", "nemesis::dsp::fm::log2")
s = s.replace("#include", "#include <nemesis/dsp/FastMath.h>\n#include", 1)
open(p, "w").write(s)
print("patched", p)
