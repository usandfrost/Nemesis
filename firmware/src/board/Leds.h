#ifndef __BOARD_LEDS_H
#define __BOARD_LEDS_H

#include <stdint.h>

#include "app/Ui.h"

// The ten layer-key LEDs (one colour each, one MCP23017 on I2C4) and the two level bars (two more). The mockup's coloured key lights
// become on/off: in STEP the lit steps are on, the cursor blinks and an open lock blinks fast; elsewhere the selected lane is on and a
// lane that plays flips its LED for a moment. Expander set-up as the stock firmware (led_initialize).
namespace board {

class Leds {
   public:
    void init();
    // Main loop; writes only what changed, at most every 20 ms. True when it took the peaks (the caller then clears them).
    bool update(app::Ui& ui, float peakL, float peakR);

   private:
    bool write(int dev, uint16_t bits);
    uint16_t layer_ = 0xFFFF, barL_ = 0xFFFF, barR_ = 0xFFFF;
    float levelL_ = 0, levelR_ = 0;
    uint32_t last_ = 0;
    bool ok_ = false;
    int failures_ = 0;
};

extern Leds leds;

}  // namespace board

#endif
