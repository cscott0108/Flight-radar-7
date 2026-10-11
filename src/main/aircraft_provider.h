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
    char originCountry[48]; /* 0.0.32: 52 -> 48 for the two raw heading fields below (alignment; longest OpenSky name ~41) */
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
    /* Ground track over the ground, degrees clockwise from TRUE north, exactly as
     * the provider reported it (0.0.32 name; was `heading`). Both providers send
     * true track: OpenSky state vector index 10 `true_track`, adsb.lol (readsb)
     * `track`. Raw provider value: never rewritten for display. Any magnetic or
     * display-rotated bearing is derived from it at display time (north_ref.h). */
    float trackTrueDeg;

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
    /* 0.0.32: raw aircraft HEADING (where the nose points, not the track) when
     * the provider sends it: adsb.lol `mag_heading` (magnetic) and
     * `true_heading` (true), in 0.1 degree; valid only with AIRCRAFT_DATA_MAG_HDG /
     * AIRCRAFT_DATA_TRUE_HDG. Kept as reported (never converted into each
     * other). OpenSky sends neither. Retained for diagnostics; the radar draws
     * the track. */
    int16_t magHeadingDeci;
    int16_t trueHeadingDeci;

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
#define AIRCRAFT_DATA_MAG_HDG 0x04  /* magHeadingDeci was reported (adsb.lol mag_heading) */
#define AIRCRAFT_DATA_TRUE_HDG 0x08 /* trueHeadingDeci was reported (adsb.lol true_heading) */
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
/* 0.1.9: elapsedMs added to the existing Normal line (request start to the end of the response). */
void ProviderDiag_RequestDone(const char *provider, const char *what, int httpStatus, size_t bytes, bool ok, uint32_t elapsedMs);
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

/* ---- 0.1.9: per-provider poll diagnostics (recorded always, logged by level; /diag shows the latest) ----
 *
 * Request (fetch) stage, both providers: request number, start (uptime), duration, HTTP status, bytes stored,
 * Content-Length, whether the response was truncated by the bounded response buffer, the transport failure kind
 * (esp_http_client error) and the mbedTLS/esp-tls error code when the request failed. The URL recorded is the
 * exact string handed to esp_http_client (esp_http_client_get_url() is NOT used: it drops the query string).
 *
 * Parse stage (one reason per array element; the first failing check wins, nothing is counted twice):
 *   entries = malformed + missingId + missingPosition + invalidPosition + onGround + airborneAdmitted + notExamined
 *   admitted = airborneAdmitted + groundShown        (groundShown is a subset of onGround)
 * notExamined: elements never looked at because the 200-aircraft list was already full (partial result). */
typedef struct {
    int entries;         /* elements in the provider's aircraft array (0 for "states":null / "ac":null) */
    int malformed;       /* element of the wrong JSON type (OpenSky: not an array; adsb.lol: not an object) */
    int missingId;       /* no ICAO24 / hex string */
    int missingPosition; /* latitude or longitude absent or not a number */
    int invalidPosition; /* latitude/longitude not finite or outside -90..90 / -180..180 */
    int onGround;        /* valid, excluded by the on-ground rule in the airborne pass */
    int groundShown;     /* of onGround: shown by a ground visibility / retention policy (second pass) */
    int notExamined;     /* never examined: the 200-aircraft list was full */
    int admitted;        /* aircraft in this provider's list after parsing (before any cross-provider merge) */
} ProviderParseStats;

typedef enum {
    PPOLL_NONE = 0,        /* nothing recorded yet */
    PPOLL_OK,              /* aircraft list parsed */
    PPOLL_OK_EMPTY,        /* valid response with no aircraft */
    PPOLL_OK_PARTIAL,      /* parsed, but elements were left unexamined because the list was full */
    PPOLL_FETCHED,         /* fetch stage only: HTTP 200 with a complete body (parse not yet recorded) */
    PPOLL_JSON_INVALID,    /* the body is not valid JSON */
    PPOLL_UNEXPECTED_SHAPE,/* valid JSON without the expected "states" / "ac" list */
    PPOLL_HTTP_STATUS,     /* HTTP status other than 200 (not a rate limit) */
    PPOLL_RATE_LIMITED,    /* 429 (adsb.lol also 503): the provider's back-off was started */
    PPOLL_TRANSPORT,       /* esp_http_client_perform failed (see transport kind) */
    PPOLL_TRUNCATED,       /* the body did not fit the bounded response buffer */
    PPOLL_CLIENT_INIT,     /* the HTTP client could not be created */
    PPOLL_IN_PROGRESS,     /* request started, no result yet (the fields below belong to it, not to the previous one) */
    PPOLL_COUNT
} ProviderPollOutcome;

