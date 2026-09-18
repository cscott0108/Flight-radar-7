#include "waveshare_rgb_lcd_port.h"
#include "ui.h"
#include "driver/gpio.h"

#include "driver/i2c.h"
#include <stdio.h>

#include "bm8563_min.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include <math.h>
#include <time.h>
#include <stdlib.h>

#include "esp_sntp.h"

#include "esp_http_client.h"
#include "opensky_client.h"
#include "webserver.h"
#include "radar.h"

#define MAX_WIFI_NETWORKS 30
#define DEFAULT_SCAN_LIST_SIZE 30

#define WIFI_NAMESPACE "wifi"
#define RADAR_NAMESPACE "radar"

static float radarLat = 13.1993f;
static float radarLon = 77.7067f;
static float radarRangeKm = 100.0f;

// OpenSky poll interval (seconds). 25s keeps the daily request count
// comfortably under the 4000/day anonymous-credential ceiling.
#define RADAR_REFRESH_DEFAULT_SEC 25
#define RADAR_REFRESH_MIN_SEC 10
#define RADAR_REFRESH_MAX_SEC 600

// When fewer than this many aircraft were seen on the last poll, the poll
// interval is stretched out to RADAR_LOW_TRAFFIC_INTERVAL_SEC instead, to
// cut down on API calls during quiet periods. A threshold of 0 disables
// this (the count can never be < 0, so the slow interval never applies).
#define RADAR_LOW_TRAFFIC_THRESHOLD_DEFAULT 10
#define RADAR_LOW_TRAFFIC_THRESHOLD_MIN 0
#define RADAR_LOW_TRAFFIC_THRESHOLD_MAX 500

#define RADAR_LOW_TRAFFIC_INTERVAL_DEFAULT_SEC 60
// Shares the same min/max as the normal refresh interval.

// Optional day/night scheduling: uses a longer interval overnight, when
// commercial air traffic is much lighter, and a shorter one during the
// day. Disabled by default so existing single-interval behavior (above)
// is unaffected unless explicitly turned on from the web UI. Requires
// SNTP to have synced system time; falls back to the day interval if it
// hasn't (see IsSystemTimeValid below).
#define RADAR_DAY_START_HOUR_DEFAULT 6   // 6:00 local
#define RADAR_DAY_END_HOUR_DEFAULT 22    // 22:00 local
#define RADAR_DAY_INTERVAL_DEFAULT_SEC RADAR_REFRESH_DEFAULT_SEC
#define RADAR_NIGHT_INTERVAL_DEFAULT_SEC 90

// How often the Selected Craft panel is repainted from cached data.
#define SELECTED_UI_REFRESH_MS 5000

static volatile uint32_t radarRefreshSec = RADAR_REFRESH_DEFAULT_SEC;
static volatile uint32_t radarLowTrafficThreshold = RADAR_LOW_TRAFFIC_THRESHOLD_DEFAULT;
static volatile uint32_t radarLowTrafficIntervalSec = RADAR_LOW_TRAFFIC_INTERVAL_DEFAULT_SEC;

static volatile bool radarDayNightEnabled = false;
static volatile int32_t radarUtcOffsetMinutes = 0;
static volatile uint32_t radarDayStartHour = RADAR_DAY_START_HOUR_DEFAULT;
static volatile uint32_t radarDayEndHour = RADAR_DAY_END_HOUR_DEFAULT;
static volatile uint32_t radarDayIntervalSec = RADAR_DAY_INTERVAL_DEFAULT_SEC;
static volatile uint32_t radarNightIntervalSec = RADAR_NIGHT_INTERVAL_DEFAULT_SEC;

// #define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_SDA_IO 19
#define I2C_MASTER_SCL_IO 20
// #define I2C_MASTER_FREQ_HZ         100000
#define I2C_MASTER_TX_BUF_DISABLE 0
#define I2C_MASTER_RX_BUF_DISABLE 0

#define DEVICE_ADDR_1 0x30
#define DEVICE_ADDR_2 0x5D

#define LCD_BL_PIN 2

volatile bool wifiConnectedEvent = false;
bool wifiConnectedState = false;
static uint32_t lastApiUpdateMs = 0;

static void radar_sweep_timer_cb(
    lv_timer_t *t)
{
    Radar_SweepTick();
}

typedef struct
{
    char ssid[33];
    int rssi;
} WifiNetwork;

