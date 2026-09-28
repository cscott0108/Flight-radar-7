#include "webserver.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "cJSON.h"
#include <stdlib.h>

#include "esp_system.h"
#include "main.h"
#include "radar.h"
#include "aircraft_provider.h"
#include "web_rules.h"
#include "web_airports.h"
#include "web_diag.h"
#include "web_seen.h"
#include "seen_aircraft.h"
#include "time_util.h"
#include <time.h>

static const char *TAG = "WEBSERVER";
static httpd_handle_t server_handle = NULL;

#define OPENSKY_NAMESPACE "opensky"
#define WIFI_NAMESPACE "wifi"

// Seen Aircraft changes are normally written to flash in batches (see
// SEEN_FLUSH_INTERVAL_SEC); write any pending ones before a deliberate reboot
// so a Wi-Fi or credentials change never costs the last few minutes of history.
static void FlushHistoryBeforeRestart(void)
{
    if (SeenAircraft_IsDirty())
        SeenAircraft_Flush();
}

// Sends a small self-contained HTML page that shows `message` and then
// auto-navigates back to the setup home page after `delaySeconds`. The
// <meta refresh> tag does the actual navigation (works even with
// JavaScript disabled); the script just keeps a visible countdown current.
static void SendRedirectPage(
    httpd_req_t *req,
    const char *message,
    int delaySeconds)
{
    char html[640];

    snprintf(
        html,
        sizeof(html),
        "<!DOCTYPE html><html><head>"
        "<meta http-equiv='refresh' content='%d;url=/'>"
        "<style>body{font-family:sans-serif;text-align:center;margin-top:3em;}</style>"
        "</head><body>"
        "<p>%s</p>"
        "<p id='countdown'>Returning to the home page in %d seconds&hellip;</p>"
        "<script>"
        "let s=%d;"
        "const el=document.getElementById('countdown');"
        "const t=setInterval(function(){"
        "s--;"
        "if(s<=0){clearInterval(t);}"
        "else{el.textContent='Returning to the home page in '+s+' seconds\\u2026';}"
        "},1000);"
        "</script>"
        "</body></html>",
        delaySeconds,
        message,
        delaySeconds,
        delaySeconds);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, html);
}

static bool SaveWifiSetup(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;

    esp_err_t err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK)
        err = nvs_set_str(handle, "pass", password);
    if (err == ESP_OK)
        err = nvs_commit(handle);

    nvs_close(handle);
    return err == ESP_OK;
}

