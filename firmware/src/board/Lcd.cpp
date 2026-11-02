#include "board/Lcd.h"

#include <string.h>

#include "main.h"
#include "board/Nt35510Init.h"
#include "gfx/BootImage.h"
#include "perf/CycleCounter.h"

extern uint32_t SystemCoreClock;

namespace board {

// the strip being rasterised: 38 KB in DTCM (single-cycle, and the CPU is the only one touching it)
alignas(8) __attribute__((section(".dtcm_bss"))) static uint16_t gStrip[gfx::kWidth * 20];

#define LCD_DATA_PORT GPIOB
static inline void csLow() { LCD_CS_GPIO_Port->BSRR = (uint32_t)LCD_CS_Pin << 16U; }
static inline void csHigh() { LCD_CS_GPIO_Port->BSRR = LCD_CS_Pin; }
static inline void rsCmd() { LCD_RS_GPIO_Port->BSRR = (uint32_t)LCD_RS_Pin << 16U; }
static inline void rsData() { LCD_RS_GPIO_Port->BSRR = LCD_RS_Pin; }
static inline void strobe() {
    LCD_WR_GPIO_Port->BSRR = (uint32_t)LCD_WR_Pin << 16U;   // low
    LCD_WR_GPIO_Port->BSRR = LCD_WR_Pin;                    // high: the panel latches
}

void Lcd::command(uint16_t c) { rsCmd(); LCD_DATA_PORT->ODR = c; strobe(); }
void Lcd::data(uint16_t d) { rsData(); LCD_DATA_PORT->ODR = d; strobe(); }

void Lcd::backlight(bool on) {
    if (on) LCD_BL_GPIO_Port->BSRR = LCD_BL_Pin;
    else LCD_BL_GPIO_Port->BSRR = (uint32_t)LCD_BL_Pin << 16U;
}

void Lcd::sleep(bool off) {
    if (off) {
        backlight(false);
        csLow();
        command(0x2800);   // display off: a self-lit panel stops emitting, a backlit one stays black
        csHigh();
    } else {
        csLow();
        command(0x2900);   // display on
        csHigh();
        first_ = true;     // the panel kept its memory, but every tile is sent again so nothing stale can show
        backlight(true);
    }
}

void Lcd::init() {
    backlight(false);
    LCD_RD_GPIO_Port->BSRR = LCD_RD_Pin;
    LCD_WR_GPIO_Port->BSRR = LCD_WR_Pin;
    csLow();
    // the stock reset timing
    HAL_Delay(250);
    HAL_GPIO_WritePin(LCD_RESET_GPIO_Port, LCD_RESET_Pin, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(LCD_RESET_GPIO_Port, LCD_RESET_Pin, GPIO_PIN_SET);
    HAL_Delay(120);
    for (const uint16_t* p = kNt35510Init; *p != 0xFFFF;) {
        const uint16_t cmd = *p++, delay = *p++, n = *p++;
        command(cmd);
        for (int i = 0; i < n; ++i) data(*p++);
        if (delay) HAL_Delay(delay);
    }
    // landscape, as the stock firmware (LANDSCAPE_1: row/column exchange, bottom to top)
    command(0x3600);
    data(0x20 | 0x80);
    command(0x2900);   // display on
    // black, then the light
    window(0, 0, gfx::kWidth - 1, gfx::kHeight - 1);
    csLow();
    rsData();
    LCD_DATA_PORT->ODR = 0;
    for (uint32_t i = 0; i < (uint32_t)gfx::kWidth * gfx::kHeight; ++i) strobe();
    csHigh();
    backlight(true);
    first_ = true;
}

void Lcd::window(int x0, int y0, int x1, int y1) {
    csLow();
    command(0x2A00); data((uint16_t)(x0 >> 8));
    command(0x2A01); data((uint16_t)(x0 & 0xFF));
    command(0x2A02); data((uint16_t)(x1 >> 8));
    command(0x2A03); data((uint16_t)(x1 & 0xFF));
    command(0x2B00); data((uint16_t)(y0 >> 8));
    command(0x2B01); data((uint16_t)(y0 & 0xFF));
    command(0x2B02); data((uint16_t)(y1 >> 8));
    command(0x2B03); data((uint16_t)(y1 & 0xFF));
    command(0x2C00);   // memory write
    rsData();
    csHigh();
}

void Lcd::sendTile(const uint16_t* strip, int tx, int rows) {
    csLow();
    rsData();
    volatile uint32_t* odr = &LCD_DATA_PORT->ODR;
    volatile uint32_t* wr = &LCD_WR_GPIO_Port->BSRR;
    const uint32_t wrLow = (uint32_t)LCD_WR_Pin << 16U, wrHigh = LCD_WR_Pin;
    for (int r = 0; r < rows; ++r) {
        const uint16_t* p = strip + r * gfx::kWidth + tx * kTileW;
        for (int i = 0; i < kTileW; ++i) {
            *odr = p[i];
            *wr = wrLow;
            *wr = wrHigh;
        }
    }
    csHigh();
}

void Lcd::showBootImage() {
    const uint32_t t0 = perf::cycles();
    gfx::BootImage img;
    for (int s = 0; s < kStrips; ++s) {
        img.rows(gStrip, kStripRows);
        for (int tx = 0; tx < kTilesX; ++tx) {
            window(tx * kTileW, s * kStripRows, tx * kTileW + kTileW - 1, s * kStripRows + kStripRows - 1);
            sendTile(gStrip, tx, kStripRows);
        }
    }
    first_ = true;   // the tile hashes do not know this picture: the next present() sends every tile
    lastPresentUs = (uint32_t)((uint64_t)(perf::cycles() - t0) * 1000000u / SystemCoreClock);
}

void Lcd::present(const gfx::Canvas& frame) {
    const uint32_t t0 = perf::cycles();
    tilesSent = 0;
    for (int s = 0; s < kStrips; ++s) {
        const int y0 = s * kStripRows;
        frame.raster(gStrip, y0, kStripRows);
        for (int tx = 0; tx < kTilesX; ++tx) {
            // FNV-1a over the tile, a word (two pixels) at a time
            uint32_t h = 2166136261u;
            for (int r = 0; r < kStripRows; ++r) {
                const uint16_t* px = gStrip + r * gfx::kWidth + tx * kTileW;
                for (int i = 0; i < kTileW; i += 2) {
                    uint32_t w;
                    memcpy(&w, px + i, 4);   // two pixels a word, without an aliasing cast
                    h = (h ^ w) * 16777619u;
                }
            }
            if (!first_ && h == hash_[s][tx]) continue;
            hash_[s][tx] = h;
            window(tx * kTileW, y0, tx * kTileW + kTileW - 1, y0 + kStripRows - 1);
            sendTile(gStrip, tx, kStripRows);
            ++tilesSent;
        }
    }
    first_ = false;
    lastPresentUs = (uint32_t)((uint64_t)(perf::cycles() - t0) * 1000000u / SystemCoreClock);
}

}  // namespace board
