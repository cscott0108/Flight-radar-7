#pragma once

// Shared I2C bus handle (new driver/i2c_master.h API, required on ESP-IDF
// v6.0+ since the legacy driver/i2c.h is end-of-life and cannot coexist
// with the new driver in the same firmware - mixing them aborts at boot
// with "CONFLICT! driver_ng is not allowed to be used with this old
// driver"). One bus is created once in main.c's i2c_master_init(), and
// every I2C device on this board (the BM8563 RTC, the GT911 touch
// controller) attaches to this same handle as a device rather than each
// owning its own bus.

#include "driver/i2c_master.h"

extern i2c_master_bus_handle_t gI2CBus;
