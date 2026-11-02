#!/usr/bin/env python3
"""Makes the picture the firmware shows at power-on: firmware/src/gfx/BootImageData.h (the data) and tests/app/boot_image_ref.pgm (the same pixels, uncompressed,
for tests/app/test_boot_image.cpp). Both are committed, so a normal build needs none of this. Only this script needs Pillow and numpy:
    python3 -m venv /tmp/v && /tmp/v/bin/pip install pillow numpy && /tmp/v/bin/python tools/gen_boot_image.py

The image is scaled down, reduced to a palette and drawn enlarged by a whole number. Each pixel is then coded with a binary range coder (the LZMA kind) whose odds depend on
the pixels to its left and above: one odds table per pair of neighbours, one entry per node of the bit tree of a palette index. The decoder is firmware/src/gfx/BootImage.cpp.
    gen_boot_image.py [--src PNG] [--size WxH] [--palette RRGGBB,RRGGBB,...] [--png OUT.png]
--size must divide the panel (960 x 540) by a whole number. The palette holds 2 to 16 colours, chosen by hand: k-means merges the few colours that make chrome look like chrome."""
import argparse, os
import numpy as np
from PIL import Image
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
PANEL = (960, 540)
# black, the sky's two blues and purple, the steel, white, and the sunset's orange and red
PALETTE = "000000,1a1058,6a1e9c,4f78b0,b6cce4,fffaf0,ff8a30,c23a24"

def oklab(rgb):
    """Colour distance is measured in OKLab: a nearest colour in RGB turns the dark blues grey."""
    c = np.asarray(rgb, float) / 255.0
    l = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    r, g, b = l[..., 0], l[..., 1], l[..., 2]
    L = np.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b)
    M = np.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b)
    S = np.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b)
    return np.stack([0.2104542553 * L + 0.7936177850 * M - 0.0040720468 * S,
                     1.9779984951 * L - 2.4285922050 * M + 0.4505937099 * S,
                     0.0259040371 * L + 0.7827717662 * M - 0.8086757660 * S], -1)

def rgb565(c):
    """gfx::rgb565: rounded, not truncated."""
    return (((c[0] * 31 + 127) // 255) << 11) | (((c[1] * 63 + 127) // 255) << 5) | ((c[2] * 31 + 127) // 255)

def shown(p):
    """What the panel shows for an RGB565 value, as RGB: colours are matched against this, not the wish."""
    return ((p >> 11) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31)

def quantise(src, w, h, palette):
    """Scales down and picks the nearest palette colour (no dither: it costs bytes, and on 4x pixels it looks like noise). Palette index 0 is the darkest."""
    want = [tuple(int(h6[i:i + 2], 16) for i in (0, 2, 4)) for h6 in palette]
    p565 = [rgb565(c) for c in want]
    pal = sorted(p565, key=lambda p: oklab([shown(p)])[0, 0])
    lab = oklab(np.asarray(src.resize((w, h), Image.LANCZOS, reducing_gap=3)).astype(float))
    plab = oklab(np.array([shown(p) for p in pal], float))
    return ((lab.reshape(-1, 1, 3) - plab[None, :, :]) ** 2).sum(2).argmin(1).reshape(h, w).astype(np.uint8), pal

class RangeEncoder:
    def __init__(self):
        self.low, self.range, self.cache, self.pending, self.out = 0, 0xFFFFFFFF, 0, 1, bytearray()
    def shift(self):
        if self.low < 0xFF000000 or self.low >= 1 << 32:
            carry = self.low >> 32
            v = self.cache
            while self.pending:
                self.out.append((v + carry) & 255)
                v = 255
                self.pending -= 1
            self.cache = (self.low >> 24) & 255
        self.pending += 1
        self.low = (self.low & 0x00FFFFFF) << 8
    def bit(self, odds, i, b):
        bound = (self.range >> 12) * odds[i]
        if b:
            self.low += bound
            self.range -= bound
            odds[i] -= odds[i] >> 4
        else:
            self.range = bound
            odds[i] += (4096 - odds[i]) >> 4
        while self.range < 1 << 24:
            self.range = (self.range << 8) & 0xFFFFFFFF
            self.shift()
    def finish(self):
        for _ in range(5):
            self.shift()
        return bytes(self.out[1:]).rstrip(b"\0")   # the first byte is always 0, and the decoder reads zeros past the end

def encode(idx, bits):
    h, w = idx.shape
    n = 1 << bits
    odds = [2048] * (n * n * n)
    rc = RangeEncoder()
    px = idx.tolist()
    above = [0] * w
    for row in px:
        left = 0
        for x in range(w):
            v = row[x]
            base = ((left << bits) | above[x]) << bits
            node = 1
            for b in range(bits - 1, -1, -1):
                bit = (v >> b) & 1
                rc.bit(odds, base + node, bit)
                node = node * 2 + bit
            left = v
        above = row
    return rc.finish()

def array(name, kind, values, per_line, fmt):
    lines = [", ".join(fmt % v for v in values[i:i + per_line]) for i in range(0, len(values), per_line)]
    return "inline constexpr %s %s = {\n    %s,\n};\n" % (kind, name, ",\n    ".join(lines))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=f"{ROOT}/content/art/Nemesis.png")
    ap.add_argument("--size", default="240x135")
    ap.add_argument("--palette", default=PALETTE)
    ap.add_argument("--png")
    a = ap.parse_args()
    w, h = (int(v) for v in a.size.split("x"))
    zoom = PANEL[0] // w
    assert (w * zoom, h * zoom) == PANEL, "the size must divide the panel by a whole number"
    colours = a.palette.split(",")
    assert 2 <= len(colours) <= 16, "the decoder's odds grow with the cube of the palette"
    bits = 1
    while (1 << bits) < len(colours):
        bits += 1
    idx, pal = quantise(Image.open(a.src).convert("RGB"), w, h, colours)
    data = encode(idx, bits)
    pal += [0] * ((1 << bits) - len(pal))
    with open(f"{ROOT}/firmware/src/gfx/BootImageData.h", "w") as f:
        f.write("// GENERATED by tools/gen_boot_image.py. Do not edit.\n#ifndef __GFX_BOOTIMAGEDATA_H\n#define __GFX_BOOTIMAGEDATA_H\n\n#include <stdint.h>\n\nnamespace gfx {\n\n")
        f.write(f"inline constexpr int kBootW = {w}, kBootH = {h}, kBootZoom = {zoom}, kBootBits = {bits};\n")
        f.write(array("kBootPalette[1 << kBootBits]", "uint16_t", pal, 8, "0x%04X"))
        f.write(array(f"kBootData[{len(data)}]", "uint8_t", list(data), 24, "%d"))
        f.write("\n}  // namespace gfx\n\n#endif\n")
    with open(f"{ROOT}/tests/app/boot_image_ref.pgm", "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h) + idx.tobytes())
    print(f"{w} x {h} x {zoom}, {len(colours)} colours: {len(data)} bytes of data + {2 << bits} of palette")
    if a.png:
        rgb = np.array([shown(p) for p in pal], np.uint8)[idx]
        Image.fromarray(rgb).resize(PANEL, Image.NEAREST).save(a.png)

if __name__ == "__main__":
    main()
