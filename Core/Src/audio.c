#include "audio.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"

#include <stdbool.h>
#include <stdint.h>

/* Each DMA half contains 256 stereo frames (4 x 16-bit words per frame). */
#define FRAMES_PER_HALF     256U
#define WORDS_PER_FRAME    4U
#define DMA_WORDS          (2U * FRAMES_PER_HALF * WORDS_PER_FRAME)

/* USB packet: 'SLAB' + uint32 sequence (little-endian) + 256 PCM16 mono samples. */
#define USB_HEADER_BYTES   8U
#define USB_PCM_BYTES      (FRAMES_PER_HALF * 2U)
#define USB_PACKET_BYTES   (USB_HEADER_BYTES + USB_PCM_BYTES)
#define USB_QUEUE_BLOCKS   8U

extern USBD_HandleTypeDef hUsbDeviceFS;

static uint16_t dma_audio[DMA_WORDS];
static I2S_HandleTypeDef *audio_i2s;

static volatile uint8_t ready_half;  /* 0 = none, 1 = first half, 2 = second half */
static volatile uint32_t half_count;
static volatile uint32_t full_count;
static volatile uint32_t error_count;
static volatile uint32_t missed_blocks;
static volatile uint16_t debug_frame[4];

/* Only normal application code accesses this USB queue. */
static uint8_t usb_queue[USB_QUEUE_BLOCKS][USB_PACKET_BYTES];
static uint8_t queue_write;
static uint8_t queue_read;
static uint8_t queue_count;
static bool usb_in_flight;
static uint32_t packet_sequence;
static uint32_t dropped_usb_blocks;

static uint64_t amplitude_sum;
static uint32_t amplitude_samples;

HAL_StatusTypeDef Audio_Start(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == NULL)
    {
        return HAL_ERROR;
    }

    audio_i2s = hi2s;
    ready_half = 0U;

    /* HAL doubles Size for 24/32-bit I2S formats with 16-bit DMA transfers. */
    return HAL_I2S_Receive_DMA(hi2s, dma_audio, (uint16_t)(DMA_WORDS / 2U));
}

void HAL_I2S_RxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s != audio_i2s)
    {
        return;
    }

    half_count++;
    if (ready_half != 0U)
    {
        missed_blocks++;
    }
    ready_half = 1U;

    for (uint32_t i = 0U; i < 4U; ++i)
    {
        debug_frame[i] = dma_audio[i];
    }
}

void HAL_I2S_RxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s != audio_i2s)
    {
        return;
    }

    full_count++;
    if (ready_half != 0U)
    {
        missed_blocks++;
    }
    ready_half = 2U;
}

void HAL_I2S_ErrorCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == audio_i2s)
    {
        error_count++;
    }
}

static void Audio_EnqueueCompletedHalf(void)
{
    /* Read-and-clear must not race against an interrupt callback. */
    uint32_t old_primask = __get_PRIMASK();
    __disable_irq();
    uint8_t half = ready_half;
    ready_half = 0U;
    if (old_primask == 0U)
    {
        __enable_irq();
    }

    if (half == 0U)
    {
        return;
    }

    /* Increment even when a block is dropped, so the PC sees sequence gaps. */
    uint32_t sequence = packet_sequence++;

    if (queue_count >= USB_QUEUE_BLOCKS)
    {
        dropped_usb_blocks++;
        return;
    }

    uint8_t *packet = usb_queue[queue_write];
    packet[0] = 'S';
    packet[1] = 'L';
    packet[2] = 'A';
    packet[3] = 'B';
    packet[4] = (uint8_t)sequence;
    packet[5] = (uint8_t)(sequence >> 8);
    packet[6] = (uint8_t)(sequence >> 16);
    packet[7] = (uint8_t)(sequence >> 24);

    uint32_t start = (half == 1U) ? 0U : DMA_WORDS / 2U;

    for (uint32_t frame = 0U; frame < FRAMES_PER_HALF; ++frame)
    {
        uint32_t word = start + frame * WORDS_PER_FRAME;

        /* The microphone is connected to the LEFT I2S channel.
         * Provisional 1-bit alignment correction, from our earlier tests.
         * Retain only the top 16 bits for playable mono PCM16.
         */
        uint16_t sample = (uint16_t)(dma_audio[word] << 1);

        packet[USB_HEADER_BYTES + frame * 2U] = (uint8_t)sample;
        packet[USB_HEADER_BYTES + frame * 2U + 1U] = (uint8_t)(sample >> 8);

        int32_t signed_sample = (int16_t)sample;
        amplitude_sum += (uint32_t)((signed_sample < 0) ? -signed_sample : signed_sample);
        amplitude_samples++;
    }

    queue_write = (uint8_t)((queue_write + 1U) % USB_QUEUE_BLOCKS);
    queue_count++;
}

static void Audio_TransmitQueuedUsb(void)
{
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED ||
        hUsbDeviceFS.pClassData == NULL)
    {
        /* Reset the queue after USB disconnects. */
        queue_read = queue_write;
        queue_count = 0U;
        usb_in_flight = false;
        return;
    }

    USBD_CDC_HandleTypeDef *cdc =
        (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;

    if (usb_in_flight)
    {
        if (cdc->TxState != 0U)
        {
            return;
        }

        /* Finished: it is now safe to reuse this packet buffer. */
        queue_read = (uint8_t)((queue_read + 1U) % USB_QUEUE_BLOCKS);
        queue_count--;
        usb_in_flight = false;
    }

    if (queue_count == 0U || cdc->TxState != 0U)
    {
        return;
    }

    if (CDC_Transmit_FS(usb_queue[queue_read], USB_PACKET_BYTES) == USBD_OK)
    {
        usb_in_flight = true;
    }
}

void Audio_Poll(void)
{
    Audio_EnqueueCompletedHalf();
    Audio_TransmitQueuedUsb();
}

AudioStats Audio_GetStats(void)
{
    AudioStats stats;
    stats.half_count = half_count;
    stats.full_count = full_count;
    stats.error_count = error_count;
    stats.missed_blocks = missed_blocks;
    stats.dropped_usb_blocks = dropped_usb_blocks;

    for (uint32_t i = 0U; i < 4U; ++i)
    {
        stats.raw_frame[i] = debug_frame[i];
    }

    stats.average_amplitude = (amplitude_samples != 0U)
        ? (uint32_t)(amplitude_sum / amplitude_samples)
        : 0U;
    amplitude_sum = 0U;
    amplitude_samples = 0U;

    return stats;
}