static char selectedSSID[33] = "";
static char savedPassword[65] = "";
static WifiNetwork wifiNetworks[MAX_WIFI_NETWORKS];
static uint16_t wifiNetworkCount = 0;

char bootSSID[33] = {0};
char bootPass[65] = {0};

esp_err_t http_event_handler(
    esp_http_client_event_t *evt)
{
    return ESP_OK;
}

void SaveRadarSettings(
    float lat,
    float lon,
    float rangeKm)
{
    nvs_handle_t handle;

    if (nvs_open(
            RADAR_NAMESPACE,
            NVS_READWRITE,
            &handle) == ESP_OK)
    {
        nvs_set_blob(
            handle,
            "lat",
            &lat,
            sizeof(lat));

        nvs_set_blob(
            handle,
            "lon",
            &lon,
            sizeof(lon));

        nvs_set_blob(
            handle,
            "range",
            &rangeKm,
            sizeof(rangeKm));

        ESP_LOGW(
            "RADAR",
            "Saving %.4f %.4f %.1f",
            lat,
            lon,
            rangeKm);

        esp_err_t err = nvs_commit(handle);

        ESP_LOGW(
            "RADAR",
            "commit=%s",
            esp_err_to_name(err));

        nvs_close(handle);
    }
}

static const char *GetCategoryName(
    int category)
{
    switch (category)
    {
    case 2:
        return "Light";

    case 3:
        return "Small";

    case 4:
        return "Large";

    case 5:
        return "Heavy Vortex";

    case 6:
        return "Heavy";

    case 8:
        return "Rotorcraft";

    case 9:
        return "Glider";

    case 14:
        return "UAV";

    default:
        return "Unknown";
    }
}

void UpdateSelectedAircraftUI(void)
{
    Aircraft *a =
        Radar_GetSelectedAircraft();

    lv_label_set_text_fmt(
        uic_LabelPlaneCount,
        "%d",
        gAircraftCount);

    if (!a)
    {
        // No aircraft in range: blank the panel instead of leaving
        // the last aircraft's details on screen.
        lv_label_set_text(uic_LabelCraftName, "---");
        lv_label_set_text(uic_LabelCraftOrigin, "---");
        lv_label_set_text(uic_LabelCraftSpeed, "---");
        lv_label_set_text(uic_LabelCraftAlt, "---");
        lv_label_set_text(uic_LabelCraftHeading, "---");
        lv_label_set_text(uic_LabelCraftCategory, "---");
        return;
    }

    lv_label_set_text(
        uic_LabelCraftName,
        a->callsign);

    lv_label_set_text(
        uic_LabelCraftOrigin,
        a->originCountry);

    char buf[64];

    snprintf(
        buf,
        sizeof(buf),
        "%.0f km/h",
        a->velocity * 3.6f);

    lv_label_set_text(
        uic_LabelCraftSpeed,
        buf);

    snprintf(
        buf,
        sizeof(buf),
        "%.0f ft",
        a->altitude * 3.28084f);

    lv_label_set_text(
        uic_LabelCraftAlt,
        buf);

    snprintf(
        buf,
        sizeof(buf),
        "%.0f°",
        a->heading);

    lv_label_set_text(
        uic_LabelCraftHeading,
        buf);

    lv_label_set_text(
        uic_LabelCraftCategory,
        GetCategoryName(
            a->category));
}

void setUICoords()
{
    if (lvgl_port_lock(-1))
    {
        UpdateSelectedAircraftUI();

        char buf[64];

        snprintf(
            buf,
            sizeof(buf),
            "%.4f\n%.4f",
            (double)radarLat,
            (double)radarLon);

        lv_label_set_text(
            uic_LabelCoords,
            buf);

        if (showAircraftLabels)
            lv_obj_add_state(ui_Switch3, LV_STATE_CHECKED);
        else
            lv_obj_clear_state(ui_Switch3, LV_STATE_CHECKED);

        ESP_LOGW("RADAR", "Radar settings updated in UI");
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGW("RADAR", "Failed to lock LVGL port for updating radar settings in UI");
    }
}

