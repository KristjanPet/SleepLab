#include "sht3x.h"

#define SHT3X_ADDR       (0x44U << 1)
#define SHT3X_TIMEOUT_MS 100U

static uint8_t SHT3X_CalculateCRC(const uint8_t *data, uint8_t length){
    uint8_t crc = 0xFFU;

    for (uint8_t i = 0; i < length; i++)
    {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8; bit++)
        {
            if (crc & 0x80U){
                crc = (uint8_t)((crc << 1) ^ 0x31U);
            }
            else{
                crc <<= 1;
            }
        }
    }

    return crc;
}

HAL_StatusTypeDef SHT3X_Read(I2C_HandleTypeDef *hi2c, float *temperature, float *humidity){
    
    if (hi2c == NULL || temperature == NULL ||
        humidity == NULL)
    {
        return HAL_ERROR;
    }

    uint8_t command[2] = {0x24, 0x00};
    uint8_t data[6];

    // Start one measurement.
    HAL_StatusTypeDef status = HAL_I2C_Master_Transmit(
        hi2c,
        SHT3X_ADDR,
        command,
        sizeof(command),
        SHT3X_TIMEOUT_MS
    );

    if (status != HAL_OK)
    {
        return status;
    }

    // Wait until the sensor completes the measurement.
    HAL_Delay(20);

    // Read temperature, humidity, and CRC bytes.
    status = HAL_I2C_Master_Receive(
        hi2c,
        SHT3X_ADDR,
        data,
        sizeof(data),
        SHT3X_TIMEOUT_MS
    );

    if (status != HAL_OK)
    {
        return status;
    }

    // Verify both CRC bytes.
    if (SHT3X_CalculateCRC(&data[0], 2) != data[2] ||
        SHT3X_CalculateCRC(&data[3], 2) != data[5])
    {
        return HAL_ERROR;
    }

    // Combine the most and least significant bytes.
    uint16_t raw_temperature =
        ((uint16_t)data[0] << 8) | data[1];

    uint16_t raw_humidity =
        ((uint16_t)data[3] << 8) | data[4];

    // Convert raw values to physical units.
    *temperature =
        -45.0f + 175.0f * raw_temperature / 65535.0f;

    *humidity =
        100.0f * raw_humidity / 65535.0f;

    return HAL_OK;
}