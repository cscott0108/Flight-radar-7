#pragma once

/* Multi-provider aircraft-data architecture.
 *
 * This header is the seam between "where the data comes from" (OpenSky,
 * adsb.lol, ...) and "what the rest of the app does with it" (radar
 * rendering, Current Aircraft UI, airport preview). Every provider fills the
 * same normalized Aircraft struct below; nothing outside opensky_client.c /
 * adsblol_client.c should ever look at provider-specific JSON or field
 * layouts.
 *
 * Data flow:
 *   Provider Select (NVS)
 *     -> AircraftProvider (this module) dispatches to the active provider
 *     -> provider-specific HTTP + JSON parsing (opensky_client.c / adsblol_client.c)
 *     -> normalized Aircraft[] (gAircraft), including an optional
 *        AircraftType hint from the provider
 *     -> existing registry/classification resolution (custom_rules.c)
 *     -> existing CraftType_Appearance() / renderer (unchanged)
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "craft_types.h"

#define MAX_AIRCRAFT 200

/* Longest operator/owner name kept per aircraft (matches MAX_OPERATOR_NAME + 1
 * in custom_rules.h so a provider name and a configured name fit the same UI
 * and CSV fields). */
#define AIRCRAFT_OPERATOR_NAME_MAX 40

/* An aircraft this low AND this slow is treated as parked/taxiing ground
 * clutter (or a ground vehicle broadcasting ADS-B) rather than a real
 * selectable target, even if the provider's own on-ground flag is missing.
 * Shared across providers so ground filtering behaves identically regardless
 * of which one is active. */
#define GROUND_ALTITUDE_THRESHOLD_M 15.0f  // ~49 ft
#define GROUND_VELOCITY_THRESHOLD_MS 8.5f  // ~30.6 km/h

/* Aircraft.visState (see visibility_policy.h) */
#define AIRCRAFT_VIS_NORMAL 0 /* airborne by the inflight rule */
#define AIRCRAFT_VIS_GROUND 1 /* on the ground, shown by an airport/retention policy */
#define AIRCRAFT_VIS_STALE 2  /* retained, no longer reported: last reported position until the stale timeout */

typedef struct
{
    char icao24[12];
    char callsign[16];
    /* 0.0.27: 64 -> 52 bytes to make room for `registration` without growing
     * the 200-entry list (the longest OpenSky country name is ~41 chars). */
    char originCountry[52];
    /* Registration (tail number) when the provider sends one (adsb.lol "r";
     * OpenSky never does). Upper-case letters, digits and '-'; "" = unknown.
     * Same width as the TF History record field it is written to. */
    char registration[12];

    CraftType craftType; /* cached classification at last parse; not used for
                           * drawing (radar/preview re-resolve live), kept for
                           * parity with the pre-existing field. */

    float longitude;
    float latitude;

    float altitude;
    float velocity;
    float heading;

    bool valid;
    /* AIRCRAFT_VIS_* (visibility_policy.h): why this aircraft is in the list.
     * 0 = normal airborne aircraft (the only state with default settings).
     * Sits in existing padding, so the struct size is unchanged. */
    uint8_t visState;
    /* 0.0.30: vertical rate in ft/min as reported by the provider (OpenSky
     * vertical_rate, adsb.lol baro_rate / geom_rate), clamped to int16; valid
     * only with AIRCRAFT_DATA_VRATE. Display only (Selected Craft trend arrow).
     * Sits in existing padding: the struct size is unchanged. */
    int16_t verticalRateFpm;

    float predictedLat;
    float predictedLon;

    uint32_t lastUpdateMs;

    /* Automatic Aircraft Type hint from the active provider (see
     * PROJECT_STATE.md "Automatic Aircraft Type Detection"). This is only a
     * hint: an explicit registry rule always wins over it. hasProviderTypeHint
     * is false when the provider gave no usable type/category information
     * (e.g. OpenSky never sets this - its category field is unreliable and
     * unused), in which case providerTypeHint is meaningless and resolution
     * falls through to AIRCRAFT_FIXED_WING. */
    AircraftType providerTypeHint;
    bool hasProviderTypeHint;
    /* 0.0.30: AIRCRAFT_DATA_* bits - which optional values the provider
     * actually sent (a missing value is stored as 0 above, as before). Display
     * only; filtering, ground detection and classification never read it. */
    uint8_t dataFlags;

    /* Operator/owner name as supplied by the provider (empty when the
     * provider gives none - OpenSky never does). Plain printable ASCII with
     * no commas or quotes (see AircraftText_Sanitize), so it is safe to drop
     * into the CSV history and the web UI. This is the "Provider Operator";
     * a configured operator (operators.csv) is looked up separately and never
     * written here. */
    char operatorName[AIRCRAFT_OPERATOR_NAME_MAX];

} Aircraft;

