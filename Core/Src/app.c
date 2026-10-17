#include "app.h"
#include "main.h"
#include "sht3x.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"

#include <stdbool.h>
#include <stdio.h>

#define SAMPLE_PERIOD_MS  1000U
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

static SHT3X_Sample latest_sample;
static SHT3X_Status latest_status;

static bool report_pending = false;
static uint8_t usb_buffer[96];

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

            if ((uint32_t)(now - last_request_ms)
                >= SAMPLE_PERIOD_MS)
            {
                last_request_ms = now;

                latest_status = SHT3X_Start(sensor_i2c);

                if (latest_status == SHT3X_OK)
                {
                    measurement_start_ms = HAL_GetTick();
                    state = APP_WAIT_MEASUREMENT;
                }
                else
                {
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

    if (latest_status == SHT3X_OK)
    {
        length = snprintf(
            (char *)usb_buffer,
            sizeof(usb_buffer),
            "Temperature: %.2f C | Humidity: %.2f %%\r\n",
            latest_sample.temperature_c,
            latest_sample.humidity_pct
        );
    }
    else
    {
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

void App_Update(void)
{
    uint32_t now = HAL_GetTick();

    App_UpdateSensor(now);
    App_UpdateUSB();

    // Independent LED heartbeat.
    if ((uint32_t)(now - last_led_ms) >= LED_PERIOD_MS)
    {
        last_led_ms = now;

        HAL_GPIO_TogglePin(
            LED_GPIO_Port,
            LED_Pin
        );
    }
}