#include "sht3x.h"

#define SHT3X_ADDR       (0x44U << 1)
#define SHT3X_TIMEOUT_MS 20U

static uint8_t SHT3X_CalculateCRC(const uint8_t *data)
{
    uint8_t crc = 0xFFU;

    for (uint8_t i = 0; i < 2U; i++)
    {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8U; bit++)
        {
            if ((crc & 0x80U) != 0U)
            {
                crc = (uint8_t)((crc << 1) ^ 0x31U);
            }
            else
            {
                crc <<= 1;
            }
        }
    }

    return crc;
}

SHT3X_Status SHT3X_Start(I2C_HandleTypeDef *i2c)
{
    if (i2c == NULL)
    {
        return SHT3X_I2C_ERROR;
    }

    uint8_t command[2] = {0x24U, 0x00U};

    if (HAL_I2C_Master_Transmit(
            i2c,
            SHT3X_ADDR,
            command,
            sizeof(command),
            SHT3X_TIMEOUT_MS) != HAL_OK)
    {
        return SHT3X_I2C_ERROR;
    }

    return SHT3X_OK;
}

SHT3X_Status SHT3X_Read(
    I2C_HandleTypeDef *i2c,
    SHT3X_Sample *sample)
{
    if (i2c == NULL || sample == NULL)
    {
        return SHT3X_I2C_ERROR;
    }

    uint8_t data[6];

    if (HAL_I2C_Master_Receive(
            i2c,
            SHT3X_ADDR,
            data,
            sizeof(data),
            SHT3X_TIMEOUT_MS) != HAL_OK)
    {
        return SHT3X_I2C_ERROR;
    }

    if (SHT3X_CalculateCRC(&data[0]) != data[2] ||
        SHT3X_CalculateCRC(&data[3]) != data[5])
    {
        return SHT3X_CRC_ERROR;
    }

    uint16_t raw_temp =
        ((uint16_t)data[0] << 8) | data[1];

    uint16_t raw_humidity =
        ((uint16_t)data[3] << 8) | data[4];

    sample->temperature_c =
        -45.0f + (175.0f * raw_temp / 65535.0f);

    sample->humidity_pct =
        100.0f * raw_humidity / 65535.0f;

    return SHT3X_OK;
}