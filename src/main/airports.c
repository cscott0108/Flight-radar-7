#include "airports.h"

#include <ctype.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "builtin_airports.h"

static const char *TAG = "Airports";

#define AIRPORT_NAMESPACE "airports"
#define AIRPORT_KEY "markers"
#define AIRPORT_VERSION 3 /* 2 added AirportMarker.color; 3 added markerMode + runway */
/* 0.0.23 added marker types H (2) and Square (3) as new markerMode VALUES in
 * the same uint8_t field. The blob layout and size are unchanged, so it is
 * still version 3 and every existing v1/v2/v3 blob loads exactly as before.
 * (Firmware older than 0.0.23 rejects a v3 blob that contains an H or Square
 * marker; the blob itself is not modified by that rejection.) */
#define AIRPORT_VERSION_1 1
#define AIRPORT_VERSION_2 2

/* Fixed at the cap MAX_AIRPORTS actually was when v1/v2 were the on-disk
 * format (10). These two legacy struct definitions exist ONLY to detect and
 * upgrade an old saved blob by its exact byte size (see Airports_Init), so
 * they must always reflect what old firmware actually wrote to flash - NOT
 * today's MAX_AIRPORTS. Raising MAX_AIRPORTS later (as this session did,
 * 10 -> 100) must never change these two struct sizes, or the size-based
 * version detection below silently breaks for anyone upgrading from a v1/v2
 * install. */
#define AIRPORT_LEGACY_MAX_V1V2 10

/* Version-1 layout (no color). Kept only so an existing saved blob can be
 * read and upgraded instead of being discarded. Must match the old
 * AirportMarker / StoredAirports exactly. */
typedef struct {
    char name[AIRPORT_NAME_LENGTH + 1];
    float latitude;
    float longitude;
    uint8_t diameter;
} AirportMarkerV1;

typedef struct {
    uint16_t version;
    uint16_t count;
    AirportMarkerV1 markers[AIRPORT_LEGACY_MAX_V1V2];
} StoredAirportsV1;

/* Version-2 layout (color, but no marker mode / runway). Kept only so an
 * existing saved blob can be read and upgraded instead of being discarded.
 * Must match the pre-existing (version 2) AirportMarker / StoredAirports
 * exactly. */
typedef struct {
    char name[AIRPORT_NAME_LENGTH + 1];
    float latitude;
    float longitude;
    uint8_t diameter;
    uint32_t color;
} AirportMarkerV2;

typedef struct {
    uint16_t version;
    uint16_t count;
    AirportMarkerV2 markers[AIRPORT_LEGACY_MAX_V1V2];
} StoredAirportsV2;

typedef struct {
    uint16_t version;
    uint16_t count;
    AirportMarker markers[MAX_AIRPORTS];
} StoredAirports;

/* Any of the three on-disk layouts fits in this union; NVS reports the real
 * stored size, which is what actually distinguishes them (all three start
 * with the same {version,count} header). */
typedef union {
    StoredAirports v3;
    StoredAirportsV2 v2;
    StoredAirportsV1 v1;
} StoredAirportsUnion;

static StoredAirports stored;
static SemaphoreHandle_t airportsLock;

/* Regional built-in selection (see Airports_SelectBuiltins). Guarded by
 * airportsLock. Only table indices are kept (2 bytes each, 500 bytes total);
 * the airport data stays in flash. */
static uint16_t builtinSel[AIRPORT_ACTIVE_MAX];
static size_t builtinSelCount;
static bool builtinSelValid;
static float builtinSelLat, builtinSelLon, builtinSelRange;
static uint32_t userRevision;    /* bumped whenever `stored` changes */
static uint32_t builtinSelUserRevision;
static uint32_t builtinSelReplaced;     /* in range but replaced by a user location naming the ident */
static uint32_t builtinSelOverCapacity; /* in range, not replaced, but beyond the active-location limit */
static uint8_t builtinSelOvr[AIRPORT_ACTIVE_MAX]; /* override slot per selected airport, 0xFF = none */
static uint32_t builtinSelOvrRevision;

/* Built-in airport overrides (see airports.h). One table, guarded by
 * airportsLock, allocated once in PSRAM (project convention for CPU-only
 * buffers; 2,004 bytes). Its first `count` records are exactly the NVS blob,
 * so it is written as-is (4 + 20 * count bytes; version-1 blobs of 16-byte
 * records are upgraded when loaded). */
#define AIRPORT_OVERRIDE_KEY "bi_ovr"
#define AIRPORT_OVERRIDE_VERSION 2 /* 2 (0.0.28) added the color field: 20-byte records */
#define AIRPORT_OVERRIDE_NONE 0xFF
typedef struct {
    uint16_t version;
    uint16_t count;
    AirportBuiltinOverride records[AIRPORT_OVERRIDE_MAX];
} StoredOverrides;
_Static_assert(sizeof(AirportBuiltinOverride) == 20, "override record must stay 20 bytes (NVS layout v2)");
/* Version-1 record (0.0.25-0.0.27): same fields without color. Read only, to
 * upgrade an existing blob; the next change writes version 2. */
typedef struct {
    char icao[4];
    uint8_t fields;
    uint8_t reserved;
    uint16_t rotationDeg;
    int32_t latE5;
    int32_t lonE5;
} AirportBuiltinOverrideV1;
_Static_assert(sizeof(AirportBuiltinOverrideV1) == 16, "v1 override record is 16 bytes");
_Static_assert(AIRPORT_OVERRIDE_MAX < AIRPORT_OVERRIDE_NONE, "override slot must fit uint8_t");
static StoredOverrides *overrides;
static void MatchOverridesLocked(void);
static uint32_t overrideRevision; /* bumped whenever the override table changes */

/* Phase-2-audit fix: at MAX_AIRPORTS=100, sizeof(StoredAirports) is 5204
 * bytes - too big for this project's ~3.5 KB main/httpd task stacks (a real,
 * confirmed-on-hardware stack overflow the moment MAX_AIRPORTS was raised
 * past 10). The previous fix for that (making each of these buffers `static`)
 * traded a stack overflow for a much larger, permanent regression: six extra
 * 5204-byte buffers living in .bss for the entire life of the device is
 * ~31 KB of internal RAM gone forever, on top of the one copy (`stored`
 * above) that actually needs to be permanent. That is internal heap the
 * WiFi/TLS stack and the web server both need, and losing it is what was
 * producing "esp-aes: Failed to allocate memory" during the OpenSky HTTPS
 * write and making the web UI unreachable under the same memory pressure.
 *
 * The actual requirement is just "don't put 5+ KB on a 3.5 KB stack" - it
 * does not require the buffer to exist permanently. These are all rare,
 * serialized operations (boot-time load/upgrade/seed, or an admin editing
 * airports), so a heap_caps_malloc'd scratch buffer, pinned to PSRAM (6 MB
 * free, and this project's existing convention for exactly this situation -
 * see opensky_client.c's responseBuffer) and freed immediately after use,
 * gives the same stack-safety with none of the permanent cost. */