bool LoadRadarSettings(
    float *lat,
    float *lon,
    float *rangeKm)
{
    nvs_handle_t handle;

    if (nvs_open(
            RADAR_NAMESPACE,
            NVS_READONLY,
            &handle) != ESP_OK)
    {
        ESP_LOGW(
            "RADAR",
            "Radar settings not found, using defaults");
        return false;
    }

    size_t len = sizeof(float);

    esp_err_t e1 =
        nvs_get_blob(
            handle,
            "lat",
            lat,
            &len);

    len = sizeof(float);

    esp_err_t e2 =
        nvs_get_blob(
            handle,
            "lon",
            lon,
            &len);

    len = sizeof(float);

    esp_err_t e3 =
        nvs_get_blob(
            handle,
            "range",
            rangeKm,
            &len);

    ESP_LOGW("RADAR", "Loaded radar settings: lat=%.4f, lon=%.4f, range=%.2f km", *lat, *lon, *rangeKm);

    nvs_close(handle);

    return e1 == ESP_OK &&
           e2 == ESP_OK &&
           e3 == ESP_OK;
}

static uint32_t ClampRefreshSeconds(
    uint32_t seconds)
{
    if (seconds < RADAR_REFRESH_MIN_SEC)
    {
        return RADAR_REFRESH_MIN_SEC;
    }

    if (seconds > RADAR_REFRESH_MAX_SEC)
    {
        return RADAR_REFRESH_MAX_SEC;
    }

    return seconds;
}

static uint32_t ClampLowTrafficThreshold(
    uint32_t count)
{
    if (count > RADAR_LOW_TRAFFIC_THRESHOLD_MAX)
    {
        return RADAR_LOW_TRAFFIC_THRESHOLD_MAX;
    }

    return count; // MIN is 0, so nothing can go below it.
}

static uint32_t ClampHourOfDay(
    uint32_t hour)
{
    return (hour > 23) ? 23 : hour;
}

// Shared helper: NVS get/set for a single u32 setting under RADAR_NAMESPACE.
static bool LoadRadarU32Setting(
    const char *key,
    uint32_t *value)
{
    nvs_handle_t handle;

    if (nvs_open(
            RADAR_NAMESPACE,
            NVS_READONLY,
            &handle) != ESP_OK)
    {
        return false;
    }

    uint32_t stored = 0;

    esp_err_t err =
        nvs_get_u32(
            handle,
            key,
            &stored);

    nvs_close(handle);

    if (err != ESP_OK)
    {
        return false;
    }

    *value = stored;

    return true;
}

static void SaveRadarU32Setting(
    const char *key,
    uint32_t value)
{
    nvs_handle_t handle;

    if (nvs_open(
            RADAR_NAMESPACE,
            NVS_READWRITE,
            &handle) == ESP_OK)
    {
        nvs_set_u32(
            handle,
            key,
            value);

        esp_err_t err = nvs_commit(handle);

        ESP_LOGW(
            "RADAR",
            "Saving %s=%lu commit=%s",
            key,
            (unsigned long)value,
            esp_err_to_name(err));

        nvs_close(handle);
    }
}

uint32_t GetRadarRefreshSeconds(void)
{
    return radarRefreshSec;
}

void SaveRadarRefreshSeconds(
    uint32_t seconds)
{
    SaveRadarU32Setting("refresh", seconds);
}

bool LoadRadarRefreshSeconds(
    uint32_t *seconds)
{
    uint32_t stored = 0;

    if (!LoadRadarU32Setting("refresh", &stored))
    {
        return false;
    }

    *seconds = ClampRefreshSeconds(stored);

    ESP_LOGW(
        "RADAR",
        "Loaded refresh interval: %lus",
        (unsigned long)*seconds);

    return true;
}

void SetRadarRefreshSeconds(
    uint32_t seconds)
{
    radarRefreshSec = ClampRefreshSeconds(seconds);

    SaveRadarRefreshSeconds(radarRefreshSec);
}

uint32_t GetRadarLowTrafficThreshold(void)
{
    return radarLowTrafficThreshold;
}

uint32_t GetRadarLowTrafficIntervalSeconds(void)
{
    return radarLowTrafficIntervalSec;
}

void SetRadarLowTrafficThreshold(
    uint32_t aircraftCount)
{
    radarLowTrafficThreshold = ClampLowTrafficThreshold(aircraftCount);

    SaveRadarU32Setting("lowthresh", radarLowTrafficThreshold);
}

void SetRadarLowTrafficIntervalSeconds(
    uint32_t seconds)
{
    radarLowTrafficIntervalSec = ClampRefreshSeconds(seconds);

    SaveRadarU32Setting("lowint", radarLowTrafficIntervalSec);
}

