#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "custom_rules.h"
#include "aircraft_provider.h" /* normalized Aircraft struct, gAircraft[] */

bool OpenSky_Init(void);
bool OpenSky_HasCredentials(void);
/* Remaining seconds of the one-hour pause after an HTTP 429 (also survives reboot). */
uint32_t OpenSky_GetRateLimitSeconds(void);

/* Reuses the existing radar center/range settings; builds OpenSky's
 * lamin/lamax/lomin/lomax bounding box internally so that query shape
 * doesn't leak past this file. */
bool OpenSky_GetAircraftJson(
    float centerLat,
    float centerLon,
    float radiusKm,
    const char **json);

bool OpenSky_ParseAircraft(
    const char *json);
