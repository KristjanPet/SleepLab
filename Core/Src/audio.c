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

static volatile uint8_t ready_half = 0U;
static volatile uint32_t missed_blocks = 0U;

static uint64_t amplitude_sum = 0U;
static uint32_t amplitude_samples = 0U;

static volatile uint16_t debug_frame[4];

static int32_t Audio_Decode24(uint16_t high, uint16_t low){

    // Join two 16-bit words into one 32-bit value.
    uint32_t packed =
        ((uint32_t)high << 16) | low;

    // Temporary test for the suspected one-bit offset.
    packed <<= 1;

    // Extract the 24-bit sample.
    uint32_t raw24 = (packed >> 8) & 0xFFFFFFU;

    // Convert unsigned 24-bit representation
    // into a signed 32-bit integer.
    return (int32_t)(raw24 ^ 0x800000U) - 0x800000;
}

HAL_StatusTypeDef Audio_Start(I2S_HandleTypeDef *hi2s)
{
    audio_i2s = hi2s;

    return HAL_I2S_Receive_DMA(
        hi2s,
        audio_buffer,
        (uint16_t)(AUDIO_BUFFER_WORDS / 2U)
    );
}

void HAL_I2S_RxHalfCpltCallback(I2S_HandleTypeDef *hi2s){
    if (hi2s == audio_i2s)
    {
        half_count++;
        if (ready_half != 0U){
            missed_blocks++;
        }
        ready_half = 1U;

        // DMA is now filling the second half.
        // The first half is safe to read.
        for (uint32_t i = 0; i < 4U; i++)
        {
            debug_frame[i] = audio_buffer[i];
        }
    }
}

void HAL_I2S_RxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == audio_i2s)
    {
        full_count++;
        if (ready_half != 0U){
            missed_blocks++;
        }
        ready_half = 2U;
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
    stats.missed_blocks = missed_blocks;

    for (uint32_t i = 0; i < 4U; i++){
        stats.raw_frame[i] = debug_frame[i];
    }

    stats.average_amplitude = (amplitude_samples > 0U)
    ? (uint32_t)(amplitude_sum / amplitude_samples)
    : 0U;

    // Start a new measurement window.
    amplitude_sum = 0U;
    amplitude_samples = 0U;

    return stats;
}

void Audio_Process(void){
    // Read and clear the ready flag atomically
    // relative to the interrupt callbacks.
    __disable_irq();
    uint8_t half = ready_half;
    ready_half = 0U;
    __enable_irq();

    if (half == 0U)
    {
        return;
    }

    uint32_t start = (half == 1U)
        ? 0U
        : AUDIO_BUFFER_WORDS / 2U;

    uint32_t end = start + AUDIO_BUFFER_WORDS / 2U;

    // Each stereo frame contains four 16-bit words.
    for (uint32_t i = start; i < end; i += 4U)
    {
        // Left channel: words 0 and 1.
        int32_t sample = Audio_Decode24(
            audio_buffer[i],
            audio_buffer[i + 1U]
        );

        // Absolute value of the sample.
        uint32_t amplitude = (sample < 0)
            ? (uint32_t)(-sample)
            : (uint32_t)sample;

        amplitude_sum += amplitude;
        amplitude_samples++;
    }
}


