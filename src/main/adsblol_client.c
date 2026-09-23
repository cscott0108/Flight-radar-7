#include "adsblol_client.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "cJSON.h"

#include "custom_rules.h" /* AircraftType, ResolveAircraftWithHint (diagnostics only) */

#define ADSBLOL_NAMESPACE "adsblol"
#define RATE_LIMIT_KEY "rate_until"
/* adsb.lol publishes no fixed quota (dynamic, load-based limiting); a
 * shorter backoff than OpenSky's fixed hour is appropriate since a 4xx here
 * is far more likely to be transient load-shedding than a hard quota. */
#define RATE_LIMIT_PAUSE_SECONDS 300U

#define ADSBLOL_MAX_RESPONSE_BYTES 32768U
#define KM_TO_NM 0.539957f
#define ADSBLOL_MAX_RADIUS_NM 250.0f /* API-enforced cap */

static const char *TAG = "AdsbLol";

static char *responseBuffer = NULL;
static size_t responseLength = 0;
static size_t responseCapacity = 0;

static volatile uint32_t rateLimitUntil = 0;

uint32_t AdsbLol_GetRateLimitSeconds(void)
{
    time_t now = time(NULL);
    uint32_t until = rateLimitUntil;
    if (now < 0 || (uint64_t)now >= until)
        return 0;
    return until - (uint32_t)now;
}

