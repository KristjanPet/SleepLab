#ifndef APP_H
#define APP_H

#include "stm32f4xx_hal.h"

void App_Init(I2C_HandleTypeDef *i2c);
void App_Update(void);

#endif