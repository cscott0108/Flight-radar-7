#include "opensky_client.h"

#include <string.h>
#include <time.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_client.h"
#include "esp_log.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "cJSON.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

#include "main.h"
#include "radar.h" // for selectedIcao24, so debug logging can target the selected aircraft

#define OPENSKY_NAMESPACE "opensky"
#define RATE_LIMIT_KEY "rate_until"
#define RATE_LIMIT_PAUSE_SECONDS 3600U

extern const uint8_t isrgrootx1_pem_start[] asm("_binary_isrgrootx1_pem_start");
extern const uint8_t isrgrootx1_pem_end[] asm("_binary_isrgrootx1_pem_end");

static const char *TAG = "OpenSky";

static char clientId[128];
static char clientSecret[256];

static char accessToken[2048];

static time_t tokenExpiry = 0;
static volatile uint32_t rateLimitUntil = 0;

static char *responseBuffer = NULL;
static size_t responseLength = 0;
static size_t responseCapacity = 0;

uint32_t OpenSky_GetRateLimitSeconds(void)
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
    if (now < 0 || (uint64_t)now > UINT32_MAX - RATE_LIMIT_PAUSE_SECONDS) {
        ESP_LOGE(TAG, "%s returned HTTP 429: API call limit exceeded; clock unavailable", requestName);
        return;
    }
    rateLimitUntil = (uint32_t)now + RATE_LIMIT_PAUSE_SECONDS;
    ESP_LOGE(TAG, "%s returned HTTP 429: API call limit exceeded; pausing all OpenSky requests for 1 hour (until epoch %u)",
             requestName, (unsigned)rateLimitUntil);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OPENSKY_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, RATE_LIMIT_KEY, rateLimitUntil);
        if (err == ESP_OK)
            err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Could not save rate-limit pause in NVS: %s", esp_err_to_name(err));
}

