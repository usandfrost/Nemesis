// The boot image through the firmware's decoder, as a PNG at the panel's size, to look at. No image library: the PNG's data is stored, not compressed, so the file is big.
//   render_boot out.png
#include <stdio.h>

#include <memory>
#include <vector>

#include "gfx/BootImage.h"

static uint32_t crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}
static void be32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s)); }
static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> v;
    be32(v, (uint32_t)data.size());
    fwrite(v.data(), 1, 4, f);
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    fwrite(body.data(), 1, body.size(), f);
    v.clear();
    be32(v, ~crc(body.data(), body.size()));
    fwrite(v.data(), 1, 4, f);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: render_boot out.png\n"); return 2; }
    using namespace gfx;
    std::unique_ptr<BootImage> img(new BootImage);
    std::vector<uint16_t> strip((size_t)kWidth * 20);
    std::vector<uint8_t> raw;
    for (int y = 0; y < kHeight; y += 20) {
        img->rows(strip.data(), 20);
        for (int r = 0; r < 20; ++r) {
            raw.push_back(0);   // the row's filter: none
            for (int x = 0; x < kWidth; ++x) {
                const uint16_t p = strip[(size_t)r * kWidth + x];
                raw.push_back((uint8_t)(((p >> 11) & 31) * 255 / 31));
                raw.push_back((uint8_t)(((p >> 5) & 63) * 255 / 63));
                raw.push_back((uint8_t)((p & 31) * 255 / 31));
            }
        }
    }
    // zlib stream of stored blocks
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t b : raw) { s1 = (s1 + b) % 65521; s2 = (s2 + s1) % 65521; }
    for (size_t at = 0; at < raw.size();) {
        const size_t n = raw.size() - at > 65535 ? 65535 : raw.size() - at;
        z.push_back(at + n == raw.size());
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8)); z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + (long)at, raw.begin() + (long)(at + n));
        at += n;
    }
    be32(z, (s2 << 16) | s1);
    FILE* f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 1; }
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    be32(ihdr, (uint32_t)kWidth);
    be32(ihdr, (uint32_t)kHeight);
    for (int v : {8, 2, 0, 0, 0}) ihdr.push_back((uint8_t)v);
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
    return 0;
}