static void UrlDecode(char *value)
{
    char *read = value;
    char *write = value;
    while (*read)
    {
        if (*read == '+')
        {
            *write++ = ' ';
            read++;
        }
        else if (*read == '%' && read[1] && read[2])
        {
            unsigned int byte;
            if (sscanf(read + 1, "%2x", &byte) == 1)
            {
                *write++ = (char)byte;
                read += 3;
            }
            else
            {
                *write++ = *read++;
            }
        }
        else
        {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static bool FormValue(const char *body, const char *name, char *value, size_t valueSize)
{
    char key[48];
    snprintf(key, sizeof(key), "%s=", name);
    const char *start = strstr(body, key);
    if (!start)
        return false;

    start += strlen(key);
    const char *end = strchr(start, '&');
    size_t length = end ? (size_t)(end - start) : strlen(start);
    if (length >= valueSize)
        length = valueSize - 1;
    memcpy(value, start, length);
    value[length] = '\0';
    UrlDecode(value);
    return true;
}

static esp_err_t WifiSetupHandler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 256)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");

    char body[257];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
    body[received] = '\0';

    char ssid[33];
    char password[65];
    if (!FormValue(body, "ssid", ssid, sizeof(ssid)) ||
        !FormValue(body, "password", password, sizeof(password)) ||
        ssid[0] == '\0')
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID and password required");

    if (!SaveWifiSetup(ssid, password))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save Wi-Fi settings");

    SendRedirectPage(
        req,
        "Wi-Fi saved. The device is restarting and will join the new network. "
        "If it joins a different network than this page is on, this redirect "
        "won't reach it &mdash; check your router or the device's screen for its new address.",
        30);
    vTaskDelay(pdMS_TO_TICKS(500));
    FlushHistoryBeforeRestart();
    esp_restart();
    return ESP_OK;
}

static esp_err_t RadarSetupHandler(httpd_req_t *req)
{
    // 768 (was 384): a fully populated form with the provider, diagnostics and
    // time zone fields is already close to 400 bytes.
    if (req->content_len <= 0 || req->content_len > 768)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");

    char body[769];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
    body[received] = '\0';

    char latitudeText[24];
    char longitudeText[24];
    char rangeText[24];
    if (!FormValue(body, "latitude", latitudeText, sizeof(latitudeText)) ||
        !FormValue(body, "longitude", longitudeText, sizeof(longitudeText)) ||
        !FormValue(body, "range", rangeText, sizeof(rangeText)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Latitude and longitude required");

    char *end = NULL;
    float latitude = strtof(latitudeText, &end);
    if (end == latitudeText || latitude < -90.0f || latitude > 90.0f)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Latitude must be between -90 and 90");

    end = NULL;
    float longitude = strtof(longitudeText, &end);
    if (end == longitudeText || longitude < -180.0f || longitude > 180.0f)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Longitude must be between -180 and 180");

    end = NULL;
    float range = strtof(rangeText, &end);
    if (end == rangeText || range < 1.0f || range > 5000.0f)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Range must be between 1 and 5000 km");

    // Refresh interval is optional so older bookmarked forms still post.
    char refreshText[24];
    uint32_t refreshSeconds = GetRadarRefreshSeconds();

    if (FormValue(body, "refresh", refreshText, sizeof(refreshText)) &&
        refreshText[0] != '\0')
    {
        end = NULL;
        long parsed = strtol(refreshText, &end, 10);

        if (end == refreshText || parsed < 10 || parsed > 600)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Refresh must be between 10 and 600 seconds");

        refreshSeconds = (uint32_t)parsed;
    }

    // Low-traffic settings are also optional, for the same reason.
    char lowThresholdText[24];
    uint32_t lowThreshold = GetRadarLowTrafficThreshold();

    if (FormValue(body, "low_threshold", lowThresholdText, sizeof(lowThresholdText)) &&
        lowThresholdText[0] != '\0')
    {
        end = NULL;
        long parsed = strtol(lowThresholdText, &end, 10);

        if (end == lowThresholdText || parsed < 0 || parsed > 500)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Low-traffic aircraft threshold must be between 0 and 500");

        lowThreshold = (uint32_t)parsed;
    }

    char lowIntervalText[24];
    uint32_t lowInterval = GetRadarLowTrafficIntervalSeconds();

    if (FormValue(body, "low_interval", lowIntervalText, sizeof(lowIntervalText)) &&
        lowIntervalText[0] != '\0')
    {
        end = NULL;
        long parsed = strtol(lowIntervalText, &end, 10);

        if (end == lowIntervalText || parsed < 10 || parsed > 600)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Low-traffic refresh interval must be between 10 and 600 seconds");

        lowInterval = (uint32_t)parsed;
    }

    // Day/night scheduling fields - all optional, all off by default.
    bool dayNightEnabled = strstr(body, "daynight_enabled=on") != NULL;

    char utcOffsetText[24];
    int32_t utcOffsetMinutes = GetRadarUtcOffsetMinutes();

    if (FormValue(body, "utc_offset", utcOffsetText, sizeof(utcOffsetText)) &&
        utcOffsetText[0] != '\0')
    {
        end = NULL;
        long parsed = strtol(utcOffsetText, &end, 10);

        if (end == utcOffsetText || parsed < -720 || parsed > 840)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "UTC offset must be between -720 and 840 minutes");

        utcOffsetMinutes = (int32_t)parsed;
    }

    // Time zone: an IANA id from the built-in table, or CUSTOM (fixed offset
    // above). Optional so an older cached form still posts; without a
    // "timezone" field the zone and Automatic DST are left as they are.
    TimeZoneConfig tzConfig;
    TimeUtil_GetConfig(&tzConfig);

    char zoneText[64];
    if (FormValue(body, "timezone", zoneText, sizeof(zoneText)) &&
        zoneText[0] != '\0')
    {
        size_t zoneIndex;

        if (strcmp(zoneText, TIMEUTIL_ZONE_CUSTOM) != 0 &&
            !TimeUtil_ZoneFind(zoneText, &zoneIndex))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Unknown time zone");

        // Both branches above accepted only a known id (or CUSTOM), all of which
        // fit; the precision keeps the copy provably in bounds.
        snprintf(tzConfig.zoneId, sizeof(tzConfig.zoneId), "%.*s",
                 (int)sizeof(tzConfig.zoneId) - 1, zoneText);
        tzConfig.autoDst = strstr(body, "auto_dst=on") != NULL;
    }

    char dayStartText[24];
    uint32_t dayStartHour = GetRadarDayStartHour();

    if (FormValue(body, "day_start", dayStartText, sizeof(dayStartText)) &&
        dayStartText[0] != '\0')
    {
        end = NULL;
        long parsed = strtol(dayStartText, &end, 10);

        if (end == dayStartText || parsed < 0 || parsed > 23)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Day start hour must be between 0 and 23");

        dayStartHour = (uint32_t)parsed;
    }

    char dayEndText[24];
    uint32_t dayEndHour = GetRadarDayEndHour();

    if (FormValue(body, "day_end", dayEndText, sizeof(dayEndText)) &&
        dayEndText[0] != '\0')
    {
        end = NULL;
        long parsed = strtol(dayEndText, &end, 10);

        if (end == dayEndText || parsed < 0 || parsed > 23)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Day end hour must be between 0 and 23");

        dayEndHour = (uint32_t)parsed;
    }

    char dayIntervalText[24];
    uint32_t dayIntervalSec = GetRadarDayIntervalSeconds();

    if (FormValue(body, "day_interval", dayIntervalText, sizeof(dayIntervalText)) &&
        dayIntervalText[0] != '\0')
    {
        end = NULL;
        long parsed = strtol(dayIntervalText, &end, 10);

        if (end == dayIntervalText || parsed < 10 || parsed > 600)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Day interval must be between 10 and 600 seconds");

        dayIntervalSec = (uint32_t)parsed;
    }

    char nightIntervalText[24];
    uint32_t nightIntervalSec = GetRadarNightIntervalSeconds();

    if (FormValue(body, "night_interval", nightIntervalText, sizeof(nightIntervalText)) &&
        nightIntervalText[0] != '\0')
    {
        end = NULL;
        long parsed = strtol(nightIntervalText, &end, 10);

        if (end == nightIntervalText || parsed < 10 || parsed > 600)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Night interval must be between 10 and 600 seconds");

        nightIntervalSec = (uint32_t)parsed;
    }

    // Day/night BRIGHTNESS schedule - reuses the day window above, but is
    // its own independent on/off switch from the polling schedule.
    bool dayNightBrightnessEnabled = strstr(body, "daynight_brightness_enabled=on") != NULL;

    char dayBrightnessText[24];
    uint32_t dayBrightnessPercent = GetRadarDayBrightnessPercent();

    if (FormValue(body, "day_brightness", dayBrightnessText, sizeof(dayBrightnessText)) &&
        dayBrightnessText[0] != '\0')
    {
        end = NULL;
        long parsed = strtol(dayBrightnessText, &end, 10);

        if (end == dayBrightnessText || parsed < 1 || parsed > 100)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Day brightness must be between 1 and 100");

        dayBrightnessPercent = (uint32_t)parsed;
    }

    char nightBrightnessText[24];
    uint32_t nightBrightnessPercent = GetRadarNightBrightnessPercent();

    if (FormValue(body, "night_brightness", nightBrightnessText, sizeof(nightBrightnessText)) &&
        nightBrightnessText[0] != '\0')
    {
        end = NULL;
        long parsed = strtol(nightBrightnessText, &end, 10);

        if (end == nightBrightnessText || parsed < 1 || parsed > 100)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Night brightness must be between 1 and 100");

        nightBrightnessPercent = (uint32_t)parsed;
    }

    // Idle dimming - dims below the day/night/manual brightness after N
    // minutes with zero aircraft in range.
    bool idleDimEnabled = strstr(body, "idle_dim_enabled=on") != NULL;

    char idleDimMinutesText[24];
    uint32_t idleDimMinutes = GetRadarIdleDimMinutes();

    if (FormValue(body, "idle_dim_minutes", idleDimMinutesText, sizeof(idleDimMinutesText)) &&
        idleDimMinutesText[0] != '\0')
    {
        end = NULL;
        long parsed = strtol(idleDimMinutesText, &end, 10);

        if (end == idleDimMinutesText || parsed < 1 || parsed > 1440)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Idle dim minutes must be between 1 and 1440");

        idleDimMinutes = (uint32_t)parsed;
    }

    char idleDimPercentText[24];
    uint32_t idleDimPercent = GetRadarIdleDimPercent();

    if (FormValue(body, "idle_dim_percent", idleDimPercentText, sizeof(idleDimPercentText)) &&
        idleDimPercentText[0] != '\0')
    {
        end = NULL;
        long parsed = strtol(idleDimPercentText, &end, 10);

        if (end == idleDimPercentText || parsed < 0 || parsed > 10)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Idle dim percent must be between 0 and 10");

        idleDimPercent = (uint32_t)parsed;
    }

    char providerText[24];
    AircraftProviderType providerType;
    if (FormValue(body, "provider", providerText, sizeof(providerText)) &&
        AircraftProviderType_Parse(providerText, &providerType))
    {
        AircraftProvider_SetActive(providerType);
    }

    char providerDebugText[24];
    ProviderDebugLevel debugLevel;
    if (FormValue(body, "provider_debug", providerDebugText, sizeof(providerDebugText)) &&
        ProviderDebugLevel_Parse(providerDebugText, &debugLevel))
    {
        AircraftProvider_SetDebugLevel(debugLevel);
    }

    SetRadarSettings(latitude, longitude, range);
    SetRadarRefreshSeconds(refreshSeconds);
    SetRadarLowTrafficThreshold(lowThreshold);
    SetRadarLowTrafficIntervalSeconds(lowInterval);
    SetRadarTimeZone(
        tzConfig.zoneId,
        tzConfig.autoDst,
        utcOffsetMinutes);
    SetRadarDayNightSchedule(
        dayNightEnabled,
        dayStartHour,
        dayEndHour,
        dayIntervalSec,
        nightIntervalSec);
    SetRadarDayNightBrightnessSchedule(
        dayNightBrightnessEnabled,
        dayBrightnessPercent,
        nightBrightnessPercent);
    SetRadarIdleDimSettings(
        idleDimEnabled,
        idleDimMinutes,
        idleDimPercent);
    Radar_SetAutoSelectClosest(strstr(body, "auto_closest=on") != NULL);
    SetRadarOpenSkyDebugEnabled(strstr(body, "opensky_debug=on") != NULL);

    char response[700];
    int len = 0;

    len += snprintf(response + len, sizeof(response) - len,
        "Radar settings saved. ");

    len += snprintf(response + len, sizeof(response) - len,
        "Time zone: %s%s. ",
        strcmp(tzConfig.zoneId, TIMEUTIL_ZONE_CUSTOM) == 0 ? "custom UTC offset" : tzConfig.zoneId,
        (tzConfig.autoDst || strcmp(tzConfig.zoneId, TIMEUTIL_ZONE_CUSTOM) == 0) ? "" : " (daylight saving off)");

    if (GetRadarDayNightEnabled())
    {
        len += snprintf(response + len, sizeof(response) - len,
            "Day/night scheduling on: %lus by day (%02lu:00-%02lu:00 local), "
            "%lus overnight. ",
            (unsigned long)GetRadarDayIntervalSeconds(),
            (unsigned long)GetRadarDayStartHour(),
            (unsigned long)GetRadarDayEndHour(),
            (unsigned long)GetRadarNightIntervalSeconds());
    }
    else
    {
        len += snprintf(response + len, sizeof(response) - len,
            "Refresh interval: %lus. ",
            (unsigned long)GetRadarRefreshSeconds());
    }

    if (GetRadarDayNightBrightnessEnabled())
    {
        len += snprintf(response + len, sizeof(response) - len,
            "Day/night brightness on: %lu%% by day, %lu%% overnight (same %02lu:00-%02lu:00 window). ",
            (unsigned long)GetRadarDayBrightnessPercent(),
            (unsigned long)GetRadarNightBrightnessPercent(),
            (unsigned long)GetRadarDayStartHour(),
            (unsigned long)GetRadarDayEndHour());
    }

    if (GetRadarIdleDimEnabled())
    {
        len += snprintf(response + len, sizeof(response) - len,
            "Idle dimming on: drops to %lu%% after %lu minutes with no aircraft in range. ",
            (unsigned long)GetRadarIdleDimPercent(),
            (unsigned long)GetRadarIdleDimMinutes());
    }

    if (GetRadarLowTrafficThreshold() > 0)
    {
        len += snprintf(response + len, sizeof(response) - len,
            "Stretched to %lus when fewer than %lu aircraft are in range. ",
            (unsigned long)GetRadarLowTrafficIntervalSeconds(),
            (unsigned long)GetRadarLowTrafficThreshold());
    }
    else
    {
        len += snprintf(response + len, sizeof(response) - len,
            "Low-traffic slowdown disabled. ");
    }

    if (GetRadarOpenSkyDebugEnabled())
    {
        snprintf(response + len, sizeof(response) - len,
            "OpenSky field debug logging is ON &mdash; check the serial console.");
    }

    SendRedirectPage(req, response, 10);
    return ESP_OK;
}

// Applies (and persists) a new backlight brightness. Called via fetch()
// from the slider on the home page, so this responds with a short plain
// text body rather than a full redirect page.
static esp_err_t BrightnessHandler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 64)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");

    char body[65];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
    body[received] = '\0';

    char brightnessText[24];
    if (!FormValue(body, "brightness", brightnessText, sizeof(brightnessText)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing brightness");

    char *end = NULL;
    long parsed = strtol(brightnessText, &end, 10);
    if (end == brightnessText || parsed < 1 || parsed > 100)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Brightness must be between 1 and 100");

    SetRadarBrightness((uint32_t)parsed);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static bool SaveCredentials(
    const char *clientId,
    const char *clientSecret)
{
    nvs_handle_t handle;

    esp_err_t err =
        nvs_open(
            OPENSKY_NAMESPACE,
            NVS_READWRITE,
            &handle);

    ESP_LOGI(
        TAG,
        "nvs_open=%s",
        esp_err_to_name(err));

    if (err != ESP_OK)
    {
        return false;
    }

    err = nvs_set_str(
        handle,
        "client_id",
        clientId);

    ESP_LOGI(
        TAG,
        "nvs_set_str(client_id)=%s",
        esp_err_to_name(err));

    err = nvs_set_str(
        handle,
        "client_secret",
        clientSecret);

    ESP_LOGI(
        TAG,
        "nvs_set_str(client_secret)=%s",
        esp_err_to_name(err));

    err = nvs_commit(handle);

    ESP_LOGI(
        TAG,
        "nvs_commit=%s",
        esp_err_to_name(err));

    nvs_close(handle);

    return err == ESP_OK;
}

static esp_err_t DeleteCredentialsHandler(httpd_req_t *req)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OPENSKY_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK)
    {
        nvs_erase_key(handle, "client_id");
        nvs_erase_key(handle, "client_secret");
        err = nvs_commit(handle);
        nvs_close(handle);
    }

    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not delete credentials");

    SendRedirectPage(req, "OpenSky credentials deleted. Restarting.", 30);
    vTaskDelay(pdMS_TO_TICKS(500));
    FlushHistoryBeforeRestart();
    esp_restart();
    return ESP_OK;
}

static esp_err_t UploadHandler(
    httpd_req_t *req)
{
    int totalLen = req->content_len;

    if (totalLen <= 0 || totalLen > 2048)
    {
        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Invalid file");

        return ESP_FAIL;
    }

    // CPU-only (httpd_req_recv copies into it, then cJSON_Parse reads it):
    // keep it out of scarce internal DMA-capable RAM.
    char *buffer = heap_caps_malloc(totalLen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!buffer)
    {
        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Out of memory");

        return ESP_FAIL;
    }

    int received = 0;

    while (received < totalLen)
    {
        int ret = httpd_req_recv(
            req,
            buffer + received,
            totalLen - received);

        if (ret <= 0)
        {
            free(buffer);

            httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Receive failed");

            return ESP_FAIL;
        }

        received += ret;
    }

    buffer[received] = '\0';

    ESP_LOGI(TAG, "Received:\n%s", buffer);

    cJSON *root = cJSON_Parse(buffer);

    free(buffer);

    if (!root)
    {
        httpd_resp_send(
            req,
            "Invalid JSON",
            HTTPD_RESP_USE_STRLEN);

        return ESP_FAIL;
    }

    cJSON *clientId =
        cJSON_GetObjectItem(
            root,
            "clientId");

    cJSON *clientSecret =
        cJSON_GetObjectItem(
            root,
            "clientSecret");

    if (!cJSON_IsString(clientId) ||
        !cJSON_IsString(clientSecret))
    {
        cJSON_Delete(root);

        httpd_resp_send(
            req,
            "Missing clientId/clientSecret",
            HTTPD_RESP_USE_STRLEN);

        return ESP_FAIL;
    }

    SaveCredentials(
        clientId->valuestring,
        clientSecret->valuestring);

    cJSON_Delete(root);

    httpd_resp_send(
        req,
        "Credentials Saved",
        HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1000));
    FlushHistoryBeforeRestart();
    esp_restart();

    return ESP_OK;
}

