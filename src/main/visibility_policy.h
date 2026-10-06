#pragma once

/* Aircraft Visibility & Retention policy (0.0.26).
 *
 * The one place that decides whether a provider-reported aircraft enters the
 * aircraft list (gAircraft) - the "admission" point that used to be the
 * duplicated ground filter in opensky_client.c / adsblol_client.c.
 *
 *   Provider data -> normalized Aircraft -> classification (ResolveAircraft*)
 *     -> VisPolicy_Admit -> gAircraft -> radar / Seen / History
 *
 * Default settings reproduce 0.0.25 exactly: an aircraft is "on the ground"
 * when the provider says so (OpenSky on_ground, adsb.lol alt_baro "ground")
 * OR altitude <= 15 m AND speed <= 8.5 m/s (a missing value is 0, as the
 * record is zeroed before parsing), and on-ground aircraft are not admitted.
 *
 * The new controls only ADD aircraft on top of that:
 *   - airport aircraft: on-ground aircraft within a radius of a displayed
 *     airport/heliport location are counted (Count) or shown (Show);
 *   - retention: helicopters / INTERESTING / IMPORTANT / police+emergency
 *     aircraft stay shown while on the ground, and stay shown at their last
 *     reported position for the stale timeout after the provider stops
 *     reporting them (measured from the last successful observation).
 * Airborne aircraft always have priority for list slots (ground aircraft are
 * admitted in a second pass into the remaining capacity, stale ones last).
 * Hidden ground aircraft are not tracked, exactly as before; admitted ones
 * keep their single ICAO24 identity in Seen/History. Stale (carried) entries
 * are never observed by Seen/History, so they cannot extend lastSeen. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aircraft_provider.h"

/* Aircraft.visState values (AIRCRAFT_VIS_*) are defined in aircraft_provider.h. */

typedef enum {
    VIS_PROVIDER_GROUND_USE = 0,   /* default: provider on-ground flag counts when reported */
    VIS_PROVIDER_GROUND_IGNORE = 1 /* thresholds only */
} VisProviderGround;

typedef enum {
    VIS_AIRPORT_OFF = 0,   /* default */
    VIS_AIRPORT_COUNT = 1, /* count on-ground aircraft per airport, do not draw them */
    VIS_AIRPORT_SHOW = 2   /* draw them as normal aircraft */
} VisAirportMode;

#define VIS_RETAIN_HELICOPTER 0x01
#define VIS_RETAIN_INTERESTING 0x02
#define VIS_RETAIN_IMPORTANT 0x04
#define VIS_RETAIN_POLICE_EMERGENCY 0x08
#define VIS_RETAIN_ALL 0x0F

#define VIS_DEFAULT_ALT_M GROUND_ALTITUDE_THRESHOLD_M    /* 15 m, the 0.0.25 value */
#define VIS_DEFAULT_SPEED_MS GROUND_VELOCITY_THRESHOLD_MS /* 8.5 m/s, the 0.0.25 value */
#define VIS_THRESHOLD_DEFAULT 0xFFFFu
#define VIS_ALT_FT_MAX 5000u
#define VIS_SPEED_KT10_MAX 2000u      /* 200.0 kt */
#define VIS_STALE_MIN_DEFAULT 5u
#define VIS_STALE_MIN_MIN 1u          /* no 0 / infinite: retention always ends */
#define VIS_STALE_MIN_MAX 60u
#define VIS_RADIUS_M_DEFAULT 3000u
#define VIS_RADIUS_M_MIN 500u
#define VIS_RADIUS_M_MAX 10000u
#define VIS_RETAIN_TABLE_MAX 16u      /* retained aircraft remembered for the stale timeout */
#define VIS_AIRPORT_COUNTS_MAX 64u    /* airports with on-ground aircraft listed per poll */

typedef struct {
    uint8_t version;        /* 1 */
    uint8_t providerGround; /* VisProviderGround */
    uint8_t airportMode;    /* VisAirportMode */
    uint8_t retainMask;     /* VIS_RETAIN_* */
    uint16_t minAltFt;      /* VIS_THRESHOLD_DEFAULT = 15 m exactly */
    uint16_t minSpeedKt10;  /* tenths of a knot; VIS_THRESHOLD_DEFAULT = 8.5 m/s exactly */
    uint16_t staleMinutes;  /* VIS_STALE_MIN_MIN..MAX */
    uint16_t airportRadiusM;
} VisSettings;

/* Copy of the current settings (loaded from NVS "radar"/"gndvis" on first use;
 * missing/invalid = defaults). */
void VisPolicy_GetSettings(VisSettings *out);
void VisPolicy_DefaultSettings(VisSettings *out);
/* Validates and persists; false (nothing changed) for out-of-range values or a storage error. */
bool VisPolicy_SetSettings(const VisSettings *in);
/* Effective thresholds in metric (defaults are exact). */
float VisPolicy_AltThresholdM(const VisSettings *s);
float VisPolicy_SpeedThresholdMs(const VisSettings *s);

/* ---- provider integration ---- */
/* Number of passes the provider parse loop makes over the response: 1 when
 * no ground aircraft can be admitted (defaults), else 2 (ground in pass 1). */
int VisPolicy_Passes(void);
/* Called by AircraftProvider_ParseAircraft around the provider parse. */
void VisPolicy_BeginPoll(void);
void VisPolicy_EndPoll(uint32_t nowMs);
/* 0.0.28 multi-provider form: `provider` is the provider that just polled
 * (-1 = single provider, nothing kept). Its per-airport counts are kept; with combineOther (another provider's list
 * is fresh and was merged in) each airport shows the higher of the two
 * providers' latest counts, so one aircraft reported by both is never counted
 * twice. EndPoll(now) == EndPollFrom(now, -1, false). */
void VisPolicy_EndPollFrom(uint32_t nowMs, int provider, bool combineOther);
/* pass 0: admits airborne aircraft; pass 1: decides on-ground aircraft
 * (classification + airport association). flagPresent/flagOnGround: the
 * provider's own on-ground report. Sets a->visState when admitting. */
bool VisPolicy_Admit(Aircraft *a, bool flagOnGround, int pass);

/* ---- airport counts (latest successful poll) ---- */
typedef struct {
    char name[25];   /* location name ("KSJC San Jose Intl", user name, ...) */
    float latitude;
    float longitude;
    uint16_t associated; /* on-ground aircraft associated with this airport (Y) */
    uint16_t shown;      /* of those, admitted to the radar (X) */
} VisAirportCount;
size_t VisPolicy_AirportCountTotal(void);
bool VisPolicy_GetAirportCount(size_t index, VisAirportCount *out);
/* Totals over all airports for the latest poll. */
void VisPolicy_GetTotals(uint32_t *associated, uint32_t *shown, uint32_t *retained, uint32_t *stale);

/* Host tests: reset all state (settings cache, retention table, counts). */
void VisPolicy_ResetForTest(void);