bool GetRadarDayNightEnabled(void)
{
    return radarDayNightEnabled;
}

int32_t GetRadarUtcOffsetMinutes(void)
{
    return radarUtcOffsetMinutes;
}

uint32_t GetRadarDayStartHour(void)
{
    return radarDayStartHour;
}

uint32_t GetRadarDayEndHour(void)
{
    return radarDayEndHour;
}

uint32_t GetRadarDayIntervalSeconds(void)
{
    return radarDayIntervalSec;
}

uint32_t GetRadarNightIntervalSeconds(void)
{
    return radarNightIntervalSec;
}

// Clamps and applies the day/night schedule to the in-memory state only;
// does not touch NVS. Used both by the public setter (which also
// persists) and by the boot-time loader (which must not rewrite NVS with
// the same values it just read back out of it on every single boot).
static void ApplyRadarDayNightSchedule(
    bool enabled,
    int32_t utcOffsetMinutes,
    uint32_t dayStartHour,
    uint32_t dayEndHour,
    uint32_t dayIntervalSec,
    uint32_t nightIntervalSec)
{
    radarDayNightEnabled = enabled;

    // Offsets run from UTC-12:00 to UTC+14:00 in real timezones.
    if (utcOffsetMinutes < -720)
    {
        utcOffsetMinutes = -720;
    }
    else if (utcOffsetMinutes > 840)
    {
        utcOffsetMinutes = 840;
    }

    radarUtcOffsetMinutes = utcOffsetMinutes;
    radarDayStartHour = ClampHourOfDay(dayStartHour);
    radarDayEndHour = ClampHourOfDay(dayEndHour);
    radarDayIntervalSec = ClampRefreshSeconds(dayIntervalSec);
    radarNightIntervalSec = ClampRefreshSeconds(nightIntervalSec);
}

void SetRadarDayNightSchedule(
    bool enabled,
    int32_t utcOffsetMinutes,
    uint32_t dayStartHour,
    uint32_t dayEndHour,
    uint32_t dayIntervalSec,
    uint32_t nightIntervalSec)
{
    ApplyRadarDayNightSchedule(
        enabled,
        utcOffsetMinutes,
        dayStartHour,
        dayEndHour,
        dayIntervalSec,
        nightIntervalSec);

    SaveRadarU32Setting("dnenabled", radarDayNightEnabled ? 1 : 0);
    SaveRadarU32Setting("utcoff", (uint32_t)(radarUtcOffsetMinutes + 720)); // store as unsigned
    SaveRadarU32Setting("daystart", radarDayStartHour);
    SaveRadarU32Setting("dayend", radarDayEndHour);
    SaveRadarU32Setting("dayint", radarDayIntervalSec);
    SaveRadarU32Setting("nightint", radarNightIntervalSec);
}

// True once SNTP has plausibly synced. Before that, time(NULL) reads back
// close to the epoch, which would otherwise be misread as the dead of
// night on Jan 1 1970.
static bool IsSystemTimeValid(
    time_t now)
{
    return now > 1700000000; // ~Nov 2023; anything before this is unsynced.
}

static bool IsCurrentlyDaytime(
    time_t now)
{
    struct tm utcTm;

    gmtime_r(&now, &utcTm);

    long localMinutesOfDay =
        ((long)utcTm.tm_hour * 60 + utcTm.tm_min + radarUtcOffsetMinutes) % 1440;

    if (localMinutesOfDay < 0)
    {
        localMinutesOfDay += 1440;
    }

    uint32_t localHour = (uint32_t)(localMinutesOfDay / 60);

    if (radarDayStartHour <= radarDayEndHour)
    {
        return (localHour >= radarDayStartHour) && (localHour < radarDayEndHour);
    }

    // Day window wraps past midnight (e.g. start=20, end=6).
    return (localHour >= radarDayStartHour) || (localHour < radarDayEndHour);
}

