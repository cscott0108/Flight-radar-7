#pragma once
#include "driver/i2c.h"
#include "esp_err.h"
#include <time.h>
#include <stdbool.h>

#define BM8563_ADDR 0x51
#define I2C_MASTER_NUM I2C_NUM_0

esp_err_t bm8563_get_time(struct tm *timeinfo, bool *valid);
esp_err_t bm8563_set_time(struct tm *timeinfo);