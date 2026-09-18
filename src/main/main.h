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