static StoredAirportsUnion *AllocScratch(void)
{
    StoredAirportsUnion *p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        ESP_LOGE(TAG, "Could not allocate a %u-byte PSRAM scratch buffer for airport storage",
                 (unsigned)sizeof(*p));
    return p;
}

/* Default dataset for a fresh install only (never overwrites an existing
 * saved list, however small): real Northern California airport identifiers,
 * names and published approximate coordinates, from general reference
 * knowledge rather than a live, on-device-verified source. Rounded to 4
 * decimal places (~11 m) - plenty for a radar overlay marker, but treat
 * these as a reasonable starting point, not surveyed positions; edit any
 * entry via the existing /airports form if better data is available. All
 * seeded in Dot mode with no runway - directional markers need a verified
 * runway heading per airport, which this list does not attempt to guess
 * (per the existing "never draw a guessed axis" rule). Roughly ordered
 * nearest-to-farthest from the project's default radar center
 * (37.3851, -122.0020, San Mateo County); a handful of major hub airports
 * are included at the end for usefulness at large configured ranges. */
static const struct {
    const char *name;
    float lat;
    float lon;
} kDefaultAirports[] = {
    {"KSQL San Carlos", 37.5119f, -122.2495f},
    {"KPAO Palo Alto", 37.4611f, -122.1150f},
    {"KNUQ Moffett Field", 37.4161f, -122.0496f},
    {"KHWD Hayward Exec", 37.6592f, -122.1219f},
    {"KSFO San Francisco Intl", 37.6213f, -122.3790f},
    {"KOAK Oakland Intl", 37.7126f, -122.2197f},
    {"KHAF Half Moon Bay", 37.5133f, -122.5003f},
    {"KSJC San Jose Intl", 37.3626f, -121.9291f},
    {"KRHV Reid-Hillview", 37.3329f, -121.8197f},
    {"KLVK Livermore Muni", 37.6934f, -121.8203f},
    {"KCCR Buchanan Field", 37.9897f, -122.0569f},
    {"KDVO Gnoss Field", 38.1447f, -122.5539f},
    {"KAPC Napa County", 38.2128f, -122.2808f},
    {"KO69 Petaluma Muni", 38.2536f, -122.6099f},
    {"KSTS Sonoma County", 38.5090f, -122.8134f},
    {"KVCB Nut Tree", 38.3778f, -121.9611f},
    {"KEDU Univ (Davis)", 38.5352f, -121.7789f},
    {"KWVI Watsonville Muni", 36.9358f, -121.7900f},
    {"KMRY Monterey Regional", 36.5870f, -121.8429f},
    {"KSNS Salinas Muni", 36.6628f, -121.6067f},
    {"KSCK Stockton Metro", 37.8942f, -121.2380f},
    {"KTCY Tracy Muni", 37.6897f, -121.4467f},
    {"KLOD Lodi", 38.0894f, -121.2661f},
    {"KRIU Rancho Murieta", 38.4826f, -121.1017f},
    {"KSAC Sacramento Exec", 38.5125f, -121.4933f},
    {"KMHR Sacramento Mather", 38.5538f, -121.2980f},
    {"KMCC McClellan (Sac)", 38.6673f, -121.4009f},
    {"KSMF Sacramento Intl", 38.6954f, -121.5908f},
    {"KO88 Woodland Muni", 38.6717f, -121.8536f},
    {"KAUN Auburn Muni", 38.9548f, -121.0822f},
    {"K1O2 Lincoln Regional", 38.9027f, -121.3502f},
    {"KGOO Grass Valley", 39.2698f, -121.0175f},
    {"KPVF Placerville", 38.7228f, -120.7538f},
    {"KJAQ Jackson (Amador)", 38.3453f, -120.7788f},
    {"KO22 Columbia", 38.0339f, -120.4152f},
    {"KTVL Lake Tahoe", 38.8938f, -119.9953f},
    {"KTRK Truckee-Tahoe", 39.3199f, -120.1399f},
    {"KUKI Ukiah Muni", 39.1260f, -123.2003f},
    {"KO61 Cloverdale Muni", 38.8267f, -123.0128f},
    {"KWLW Willows-Glenn", 39.5145f, -122.2178f},
    {"KCIC Chico Muni", 39.7954f, -121.8584f},
    {"KOVE Oroville Muni", 39.4898f, -121.6161f},
    {"KMYV Yuba County", 39.0973f, -121.5704f},
    {"KRDD Redding Muni", 40.5090f, -122.2934f},
    {"KMOD Modesto City-Co", 37.6258f, -120.9541f},
    {"KMCE Merced Regional", 37.2846f, -120.5138f},
    {"KFAT Fresno Yosemite", 36.7762f, -119.7181f},
    /* Major hubs, useful once the radar range is turned up. */
    {"KLAX Los Angeles Intl", 33.9425f, -118.4081f},
    {"KSAN San Diego Intl", 32.7338f, -117.1933f},
    {"KLAS Las Vegas Reid Intl", 36.0840f, -115.1537f},
    {"KPHX Phoenix Sky Harbor", 33.4342f, -112.0116f},
    {"KPDX Portland Intl", 45.5898f, -122.5951f},
    {"KSEA Seattle-Tacoma Intl", 47.4502f, -122.3088f},
};
#define DEFAULT_AIRPORT_COUNT (sizeof(kDefaultAirports) / sizeof(kDefaultAirports[0]))

/* Runway text itself only needs bounds/character checking here (used to
 * decide whether a saved blob or a form submission is even well-formed
 * enough to store). Whether it actually names a usable physical axis is a
 * separate, render-time question - see Airport_ParseRunwayAxis - so that an
 * airport saved with a since-edited-out-of-range or empty runway simply
 * falls back to the dot instead of ever being rejected as "invalid" here. */
static bool ValidRunwayText(const char *runway)
{
    if (!memchr(runway, '\0', AIRPORT_RUNWAY_LENGTH + 1))
        return false;
    size_t length = strlen(runway);
    if (length > AIRPORT_RUNWAY_LENGTH)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)runway[i];
        if (!((c >= '0' && c <= '9') || c == 'L' || c == 'C' || c == 'R' ||
              c == 'l' || c == 'c' || c == 'r'))
            return false;
    }
    return true;
}

