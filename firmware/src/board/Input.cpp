#include "board/Input.h"

#include "i2c.h"
#include "main.h"
#include "perf/CycleCounter.h"

extern uint32_t SystemCoreClock;

namespace board {

Input input;

using app::Key;

// encoder number -> knob, and direction: the stock pin names say encoder 0..7; which one sits where on the panel is to be checked on
// the board (docs/BRINGUP.md). Until then: 0..3 the top row left to right, 4..7 the bottom row.
const uint8_t Input::kKnobOfEncoder[8] = {0, 1, 2, 3, 4, 5, 6, 7};
const int8_t Input::kDirection[8] = {1, 1, 1, 1, 1, 1, 1, 1};

// I2C addresses (7-bit 0x20 | n, shifted for the HAL) and the key on each bit, from the stock firmware's enums (Global.h)
static const uint16_t kAddr[3] = {(0x20 | 0x25) << 1, (0x20 | 0x20) << 1, (0x20 | 0x21) << 1};
static const Key kLeftKeys[16] = {Key::Stop, Key::Play, Key::Rec, Key::Add, Key::Up, Key::Clear, Key::Left, Key::Ok,
                                  Key::Right, Key::Copy, Key::Down, Key::Paste, Key::Mute, Key::Fill, Key::StepK, Key::Spare};
//                                 RESET     PLAYSTOP   REC       ADD       UP       CLEAR      LEFT       CENTER
//                                 RIGHT     COPY       DOWN      PASTE     MUTE     FILL       STYLE      SONG (bottom left)
static const Key kRightKeys[16] = {Key::File, Key::PartK, Key::Mixer, Key::Osc1, Key::Osc2, Key::Sub, Key::Flt1, Key::Flt2,
                                   Key::Grit, Key::Amp, Key::ModK, Key::Glob, Key::Lane, Key::Info, Key::SongK, Key::Shift};
//                                 FILE       DRUMKIT     MIXER       RHYTHM      METRONOME   IO         FILTER_0    FILTER_1
//                                 EQ         EFFECT_0    EFFECT_1    REVERB      LFO         LAYER_INST LAYER_SONG  KNOB

// MCP23017 registers (IOCON.BANK = 0)
enum : uint8_t { MCP_IODIRA = 0x00, MCP_IPOLA = 0x02, MCP_GPINTENA = 0x04, MCP_DEFVALA = 0x06, MCP_INTCONA = 0x08, MCP_IOCONA = 0x0A, MCP_GPPUA = 0x0C, MCP_GPIOA = 0x12 };

// the encoder timing in raw cycle-counter ticks (a microsecond count derived from them would wrap every 7.8 s at 550 MHz)
static uint32_t usToCycles(uint32_t us) { return (uint32_t)((uint64_t)SystemCoreClock * us / 1000000u); }

// The stock recovery for a stuck keypad bus: reset the I2C3 peripheral through RCC, then set it up again.
static void resetI2C3() {
    __HAL_RCC_I2C3_FORCE_RESET();
    HAL_Delay(1);
    __HAL_RCC_I2C3_RELEASE_RESET();
    HAL_I2C_DeInit(&hi2c3);
    MX_I2C3_Init();
}

void Input::init() {
    // the stock set-up for each keypad: all inputs, pull-ups, interrupt on any change, INTA/INTB mirrored
    for (int p = 0; p < 3; ++p) {
        uint8_t all[2] = {0xFF, 0xFF}, none[2] = {0, 0}, iocon = 0x40;
        bool good = true;
        good &= HAL_I2C_Mem_Write(&hi2c3, kAddr[p], MCP_IODIRA, I2C_MEMADD_SIZE_8BIT, all, 2, 5) == HAL_OK;
        good &= HAL_I2C_Mem_Write(&hi2c3, kAddr[p], MCP_IPOLA, I2C_MEMADD_SIZE_8BIT, none, 2, 5) == HAL_OK;
        good &= HAL_I2C_Mem_Write(&hi2c3, kAddr[p], MCP_GPPUA, I2C_MEMADD_SIZE_8BIT, all, 2, 5) == HAL_OK;
        good &= HAL_I2C_Mem_Write(&hi2c3, kAddr[p], MCP_DEFVALA, I2C_MEMADD_SIZE_8BIT, all, 2, 5) == HAL_OK;
        good &= HAL_I2C_Mem_Write(&hi2c3, kAddr[p], MCP_INTCONA, I2C_MEMADD_SIZE_8BIT, none, 2, 5) == HAL_OK;
        good &= HAL_I2C_Mem_Write(&hi2c3, kAddr[p], MCP_GPINTENA, I2C_MEMADD_SIZE_8BIT, all, 2, 5) == HAL_OK;
        good &= HAL_I2C_Mem_Write(&hi2c3, kAddr[p], MCP_IOCONA, I2C_MEMADD_SIZE_8BIT, &iocon, 1, 5) == HAL_OK;
        good &= HAL_I2C_Mem_Write(&hi2c3, kAddr[p], MCP_IOCONA + 1, I2C_MEMADD_SIZE_8BIT, &iocon, 1, 5) == HAL_OK;
        ok_[p] = good;
        uint16_t s;
        if (good && readPad(p, s)) state_[p] = s;   // what is held at power-on is not a press
        pending_[p] = 0;
    }
    static GPIO_TypeDef* const kAPort[8] = {ENCO_0A_GPIO_Port, ENCO_1A_GPIO_Port, ENCO_2A_GPIO_Port, ENCO_3A_GPIO_Port,
                                            ENCO_4A_GPIO_Port, ENCO_5A_GPIO_Port, ENCO_6A_GPIO_Port, ENCO_7A_GPIO_Port};
    static const uint16_t kAPin[8] = {ENCO_0A_Pin, ENCO_1A_Pin, ENCO_2A_Pin, ENCO_3A_Pin, ENCO_4A_Pin, ENCO_5A_Pin, ENCO_6A_Pin, ENCO_7A_Pin};
    static GPIO_TypeDef* const kBPort[8] = {ENCO_0B_GPIO_Port, ENCO_1B_GPIO_Port, ENCO_2B_GPIO_Port, ENCO_3B_GPIO_Port,
                                            ENCO_4B_GPIO_Port, ENCO_5B_GPIO_Port, ENCO_6B_GPIO_Port, ENCO_7B_GPIO_Port};
    static const uint16_t kBPin[8] = {ENCO_0B_Pin, ENCO_1B_Pin, ENCO_2B_Pin, ENCO_3B_Pin, ENCO_4B_Pin, ENCO_5B_Pin, ENCO_6B_Pin, ENCO_7B_Pin};
    for (int i = 0; i < 8; ++i) {
        const uint8_t a = (kAPort[i]->IDR & kAPin[i]) ? 2 : 0, b = (kBPort[i]->IDR & kBPin[i]) ? 1 : 0;
        abPrev_[i] = a | b;
        detents_[i] = 0;
    }
}

bool Input::readPad(int pad, uint16_t& state) {
    uint8_t b[2];
    // reading GPIOA/GPIOB also clears the expander's interrupt
    if (HAL_I2C_Mem_Read(&hi2c3, kAddr[pad], MCP_GPIOA, I2C_MEMADD_SIZE_8BIT, b, 2, 3) != HAL_OK) return false;
    state = (uint16_t)(b[0] | (b[1] << 8));
    return true;
}

// The stock reader, run in the encoder's interrupt: a step in the same direction counts at once, a reversal must be seen twice (kills
// the 15-14-15 jitter of a detent), edges closer than 700 us are bounce, and two seconds of rest forget the direction.
void Input::onEncoderInterrupt(int i) {
    static GPIO_TypeDef* const kAPort[8] = {ENCO_0A_GPIO_Port, ENCO_1A_GPIO_Port, ENCO_2A_GPIO_Port, ENCO_3A_GPIO_Port,
                                            ENCO_4A_GPIO_Port, ENCO_5A_GPIO_Port, ENCO_6A_GPIO_Port, ENCO_7A_GPIO_Port};
    static const uint16_t kAPin[8] = {ENCO_0A_Pin, ENCO_1A_Pin, ENCO_2A_Pin, ENCO_3A_Pin, ENCO_4A_Pin, ENCO_5A_Pin, ENCO_6A_Pin, ENCO_7A_Pin};
    static GPIO_TypeDef* const kBPort[8] = {ENCO_0B_GPIO_Port, ENCO_1B_GPIO_Port, ENCO_2B_GPIO_Port, ENCO_3B_GPIO_Port,
                                            ENCO_4B_GPIO_Port, ENCO_5B_GPIO_Port, ENCO_6B_GPIO_Port, ENCO_7B_GPIO_Port};
    static const uint16_t kBPin[8] = {ENCO_0B_Pin, ENCO_1B_Pin, ENCO_2B_Pin, ENCO_3B_Pin, ENCO_4B_Pin, ENCO_5B_Pin, ENCO_6B_Pin, ENCO_7B_Pin};
    static const int8_t kLut[16] = {0, 0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0};
    if (i < 0 || i >= 8) return;
    const uint8_t ab = ((kAPort[i]->IDR & kAPin[i]) ? 2 : 0) | ((kBPort[i]->IDR & kBPin[i]) ? 1 : 0);
    const uint32_t now = perf::cycles(), dt = now - lastCycles_[i];
    if (dt > usToCycles(2000000u)) { abPrev_[i] = ab; lastDir_[i] = 0; reversals_[i] = 0; count_[i] = 0; }
    if (dt <= usToCycles(700u)) return;
    const int dir = kLut[(abPrev_[i] << 2) | ab];
    lastCycles_[i] = now;
    abPrev_[i] = ab;
    if (!dir) return;
    if (dir != lastDir_[i]) {
        if (++reversals_[i] < 2) return;
        lastDir_[i] = (int8_t)dir;
        reversals_[i] = 0;
        count_[i] = 0;
    } else {
        reversals_[i] = 0;
    }
    count_[i] = (int8_t)(count_[i] + dir);
    if (count_[i] >= kEventsPerDetent || count_[i] <= -kEventsPerDetent) {
        detents_[i] = (int16_t)(detents_[i] + (count_[i] > 0 ? 1 : -1));
        count_[i] = 0;
    }
}

void Input::poll(app::Ui& ui) {
    const uint32_t ms = HAL_GetTick();
    // an expander holds its interrupt line until it is read; a missed edge would leave it held, so every pad is also read every 50 ms
    const bool sweep = ms - lastPoll_ >= 50;
    if (sweep) lastPoll_ = ms;
    for (int p = 0; p < 3; ++p) {
        if (!ok_[p]) continue;
        // the stock 1 ms settle after an interrupt before reading
        if (pending_[p] && !armed_[p]) { armed_[p] = true; pendingSince_[p] = ms; }
        const bool due = armed_[p] && ms - pendingSince_[p] >= 1;
        if (!due && !sweep) continue;
        // cleared BEFORE the read: an interrupt during the read arms the next one instead of being lost
        pending_[p] = 0;
        armed_[p] = false;
        uint16_t s;
        if (!readPad(p, s)) {
            if (++failures_ >= 3) { resetI2C3(); failures_ = 0; }
            continue;
        }
        failures_ = 0;
        const uint16_t changed = (uint16_t)(s ^ state_[p]);
        state_[p] = s;
        for (int b = 0; b < 16; ++b) {
            if (!((changed >> b) & 1)) continue;
            const bool down = !((s >> b) & 1);   // active low
            activityMs_ = ms;
            if (dark_) {   // the screen is dark: this press wakes it and nothing else (nor does its release)
                if (down) wakeAsked_ = true;
                continue;
            }
            if (p == Layer) {
                if (b >= 10) continue;
                if (down) ui.layerDown(b);
                else ui.layerUp(b);
            } else {
                const Key k = p == Left ? kLeftKeys[b] : kRightKeys[b];
                if (down) ui.keyDown(k);
                else ui.keyUp(k);
            }
        }
    }
    for (int i = 0; i < 8; ++i) {
        __disable_irq();
        const int d = detents_[i];
        detents_[i] = 0;
        __enable_irq();
        if (d) {
            activityMs_ = ms;
            if (dark_) { wakeAsked_ = true; continue; }
            ui.turn(kKnobOfEncoder[i], d * kDirection[i]);
        }
    }
}

}  // namespace board
