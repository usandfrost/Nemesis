#!/usr/bin/env python3
"""The NEMESIS logo of the website: web/www/img/logo-*.png and icon.png. It is drawn here from scratch, for the title screen, where it moves over a moving background. Each size is rendered at
its own resolution, not scaled: polygons with no anti-aliasing, a palette of a few colours, and an alpha that is 0 or 255, so there is no edge to fringe. The page draws it 1:1 on a canvas it
scales up by a whole number. The PNGs are committed, so nothing here runs in a normal build. Only this script needs Pillow and numpy:
    python3 -m venv /tmp/v && /tmp/v/bin/pip install pillow numpy && /tmp/v/bin/python web/tools/gen_logo.py"""
import os
import numpy as np
from PIL import Image, ImageDraw
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = f"{ROOT}/web/www/img"
HEX = {"pale": "dfeafa", "ice": "b6cce4", "mid": "7fa6d0", "steel": "4f78b0", "deep": "2c4688", "white": "fffaf0", "peach": "ffc884", "orange": "ff8a30", "ember": "e0562a", "red": "b5301f",
       "violet": "6a1e9c", "plum": "3c1478", "navy": "1a1058", "line": "0a0628"}
RGB = {k: tuple(int(v[i:i + 2], 16) for i in (0, 2, 4)) for k, v in HEX.items()}
# The letters, on a cap height of 100: N, M and I stand taller than the rest, with spikes above and below. All of them are one polygon: none has a closed counter
LETTERS = {
    "N": (80, [(0, 0), (19, 15), (60, 62), (60, 22), (80, 0), (80, 104), (60, 86), (19, 38), (19, 84), (0, 100)]),
    "E": (64, [(0, 14), (64, 14), (53, 30), (19, 30), (19, 42), (53, 42), (45, 56), (19, 56), (19, 70), (53, 70), (64, 86), (0, 86)]),
    "M": (104, [(0, 0), (52, 46), (104, 0), (104, 100), (85, 84), (85, 34), (52, 66), (19, 34), (19, 84), (0, 100)]),
    "S": (68, [(12, 14), (68, 14), (57, 30), (23, 30), (23, 41), (56, 41), (68, 53), (68, 74), (56, 86), (0, 86), (11, 70), (45, 70), (45, 58), (12, 58), (0, 46), (0, 26)]),
    "I": (22, [(0, 18), (22, -4), (22, 84), (0, 104)]),
}
# where the light catches: letter, index in the word, a corner of it
GLINTS = [(0, 0), (2, 2), (5, 1), (3, 1)]
GAP = 7
TALL = 1.3   # the letters are drawn taller than their polygons: the depth then has room under the arms of E and S
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0
# the chrome, top to bottom: sky, the bright horizon, the sunset, and the sky again in the lower half
STOPS = [(0.00, "pale"), (0.09, "ice"), (0.20, "mid"), (0.32, "steel"), (0.45, "deep"), (0.485, "peach"), (0.50, "white"), (0.535, "white"), (0.55, "peach"), (0.61, "orange"), (0.70, "ember"), (0.79, "red"),
         (0.83, "plum"), (0.87, "steel"), (0.95, "mid"), (1.00, "ice")]

def shift(m, dx, dy):
    """The mask moved by whole pixels, with nothing wrapping round."""
    out = np.zeros_like(m)
    h, w = m.shape
    sy0, sy1 = max(0, -dy), min(h, h - dy)
    sx0, sx1 = max(0, -dx), min(w, w - dx)
    out[sy0 + dy:sy1 + dy, sx0 + dx:sx1 + dx] = m[sy0:sy1, sx0:sx1]
    return out

def erode(m, n):
    for _ in range(n):
        m = m & shift(m, 1, 0) & shift(m, -1, 0) & shift(m, 0, 1) & shift(m, 0, -1)
    return m

def dilate(m):
    return m | shift(m, 1, 0) | shift(m, -1, 0) | shift(m, 0, 1) | shift(m, 0, -1)

