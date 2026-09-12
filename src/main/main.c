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

// #define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_SDA_IO 15
#define I2C_MASTER_SCL_IO 16
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

    if (!a)
    {
        return;
    }

    lv_label_set_text_fmt(
        uic_LabelPlaneCount,
        "%d",
        gAircraftCount);

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
            WIFI_MODE_STA));

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
        esp_wifi_set_mode(WIFI_MODE_STA));

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

            if (!OpenSky_HasCredentials())
            {
                StartWebServer();
            }
        }
        else
        {
            lv_label_set_text(
                uic_LabelConnection,
                "Disconnected");

            lv_obj_set_style_text_color(
                uic_LabelConnection,
                lv_palette_main(LV_PALETTE_RED),
                LV_PART_MAIN);
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
                    UpdateSelectedAircraftUI();
                    Radar_ReconcileSelection();

                    Radar_Refresh();
                    lvgl_port_unlock();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

static void RadarPredictTask(
    void *pvParameters)
{
    while (1)
    {
        Radar_PredictAircraft();

        uint32_t now =
            xTaskGetTickCount() *
            portTICK_PERIOD_MS;

        uint32_t ageSec =
            (now - lastApiUpdateMs) / 1000;

        if (lvgl_port_lock(-1))
        {
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