// Effective poll interval for the CURRENT cycle. Day/night scheduling (if
// enabled) picks the base interval; the low-traffic threshold can then
// stretch that base out further when few aircraft were seen on the last
// poll, but never shortens it.
static uint32_t GetEffectivePollIntervalSeconds(void)
{
    uint32_t baseInterval = radarRefreshSec;

    if (radarDayNightEnabled)
    {
        time_t now = time(NULL);

        if (IsSystemTimeValid(now))
        {
            baseInterval = IsCurrentlyDaytime(now) ?
                radarDayIntervalSec : radarNightIntervalSec;
        }
        else
        {
            // Time not synced yet: default to the more frequent option so
            // no traffic is missed during the startup window.
            baseInterval = radarDayIntervalSec;
        }
    }

    uint32_t threshold = radarLowTrafficThreshold;

    if (threshold > 0 &&
        (uint32_t)gAircraftCount < threshold)
    {
        uint32_t lowInterval = radarLowTrafficIntervalSec;

        return (lowInterval > baseInterval) ? lowInterval : baseInterval;
    }

    return baseInterval;
}

float GetRadarLat(void)
{
    return radarLat;
}

float GetRadarLon(void)
{
    return radarLon;
}

float GetRadarRange(void)
{
    return radarRangeKm;
}

void SetRadarSettings(
    float lat,
    float lon,
    float rangeKm)
{
    radarLat = lat;
    radarLon = lon;
    radarRangeKm = rangeKm;

    SaveRadarSettings(
        radarLat,
        radarLon,
        radarRangeKm);

    Radar_SetCenter(
        radarLat,
        radarLon,
        radarRangeKm);

    setUICoords();
}

void SaveWifiCredentials(
    const char *ssid,
    const char *password)
{
    nvs_handle_t handle;

    if (nvs_open(
            WIFI_NAMESPACE,
            NVS_READWRITE,
            &handle) == ESP_OK)
    {
        nvs_set_str(handle, "ssid", ssid);
        nvs_set_str(handle, "pass", password);

        nvs_commit(handle);
        nvs_close(handle);

        ESP_LOGI("WIFI", "Credentials saved");
    }
}

bool LoadWifiCredentials(
    char *ssid,
    size_t ssidLen,
    char *password,
    size_t passLen)
{
    nvs_handle_t handle;

    if (nvs_open(
            WIFI_NAMESPACE,
            NVS_READONLY,
            &handle) != ESP_OK)
    {
        return false;
    }

    esp_err_t err1 =
        nvs_get_str(
            handle,
            "ssid",
            ssid,
            &ssidLen);

    esp_err_t err2 =
        nvs_get_str(
            handle,
            "pass",
            password,
            &passLen);

    nvs_close(handle);

    return (err1 == ESP_OK &&
            err2 == ESP_OK);
}

void ConnectToWifi(
    const char *ssid,
    const char *password)
{
    wifi_config_t wifi_config = {0};

    strncpy(
        (char *)wifi_config.sta.ssid,
        ssid,
        sizeof(wifi_config.sta.ssid));

    strncpy(
        (char *)wifi_config.sta.password,
        password,
        sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_APSTA));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config));

    ESP_ERROR_CHECK(
        esp_wifi_connect());

    ESP_LOGI(
        "WIFI",
        "Connecting to %s",
        ssid);
}

void InitTime(void)
{
    esp_sntp_setoperatingmode(
        SNTP_OPMODE_POLL);

    esp_sntp_setservername(
        0,
        "pool.ntp.org");

    esp_sntp_init();
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI("WIFI", "STA Started");
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI("WIFI", "Connected");

        wifiConnectedEvent = true;
        wifiConnectedState = true;
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI("WIFI", "Disconnected");
        wifiConnectedEvent = true;
        wifiConnectedState = false;

        esp_wifi_connect();
    }

    if (event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        char ipStr[16];

        snprintf(
            ipStr,
            sizeof(ipStr),
            IPSTR,
            IP2STR(&event->ip_info.ip));

        lv_label_set_text(
            ui_LabelIPData,
            ipStr);

        ESP_LOGW(
            "WIFI",
            "IP: %s",
            ipStr);

        if (OpenSky_HasCredentials())
            lv_obj_add_flag(uic_DialogConfigReq, LV_OBJ_FLAG_HIDDEN);

        InitTime();
    }
}

static void wifi_ssid_btn_cb(
    lv_event_t *e)
{
    WifiNetwork *network =
        (WifiNetwork *)
            lv_event_get_user_data(e);

    strcpy(
        selectedSSID,
        network->ssid);

    lv_label_set_text(
        uic_wifiName,
        selectedSSID);
}

