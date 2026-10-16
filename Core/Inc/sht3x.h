#ifndef SHT3X_H
#define SHT3X_H

#include "stm32f4xx_hal.h"

HAL_StatusTypeDef SHT3X_Read(
    I2C_HandleTypeDef *hi2c,
    float *temperature,
    float *humidity
);

#endif