#pragma once

#include <stdint.h>
#include <stdbool.h>

void UpdateSelectedAircraftUI(void);

float GetRadarLat(void);
float GetRadarLon(void);
float GetRadarRange(void);

void SetRadarSettings(
    float lat,
    float lon,
    float rangeKm);

void SaveRadarSettings(
    float lat,
    float lon,
    float rangeKm);

uint32_t GetRadarRefreshSeconds(void);

void SetRadarRefreshSeconds(
    uint32_t seconds);

uint32_t GetRadarLowTrafficThreshold(void);
uint32_t GetRadarLowTrafficIntervalSeconds(void);

void SetRadarLowTrafficThreshold(
    uint32_t aircraftCount);

void SetRadarLowTrafficIntervalSeconds(
    uint32_t seconds);

bool GetRadarDayNightEnabled(void);
int32_t GetRadarUtcOffsetMinutes(void);
uint32_t GetRadarDayStartHour(void);
uint32_t GetRadarDayEndHour(void);
uint32_t GetRadarDayIntervalSeconds(void);
uint32_t GetRadarNightIntervalSeconds(void);

void SetRadarDayNightSchedule(
    bool enabled,
    int32_t utcOffsetMinutes,
    uint32_t dayStartHour,
    uint32_t dayEndHour,
    uint32_t dayIntervalSec,
    uint32_t nightIntervalSec);

// When enabled, logs every raw OpenSky field for the currently-selected
// (or first-seen, if nothing is selected yet) aircraft to the serial
// console on each poll. Toggleable from the web UI and persisted to NVS,
// so it can be flipped on to inspect what OpenSky actually sends without
// reflashing.
bool GetRadarOpenSkyDebugEnabled(void);
void SetRadarOpenSkyDebugEnabled(bool enabled);

// LCD backlight brightness, as a percentage (1-100) of full duty on the
// PWM-driven backlight pin. Persisted to NVS and re-applied at boot.
uint32_t GetRadarBrightness(void);
void SetRadarBrightness(uint32_t percent);

// Optional day/night brightness schedule. Reuses the same day window
// (start/end hour + UTC offset) as the day/night poll schedule, but is
// enabled and applied independently of it. When enabled, overrides the
// manual brightness slider above based on time of day.
bool GetRadarDayNightBrightnessEnabled(void);
uint32_t GetRadarDayBrightnessPercent(void);
uint32_t GetRadarNightBrightnessPercent(void);

void SetRadarDayNightBrightnessSchedule(
    bool enabled,
    uint32_t dayPercent,
    uint32_t nightPercent);