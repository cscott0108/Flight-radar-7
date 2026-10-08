#pragma once
/*
 * Radar north reference (0.0.32): TRUE vs MAGNETIC, and the local magnetic
 * variation (declination) that converts between them.
 *
 * Semantics (one model for the whole firmware):
 *  - Raw provider data is never rewritten. Aircraft.trackTrueDeg is the
 *    provider's true track; adsb.lol mag/true heading are kept as reported.
 *  - Geography (positions, the projection) is true. The DISPLAY may be rotated
 *    so that screen-up is magnetic north: display bearing = true bearing - R,
 *    where R = NorthRef_DisplayRotationDeg() (declination when the resolved
 *    reference is MAGNETIC, 0 when TRUE).
 *  - Runway designators ("30", "12L") are magnetic headings / 10, so a
 *    designator axis becomes true by adding the declination. A 1-degree
 *    rotation override is an exact true axis and is used as is.
 *  - Declination D is east-positive: true = magnetic + D.
 *
 * Variation source: the World Magnetic Model 2025 (NOAA NCEI / BGS, public
 * domain) evaluated on the device. No network. The result is cached in SPIFFS
 * (NORTHREF_CACHE_PATH) with its location, value and computation date, so it is
 * available right after boot, before the clock syncs.
 *
 * Modes: AUTO (default) resolves to MAGNETIC whenever WMM gives a valid,
 * reliable value for the radar location; otherwise TRUE, flagged as a fallback
 * on /diag. WMM's own validity conditions decide "valid/reliable": the model's
 * 5-year life (2025.0-2030.0) and its blackout zone (horizontal field < 2000 nT,
 * near the magnetic poles). MAGNETIC and TRUE are explicit choices (MAGNETIC
 * still falls back to TRUE if no declination exists at all).
 */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NORTH_REF_AUTO = 0,
    NORTH_REF_MAGNETIC = 1,
    NORTH_REF_TRUE = 2,
    NORTH_REF_MODE_COUNT
} NorthRefMode;

typedef enum {
    NORTH_RESOLVED_TRUE = 0,
    NORTH_RESOLVED_MAGNETIC = 1
} NorthResolved;

/* ---- WMM2025 (pure, host-testable) ---- */
#define WMM_EPOCH 2025.0
#define WMM_LIFE_YEARS 5.0
#define WMM_BLACKOUT_NT 2000.0 /* WMM: compass unreliable where H < 2000 nT */
#define WMM_CAUTION_NT 6000.0  /* WMM: large uncertainty where H < 6000 nT */

typedef struct {
    double declDeg; /* east positive */
    double inclDeg;
    double horizNt; /* horizontal intensity H */
    double totalNt;
    bool blackout;  /* H < 2000 nT */
    bool caution;   /* H < 6000 nT */
} WmmResult;

/* Geodetic lat/lon in degrees, altitude km above the WGS-84 ellipsoid, decimal
 * year. False (out untouched) when the inputs are invalid or the date is
 * outside the model life. Uses a transient ~8 KB work buffer (PSRAM on the
 * device), freed before returning; nothing permanent. */
bool Wmm_Compute(double latDeg, double lonDeg, double altKm, double decYear, WmmResult *out);
/* UTC epoch seconds -> decimal year (e.g. 2026.77). */
double Wmm_DecimalYear(int64_t utcSeconds);

/* ---- radar north reference state ---- */
#ifndef NORTHREF_CACHE_PATH
#define NORTHREF_CACHE_PATH "/spiffs/magvar.dat"
#endif
/* A cached value applies to the radar centre if both coordinates are within
 * this many degrees (~1 km); any larger move recomputes. */
#define NORTHREF_SAME_PLACE_DEG 0.01f
/* Recompute a valid value after this long (WMM secular change is ~0.1 deg/yr);
 * done in the zero-aircraft maintenance window, never during normal polling. */
#define NORTHREF_STALE_SEC (365LL * 24 * 3600)

typedef struct {
    NorthRefMode mode;
    NorthResolved resolved;
    bool varValid;        /* a declination for the current radar centre is known */
    float declDeg;
    float horizNt;
    float lat, lon;       /* location the declination applies to */
    float decYear;        /* model date used */
    int64_t computedUtc;  /* 0 = date was estimated (clock not synced) */
    bool dateEstimated;
    bool fromCache;       /* value came from the SPIFFS cache (not recomputed this boot) */
    bool blackout, caution;
    bool fallback;        /* AUTO/MAGNETIC wanted magnetic but shows TRUE */
    const char *fallbackReason;
    const char *cacheState; /* "loaded", "missing", "other location", "invalid", "written", "write failed", ... */
    bool stale;
    uint32_t computeCount;
    int64_t lastAttemptUtc;
    uint32_t lastAttemptUptimeSec;
    bool lastAttemptOk;
    const char *lastResult;
} NorthRefStatus;

/* Boot (after SPIFFS and NVS are up): loads the mode (NVS "radar"/"northref")
 * and the cache, computes if nothing valid applies. Never blocks on network. */
void NorthRef_Init(float radarLat, float radarLon);
NorthRefMode NorthRef_GetMode(void);
bool NorthRef_SetMode(NorthRefMode mode); /* persists; applies immediately */
/* Radar centre changed: the old location's value stops applying at once and
 * the new one is computed (or taken from the cache if it is the same place). */
void NorthRef_OnLocationChanged(float radarLat, float radarLon);
/* RadarTask, every slice (cheap flag tests): recomputes once after the clock
 * first syncs if the value used an estimated date, and recomputes a stale value
 * only when maintenanceWindow (zero-aircraft idle maintenance) is true. */
void NorthRef_Service(bool maintenanceWindow);

/* Readers (any task; short critical section). */
NorthResolved NorthRef_Resolved(void);
float NorthRef_DisplayRotationDeg(void);  /* R: display bearing = true - R */
bool NorthRef_Declination(float *declDegOut); /* false if none known */
/* True bearing -> display bearing, normalized to [0,360). */
float NorthRef_TrueToDisplay(float trueBearingDeg);
/* Designator (magnetic) runway axis -> true axis (declination added; unchanged
 * when no declination is known). */
float NorthRef_DesignatorToTrue(float magneticAxisDeg);
void NorthRef_GetStatus(NorthRefStatus *out);
const char *NorthRefMode_Name(NorthRefMode mode);

/* Pure resolution rule (exposed for tests). */
NorthResolved NorthRef_ResolveFor(NorthRefMode mode, bool varValid, bool blackout, bool *fallbackOut,
                                  const char **reasonOut);

/* Host tests. */
typedef struct {
    int64_t (*utc)(void);           /* 0 = clock not synced */
    uint32_t (*upSec)(void);
    double (*buildYear)(void);      /* decimal year used when no clock and no cache */
} NorthRefHooks;
void NorthRef_SetHooks(const NorthRefHooks *h);
void NorthRef_ResetForTest(void);