static void PauseAfterRateLimit(const char *requestName)
{
    time_t now = time(NULL);
    if (now < 0 || (uint64_t)now > UINT32_MAX - RATE_LIMIT_PAUSE_SECONDS)
    {
        ESP_LOGE(TAG, "%s returned an error status; clock unavailable, cannot schedule backoff", requestName);
        return;
    }
    rateLimitUntil = (uint32_t)now + RATE_LIMIT_PAUSE_SECONDS;
    ESP_LOGW(TAG, "%s: pausing adsb.lol requests for %u seconds", requestName, RATE_LIMIT_PAUSE_SECONDS);

    nvs_handle_t handle;
    if (nvs_open(ADSBLOL_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_set_u32(handle, RATE_LIMIT_KEY, rateLimitUntil);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void LoadRateLimit(void)
{
    nvs_handle_t handle;
    if (nvs_open(ADSBLOL_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return;
    uint32_t until = 0;
    if (nvs_get_u32(handle, RATE_LIMIT_KEY, &until) == ESP_OK)
        rateLimitUntil = until;
    nvs_close(handle);
}

bool AdsbLol_Init(void)
{
    LoadRateLimit();

    if (responseBuffer != NULL)
    {
        free(responseBuffer);
        responseBuffer = NULL;
    }

    responseCapacity = ADSBLOL_MAX_RESPONSE_BYTES;
    responseBuffer = heap_caps_malloc(responseCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!responseBuffer)
    {
        ESP_LOGE(TAG, "Could not allocate adsb.lol response buffer in PSRAM");
        return false;
    }
    responseBuffer[0] = '\0';
    return true;
}

bool AdsbLol_HasCredentials(void)
{
    return true; /* no auth required */
}

static esp_err_t HttpEventHandler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        if (responseLength + evt->data_len + 1 > responseCapacity)
        {
            ESP_LOGE(TAG, "adsb.lol response exceeds %u-byte bounded buffer; dropping remainder",
                     (unsigned)responseCapacity);
            return ESP_FAIL;
        }
        memcpy(responseBuffer + responseLength, evt->data, evt->data_len);
        responseLength += evt->data_len;
        responseBuffer[responseLength] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

bool AdsbLol_GetAircraftJson(
    float centerLat,
    float centerLon,
    float radiusKm,
    const char **json)
{
    if (!json || !responseBuffer)
    {
        ESP_LOGE(TAG, "adsb.lol response buffer is unavailable");
        return false;
    }
    *json = NULL;

    if (AdsbLol_GetRateLimitSeconds() != 0)
        return false;

    responseLength = 0;
    responseBuffer[0] = '\0';

    float radiusNm = radiusKm * KM_TO_NM;
    if (radiusNm > ADSBLOL_MAX_RADIUS_NM)
        radiusNm = ADSBLOL_MAX_RADIUS_NM;
    if (radiusNm < 1.0f)
        radiusNm = 1.0f;

    char url[192];
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.6f/%.6f/%.0f",
             centerLat, centerLon, radiusNm);

    ProviderDiag_RequestStart("adsb.lol", "aircraft request", url);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = HttpEventHandler,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "flight-radar-7-esp32/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Could not allocate aircraft HTTP client");
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        ProviderDiag_RequestDone("adsb.lol", "aircraft request", 0, 0, false);
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ProviderDiag_RequestDone("adsb.lol", "aircraft request", status, responseLength, status == 200);
    ProviderDiag_RawPreview("adsb.lol", responseBuffer, responseLength);

    if (status == 429 || status == 503)
    {
        PauseAfterRateLimit("Aircraft point request");
        return false;
    }

    if (status != 200)
    {
        ESP_LOGE(TAG, "Unexpected HTTP status: %d", status);
        return false;
    }

    *json = responseBuffer;
    return true;
}

bool AdsbLol_ParseAircraft(const char *json)
{
    gAircraftCount = 0;
    int rawCount = 0;
    int rejectedCount = 0;

    cJSON *root = cJSON_Parse(json);
    if (!root)
    {
        ProviderDiag_Warning("adsb.lol", "JSON parse failed");
        ProviderDiag_ParseResult("adsb.lol", false, 0, 0, 0);
        return false;
    }

    cJSON *ac = cJSON_GetObjectItem(root, "ac");
    if (!ac || cJSON_IsNull(ac))
    {
        /* No aircraft in range: a normal, successful empty response. */
        cJSON_Delete(root);
        ProviderDiag_ParseResult("adsb.lol", true, 0, 0, 0);
        return true;
    }

    if (!cJSON_IsArray(ac))
    {
        cJSON_Delete(root);
        ProviderDiag_Warning("adsb.lol", "\"ac\" field was present but not an array/null");
        ProviderDiag_ParseResult("adsb.lol", false, 0, 0, 0);
        return false;
    }

    int count = cJSON_GetArraySize(ac);

    for (int i = 0; i < count && gAircraftCount < MAX_AIRCRAFT; i++)
    {
        cJSON *entry = cJSON_GetArrayItem(ac, i);
        if (!cJSON_IsObject(entry))
            continue;

        rawCount++;

        cJSON *hex = cJSON_GetObjectItem(entry, "hex");
        cJSON *flight = cJSON_GetObjectItem(entry, "flight");
        cJSON *lat = cJSON_GetObjectItem(entry, "lat");
        cJSON *lon = cJSON_GetObjectItem(entry, "lon");
        cJSON *gs = cJSON_GetObjectItem(entry, "gs");
        cJSON *track = cJSON_GetObjectItem(entry, "track");
        cJSON *altBaro = cJSON_GetObjectItem(entry, "alt_baro");
        cJSON *category = cJSON_GetObjectItem(entry, "category");

        if (!cJSON_IsString(hex) || !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon))
        {
            rejectedCount++;
            continue;
        }

        Aircraft *a = &gAircraft[gAircraftCount];
        memset(a, 0, sizeof(Aircraft));

        strncpy(a->icao24, hex->valuestring, sizeof(a->icao24) - 1);

        if (flight && cJSON_IsString(flight))
        {
            strncpy(a->callsign, flight->valuestring, sizeof(a->callsign) - 1);
            /* adsb.lol/ADSBExchange pad "flight" to 8 characters with
             * trailing spaces; the rest of the app (registry matching,
             * label drawing) expects a trimmed callsign like OpenSky's. */
            for (int c = (int)strlen(a->callsign) - 1; c >= 0 && a->callsign[c] == ' '; c--)
                a->callsign[c] = '\0';
        }

        a->latitude = (float)lat->valuedouble;
        a->longitude = (float)lon->valuedouble;

        bool reportedOnGround = false;
        if (altBaro)
        {
            if (cJSON_IsString(altBaro) && strcasecmp(altBaro->valuestring, "ground") == 0)
            {
                reportedOnGround = true;
                a->altitude = 0.0f;
            }
            else if (cJSON_IsNumber(altBaro))
            {
                a->altitude = (float)altBaro->valuedouble * 0.3048f; /* ft -> m */
            }
        }

        if (gs && cJSON_IsNumber(gs))
            a->velocity = (float)gs->valuedouble * 0.514444f; /* knots -> m/s */

        if (track && cJSON_IsNumber(track))
            a->heading = (float)track->valuedouble;

        const char *categoryStr = (category && cJSON_IsString(category)) ? category->valuestring : "";
        AircraftType hint = AIRCRAFT_FIXED_WING;
        bool hasHint = AdsbLol_CategoryToAircraftType(categoryStr, &hint);
        a->hasProviderTypeHint = hasHint;
        a->providerTypeHint = hasHint ? hint : AIRCRAFT_FIXED_WING;

        bool looksParked =
            (a->altitude <= GROUND_ALTITUDE_THRESHOLD_M) &&
            (a->velocity <= GROUND_VELOCITY_THRESHOLD_MS);

        if (reportedOnGround || looksParked)
        {
            rejectedCount++;
            continue;
        }

        a->valid = true;
        a->predictedLat = a->latitude;
        a->predictedLon = a->longitude;
        a->lastUpdateMs = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (AircraftProvider_GetDebugLevel() >= PROVIDER_DEBUG_VERBOSE)
        {
            CraftResolution resolved = ResolveAircraftWithHint(
                a->callsign, a->icao24, a->providerTypeHint, a->hasProviderTypeHint);
            const char *regName = (resolved.source == CRAFT_SRC_REGISTRY) ?
                AircraftType_Name(resolved.aircraftType) : "none";
            ProviderDiag_TypeResolution(
                a->icao24, categoryStr, hasHint, hint, regName, resolved.aircraftType);
        }

        gAircraftCount++;
    }

    cJSON_Delete(root);

    ProviderDiag_ParseResult("adsb.lol", true, rawCount, gAircraftCount, rejectedCount);

    return true;
}
