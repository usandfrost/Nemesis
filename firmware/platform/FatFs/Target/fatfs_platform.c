/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : fatfs_platform.c
  * @brief          : fatfs_platform source file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
*/
/* USER CODE END Header */
#include "fatfs_platform.h"

uint8_t	BSP_PlatformIsDetected(void) {
    uint8_t status = SD_PRESENT;
    /* Check SD card detect pin */
    if(HAL_GPIO_ReadPin(SD_DETECT_GPIO_PORT, SD_DETECT_PIN) != GPIO_PIN_RESET)
    {
        status = SD_NOT_PRESENT;
    }
    /* USER CODE BEGIN 1 */
    /* user code can be inserted here */
    /* USER CODE END 1 */
    return status;
}

extern SD_HandleTypeDef hsd2;

/* FIREFORK (dboy): stops a transfer that failed or timed out, so that it cannot complete into a buffer later. The HAL returns from several failures with the
   data path still armed and calls that ready, and then its abort does nothing; and a card that took a command whose response was lost is waiting for data. */
void BSP_SD_Abort(void)
{
  HAL_SD_Abort(&hsd2);
  hsd2.Instance->IDMACTRL = SDMMC_DISABLE_IDMA;
  __SDMMC_CMDTRANS_DISABLE(hsd2.Instance);
  hsd2.Instance->DCTRL = 0;
  __HAL_SD_CLEAR_FLAG(&hsd2, SDMMC_STATIC_DATA_FLAGS);
  hsd2.State = HAL_SD_STATE_READY;
  hsd2.Context = SD_CONTEXT_NONE;
  const HAL_SD_CardStateTypeDef state = HAL_SD_GetCardState(&hsd2);
  if ((state == HAL_SD_CARD_SENDING) || (state == HAL_SD_CARD_RECEIVING))
  {
    (void)SDMMC_CmdStopTransfer(hsd2.Instance);
  }
}
