#!/usr/bin/env python3
"""Compare a float-build render against the double reference, per patch.

usage: compare.py ref.f32 test.f32 names.txt
Reports SNR (dB, signal power vs error power), peak error, RMS levels, and NaN/Inf counts. Patches are
44100 stereo frames each, in the order render_ref prints them.
"""
import math
import struct
import sys

FRAMES = 44100
ref = open(sys.argv[1], "rb").read()
tst = open(sys.argv[2], "rb").read()
names = [l.split()[0] for l in open(sys.argv[3]) if l.strip()]
n = FRAMES * 2
assert len(ref) == len(tst) == 4 * n * len(names), (len(ref), len(tst), n, len(names))

print(f"{'patch':<16} {'SNR dB':>8} {'SNR 50ms':>9} {'peak err':>10} {'rms ref':>9} {'rms tst':>9} {'level dB':>9} {'nan/inf':>8}")
# 50 ms window starting 100 ms in: after the attack, before slow phase drift (a few ppm of pitch) has accumulated
W0, W1 = 2 * 4410, 2 * (4410 + 2205)
worst = 1e9
for i, name in enumerate(names):
    a = struct.unpack_from(f"<{n}f", ref, 4 * n * i)
    b = struct.unpack_from(f"<{n}f", tst, 4 * n * i)
    bad = sum(1 for x in b if not math.isfinite(x))
    sig = sum(x * x for x in a) / n
    err = sum((x - y) ** 2 for x, y in zip(a, b) if math.isfinite(y)) / n
    peak = max((abs(x - y) for x, y in zip(a, b) if math.isfinite(y)), default=float("nan"))
    rms_t = math.sqrt(sum(y * y for y in b if math.isfinite(y)) / n)
    snr = 10 * math.log10(sig / err) if err > 0 and sig > 0 else float("inf")
    lvl = 20 * math.log10(rms_t / math.sqrt(sig)) if sig > 0 and rms_t > 0 else float("-inf")
    wsig = sum(x * x for x in a[W0:W1])
    werr = sum((x - y) ** 2 for x, y in zip(a[W0:W1], b[W0:W1]) if math.isfinite(y))
    wsnr = 10 * math.log10(wsig / werr) if werr > 0 and wsig > 0 else float("inf")
    worst = min(worst, snr)
    print(f"{name:<16} {snr:8.1f} {wsnr:9.1f} {peak:10.4f} {math.sqrt(sig):9.4f} {rms_t:9.4f} {lvl:9.2f} {bad:8d}")
print(f"worst SNR: {worst:.1f} dB")
