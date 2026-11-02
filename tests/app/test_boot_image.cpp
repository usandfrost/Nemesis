// The boot image's decoder on the host: its pixels against the ones tools/gen_boot_image.py quantised (boot_image_ref.pgm), strip by strip as the board reads it.
// Run: ctest --test-dir tests/build
#include <stdio.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "gfx/BootImage.h"

using namespace gfx;

static int checks = 0, fails = 0;
static void ok(bool c, const std::string& m) {
    ++checks;
    if (!c) { ++fails; printf("FAIL %s\n", m.c_str()); }
}

// The P5 file: its header, then one byte per pixel, a palette index
static std::vector<uint8_t> readRef() {
    std::vector<uint8_t> px;
    FILE* f = fopen(DBOY_BOOT_REF, "rb");
    if (!f) return px;
    int w = 0, h = 0, max = 0;
    if (fscanf(f, "P5 %d %d %d", &w, &h, &max) == 3 && w == kBootW && h == kBootH) {
        fgetc(f);
        px.resize((size_t)w * h);
        if (fread(px.data(), 1, px.size(), f) != px.size()) px.clear();
    }
    fclose(f);
    return px;
}

// The whole picture through one object, `step` panel rows at a time
static std::vector<uint16_t> decodeAll(int step, const uint8_t* data = kBootData, size_t size = sizeof(kBootData)) {
    std::unique_ptr<BootImage> img(new BootImage(data, size));
    std::vector<uint16_t> out((size_t)kWidth * kHeight);
    for (int y = 0; y < kHeight; y += step) img->rows(out.data() + (size_t)y * kWidth, y + step > kHeight ? kHeight - y : step);
    return out;
}

int main() {
    const std::vector<uint8_t> ref = readRef();
    ok(ref.size() == (size_t)kBootW * kBootH, "boot_image_ref.pgm is there and has the data's size");
    if (ref.empty()) { printf("%d of %d checks failed\n", fails, checks); return 1; }

    // the round trip, as Lcd::showBootImage reads it: 20 rows at a time, each source pixel a block of the zoom's size
    const std::vector<uint16_t> frame = decodeAll(20);
    size_t wrong = 0;
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            wrong += frame[(size_t)y * kWidth + x] != kBootPalette[ref[(size_t)(y / kBootZoom) * kBootW + x / kBootZoom]];
    ok(wrong == 0, "every panel pixel is its source pixel's palette colour (" + std::to_string(wrong) + " wrong)");

    // the strip size must not matter, even one that cuts a block of rows in two
    ok(decodeAll(1) == frame, "row by row gives the same picture");
    ok(decodeAll(7) == frame, "7 rows at a time gives the same picture");

    // the picture uses every colour of the palette, and the palette is what the panel gets: black first (the sky)
    std::vector<int> used(1 << kBootBits, 0);
    for (uint8_t v : ref) { ok(v < (1 << kBootBits), "index in range"); used[v] = 1; }
    for (int c = 0; c < (1 << kBootBits); ++c) ok(used[c], "colour " + std::to_string(c) + " is used");
    ok(kBootPalette[0] == 0, "palette entry 0 is black");

    // the budget: the owner's ceiling is 200 KB, and the point is to stay far below it. A bigger picture has to be a decision, so this fails first
    ok(sizeof(kBootData) + sizeof(kBootPalette) <= 8 * 1024, "the picture is at most 8 KB (" + std::to_string(sizeof(kBootData) + sizeof(kBootPalette)) + " bytes)");
    // the object goes on the stack in Lcd::showBootImage, whose minimum is 16 KB (the linker script)
    ok(sizeof(BootImage) <= 12 * 1024, "the decoder is at most 12 KB of stack (" + std::to_string(sizeof(BootImage)) + " bytes)");

    // the hold: its time, a wrap of the tick counter during it, and it never comes back
    {
        BootHold h(2000);
        ok(h.up(0, false) && h.up(1999, false), "the picture stays until its time is up");
        ok(!h.up(2000, false), "and goes then");
        ok(!h.up(2000u + (1u << 31) + 5, false) && !h.up(0xFFFFFFFFu, false), "and does not come back when the tick is half a range away");
        BootHold k(2000);
        ok(!k.up(10, true), "a key ends it early");
        ok(!k.up(11, false), "and it stays ended");
        BootHold w(100);
        ok(w.up(0xFFFFFF00u, false), "a tick just before the wrap is before a time just after it");
        ok(!w.up(100, false), "and the time arrives after the wrap");
    }

    // cut-off or damaged data gives a wrong picture, and it stops at the end of the buffer (boot_image_test_asan checks that it does)
    const size_t half = sizeof(kBootData) / 2;
    std::vector<uint8_t> cut(kBootData, kBootData + half);
    const std::vector<uint16_t> a = decodeAll(20, cut.data(), cut.size());
    ok(a != frame, "half the data is not the whole picture");
    bool sameTop = true;
    for (int i = 0; i < kWidth * 40; ++i) sameTop = sameTop && a[(size_t)i] == frame[(size_t)i];
    ok(sameTop, "half the data still decodes its first rows");
    std::vector<uint8_t> bad(kBootData, kBootData + sizeof(kBootData));
    for (size_t i = 0; i < bad.size(); i += 7) bad[i] ^= 0x5A;
    ok(decodeAll(20, bad.data(), bad.size()) != frame, "damaged data does not pass for the picture");
    ok(decodeAll(20, nullptr, 0) != frame, "no data at all gives no picture");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
