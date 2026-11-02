#!/usr/bin/env python3
"""Spectral comparison of two renders, per patch. Insensitive to a few-ppm pitch offset (a sample-aligned SNR is not).

usage: compare_spec.py ref.f32 test.f32 names.txt

For each patch (44100 stereo frames, left channel used): Hann-windowed 4096-pt frames, hop 4096, from 0.1 s on.
  worst  = largest |20 log10(|test|/|ref|)| over bins whose reference level is within 60 dB of that frame's peak
  rms    = rms of the same difference over those bins
  floor  = level change of the quiet part: energy (dB) in bins more than 60 dB below the peak, test minus ref
           (aliasing / noise from lower precision shows up here)
  nan    = non-finite sample count
Pure python (no numpy): an iterative radix-2 FFT.
"""
import cmath
import math
import struct
import sys

N = 4096
FRAMES = 44100
HANN = [0.5 - 0.5 * math.cos(2 * math.pi * i / N) for i in range(N)]


def fft(x):
    n = len(x)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            x[i], x[j] = x[j], x[i]
    size = 2
    while size <= n:
        half = size // 2
        w = cmath.exp(-2j * math.pi / size)
        for start in range(0, n, size):
            wk = 1.0
            for k in range(half):
                a = x[start + k]
                b = x[start + k + half] * wk
                x[start + k] = a + b
                x[start + k + half] = a - b
                wk *= w
        size *= 2
    return x


def spectrum(samples, start):
    seg = [samples[start + i] * HANN[i] for i in range(N)]
    s = fft([complex(v, 0) for v in seg])
    return [abs(s[k]) for k in range(N // 2)]


def main():
    ref, tst, names = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read(), [l.split()[0] for l in open(sys.argv[3]) if l.strip()]
    n = FRAMES * 2
    print(f"{'patch':<16} {'worst dB':>9} {'rms dB':>8} {'floor dB':>9} {'nan':>5}")
    worst_all = 0.0
    for i, name in enumerate(names):
        a = struct.unpack_from(f"<{n}f", ref, 4 * n * i)[0::2]
        b = struct.unpack_from(f"<{n}f", tst, 4 * n * i)[0::2]
        bad = sum(1 for v in b if not math.isfinite(v))
        b = [v if math.isfinite(v) else 0.0 for v in b]
        worst = 0.0
        sq = 0.0
        cnt = 0
        qa = qb = 0.0
        for start in range(4410, FRAMES - N, N):
            A, B = spectrum(a, start), spectrum(b, start)
            peak = max(A)
            if peak <= 0:
                continue
            thr, lo = peak * 10 ** (-60 / 20), peak * 10 ** (-90 / 20)
            for ka, kb in zip(A, B):
                if ka >= thr:
                    d = 20 * math.log10(max(kb, 1e-30) / ka)
                    worst = max(worst, abs(d))
                    sq += d * d
                    cnt += 1
                elif ka >= lo:
                    qa += ka * ka
                    qb += kb * kb
        rms = math.sqrt(sq / cnt) if cnt else 0.0
        floor = 10 * math.log10(qb / qa) if qa > 0 and qb > 0 else 0.0
        worst_all = max(worst_all, worst)
        print(f"{name:<16} {worst:9.3f} {rms:8.4f} {floor:+9.2f} {bad:5d}")
    print(f"worst bin error over all patches: {worst_all:.3f} dB")


main()
