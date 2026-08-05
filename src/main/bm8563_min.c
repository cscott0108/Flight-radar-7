#include "bm8563_min.h"

#define REG_SECONDS 0x02

static uint8_t bcd2dec(uint8_t val)
{
    return ((val >> 4) * 10) + (val & 0x0F);
}

static uint8_t dec2bcd(uint8_t val)
{
    return ((val / 10) << 4) | (val % 10);
}

esp_err_t bm8563_get_time(struct tm *timeinfo, bool *valid)
{
    uint8_t data[7];

    esp_err_t err = i2c_master_write_read_device(
        I2C_MASTER_NUM,
        BM8563_ADDR,
        (uint8_t[]){REG_SECONDS},
        1,
        data,
        7,
        pdMS_TO_TICKS(100));

    if (err != ESP_OK)
        return err;

    // Check VL (voltage low) flag
    if (valid)
        *valid = !(data[0] & 0x80);

    timeinfo->tm_sec = bcd2dec(data[0] & 0x7F);
    timeinfo->tm_min = bcd2dec(data[1] & 0x7F);
    timeinfo->tm_hour = bcd2dec(data[2] & 0x3F);
    timeinfo->tm_mday = bcd2dec(data[3] & 0x3F);
    timeinfo->tm_wday = bcd2dec(data[4] & 0x07);
    timeinfo->tm_mon = bcd2dec(data[5] & 0x1F) - 1;
    timeinfo->tm_year = bcd2dec(data[6]) + 100; // 2000 offset

    return ESP_OK;
}

esp_err_t bm8563_set_time(struct tm *timeinfo)
{
    uint8_t data[8];

    data[0] = REG_SECONDS;
    data[1] = dec2bcd(timeinfo->tm_sec);
    data[2] = dec2bcd(timeinfo->tm_min);
    data[3] = dec2bcd(timeinfo->tm_hour);
    data[4] = dec2bcd(timeinfo->tm_mday);
    data[5] = dec2bcd(timeinfo->tm_wday);
    data[6] = dec2bcd(timeinfo->tm_mon + 1);
    data[7] = dec2bcd(timeinfo->tm_year - 100); // 2000 base

    return i2c_master_write_to_device(
        I2C_MASTER_NUM,
        BM8563_ADDR,
        data,
        8,
        pdMS_TO_TICKS(100));
}