// Formats a float with a fixed number of decimal places using only
// integer arithmetic, deliberately avoiding printf's %f path. Embedded
// newlib's floating-point formatting can use a surprising amount of stack,
// and this function runs inside the httpd worker task, which ESP-IDF
// gives a fairly small default stack. Blowing that stack manifested as
// crashes on every page load before this was in place.
static void FormatFixed(
    float value,
    int decimals,
    char *out,
    size_t outSize)
{
    bool negative = (value < 0.0f);

    if (negative)
    {
        value = -value;
    }

    // Clamp to what the fixed-size fracDigits buffer below can hold.
    if (decimals < 0)
    {
        decimals = 0;
    }
    else if (decimals > 8)
    {
        decimals = 8;
    }

    long scale = 1;

    for (int i = 0; i < decimals; i++)
    {
        scale *= 10;
    }

    long scaledValue = (long)(value * (float)scale + 0.5f);
    long intPart = scaledValue / scale;
    long fracPart = scaledValue % scale;

    if (decimals > 0)
    {
        // Zero-pad the fractional part by hand rather than with printf's
        // "%0*ld" dynamic-width directive: GCC's format-truncation checker
        // can't bound a runtime width, so it assumes a worst case and
        // trips -Werror=format-truncation on ESP-IDF's strict build flags.
        char fracDigits[9];

        fracDigits[decimals] = '\0';

        for (int i = decimals - 1; i >= 0; i--)
        {
            fracDigits[i] = (char)('0' + (fracPart % 10));
            fracPart /= 10;
        }

        snprintf(
            out,
            outSize,
            "%s%ld.%s",
            negative ? "-" : "",
            intPart,
            fracDigits);
    }
    else
    {
        snprintf(
            out,
            outSize,
            "%s%ld",
            negative ? "-" : "",
            intPart);
    }
}