static bool ValidMarker(const AirportMarker *marker)
{
    if (!marker || !memchr(marker->name, '\0', sizeof(marker->name)))
        return false;
    size_t length = strlen(marker->name);
    if (length == 0 || length > AIRPORT_NAME_LENGTH)
        return false;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)marker->name[i] < 32 || (unsigned char)marker->name[i] > 126)
            return false;
    return isfinite(marker->latitude) && isfinite(marker->longitude) &&
           marker->latitude >= -90.0f && marker->latitude <= 90.0f &&
           marker->longitude >= -180.0f && marker->longitude <= 180.0f &&
           marker->diameter >= 6 && marker->diameter <= 24 &&
           marker->color <= 0xFFFFFFu &&
           Airport_MarkerTypeValid(marker->markerMode) &&
           ValidRunwayText(marker->runway);
}

/* Bytes of the v3 blob for `count` locations: the {version,count} header plus
 * only the used markers (0.0.32). Until 0.0.31 every save wrote the whole
 * 100-slot array (4 + 100 x 52 = 5,204 B) whatever the count; NVS writes the
 * new copy before erasing the old one, so each save needed ~5.2 KB free in the
 * 24 KB nvs partition and failed with "storage error" on a nearly full
 * partition even with 7 locations. Both lengths load (see Airports_Init). */
static size_t StoredBlobSize(size_t count)
{
    return offsetof(StoredAirports, markers) + count * sizeof(AirportMarker);
}

/* Last Save/Delete failure, for an accurate WebUI message (guarded by airportsLock). */
static esp_err_t s_lastError = ESP_OK;

static bool SaveStored(const StoredAirports *next)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AIRPORT_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        s_lastError = err;
        return false;
    }
    err = nvs_set_blob(handle, AIRPORT_KEY, next, StoredBlobSize(next->count));
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    s_lastError = err;
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Saving %u location(s) (%u bytes) failed: %s", (unsigned)next->count,
                 (unsigned)StoredBlobSize(next->count), esp_err_to_name(err));
    return err == ESP_OK;
}

/* Fresh-install only: called from Airports_Init() the two ways "nothing has
 * ever been saved" shows up (no NVS namespace yet, or the namespace exists
 * but this key doesn't). Uses the exact same SaveStored() the rest of this
 * file uses - no parallel persistence path - written once as a single blob
 * rather than DEFAULT_AIRPORT_COUNT separate saves. Failure here is not
 * fatal to Airports_Init(): the device just starts with an empty list, same
 * as before this dataset existed. */
static void SeedDefaultAirports(void)
{
    _Static_assert(DEFAULT_AIRPORT_COUNT <= MAX_AIRPORTS,
                   "default airport list no longer fits MAX_AIRPORTS");
    /* Heap/PSRAM scratch buffer, not a permanent static - see AllocScratch(). */
    StoredAirportsUnion *scratch = AllocScratch();
    if (!scratch)
        return; /* leave `stored` as the empty default set by the caller */
    StoredAirports *seeded = &scratch->v3;
    memset(seeded, 0, sizeof(*seeded));
    seeded->version = AIRPORT_VERSION;
    seeded->count = (uint16_t)DEFAULT_AIRPORT_COUNT;
    for (size_t i = 0; i < DEFAULT_AIRPORT_COUNT; i++) {
        AirportMarker *m = &seeded->markers[i];
        strncpy(m->name, kDefaultAirports[i].name, AIRPORT_NAME_LENGTH);
        m->name[AIRPORT_NAME_LENGTH] = '\0';
        m->latitude = kDefaultAirports[i].lat;
        m->longitude = kDefaultAirports[i].lon;
        m->diameter = AIRPORT_DEFAULT_DIAMETER;
        m->color = AIRPORT_DEFAULT_COLOR;
        m->markerMode = AIRPORT_MARKER_DOT;
        m->runway[0] = '\0';
        if (!ValidMarker(m)) {
            ESP_LOGE(TAG, "Default airport '%s' failed validation; skipping seed", kDefaultAirports[i].name);
            free(scratch);
            return; /* leave `stored` as the empty default set by the caller */
        }
    }
    if (SaveStored(seeded)) {
        stored = *seeded;
        userRevision++;
    }
    free(scratch);
}

static bool ValidOverride(const AirportBuiltinOverride *o)
{
    return o->icao[0] && (o->fields & ~AIRPORT_OVR_ALL) == 0 && o->fields != 0 && o->reserved == 0 &&
           (!(o->fields & AIRPORT_OVR_ROTATION) || o->rotationDeg < 180) &&
           (!(o->fields & AIRPORT_OVR_LAT) || (o->latE5 >= -9000000 && o->latE5 <= 9000000)) &&
           (!(o->fields & AIRPORT_OVR_LON) || (o->lonE5 >= -18000000 && o->lonE5 <= 18000000)) &&
           ((o->fields & AIRPORT_OVR_COLOR) ? o->color <= 0xFFFFFFu : o->color == 0);
}

/* Loads the override blob (missing key = no overrides). A malformed blob is
 * ignored, not modified. Independent of the user-location blob. */
