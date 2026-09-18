#include "webserver.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "cJSON.h"
#include <stdlib.h>

#include "esp_system.h"
#include "main.h"
#include "radar.h"

static const char *TAG = "WEBSERVER";
static httpd_handle_t server_handle = NULL;

#define OPENSKY_NAMESPACE "opensky"
#define WIFI_NAMESPACE "wifi"

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
    esp_restart();
    return ESP_OK;
}

static esp_err_t RadarSetupHandler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 384)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");

    char body[385];
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

    SetRadarSettings(latitude, longitude, range);
    SetRadarRefreshSeconds(refreshSeconds);
    SetRadarLowTrafficThreshold(lowThreshold);
    SetRadarLowTrafficIntervalSeconds(lowInterval);
    SetRadarDayNightSchedule(
        dayNightEnabled,
        utcOffsetMinutes,
        dayStartHour,
        dayEndHour,
        dayIntervalSec,
        nightIntervalSec);
    Radar_SetAutoSelectClosest(strstr(body, "auto_closest=on") != NULL);

    char response[320];
    int len = 0;

    len += snprintf(response + len, sizeof(response) - len,
        "Radar settings saved. ");

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

    if (GetRadarLowTrafficThreshold() > 0)
    {
        snprintf(response + len, sizeof(response) - len,
            "Stretched to %lus when fewer than %lu aircraft are in range.",
            (unsigned long)GetRadarLowTrafficIntervalSeconds(),
            (unsigned long)GetRadarLowTrafficThreshold());
    }
    else
    {
        snprintf(response + len, sizeof(response) - len,
            "Low-traffic slowdown disabled.");
    }

    SendRedirectPage(req, response, 10);
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

    char *buffer = malloc(totalLen + 1);

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

static esp_err_t RootHandler(
    httpd_req_t *req)
{
    const char htmlFormat[] =
        "<!DOCTYPE html>"
        "<html>"
        "<body>"
        "<h2>Flight Radar Setup</h2>"
        "<h3>Wi-Fi</h3>"
        "<form method='POST' action='/wifi'>"
        "<label>Network name <input name='ssid' maxlength='32'></label><br>"
        "<label>Password <input name='password' type='password' maxlength='64'></label><br>"
        "<button type='submit'>Save Wi-Fi</button>"
        "</form>"
        "<h3>Radar location</h3>"
        "<form method='POST' action='/radar'>"
        "<label>Latitude <input name='latitude' type='number' step='any' min='-90' max='90' value='%s'></label><br>"
        "<label>Longitude <input name='longitude' type='number' step='any' min='-180' max='180' value='%s'></label><br>"
        "<label>Range <input name='range' type='number' step='any' min='1' max='5000' value='%s'> km</label><br>"
        "<label>OpenSky refresh interval <input name='refresh' type='number' min='10' max='600' step='1' value='%lu'> seconds</label><br>"
        "<small>10-600 s. Values below 25 s risk exceeding the 4000 requests/day API limit.</small><br>"

        "<h4>Reduce polling when quiet</h4>"
        "<label>Slow down to a longer interval when fewer than "
        "<input name='low_threshold' type='number' min='0' max='500' step='1' value='%lu'> "
        "aircraft are in range</label><br>"
        "<label>Slow interval <input name='low_interval' type='number' min='10' max='600' step='1' value='%lu'> seconds</label><br>"
        "<small>Set the threshold to 0 to disable this.</small><br>"

        "<h4>Day/night schedule (optional)</h4>"
        "<label><input name='daynight_enabled' type='checkbox'%s> Enable day/night schedule</label><br>"
        "<label>UTC offset <input name='utc_offset' type='number' min='-720' max='840' step='1' value='%ld'> minutes (For PDT -7 is -420 min, PST -8 is -480 min)</label><br>"
        "<label>Day starts at <input name='day_start' type='number' min='0' max='23' step='1' value='%lu'>:00 local</label><br>"
        "<label>Day ends at <input name='day_end' type='number' min='0' max='23' step='1' value='%lu'>:00 local</label><br>"
        "<label>Interval during the day <input name='day_interval' type='number' min='10' max='600' step='1' value='%lu'> seconds</label><br>"
        "<label>Interval overnight <input name='night_interval' type='number' min='10' max='600' step='1' value='%lu'> seconds</label><br>"
        "<small>When enabled, this replaces the refresh interval above during those hours. "
        "Requires the device's clock to be synced over the network, which happens "
        "automatically once online; falls back to the day interval until then.</small><br>"

        "<label><input name='auto_closest' type='checkbox'%s> Automatically select closest aircraft</label><br>"
        "<button type='submit'>Save settings</button>"
        "</form>"
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

    char *html = malloc(6144);

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

    snprintf(
        html,
        6144,
        htmlFormat,
        latStr,
        lonStr,
        rangeStr,
        (unsigned long)GetRadarRefreshSeconds(),
        (unsigned long)GetRadarLowTrafficThreshold(),
        (unsigned long)GetRadarLowTrafficIntervalSeconds(),
        GetRadarDayNightEnabled() ? " checked" : "",
        (long)GetRadarUtcOffsetMinutes(),
        (unsigned long)GetRadarDayStartHour(),
        (unsigned long)GetRadarDayEndHour(),
        (unsigned long)GetRadarDayIntervalSeconds(),
        (unsigned long)GetRadarNightIntervalSeconds(),
        Radar_GetAutoSelectClosest() ? " checked" : "");

    httpd_resp_set_type(
        req,
        "text/html");

    esp_err_t err = httpd_resp_send(
        req,
        html,
        HTTPD_RESP_USE_STRLEN);

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
    config.stack_size = 8192;

    if (httpd_start(
            &server_handle,
            &config) != ESP_OK)
    {
        server_handle = NULL;
        return ESP_FAIL;
    }

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

    httpd_register_uri_handler(
    server_handle,
        &root_uri);

    httpd_register_uri_handler(
    server_handle,
        &upload_uri);

    httpd_register_uri_handler(
    server_handle,
        &wifi_uri);

    httpd_register_uri_handler(
    server_handle,
        &radar_uri);

    httpd_register_uri_handler(
    server_handle,
        &delete_credentials_uri);

    ESP_LOGI(
        TAG,
        "Web server started");

    return ESP_OK;
}