#ifndef __GLOBAL_H
#define __GLOBAL_H

// Minimal replacement for the stock app's Global.h: only what the platform code and the SGTL5000
// driver need. Audio rate and block size match the stock firmware so the I2S/codec setup is unchanged.

#include <stdint.h>

#include "main.h"

#define APPLICATION_ADDRESS     0x08020000U

#define AUDIO_SAMPLE_RATE       44100.0f
#ifndef I2S_BLOCK_SIZE
#define I2S_BLOCK_SIZE          32                                  // samples per DMA half-buffer (0.7 ms); CMake option DBOY_AUDIO_BLOCK
#endif
#define I2S_CHANNEL_COUNT       2
#define I2S_TOTAL_BUFFER_SIZE   (I2S_BLOCK_SIZE * I2S_CHANNEL_COUNT * 2)  // two half-buffers

const uint8_t kHpBase = 0x15;                                       // headphone volume at start-up


// SDRAM mode-register fields used by Sdram_Initialize (board.cpp).
#define SDRAM_TIMEOUT                               ((uint32_t)0xFFFF)
#define SDRAM_MODEREG_BURST_LENGTH_2                ((uint16_t)0x0001)
#define SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL         ((uint16_t)0x0000)
#define SDRAM_MODEREG_CAS_LATENCY_2                 ((uint16_t)0x0020)
#define SDRAM_MODEREG_OPERATING_MODE_STANDARD       ((uint16_t)0x0000)
#define SDRAM_MODEREG_WRITEBURST_MODE_SINGLE        ((uint16_t)0x0200)

// Codec output volume steps, index 0 = -20 dB ... index 20 = 0 dB (same values as the stock firmware).
const uint8_t kLoBase = 0x0D;
struct MixerHpVolume { char data; };
struct MixerLoVolume { char data; };
const MixerHpVolume kMixerHpVolumeDataLibrary[] = {{(char)(kHpBase + 40)}, {(char)(kHpBase + 38)}, {(char)(kHpBase + 36)}, {(char)(kHpBase + 34)}, {(char)(kHpBase + 32)}, {(char)(kHpBase + 30)}, {(char)(kHpBase + 28)}, {(char)(kHpBase + 26)}, {(char)(kHpBase + 24)}, {(char)(kHpBase + 22)}, {(char)(kHpBase + 20)}, {(char)(kHpBase + 18)}, {(char)(kHpBase + 16)}, {(char)(kHpBase + 14)}, {(char)(kHpBase + 12)}, {(char)(kHpBase + 10)}, {(char)(kHpBase + 8)}, {(char)(kHpBase + 6)}, {(char)(kHpBase + 4)}, {(char)(kHpBase + 2)}, {(char)(kHpBase + 0)}};
const MixerLoVolume kMixerLoVolumeDataLibrary[] = {{(char)(kLoBase + 18)}, {(char)(kLoBase + 18)}, {(char)(kLoBase + 18)}, {(char)(kLoBase + 17)}, {(char)(kLoBase + 16)}, {(char)(kLoBase + 15)}, {(char)(kLoBase + 14)}, {(char)(kLoBase + 13)}, {(char)(kLoBase + 12)}, {(char)(kLoBase + 11)}, {(char)(kLoBase + 10)}, {(char)(kLoBase + 9)}, {(char)(kLoBase + 8)}, {(char)(kLoBase + 7)}, {(char)(kLoBase + 6)}, {(char)(kLoBase + 5)}, {(char)(kLoBase + 4)}, {(char)(kLoBase + 3)}, {(char)(kLoBase + 2)}, {(char)(kLoBase + 1)}, {(char)(kLoBase + 0)}};

#define LED0_ON      HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET)
#define LED1_ON      HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET)
#define LED2_ON      HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET)
#define LED0_OFF     HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET)
#define LED1_OFF     HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET)
#define LED2_OFF     HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_RESET)
#define LED0_TOGGLE  HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin)
#define LED1_TOGGLE  HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin)
#define LED2_TOGGLE  HAL_GPIO_TogglePin(LED2_GPIO_Port, LED2_Pin)

#endif
