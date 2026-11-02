#ifndef __GFX_BOOTIMAGE_H
#define __GFX_BOOTIMAGE_H

#include <stddef.h>
#include <stdint.h>

#include "gfx/BootImageData.h"
#include "gfx/Canvas.h"

// The picture shown at power-on, from flash (tools/gen_boot_image.py makes the data): a few colours at a fraction of the panel's size, drawn enlarged by a whole number.
// Decoded from the top, a row at a time, into strips the size of the board's. Nothing is kept but the two rows the odds are read from, so an object fits on the stack.
namespace gfx {

// How long the picture stays. Once it has gone it stays gone, however long the tick counter runs (a compare alone would bring it back 24.8 days later).
class BootHold {
   public:
    explicit BootHold(uint32_t untilMs) : until_(untilMs) {}
    bool up(uint32_t nowMs, bool endIt) {
        if (up_ && (endIt || (int32_t)(nowMs - until_) >= 0)) up_ = false;
        return up_;
    }

   private:
    uint32_t until_;
    bool up_ = true;
};

class BootImage {
   public:
    // `data`: the coded pixels (the firmware's own by default; the tests also feed it a cut-off copy). Past the end it reads zeros, so bad data gives a bad picture and nothing worse.
    explicit BootImage(const uint8_t* data = kBootData, size_t size = sizeof(kBootData));
    // The next `rows` rows of the enlarged picture, in RGB565, full width, kWidth stride. One object goes down the picture once.
    void rows(uint16_t* strip, int rows);

   private:
    static constexpr int kColours = 1 << kBootBits;
    static_assert(kBootW * kBootZoom == kWidth && kBootH * kBootZoom == kHeight, "the picture must fill the panel");
    void decodeRow();
    int decodeBit(uint16_t& odds);
    uint8_t next() { return pos_ < size_ ? data_[pos_++] : 0; }

    const uint8_t* data_;
    size_t size_, pos_ = 0;
    uint32_t range_ = 0xFFFFFFFFu, code_ = 0;
    int repeat_ = 0, now_ = 0;                           // panel rows left of this source row; which of row_ is the current one
    uint16_t odds_[kColours * kColours * kColours];      // per left pixel, per pixel above, per node of an index's bit tree
    uint8_t row_[2][kBootW];
};

}  // namespace gfx

#endif
