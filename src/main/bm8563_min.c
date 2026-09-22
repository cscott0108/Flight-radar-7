#include "bm8563_min.h"

#define REG_SECONDS 0x02

// Set once by bm8563_i2c_attach(); all later reads/writes go through
// this device handle rather than a bus/port number directly (new
// driver/i2c_master.h model: one handle per device, not per transaction).
static i2c_master_dev_handle_t bm8563_dev = NULL;

esp_err_t bm8563_i2c_attach(i2c_master_bus_handle_t bus_handle)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BM8563_ADDR,
        .scl_speed_hz = 100000,
    };

    return i2c_master_bus_add_device(bus_handle, &dev_config, &bm8563_dev);
}

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
    if (!bm8563_dev)
        return ESP_ERR_INVALID_STATE;

    uint8_t reg = REG_SECONDS;
    uint8_t data[7];

    // New API's transfer timeout is in plain milliseconds, not FreeRTOS
    // ticks - no pdMS_TO_TICKS() here (that was the old i2c.h driver's
    // convention).
    esp_err_t err = i2c_master_transmit_receive(
        bm8563_dev,
        &reg,
        1,
        data,
        7,
        100);

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
    if (!bm8563_dev)
        return ESP_ERR_INVALID_STATE;

    uint8_t data[8];

    data[0] = REG_SECONDS;
    data[1] = dec2bcd(timeinfo->tm_sec);
    data[2] = dec2bcd(timeinfo->tm_min);
    data[3] = dec2bcd(timeinfo->tm_hour);
    data[4] = dec2bcd(timeinfo->tm_mday);
    data[5] = dec2bcd(timeinfo->tm_wday);
    data[6] = dec2bcd(timeinfo->tm_mon + 1);
    data[7] = dec2bcd(timeinfo->tm_year - 100); // 2000 base

    return i2c_master_transmit(
        bm8563_dev,
        data,
        8,
        100);
}