#include "gfx/BootImage.h"

#include <string.h>

namespace gfx {

BootImage::BootImage(const uint8_t* data, size_t size) : data_(data), size_(size) {
    for (uint16_t& o : odds_) o = 2048;
    memset(row_, 0, sizeof(row_));
    for (int i = 0; i < 4; ++i) code_ = (code_ << 8) | next();
}

// One binary decision of the range coder (the encoder is in tools/gen_boot_image.py). `odds` is the chance of a 0 in 1/4096, and it learns from each answer.
int BootImage::decodeBit(uint16_t& odds) {
    const uint32_t bound = (range_ >> 12) * odds;
    int bit;
    if (code_ < bound) {
        range_ = bound;
        odds = (uint16_t)(odds + ((4096 - odds) >> 4));
        bit = 0;
    } else {
        code_ -= bound;
        range_ -= bound;
        odds = (uint16_t)(odds - (odds >> 4));
        bit = 1;
    }
    while (range_ < (1u << 24)) {
        range_ <<= 8;
        code_ = (code_ << 8) | next();
    }
    return bit;
}

void BootImage::decodeRow() {
    const uint8_t* above = row_[now_];
    now_ ^= 1;
    uint8_t* row = row_[now_];
    int left = 0;
    for (int x = 0; x < kBootW; ++x) {
        uint16_t* tree = odds_ + (((left << kBootBits) | above[x]) << kBootBits);
        int node = 1;
        while (node < kColours) node = node * 2 + decodeBit(tree[node]);
        row[x] = (uint8_t)(left = node - kColours);
    }
}

void BootImage::rows(uint16_t* strip, int rows) {
    for (int r = 0; r < rows; ++r) {
        if (repeat_ == 0) {
            decodeRow();
            repeat_ = kBootZoom;
        }
        --repeat_;
        uint16_t* out = strip + r * kWidth;
        const uint8_t* row = row_[now_];
        for (int x = 0; x < kBootW; ++x)
            for (int z = 0; z < kBootZoom; ++z) *out++ = kBootPalette[row[x]];
    }
}

}  // namespace gfx