// Time zone section of the setup page. Generated separately from the big
// page template (it carries the whole zone list, about 9 KB) and streamed in
// small chunks. All values here come from the built-in zone table, so none
// needs HTML escaping, and none is ever passed as a format string.
static esp_err_t SendTimeZoneFieldset(httpd_req_t *req)
{
    TimeZoneConfig cfg;
    TimeUtil_GetConfig(&cfg);
    bool custom = (strcmp(cfg.zoneId, TIMEUTIL_ZONE_CUSTOM) == 0);

    if (httpd_resp_send_chunk(
            req,
            "<fieldset><legend>Time zone</legend>"
            "<label>Time zone <select name='timezone'>",
            HTTPD_RESP_USE_STRLEN) != ESP_OK)
        return ESP_FAIL;

    char chunk[1024];
    size_t used = 0;

    for (size_t i = 0; i < TimeUtil_ZoneCount(); i++)
    {
        char label[96];
        char option[160];

        TimeUtil_ZoneLabel(i, label, sizeof(label));

        int length = snprintf(
            option,
            sizeof(option),
            "<option value='%s'%s>%s</option>",
            TimeUtil_ZoneId(i),
            (!custom && strcmp(cfg.zoneId, TimeUtil_ZoneId(i)) == 0) ? " selected" : "",
            label);

        if (length <= 0 || length >= (int)sizeof(option))
            continue;

        if (used + (size_t)length > sizeof(chunk))
        {
            if (httpd_resp_send_chunk(req, chunk, (ssize_t)used) != ESP_OK)
                return ESP_FAIL;
            used = 0;
        }

        memcpy(chunk + used, option, (size_t)length);
        used += (size_t)length;
    }

    if (used > 0 && httpd_resp_send_chunk(req, chunk, (ssize_t)used) != ESP_OK)
        return ESP_FAIL;

    char nowText[64];
    time_t now = time(NULL);

    if (TimeUtil_IsSynced((int64_t)now))
        TimeUtil_FormatLocal((int64_t)now, nowText, sizeof(nowText));
    else
        snprintf(nowText, sizeof(nowText), "clock not synchronized yet");

    int length = snprintf(
        chunk,
        sizeof(chunk),
        "<option value='" TIMEUTIL_ZONE_CUSTOM "'%s>Custom fixed UTC offset</option></select></label>"
        "<label><input name='auto_dst' type='checkbox'%s> Adjust for daylight saving time automatically</label>"
        "<small>Turn this off for regions that do not observe daylight saving time, or to stay on standard time "
        "all year. It has no effect for zones that never observe it.</small>"
        "<label>Custom UTC offset <input name='utc_offset' type='number' min='-720' max='840' step='1' value='%d'> minutes</label>"
        "<small>Used only with &quot;Custom fixed UTC offset&quot; (for example -420 = UTC-7, 330 = UTC+5:30) "
        "and never adjusted for daylight saving. Zones not in the list can use this.</small>"
        "<small>Device time: %s. Used for the day/night schedules and for Seen Aircraft timestamps "
        "(stored in UTC, shown in this zone).</small></fieldset>",
        custom ? " selected" : "",
        cfg.autoDst ? " checked" : "",
        (int)cfg.customOffsetMinutes,
        nowText);

    if (length <= 0 || length >= (int)sizeof(chunk))
        return ESP_FAIL;

    return httpd_resp_send_chunk(req, chunk, (ssize_t)length);
}