void PopulateWifiList(void)
{
    lv_obj_clean(uic_ContainerSSIDs);

    for (int i = 0; i < wifiNetworkCount; i++)
    {
        lv_obj_t *btn =
            lv_btn_create(uic_ContainerSSIDs);

        lv_obj_set_width(btn, lv_pct(90));
        lv_obj_set_height(btn, 40);

        lv_obj_t *label =
            lv_label_create(btn);

        char text[64];

        snprintf(
            text,
            sizeof(text),
            "%s (%d dBm)",
            wifiNetworks[i].ssid,
            wifiNetworks[i].rssi);

        lv_label_set_text(label, text);

        lv_obj_center(label);

        lv_obj_add_event_cb(
            btn,
            wifi_ssid_btn_cb,
            LV_EVENT_CLICKED,
            &wifiNetworks[i]);
    }
}

void ScanWifiNetworks(void)
{

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            NULL));

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t setup_ap_config = {0};
    strcpy((char *)setup_ap_config.ap.ssid, "Flight-Radar-Setup");
    setup_ap_config.ap.ssid_len = strlen("Flight-Radar-Setup");
    setup_ap_config.ap.channel = 1;
    setup_ap_config.ap.max_connection = 2;
    setup_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &setup_ap_config));

    ESP_ERROR_CHECK(
        esp_wifi_start());

    if (LoadWifiCredentials(
            bootSSID,
            sizeof(bootSSID),
            bootPass,
            sizeof(bootPass)))
    {
        ESP_LOGI(
            "WIFI",
            "Found saved network: %s",
            bootSSID);

        ConnectToWifi(
            bootSSID,
            bootPass);

        lv_scr_load_anim(
            ui_Screen1,
            LV_SCR_LOAD_ANIM_FADE_IN,
            300,
            0,
            false);
        return;
    }
    else
    {
        ESP_LOGI(
            "WIFI",
            "No saved credentials");

        lv_obj_add_flag(
            uic_splash,
            LV_OBJ_FLAG_HIDDEN);

        StartWebServer();
        ESP_LOGI("WIFI", "Setup AP active. Open http://192.168.4.1");
        return;
    }

    wifi_scan_config_t scan_config =
        {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = true,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE};

    ESP_LOGI(TAG, "Starting WiFi scan...");

    ESP_ERROR_CHECK(
        esp_wifi_scan_start(
            &scan_config,
            true));

    uint16_t ap_count =
        DEFAULT_SCAN_LIST_SIZE;

    wifi_ap_record_t *ap_records =
        malloc(
            sizeof(wifi_ap_record_t) *
            DEFAULT_SCAN_LIST_SIZE);

    if (ap_records == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to allocate AP list");

        return;
    }

    ESP_ERROR_CHECK(
        esp_wifi_scan_get_ap_records(
            &ap_count,
            ap_records));

    wifiNetworkCount = 0;

    ESP_LOGI(
        TAG,
        "Found %u APs",
        ap_count);

    for (int i = 0; i < ap_count; i++)
    {
        if (strlen((char *)ap_records[i].ssid) == 0)
            continue;

        if (wifiNetworkCount >= MAX_WIFI_NETWORKS)
            break;

        strncpy(
            wifiNetworks[wifiNetworkCount].ssid,
            (char *)ap_records[i].ssid,
            sizeof(wifiNetworks[wifiNetworkCount].ssid) - 1);

        wifiNetworks[wifiNetworkCount].ssid[32] = '\0';

        wifiNetworks[wifiNetworkCount].rssi =
            ap_records[i].rssi;

        ESP_LOGI(
            TAG,
            "%s (%d dBm)",
            wifiNetworks[wifiNetworkCount].ssid,
            wifiNetworks[wifiNetworkCount].rssi);

        wifiNetworkCount++;
    }

    free(ap_records);

    PopulateWifiList();
}

void wifi_connect_btn_cb(
    lv_event_t *e)
{
    const char *password =
        lv_textarea_get_text(
            uic_wifiPassword);

    strcpy(
        savedPassword,
        password);

    ConnectToWifi(
        selectedSSID,
        savedPassword);
}

// I2C init
void i2c_master_init()
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, conf.mode,
                                       I2C_MASTER_RX_BUF_DISABLE,
                                       I2C_MASTER_TX_BUF_DISABLE, 0));
}

// Scan a specific I2C address to see if there is a response.
bool i2c_scan_address(uint8_t address)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret == ESP_OK;
}

