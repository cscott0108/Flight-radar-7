#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "custom_rules.h"

#define MAX_AIRCRAFT 200

typedef struct
{
    char icao24[12];
    char callsign[16];
    char originCountry[64];

    CraftType craftType;

    float longitude;
    float latitude;

    float altitude;
    float velocity;
    float heading;

    bool valid;

    float predictedLat;
    float predictedLon;

    uint32_t lastUpdateMs;

} Aircraft;

extern Aircraft gAircraft[MAX_AIRCRAFT];
extern int gAircraftCount;

bool OpenSky_Init(void);
bool OpenSky_HasCredentials(void);
/* Remaining seconds of the one-hour pause after an HTTP 429 (also survives reboot). */
uint32_t OpenSky_GetRateLimitSeconds(void);

bool OpenSky_GetAircraftJson(
    float minLat,
    float maxLat,
    float minLon,
    float maxLon,
    const char **json);

bool OpenSky_ParseAircraft(
    const char *json);
