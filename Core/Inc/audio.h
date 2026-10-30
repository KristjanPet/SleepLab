#ifndef AUDIO_H
#define AUDIO_H

#include "stm32f4xx_hal.h"

typedef struct
{
    uint32_t half_count;
    uint32_t full_count;
    uint32_t error_count;
    uint32_t missed_blocks;
    
    float rms;
    float dbfs;
    float estimated_level_db; 
    uint32_t processed_samples;

    uint16_t raw_frame[4];
} AudioStats;

HAL_StatusTypeDef Audio_Start(I2S_HandleTypeDef *hi2s);
AudioStats Audio_GetStats(void);
void Audio_Process(void);

#endif