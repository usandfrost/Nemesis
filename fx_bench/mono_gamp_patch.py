#!/usr/bin/env python3
"""SCRATCH-ONLY experiment: patch the converted copy of GampEffect.cpp (in the scratch tree, never in the repo) so that the right channel is
skipped when GAMP_MONO is defined (wr = wl). Measures what a mono-sum amp insert would cost. Usage: mono_gamp_patch.py TREE_DIR"""
import sys
p = sys.argv[1] + "/src/GampEffect.cpp"
s = open(p).read()
def rep(old, new):
    global s
    assert old in s, old
    s = s.replace(old, new, 1)
rep("""            float wr = cr.os[mi].processSample (xr * gInput_.cur,
                [this, &cr] (const float u) { return core (cr, u); });""",
"""#ifdef GAMP_MONO
            float wr = 0;
#else
            float wr = cr.os[mi].processSample (xr * gInput_.cur,
                [this, &cr] (const float u) { return core (cr, u); });
#endif""")
rep("            wr = cr.dcBlock.hp (wr);", "#ifndef GAMP_MONO\n            wr = cr.dcBlock.hp (wr);\n#endif")
rep("            wr += gPresence_.cur * cr.presenceSvf.processSample (wr).hp;", "#ifndef GAMP_MONO\n            wr += gPresence_.cur * cr.presenceSvf.processSample (wr).hp;\n#endif")
rep("            wr = cabinet (cr, wr);", "#ifdef GAMP_MONO\n            wr = wl;\n#else\n            wr = cabinet (cr, wr);\n#endif")
open(p, "w").write(s)
print("patched", p)