def render(word, width, pad=7):
    total = sum(LETTERS[c][0] for c in word) + GAP * (len(word) - 1)
    s = (width - 2 * pad) / (total + 16)
    sy = s * TALL
    depth = max(3, round(11 * s))
    rim = max(1, round(3.4 * s))
    top = pad + round(5 * sy)
    h = top + round(112 * sy) + depth + pad
    # the faces, as hard polygons; each letter keeps its own mask for its own sheen
    face = np.zeros((h, width), bool)
    label = np.zeros((h, width), np.int16)
    x = pad
    tips = []
    for i, c in enumerate(word):
        w, poly = LETTERS[c]
        im = Image.new("1", (width, h), 0)
        pts = [(x + px * s, top + py * sy) for px, py in poly]
        ImageDraw.Draw(im).polygon(pts, fill=1)
        m = np.array(im, bool)
        face |= m
        label[m] = i + 1
        tips.append(pts)
        x += (w + GAP) * s
    # the depth: the faces pushed down and a little right, ending in a hot line
    solid = face.copy()
    for k in range(1, depth + 1):
        solid |= shift(face, round(k * 0.42), k)
    side = solid & ~face
    y0, y1 = top, top + round(100 * sy)
    # chrome by rows, dithered between its stops, so the whole word shares one horizon
    names = np.empty((h, width), dtype=object)
    for y in range(h):
        v = min(1.0, max(0.0, (y - y0) / max(1, y1 - y0)))
        for x_ in range(width):
            if not face[y, x_]: continue
            for a, b in zip(STOPS, STOPS[1:]):
                if v <= b[0]:
                    t = (v - a[0]) / max(1e-6, b[0] - a[0])
                    names[y, x_] = b[1] if t > BAYER[y % 4, x_ % 4] else a[1]
                    break
    # sheen: slanting bands of reflected light in the sky of each letter, a dark one below the horizon
    lighter = {"deep": "steel", "steel": "mid", "mid": "ice", "ice": "pale", "pale": "white"}
    period = max(12, round(46 * s))
    for y, x_ in zip(*np.nonzero(face)):
        v = (y - y0) / max(1, y1 - y0)
        d = (x_ + y * 1.1 + label[y, x_] * 13) % period
        if v < 0.46 and d < period * 0.26:
            n = names[y, x_]
            names[y, x_] = lighter.get(lighter.get(n, n), lighter.get(n, n))
        elif 0.86 < v and d < period * 0.2:
            names[y, x_] = lighter.get(names[y, x_], names[y, x_])
    # the chamfer: a slanted edge that catches the light on the top left and falls away on the bottom right
    inner = erode(face, rim)
    tl = face & ~shift(face, rim, rim) & ~inner
    br = face & ~shift(face, -rim, -rim) & ~inner
    for y, x_ in zip(*np.nonzero(br)):
        v = (y - y0) / max(1, y1 - y0)
        names[y, x_] = "plum" if v < 0.46 else "red" if v < 0.8 else "deep"
    for y, x_ in zip(*np.nonzero(tl)):
        v = (y - y0) / max(1, y1 - y0)
        names[y, x_] = "pale" if v < 0.46 else "white" if v < 0.55 else "peach" if v < 0.8 else "ice"
    # a hard white line along the very top and left of each face
    names[face & ~shift(face, 1, 1)] = "white"
    # the side: purple, with lines across it like the original's, and the hot line at its foot
    for y, x_ in zip(*np.nonzero(side)):
        names[y, x_] = "violet" if (y % 4) < 2 else "plum"
    foot = side & ~shift(solid, 0, -1)
    names[foot] = "orange"
    names[side & shift(face, 0, 1)] = "navy"
    # a line around the whole shape, so it stands off anything that moves behind it. It is part of the sprite, opaque, not a glow
    ring = dilate(solid) & ~solid
    names[ring] = "line"
    alpha = solid | ring
    # glints: four-pointed, opaque, a few pixels long
    arm = max(2, round(7 * s))
    for letter, corner in GLINTS:
        if letter >= len(tips): continue
        gx, gy = tips[letter][corner % len(tips[letter])]
        gx, gy = int(round(gx)) + 1, int(round(gy)) + 1
        for d in range(-arm, arm + 1):
            for px, py in ((gx + d, gy), (gx, gy + d)):
                if 0 <= px < width and 0 <= py < h:
                    names[py, px] = "white" if abs(d) <= arm // 2 else "ice"
                    alpha[py, px] = True
    out = np.zeros((h, width, 4), np.uint8)
    for y, x_ in zip(*np.nonzero(alpha)):
        out[y, x_] = (*RGB[names[y, x_]], 255)
    # the sprite, tight to what it draws
    ys, xs = np.nonzero(alpha)
    return out[ys.min():ys.max() + 1, xs.min():xs.max() + 1]

def save(a, name):
    alpha = set(np.unique(a[..., 3]).tolist())
    assert alpha <= {0, 255}, "the alpha must be hard"
    assert not a[a[..., 3] == 0].any(), "transparent pixels hold no colour"
    colours = {tuple(p) for p in a[a[..., 3] == 255][:, :3].tolist()}
    assert len(colours) <= 16, "the palette has grown"
    Image.fromarray(a, "RGBA").save(f"{OUT}/{name}", optimize=True)
    print(name, a.shape[1], "x", a.shape[0], len(colours), "colours", os.path.getsize(f"{OUT}/{name}"), "bytes")

for w in (260, 340, 440):
    save(render("NEMESIS", w), f"logo-{w}.png")
save(render("N", 44, pad=2), "icon.png")