/* Copies src into dst (capacity cap, always NUL-terminated), keeping printable
 * ASCII only, mapping commas/double quotes to spaces (the CSV convention used
 * by the registry and operator files: no quoting), collapsing runs of spaces
 * and trimming. Idempotent. Used for every provider-supplied or file-supplied
 * free-text field. */
static inline void AircraftText_Sanitize(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0)
        return;
    size_t used = 0;
    bool pendingSpace = false;
    for (const char *p = src ? src : ""; *p && used + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ',' || c == '"' || c == ' ' || c == '\t') {
            pendingSpace = (used > 0);
            continue;
        }
        if (c < 0x20 || c > 0x7E)
            continue;
        if (pendingSpace) {
            if (used + 2 >= cap)
                break;
            dst[used++] = ' ';
            pendingSpace = false;
        }
        dst[used++] = (char)c;
    }
    dst[used] = '\0';
}

#define AIRCRAFT_DATA_VELOCITY 0x01 /* velocity was reported */
#define AIRCRAFT_DATA_VRATE 0x02    /* verticalRateFpm was reported */
_Static_assert(sizeof(Aircraft) == 180, "Aircraft must stay 180 bytes (200-entry list in internal RAM)");

/* ft/min from a provider value, clamped to the int16 field. */
static inline int16_t Aircraft_ClampFpm(double fpm)
{
    if (!(fpm == fpm))
        return 0;
    if (fpm > 32000.0)
        return 32000;
    if (fpm < -32000.0)
        return -32000;
    return (int16_t)(fpm < 0 ? fpm - 0.5 : fpm + 0.5);
}

extern Aircraft gAircraft[MAX_AIRCRAFT];
extern int gAircraftCount;

/* ---- provider selection ---- */

typedef enum {
    AIRCRAFT_PROVIDER_OPENSKY = 0,
    AIRCRAFT_PROVIDER_ADSBLOL,
    AIRCRAFT_PROVIDER_COUNT
} AircraftProviderType;

/* ---- diagnostics levels ---- */

typedef enum {
    PROVIDER_DEBUG_OFF = 0,
    PROVIDER_DEBUG_NORMAL,
    PROVIDER_DEBUG_VERBOSE,
    PROVIDER_DEBUG_RAW,
    PROVIDER_DEBUG_COUNT
} ProviderDebugLevel;

/* Call once after nvs_flash_init(), before the web server or aircraft
 * polling starts. Initializes every provider (loads OpenSky credentials,
 * allocates response buffers, etc.) and loads the persisted provider/debug
 * selections from NVS (same "radar" namespace/infrastructure everything
 * else here uses - no second settings store). */
void AircraftProvider_Init(void);

/* Legacy single selection: the first enabled provider (logs, older callers).
 * SetActive enables exactly that one provider. */
AircraftProviderType AircraftProvider_GetActive(void);
void AircraftProvider_SetActive(AircraftProviderType type); /* persists to NVS */

/* ---- independent providers (0.0.28) ----
 *
 * OpenSky and adsb.lol are enabled independently (one, the other or both) and
 * each keeps its own poll interval. Stored in NVS "radar": "prov_mask"
 * (enabled bits) and "int_osky" / "int_adsb" (seconds); changing one never
 * writes the other. Disabled providers keep their interval for later. */
#define AIRCRAFT_PROVIDER_ALL_MASK ((uint8_t)((1u << AIRCRAFT_PROVIDER_COUNT) - 1u))
#define PROVIDER_INTERVAL_DEFAULT_SEC 25u
#define PROVIDER_INTERVAL_MAX_SEC 600u
uint8_t AircraftProvider_EnabledMask(void);
bool AircraftProvider_IsEnabled(AircraftProviderType type);
bool AircraftProvider_SetEnabledMask(uint8_t mask); /* false (nothing changed) when no provider is enabled */
uint32_t AircraftProvider_MinIntervalSecondsFor(AircraftProviderType type); /* OpenSky 10, adsb.lol 5 */
uint32_t AircraftProvider_GetIntervalSeconds(AircraftProviderType type);
bool AircraftProvider_SetIntervalSeconds(AircraftProviderType type, uint32_t seconds); /* false if out of range */
/* First boot after the upgrade: the provider in use stays the only enabled
 * one and takes the old single refresh interval; the other gets the default. */