static esp_err_t RootHandler(
    httpd_req_t *req)
{
    const char htmlFormatA[] =
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<style>"
        "body{font-family:sans-serif;max-width:640px;margin:0 auto;padding:1em;line-height:1.5;color:#222;}"
        "h2{border-bottom:2px solid #4CAF50;padding-bottom:4px;margin-top:1.2em;}"
        "h3{margin-top:0;}"
        "fieldset{border:1px solid #bbb;border-radius:6px;margin:1em 0;padding:0.6em 1em 1em;}"
        "legend{font-weight:bold;padding:0 6px;}"
        "details{border:1px solid #ddd;border-radius:6px;margin:0.8em 0;padding:0.2em 1em;}"
        "details summary{cursor:pointer;font-weight:bold;padding:6px 0;}"
        "label{display:block;margin:0.6em 0;}"
        "input[type=number],input[type=text],input[type=password]{width:130px;}"
        "small{color:#666;display:block;margin:2px 0 10px 0;}"
        "button{margin:0.8em 0;padding:6px 16px;}"
        "a{color:#2a7a2a;}"
        "</style>"
        "</head>"
        "<body>"
        "<h2>Flight Radar Setup</h2>"
        "<p><a href='/rules'>Craft Types: Registry, Operators and Current Aircraft</a> &middot; "
        "<a href='/seen'>Seen Aircraft</a> &middot; "
        "<a href='/airports'>Add or Edit Airports</a> &middot; "
        "<a href='/diag'>Runtime Capacity Report</a></p>"
        "<h3>Wi-Fi</h3>"
        "<form method='POST' action='/wifi'>"
        "<label>Network name <input name='ssid' maxlength='32'></label>"
        "<label>Password <input name='password' type='password' maxlength='64'></label>"
        "<button type='submit'>Save Wi-Fi</button>"
        "</form>"
        "<h3>Radar Settings</h3>"
        "<form method='POST' action='/radar'>"
        "<fieldset><legend>Location &amp; range</legend>"
        "<label>Latitude <input name='latitude' type='number' step='any' min='-90' max='90' value='%s'></label>"
        "<label>Longitude <input name='longitude' type='number' step='any' min='-180' max='180' value='%s'></label>"
        "<label>Range <input name='range' type='number' step='any' min='1' max='5000' value='%s'> km</label>"
        "<label>Aircraft data refresh interval <input name='refresh' type='number' min='10' max='600' step='1' value='%lu'> seconds</label>"
        "<small>10-600 s. Values below 25 s risk exceeding OpenSky's 4000 requests/day API limit; the active provider's own safe minimum is enforced automatically either way.</small>"
        "</fieldset>"
        "<fieldset><legend>Aircraft data provider</legend>"
        "<label>Provider <select name='provider'>"
        "<option value='OPENSKY'%s>OpenSky Network</option>"
        "<option value='ADSBLOL'%s>adsb.lol</option>"
        "</select></label>"
        "<small>OpenSky needs the client credentials configured above/below; adsb.lol is a free community API and needs no credentials. Switching providers takes effect on the next poll.</small>"
        "</fieldset>";

    // Second half of the page, sent after the (separately generated) Time
    // zone fieldset. Split so neither snprintf buffer nor this function's
    // stack has to hold the whole page.
    const char htmlFormatB[] =
        "<details><summary>Reduce polling when quiet</summary>"
        "<label>Slow down to a longer interval when fewer than "
        "<input name='low_threshold' type='number' min='0' max='500' step='1' value='%lu'> "
        "aircraft are in range</label>"
        "<label>Slow interval <input name='low_interval' type='number' min='10' max='600' step='1' value='%lu'> seconds</label>"
        "<small>Set the threshold to 0 to disable this.</small>"
        "</details>"
        "<details><summary>Day/night polling schedule</summary>"
        "<label><input name='daynight_enabled' type='checkbox'%s> Enable day/night schedule</label>"
        "<label>Day starts at <input name='day_start' type='number' min='0' max='23' step='1' value='%lu'>:00 local</label>"
        "<label>Day ends at <input name='day_end' type='number' min='0' max='23' step='1' value='%lu'>:00 local</label>"
        "<label>Interval during the day <input name='day_interval' type='number' min='10' max='600' step='1' value='%lu'> seconds</label>"
        "<label>Interval overnight <input name='night_interval' type='number' min='10' max='600' step='1' value='%lu'> seconds</label>"
        "<small>When enabled, this replaces the refresh interval above during those hours, in the time zone (and daylight saving setting) chosen above. Requires the device's clock to be synced over the network, which happens automatically once online; falls back to the day interval until then.</small>"
        "</details>"
        "<details><summary>Day/night brightness</summary>"
        "<label><input name='daynight_brightness_enabled' type='checkbox'%s> Enable day/night brightness</label>"
        "<label>Brightness during the day <input name='day_brightness' type='number' min='1' max='100' step='1' value='%lu'>%%</label>"
        "<label>Brightness overnight <input name='night_brightness' type='number' min='1' max='100' step='1' value='%lu'>%%</label>"
        "<small>Uses the same day/night hours configured above, independently of whether the polling schedule is on. Overrides the manual slider below based on time of day; falls back to the day brightness until the clock has synced.</small>"
        "</details>"
        "<details><summary>Idle dimming</summary>"
        "<label><input name='idle_dim_enabled' type='checkbox'%s> Dim when no aircraft are in range</label>"
        "<label>After <input name='idle_dim_minutes' type='number' min='1' max='1440' step='1' value='%lu'> "
        "minutes with zero aircraft in range</label>"
        "<label>Dim to <input name='idle_dim_percent' type='number' min='0' max='10' step='1' value='%lu'>%%</label>"
        "<small>Overrides the day/night brightness and the manual slider while idle; restores immediately once an aircraft reappears. 0%% turns the backlight fully off.</small>"
        "</details>"
        "<label><input name='auto_closest' type='checkbox'%s> Automatically select closest aircraft</label>"
        "<details><summary>Debugging</summary>"
        "<label><input name='opensky_debug' type='checkbox'%s> Log raw OpenSky fields for the selected aircraft</label>"
        "<small>Dumps every field OpenSky's API returns (by index) for the Selected Craft aircraft to the serial console on each poll. Turn this on to see what's available before wiring a new field into the Selected Craft panel, then turn it back off &mdash; no reflash needed either way.</small>"
        "<label>Provider diagnostics <select name='provider_debug'>"
        "<option value='OFF'%s>Off</option>"
        "<option value='NORMAL'%s>Normal</option>"
        "<option value='VERBOSE'%s>Verbose</option>"
        "<option value='RAW'%s>Raw</option>"
        "</select></label>"
        "<small>Off: nothing logged. Normal: request/response status and aircraft counts for the active provider. Verbose: adds per-aircraft Aircraft Type resolution (provider category, hint, registry override, final result). Raw: adds a bounded, credential-redacted preview of the raw response body. Applies to whichever provider is selected above.</small>"
        "</details>"
        "<button type='submit'>Save settings</button>"
        "</form>"
        "<h2>Display</h2>"
        "<label>Backlight brightness "
        "<input id='brightness' name='brightness' type='range' min='1' max='100' step='1' value='%lu' "
        "oninput=\"document.getElementById('brightnessValue').textContent=this.value;setBrightness(this.value);\">"
        " <span id='brightnessValue'>%lu</span>%%</label>"
        "<small>Applies immediately. Useful to turn down at night so it isn't blinding.</small>"
        "<script>"
        "let brightnessTimer=null;"
        "function setBrightness(value){"
        "if(brightnessTimer)clearTimeout(brightnessTimer);"
        "brightnessTimer=setTimeout(function(){"
        "fetch('/brightness',{"
        "method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'brightness='+value"
        "});"
        "},150);"
        "}"
        "</script>"
        "<h2>OpenSky Credentials</h2>"
        "<p>Select credentials.json</p>"
        "<form method='POST' "
        "action='/upload' "
        "enctype='application/octet-stream'>"
        "<input type='file' "
        "id='fileInput'>"
        "<button type='button' "
        "onclick='uploadFile()'>Upload</button>"
        "</form>"
        "<script>"
        "async function uploadFile(){"
        "const file="
        "document.getElementById('fileInput').files[0];"
        "if(!file){"
        "alert('Select a file');"
        "return;"
        "}"
        "const data=await file.text();"
        "const response=await fetch('/upload',{"
        "method:'POST',"
        "headers:{"
        "'Content-Type':'application/json'"
        "},"
        "body:data"
        "});"
        "const msg=await response.text();"
        "let status=document.getElementById('uploadStatus');"
        "if(!status){"
        "status=document.createElement('p');"
        "status.id='uploadStatus';"
        "document.body.appendChild(status);"
        "}"
        "status.textContent=msg+' Returning to the home page in 30 seconds\\u2026';"
        "setTimeout(function(){location.href='/';},30000);"
        "}"
        "</script>"
        "<form method='POST' action='/delete-credentials' "
        "onsubmit=\"return confirm('Delete stored OpenSky credentials?')\">"
        "<button type='submit'>Delete OpenSky credentials</button>"
        "</form>"
        "</body>"
        "</html>";

    // CPU-only page buffer (snprintf, then httpd_resp_send_chunk copies it
    // into lwIP): PSRAM, so page generation does not take 8 KB of internal
    // DMA-capable RAM from Wi-Fi/TLS. Freed with free() as before.
    char *html = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!html)
    {
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Out of memory");
    }

    char latStr[24];
    char lonStr[24];
    char rangeStr[24];

    FormatFixed(GetRadarLat(), 4, latStr, sizeof(latStr));
    FormatFixed(GetRadarLon(), 4, lonStr, sizeof(lonStr));
    FormatFixed(GetRadarRange(), 0, rangeStr, sizeof(rangeStr));

    // Part A: up to and including the provider fieldset.
    int lengthA = snprintf(
        html,
        8192,
        htmlFormatA,
        latStr,
        lonStr,
        rangeStr,
        (unsigned long)GetRadarRefreshSeconds(),
        (AircraftProvider_GetActive() == AIRCRAFT_PROVIDER_OPENSKY) ? " selected" : "",
        (AircraftProvider_GetActive() == AIRCRAFT_PROVIDER_ADSBLOL) ? " selected" : "");

    httpd_resp_set_type(
        req,
        "text/html");

    esp_err_t err = ESP_FAIL;

    if (lengthA > 0 && lengthA < 8192 &&
        httpd_resp_send_chunk(req, html, lengthA) == ESP_OK &&
        SendTimeZoneFieldset(req) == ESP_OK)
    {
        // Part B: everything after the time zone fieldset.
        int lengthB = snprintf(
            html,
            8192,
            htmlFormatB,
            (unsigned long)GetRadarLowTrafficThreshold(),
            (unsigned long)GetRadarLowTrafficIntervalSeconds(),
            GetRadarDayNightEnabled() ? " checked" : "",
            (unsigned long)GetRadarDayStartHour(),
            (unsigned long)GetRadarDayEndHour(),
            (unsigned long)GetRadarDayIntervalSeconds(),
            (unsigned long)GetRadarNightIntervalSeconds(),
            GetRadarDayNightBrightnessEnabled() ? " checked" : "",
            (unsigned long)GetRadarDayBrightnessPercent(),
            (unsigned long)GetRadarNightBrightnessPercent(),
            GetRadarIdleDimEnabled() ? " checked" : "",
            (unsigned long)GetRadarIdleDimMinutes(),
            (unsigned long)GetRadarIdleDimPercent(),
            Radar_GetAutoSelectClosest() ? " checked" : "",
            GetRadarOpenSkyDebugEnabled() ? " checked" : "",
            (AircraftProvider_GetDebugLevel() == PROVIDER_DEBUG_OFF) ? " selected" : "",
            (AircraftProvider_GetDebugLevel() == PROVIDER_DEBUG_NORMAL) ? " selected" : "",
            (AircraftProvider_GetDebugLevel() == PROVIDER_DEBUG_VERBOSE) ? " selected" : "",
            (AircraftProvider_GetDebugLevel() == PROVIDER_DEBUG_RAW) ? " selected" : "",
            (unsigned long)GetRadarBrightness(),
            (unsigned long)GetRadarBrightness());

        if (lengthB > 0 && lengthB < 8192 &&
            httpd_resp_send_chunk(req, html, lengthB) == ESP_OK)
        {
            err = httpd_resp_send_chunk(req, NULL, 0);
        }
    }

    free(html);

    return err;
}

