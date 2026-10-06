#pragma once

/* Curated built-in airport database (read-only, compiled into flash).
 *
 * A global baseline of major and regional airports with scheduled airline
 * service, generated from OurAirports (public domain) by
 * tools/gen_builtin_airports.py into builtin_airports.c:
 *   tier 1 = large airports (major international / hub airports)
 *   tier 2 = medium airports with scheduled service and an IATA code
 * Small, private, specialty and personal locations are NOT built in; they are
 * user-defined locations (airports.c, /airports).
 *
 * The table lives in flash (.rodata) and is never copied to RAM. Rows are
 * sorted by latitude so a regional latitude band can be found with a binary
 * search (see Airports_SelectBuiltins in airports.c). */

#include <stdint.h>

#define BUILTIN_AIRPORT_TIER_MAJOR 1
#define BUILTIN_AIRPORT_TIER_REGIONAL 2
#define BUILTIN_AIRPORT_AXIS_UNKNOWN 0xFF

typedef struct {
    int32_t latE5;       /* latitude  * 1e5 (about 1 m resolution) */
    int32_t lonE5;       /* longitude * 1e5 */
    uint16_t nameOffset; /* into kBuiltinAirportNames (NUL-terminated, <= 19 chars) */
    char icao[4];        /* ICAO (or local) ident, 3-4 chars, NUL-padded, not NUL-terminated at 4 */
    uint8_t tier;        /* BUILTIN_AIRPORT_TIER_* */
    uint8_t runwayAxis10; /* physical axis of the longest runway / 10 deg (0-17), or AXIS_UNKNOWN */
} BuiltinAirport;

_Static_assert(sizeof(BuiltinAirport) == 16, "BuiltinAirport must stay 16 bytes");

extern const BuiltinAirport kBuiltinAirports[];
extern const uint16_t kBuiltinAirportCount;
extern const char kBuiltinAirportNames[];