// Write a byte to a certain address
esp_err_t i2c_write_byte(uint8_t device_addr, uint8_t data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (device_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void ui_status_timer_cb(lv_timer_t *t)
{
    if (wifiConnectedEvent)
    {
        wifiConnectedEvent = false;

        if (wifiConnectedState)
        {

            if (lv_scr_act() == ui_Screen2)
            {
                SaveWifiCredentials(
                    selectedSSID,
                    savedPassword);

                lv_scr_load_anim(
                    ui_Screen1,
                    LV_SCR_LOAD_ANIM_FADE_IN,
                    300,
                    500,
                    false);

                lv_label_set_text(
                    uic_wifiStatus,
                    "Connected");

                lv_label_set_text(
                    uic_LabelWifiName,
                    selectedSSID);

                ESP_LOGI(
                    "WIFI",
                    "Connected newly to %s, updating Screen1 WiFi info",
                    selectedSSID);
            }
            else if (lv_scr_act() == ui_Screen1)
            {
                ESP_LOGI(
                    "WIFI",
                    "Already on Screen1, updating BOOT WiFi info");

                lv_label_set_text(
                    uic_LabelWifiName,
                    bootSSID);

                lv_label_set_text(
                    uic_LabelConnection,
                    "Connected");

                lv_obj_set_style_text_color(uic_LabelConnection, lv_color_hex(0x00FF00), 0);
            }

            OpenSky_Init();

            ESP_LOGI(
                "OpenSky",
                "Has creds: %d",
                OpenSky_HasCredentials());

            StartWebServer();
        }
        else
        {
            lv_label_set_text(
                uic_LabelConnection,
                "WiFi failed. Connect to Flight-Radar-Setup" );

            lv_obj_set_style_text_color(
                uic_LabelConnection,
                lv_palette_main(LV_PALETTE_RED),
                LV_PART_MAIN);

            StartWebServer();
            ESP_LOGW(
                "WIFI",
                "Wi-Fi connection failed. Use Flight-Radar-Setup at http://192.168.4.1");
        }
    }
}

static void radar_update_timer_cb(void *pvParameters)
{
    static char json[65536];

    while (1)
    {
        if (wifiConnectedState &&
            OpenSky_HasCredentials())
        {
            float centerLat = radarLat;
            float centerLon = radarLon;
            float radiusKm = radarRangeKm;

            float latDelta =
                radiusKm / 111.0f;

            float lonDelta =
                radiusKm /
                (111.0f *
                 cosf(centerLat *
                      M_PI / 180.0f));

            float minLat =
                centerLat - latDelta;

            float maxLat =
                centerLat + latDelta;

            float minLon =
                centerLon - lonDelta;

            float maxLon =
                centerLon + lonDelta;

            if (OpenSky_GetAircraftJson(
                    minLat,
                    maxLat,
                    minLon,
                    maxLon,
                    json,
                    sizeof(json)))
            {
                OpenSky_ParseAircraft(json);

                lastApiUpdateMs =
                    xTaskGetTickCount() *
                    portTICK_PERIOD_MS;

                if (lvgl_port_lock(0))
                {
                    // Re-index/re-pick the selection against the freshly
                    // parsed gAircraft[] first, then paint it. Painting
                    // before reconciling used a stale index.
                    Radar_ReconcileSelection();
                    UpdateSelectedAircraftUI();

                    Radar_Refresh();
                    lvgl_port_unlock();
                }
            }
        }

        // Sleep in slices so a refresh interval changed from the web UI
        // takes effect without waiting out the previous interval. The
        // interval itself is stretched out when few aircraft were seen,
        // to cut down on API calls during quiet periods.
        uint32_t waitedMs = 0;
        uint32_t effectiveIntervalSec = GetEffectivePollIntervalSeconds();

        while (waitedMs < (effectiveIntervalSec * 1000))
        {
            vTaskDelay(pdMS_TO_TICKS(250));
            waitedMs += 250;
        }
    }
}

static void RadarPredictTask(
    void *pvParameters)
{
    uint32_t selectedUiElapsedMs = 0;

    while (1)
    {
        Radar_PredictAircraft();

        uint32_t now =
            xTaskGetTickCount() *
            portTICK_PERIOD_MS;

        uint32_t ageSec =
            (now - lastApiUpdateMs) / 1000;

        selectedUiElapsedMs += 250;

        bool refreshSelected =
            (selectedUiElapsedMs >= SELECTED_UI_REFRESH_MS);

        if (refreshSelected)
        {
            selectedUiElapsedMs = 0;
        }

        if (lvgl_port_lock(-1))
        {
            if (refreshSelected)
            {
                // Cached-data refresh: no API call, just re-run the
                // selection against gAircraft[] and repaint the panel.
                Radar_ReconcileSelection();
                UpdateSelectedAircraftUI();
            }

            Radar_Refresh();
            lv_label_set_text_fmt(
                uic_LabelAPIRefresh,
                "%lus ago",
                ageSec);
            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void app_main()
{

    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_master_init();
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_reset_pin(LCD_BL_PIN);
    gpio_set_direction(LCD_BL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL_PIN, 1);

    waveshare_esp32_s3_rgb_lcd_init(); // Initialize the Waveshare ESP32-S3 RGB LCD

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(
            nvs_flash_erase());

        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // Lock the mutex due to the LVGL APIs are not thread-safe
    if (lvgl_port_lock(-1))
    {
        ui_init();
        lv_timer_create(
            ui_status_timer_cb,
            500,
            NULL);

        Radar_AttachToObject(
            uic_Imageradar);

        lv_timer_create(
            radar_sweep_timer_cb,
            30,
            NULL);

        Radar_SetCenter(
            12.9716f,
            77.5946f,
            100.0f);

        lvgl_port_unlock();
    }

    if (!LoadRadarSettings(
            &radarLat,
            &radarLon,
            &radarRangeKm))
    {
        radarLat = 13.1993f;
        radarLon = 77.7067f;
        radarRangeKm = 100.0f;
    }

    Radar_SetCenter(
        radarLat,
        radarLon,
        radarRangeKm);

    uint32_t storedRefresh = RADAR_REFRESH_DEFAULT_SEC;

    if (LoadRadarRefreshSeconds(&storedRefresh))
    {
        radarRefreshSec = storedRefresh;
    }
    else
    {
        radarRefreshSec = RADAR_REFRESH_DEFAULT_SEC;
    }

    uint32_t storedLowThreshold = RADAR_LOW_TRAFFIC_THRESHOLD_DEFAULT;
    uint32_t storedLowInterval = RADAR_LOW_TRAFFIC_INTERVAL_DEFAULT_SEC;

    if (LoadRadarU32Setting("lowthresh", &storedLowThreshold))
    {
        radarLowTrafficThreshold = ClampLowTrafficThreshold(storedLowThreshold);
    }
    else
    {
        radarLowTrafficThreshold = RADAR_LOW_TRAFFIC_THRESHOLD_DEFAULT;
    }

    if (LoadRadarU32Setting("lowint", &storedLowInterval))
    {
        radarLowTrafficIntervalSec = ClampRefreshSeconds(storedLowInterval);
    }
    else
    {
        radarLowTrafficIntervalSec = RADAR_LOW_TRAFFIC_INTERVAL_DEFAULT_SEC;
    }

    uint32_t storedDayNightEnabled = 0;
    uint32_t storedUtcOffset = 720; // encoded as offsetMinutes + 720
    uint32_t storedDayStart = RADAR_DAY_START_HOUR_DEFAULT;
    uint32_t storedDayEnd = RADAR_DAY_END_HOUR_DEFAULT;
    uint32_t storedDayInterval = RADAR_DAY_INTERVAL_DEFAULT_SEC;
    uint32_t storedNightInterval = RADAR_NIGHT_INTERVAL_DEFAULT_SEC;

    LoadRadarU32Setting("dnenabled", &storedDayNightEnabled);
    LoadRadarU32Setting("utcoff", &storedUtcOffset);
    LoadRadarU32Setting("daystart", &storedDayStart);
    LoadRadarU32Setting("dayend", &storedDayEnd);
    LoadRadarU32Setting("dayint", &storedDayInterval);
    LoadRadarU32Setting("nightint", &storedNightInterval);

    ApplyRadarDayNightSchedule(
        storedDayNightEnabled != 0,
        (int32_t)storedUtcOffset - 720,
        storedDayStart,
        storedDayEnd,
        storedDayInterval,
        storedNightInterval);
    xTaskCreate(
        radar_update_timer_cb,
        "RadarTask",
        12288,
        NULL,
        5,
        NULL);

    xTaskCreatePinnedToCore(
        RadarPredictTask,
        "RadarPredict",
        4096,
        NULL,
        1,
        NULL,
        0);

    ScanWifiNetworks();

    setUICoords();
}