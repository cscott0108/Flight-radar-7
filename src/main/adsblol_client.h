#pragma once

/* adsb.lol provider (https://api.adsb.lol).
 *
 * A community-run, ADSBExchange-Rapid-API-compatible aircraft data source.
 * Unlike OpenSky it needs no OAuth credentials at all: HasCredentials()
 * always returns true and every request is a plain HTTPS GET. Its rate
 * limiting is dynamic/undocumented rather than a fixed quota (see
 * github.com/adsblol/api), so this client is conservative about how often
 * it's called (enforced centrally - see AircraftProvider_MinPollIntervalSeconds).
 *
 * Endpoint used: GET /v2/point/{lat}/{lon}/{radiusNm} - radius is capped at
 * 250 nm by the API itself; centerLat/centerLon/radiusKm come straight from
 * the existing radar location settings, same as OpenSky.
 */

#include <stdbool.h>
#include <stdint.h>

#include "aircraft_provider.h"
#include "adsblol_category.h" /* AdsbLol_CategoryToAircraftType - pure, host-testable */

bool AdsbLol_Init(void);
bool AdsbLol_HasCredentials(void); /* always true: no auth required */
uint32_t AdsbLol_GetRateLimitSeconds(void);

bool AdsbLol_GetAircraftJson(
    float centerLat,
    float centerLon,
    float radiusKm,
    const char **json);

bool AdsbLol_ParseAircraft(const char *json);