typedef enum {
    PXPORT_NONE = 0,
    PXPORT_TIMEOUT,     /* ESP_ERR_HTTP_EAGAIN / READ_TIMEOUT / CONNECTING (timed out before connecting) */
    PXPORT_CONNECT,     /* ESP_ERR_HTTP_CONNECT: TCP or TLS connection failed (see tlsErr) */
    PXPORT_INCOMPLETE,  /* ESP_ERR_HTTP_INCOMPLETE_DATA: less than Content-Length / last chunk */
    PXPORT_CLOSED,      /* ESP_ERR_HTTP_CONNECTION_CLOSED */
    PXPORT_HEADER,      /* ESP_ERR_HTTP_FETCH_HEADER */
    PXPORT_WRITE,       /* ESP_ERR_HTTP_WRITE_DATA */
    PXPORT_OTHER
} ProviderTransportKind;

typedef struct {
    bool polled;               /* at least one request this boot */
    uint32_t seq;              /* request number this boot (1 = first) */
    uint32_t startUptimeS, durationMs;
    char url[176];             /* as handed to the HTTP client (render it redacted: it holds the radar position) */
    int httpStatus;            /* 0 = no HTTP response */
    uint32_t bytes;            /* body bytes stored */
    uint32_t received;         /* body bytes received (> bytes when truncated) */
    int64_t contentLength;     /* -1 = not known (chunked or no response) */
    bool complete;             /* esp_http_client_is_complete_data_received() after a successful perform */
    bool truncated;
    ProviderPollOutcome fetchOutcome, outcome; /* outcome = final (fetch, then parse) */
    ProviderTransportKind transport;
    int espErr, tlsErr, tlsFlags;
    bool haveParse;
    ProviderParseStats parse;
    int jsonErrorOffset;       /* PPOLL_JSON_INVALID: byte offset where cJSON stopped, -1 unknown */
    bool haveSuccess;
    uint32_t lastSuccessUptimeS;
    bool haveError;
    char lastError[80];
    uint32_t lastErrorUptimeS;
} ProviderPollDiag;

/* Filled by the provider clients (RadarTask). */
typedef struct {
    AircraftProviderType type;
    uint32_t t0Ms;
    int httpStatus;
    uint32_t bytes, received;
    int64_t contentLength;
    bool complete, truncated;
    int espErr, tlsErr, tlsFlags;
    ProviderTransportKind transport;
} ProviderFetch;

const char *ProviderPollOutcome_Name(ProviderPollOutcome o);
const char *ProviderTransportKind_Name(ProviderTransportKind k);
/* Starts request #N (returns N) and records the URL. No allocation; never fails. */
uint32_t ProviderDiag_FetchStart(ProviderFetch *f, AircraftProviderType type, const char *url);
/* Records the fetch result. Logs at Normal only when the request did not succeed (outcome, transport kind,
 * TLS code), and a detail line at Verbose. */
void ProviderDiag_FetchEnd(const ProviderFetch *f, ProviderPollOutcome outcome);
/* Records the parse result (final outcome) and logs the stage counts at Verbose. If the fetch stage flagged a
 * truncated body, a failed parse is recorded as PPOLL_TRUNCATED. */
void ProviderDiag_ParseStats(AircraftProviderType type, ProviderPollOutcome outcome, const ProviderParseStats *s,
                             int jsonErrorOffset);
/* Normal and above: the same heap figures OpenSky logs before its TLS session, for the other providers. */
void ProviderDiag_HeapBeforeTls(const char *provider);
/* Raw only: one bounded record sample (callers allow at most PROVIDER_DIAG_SAMPLES_MAX per poll). */
#define PROVIDER_DIAG_SAMPLES_MAX 8
void ProviderDiag_Sample(const char *provider, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* /diag: a copy of the latest record (false when none is available). */
bool ProviderDiag_GetPoll(AircraftProviderType type, ProviderPollDiag *out);
/* Allocates the record (PSRAM, once); called by AircraftProvider_Init. Without it nothing is recorded. */
void ProviderPollDiag_Init(void);

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
