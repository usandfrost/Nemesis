#include "board/Leds.h"

#include <math.h>

#include "i2c.h"
#include "main.h"

namespace board {

Leds leds;

using namespace app;

static const uint16_t kAddr[3] = {(0x20 | 0x22) << 1, (0x20 | 0x23) << 1, (0x20 | 0x24) << 1};   // layer keys, bar L, bar R
// the stock level bar: the number of lit segments for each threshold (16-bit peak), -44 dB .. -1 dB
static const uint16_t kBarThreshold[12] = {207, 519, 1036, 2067, 4125, 8231, 11626, 16422, 20675, 23197, 26028, 29193};

void Leds::init() {
    uint8_t out[2] = {0, 0}, pull[2] = {0xFF, 0xFF};
    ok_ = true;
    for (int d = 0; d < 3; ++d) {
        ok_ &= HAL_I2C_Mem_Write(&hi2c4, kAddr[d], 0x00, I2C_MEMADD_SIZE_8BIT, out, 2, 5) == HAL_OK;    // IODIR: outputs
        ok_ &= HAL_I2C_Mem_Write(&hi2c4, kAddr[d], 0x02, I2C_MEMADD_SIZE_8BIT, out, 2, 5) == HAL_OK;    // IPOL
        ok_ &= HAL_I2C_Mem_Write(&hi2c4, kAddr[d], 0x0C, I2C_MEMADD_SIZE_8BIT, pull, 2, 5) == HAL_OK;   // GPPU
        ok_ &= HAL_I2C_Mem_Write(&hi2c4, kAddr[d], 0x08, I2C_MEMADD_SIZE_8BIT, out, 2, 5) == HAL_OK;    // INTCON
        write(d, 0);
    }
    layer_ = barL_ = barR_ = 0;
}

bool Leds::write(int dev, uint16_t bits) {
    uint8_t b[2] = {(uint8_t)(bits & 0xFF), (uint8_t)(bits >> 8)};
    if (HAL_I2C_Mem_Write(&hi2c4, kAddr[dev], 0x12, I2C_MEMADD_SIZE_8BIT, b, 2, 3) == HAL_OK) { failures_ = 0; return true; }   // GPIOA, GPIOB
    if (++failures_ >= 3) {   // a stuck bus: reset the peripheral, as the stock firmware does for its keypads
        __HAL_RCC_I2C4_FORCE_RESET();
        HAL_Delay(1);
        __HAL_RCC_I2C4_RELEASE_RESET();
        HAL_I2C_DeInit(&hi2c4);
        MX_I2C4_Init();
        failures_ = 0;
    }
    return false;
}

bool Leds::update(Ui& ui, float peakL, float peakR) {
    if (!ok_) return true;
    const uint32_t ms = HAL_GetTick();
    if (ms - last_ < 20) return false;
    last_ = ms;
    const Transport& tp = ui.transport();
    const Lane& ln = ui.lane();
    const bool blink = (ms / 250) & 1, fast = (ms / 90) & 1;
    uint16_t bits = 0;
    if (ui.mode() == Mode::Step && !ui.tracker()) {   // (the tracker's keys are lanes)
        const int s0 = ui.stepPage() * 8;
        for (int i = 0; i < 8; ++i) {
            const int idx = s0 + i;
            if (idx >= ln.n) continue;
            bool on = trig(ln, idx, tp.fill);
            if (idx == ui.cursor()) on = on != blink;   // the cursor blinks
            if (ui.lockOpen() && ui.lockLane() == ui.sel() && ui.lockIdx() == idx) on = fast;
            if (on) bits |= (uint16_t)(1u << i);
        }
        if (ui.stepPage() > 0) bits |= 1u << 8;                     // there is a page before
        if (s0 + 8 < ln.n) bits |= 1u << 9;                         // and after
    } else {
        for (int i = 0; i < kLanes; ++i) {
            bool on = i == ui.sel();
            if (tp.playing && ms - tp.hitAtMs[i] < 80) on = !on;    // the lane played: a short flip
            if (on) bits |= (uint16_t)(1u << i);
        }
    }
    if (bits != layer_ && write(0, bits)) layer_ = bits;
    // the level bars: quick up, slower down
    auto segs = [](float peak, float& level) {
        level = peak > level ? peak : level * 0.85f;
        const int v = (int)(level * 32767.0f);
        int n = 0;
        while (n < 12 && v >= kBarThreshold[n]) ++n;
        return (uint16_t)((1u << n) - 1u);
    };
    const uint16_t l = segs(peakL, levelL_), r = segs(peakR, levelR_);
    if (l != barL_ && write(1, l)) barL_ = l;
    if (r != barR_ && write(2, r)) barR_ = r;
    return true;
}

}  // namespace board
