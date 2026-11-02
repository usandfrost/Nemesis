#pragma once
#include "stm32h7xx_hal.h"
typedef struct { volatile uint32_t IDR; } GpioStub;
#ifdef __cplusplus
extern "C" {
#endif
extern GpioStub gDetectPort;
#ifdef __cplusplus
}
#endif
#define SDMMC2_DETECT_GPIO_Port (&gDetectPort)
#define SDMMC2_DETECT_Pin 0x2000u