void AircraftProvider_MigrateLegacyInterval(uint32_t legacySeconds);
/* How old another provider's last list may be and still be merged in
 * (the poll loop sets 2 x that provider's effective interval). */
void AircraftProvider_SetMergeMaxAgeMs(AircraftProviderType type, uint32_t ms);
uint32_t AircraftProvider_MergeMaxAgeMs(AircraftProviderType type);

ProviderDebugLevel AircraftProvider_GetDebugLevel(void);
void AircraftProvider_SetDebugLevel(ProviderDebugLevel level); /* persists to NVS */

const char *AircraftProviderType_Name(AircraftProviderType type);    /* "OpenSky" */
const char *AircraftProviderType_CsvName(AircraftProviderType type); /* "OPENSKY" - stable token */
bool AircraftProviderType_Parse(const char *token, AircraftProviderType *out);

const char *ProviderDebugLevel_Name(ProviderDebugLevel level);
bool ProviderDebugLevel_Parse(const char *token, ProviderDebugLevel *out);

/* ---- dispatch to the active provider ---- */

bool AircraftProvider_HasCredentials(void); /* any enabled provider can poll (adsb.lol needs no credentials) */
uint32_t AircraftProvider_GetRateLimitSeconds(void); /* longest remaining 429 backoff of the enabled providers, 0 if none */
AircraftProviderType AircraftProvider_RateLimitedProvider(void); /* the provider that backoff belongs to */

/* Reuses the existing radar center/range configuration - no separate
 * per-provider location setting. Internally builds whatever query shape the
 * active provider needs (OpenSky: a lat/lon bounding box; adsb.lol: a
 * point+radius query), so provider-specific request formats never leak. */
bool AircraftProvider_GetAircraftJson(
    float centerLat,
    float centerLon,
    float radiusKm,
    const char **json);

bool AircraftProvider_ParseAircraft(const char *json);

/* Per-provider forms (0.0.28). ParseAircraftFor parses one provider's
 * response; with several providers enabled gAircraft is then the ICAO24
 * merge of every provider's latest fresh list (provider_merge.h). */
bool AircraftProvider_HasCredentialsFor(AircraftProviderType type);
uint32_t AircraftProvider_GetRateLimitSecondsFor(AircraftProviderType type);
bool AircraftProvider_GetAircraftJsonFor(AircraftProviderType type, float centerLat, float centerLon, float radiusKm,
                                         const char **json);
bool AircraftProvider_ParseAircraftFor(AircraftProviderType type, const char *json);

/* The active provider's documented safe minimum poll interval; the poll loop
 * in main.c clamps the user-configured interval to this so the UI can't be
 * used to hammer a provider faster than it should be. */
uint32_t AircraftProvider_MinPollIntervalSeconds(void);

/* ---- reusable provider diagnostics ----
 *
 * A small, deliberately simple logging surface so provider implementations
 * don't scatter ad-hoc ESP_LOGx calls. All calls are no-ops below the
 * configured debug level, and Raw-level helpers enforce their own size caps
 * and redact credentials - see PROJECT_STATE.md "Provider diagnostics". */

void ProviderDiag_RequestStart(const char *provider, const char *what, const char *urlRedacted);
void ProviderDiag_RequestDone(const char *provider, const char *what, int httpStatus, size_t bytes, bool ok);
void ProviderDiag_ParseResult(const char *provider, bool parseOk, int rawCount, int normalizedCount, int rejectedCount);
void ProviderDiag_Warning(const char *provider, const char *msg);

/* Verbose-level per-aircraft trace of how the final Aircraft Type was
 * chosen. registryOverrideName is "none" when no registry rule applied. */
void ProviderDiag_TypeResolution(
    const char *icao24,
    const char *providerTypeRaw,   /* provider's raw category/type string, or "" */
    bool hasHint,
    AircraftType hint,
    const char *registryOverrideName, /* AircraftType_Name(...) or "none" */
    AircraftType finalType);

/* Raw/Deep only: a bounded, credential-redacted preview of a response body.
 * Truncates internally; safe to pass the full buffer. */
void ProviderDiag_RawPreview(const char *provider, const char *body, size_t bodyLen);

/* A provider position is usable only if it is a finite point on Earth. Out of
 * range values (a provider bug, a units mix-up, 1e999 overflowing to infinity)
 * would otherwise be projected far off the radar. Both providers reject
 * records that fail this and count them as "rejected". */
static inline bool Aircraft_IsValidPosition(double latitude, double longitude)
{
    return isfinite(latitude) && isfinite(longitude) &&
           latitude >= -90.0 && latitude <= 90.0 &&
           longitude >= -180.0 && longitude <= 180.0;
}
