#ifndef __BOARD_LCD_H
#define __BOARD_LCD_H

#include <stdint.h>

#include "gfx/Canvas.h"

// The 960 x 540 NT35510 panel on its 16-bit parallel bus (port B = data, RS/WR/CS/RD/RESET/BL on their own pins), driven the way the
// stock firmware drives it: the start-up sequence and the bus strobes are the stock ones. On top: a presenter that rasterises a frame
// strip by strip into internal RAM and sends only the 32 x 20 tiles whose contents changed since the last frame.
namespace board {

class Lcd {
   public:
    void init();                                  // reset, start-up sequence, landscape, black, backlight on
    void present(const gfx::Canvas& frame);       // main loop
    void showBootImage();                         // once, at power-on: the picture in flash (gfx/BootImage.h). The panel keeps it until the next present()
    void backlight(bool on);
    // Screen saver: the panel's own display-off command (DISPOFF 0x2800 / DISPON 0x2900) and the backlight. Waking forces the whole next frame out again.
    void sleep(bool off);

    // what the last present() did, for the CPU meter and tuning
    uint32_t tilesSent = 0;
    uint32_t lastPresentUs = 0;

   private:
    void command(uint16_t c);
    void data(uint16_t d);
    void window(int x0, int y0, int x1, int y1);
    void sendTile(const uint16_t* strip, int tx, int rows);
    static constexpr int kStripRows = 20, kTileW = 32;
    static constexpr int kTilesX = gfx::kWidth / kTileW, kStrips = gfx::kHeight / kStripRows;
    uint32_t hash_[kStrips][kTilesX];
    bool first_ = true;
};

}  // namespace board

#endif
