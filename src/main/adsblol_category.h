#pragma once

/* Isolated from adsblol_client.c specifically so it has zero ESP-IDF
 * dependencies and can be compiled and unit-tested on the host (see
 * host_tests/). Kept in its own tiny translation unit rather than inlined,
 * so the test binary doesn't need to stub esp_http_client/cJSON/etc. just to
 * exercise this mapping table. */

#include <stdbool.h>

#include "craft_types.h"

/* See adsblol_client.h for the full doc comment on this mapping's rationale
 * and the ICAO Annex 10 / DO-260B category semantics it relies on. */
bool AdsbLol_CategoryToAircraftType(const char *category, AircraftType *out);