esp_err_t StartWebServer(void)
{
    if (server_handle != NULL)
        return ESP_OK;

    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    // Default is 4096 bytes, which is tight once request handlers build
    // sizeable HTML responses. Bumped for headroom; this task is otherwise
    // idle most of the time so the extra RAM cost is worth the safety margin.
    // Bumped again after the page grew substantially (day/night brightness,
    // idle dimming, the reorganized/CSS'd layout, more form fields overall)
    // - 8192 was enough for the earlier, smaller version of this page but
    // wasn't anymore, and caused the exact same class of httpd stack
    // overflow this value was originally introduced to fix. Generous
    // headroom this time since this task is idle almost all the time and
    // the board has ~200KB+ of free internal RAM at boot.
    config.stack_size = 16384;
    // Total registered handlers across webserver.c (6), web_rules.c (11),
    // web_airports.c (3), web_seen.c (3) and web_diag.c (1, added for the
    // PHASE 1/13 runtime capacity page) is 24 (23 before /diag; 19 before
    // the Seen Aircraft pages; it was 18, and 16 before that, silently
    // dropping the last handler to register - hence "no slots left" in the
    // log). Set with headroom for future additions rather than the exact
    // current count; each unused slot only costs one small httpd_uri_t-sized
    // entry of heap.
    config.max_uri_handlers = 28;

    if (httpd_start(
            &server_handle,
            &config) != ESP_OK)
    {
        server_handle = NULL;
        return ESP_FAIL;
    }

    // Diagnostic-only heap checkpoint (PROJECT_STATE.md memory investigation),
    // same style/tag as main.c's HeapCheckpoint().
    ESP_LOGW("HEAPCHK",
             "After httpd_start(): internal free=%u largest=%u, PSRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    httpd_uri_t root_uri =
        {
            .uri = "/",
            .method = HTTP_GET,
            .handler = RootHandler,
            .user_ctx = NULL};

    httpd_uri_t upload_uri =
        {
            .uri = "/upload",
            .method = HTTP_POST,
            .handler = UploadHandler,
            .user_ctx = NULL};

    httpd_uri_t wifi_uri =
        {
            .uri = "/wifi",
            .method = HTTP_POST,
            .handler = WifiSetupHandler,
            .user_ctx = NULL};

    httpd_uri_t radar_uri =
        {
            .uri = "/radar",
            .method = HTTP_POST,
            .handler = RadarSetupHandler,
            .user_ctx = NULL};

    httpd_uri_t delete_credentials_uri =
        {
            .uri = "/delete-credentials",
            .method = HTTP_POST,
            .handler = DeleteCredentialsHandler,
            .user_ctx = NULL};

    httpd_uri_t brightness_uri =
        {
            .uri = "/brightness",
            .method = HTTP_POST,
            .handler = BrightnessHandler,
            .user_ctx = NULL};

    // These 6 core routes previously weren't checked for registration
    // failure at all - a silent way for exactly this class of bug (the
    // handler-table capacity issue above) to hide. Aggregated the same
    // way WebRules_Register/WebAirports_Register already do, so a
    // failure here is loud and stops startup cleanly instead of leaving
    // some routes silently missing.
    esp_err_t err = httpd_register_uri_handler(server_handle, &root_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(server_handle, &upload_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(server_handle, &wifi_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(server_handle, &radar_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(server_handle, &delete_credentials_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(server_handle, &brightness_uri);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register core web routes (%s)", esp_err_to_name(err));
        httpd_stop(server_handle);
        server_handle = NULL;
        return ESP_FAIL;
    }

    if (WebRules_Register(server_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register rules routes");
        httpd_stop(server_handle);
        server_handle = NULL;
        return ESP_FAIL;
    }
    if (WebAirports_Register(server_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register airport routes");
        httpd_stop(server_handle);
        server_handle = NULL;
        return ESP_FAIL;
    }
    if (WebSeen_Register(server_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register seen-aircraft routes");
        httpd_stop(server_handle);
        server_handle = NULL;
        return ESP_FAIL;
    }
    if (WebDiag_Register(server_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register diagnostics route");
        httpd_stop(server_handle);
        server_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Web server started");

    return ESP_OK;
}