static void LoadOverrides(void)
{
    if (!overrides) {
        overrides = heap_caps_malloc(sizeof(*overrides), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!overrides) {
            ESP_LOGE(TAG, "Could not allocate the %u-byte built-in override table; overrides disabled",
                     (unsigned)sizeof(*overrides));
            return;
        }
    }
    memset(overrides, 0, sizeof(*overrides));
    overrides->version = AIRPORT_OVERRIDE_VERSION;
    overrideRevision++;
    nvs_handle_t handle;
    if (nvs_open(AIRPORT_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return;
    StoredOverrides *scratch = heap_caps_malloc(sizeof(*scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch) {
        nvs_close(handle);
        return;
    }
    memset(scratch, 0, sizeof(*scratch));
    size_t size = sizeof(*scratch);
    esp_err_t err = nvs_get_blob(handle, AIRPORT_OVERRIDE_KEY, scratch, &size);
    nvs_close(handle);
    bool ok = err == ESP_OK && size >= 4 && scratch->count <= AIRPORT_OVERRIDE_MAX;
    if (ok && scratch->version == 1 && size == 4 + (size_t)scratch->count * sizeof(AirportBuiltinOverrideV1)) {
        /* Upgrade in RAM: v1 records are packed 16 bytes apart right after
         * the header. Walk backwards so no record is overwritten before it is
         * read (v2 records are larger). */
        const uint8_t *base = (const uint8_t *)scratch->records;
        for (int i = (int)scratch->count - 1; i >= 0; i--) {
            AirportBuiltinOverrideV1 v1;
            memcpy(&v1, base + (size_t)i * sizeof(v1), sizeof(v1));
            AirportBuiltinOverride v2 = {0};
            memcpy(v2.icao, v1.icao, sizeof(v2.icao));
            v2.fields = v1.fields;
            v2.reserved = v1.reserved;
            v2.rotationDeg = v1.rotationDeg;
            v2.latE5 = v1.latE5;
            v2.lonE5 = v1.lonE5;
            scratch->records[i] = v2;
        }
        scratch->version = AIRPORT_OVERRIDE_VERSION;
        for (size_t i = 0; ok && i < scratch->count; i++) /* v1 had no color bit */
            ok = (scratch->records[i].fields & ~0x0Fu) == 0;
    } else {
        ok = ok && scratch->version == AIRPORT_OVERRIDE_VERSION &&
             size == 4 + (size_t)scratch->count * sizeof(AirportBuiltinOverride);
    }
    for (size_t i = 0; ok && i < scratch->count; i++)
        ok = ValidOverride(&scratch->records[i]);
    if (ok)
        *overrides = *scratch;
    else if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW(TAG, "Ignoring malformed built-in override blob");
    free(scratch);
}

bool Airports_Init(void)
{
    if (!airportsLock)
        airportsLock = xSemaphoreCreateMutex();
    if (!airportsLock)
        return false;
    LoadOverrides();
    memset(&stored, 0, sizeof(stored));
    stored.version = AIRPORT_VERSION;
    userRevision++;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(AIRPORT_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        SeedDefaultAirports(); /* fresh install: no namespace has ever existed */
        return true;
    }
    if (err != ESP_OK)
        return false;

    /* Heap/PSRAM scratch buffer, not a permanent static - see AllocScratch().
     * Any of the three on-disk layouts fits in this buffer; NVS reports the
     * real stored size, which is what actually distinguishes them (all three
     * start with the same {version,count} header). Safe to free before
     * returning on every path below: Airports_Init() runs once, synchronously,
     * at boot, before anything else touches airport state, so there is never
     * a concurrent user of this buffer to worry about. */
    StoredAirportsUnion *scratch = AllocScratch();
    if (!scratch)
        return false;
    memset(scratch, 0, sizeof(*scratch));
    size_t size = sizeof(*scratch);
    err = nvs_get_blob(handle, AIRPORT_KEY, scratch, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        free(scratch);
        SeedDefaultAirports(); /* namespace exists, but this key was never saved */
        return true;
    }
    if (err != ESP_OK) {
        free(scratch);
        return false;
    }

    if (size == sizeof(StoredAirportsV1) && scratch->v1.version == AIRPORT_VERSION_1) {
        /* Upgrade: every existing airport keeps its data and gets the original red,
         * dot mode, and no runway. The blob is rewritten in the new layout the
         * next time an airport is saved. */
        if (scratch->v1.count > AIRPORT_LEGACY_MAX_V1V2) {
            free(scratch);
            return false;
        }
        /* Second heap/PSRAM scratch buffer for the upgrade target - `scratch`
         * above still holds the v1 source data we're reading from, so it
         * can't double as the destination too. */
        StoredAirportsUnion *upgradedScratch = AllocScratch();
        if (!upgradedScratch) {
            free(scratch);
            return false;
        }
        StoredAirports *upgraded = &upgradedScratch->v3;
        memset(upgraded, 0, sizeof(*upgraded));
        upgraded->version = AIRPORT_VERSION;
        upgraded->count = scratch->v1.count;
        bool ok = true;
        for (size_t i = 0; i < scratch->v1.count; i++) {
            memcpy(upgraded->markers[i].name, scratch->v1.markers[i].name, sizeof(upgraded->markers[i].name));
            upgraded->markers[i].latitude = scratch->v1.markers[i].latitude;
            upgraded->markers[i].longitude = scratch->v1.markers[i].longitude;
            upgraded->markers[i].diameter = scratch->v1.markers[i].diameter;
            upgraded->markers[i].color = AIRPORT_DEFAULT_COLOR;
            upgraded->markers[i].markerMode = AIRPORT_MARKER_DOT;
            /* runway[] already zeroed by memset above */
            if (!ValidMarker(&upgraded->markers[i])) {
                ok = false;
                break;
            }
        }
        if (ok)
            stored = *upgraded;
        userRevision++;
        free(scratch);
        free(upgradedScratch);
        return ok;
    }

    if (size == sizeof(StoredAirportsV2) && scratch->v2.version == AIRPORT_VERSION_2) {
        /* Upgrade: every existing airport keeps its name/position/size/color,
         * starts in dot mode with no runway (matches its on-screen appearance
         * exactly, since dot was the only mode before this version existed). */
        if (scratch->v2.count > AIRPORT_LEGACY_MAX_V1V2) {
            free(scratch);
            return false;
        }
        StoredAirportsUnion *upgradedScratch = AllocScratch();
        if (!upgradedScratch) {
            free(scratch);
            return false;
        }
        StoredAirports *upgraded = &upgradedScratch->v3;
        memset(upgraded, 0, sizeof(*upgraded));
        upgraded->version = AIRPORT_VERSION;
        upgraded->count = scratch->v2.count;
        bool ok = true;
        for (size_t i = 0; i < scratch->v2.count; i++) {
            memcpy(upgraded->markers[i].name, scratch->v2.markers[i].name, sizeof(upgraded->markers[i].name));
            upgraded->markers[i].latitude = scratch->v2.markers[i].latitude;
            upgraded->markers[i].longitude = scratch->v2.markers[i].longitude;
            upgraded->markers[i].diameter = scratch->v2.markers[i].diameter;
            upgraded->markers[i].color = scratch->v2.markers[i].color;
            upgraded->markers[i].markerMode = AIRPORT_MARKER_DOT;
            if (!ValidMarker(&upgraded->markers[i])) {
                ok = false;
                break;
            }
        }
        if (ok)
            stored = *upgraded;
        userRevision++;
        free(scratch);
        free(upgradedScratch);
        return ok;
    }

    /* v3: the used markers only (0.0.32) or, as written up to 0.0.31, the full
     * 100-slot array. Neither length can be mistaken for v1 (404 B) or v2 (444 B),
     * and the version field must match as well. */
    bool ok = scratch->v3.version == AIRPORT_VERSION && scratch->v3.count <= MAX_AIRPORTS &&
              (size == StoredBlobSize(scratch->v3.count) || size == sizeof(StoredAirports));
    for (size_t i = 0; ok && i < scratch->v3.count; i++)
        if (!ValidMarker(&scratch->v3.markers[i]))
            ok = false;
    if (ok)
        stored = scratch->v3;
    userRevision++;
    free(scratch);
    return ok;
}

esp_err_t Airports_LastError(void)
{
    if (!airportsLock)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    esp_err_t e = s_lastError;
    xSemaphoreGive(airportsLock);
    return e;
}

size_t Airports_StoredBlobBytes(void)
{
    return StoredBlobSize(Airports_Count());
}

size_t Airports_Count(void)
{
    if (!airportsLock)
        return 0;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    size_t count = stored.count;
    xSemaphoreGive(airportsLock);
    return count;
}

bool Airports_Get(size_t index, AirportMarker *out)
{
    if (!airportsLock || !out)
        return false;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    bool found = index < stored.count;
    if (found)
        *out = stored.markers[index];
    xSemaphoreGive(airportsLock);
    return found;
}

bool Airports_Save(int index, const AirportMarker *marker)
{
    if (!airportsLock)
        return false;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    if (!ValidMarker(marker) || index < -1 || index >= MAX_AIRPORTS) {
        s_lastError = ESP_ERR_INVALID_ARG;
        xSemaphoreGive(airportsLock);
        return false;
    }
    /* Heap/PSRAM scratch buffer, not a permanent static - see AllocScratch().
     * This runs on the HTTPD task in response to an infrequent admin edit, so
     * a transient allocation costs nothing that matters, unlike a static
     * buffer that would sit in internal RAM for the device's entire uptime.
     * Every caller is already serialized by airportsLock, and the struct is
     * fully overwritten (`= stored`) before use on every call. */
    StoredAirportsUnion *scratch = AllocScratch();
    if (!scratch) {
        s_lastError = ESP_ERR_NO_MEM;
        xSemaphoreGive(airportsLock);
        return false;
    }
    StoredAirports *next = &scratch->v3;
    *next = stored;
    if (index == -1) {
        if (next->count == MAX_AIRPORTS) {
            s_lastError = ESP_ERR_INVALID_STATE; /* the 100-location limit */
            free(scratch);
            xSemaphoreGive(airportsLock);
            return false;
        }
        index = next->count++;
    } else if (index >= next->count) {
        s_lastError = ESP_ERR_NOT_FOUND; /* edited entry no longer exists */
        free(scratch);
        xSemaphoreGive(airportsLock);
        return false;
    }
    next->markers[index] = *marker;
    bool saved = SaveStored(next);
    if (saved) {
        stored = *next;
        userRevision++;
    }
    free(scratch);
    xSemaphoreGive(airportsLock);
    return saved;
}

bool Airports_Delete(size_t index)
{
    if (!airportsLock)
        return false;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    if (index >= stored.count) {
        s_lastError = ESP_ERR_NOT_FOUND;
        xSemaphoreGive(airportsLock);
        return false;
    }
    /* Heap/PSRAM scratch buffer, not a permanent static - see AllocScratch()
     * and the identical reasoning in Airports_Save() above. */
    StoredAirportsUnion *scratch = AllocScratch();
    if (!scratch) {
        s_lastError = ESP_ERR_NO_MEM;
        xSemaphoreGive(airportsLock);
        return false;
    }
    StoredAirports *next = &scratch->v3;
    *next = stored;
    for (size_t i = index + 1; i < next->count; i++)
        next->markers[i - 1] = next->markers[i];
    memset(&next->markers[--next->count], 0, sizeof(next->markers[0]));
    bool saved = SaveStored(next);
    if (saved) {
        stored = *next;
        userRevision++;
    }
    free(scratch);
    xSemaphoreGive(airportsLock);
    return saved;
}

/* ---- marker types (see airports.h) ---- */

bool Airport_MarkerTypeValid(unsigned markerMode)
{
    return markerMode < AIRPORT_MARKER_TYPE_COUNT;
}

const char *Airport_MarkerTypeName(unsigned markerMode)
{
    switch (markerMode) {
    case AIRPORT_MARKER_DOT: return "Dot";
    case AIRPORT_MARKER_DIRECTIONAL: return "Directional";
    case AIRPORT_MARKER_HELIPORT: return "H (heliport)";
    case AIRPORT_MARKER_SQUARE: return "Square";
    default: return "Unknown";
    }
}

AirportMarkerMode Airport_EffectiveShape(const AirportMarker *marker, float *axisDegOut)
{
    if (!marker)
        return AIRPORT_MARKER_DOT;
    switch (marker->markerMode) {
    case AIRPORT_MARKER_DIRECTIONAL: {
        float axis;
        if (Airport_ParseRunwayAxis(marker->runway, &axis) || Airport_ParseTrueDirection(marker->runway, &axis)) {
            if (axisDegOut)
                *axisDegOut = axis;
            return AIRPORT_MARKER_DIRECTIONAL;
        }
        return AIRPORT_MARKER_DOT; /* never draw a guessed axis */
    }
    case AIRPORT_MARKER_HELIPORT: return AIRPORT_MARKER_HELIPORT;
    case AIRPORT_MARKER_SQUARE: return AIRPORT_MARKER_SQUARE;
    default: return AIRPORT_MARKER_DOT;
    }
}

void Airport_HeliportSegments(int diameter, int seg[3][4])
{
    /* Fits a diameter x diameter box centered on the location; the bars sit
     * at +/- 1/3 of the width so the letter stays readable at 6 px. */
    const int h = diameter / 2;
    int w = diameter / 3;
    if (w < 2)
        w = 2;
    seg[0][0] = -w; seg[0][1] = -h; seg[0][2] = -w; seg[0][3] = h; /* left bar */
    seg[1][0] = w;  seg[1][1] = -h; seg[1][2] = w;  seg[1][3] = h; /* right bar */
    seg[2][0] = -w; seg[2][1] = 0;  seg[2][2] = w;  seg[2][3] = 0; /* crossbar */
}

/* ---- shared directional-marker geometry (see airports.h) ---- */

bool Airport_ParseRunwayAxis(const char *runwayText, float *axisDegOut)
{
    if (!runwayText || !axisDegOut)
        return false;
    size_t length = strlen(runwayText);
    if (length < 1 || length > AIRPORT_RUNWAY_LENGTH)
        return false;

    size_t i = 0;
    int digits = 0;
    long number = 0;
    while (i < length && isdigit((unsigned char)runwayText[i])) {
        if (digits >= 2) /* a runway number is at most 2 digits (01-36) */
            return false;
        number = number * 10 + (runwayText[i] - '0');
        digits++;
        i++;
    }
    if (digits == 0)
        return false;

    if (i < length) {
        /* At most one trailing L/C/R (parallel-runway suffix). It never
         * changes the physical axis - only which of several parallel strips
         * is meant - so it's accepted here and then ignored. */
        char c = (char)toupper((unsigned char)runwayText[i]);
        if (c != 'L' && c != 'C' && c != 'R')
            return false;
        i++;
    }
    if (i != length) /* trailing junk after the suffix */
        return false;

    if (number < 1 || number > 36)
        return false;

    /* Runway numbers are true/magnetic heading / 10, rounded to the nearest
     * 10 degrees. Reciprocal ends (e.g. 09/27, 18/36) are 180 degrees apart,
     * i.e. 18 runway-number apart, so the PHYSICAL axis - independent of
     * which end is named - is the heading modulo 180: */
    long axis = (number % 18) * 10; /* 0, 10, ..., 170 */
    *axisDegOut = (float)axis;
    return true;
}

bool Airport_ParseTrueDirection(const char *text, float *axisDegOut)
{
    if (!text || !axisDegOut || strlen(text) != 3 || !isdigit((unsigned char)text[0]) ||
        !isdigit((unsigned char)text[1]) || !isdigit((unsigned char)text[2]))
        return false;
    const int deg = (text[0] - '0') * 100 + (text[1] - '0') * 10 + (text[2] - '0');
    if (deg > 360)
        return false;
    *axisDegOut = (float)(deg % 180); /* a runway axis: 000, 180 and 360 are the same line, as are 090 and 270 */
    return true;
}

bool Airport_DirectionTextValid(const char *text)
{
    float axis;
    return text && (text[0] == '\0' || Airport_ParseRunwayAxis(text, &axis) || Airport_ParseTrueDirection(text, &axis));
}

bool Airport_DisplayAxisDeg(const AirportMarker *marker, const AirportBuiltinView *view, float declDeg,
                            float rotationDeg, float *displayAxisOut)
{
    if (!marker || !displayAxisOut)
        return false;
    float axis = 0.0f;
    if (Airport_EffectiveShape(marker, &axis) != AIRPORT_MARKER_DIRECTIONAL)
        return false;
    float trueAxis;
    if (view && view->hasAxis && (view->overrideFields & AIRPORT_OVR_ROTATION))
        trueAxis = view->axisDeg;                 /* exact true geometry set by the user */
    else if (view && view->hasAxis)
        trueAxis = view->axisDeg + declDeg;       /* database axis: designator (magnetic) */
    else if (Airport_ParseTrueDirection(marker->runway, &trueAxis))
        ;                                         /* user location: 3-digit direction (true), as the Rotation override */
    else
        trueAxis = axis + declDeg;                /* user location: designator (magnetic) */
    float d = fmodf(trueAxis - rotationDeg, 360.0f);
    if (d < 0.0f)
        d += 360.0f;
    if (!isfinite(d))
        return false;
    *displayAxisOut = d;
    return true;
}

void Airport_RunwayAxisOffsets(float axisDeg, float totalLength,
                               float *dx1, float *dy1, float *dx2, float *dy2)
{
    /* Same north-up compass-bearing convention as Radar_ProjectPosition and
     * DrawAircraft's heading math: 0=N (straight up on screen), 90=E
     * (straight right). A runway axis is a line, not a one-way direction, so
     * the two ends are simply +/- half the total length along that bearing. */
    const float rad = axisDeg * 0.0174532925f;
    const float half = totalLength * 0.5f;
    const float dx = sinf(rad) * half;
    const float dy = -cosf(rad) * half;
    *dx1 = dx;
    *dy1 = dy;
    *dx2 = -dx;
    *dy2 = -dy;
}
/* ---- built-in airports: regional selection (see airports.h) ---- */

/* Same flat-earth offset as radar.c's Radar_GeoOffsetKm, so "in range" here
 * means exactly "Radar_ProjectPosition will place it on the radar". */
static float BuiltinDistanceKm(const BuiltinAirport *b, float centerLat, float centerLon)
{
    const float lat = (float)b->latE5 / 1e5f;
    const float lon = (float)b->lonE5 / 1e5f;
    const float dx = lon - centerLon;
    const float dy = lat - centerLat;
    const float kmPerDegLat = 111.0f;
    const float kmPerDegLon = 111.0f * cosf(centerLat * 3.14159265f / 180.0f);
    const float east = dx * kmPerDegLon;
    const float north = dy * kmPerDegLat;
    return sqrtf(east * east + north * north);
}

static size_t BuiltinIdentLength(const BuiltinAirport *b)
{
    size_t n = 0;
    while (n < sizeof(b->icao) && b->icao[n])
        n++;
    return n;
}

/* A user location "names" a built-in airport when its name starts with the
 * airport's ident as a whole word (e.g. "KSFO San Francisco Intl", the format
 * of the seeded list and of the built-in markers). The user entry then wins. */
static bool UserNamesBuiltin(const BuiltinAirport *b)
{
    const size_t n = BuiltinIdentLength(b);
    for (size_t i = 0; i < stored.count; i++) {
        const char *name = stored.markers[i].name;
        size_t k = 0;
        while (k < n && toupper((unsigned char)name[k]) == b->icao[k])
            k++;
        if (k == n && (name[n] == ' ' || name[n] == '\0'))
            return true;
    }
    return false;
}

/* Priority order: tier (major first), then distance, then table index. */
static bool BuiltinBefore(uint16_t a, float distA, uint16_t b, float distB)
{
    const uint8_t tierA = kBuiltinAirports[a].tier, tierB = kBuiltinAirports[b].tier;
    if (tierA != tierB)
        return tierA < tierB;
    if (distA != distB)
        return distA < distB;
    return a < b;
}

static void RecomputeBuiltinSelectionLocked(float centerLat, float centerLon, float rangeKm)
{
    builtinSelCount = 0;
    builtinSelReplaced = 0;
    builtinSelOverCapacity = 0;
    builtinSelLat = centerLat;
    builtinSelLon = centerLon;
    builtinSelRange = rangeKm;
    builtinSelUserRevision = userRevision;
    builtinSelOvrRevision = overrideRevision;
    builtinSelValid = true;
    if (!isfinite(centerLat) || !isfinite(centerLon) || !isfinite(rangeKm) || rangeKm <= 0.0f ||
        stored.count >= AIRPORT_ACTIVE_MAX)
        return;
    const size_t limit = AIRPORT_ACTIVE_MAX - stored.count;

    /* Latitude band [center - range, center + range] (+ a small margin; the
     * exact distance test below decides). Rows are sorted by latitude. */
    const float bandDeg = rangeKm / 111.0f + 0.01f;
    const int32_t minE5 = (int32_t)floorf((centerLat - bandDeg) * 1e5f);
    const int32_t maxE5 = (int32_t)ceilf((centerLat + bandDeg) * 1e5f);
    size_t lo = 0, hi = kBuiltinAirportCount;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (kBuiltinAirports[mid].latE5 < minE5)
            lo = mid + 1;
        else
            hi = mid;
    }
    uint32_t eligible = 0;
    for (size_t i = lo; i < kBuiltinAirportCount && kBuiltinAirports[i].latE5 <= maxE5; i++) {
        const BuiltinAirport *b = &kBuiltinAirports[i];
        const float dist = BuiltinDistanceKm(b, centerLat, centerLon);
        if (!(dist <= rangeKm))
            continue; /* outside the radar area: not counted anywhere */
        if (UserNamesBuiltin(b)) {
            builtinSelReplaced++; /* existing dedupe, only counted for the /airports explanation */
            continue;
        }
        eligible++;
        const uint16_t idx = (uint16_t)i;
        /* Bounded insertion into the priority-ordered selection. */
        if (builtinSelCount == limit) {
            const uint16_t last = builtinSel[limit - 1];
            if (!BuiltinBefore(idx, dist, last,
                               BuiltinDistanceKm(&kBuiltinAirports[last], centerLat, centerLon)))
                continue;
        }
        size_t a = 0, z = builtinSelCount;
        while (a < z) {
            const size_t m = a + (z - a) / 2;
            const uint16_t other = builtinSel[m];
            if (BuiltinBefore(other, BuiltinDistanceKm(&kBuiltinAirports[other], centerLat, centerLon), idx, dist))
                a = m + 1;
            else
                z = m;
        }
        size_t moveCount = builtinSelCount - a;
        if (builtinSelCount == limit)
            moveCount--; /* the last entry drops out */
        memmove(&builtinSel[a + 1], &builtinSel[a], moveCount * sizeof(builtinSel[0]));
        builtinSel[a] = idx;
        if (builtinSelCount < limit)
            builtinSelCount++;
    }
    builtinSelOverCapacity = eligible - (uint32_t)builtinSelCount;
    MatchOverridesLocked();
}

void Airports_GetBuiltinSelectionStats(uint32_t *replacedByUser, uint32_t *overCapacity)
{
    if (!airportsLock)
        return;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    if (replacedByUser)
        *replacedByUser = builtinSelReplaced;
    if (overCapacity)
        *overCapacity = builtinSelOverCapacity;
    xSemaphoreGive(airportsLock);
}

static bool SameIdent(const char a[4], const char b[4])
{
    return memcmp(a, b, 4) == 0;
}

static int FindOverrideLocked(const char icao[4])
{
    if (!overrides)
        return -1;
    for (size_t i = 0; i < overrides->count; i++)
        if (SameIdent(overrides->records[i].icao, icao))
            return (int)i;
    return -1;
}

/* Step 3 of the pipeline: attach each selected airport's override (if any). */
static void MatchOverridesLocked(void)
{
    for (size_t i = 0; i < builtinSelCount; i++) {
        const int slot = FindOverrideLocked(kBuiltinAirports[builtinSel[i]].icao);
        builtinSelOvr[i] = slot < 0 ? AIRPORT_OVERRIDE_NONE : (uint8_t)slot;
    }
    builtinSelOvrRevision = overrideRevision;
}

size_t Airports_SelectBuiltins(float centerLat, float centerLon, float rangeKm)
{
    if (!airportsLock)
        return 0;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    if (!builtinSelValid || builtinSelLat != centerLat || builtinSelLon != centerLon ||
        builtinSelRange != rangeKm || builtinSelUserRevision != userRevision)
        RecomputeBuiltinSelectionLocked(centerLat, centerLon, rangeKm);
    if (builtinSelOvrRevision != overrideRevision)
        MatchOverridesLocked(); /* overrides never change the selection itself */
    const size_t count = builtinSelCount;
    xSemaphoreGive(airportsLock);
    return count;
}

static void BuiltinToMarker(const BuiltinAirport *b, AirportMarker *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%.*s %s", (int)BuiltinIdentLength(b), b->icao,
             &kBuiltinAirportNames[b->nameOffset]);
    out->latitude = (float)b->latE5 / 1e5f;
    out->longitude = (float)b->lonE5 / 1e5f;
    out->diameter = b->tier == BUILTIN_AIRPORT_TIER_MAJOR ? AIRPORT_BUILTIN_TIER1_DIAMETER
                                                          : AIRPORT_BUILTIN_TIER2_DIAMETER;
    out->color = AIRPORT_BUILTIN_COLOR;
    if (b->runwayAxis10 < 18) {
        /* A designator that Airport_ParseRunwayAxis maps back to the same
         * physical axis ("18" for the north-south axis 0). */
        out->markerMode = AIRPORT_MARKER_DIRECTIONAL;
        snprintf(out->runway, sizeof(out->runway), "%02u",
                 (unsigned)(b->runwayAxis10 ? b->runwayAxis10 : 18));
    } else {
        out->markerMode = AIRPORT_MARKER_DOT;
    }
}

/* Database marker + the override's fields (only those marked). */
static void ApplyOverride(const BuiltinAirport *b, const AirportBuiltinOverride *o, AirportMarker *out,
                          AirportBuiltinView *view)
{
    BuiltinToMarker(b, out);
    AirportBuiltinView v = {.hidden = false, .hasAxis = b->runwayAxis10 < 18,
                            .axisDeg = b->runwayAxis10 < 18 ? b->runwayAxis10 * 10.0f : 0.0f,
                            .overrideFields = 0};
    if (o) {
        v.overrideFields = o->fields;
        v.hidden = (o->fields & AIRPORT_OVR_HIDDEN) != 0;
        if (o->fields & AIRPORT_OVR_LAT)
            out->latitude = (float)o->latE5 / 1e5f;
        if (o->fields & AIRPORT_OVR_LON)
            out->longitude = (float)o->lonE5 / 1e5f;
        if (o->fields & AIRPORT_OVR_COLOR)
            out->color = o->color; /* the same final color field user locations use */
        if (o->fields & AIRPORT_OVR_ROTATION) {
            /* An explicit rotation uses the existing Directional marker, also
             * for an airport that is a Dot in the database. */
            v.hasAxis = true;
            v.axisDeg = (float)o->rotationDeg;
            unsigned n = (unsigned)((o->rotationDeg + 5) / 10) % 18;
            out->markerMode = AIRPORT_MARKER_DIRECTIONAL;
            snprintf(out->runway, sizeof(out->runway), "%02u", n ? n : 18u);
        }
    }
    if (view)
        *view = v;
}

bool Airports_GetActiveBuiltinView(size_t index, AirportMarker *out, AirportBuiltinView *view)
{
    if (!airportsLock || !out)
        return false;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    if (builtinSelValid && builtinSelOvrRevision != overrideRevision)
        MatchOverridesLocked();
    const bool found = index < builtinSelCount;
    if (found) {
        const uint8_t slot = builtinSelOvr[index];
        ApplyOverride(&kBuiltinAirports[builtinSel[index]],
                      slot != AIRPORT_OVERRIDE_NONE && overrides ? &overrides->records[slot] : NULL, out, view);
    }
    xSemaphoreGive(airportsLock);
    return found;
}

bool Airports_GetActiveBuiltin(size_t index, AirportMarker *out)
{
    return Airports_GetActiveBuiltinView(index, out, NULL);
}

/* "ksfo" / "KSFO" -> NUL-padded upper-case ident; false if not 1-4 alnum. */
static bool NormalizeIdent(const char *in, char out[4])
{
    memset(out, 0, 4);
    if (!in)
        return false;
    size_t n = 0;
    for (; in[n]; n++) {
        if (n >= 4 || !isalnum((unsigned char)in[n]))
            return false;
        out[n] = (char)toupper((unsigned char)in[n]);
    }
    return n > 0;
}

static int FindBuiltinIndex(const char icao[4])
{
    for (size_t i = 0; i < kBuiltinAirportCount; i++)
        if (SameIdent(kBuiltinAirports[i].icao, icao))
            return (int)i;
    return -1;
}

bool Airports_GetBuiltinDefaults(const char *icao, AirportMarker *out, float *axisDegOut)
{
    char key[4];
    if (!out || !NormalizeIdent(icao, key))
        return false;
    const int i = FindBuiltinIndex(key);
    if (i < 0)
        return false;
    BuiltinToMarker(&kBuiltinAirports[i], out);
    if (axisDegOut)
        *axisDegOut = kBuiltinAirports[i].runwayAxis10 < 18 ? kBuiltinAirports[i].runwayAxis10 * 10.0f : NAN;
    return true;
}

size_t Airports_OverrideCount(void)
{
    if (!airportsLock)
        return 0;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    const size_t n = overrides ? overrides->count : 0;
    xSemaphoreGive(airportsLock);
    return n;
}

bool Airports_GetOverride(size_t index, AirportBuiltinOverride *out)
{
    if (!airportsLock || !out)
        return false;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    const bool found = overrides && index < overrides->count;
    if (found)
        *out = overrides->records[index];
    xSemaphoreGive(airportsLock);
    return found;
}

bool Airports_FindOverride(const char *icao, AirportBuiltinOverride *out)
{
    char key[4];
    if (!airportsLock || !out || !NormalizeIdent(icao, key))
        return false;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    const int i = FindOverrideLocked(key);
    if (i >= 0)
        *out = overrides->records[i];
    xSemaphoreGive(airportsLock);
    return i >= 0;
}

/* Writes `next` (only its records in use); an empty table erases the key so
 * NVS holds nothing for overrides. */
static bool SaveOverrides(const StoredOverrides *next)
{
    nvs_handle_t handle;
    if (nvs_open(AIRPORT_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    esp_err_t err;
    if (next->count == 0) {
        err = nvs_erase_key(handle, AIRPORT_OVERRIDE_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
    } else {
        err = nvs_set_blob(handle, AIRPORT_OVERRIDE_KEY, next,
                           4 + (size_t)next->count * sizeof(AirportBuiltinOverride));
    }
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

/* Replace (fields != 0) or delete (fields == 0) the record for key, then persist. */
static bool StoreOverride(const char key[4], const AirportBuiltinOverride *rec)
{
    if (!airportsLock)
        return false;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    if (!overrides) {
        xSemaphoreGive(airportsLock);
        return false;
    }
    StoredOverrides *next = heap_caps_malloc(sizeof(*next), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) {
        xSemaphoreGive(airportsLock);
        return false;
    }
    *next = *overrides;
    const int i = FindOverrideLocked(key);
    bool ok = true, changed = true;
    if (!rec) {
        if (i < 0) {
            changed = false; /* nothing stored: already the database default */
        } else {
            for (size_t j = (size_t)i + 1; j < next->count; j++)
                next->records[j - 1] = next->records[j];
            memset(&next->records[--next->count], 0, sizeof(next->records[0]));
        }
    } else if (i >= 0) {
        next->records[i] = *rec;
    } else if (next->count < AIRPORT_OVERRIDE_MAX) {
        next->records[next->count++] = *rec;
    } else {
        ok = false; /* table full */
    }
    if (ok && changed) {
        ok = SaveOverrides(next);
        if (ok) {
            *overrides = *next;
            overrideRevision++;
        }
    }
    free(next);
    xSemaphoreGive(airportsLock);
    return ok;
}

bool Airports_SetOverride(const AirportBuiltinOverride *ovr)
{
    char key[4];
    char text[5] = {0};
    if (!ovr)
        return false;
    memcpy(text, ovr->icao, 4);
    if (!NormalizeIdent(text, key) || FindBuiltinIndex(key) < 0)
        return false;
    AirportBuiltinOverride rec = {0};
    memcpy(rec.icao, key, 4);
    rec.fields = ovr->fields & AIRPORT_OVR_ALL;
    if (ovr->fields & ~AIRPORT_OVR_ALL)
        return false;
    if (rec.fields & AIRPORT_OVR_ROTATION)
        rec.rotationDeg = ovr->rotationDeg;
    if (rec.fields & AIRPORT_OVR_LAT)
        rec.latE5 = ovr->latE5;
    if (rec.fields & AIRPORT_OVR_LON)
        rec.lonE5 = ovr->lonE5;
    if (rec.fields & AIRPORT_OVR_COLOR)
        rec.color = ovr->color;
    if (rec.fields == 0)
        return StoreOverride(key, NULL); /* every field back to default: no record */
    if (!ValidOverride(&rec))
        return false;
    return StoreOverride(key, &rec);
}

bool Airports_ResetOverride(const char *icao)
{
    char key[4];
    if (!NormalizeIdent(icao, key))
        return false;
    return StoreOverride(key, NULL);
}

bool Airports_GetActiveBuiltinInfo(size_t index, uint8_t *tierOut, float *distanceKmOut)
{
    if (!airportsLock)
        return false;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    const bool found = index < builtinSelCount;
    if (found) {
        const BuiltinAirport *b = &kBuiltinAirports[builtinSel[index]];
        if (tierOut)
            *tierOut = b->tier;
        if (distanceKmOut)
            *distanceKmOut = BuiltinDistanceKm(b, builtinSelLat, builtinSelLon);
    }
    xSemaphoreGive(airportsLock);
    return found;
}

size_t Airports_BuiltinTotal(void)
{
    return kBuiltinAirportCount;
}
