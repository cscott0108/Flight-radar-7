#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void UpdateSelectedAircraftUI(void);

float GetRadarLat(void);
float GetRadarLon(void);
float GetRadarRange(void);

/* Highest gAircraftCount observed since boot (post ground-filter - the same
 * count already used everywhere else; the architecture doesn't separately
 * retain a pre-filter provider count). Used only by the /diag page (PHASE 13
 * runtime capacity report); updated once per successful poll, nothing else
 * reads or resets it. */
int GetMaxAircraftCountSinceBoot(void);

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
// The fixed offset used by the "Custom fixed UTC offset" time zone.
int32_t GetRadarUtcOffsetMinutes(void);
uint32_t GetRadarDayStartHour(void);
uint32_t GetRadarDayEndHour(void);
uint32_t GetRadarDayIntervalSeconds(void);
uint32_t GetRadarNightIntervalSeconds(void);

// Time zone and Automatic DST (see time_util.h). zoneId is an IANA id from
// the built-in table or "CUSTOM"; customOffsetMinutes applies only to
// "CUSTOM". Persisted to NVS; every time-dependent feature reads it through
// time_util, so nothing else stores a zone.
void SetRadarTimeZone(
    const char *zoneId,
    bool autoDst,
    int32_t customOffsetMinutes);

void SetRadarDayNightSchedule(
    bool enabled,
    uint32_t dayStartHour,
    uint32_t dayEndHour,
    uint32_t dayIntervalSec,
    uint32_t nightIntervalSec);

// When enabled, logs every raw OpenSky field for the currently-selected
// (or first-seen, if nothing is selected yet) aircraft to the serial
// console on each poll. Toggleable from the web UI and persisted to NVS,
// so it can be flipped on to inspect what OpenSky actually sends without
// reflashing.
/* Automatic closest-aircraft selection (persisted in NVS radar/"autosel"). Must be
 * called from a task that is NOT holding the LVGL lock: it takes the lock itself. */
void SetRadarAutoSelect(bool enabled);

/* Hot Seen eviction policy (SeenEvictionPolicy in seen_aircraft.h, passed as int). Persisted in NVS
 * radar/"seenpol", applied to later evictions only; an unknown value is ignored. */
void SetSeenEvictionPolicy(int policy);

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

// Idle dimming: overrides brightness to a near-off level after N minutes
// with zero aircraft in range. Takes priority over the day/night
// schedule and the manual slider.
bool GetRadarIdleDimEnabled(void);
uint32_t GetRadarIdleDimMinutes(void);
uint32_t GetRadarIdleDimPercent(void);

void SetRadarIdleDimSettings(
    bool enabled,
    uint32_t minutes,
    uint32_t percent);

// Wi-Fi profiles (wifi_profiles.h). Asks the radio to join saved slot `slot`; applied on the LVGL timer.
void WifiRequestConnect(int slot);
// Current connection state for the WebUI: SSID being used, saved-slot index (-1 if unsaved/unknown).
bool WifiGetStatus(char *ssid, size_t cap, int *slotOut);
