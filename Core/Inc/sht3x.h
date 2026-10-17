#ifndef SHT3X_H
#define SHT3X_H

#include "stm32f4xx_hal.h"

typedef struct
{
    float temperature_c;
    float humidity_pct;
} SHT3X_Sample;

typedef enum
{
    SHT3X_OK = 0,
    SHT3X_I2C_ERROR,
    SHT3X_CRC_ERROR
} SHT3X_Status;

SHT3X_Status SHT3X_Start(I2C_HandleTypeDef *i2c);
SHT3X_Status SHT3X_Read(
    I2C_HandleTypeDef *i2c,
    SHT3X_Sample *sample
);

#endif