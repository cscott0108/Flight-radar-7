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
#include "diag_telemetry.h"   /* DiagTelemetry_NowMs (request elapsed time) */
#include "provider_http_diag.h"
#include "visibility_policy.h" /* VisPolicy_Admit: the shared on-ground / visibility decision */

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
/* 0.1.9: esp_http_client ignores the event handler's return value (http_on_body), so a full buffer does NOT
 * stop the request and perform() still reports HTTP 200. Once a chunk does not fit, nothing more is stored (no
 * gaps from a later, smaller chunk) and the request is reported as truncated instead of successful. */
static bool responseTruncated = false;
static size_t responseReceived = 0;

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
        responseReceived += (size_t)evt->data_len;
        if (responseTruncated)
            return ESP_FAIL; /* already cut off: keep the stored prefix contiguous */
        if (responseLength + evt->data_len + 1 > responseCapacity)
        {
            responseTruncated = true;
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
    responseTruncated = false;
    responseReceived = 0;

    float radiusNm = radiusKm * KM_TO_NM;
    if (radiusNm > ADSBLOL_MAX_RADIUS_NM)
        radiusNm = ADSBLOL_MAX_RADIUS_NM;
    if (radiusNm < 1.0f)
        radiusNm = 1.0f;

    char url[192];
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.6f/%.6f/%.0f",
             centerLat, centerLon, radiusNm);

    /* 0.1.9: request number, the exact URL handed to the client, timing and outcome for /diag and the log. */
    ProviderFetch fetch;
    const uint32_t seq = ProviderDiag_FetchStart(&fetch, AIRCRAFT_PROVIDER_ADSBLOL, url);
    char what[40];
    snprintf(what, sizeof(what), "aircraft request #%u", (unsigned)seq);
    ProviderDiag_RequestStart("adsb.lol", what, url);
    ProviderDiag_HeapBeforeTls("adsb.lol"); /* Normal and above: the figures OpenSky logs before its TLS session */

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
        ProviderDiag_FetchEnd(&fetch, PPOLL_CLIENT_INIT);
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        fetch.espErr = err;
        fetch.transport = ProviderHttp_TransportKind(err);
        (void)esp_http_client_get_and_clear_last_tls_error(client, &fetch.tlsErr, &fetch.tlsFlags);
        fetch.httpStatus = esp_http_client_get_status_code(client); /* 0 unless a response header arrived */
        fetch.bytes = (uint32_t)responseLength;
        fetch.received = (uint32_t)responseReceived;
        fetch.truncated = responseTruncated;
        ProviderDiag_RequestDone("adsb.lol", what, 0, 0, false, DiagTelemetry_NowMs() - fetch.t0Ms);
        esp_http_client_cleanup(client);
        ProviderDiag_FetchEnd(&fetch, PPOLL_TRANSPORT);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    fetch.httpStatus = status;
    fetch.contentLength = esp_http_client_get_content_length(client);
    fetch.complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    fetch.bytes = (uint32_t)responseLength;
    fetch.received = (uint32_t)responseReceived;
    fetch.truncated = responseTruncated;

    ProviderDiag_RequestDone("adsb.lol", what, status, responseLength, status == 200 && !responseTruncated,
                             DiagTelemetry_NowMs() - fetch.t0Ms);
    ProviderDiag_RawPreview("adsb.lol", responseBuffer, responseLength);

    if (status == 429 || status == 503)
    {
        PauseAfterRateLimit("Aircraft point request");
        ProviderDiag_FetchEnd(&fetch, PPOLL_RATE_LIMITED);
        return false;
    }

    if (status != 200)
    {
        ESP_LOGE(TAG, "Unexpected HTTP status: %d", status);
        ProviderDiag_FetchEnd(&fetch, PPOLL_HTTP_STATUS);
        return false;
    }

    if (responseTruncated)
    {
        /* HTTP 200 is not a complete response: the body did not fit the bounded buffer (0.1.9). The handler's
         * existing error line already printed; FetchEnd logs "response truncated" at Normal and records it. */
        ProviderDiag_FetchEnd(&fetch, PPOLL_TRUNCATED);
        return false;
    }

    ProviderDiag_FetchEnd(&fetch, PPOLL_FETCHED);
    *json = responseBuffer;
    return true;
}

