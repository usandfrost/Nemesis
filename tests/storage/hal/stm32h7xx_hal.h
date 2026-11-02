// What the SD driver sees of the HAL and the core, on the host: tests/storage/SdModel.cpp stands behind it. The real sd_diskio.c, bsp_driver_sd.c and
// FatFs compile against this unchanged.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define __weak __attribute__((weak))
#define __IO volatile
#define __ALIGN_BEGIN
#define __ALIGN_END __attribute__((aligned(32)))
#define ALIGN_32BYTES(buf) buf __attribute__((aligned(32)))
#define BLOCKSIZE 512U
#define SDMMC2 0
#define SDMMC_CLOCK_EDGE_RISING 0
#define SDMMC_CLOCK_POWER_SAVE_DISABLE 0
#define SDMMC_BUS_WIDE_4B 0
#define SDMMC_HARDWARE_FLOW_CONTROL_DISABLE 0
#define GPIO_PIN_13 13
#define GPIOC 0
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { HAL_SD_CARD_TRANSFER = 4, HAL_SD_CARD_PROGRAMMING = 7 } HAL_SD_CardStateTypeDef;
typedef struct { uint32_t LogBlockNbr, LogBlockSize; } HAL_SD_CardInfoTypeDef;
typedef struct { int ClockEdge, ClockPowerSave, BusWide, HardwareFlowControl, ClockDiv; } SD_InitStub;
typedef struct { int Instance; SD_InitStub Init; } SD_HandleTypeDef;

uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_SD_Init(SD_HandleTypeDef* hsd);
HAL_StatusTypeDef HAL_SD_ConfigWideBusOperation(SD_HandleTypeDef* hsd, uint32_t mode);
HAL_StatusTypeDef HAL_SD_ReadBlocks(SD_HandleTypeDef* hsd, uint8_t* d, uint32_t add, uint32_t n, uint32_t timeout);
HAL_StatusTypeDef HAL_SD_WriteBlocks(SD_HandleTypeDef* hsd, const uint8_t* d, uint32_t add, uint32_t n, uint32_t timeout);
HAL_StatusTypeDef HAL_SD_ReadBlocks_DMA(SD_HandleTypeDef* hsd, uint8_t* d, uint32_t add, uint32_t n);
HAL_StatusTypeDef HAL_SD_WriteBlocks_DMA(SD_HandleTypeDef* hsd, const uint8_t* d, uint32_t add, uint32_t n);
HAL_StatusTypeDef HAL_SD_Erase(SD_HandleTypeDef* hsd, uint32_t a, uint32_t b);
HAL_StatusTypeDef HAL_SD_Abort(SD_HandleTypeDef* hsd);
HAL_SD_CardStateTypeDef HAL_SD_GetCardState(SD_HandleTypeDef* hsd);
HAL_StatusTypeDef HAL_SD_GetCardInfo(SD_HandleTypeDef* hsd, HAL_SD_CardInfoTypeDef* info);
uint32_t HAL_SD_GetError(const SD_HandleTypeDef* hsd);
void SCB_CleanDCache_by_Addr(volatile void* p, int32_t n);
void SCB_InvalidateDCache_by_Addr(volatile void* p, int32_t n);
void SCB_CleanInvalidateDCache_by_Addr(volatile void* p, int32_t n);
int sd_model_reaches(const void* p);   // SD_DMA_REACHES on the host
#ifdef __cplusplus
}
#endif
