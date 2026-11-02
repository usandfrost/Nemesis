#ifndef __BOARD_INPUT_H
#define __BOARD_INPUT_H

#include <stdint.h>

#include "app/Ui.h"

// The panel's controls: 42 keys on three MCP23017 port expanders (I2C3; each pulls an interrupt line when a key changes) and eight
// quadrature encoders on interrupt pins. Key bits and expander set-up are the stock firmware's (Controller.cpp keyboard_*), the encoder
// decoding is its direction-confirming reader (encoder_read). Everything below the interrupt flags runs in the main loop.
namespace board {

class Input {
   public:
    void init();
    // Reads what changed and hands it to the UI (key presses and releases, layer keys, encoder detents). Main loop.
    void poll(app::Ui& ui);

    // Screen saver (board/Lcd.h sleep): the time of the last real key or knob action, and a "dark" mode in which the first action only asks to wake the
    // screen and is not delivered to the UI (a key pressed to wake the board must not also do what it is labelled with).
    uint32_t activityMs() const { return activityMs_; }
    void setScreenDark(bool dark) { dark_ = dark; wakeAsked_ = false; }
    bool wakeAsked() const { return wakeAsked_; }

    // from HAL_GPIO_EXTI_Callback
    void onKeypadInterrupt(int pad) { pending_[pad] = 1; }
    void onEncoderInterrupt(int enc);

    // Tuning that needs the board in hand (docs/BRINGUP.md): quadrature events per detent, which encoder is which knob, direction.
    static constexpr int kEventsPerDetent = 2;
    static const uint8_t kKnobOfEncoder[8];   // encoder number -> knob (0..3 T1..T4, 4..7 B1..B4)
    static const int8_t kDirection[8];        // +1 or -1

   private:
    bool readPad(int pad, uint16_t& state);
    enum Pad { Layer = 0, Left = 1, Right = 2 };
    volatile uint8_t pending_[3] = {1, 1, 1};
    uint16_t state_[3] = {0xFFFF, 0xFFFF, 0xFFFF};   // active low: 1 = up
    uint32_t pendingSince_[3] = {};
    bool armed_[3] = {};            // an interrupt was seen; pendingSince_ is when
    int failures_ = 0;              // consecutive failed reads: the bus is reset after a few (the stock keyboard_resetI2C)
    uint32_t lastPoll_ = 0;
    uint32_t activityMs_ = 0;
    bool dark_ = false, wakeAsked_ = false;
    bool ok_[3] = {};
    // encoders: decoded in the interrupt (a slow main loop must not lose edges); the main loop collects whole detents
    volatile int16_t detents_[8] = {};
    uint8_t abPrev_[8] = {};
    uint32_t lastCycles_[8] = {};
    int8_t lastDir_[8] = {};
    uint8_t reversals_[8] = {};
    int8_t count_[8] = {};
};

extern Input input;

}  // namespace board

#endif
