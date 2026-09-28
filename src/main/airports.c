#include "airports.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "Airports";

#define AIRPORT_NAMESPACE "airports"
#define AIRPORT_KEY "markers"
#define AIRPORT_VERSION 3 /* 2 added AirportMarker.color; 3 added markerMode + runway */
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
           (marker->markerMode == AIRPORT_MARKER_DOT || marker->markerMode == AIRPORT_MARKER_DIRECTIONAL) &&
           ValidRunwayText(marker->runway);
}

static bool SaveStored(const StoredAirports *next)
{
    nvs_handle_t handle;
    if (nvs_open(AIRPORT_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    esp_err_t err = nvs_set_blob(handle, AIRPORT_KEY, next, sizeof(*next));
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
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
    if (SaveStored(seeded))
        stored = *seeded;
    free(scratch);
}

bool Airports_Init(void)
{
    if (!airportsLock)
        airportsLock = xSemaphoreCreateMutex();
    if (!airportsLock)
        return false;
    memset(&stored, 0, sizeof(stored));
    stored.version = AIRPORT_VERSION;

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
        free(scratch);
        free(upgradedScratch);
        return ok;
    }

    bool ok = size == sizeof(StoredAirports) && scratch->v3.version == AIRPORT_VERSION &&
               scratch->v3.count <= MAX_AIRPORTS;
    for (size_t i = 0; ok && i < scratch->v3.count; i++)
        if (!ValidMarker(&scratch->v3.markers[i]))
            ok = false;
    if (ok)
        stored = scratch->v3;
    free(scratch);
    return ok;
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
    if (!airportsLock || !ValidMarker(marker) || index < -1 || index >= MAX_AIRPORTS)
        return false;
    xSemaphoreTake(airportsLock, portMAX_DELAY);
    /* Heap/PSRAM scratch buffer, not a permanent static - see AllocScratch().
     * This runs on the HTTPD task in response to an infrequent admin edit, so
     * a transient allocation costs nothing that matters, unlike a static
     * buffer that would sit in internal RAM for the device's entire uptime.
     * Every caller is already serialized by airportsLock, and the struct is
     * fully overwritten (`= stored`) before use on every call. */
    StoredAirportsUnion *scratch = AllocScratch();
    if (!scratch) {
        xSemaphoreGive(airportsLock);
        return false;
    }
    StoredAirports *next = &scratch->v3;
    *next = stored;
    if (index == -1) {
        if (next->count == MAX_AIRPORTS) {
            free(scratch);
            xSemaphoreGive(airportsLock);
            return false;
        }
        index = next->count++;
    } else if (index >= next->count) {
        free(scratch);
        xSemaphoreGive(airportsLock);
        return false;
    }
    next->markers[index] = *marker;
    bool saved = SaveStored(next);
    if (saved)
        stored = *next;
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
        xSemaphoreGive(airportsLock);
        return false;
    }
    /* Heap/PSRAM scratch buffer, not a permanent static - see AllocScratch()
     * and the identical reasoning in Airports_Save() above. */
    StoredAirportsUnion *scratch = AllocScratch();
    if (!scratch) {
        xSemaphoreGive(airportsLock);
        return false;
    }
    StoredAirports *next = &scratch->v3;
    *next = stored;
    for (size_t i = index + 1; i < next->count; i++)
        next->markers[i - 1] = next->markers[i];
    memset(&next->markers[--next->count], 0, sizeof(next->markers[0]));
    bool saved = SaveStored(next);
    if (saved)
        stored = *next;
    free(scratch);
    xSemaphoreGive(airportsLock);
    return saved;
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