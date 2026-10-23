#include "audio.h"

#define AUDIO_FRAMES_PER_HALF 256U
#define I2S_WORDS_PER_FRAME   4U
#define AUDIO_BUFFER_WORDS \
    (2U * AUDIO_FRAMES_PER_HALF * I2S_WORDS_PER_FRAME)

static uint16_t audio_buffer[AUDIO_BUFFER_WORDS];

static I2S_HandleTypeDef *audio_i2s;

static volatile uint32_t half_count = 0U;
static volatile uint32_t full_count = 0U;
static volatile uint32_t error_count = 0U;

HAL_StatusTypeDef Audio_Start(I2S_HandleTypeDef *hi2s)
{
    audio_i2s = hi2s;

    return HAL_I2S_Receive_DMA(
        hi2s,
        audio_buffer,
        (uint16_t)(AUDIO_BUFFER_WORDS / 2U)
    );
}

void HAL_I2S_RxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == audio_i2s)
    {
        half_count++;
    }
}

void HAL_I2S_RxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == audio_i2s)
    {
        full_count++;
    }
}

void HAL_I2S_ErrorCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == audio_i2s)
    {
        error_count++;
    }
}

AudioStats Audio_GetStats(void)
{
    AudioStats stats;

    stats.half_count = half_count;
    stats.full_count = full_count;
    stats.error_count = error_count;

    return stats;
}