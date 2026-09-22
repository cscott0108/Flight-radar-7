#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
#include <time.h>
#include <stdbool.h>

#define BM8563_ADDR 0x51
#ifndef I2C_MASTER_NUM
#define I2C_MASTER_NUM I2C_NUM_0
#endif

// Attaches the BM8563 RTC as a device on the given shared I2C bus. Call
// once, after the bus itself has been created (see i2c_bus.h).
esp_err_t bm8563_i2c_attach(i2c_master_bus_handle_t bus_handle);

esp_err_t bm8563_get_time(struct tm *timeinfo, bool *valid);
esp_err_t bm8563_set_time(struct tm *timeinfo);