/* HOSTTEST:BEGIN parse (extracted verbatim by host_tests/visibility_test.c) */
bool AdsbLol_ParseAircraft(const char *json)
{
    /* gAircraft/gAircraftCount are only replaced once the response has been
     * accepted as structurally valid (below). A malformed, truncated or
     * unexpected response therefore leaves the previous poll's aircraft in
     * place - the same stale-data behavior a failed HTTP request has. */
    int rawCount = 0;
    int rejectedCount = 0;
    /* 0.1.9: stage counts (aircraft_provider.h); observation only, nothing below depends on them */
    ProviderParseStats st;
    memset(&st, 0, sizeof(st));
    int examined0 = 0, samples = 0, admittedSamples = 0;
    const bool rawDiag = AircraftProvider_GetDebugLevel() >= PROVIDER_DEBUG_RAW;

    cJSON *root = cJSON_Parse(json);
    if (!root)
    {
        const char *at = cJSON_GetErrorPtr();
        const size_t len = json ? strlen(json) : 0;
        /* cJSON's error pointer is process-global: use it only when it points into this body. */
        const int off = (at && json && at >= json && at <= json + len) ? (int)(at - json) : -1;
        char msg[96];
        snprintf(msg, sizeof(msg), "JSON parse failed near byte %d of %u", off, (unsigned)len);
        ProviderDiag_Warning("adsb.lol", msg);
        ProviderDiag_ParseResult("adsb.lol", false, 0, 0, 0);
        ProviderDiag_ParseStats(AIRCRAFT_PROVIDER_ADSBLOL, PPOLL_JSON_INVALID, NULL, off);
        return false;
    }

    cJSON *ac = cJSON_GetObjectItem(root, "ac");
    if (!ac)
    {
        /* No "ac" key at all is not an empty sky (that is "ac":[]); it is an
         * error/unknown document, so keep the previous aircraft. */
        cJSON_Delete(root);
        ProviderDiag_Warning("adsb.lol", "response has no \"ac\" field");
        ProviderDiag_ParseResult("adsb.lol", false, 0, 0, 0);
        ProviderDiag_ParseStats(AIRCRAFT_PROVIDER_ADSBLOL, PPOLL_UNEXPECTED_SHAPE, NULL, -1);
        return false;
    }

    if (cJSON_IsNull(ac))
    {
        /* No aircraft in range: a normal, successful empty response. */
        gAircraftCount = 0;
        cJSON_Delete(root);
        ProviderDiag_ParseResult("adsb.lol", true, 0, 0, 0);
        ProviderDiag_ParseStats(AIRCRAFT_PROVIDER_ADSBLOL, PPOLL_OK_EMPTY, &st, -1);
        return true;
    }

    if (!cJSON_IsArray(ac))
    {
        cJSON_Delete(root);
        ProviderDiag_Warning("adsb.lol", "\"ac\" field was present but not an array/null");
        ProviderDiag_ParseResult("adsb.lol", false, 0, 0, 0);
        ProviderDiag_ParseStats(AIRCRAFT_PROVIDER_ADSBLOL, PPOLL_UNEXPECTED_SHAPE, NULL, -1);
        return false;
    }

    int count = cJSON_GetArraySize(ac);
    st.entries = count;

    gAircraftCount = 0; /* the response is valid: replace the previous list */

    /* Pass 0 admits airborne aircraft (the 0.0.25 list); pass 1 (only when a
     * ground visibility/retention policy is enabled) adds on-ground aircraft
     * into the remaining slots, so airborne aircraft always have priority. */
    const int passes = VisPolicy_Passes();
    for (int pass = 0; pass < passes; pass++)
    /* Pass 1 keeps going when the list is full: those on-ground aircraft are
     * parsed into a scratch slot only so the airport counts (Y) include them;
     * VisPolicy_Admit never admits them. */
    for (int i = 0; i < count && (gAircraftCount < MAX_AIRCRAFT || pass == 1); i++)
    {
        cJSON *entry = cJSON_GetArrayItem(ac, i);
        if (pass == 0)
            examined0++;
        if (!cJSON_IsObject(entry))
        {
            if (pass == 0)
                st.malformed++;
            continue;
        }

        if (pass == 0)
            rawCount++;

        cJSON *hex = cJSON_GetObjectItem(entry, "hex");
        cJSON *flight = cJSON_GetObjectItem(entry, "flight");
        cJSON *lat = cJSON_GetObjectItem(entry, "lat");
        cJSON *lon = cJSON_GetObjectItem(entry, "lon");
        cJSON *gs = cJSON_GetObjectItem(entry, "gs");
        cJSON *track = cJSON_GetObjectItem(entry, "track");
        cJSON *altBaro = cJSON_GetObjectItem(entry, "alt_baro");
        cJSON *category = cJSON_GetObjectItem(entry, "category");

        if (!cJSON_IsString(hex) || !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon) ||
            !Aircraft_IsValidPosition(lat->valuedouble, lon->valuedouble))
        {
            if (pass == 0)
            {
                rejectedCount++;
                const char *why;
                if (!cJSON_IsString(hex)) { st.missingId++; why = "missing hex"; }
                else if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) { st.missingPosition++; why = "missing lat/lon"; }
                else { st.invalidPosition++; why = "lat/lon out of range"; }
                if (rawDiag && samples < PROVIDER_DIAG_SAMPLES_MAX - 3) /* up to 5 rejected + 3 admitted */
                {
                    samples++;
                    ProviderDiag_Sample("adsb.lol", "#%d rejected (%s): hex=%.12s lat=%s%.4f lon=%s%.4f", i, why,
                                        cJSON_IsString(hex) ? hex->valuestring : "-",
                                        cJSON_IsNumber(lat) ? "" : "n/a ", cJSON_IsNumber(lat) ? lat->valuedouble : 0.0,
                                        cJSON_IsNumber(lon) ? "" : "n/a ", cJSON_IsNumber(lon) ? lon->valuedouble : 0.0);
                }
            }
            continue;
        }

        static Aircraft s_countOnlySlot; /* list full (pass 1 only): counted, never admitted */
        Aircraft *a = gAircraftCount < MAX_AIRCRAFT ? &gAircraft[gAircraftCount] : &s_countOnlySlot;
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

        /* Registration ("r", readsb/ADSBExchange-v2 field; absent for many
         * aircraft): kept as plain upper-case letters, digits and '-'. */
        cJSON *reg = cJSON_GetObjectItem(entry, "r");
        if (reg && cJSON_IsString(reg))
        {
            size_t used = 0;
            for (const char *p = reg->valuestring; *p && used < sizeof(a->registration) - 1; p++)
            {
                char c = *p;
                if (c >= 'a' && c <= 'z')
                    c = (char)(c - 'a' + 'A');
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')
                    a->registration[used++] = c;
            }
            a->registration[used] = '\0';
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
        {
            a->velocity = (float)gs->valuedouble * 0.514444f; /* knots -> m/s */
            a->dataFlags |= AIRCRAFT_DATA_VELOCITY;
        }

        /* 0.0.30, display only (Selected Craft trend arrow): vertical rate in
         * ft/min, barometric first (same reference as alt_baro), else geometric. */
        cJSON *vrate = cJSON_GetObjectItem(entry, "baro_rate");
        if (!cJSON_IsNumber(vrate))
            vrate = cJSON_GetObjectItem(entry, "geom_rate");
        if (cJSON_IsNumber(vrate))
        {
            a->verticalRateFpm = Aircraft_ClampFpm(vrate->valuedouble);
            a->dataFlags |= AIRCRAFT_DATA_VRATE;
        }

        if (track && cJSON_IsNumber(track))
            a->trackTrueDeg = (float)track->valuedouble; /* readsb "track": true track over the ground */

        /* 0.0.32: raw heading fields, kept exactly as reported (magnetic stays
         * magnetic, true stays true; neither is derived from the other here). */
        {
            cJSON *magHdg = cJSON_GetObjectItem(entry, "mag_heading");
            cJSON *trueHdg = cJSON_GetObjectItem(entry, "true_heading");
            if (cJSON_IsNumber(magHdg) && magHdg->valuedouble >= 0.0 && magHdg->valuedouble <= 360.0) {
                a->magHeadingDeci = (int16_t)lround(magHdg->valuedouble * 10.0);
                a->dataFlags |= AIRCRAFT_DATA_MAG_HDG;
            }
            if (cJSON_IsNumber(trueHdg) && trueHdg->valuedouble >= 0.0 && trueHdg->valuedouble <= 360.0) {
                a->trueHeadingDeci = (int16_t)lround(trueHdg->valuedouble * 10.0);
                a->dataFlags |= AIRCRAFT_DATA_TRUE_HDG;
            }
        }

        /* Owner/operator: "ownOp" is emitted by readsb-based ADSBExchange-v2
         * services when their aircraft database has an owner for the hex. It
         * is NOT in the published v2 field list we could verify (which
         * documents r, t, dbFlags but not ownOp), so it is parsed
         * opportunistically: absent means no Provider Operator, never an
         * error. */
        cJSON *ownOp = cJSON_GetObjectItem(entry, "ownOp");
        if (ownOp && cJSON_IsString(ownOp))
            AircraftText_Sanitize(a->operatorName, sizeof(a->operatorName), ownOp->valuestring);

        const char *categoryStr = (category && cJSON_IsString(category)) ? category->valuestring : "";
        AircraftType hint = AIRCRAFT_FIXED_WING;
        bool hasHint = AdsbLol_CategoryToAircraftType(categoryStr, &hint);
        a->hasProviderTypeHint = hasHint;
        a->providerTypeHint = hasHint ? hint : AIRCRAFT_FIXED_WING;

        /* On-ground rule (provider "ground" OR low AND slow) and the optional
         * ground visibility policy: one decision for every provider. */
        if (!VisPolicy_Admit(a, reportedOnGround, pass))
        {
            if (pass == 0)
            {
                rejectedCount++;
                st.onGround++;
                if (rawDiag && samples < PROVIDER_DIAG_SAMPLES_MAX - 3)
                {
                    samples++;
                    ProviderDiag_Sample("adsb.lol", "#%d on ground (hidden in the airborne pass): hex=%s alt_baro=%s gs=%.1f m/s",
                                        i, a->icao24, reportedOnGround ? "\"ground\"" : "low", (double)a->velocity);
                }
            }
            continue;
        }
        if (pass == 1)
            st.groundShown++;
        else if (rawDiag && admittedSamples < 3)
        {
            admittedSamples++;
            ProviderDiag_Sample("adsb.lol", "#%d admitted: hex=%s alt_baro=%s%.0f ft -> %.1f m, gs=%s%.1f kn -> %.2f m/s, "
                                "track=%s%.1f deg true (stored as reported)",
                                i, a->icao24, cJSON_IsNumber(altBaro) ? "" : "n/a ",
                                cJSON_IsNumber(altBaro) ? altBaro->valuedouble : 0.0, (double)a->altitude,
                                cJSON_IsNumber(gs) ? "" : "n/a ", cJSON_IsNumber(gs) ? gs->valuedouble : 0.0,
                                (double)a->velocity, cJSON_IsNumber(track) ? "" : "n/a ", (double)a->trackTrueDeg);
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
    st.notExamined = count - examined0;
    st.admitted = gAircraftCount;
    ProviderDiag_ParseStats(AIRCRAFT_PROVIDER_ADSBLOL,
                            st.notExamined > 0 ? PPOLL_OK_PARTIAL : (count ? PPOLL_OK : PPOLL_OK_EMPTY), &st, -1);

    return true;
}
/* HOSTTEST:END parse */
