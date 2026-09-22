#include "airports.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define AIRPORT_NAMESPACE "airports"
#define AIRPORT_KEY "markers"
#define AIRPORT_VERSION 2 /* 2 added AirportMarker.color */
#define AIRPORT_VERSION_1 1

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
    AirportMarkerV1 markers[MAX_AIRPORTS];
} StoredAirportsV1;

typedef struct {
    uint16_t version;
    uint16_t count;
    AirportMarker markers[MAX_AIRPORTS];
} StoredAirports;

static StoredAirports stored;
static SemaphoreHandle_t airportsLock;

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
           marker->color <= 0xFFFFFFu;
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
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return true;
    if (err != ESP_OK)
        return false;
    /* Either layout fits in this buffer; NVS reports the real stored size. */
    union {
        StoredAirports v2;
        StoredAirportsV1 v1;
    } loaded;
    memset(&loaded, 0, sizeof(loaded));
    size_t size = sizeof(loaded);
    err = nvs_get_blob(handle, AIRPORT_KEY, &loaded, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return true;
    if (err != ESP_OK)
        return false;

    if (size == sizeof(StoredAirportsV1) && loaded.v1.version == AIRPORT_VERSION_1) {
        /* Upgrade: every existing airport keeps its data and gets the original red.
         * The blob is rewritten in the new layout the next time an airport is saved. */
        if (loaded.v1.count > MAX_AIRPORTS)
            return false;
        StoredAirports upgraded;
        memset(&upgraded, 0, sizeof(upgraded));
        upgraded.version = AIRPORT_VERSION;
        upgraded.count = loaded.v1.count;
        for (size_t i = 0; i < loaded.v1.count; i++) {
            memcpy(upgraded.markers[i].name, loaded.v1.markers[i].name, sizeof(upgraded.markers[i].name));
            upgraded.markers[i].latitude = loaded.v1.markers[i].latitude;
            upgraded.markers[i].longitude = loaded.v1.markers[i].longitude;
            upgraded.markers[i].diameter = loaded.v1.markers[i].diameter;
            upgraded.markers[i].color = AIRPORT_DEFAULT_COLOR;
            if (!ValidMarker(&upgraded.markers[i]))
                return false;
        }
        stored = upgraded;
        return true;
    }

    if (size != sizeof(StoredAirports) || loaded.v2.version != AIRPORT_VERSION ||
        loaded.v2.count > MAX_AIRPORTS)
        return false;
    for (size_t i = 0; i < loaded.v2.count; i++)
        if (!ValidMarker(&loaded.v2.markers[i]))
            return false;
    stored = loaded.v2;
    return true;
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
    StoredAirports next = stored;
    if (index == -1) {
        if (next.count == MAX_AIRPORTS) {
            xSemaphoreGive(airportsLock);
            return false;
        }
        index = next.count++;
    } else if (index >= next.count) {
        xSemaphoreGive(airportsLock);
        return false;
    }
    next.markers[index] = *marker;
    bool saved = SaveStored(&next);
    if (saved)
        stored = next;
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
    StoredAirports next = stored;
    for (size_t i = index + 1; i < next.count; i++)
        next.markers[i - 1] = next.markers[i];
    memset(&next.markers[--next.count], 0, sizeof(next.markers[0]));
    bool saved = SaveStored(&next);
    if (saved)
        stored = next;
    xSemaphoreGive(airportsLock);
    return saved;
}