static void LoadRateLimit(void)
{
    nvs_handle_t handle;
    if (nvs_open(OPENSKY_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return;
    uint32_t until = 0;
    if (nvs_get_u32(handle, RATE_LIMIT_KEY, &until) == ESP_OK)
        rateLimitUntil = until;
    nvs_close(handle);
    uint32_t remaining = OpenSky_GetRateLimitSeconds();
    if (remaining)
        ESP_LOGW(TAG, "OpenSky HTTP 429 pause restored: %u seconds remaining", (unsigned)remaining);
}

static void LogHttpMemory(const char *stage)
{
    ESP_LOGI(TAG, "%s: internal heap free=%u largest=%u, PSRAM free=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

// GROUND_ALTITUDE_THRESHOLD_M / GROUND_VELOCITY_THRESHOLD_MS now live in
// aircraft_provider.h, shared with every provider so ground filtering is
// identical regardless of which one is active.

// Logs every raw field OpenSky's /states/all endpoint provides for one
// state vector, by array index, to the serial console. Only called (see
// below) for the currently-selected aircraft, and only when the debug
// toggle is on, so this doesn't flood the log with 200 aircraft every
// poll. Meant to make it easy to see what's available to add to the
// Selected Craft panel without digging through the OpenSky API docs.
static void LogOpenSkyStateFields(cJSON *state)
{
    static const char *TAG_DBG = "OpenSky/Debug";

    const char *fieldNames[17] =
        {
            "0  icao24",
            "1  callsign",
            "2  origin_country",
            "3  time_position",
            "4  last_contact",
            "5  longitude",
            "6  latitude",
            "7  baro_altitude",
            "8  on_ground",
            "9  velocity",
            "10 true_track",
            "11 vertical_rate",
            "12 sensors",
            "13 geo_altitude",
            "14 squawk",
            "15 spi",
            "16 position_source",
        };

    ESP_LOGI(TAG_DBG, "---- raw OpenSky state vector ----");

    for (int i = 0; i < 17; i++)
    {
        cJSON *item = cJSON_GetArrayItem(state, i);

        if (!item || cJSON_IsNull(item))
        {
            ESP_LOGI(TAG_DBG, "%s = null", fieldNames[i]);
        }
        else if (cJSON_IsString(item))
        {
            ESP_LOGI(TAG_DBG, "%s = \"%s\"", fieldNames[i], item->valuestring);
        }
        else if (cJSON_IsBool(item))
        {
            ESP_LOGI(TAG_DBG, "%s = %s", fieldNames[i], cJSON_IsTrue(item) ? "true" : "false");
        }
        else if (cJSON_IsNumber(item))
        {
            ESP_LOGI(TAG_DBG, "%s = %f", fieldNames[i], item->valuedouble);
        }
        else if (cJSON_IsArray(item))
        {
            // "sensors" (index 12) is the only array field; not decoded
            // further here, just flagged so its presence is visible.
            ESP_LOGI(TAG_DBG, "%s = [array, %d items]", fieldNames[i], cJSON_GetArraySize(item));
        }
        else
        {
            ESP_LOGI(TAG_DBG, "%s = <unhandled JSON type>", fieldNames[i]);
        }
    }

    ESP_LOGI(TAG_DBG, "-----------------------------------");
}

bool OpenSky_ParseAircraft(
    const char *json)
{
    gAircraftCount = 0;
    int rawCount = 0;
    int rejectedCount = 0;

    cJSON *root =
        cJSON_Parse(json);

    if (!root)
        return false;

    cJSON *states =
        cJSON_GetObjectItem(
            root,
            "states");

    // OpenSky returns "states": null (not an empty array) when zero
    // aircraft match the queried bounding box — a normal, successful
    // response (nothing currently flying over a quiet area), not a
    // parse failure.
    if (!states || cJSON_IsNull(states))
    {
        cJSON_Delete(root);
        ProviderDiag_ParseResult("OpenSky", true, 0, 0, 0);
        return true;
    }

    if (!cJSON_IsArray(states))
    {
        cJSON_Delete(root);
        ProviderDiag_Warning("OpenSky", "\"states\" field was present but not an array/null");
        ProviderDiag_ParseResult("OpenSky", false, 0, 0, 0);
        return false;
    }

    int count =
        cJSON_GetArraySize(states);

    for (int i = 0;
         i < count &&
         gAircraftCount < MAX_AIRCRAFT;
         i++)
    {
        cJSON *state =
            cJSON_GetArrayItem(
                states,
                i);

        if (!cJSON_IsArray(state))
            continue;

        rawCount++;

        Aircraft *a =
            &gAircraft[gAircraftCount];

        memset(
            a,
            0,
            sizeof(Aircraft));

        /* OpenSky's own category field is unreliable/mostly empty (see
         * README), so it is never surfaced as an Aircraft Type hint - shape
         * always falls through to the registry or the Fixed-Wing default. */
        a->hasProviderTypeHint = false;
        a->providerTypeHint = AIRCRAFT_FIXED_WING;

        cJSON *icao =
            cJSON_GetArrayItem(state, 0);

        cJSON *callsign =
            cJSON_GetArrayItem(state, 1);

        cJSON *lon =
            cJSON_GetArrayItem(state, 5);

        cJSON *lat =
            cJSON_GetArrayItem(state, 6);

        cJSON *vel =
            cJSON_GetArrayItem(state, 9);

        cJSON *hdg =
            cJSON_GetArrayItem(state, 10);

        cJSON *onGround =
            cJSON_GetArrayItem(state, 8);

        cJSON *alt =
            cJSON_GetArrayItem(state, 13);

        cJSON *country =
            cJSON_GetArrayItem(state, 2);

        if (!cJSON_IsString(icao) ||
            !cJSON_IsNumber(lat) ||
            !cJSON_IsNumber(lon))
        {
            rejectedCount++;
            continue;
        }

        if (country &&
            cJSON_IsString(country))
        {
            strncpy(
                a->originCountry,
                country->valuestring,
                sizeof(a->originCountry) - 1);
        }

        strncpy(
            a->icao24,
            icao->valuestring,
            sizeof(a->icao24) - 1);

        if (callsign &&
            cJSON_IsString(callsign))
        {
            strncpy(
                a->callsign,
                callsign->valuestring,
                sizeof(a->callsign) - 1);
        }

        a->craftType = evaluateAircraftType(a->callsign, a->icao24);

        if (cJSON_IsNumber(lat))
            a->latitude = lat->valuedouble;

        if (cJSON_IsNumber(lon))
            a->longitude = lon->valuedouble;

        if (cJSON_IsNumber(vel))
            a->velocity = vel->valuedouble;

        if (cJSON_IsNumber(hdg))
            a->heading = hdg->valuedouble;

        if (cJSON_IsNumber(alt))
            a->altitude = alt->valuedouble;

        // Skip parked/taxiing aircraft and ground vehicles: OpenSky's
        // on_ground flag when present, or (as a fallback for feeders that
        // omit it) an aircraft reporting both near-zero altitude and a
        // walking/taxi-speed ground velocity. A real airborne aircraft at
        // low altitude still has substantial forward speed, so this
        // combined check doesn't catch genuine low approaches.
        bool reportedOnGround =
            cJSON_IsBool(onGround) && cJSON_IsTrue(onGround);

        bool looksParked =
            (a->altitude <= GROUND_ALTITUDE_THRESHOLD_M) &&
            (a->velocity <= GROUND_VELOCITY_THRESHOLD_MS);

        if (reportedOnGround || looksParked)
        {
            rejectedCount++;
            continue;
        }

        a->valid = true;

        a->latitude = lat->valuedouble;
        a->longitude = lon->valuedouble;

        a->predictedLat = a->latitude;
        a->predictedLon = a->longitude;

        // Debug field dump: only for the aircraft currently shown in the
        // Selected Craft panel (or the first valid aircraft seen, before
        // anything has been selected yet), so this stays readable instead
        // of scrolling 200 aircraft past every poll.
        if (GetRadarOpenSkyDebugEnabled())
        {
            bool isSelected =
                (selectedIcao24[0] != '\0' &&
                 strcmp(a->icao24, selectedIcao24) == 0);

            bool isFirstBeforeAnySelection =
                (selectedIcao24[0] == '\0' && gAircraftCount == 0);

            if (isSelected || isFirstBeforeAnySelection)
            {
                LogOpenSkyStateFields(state);
            }
        }

        a->lastUpdateMs = xTaskGetTickCount() *
                          portTICK_PERIOD_MS;

        gAircraftCount++;

        if (AircraftProvider_GetDebugLevel() >= PROVIDER_DEBUG_VERBOSE)
        {
            CraftResolution resolved = ResolveAircraftWithHint(
                a->callsign, a->icao24, a->providerTypeHint, a->hasProviderTypeHint);
            const char *regName = (resolved.source == CRAFT_SRC_REGISTRY) ?
                AircraftType_Name(resolved.aircraftType) : "none";
            ProviderDiag_TypeResolution(
                a->icao24, "" /* OpenSky exposes no usable category */,
                false, AIRCRAFT_FIXED_WING, regName, resolved.aircraftType);
        }
    }

    cJSON_Delete(root);

    ProviderDiag_ParseResult("OpenSky", true, rawCount, gAircraftCount, rejectedCount);

    return true;
}

static esp_err_t HttpEventHandler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        // REMOVED the strict non-chunked check because chunked responses are common!
        if (responseLength + evt->data_len + 1 > responseCapacity)
        {
            ESP_LOGE(TAG, "OpenSky response exceeds %u-byte buffer", (unsigned)responseCapacity);
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

static bool LoadCredentials(void)
{
    nvs_handle_t handle;

    esp_err_t err =
        nvs_open(
            OPENSKY_NAMESPACE,
            NVS_READONLY,
            &handle);

    ESP_LOGI(TAG,
             "LoadCredentials nvs_open=%s",
             esp_err_to_name(err));

    if (err != ESP_OK)
    {
        return false;
    }

    size_t idLen =
        sizeof(clientId);

    size_t secretLen =
        sizeof(clientSecret);

    esp_err_t err1 =
        nvs_get_str(
            handle,
            "client_id",
            clientId,
            &idLen);

    esp_err_t err2 =
        nvs_get_str(
            handle,
            "client_secret",
            clientSecret,
            &secretLen);

    ESP_LOGI(TAG,
             "client_id=%s",
             esp_err_to_name(err1));

    ESP_LOGI(TAG,
             "client_secret=%s",
             esp_err_to_name(err2));

    nvs_close(handle);

    return (
        err1 == ESP_OK &&
        err2 == ESP_OK);
}

static bool RequestToken(void)
{
    LogHttpMemory("Before OAuth TLS");

    time_t now;
    time(&now);

    ESP_LOGI(
        TAG,
        "Epoch=%lld",
        (long long)now);

    responseLength = 0;
    responseBuffer[0] = '\0';

    char postBody[512];

    snprintf(
        postBody,
        sizeof(postBody),
        "grant_type=client_credentials"
        "&client_id=%s"
        "&client_secret=%s",
        clientId,
        clientSecret);

    esp_http_client_config_t config =
        {
            .url = "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token",
            //.url = "https://www.google.com",
            .event_handler = HttpEventHandler,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            //.cert_pem = (const char *)isrgrootx1_pem_start,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 30000, // HTTP request timeout, NOT the poll interval.
            // The poll interval lives in main.c (radarRefreshSec, web-configurable).
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
        };

    esp_http_client_handle_t client =
        esp_http_client_init(
            &config);

    if (!client)
    {
        ESP_LOGE(TAG, "Could not allocate OAuth HTTP client");
        return false;
    }

    esp_http_client_set_method(
        client,
        HTTP_METHOD_POST);

    esp_http_client_set_header(
        client,
        "Content-Type",
        "application/x-www-form-urlencoded");

    esp_http_client_set_post_field(
        client,
        postBody,
        strlen(postBody));

    esp_err_t request_err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (request_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Token request failed: %s", esp_err_to_name(request_err));
        esp_http_client_cleanup(
            client);

        return false;
    }

    ESP_LOGI(TAG, "Token response status=%d", status);

    if (status == 429)
    {
        PauseAfterRateLimit("OAuth token request");
        esp_http_client_cleanup(client);
        return false;
    }

    if (status != 200)
    {
        ESP_LOGE(TAG, "Token request rejected: %s", responseBuffer);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_cleanup(
        client);

    cJSON *root =
        cJSON_Parse(
            responseBuffer);

    if (!root)
        return false;

    cJSON *token =
        cJSON_GetObjectItem(
            root,
            "access_token");

    cJSON *expires =
        cJSON_GetObjectItem(
            root,
            "expires_in");

    if (!token ||
        !expires)
    {
        cJSON_Delete(root);
        return false;
    }

    strncpy(
        accessToken,
        token->valuestring,
        sizeof(accessToken) - 1);

    tokenExpiry =
        time(NULL) +
        expires->valueint -
        60;

    cJSON_Delete(root);

    ESP_LOGI(
        TAG,
        "Token acquired");

    return true;
}

static bool EnsureToken(void)
{
    time_t now =
        time(NULL);

    if (accessToken[0] &&
        now < tokenExpiry)
    {
        return true;
    }

    return RequestToken();
}

bool OpenSky_Init(void)
{
    LoadRateLimit();
    if (responseBuffer != NULL)
    {
        free(responseBuffer);
        responseBuffer = NULL;
    }

    accessToken[0] = '\0';
    tokenExpiry = 0;

    responseCapacity = 65536;

    responseBuffer = heap_caps_malloc(
        responseCapacity,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!responseBuffer)
    {
        ESP_LOGE(TAG, "Could not allocate OpenSky response buffer in PSRAM");
        return false;
    }

    responseBuffer[0] = '\0';

    return LoadCredentials();
}

bool OpenSky_HasCredentials(void)
{
    return strlen(clientId) > 0 &&
           strlen(clientSecret) > 0;
}

bool OpenSky_GetAircraftJson(
    float centerLat,
    float centerLon,
    float radiusKm,
    const char **json)
{
    if (!json || !responseBuffer)
    {
        ESP_LOGE(TAG, "OpenSky response buffer is unavailable");
        return false;
    }
    *json = NULL;
    if (OpenSky_GetRateLimitSeconds() != 0)
        return false;
    bool authenticated = EnsureToken();
    if (OpenSky_GetRateLimitSeconds() != 0)
        return false;
    if (!authenticated)
        ESP_LOGW(TAG, "OAuth unavailable; using anonymous OpenSky request");

    responseLength = 0;
    responseBuffer[0] = '\0';

    // OpenSky's states/all endpoint wants a lat/lon bounding box rather than
    // a point+radius; this is an OpenSky-specific query shape and stays
    // entirely inside this file (see aircraft_provider.h).
    float latDelta = radiusKm / 111.0f;
    float lonDelta = radiusKm / (111.0f * cosf(centerLat * (float)M_PI / 180.0f));

    float minLat = centerLat - latDelta;
    float maxLat = centerLat + latDelta;
    float minLon = centerLon - lonDelta;
    float maxLon = centerLon + lonDelta;

    char url[512];

    snprintf(
        url,
        sizeof(url),
        "https://opensky-network.org/api/states/all?"
        "lamin=%.6f&lamax=%.6f&"
        "lomin=%.6f&lomax=%.6f",
        minLat,
        maxLat,
        minLon,
        maxLon);

    ESP_LOGI(TAG, "Request URL: %s", url);
    // The bounding box is not sensitive, so it's fine to show in full at
    // Normal diagnostics (unlike the OAuth request, which never logs its
    // body/headers - see RequestToken()).
    ProviderDiag_RequestStart("OpenSky", "aircraft request", url);
    LogHttpMemory("Before aircraft TLS");

    esp_http_client_config_t config =
        {
            .url = url,
            .event_handler = HttpEventHandler,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            //.cert_pem = (const char *)isrgrootx1_pem_start,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 30000, // HTTP request timeout, NOT the poll interval.
            // The poll interval lives in main.c (radarRefreshSec, web-configurable).
            .buffer_size = 2048,
            // The bearer token and other request headers must fit together.
            .buffer_size_tx = 4096,
        };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (!client)
    {
        ESP_LOGE(TAG, "Could not allocate aircraft HTTP client");
        return false;
    }

    esp_http_client_set_method(
        client,
        HTTP_METHOD_GET);

    if (authenticated)
    {
        char bearer[2200];
        snprintf(bearer, sizeof(bearer), "Bearer %s", accessToken);
        esp_http_client_set_header(client, "Authorization", bearer);
    }

    esp_err_t err =
        esp_http_client_perform(client);

    /*
ESP_LOGI(
    TAG,
    "perform=%s",
    esp_err_to_name(err));

ESP_LOGI(
    TAG,
    "status=%d",
    esp_http_client_get_status_code(client));

ESP_LOGI(TAG, "response=%s", responseBuffer);

ESP_LOGI(TAG, "RequestToken start");
*/

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "HTTP GET failed: %s",
            esp_err_to_name(err));

        ProviderDiag_RequestDone("OpenSky", "aircraft request", 0, 0, false);
        esp_http_client_cleanup(client);
        return false;
    }

    int status =
        esp_http_client_get_status_code(client);

    ESP_LOGI(
        TAG,
        "HTTP Status = %d, response bytes = %u",
        status,
        (unsigned)responseLength);

    esp_http_client_cleanup(client);

    ProviderDiag_RequestDone("OpenSky", "aircraft request", status, responseLength, status == 200);
    ProviderDiag_RawPreview("OpenSky", responseBuffer, responseLength);

    if (status == 429)
    {
        PauseAfterRateLimit("Aircraft states request");
        return false;
    }

    if (status != 200)
    {
        ESP_LOGE(
            TAG,
            "Unexpected HTTP status: %d",
            status);
        ESP_LOGE(TAG, "Missing access_token");
        return false;
    }

    *json = responseBuffer;

    return true;
}