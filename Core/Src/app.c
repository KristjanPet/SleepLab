#include "app.h"
#include "main.h"
#include "sht3x.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include "audio.h"

#include <stdbool.h>
#include <stdio.h>

#define SAMPLE_PERIOD_MS  500U //TODO temp
#define CONVERSION_MS     20U
#define LED_PERIOD_MS     500U

typedef enum
{
    APP_IDLE,
    APP_WAIT_MEASUREMENT
} App_State;

extern USBD_HandleTypeDef hUsbDeviceFS;

static I2C_HandleTypeDef *sensor_i2c;

static App_State state = APP_IDLE;

static uint32_t last_request_ms;
static uint32_t measurement_start_ms;
static uint32_t last_led_ms;

static uint32_t last_actual_request_ms = 0U;
static uint32_t actual_interval_ms = 0U;
static bool first_request = true;


static SHT3X_Sample latest_sample;
static SHT3X_Status latest_status;

static bool report_pending = false;
static uint8_t usb_buffer[128];

void App_Init(I2C_HandleTypeDef *i2c)
{
    sensor_i2c = i2c;

    uint32_t now = HAL_GetTick();

    // Allow the first measurement immediately.
    last_request_ms = now - SAMPLE_PERIOD_MS;
    last_led_ms = now;

    state = APP_IDLE;
}

static void App_UpdateSensor(uint32_t now)
{
    switch (state)
    {
        case APP_IDLE:
            uint32_t elapsed = (uint32_t)(now - last_request_ms);
            if (elapsed >= SAMPLE_PERIOD_MS){
                uint32_t periods = elapsed / SAMPLE_PERIOD_MS;
                last_request_ms += periods * SAMPLE_PERIOD_MS; //skip the late periods

                if (!first_request){
                    actual_interval_ms = (uint32_t)(now - last_actual_request_ms);
                }

                last_actual_request_ms = now;
                first_request = false;

                latest_status = SHT3X_Start(sensor_i2c);

                if (latest_status == SHT3X_OK){
                    measurement_start_ms = HAL_GetTick();
                    state = APP_WAIT_MEASUREMENT;
                }
                else{
                    report_pending = true;
                }
            }
            break;

        case APP_WAIT_MEASUREMENT:

            if ((uint32_t)(now - measurement_start_ms)
                >= CONVERSION_MS)
            {
                latest_status = SHT3X_Read(
                    sensor_i2c,
                    &latest_sample
                );

                report_pending = true;
                state = APP_IDLE;
            }
            break;
    }
}

static void App_UpdateUSB(void)
{
    if (!report_pending ||
        hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED ||
        hUsbDeviceFS.pClassData == NULL)
    {
        return;
    }

    USBD_CDC_HandleTypeDef *cdc =
        (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;

    // The previous transfer must be complete.
    if (cdc->TxState != 0U)
    {
        return;
    }

    int length;

    if (latest_status == SHT3X_OK){
        AudioStats audio = Audio_GetStats();

        length = snprintf(
            (char *)usb_buffer,
            sizeof(usb_buffer),
            "T:%.2f RH:%.2f"
            " | H:%lu F:%lu E:%lu"
            " | Miss:%lu N:%lu"
            " | RMS:%.5f dBFS:%.1f Est:%.1f\r\n",
            latest_sample.temperature_c,
            latest_sample.humidity_pct,
            (unsigned long)audio.half_count,
            (unsigned long)audio.full_count,
            (unsigned long)audio.error_count,
            (unsigned long)audio.missed_blocks,
            (unsigned long)audio.processed_samples,
            audio.rms,
            audio.dbfs,
            audio.estimated_level_db
        );
    }
    else{
        length = snprintf(
            (char *)usb_buffer,
            sizeof(usb_buffer),
            "SHT3x error: %d\r\n",
            (int)latest_status
        );
    }

    if (length <= 0 ||
        (size_t)length >= sizeof(usb_buffer))
    {
        return;
    }

    if (CDC_Transmit_FS(
            usb_buffer,
            (uint16_t)length) == USBD_OK)
    {
        report_pending = false;
    }
}

void App_Update(void){
    uint32_t now = HAL_GetTick();

    App_UpdateSensor(now);

    App_UpdateUSB();

    // Independent LED heartbeat.
    if ((uint32_t)(now - last_led_ms) >= LED_PERIOD_MS)
    {
        last_led_ms = now;
        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
    }
}

