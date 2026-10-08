#include "waveshare_rgb_lcd_port.h"
#include "ui.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "driver/i2c_master.h"
#include "i2c_bus.h"
#include <stdio.h>

#include "bm8563_min.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <stdatomic.h>

#include "esp_sntp.h"

#include "esp_http_client.h"
#include "opensky_client.h"
#include "adsblol_client.h"
#include "aircraft_provider.h"
#include "visibility_policy.h" // 0.0.29: boot-time lock/settings init (app_main)
#include "webserver.h"
#include "radar.h"
#include "custom_rules.h"
#include "feature_flags.h"
#include "airports.h"
#include "seen_aircraft.h"
#include "history_manager.h"
#include "wifi_profiles.h"
#include "adv_diag.h"
#include "boot_warmup.h"
#include "diag_telemetry.h"
#include "expert_debug.h"
#include "fr_lv_pool.h"
#include "idle_maint.h"
#include "north_ref.h" // 0.0.32: radar north reference / magnetic variation
#include "ui_prefs.h"  // 0.0.32: accent colors, device clock format
#include "web_style.h"
#include "fw_version.h"
#include "esp_timer.h"

// Build-time A/B switch: flip to 0 and reflash to test whether TF/SD mount
// is contributing to internal-heap fragmentation behind the
// "esp-aes: Failed to allocate memory" / OpenSky TLS failures reported
// after this feature was added, without removing any code. Remove this
// switch once that question is settled (see PROJECT_STATE.md).
#define TF_HISTORY_ENABLED 1
#include "time_util.h"

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
static volatile uint32_t radarDayStartHour = RADAR_DAY_START_HOUR_DEFAULT;
static volatile uint32_t radarDayEndHour = RADAR_DAY_END_HOUR_DEFAULT;
static volatile uint32_t radarDayIntervalSec = RADAR_DAY_INTERVAL_DEFAULT_SEC;
static volatile uint32_t radarNightIntervalSec = RADAR_NIGHT_INTERVAL_DEFAULT_SEC;

static volatile bool radarOpenSkyDebugEnabled = false;

// Backlight brightness, driven as PWM over LEDC rather than a plain
// digital on/off, so it can be dimmed instead of just switched. 1 is
// used as the floor instead of 0 so the "off" end of the slider doesn't
// leave the screen fully black with no obvious way back in from the
// device itself.
#define RADAR_BRIGHTNESS_DEFAULT 100
#define RADAR_BRIGHTNESS_MIN 1
#define RADAR_BRIGHTNESS_MAX 100

#define BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_DUTY_RES LEDC_TIMER_10_BIT // 0-1023
#define BACKLIGHT_LEDC_FREQ_HZ 5000               // Above audible range, no visible flicker.

static volatile uint32_t radarBrightnessPercent = RADAR_BRIGHTNESS_DEFAULT;

// Optional day/night BRIGHTNESS schedule. Reuses the same day window
// (start/end hour + UTC offset) already configured for the day/night
// POLL interval above, rather than asking for a second copy of the same
// three settings, but is enabled/applied independently of it - either
// can be on without the other. When enabled, this overrides whatever the
// manual brightness slider is set to, based on time of day.
#define RADAR_DAY_BRIGHTNESS_DEFAULT 100
#define RADAR_NIGHT_BRIGHTNESS_DEFAULT 25

static volatile bool radarDayNightBrightnessEnabled = false;
static volatile uint32_t radarDayBrightnessPercent = RADAR_DAY_BRIGHTNESS_DEFAULT;
static volatile uint32_t radarNightBrightnessPercent = RADAR_NIGHT_BRIGHTNESS_DEFAULT;

// Optional idle dimming: if no aircraft have been in range for at least
// this many minutes, override whatever brightness would otherwise be
// active (manual or day/night) and drop to a near-off level, since
// there's nothing to look at. Takes priority over the day/night
// schedule - an idle, aircraft-free screen dims regardless of time of
// day - and restores immediately once an aircraft reappears.
#define RADAR_IDLE_DIM_MINUTES_DEFAULT 15
#define RADAR_IDLE_DIM_PERCENT_DEFAULT 1
#define RADAR_IDLE_DIM_MINUTES_MIN 1
#define RADAR_IDLE_DIM_MINUTES_MAX 1440 // 24 hours
#define RADAR_IDLE_DIM_PERCENT_MAX 10   // deliberately capped low - this is a "nearly off" level, not a normal brightness setting

static volatile bool radarIdleDimEnabled = false;
static volatile uint32_t radarIdleDimMinutes = RADAR_IDLE_DIM_MINUTES_DEFAULT;
static volatile uint32_t radarIdleDimPercent = RADAR_IDLE_DIM_PERCENT_DEFAULT;

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
static lv_obj_t *rateLimitUiLabel = NULL;
static lv_obj_t *idleStatusLabel = NULL; // zero-aircraft status, a child of the radar image
/* 0.0.32 bottom status bar: the device clock (centre) and a small version / uptime
 * line left of the settings gear. Both updated from RadarPredictTick, text set
 * only when the shown minute changes. */
static lv_obj_t *clockUiLabel = NULL;
static lv_obj_t *buildUiLabel = NULL;
// Idle-maintenance phase for the display (IdleMaintPhase), written by RadarTask only, read by the UI.
static volatile uint8_t idleMaintPhase = IDLE_MAINT_PHASE_NONE;

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

// SSID of the network the radio is currently trying/using (display only). Written by the task that starts a connection.
static char currentSSID[33] = "";
// Web "Connect" request (slot to join), applied on the LVGL timer so Wi-Fi calls never run on the httpd task.
static volatile int pendingWifiSlot = -1;

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

/* 0.0.30 Selected Craft: the Note / Airline row (created at runtime under the
 * Heading card, styled like it) and the Speed card that can be hidden. */
static lv_obj_t *selInfoCard = NULL, *selInfoTitle = NULL, *selInfoValue = NULL;
#define uic_ContainerCardSpeed ui_ContainerCard6
#define SEL_CATEGORY_DEFAULT_RGB 0x00A4F9 /* the Craft Type value's color in the screen design */

/* HOSTTEST:BEGIN selui (extracted verbatim by host_tests/display_idle_test.c) */
/* ---- 0.0.30 Selected Craft additions (presentation only) ----
 *
 * Altitude trend: from the climb rate the provider reports (Aircraft.verticalRateFpm,
 * AIRCRAFT_DATA_VRATE), never from a single altitude sample and never from
 * altitudes of different providers (OpenSky geometric vs adsb.lol barometric).
 * An arrow appears only after two consecutive provider reports agree (>= 400
 * ft/min up or down) and stays until a report drops below 200 ft/min
 * (hysteresis), so small variations never make it flicker. State is for the
 * selected aircraft only and resets when the selection changes. */
#define SEL_TREND_ON_FPM 400
#define SEL_TREND_OFF_FPM 200

static int SelectedAltitudeTrend(const Aircraft *a)
{
    static char icao[sizeof(a->icao24)];
    static uint32_t seenPollMs;
    static bool havePoll;
    static int shown, pending;
    if (strcmp(icao, a->icao24) != 0)
    {
        snprintf(icao, sizeof(icao), "%s", a->icao24);
        havePoll = false;
        shown = pending = 0;
    }
    if (havePoll && seenPollMs == lastApiUpdateMs)
        return shown; /* no new provider report since the last evaluation */
    havePoll = true;
    seenPollMs = lastApiUpdateMs;

    int dir = 0;
    if (a->dataFlags & AIRCRAFT_DATA_VRATE)
    {
        const int rate = a->verticalRateFpm;
        if (shown > 0 && rate >= SEL_TREND_OFF_FPM)
            dir = 1;
        else if (shown < 0 && rate <= -SEL_TREND_OFF_FPM)
            dir = -1;
        else if (rate >= SEL_TREND_ON_FPM)
            dir = 1;
        else if (rate <= -SEL_TREND_ON_FPM)
            dir = -1;
    }
    if (dir == 0)
    {
        shown = pending = 0; /* level, or the rate is not reported */
    }
    else if (dir == shown)
    {
        pending = dir;
    }
    else if (dir == pending)
    {
        shown = dir; /* second consecutive report in this direction */
    }
    else
    {
        pending = dir; /* first report only: no arrow yet */
        shown = 0;
    }
    return shown;
}

/* The row under Heading: the matched Registered Aircraft rule's Note when it
 * has one, otherwise the airline (configured operator from the call sign, else
 * the operator the provider supplied), otherwise nothing (row hidden). Reuses
 * the registry and operator stores; honours the Registered Aircraft /
 * Operators switches like the radar does. */
static const char *SelectedInfoRow(const Aircraft *a, const CraftResolution *res, char *value, size_t cap)
{
    value[0] = '\0';
    if (res->source == CRAFT_SRC_REGISTRY)
    {
        CustomRule rule;
        if (CustomRules_FindEntry(res->registryIcao24, res->registryPrefix, &rule) && rule.notes[0])
        {
            snprintf(value, cap, "%s", rule.notes);
            return "Note";
        }
    }
    if (Features_OperatorsEnabled())
    {
        char code[MAX_OPERATOR_CODE + 1];
        OperatorInfo op;
        if (Operators_CodeFromCallsign(a->callsign, code) && Operators_Find(code, &op) && op.name[0])
        {
            snprintf(value, cap, "%s", op.name);
            return "Airline";
        }
    }
    if (a->operatorName[0])
    {
        snprintf(value, cap, "%s", a->operatorName);
        return "Airline";
    }
    return NULL;
}

static void ShowObject(lv_obj_t *o, bool show)
{
    if (!o)
        return;
    if (show)
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* Callers hold the LVGL lock. lv_label_set_text re-allocates and invalidates even for identical text,
 * so the count is only rewritten when it changes and the empty panel is painted once per empty period
 * (keyed on the label object, so a recreated screen is always repainted). A real aircraft always
 * repaints, as before. */
void UpdateSelectedAircraftUI(void)
{
    static int shownCount = -1;
    static lv_obj_t *blankPaintedOn = NULL; /* non-NULL: the panel currently shows "---" */
    static lv_obj_t *countPaintedOn = NULL;

    Aircraft *a =
        Radar_GetSelectedAircraft();

    if (gAircraftCount != shownCount || countPaintedOn != uic_LabelPlaneCount)
    {
        lv_label_set_text_fmt(
            uic_LabelPlaneCount,
            "%d",
            gAircraftCount);
        shownCount = gAircraftCount;
        countPaintedOn = uic_LabelPlaneCount;
    }

    // 0.0.30: the Speed row is optional (Setup > Features & appearance); a
    // hidden row collapses in the panel's column layout. Flag changes only
    // invalidate when the state actually changes.
    ShowObject(uic_ContainerCardSpeed, Features_PanelSpeed());

    if (!a)
    {
        // No aircraft in range: blank the panel instead of leaving
        // the last aircraft's details on screen (once per empty period).
        if (blankPaintedOn == uic_LabelCraftName)
            return;
        lv_label_set_text(uic_LabelCraftName, "---");
        lv_label_set_text(uic_LabelCraftOrigin, "---");
        lv_label_set_text(uic_LabelCraftSpeed, "---");
        lv_label_set_text(uic_LabelCraftAlt, "---");
        lv_label_set_text(uic_LabelCraftHeading, "---");
        lv_label_set_text(uic_LabelCraftCategory, "---");
        lv_obj_set_style_text_color(uic_LabelCraftCategory, lv_color_hex(SEL_CATEGORY_DEFAULT_RGB), LV_PART_MAIN);
        ShowObject(selInfoCard, false);
        blankPaintedOn = uic_LabelCraftName;
        return;
    }

    blankPaintedOn = NULL;

    lv_label_set_text(
        uic_LabelCraftName,
        a->callsign);

    lv_label_set_text(
        uic_LabelCraftOrigin,
        a->originCountry);

    char buf[64];

    // Speed: unchanged format; a speed the provider did not send shows the
    // panel's "---" instead of 0.
    if (a->dataFlags & AIRCRAFT_DATA_VELOCITY)
    {
        snprintf(
            buf,
            sizeof(buf),
            "%.0f km/h",
            a->velocity * 3.6f);
        lv_label_set_text(
            uic_LabelCraftSpeed,
            buf);
    }
    else
    {
        lv_label_set_text(uic_LabelCraftSpeed, "---");
    }

    const int trend = Features_PanelAltTrend() ? SelectedAltitudeTrend(a) : 0;
    snprintf(
        buf,
        sizeof(buf),
        "%.0f ft%s",
        a->altitude * 3.28084f,
        trend > 0 ? " " LV_SYMBOL_UP : trend < 0 ? " " LV_SYMBOL_DOWN : "");

    lv_label_set_text(
        uic_LabelCraftAlt,
        buf);

    /* 0.0.32: the track in the radar's resolved reference (T = true, M = magnetic,
     * derived from the raw true track; the provider value is not changed). */
    if (!isfinite(a->trackTrueDeg))
        snprintf(buf, sizeof(buf), "---");
    else
        snprintf(
            buf,
            sizeof(buf),
            "%d°%c",
            (int)lroundf(NorthRef_TrueToDisplay(a->trackTrueDeg)) % 360,
            NorthRef_Resolved() == NORTH_RESOLVED_MAGNETIC ? 'M' : 'T');

    lv_label_set_text(
        uic_LabelCraftHeading,
        buf);

    // One resolution (the same one the radar uses for this aircraft's color):
    // the Craft Type value carries its classification color.
    const CraftResolution res = ResolveAircraft(a->callsign, a->icao24);
    lv_label_set_text(
        uic_LabelCraftCategory,
        CraftType_Name(res.type));
    lv_obj_set_style_text_color(uic_LabelCraftCategory, lv_color_hex(CraftType_ColorRgb(res.type)), LV_PART_MAIN);

    // Note / Airline row under Heading (hidden when neither is known).
    if (selInfoCard)
    {
        char value[MAX_RULE_NOTES + 1];
        const char *title = SelectedInfoRow(a, &res, value, sizeof(value));
        if (title)
        {
            lv_label_set_text(selInfoTitle, title);
            lv_label_set_text(selInfoValue, value);
        }
        ShowObject(selInfoCard, title != NULL);
    }
}
/* HOSTTEST:END selui */

/* 0.0.30: one more card in the Selected Craft column, directly under Heading,
 * built exactly like the Heading card (ui_Screen1.c ContainerCard10): 50 px,
 * card background, 12 px gray title at (10,2), 16 px value centered. The
 * column (ui_PanelRight, flex) has room for one more card below Heading. A
 * long value ends in "..." within the card. Hidden until there is something
 * to show. Called once, with the LVGL lock held, after ui_init(). */
static void CreateSelectedInfoRow(void)
{
    if (!ui_PanelRight || selInfoCard)
        return;
    selInfoCard = lv_obj_create(ui_PanelRight);
    lv_obj_remove_style_all(selInfoCard);
    lv_obj_set_height(selInfoCard, 50);
    lv_obj_set_width(selInfoCard, lv_pct(100));
    lv_obj_clear_flag(selInfoCard, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    ui_object_set_themeable_style_property(selInfoCard, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR,
                                           _ui_theme_color_CardBG);
    ui_object_set_themeable_style_property(selInfoCard, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA,
                                           _ui_theme_alpha_CardBG);

    selInfoTitle = lv_label_create(selInfoCard);
    lv_obj_set_width(selInfoTitle, LV_SIZE_CONTENT);
    lv_obj_set_height(selInfoTitle, LV_SIZE_CONTENT);
    lv_obj_set_x(selInfoTitle, 10);
    lv_obj_set_y(selInfoTitle, 2);
    lv_label_set_text(selInfoTitle, "Airline");
    ui_object_set_themeable_style_property(selInfoTitle, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_COLOR,
                                           _ui_theme_color_Gray);
    ui_object_set_themeable_style_property(selInfoTitle, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_OPA,
                                           _ui_theme_alpha_Gray);
    lv_obj_set_style_text_font(selInfoTitle, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);

    selInfoValue = lv_label_create(selInfoCard);
    lv_obj_set_width(selInfoValue, 176); /* card is 190 px wide: keeps a long note inside it */
    lv_obj_set_height(selInfoValue, LV_SIZE_CONTENT);
    lv_label_set_long_mode(selInfoValue, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(selInfoValue, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_x(selInfoValue, 0);
    lv_obj_set_y(selInfoValue, 5);
    lv_obj_set_align(selInfoValue, LV_ALIGN_CENTER);
    lv_label_set_text(selInfoValue, "");
    lv_obj_set_style_text_color(selInfoValue, lv_color_hex(0x00A4F9), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(selInfoValue, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(selInfoValue, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_flag(selInfoCard, LV_OBJ_FLAG_HIDDEN);
}

/* 0.0.32: device clock (system time, configured zone/DST via time_util, format
 * from Setup) and the version / uptime line. Callers hold the LVGL lock. */
static void UpdateStatusBarUI(void)
{
    static char shownClock[24] = "?";
    static uint32_t shownUpMin = UINT32_MAX;
    if (clockUiLabel) {
        char text[24] = "";
        const UiClockFormat fmt = UiPrefs_ClockFormat();
        TimeLocal tl;
        if (fmt != UI_CLOCK_OFF) {
            if (TimeUtil_ToLocal((int64_t)time(NULL), &tl))
                UiPrefs_FormatClock(fmt, tl.hour, tl.minute, text, sizeof(text));
            else
                snprintf(text, sizeof(text), "--:--");
        }
        if (strcmp(text, shownClock) != 0) {
            snprintf(shownClock, sizeof(shownClock), "%s", text);
            lv_label_set_text(clockUiLabel, text);
            if (text[0])
                lv_obj_clear_flag(clockUiLabel, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(clockUiLabel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (buildUiLabel) {
        const uint32_t upMin = (uint32_t)(esp_timer_get_time() / 60000000LL);
        if (upMin != shownUpMin) {
            shownUpMin = upMin;
            if (upMin < 60)
                lv_label_set_text_fmt(buildUiLabel, "v" FW_VERSION_STRING "\nup %lum", (unsigned long)upMin);
            else if (upMin < 48 * 60)
                lv_label_set_text_fmt(buildUiLabel, "v" FW_VERSION_STRING "\nup %luh %02lum",
                                      (unsigned long)(upMin / 60), (unsigned long)(upMin % 60));
            else
                lv_label_set_text_fmt(buildUiLabel, "v" FW_VERSION_STRING "\nup %lud %luh",
                                      (unsigned long)(upMin / 1440), (unsigned long)((upMin / 60) % 24));
        }
    }
}

/* 0.0.32: Setup display settings saved (rotation / accent changed): redraw the
 * radar once even while it is frozen, and restyle the idle label. */
static void OnAppearanceChanged(void)
{
    if (lvgl_port_lock(-1)) {
        if (idleStatusLabel)
            lv_obj_set_style_text_color(idleStatusLabel, lv_color_hex(UiPrefs_RadarAccentRgb()), LV_PART_MAIN);
        lvgl_port_unlock();
    }
    Radar_RequestRedraw();
}

/* HOSTTEST:BEGIN idleui (extracted verbatim by host_tests/display_idle_test.c) */
// Callers hold the LVGL lock. Static on-radar status for zero-aircraft periods (no timer, no animation):
// the text/colour/visibility are touched only when the selected status changes, so a static radar stays
// static - the label's own area is the only thing LVGL repaints, once per change.
static void UpdateIdleStatusUI(void)
{
    static IdleStatus shown = IDLE_STATUS_NONE; // the label is created hidden

    if (!idleStatusLabel)
        return;

    IdleStatus s = IdleStatus_Select(gAircraftCount, Radar_IsDisplayIdle(), (IdleMaintPhase)idleMaintPhase);
    if (s == shown)
        return;

    if (s == IDLE_STATUS_NONE)
    {
        lv_obj_add_flag(idleStatusLabel, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_label_set_text_static(idleStatusLabel, IdleStatus_Text(s));
        // Warnings in amber (non-alarming), everything else in the radar's own green.
        lv_obj_set_style_text_color(idleStatusLabel,
                                    s == IDLE_STATUS_MAINT_WARN ? lv_color_hex(0xFFB000) : lv_color_hex(UiPrefs_RadarAccentRgb()),
                                    LV_PART_MAIN);
        if (shown == IDLE_STATUS_NONE)
            lv_obj_clear_flag(idleStatusLabel, LV_OBJ_FLAG_HIDDEN);
    }
    shown = s;
}
/* HOSTTEST:END idleui */

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

        // On-screen range label: always the same radarRangeKm the radar
        // filters and draws with (ui_Label29 used to be a fixed placeholder).
        if (ui_Label29)
        {
            char rangeBuf[24];
            snprintf(
                rangeBuf,
                sizeof(rangeBuf),
                "< %.0f km >",
                (double)radarRangeKm);
            lv_label_set_text(
                ui_Label29,
                rangeBuf);
        }

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

// Forward declaration: defined further down (it needs IsCurrentlyDaytime,
// which needs the day-window settings), but the day/night brightness
// setter above needs to call it as soon as the schedule changes.
static void UpdateDayNightBrightness(void);

static uint32_t ClampBrightness(
    uint32_t percent)
{
    if (percent < RADAR_BRIGHTNESS_MIN)
    {
        return RADAR_BRIGHTNESS_MIN;
    }

    if (percent > RADAR_BRIGHTNESS_MAX)
    {
        return RADAR_BRIGHTNESS_MAX;
    }

    return percent;
}

// Unlike the manual slider (which floors at RADAR_BRIGHTNESS_MIN so the
// screen is never accidentally left un-recoverably dark), idle dimming
// is explicitly allowed down to 0% - the whole point is "nothing to see,
// turn it off" - but capped at a low ceiling since this isn't meant to
// be used as a general brightness level.
static uint32_t ClampIdleDimPercent(
    uint32_t percent)
{
    return (percent > RADAR_IDLE_DIM_PERCENT_MAX) ? RADAR_IDLE_DIM_PERCENT_MAX : percent;
}

static uint32_t ClampIdleDimMinutes(
    uint32_t minutes)
{
    if (minutes < RADAR_IDLE_DIM_MINUTES_MIN)
    {
        return RADAR_IDLE_DIM_MINUTES_MIN;
    }

    if (minutes > RADAR_IDLE_DIM_MINUTES_MAX)
    {
        return RADAR_IDLE_DIM_MINUTES_MAX;
    }

    return minutes;
}

// Configures GPIO2 (LCD_BL_PIN) as an LEDC PWM output. Call once, early
// at boot, before anything tries to set a duty cycle.
static void InitBacklightPWM(void)
{
    ledc_timer_config_t timer =
        {
            .speed_mode = BACKLIGHT_LEDC_MODE,
            .timer_num = BACKLIGHT_LEDC_TIMER,
            .duty_resolution = BACKLIGHT_LEDC_DUTY_RES,
            .freq_hz = BACKLIGHT_LEDC_FREQ_HZ,
            .clk_cfg = LEDC_AUTO_CLK,
        };

    ledc_timer_config(&timer);

    ledc_channel_config_t channel =
        {
            .gpio_num = LCD_BL_PIN,
            .speed_mode = BACKLIGHT_LEDC_MODE,
            .channel = BACKLIGHT_LEDC_CHANNEL,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = BACKLIGHT_LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
        };

    ledc_channel_config(&channel);
}

// Applies a brightness percentage (already clamped by the caller) to the
// backlight's PWM duty cycle.
static void ApplyBacklightDuty(
    uint32_t percent)
{
    uint32_t maxDuty = (1u << BACKLIGHT_LEDC_DUTY_RES) - 1;
    uint32_t duty = (percent * maxDuty) / 100;

    ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
}

/* HOSTTEST:BEGIN cfgio (extracted verbatim by host_tests/seen_policy_test.c) */
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

/* HOSTTEST:END cfgio */

// String setting under RADAR_NAMESPACE (used for the IANA time zone id).
static bool LoadRadarStringSetting(
    const char *key,
    char *value,
    size_t valueSize)
{
    nvs_handle_t handle;

    if (nvs_open(RADAR_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return false;
    }

    size_t length = valueSize;
    esp_err_t err = nvs_get_str(handle, key, value, &length);

    nvs_close(handle);

    return err == ESP_OK;
}

static void SaveRadarStringSetting(
    const char *key,
    const char *value)
{
    nvs_handle_t handle;

    if (nvs_open(RADAR_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_set_str(handle, key, value);

        esp_err_t err = nvs_commit(handle);

        ESP_LOGW(
            "RADAR",
            "Saving %s=%s commit=%s",
            key,
            value,
            esp_err_to_name(err));

        nvs_close(handle);
    }
}

// Applies and persists the time zone. Everything time-dependent (day/night
// schedules, Seen Aircraft timestamps, web pages) reads it through time_util,
// so this is the only place the zone is stored.
//   tz_id   IANA zone id, or "CUSTOM" for a fixed offset
//   tz_dst  Automatic DST on/off
//   utcoff  the fixed offset (minutes + 720), unchanged from earlier
//           firmware: it is the custom offset now, and is also what a
//           pre-time-zone install is migrated from (see LoadTimeZoneSettings)
void SetRadarTimeZone(
    const char *zoneId,
    bool autoDst,
    int32_t customOffsetMinutes)
{
    TimeZoneConfig cfg;

    TimeUtil_DefaultConfig(&cfg);
    snprintf(cfg.zoneId, sizeof(cfg.zoneId), "%s", zoneId ? zoneId : TIMEUTIL_DEFAULT_ZONE);
    cfg.autoDst = autoDst;
    cfg.customOffsetMinutes = (int)customOffsetMinutes;

    if (!TimeUtil_SetConfig(&cfg))
    {
        ESP_LOGW("RADAR", "Ignoring invalid time zone %s / offset %ld", cfg.zoneId, (long)customOffsetMinutes);
        return;
    }

    SaveRadarStringSetting("tz_id", cfg.zoneId);
    SaveRadarU32Setting("tz_dst", cfg.autoDst ? 1 : 0);
    SaveRadarU32Setting("utcoff", (uint32_t)(cfg.customOffsetMinutes + 720)); // store as unsigned
}

// Boot-time load. Existing installations only ever stored "utcoff": they
// are migrated to a fixed-offset zone with DST off, which reproduces their
// old day/night behavior exactly. Nothing is written here (no flash wear on
// every boot); the migrated state is persisted when the user next saves the
// setup page. A fresh install gets UTC with Automatic DST on.
static void LoadTimeZoneSettings(void)
{
    TimeZoneConfig cfg;

    TimeUtil_DefaultConfig(&cfg);

    uint32_t storedUtcOffset = 720; // encoded as offsetMinutes + 720
    bool haveUtcOffset = LoadRadarU32Setting("utcoff", &storedUtcOffset);

    if (haveUtcOffset)
    {
        cfg.customOffsetMinutes = (int)storedUtcOffset - 720;
    }

    char storedZone[TIMEUTIL_ZONE_ID_MAX];
    uint32_t storedDst = 1;

    if (LoadRadarStringSetting("tz_id", storedZone, sizeof(storedZone)))
    {
        LoadRadarU32Setting("tz_dst", &storedDst);
        snprintf(cfg.zoneId, sizeof(cfg.zoneId), "%s", storedZone);
        cfg.autoDst = (storedDst != 0);
    }
    else if (haveUtcOffset)
    {
        snprintf(cfg.zoneId, sizeof(cfg.zoneId), "%s", TIMEUTIL_ZONE_CUSTOM);
        cfg.autoDst = false;
    }

    if (!TimeUtil_SetConfig(&cfg))
    {
        // Unknown zone id (for example a table change) or an out-of-range
        // offset: fall back to the safest interpretation of what is stored.
        ESP_LOGW("RADAR", "Stored time zone '%s' is not usable; falling back", cfg.zoneId);

        if (haveUtcOffset)
        {
            snprintf(cfg.zoneId, sizeof(cfg.zoneId), "%s", TIMEUTIL_ZONE_CUSTOM);
            cfg.autoDst = false;
        }
        else
        {
            TimeUtil_DefaultConfig(&cfg);
        }

        if (cfg.customOffsetMinutes < TIMEUTIL_CUSTOM_MIN_MINUTES ||
            cfg.customOffsetMinutes > TIMEUTIL_CUSTOM_MAX_MINUTES)
        {
            cfg.customOffsetMinutes = 0;
        }

        TimeUtil_SetConfig(&cfg);
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

// The fixed offset used when the time zone is "Custom fixed UTC offset".
// The active zone/DST configuration itself lives in time_util (see
// SetRadarTimeZone below); this only exposes its custom-offset field.
int32_t GetRadarUtcOffsetMinutes(void)
{
    TimeZoneConfig tz;

    TimeUtil_GetConfig(&tz);

    return tz.customOffsetMinutes;
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
    uint32_t dayStartHour,
    uint32_t dayEndHour,
    uint32_t dayIntervalSec,
    uint32_t nightIntervalSec)
{
    radarDayNightEnabled = enabled;

    radarDayStartHour = ClampHourOfDay(dayStartHour);
    radarDayEndHour = ClampHourOfDay(dayEndHour);
    radarDayIntervalSec = ClampRefreshSeconds(dayIntervalSec);
    radarNightIntervalSec = ClampRefreshSeconds(nightIntervalSec);
}

void SetRadarDayNightSchedule(
    bool enabled,
    uint32_t dayStartHour,
    uint32_t dayEndHour,
    uint32_t dayIntervalSec,
    uint32_t nightIntervalSec)
{
    ApplyRadarDayNightSchedule(
        enabled,
        dayStartHour,
        dayEndHour,
        dayIntervalSec,
        nightIntervalSec);

    SaveRadarU32Setting("dnenabled", radarDayNightEnabled ? 1 : 0);
    SaveRadarU32Setting("daystart", radarDayStartHour);
    SaveRadarU32Setting("dayend", radarDayEndHour);
    SaveRadarU32Setting("dayint", radarDayIntervalSec);
    SaveRadarU32Setting("nightint", radarNightIntervalSec);
}

/* HOSTTEST:BEGIN seenpol */
void SetSeenEvictionPolicy(int policy)
{
    if (policy < 0 || policy >= SEEN_EVICT_COUNT)
        return;
    SeenAircraft_SetEvictionPolicy((SeenEvictionPolicy)policy);
    SaveRadarU32Setting("seenpol", (uint32_t)policy);
    DiagTelemetry_Event("Seen eviction policy set to %s", SeenEvictionPolicy_Name((SeenEvictionPolicy)policy));
}

static void LoadSeenEvictionPolicy(void)
{
    uint32_t stored = 0;
    if (LoadRadarU32Setting("seenpol", &stored) && stored < SEEN_EVICT_COUNT)
        SeenAircraft_SetEvictionPolicy((SeenEvictionPolicy)stored);
}
/* HOSTTEST:END seenpol */

void SetRadarAutoSelect(bool enabled)
{
    SaveRadarU32Setting("autosel", enabled ? 1 : 0);

    if (lvgl_port_lock(-1))
    {
        Radar_SetAutoSelectClosest(enabled);
        UpdateSelectedAircraftUI();
        Radar_Refresh();
        lvgl_port_unlock();
    }
}

bool GetRadarOpenSkyDebugEnabled(void)
{
    return radarOpenSkyDebugEnabled;
}

void SetRadarOpenSkyDebugEnabled(
    bool enabled)
{
    radarOpenSkyDebugEnabled = enabled;

    SaveRadarU32Setting("openskydbg", enabled ? 1 : 0);
}

uint32_t GetRadarBrightness(void)
{
    return radarBrightnessPercent;
}

void SetRadarBrightness(
    uint32_t percent)
{
    radarBrightnessPercent = ClampBrightness(percent);

    ApplyBacklightDuty(radarBrightnessPercent);

    SaveRadarU32Setting("brightness", radarBrightnessPercent);
}

bool GetRadarDayNightBrightnessEnabled(void)
{
    return radarDayNightBrightnessEnabled;
}

uint32_t GetRadarDayBrightnessPercent(void)
{
    return radarDayBrightnessPercent;
}

uint32_t GetRadarNightBrightnessPercent(void)
{
    return radarNightBrightnessPercent;
}

// Clamps and applies the day/night brightness schedule to the in-memory
// state only; does not touch NVS or the backlight itself. Mirrors the
// Apply/Set split used for the day/night poll schedule above, for the
// same reason: the boot-time loader must not rewrite NVS with the same
// values it just read back out of it.
static void ApplyRadarDayNightBrightness(
    bool enabled,
    uint32_t dayPercent,
    uint32_t nightPercent)
{
    radarDayNightBrightnessEnabled = enabled;
    radarDayBrightnessPercent = ClampBrightness(dayPercent);
    radarNightBrightnessPercent = ClampBrightness(nightPercent);
}

void SetRadarDayNightBrightnessSchedule(
    bool enabled,
    uint32_t dayPercent,
    uint32_t nightPercent)
{
    ApplyRadarDayNightBrightness(enabled, dayPercent, nightPercent);

    SaveRadarU32Setting("brdnenabled", radarDayNightBrightnessEnabled ? 1 : 0);
    SaveRadarU32Setting("daybri", radarDayBrightnessPercent);
    SaveRadarU32Setting("nightbri", radarNightBrightnessPercent);

    // Apply right away rather than waiting for the next periodic check,
    // so toggling this on the web page gives immediate visual feedback.
    UpdateDayNightBrightness();
}

bool GetRadarIdleDimEnabled(void)
{
    return radarIdleDimEnabled;
}

uint32_t GetRadarIdleDimMinutes(void)
{
    return radarIdleDimMinutes;
}

uint32_t GetRadarIdleDimPercent(void)
{
    return radarIdleDimPercent;
}

static void ApplyRadarIdleDimSettings(
    bool enabled,
    uint32_t minutes,
    uint32_t percent)
{
    radarIdleDimEnabled = enabled;
    radarIdleDimMinutes = ClampIdleDimMinutes(minutes);
    radarIdleDimPercent = ClampIdleDimPercent(percent);
}

void SetRadarIdleDimSettings(
    bool enabled,
    uint32_t minutes,
    uint32_t percent)
{
    ApplyRadarIdleDimSettings(enabled, minutes, percent);

    SaveRadarU32Setting("idledimen", radarIdleDimEnabled ? 1 : 0);
    SaveRadarU32Setting("idledimmin", radarIdleDimMinutes);
    SaveRadarU32Setting("idledimpct", radarIdleDimPercent);

    UpdateDayNightBrightness();
}

// True once SNTP has plausibly synced. Before that, time(NULL) reads back
// close to the epoch, which would otherwise be misread as the dead of
// night on Jan 1 1970.
static bool IsSystemTimeValid(
    time_t now)
{
    return TimeUtil_IsSynced((int64_t)now); // ~Nov 2023; anything before this is unsynced.
}

static bool IsCurrentlyDaytime(
    time_t now)
{
    // Local time comes from the central time utility (configured zone +
    // Automatic DST), so the schedule follows the same clock as every web page.
    int localMinutesOfDay = TimeUtil_LocalMinutesOfDay((int64_t)now);

    if (localMinutesOfDay < 0)
    {
        // Not synchronized; callers check IsSystemTimeValid first.
        return true;
    }

    uint32_t localHour = (uint32_t)(localMinutesOfDay / 60);

    if (radarDayStartHour <= radarDayEndHour)
    {
        return (localHour >= radarDayStartHour) && (localHour < radarDayEndHour);
    }

    // Day window wraps past midnight (e.g. start=20, end=6).
    return (localHour >= radarDayStartHour) || (localHour < radarDayEndHour);
}

// Re-applies the backlight duty cycle from the day/night brightness
// schedule, if enabled; otherwise re-applies the manual slider's value
// (so turning the schedule off snaps the backlight back to it instead of
// leaving it stuck at whatever the schedule last set). Does NOT touch
// NVS - this only drives the PWM output directly, so it's safe to call
// frequently (e.g. every few seconds from a background task). A cached
// "last applied" value keeps this from hammering the LEDC registers on
// every call when nothing has actually changed.
// Tracks how long gAircraftCount has been continuously zero, for the
// idle-dim feature below. An explicit flag (rather than overloading 0 as
// a sentinel on the timestamp) since 0 is a legitimately reachable tick
// count - overloading it could make a streak that happens to start at
// tick 0 perpetually re-anchor to "now" and never actually accumulate.
/* HOSTTEST:BEGIN bright (extracted verbatim by host_tests/display_idle_test.c) */
static bool zeroAircraftStreakActive = false;
static uint32_t zeroAircraftSinceMs = 0;

static bool IsIdleDimActive(void)
{
    if (!radarIdleDimEnabled)
    {
        zeroAircraftStreakActive = false;
        return false;
    }

    uint32_t nowMs = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (gAircraftCount == 0)
    {
        if (!zeroAircraftStreakActive)
        {
            zeroAircraftStreakActive = true;
            zeroAircraftSinceMs = nowMs;
        }
    }
    else
    {
        zeroAircraftStreakActive = false;
        return false;
    }

    // Unsigned subtraction wraps correctly even across a tick-count
    // rollover (~49 days), since the actual elapsed idle time here is
    // always far shorter than that.
    uint32_t idleMs = nowMs - zeroAircraftSinceMs;

    return idleMs >= (radarIdleDimMinutes * 60000UL);
}

// Applies whichever brightness source currently has priority:
//   1. Idle dimming (0 aircraft for the configured number of minutes) -
//      overrides everything else, since there's nothing to look at.
//   2. Day/night schedule, if enabled.
//   3. The manual slider, as the fallback / normal case.
static void UpdateDayNightBrightness(void)
{
    uint32_t desiredPercent;

    bool idleDim = IsIdleDimActive();

    // Tell the radar renderer; it freezes only while this is true AND there
    // are zero aircraft (radar.c). Plain flag store, no LVGL call.
    Radar_SetIdleDimActive(idleDim);

    if (idleDim)
    {
        desiredPercent = radarIdleDimPercent;
    }
    else if (radarDayNightBrightnessEnabled)
    {
        time_t now = time(NULL);
        bool daytime;

        if (IsSystemTimeValid(now))
        {
            daytime = IsCurrentlyDaytime(now);
        }
        else
        {
            // Time not synced yet: default to the day brightness so the
            // screen isn't unexpectedly dim during the startup window.
            daytime = true;
        }

        desiredPercent = daytime ?
            radarDayBrightnessPercent : radarNightBrightnessPercent;
    }
    else
    {
        // Schedule is off: track the manual slider instead, so switching
        // the schedule off snaps back to it rather than leaving the
        // backlight stuck at whatever the schedule last applied.
        desiredPercent = radarBrightnessPercent;
    }

    static uint32_t lastAppliedPercent = UINT32_MAX;

    if (desiredPercent != lastAppliedPercent)
    {
        ApplyBacklightDuty(desiredPercent);
        lastAppliedPercent = desiredPercent;
    }
}
/* HOSTTEST:END bright */

/* HOSTTEST:BEGIN provint (extracted verbatim by host_tests/provider_sched_test.c) */
// Effective poll interval of one provider for the CURRENT cycle (0.0.28).
// Each provider has its own configured interval. The day/night schedule and
// the quiet-traffic slowdown are slowdown policies: they can only lengthen it,
// never shorten it, and never change the stored provider intervals:
//   effective = MAX(provider interval, schedule interval, quiet-traffic interval)
// e.g. OpenSky 20 s / adsb.lol 5 s with a 10 s schedule -> 20 s / 10 s.
static uint32_t GetEffectivePollIntervalSecondsFor(AircraftProviderType provider)
{
    uint32_t interval = AircraftProvider_GetIntervalSeconds(provider);
    uint32_t providerMin = AircraftProvider_MinIntervalSecondsFor(provider);
    if (interval < providerMin)
        interval = providerMin;

    if (radarDayNightEnabled)
    {
        time_t now = time(NULL);
        // Time not synced yet: the day interval (the more frequent option, so
        // no traffic is missed during the startup window).
        uint32_t scheduled = (IsSystemTimeValid(now) && !IsCurrentlyDaytime(now)) ?
            radarNightIntervalSec : radarDayIntervalSec;
        if (scheduled > interval)
            interval = scheduled;
    }

    uint32_t threshold = radarLowTrafficThreshold;
    if (threshold > 0 &&
        (uint32_t)gAircraftCount < threshold &&
        radarLowTrafficIntervalSec > interval)
    {
        interval = radarLowTrafficIntervalSec;
    }

    return interval;
}
/* HOSTTEST:END provint */

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

    // 0.0.32: the old location's magnetic variation stops applying at once;
    // the new one is computed locally (or reused from the cache) before the
    // radar redraws with the new centre.
    NorthRef_OnLocationChanged(radarLat, radarLon);

    Radar_SetCenter(
        radarLat,
        radarLon,
        radarRangeKm);

    setUICoords();
}

/* HOSTTEST:BEGIN wificonn (extracted verbatim by host_tests/wifi_retry_test.c) */
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

    strlcpy(currentSSID, ssid, sizeof(currentSSID));

    ESP_LOGI(
        "WIFI",
        "Connecting to %s",
        ssid);
}
/* HOSTTEST:END wificonn */

/* HOSTTEST:BEGIN wifisw (extracted verbatim by host_tests/wifi_retry_test.c) */
static void WifiRetryCancel(void);

// Non-fatal variant used for profile failover and web "Connect": a bad profile must never abort().
// Radio calls are best-effort; the disconnect handler keeps retrying / rotating.
static bool WifiSwitchToSlot(int slot)
{
    char ssid[WIFI_SSID_MAX + 1];
    char pass[WIFI_PASS_MAX + 1];
    if (!WifiProfiles_Get(slot, ssid, sizeof(ssid), pass, sizeof(pass)))
        return false;

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    memset(pass, 0, sizeof(pass));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK)
    {
        ESP_LOGW("WIFI", "set_config for slot %d failed: %s", slot, esp_err_to_name(err));
        return false;
    }
    strlcpy(currentSSID, ssid, sizeof(currentSSID));
    ESP_LOGI("WIFI", "Switching to saved network %d (%s)", slot + 1, ssid);
    (void)esp_wifi_connect();
    return true;
}

void WifiRequestConnect(int slot)
{
    pendingWifiSlot = slot;
}

bool WifiGetStatus(char *ssid, size_t cap, int *slotOut)
{
    if (ssid && cap)
        strlcpy(ssid, currentSSID, cap);
    if (slotOut)
        *slotOut = wifiConnectedState ? WifiProfiles_TrySlot() : -1;
    return wifiConnectedState;
}

// Runs on the LVGL timer (ui_status_timer_cb).
static void ApplyPendingWifiSwitch(void)
{
    int slot = pendingWifiSlot;
    if (slot < 0)
        return;
    pendingWifiSlot = -1;
    if (!WifiProfiles_SlotUsed(slot))
        return;
    WifiProfiles_SetTrySlot(slot);
    // New credentials first, then drop the current association; the disconnect handler (or the
    // explicit connect below) joins with the new config.
    char ssid[WIFI_SSID_MAX + 1];
    char pass[WIFI_PASS_MAX + 1];
    if (!WifiProfiles_Get(slot, ssid, sizeof(ssid), pass, sizeof(pass)))
        return;
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    memset(pass, 0, sizeof(pass));
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK)
    {
        strlcpy(currentSSID, ssid, sizeof(currentSSID));
        ESP_LOGI("WIFI", "Joining saved network %d (%s)", slot + 1, ssid);
        WifiRetryCancel(); // an explicit join replaces any deferred retry
        (void)esp_wifi_disconnect();
        (void)esp_wifi_connect();
    }
    memset(&cfg, 0, sizeof(cfg));
}
/* HOSTTEST:END wifisw */

void InitTime(void)
{
    AdvDiag_HeapCheckpoint("CP1 before esp_sntp_init");
    esp_sntp_setoperatingmode(
        SNTP_OPMODE_POLL);

    esp_sntp_setservername(
        0,
        "pool.ntp.org");

    esp_sntp_init();

    AdvDiag_HeapCheckpoint("CP2 after esp_sntp_init");
}

/* HOSTTEST:BEGIN wifiretry (extracted verbatim by host_tests/wifi_retry_test.c) */
// STA retry backoff. The setup AP shares the radio with the STA (APSTA, AP fixed on channel 1), and a
// STA retrying an unavailable SSID with no delay scans every channel back to back, so the AP almost
// never beacons. After WIFI_FAILS_BEFORE_SWITCH failures with no other usable profile the retry is
// deferred by one-shot esp_timer (no task, no blocking in the event handler). At most one is pending.
#define WIFI_STA_BACKOFF_US (30LL * 1000000LL)
static esp_timer_handle_t wifiRetryTimer;
static atomic_bool wifiRetryPending;

// Invalidates a pending retry (STA connected, or an explicit join replaced it). Safe from any task:
// whoever clears the pending flag first wins, so the timer callback either runs once or not at all.
static void WifiRetryCancel(void)
{
    if (atomic_exchange(&wifiRetryPending, false) && wifiRetryTimer)
        (void)esp_timer_stop(wifiRetryTimer);
}

static void WifiRetryTimerCb(void *arg)
{
    (void)arg;
    if (!atomic_exchange(&wifiRetryPending, false))
        return; // cancelled
    if (wifiConnectedState)
        return; // already connected: nothing to retry
    WifiProfiles_OnBackoffRetry();
    ESP_LOGI("WIFI", "WiFi STA backoff finished; retrying saved network");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
        ESP_LOGW("WIFI", "WiFi STA retry: esp_wifi_connect failed: %s", esp_err_to_name(err));
}

static void WifiRetryInit(void)
{
    const esp_timer_create_args_t args = {.callback = WifiRetryTimerCb, .name = "wifi_retry"};
    esp_err_t err = esp_timer_create(&args, &wifiRetryTimer);
    if (err != ESP_OK)
    {
        wifiRetryTimer = NULL; // retries then fall back to the previous immediate behaviour
        ESP_LOGW("WIFI", "WiFi STA retry timer unavailable: %s", esp_err_to_name(err));
    }
}

static void WifiRetryBackoff(void)
{
    if (atomic_exchange(&wifiRetryPending, true))
        return; // one deferred retry at most
    ESP_LOGW("WIFI", "WiFi STA profile exhausted retries; retrying in %d seconds", (int)(WIFI_STA_BACKOFF_US / 1000000));
    if (!wifiRetryTimer || esp_timer_start_once(wifiRetryTimer, WIFI_STA_BACKOFF_US) != ESP_OK)
    {
        atomic_store(&wifiRetryPending, false);
        ESP_LOGW("WIFI", "WiFi STA backoff could not be scheduled; retrying now");
        (void)esp_wifi_connect();
    }
}

// Event-loop task, on every STA_DISCONNECTED.
static void WifiStaDisconnectedPolicy(void)
{
    if (atomic_load(&wifiRetryPending))
        return; // a deferred retry is already scheduled; this event is not a new attempt
    int next = -1;
    switch (WifiProfiles_OnDisconnectAction(&next))
    {
    case WIFI_RETRY_SWITCH:
        if (!WifiSwitchToSlot(next))
            (void)esp_wifi_connect();
        break;
    case WIFI_RETRY_BACKOFF:
        WifiRetryBackoff();
        break;
    default:
        (void)esp_wifi_connect();
        break;
    }
}
/* HOSTTEST:END wifiretry */

static void LogApStarted(void)
{
    static wifi_config_t apCfg; // static: the event task stack is small
    if (esp_wifi_get_config(WIFI_IF_AP, &apCfg) == ESP_OK)
        ESP_LOGI("WIFI", "WiFi AP started: %.*s (channel %u)", (int)apCfg.ap.ssid_len, (const char *)apCfg.ap.ssid, (unsigned)apCfg.ap.channel);
    else
        ESP_LOGI("WIFI", "WiFi AP started");
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip;
    if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK)
        ESP_LOGI("WIFI", "WiFi AP IP: " IPSTR, IP2STR(&ip.ip));
    else
        ESP_LOGW("WIFI", "WiFi AP IP unavailable");
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

        WifiRetryCancel();
        WifiProfiles_ResetFailures();
        wifiConnectedEvent = true;
        wifiConnectedState = true;
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGI("WIFI", "Disconnected (reason %d)", d ? (int)d->reason : -1);
        wifiConnectedEvent = true;
        wifiConnectedState = false;

        // After WIFI_FAILS_BEFORE_SWITCH consecutive failures move on to the next saved network
        // (RAM-only decision); with no other usable profile the retry is deferred (30 s, one-shot
        // timer) so the setup AP keeps its beacons; below the threshold retry the same one at once.
        WifiStaDisconnectedPolicy();
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_AP_START)
    {
        LogApStarted();
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_AP_STOP)
    {
        ESP_LOGW("WIFI", "WiFi AP stopped");
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        const wifi_event_ap_staconnected_t *c = (const wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI("WIFI", "WiFi AP client connected (aid %d)", c ? (int)c->aid : -1);
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        const wifi_event_ap_stadisconnected_t *c = (const wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI("WIFI", "WiFi AP client disconnected (aid %d)", c ? (int)c->aid : -1);
    }

    if (event_base == IP_EVENT &&
        event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT)
    {
        ESP_LOGI("WIFI", "WiFi AP client received an IP address");
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

        if (AircraftProvider_HasCredentials())
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

    WifiRetryInit();

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_ASSIGNED_IP_TO_CLIENT,
            &wifi_event_handler,
            NULL,
            NULL));

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

    WifiProfiles_Init();
    int firstSlot = WifiProfiles_FirstSlotToTry();
    char bootPass[WIFI_PASS_MAX + 1];

    if (firstSlot >= 0 &&
        WifiProfiles_Get(firstSlot, bootSSID, sizeof(bootSSID), bootPass, sizeof(bootPass)))
    {
        ESP_LOGI(
            "WIFI",
            "Found saved network %d: %s",
            firstSlot + 1,
            bootSSID);

        WifiProfiles_SetTrySlot(firstSlot);
        ConnectToWifi(
            bootSSID,
            bootPass);
        memset(bootPass, 0, sizeof(bootPass));

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

    strlcpy(
        savedPassword,
        password,
        sizeof(savedPassword));

    WifiProfiles_SetTrySlot(-1); // manual: never rotates away until it has connected and been saved
    ConnectToWifi(
        selectedSSID,
        savedPassword);
}

// I2C init
i2c_master_bus_handle_t gI2CBus = NULL;

// Creates the single shared I2C bus for this board (GPIO19/20) using the
// new driver/i2c_master.h API. Every I2C device (BM8563 RTC, GT911 touch)
// attaches to this same bus handle as a device - see i2c_bus.h for why
// this can't be split across the old and new drivers on ESP-IDF v6+.
void i2c_master_init()
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &gI2CBus));

    ESP_ERROR_CHECK(bm8563_i2c_attach(gI2CBus));
}

static void ui_status_timer_cb(lv_timer_t *t)
{
    ApplyPendingWifiSwitch();

    if (wifiConnectedEvent)
    {
        wifiConnectedEvent = false;

        if (wifiConnectedState)
        {

            if (lv_scr_act() == ui_Screen2)
            {
                int savedSlot = -1;
                WifiProfileResult saveResult =
                    WifiProfiles_Upsert(selectedSSID, savedPassword, &savedSlot);
                memset(savedPassword, 0, sizeof(savedPassword));
                if (saveResult == WP_OK)
                    WifiProfiles_SetTrySlot(savedSlot);
                else
                    ESP_LOGW("WIFI", "Connected, but could not save the network: %s",
                             WifiProfiles_ResultText(saveResult));

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
                    currentSSID);

                lv_label_set_text(
                    uic_LabelConnection,
                    "Connected");

                lv_obj_set_style_text_color(uic_LabelConnection, lv_color_hex(0x00FF00), 0);
            }

            WifiProfiles_OnConnected(); // remembers the last good slot (written only when it changed)

            ESP_LOGI(
                "AircraftProvider",
                "Active provider: %s, has creds: %d",
                AircraftProviderType_Name(AircraftProvider_GetActive()),
                AircraftProvider_HasCredentials());

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

// PHASE 1.2/13 capacity diagnostics: cheap high-water mark, updated in the
// one place gAircraftCount already changes. No polling, no separate
// tracking task - see web_diag.c for where this is read.
static int maxAircraftCountSinceBoot = 0;

// Zero-aircraft idle maintenance (idle_maint.h): RadarTask only. Zero-initialized = HOLDOFF.
static IdleMaint idleMaint;

// Local calendar date (YYYYMMDD) from the central time layer, or 0 while SNTP has not synchronized.
static int32_t LocalDayKeyNow(void)
{
    int64_t now = (int64_t)time(NULL);
    TimeLocal local;
    if (!TimeUtil_IsSynced(now) || !TimeUtil_ToLocal(now, &local))
        return 0;
    return (int32_t)(local.year * 10000 + local.month * 100 + local.day);
}

int GetMaxAircraftCountSinceBoot(void)
{
    return maxAircraftCountSinceBoot;
}

// Everything that follows a successful poll of any provider: Seen, History,
// idle maintenance and the display. Runs once per successful poll on the one
// (merged) aircraft list, so no subsystem sees a provider-specific list.
static void OnSuccessfulPoll(AircraftProviderType provider)
{
    ESP_LOGI("AircraftProvider", "%s poll: %d aircraft in the list", AircraftProviderType_Name(provider), gAircraftCount);
    if (gAircraftCount > maxAircraftCountSinceBoot)
        maxAircraftCountSinceBoot = gAircraftCount;
    lastApiUpdateMs = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Provider-independent history: RAM only here; the CSV is
    // written in batches by SeenAircraft_FlushIfDue below.
    // Seen Logging switch (WebUI Setup > Features). OFF skips only
    // new Seen records; existing records stay and tracking continues.
    if (Features_SeenEnabled())
        SeenAircraft_ObservePoll(gAircraft, gAircraftCount, (int64_t)time(NULL));

    // Persistent (TF) history: one classification resolve per
    // aircraft per poll (the radar/preview paths already re-resolve
    // live and uncached every sweep tick - see PROJECT_STATE.md - so
    // one more resolve here, at poll cadence rather than 30 ms sweep
    // cadence, is negligible). RAM-only unless this is an aircraft
    // first sighting this session; TF writes are coalesced separately
    // by HistoryManager_FlushIfDue below.
#if TF_HISTORY_ENABLED
    {
        int64_t nowUtc = (int64_t)time(NULL);
        for (int hi = 0; hi < gAircraftCount; hi++) {
            // Stale entries (visibility retention, no longer reported)
            // are not sightings: never let them extend History.
            if (!gAircraft[hi].valid || gAircraft[hi].visState == AIRCRAFT_VIS_STALE)
                continue;
            CraftResolution hres = ResolveAircraftWithHint(
                gAircraft[hi].callsign,
                gAircraft[hi].icao24,
                gAircraft[hi].providerTypeHint,
                gAircraft[hi].hasProviderTypeHint);
            HistoryManager_Observe(gAircraft[hi].icao24, gAircraft[hi].callsign, hres, nowUtc);
            // 0.0.27: registration from the provider (adsb.lol "r") into the
            // existing, previously unused TF registry field.
            HistoryManager_ObserveRegistration(gAircraft[hi].icao24, gAircraft[hi].registration);
        }
    }
#endif

    // Successful poll only (a failed/malformed response never gets
    // here): after the 300 s boot holdoff, two consecutive zero polls
    // and at most once per local calendar day, one bounded Seen +
    // History drain per zero period. The History request is served
    // below by HistoryManager_FlushIfDue; the result is collected by
    // IdleMaint_PollResult in the same slice loop.
    bool startDrain = IdleMaint_OnSuccessfulPoll(&idleMaint, esp_timer_get_time(),
                                                 gAircraftCount, LocalDayKeyNow());
    idleMaintPhase = (uint8_t)IdleMaint_Phase(&idleMaint);

    // Aircraft back: selection, panel, a forced full radar redraw and
    // the cleared status all happen here, at once (not on the next
    // sweep tick or backlight pass). Zero + maintenance starting: the
    // status shows "performing maintenance" before the flush runs.
    if (lvgl_port_lock(0))
    {
        Radar_ReconcileSelection();
        UpdateSelectedAircraftUI();
        Radar_Refresh();
        UpdateIdleStatusUI();
        lvgl_port_unlock();
    }

    if (startDrain)
    {
        IdleMaint_Drain(&idleMaint, TF_HISTORY_ENABLED != 0, esp_timer_get_time());
        idleMaintPhase = (uint8_t)IdleMaint_Phase(&idleMaint);
        // 0.0.32: the same zero-aircraft window refreshes a stale (>1 year)
        // magnetic variation. Local WMM computation, no network request.
        NorthRef_Service(true);
    }
}

/* HOSTTEST:BEGIN provsched (extracted verbatim by host_tests/provider_sched_test.c) */
// 0.0.28: independent provider scheduling in this one task (no new task, no
// concurrent TLS sessions: the OpenSky TLS memory fix switches a global
// allocation setting during its request). Each enabled provider is polled
// when its own effective interval has elapsed since its previous poll ENDED
// (as before 0.0.28: the interval is the pause between polls, so a provider's
// request rate - e.g. OpenSky's daily budget - is unchanged), independently of
// the other provider, so a 5 s provider keeps its cadence next to a 20 s one. Requests are
// sequential: a slow or hanging request (HTTP timeout 30 s) can delay the
// other provider's next poll by at most that long, never stop it. A failed
// poll of one provider (HTTP, TLS, malformed data, rate limit) only affects
// that provider; the other keeps polling and its list keeps the radar current.
static void PollProviderIfDue(AircraftProviderType provider, uint32_t nowMs)
{
    static bool everPolled[AIRCRAFT_PROVIDER_COUNT];
    static uint32_t lastEndMs[AIRCRAFT_PROVIDER_COUNT];
    static bool wasEnabled[AIRCRAFT_PROVIDER_COUNT];

    const bool enabled = AircraftProvider_IsEnabled(provider);
    if (!enabled)
    {
        wasEnabled[provider] = false;
        return;
    }
    if (!wasEnabled[provider])
    {
        wasEnabled[provider] = true;
        everPolled[provider] = false; // newly enabled: poll at once
    }

    const uint32_t intervalMs = GetEffectivePollIntervalSecondsFor(provider) * 1000u;
    // Another provider's list is merged in while it is at most two of its own
    // intervals old; older means that provider is failing, and its aircraft
    // leave the radar instead of freezing there.
    AircraftProvider_SetMergeMaxAgeMs(provider, 2u * intervalMs);

    if (everPolled[provider] && (uint32_t)(nowMs - lastEndMs[provider]) < intervalMs)
        return;
    if (!wifiConnectedState ||
        !AircraftProvider_HasCredentialsFor(provider) ||
        AircraftProvider_GetRateLimitSecondsFor(provider) != 0)
        return; // checked again on the next 250 ms slice

    everPolled[provider] = true;
    lastEndMs[provider] = nowMs;

    // One-time, in RadarTask's own context (this function runs in the
    // RadarTask task body - see xTaskCreate in app_main), before its
    // first network call: create this task's lwIP per-thread
    // semaphore now, while nothing transient sits in front of it,
    // instead of inside the first OAuth request. See boot_warmup.h.
    // wifiConnectedState implies esp_netif_init() (lwIP) has run.
    static bool firstFetchDone = false;
    if (!firstFetchDone)
    {
        firstFetchDone = true;
        BootWarmup_NetCurrentTask();
        AdvDiag_HeapCheckpoint("CP3 RadarTask first fetch, before GetAircraftJson");
    }

    const char *json = NULL;
    if (AircraftProvider_GetAircraftJsonFor(provider, radarLat, radarLon, radarRangeKm, &json))
    {
        if (AircraftProvider_ParseAircraftFor(provider, json))
            OnSuccessfulPoll(provider);
        else
            ESP_LOGE("AircraftProvider", "%s aircraft JSON parse failed", AircraftProviderType_Name(provider));
    }
    lastEndMs[provider] = xTaskGetTickCount() * portTICK_PERIOD_MS;
}
/* HOSTTEST:END provsched */

static void radar_update_timer_cb(void *pvParameters)
{
    while (1)
    {
        for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++)
            PollProviderIfDue((AircraftProviderType)p, xTaskGetTickCount() * portTICK_PERIOD_MS);

        // One 250 ms slice; interval changes from the web UI take effect on
        // the next slice.
        vTaskDelay(pdMS_TO_TICKS(250));

        // Min-ever heap/stack samples (stamped when a lower value first appears), the
        // 5-minute clock marker and the SNTP-sync announcement (events are silent unless
        // Advanced/Expert logging is on). Fixed-size, no allocation.
        DiagTelemetry_Tick();
        DiagTelemetry_SampleThisTaskStack("RadarTask");

        // Cheap check: writes flash at most once per
        // SEEN_FLUSH_INTERVAL_SEC, and only if something changed.
        SeenAircraft_FlushIfDue((xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000);

        // Same coalescing pattern for TF-backed persistent history -
        // cheap, writes at most once per HM_SYNC_INTERVAL_SEC, and only
        // the aircraft that actually changed (history_manager.c). A
        // missing/unavailable TF card makes this a no-op every time.
#if TF_HISTORY_ENABLED
        HistoryManager_FlushIfDue((xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000);
#endif

        // 0.0.32: flag tests only, except once after the clock first syncs when
        // the magnetic variation was computed with an estimated date.
        NorthRef_Service(false);

        // Completes a pending idle-maintenance attempt from its real History
        // result (served just above). Only a flag test when nothing is pending.
        if (idleMaint.resultPending && IdleMaint_PollResult(&idleMaint, esp_timer_get_time()))
            idleMaintPhase = (uint8_t)IdleMaint_Phase(&idleMaint);
    }
}

/* HOSTTEST:BEGIN predtick (extracted verbatim by host_tests/display_idle_test.c) */
// One 250 ms pass of RadarPredictTask.
static void RadarPredictTick(void)
{
    static uint32_t selectedUiElapsedMs = 0;
    static uint32_t shownAgeSec = UINT32_MAX;

    {
        Radar_PredictAircraft();
        DiagTelemetry_SampleThisTaskStack("RadarPredict");

        uint32_t now =
            xTaskGetTickCount() *
            portTICK_PERIOD_MS;

        uint32_t ageSec =
            (now - lastApiUpdateMs) / 1000;

        selectedUiElapsedMs += 250;

        // With automatic selection on, re-evaluate once a second so a closer / more important
        // aircraft (or the dwell rotation) is picked up promptly; Radar_ReconcileSelection repaints
        // the Selected Craft panel itself when the identity changes. Otherwise the 5 s cadence stays.
        static uint32_t autoSelectElapsedMs = 0;
        autoSelectElapsedMs += 250;
        bool refreshSelected =
            (selectedUiElapsedMs >= SELECTED_UI_REFRESH_MS) ||
            (Radar_GetAutoSelectClosest() && autoSelectElapsedMs >= 1000);
        if (autoSelectElapsedMs >= 1000)
            autoSelectElapsedMs = 0;

        if (selectedUiElapsedMs >= SELECTED_UI_REFRESH_MS)
        {
            selectedUiElapsedMs = 0;
        }

        UpdateDayNightBrightness();

        // Zero aircraft AND idle dim active: nothing on the radar or the
        // Selected Craft panel can change (the selection was already cleared
        // and the panel blanked when the count reached zero), so skip the
        // repaint work. Brightness and the rate-limit banner keep running.
        // When aircraft return, RadarTask's poll path reconciles, repaints the
        // panel and refreshes the radar immediately; this resumes next pass.
        bool displayIdle = Radar_IsDisplayIdle();

        if (lvgl_port_lock(-1))
        {
            uint32_t cooldownSeconds = AircraftProvider_GetRateLimitSeconds();
            if (rateLimitUiLabel) {
                if (cooldownSeconds) {
                    lv_obj_clear_flag(rateLimitUiLabel, LV_OBJ_FLAG_HIDDEN);
                    uint32_t minutes = (cooldownSeconds + 59) / 60;
                    static uint32_t shownMinutes = UINT32_MAX;
                    static int shownProvider = -1;
                    const AircraftProviderType limited = AircraftProvider_RateLimitedProvider();
                    if (minutes != shownMinutes || (int)limited != shownProvider) {
                        lv_label_set_text_fmt(rateLimitUiLabel,
                            "%s API call limit exceeded\nRetry in %lu min",
                            AircraftProviderType_Name(limited),
                            (unsigned long)minutes);
                        shownMinutes = minutes;
                        shownProvider = (int)limited;
                    }
                } else {
                    lv_obj_add_flag(rateLimitUiLabel, LV_OBJ_FLAG_HIDDEN);
                }
            }
            UpdateIdleStatusUI(); // compare-only unless the status changed
            UpdateStatusBarUI();  // 0.0.32: clock + version/uptime, text only when the minute changes

            if (!displayIdle)
            {
                if (refreshSelected)
                {
                    // Cached-data refresh: no API call, just re-run the
                    // selection against gAircraft[] and repaint the panel.
                    Radar_ReconcileSelection();
                    UpdateSelectedAircraftUI();
                }

                Radar_Refresh();

                // Only when the shown value changes (once a second, not 4x).
                if (ageSec != shownAgeSec)
                {
                    lv_label_set_text_fmt(
                        uic_LabelAPIRefresh,
                        "%lus ago",
                        ageSec);
                    shownAgeSec = ageSec;
                }
            }
            lvgl_port_unlock();
        }
    }
}
/* HOSTTEST:END predtick */

static void RadarPredictTask(
    void *pvParameters)
{
    while (1)
    {
        RadarPredictTick();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void app_main()
{

    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_master_init();
    vTaskDelay(pdMS_TO_TICKS(50));

    // Backlight is driven via LEDC PWM (instead of a plain gpio_set_level
    // on/off) so brightness can be adjusted from the web UI. Start at full
    // brightness here so the screen is lit during panel/UI init; the saved
    // brightness (if any) is applied further down once NVS is available.
    InitBacklightPWM();
    ApplyBacklightDuty(RADAR_BRIGHTNESS_DEFAULT);

    waveshare_esp32_s3_rgb_lcd_init(); // Initialize the Waveshare ESP32-S3 RGB LCD

    // One-shot check of LVGL's built-in TLSF pool, now provided from PSRAM
    // by components/fr_lvgl_pool (see fr_lv_pool.h).
    FrLvPool_LogHeap("After LVGL init");
    if (lvgl_port_lock(-1))
    {
        lv_mem_monitor_t lvMon;
        lv_mem_monitor(&lvMon);
        lvgl_port_unlock();
        ESP_LOGI("LV_POOL", "LVGL TLSF pool: total=%u free=%u biggest=%u used=%u%% frag=%u%%",
                 (unsigned)lvMon.total_size, (unsigned)lvMon.free_size, (unsigned)lvMon.free_biggest_size,
                 (unsigned)lvMon.used_pct, (unsigned)lvMon.frag_pct);
    }

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(
            nvs_flash_erase());

        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // Advanced Diagnostics setting is read once here (reboot to change), then
    // the one-time crypto initializations are triggered while internal RAM
    // is still plentiful - before CustomRules/Airports/SEEN/History/UI/Wi-Fi
    // carve it up, and after the LCD has taken its GDMA channels. See
    // boot_warmup.h for why this prevents the aircraft TLS AES failure.
    AdvDiag_LoadAtBoot();
    ExpertDebug_LoadAtBoot(); // forensic hooks; registers nothing unless ON
    BootWarmup_Crypto();

    // Persistent feature switches + dark mode (NVS "radar"); absent keys keep
    // the defaults (all features ON, light theme). Must precede CustomRules_Init
    // consumers, the radar task and the web server.
    Features_Init();
    UiPrefs_Init(); // 0.0.32: accent colors + clock format (NVS "radar"), before any UI is drawn
    if (UiPrefs_ScreenRot180()) // 0.1.2: saved Rotated 180; applied before ui_init() so the first real UI is rotated
    {
        const esp_err_t rotErr = lvgl_port_set_rotation_180(true);
        if (rotErr == ESP_OK)
            ESP_LOGI("SCREENROT", "Screen orientation: rotated 180 (saved setting); PSRAM free %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        else
            ESP_LOGE("SCREENROT", "Saved Rotated 180 could not be applied (%s); staying Normal", esp_err_to_name(rotErr));
    }
    WebStyle_SetAppearanceChangedHook(OnAppearanceChanged);

    if (!CustomRules_Init())
        ESP_LOGW("CRAFT_RULES", "Could not load one or more saved craft lists; built-in classification remains active");
    if (!Airports_Init())
        ESP_LOGW("AIRPORTS", "Could not load saved locations (airports and special air traffic)");
    { VisSettings visBoot; VisPolicy_GetSettings(&visBoot); } // 0.0.29: create the visibility-settings lock (and load its settings) once, before any task can race to create it lazily

    // Seen Aircraft history: needs SPIFFS, which CustomRules_Init mounted.
    if (!SeenAircraft_Init())
        ESP_LOGW("SEEN", "Could not initialize the seen-aircraft history");

    // Persistent (TF/microSD) aircraft history: layered underneath the
    // existing RAM Hot Seen cache above, never replacing it. A missing or
    // unmountable TF card leaves this in RAM-only degraded mode; the radar
    // and every existing feature are unaffected either way (see
    // PROJECT_STATE.md "Persistent (TF) aircraft history").
    //
#if TF_HISTORY_ENABLED
    {
        size_t heapBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        bool historyOk = HistoryManager_Init();
        size_t heapAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGW("HISTORY", "TF/SD mount internal-heap cost: free %u -> %u (-%d), largest block %u -> %u (-%d)",
                 (unsigned)heapBefore, (unsigned)heapAfter, (int)heapBefore - (int)heapAfter,
                 (unsigned)largestBefore, (unsigned)largestAfter, (int)largestBefore - (int)largestAfter);
        if (!historyOk)
            ESP_LOGW("HISTORY", "TF card not available - persistent history disabled; Hot Seen and radar continue normally");
    }
#else
    ESP_LOGW("HISTORY", "TF_HISTORY_ENABLED=0: persistent history compiled out for this build (A/B test)");
#endif

    // Loads the persisted provider selection/debug level and initializes
    // every provider (OpenSky credentials, response buffers) unconditionally
    // at boot - not gated on Wi-Fi - so the settings page reflects the
    // correct saved provider even before/without a network connection (e.g.
    // the Flight-Radar-Setup fallback AP).
    AircraftProvider_Init();

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

        CreateSelectedInfoRow(); // 0.0.30: Note / Airline row under Heading

        rateLimitUiLabel = lv_label_create(uic_PanelRadar);
        lv_obj_set_width(rateLimitUiLabel, 360);
        lv_label_set_long_mode(rateLimitUiLabel, LV_LABEL_LONG_WRAP);
        lv_obj_align(rateLimitUiLabel, LV_ALIGN_TOP_MID, 0, 5);
        lv_obj_set_style_text_align(rateLimitUiLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(rateLimitUiLabel, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(rateLimitUiLabel, lv_color_hex(0xB00020), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(rateLimitUiLabel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(rateLimitUiLabel, 6, LV_PART_MAIN);
        lv_obj_add_flag(rateLimitUiLabel, LV_OBJ_FLAG_HIDDEN);

        // Zero-aircraft status: centred on the (empty) radar, above its drawing, hidden until needed.
        idleStatusLabel = lv_label_create(uic_Imageradar);
        lv_obj_set_width(idleStatusLabel, 340);
        lv_label_set_long_mode(idleStatusLabel, LV_LABEL_LONG_WRAP);
        lv_obj_align(idleStatusLabel, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_align(idleStatusLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(idleStatusLabel, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(idleStatusLabel, lv_color_hex(UiPrefs_RadarAccentRgb()), LV_PART_MAIN);
        lv_obj_set_style_bg_color(idleStatusLabel, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(idleStatusLabel, LV_OPA_80, LV_PART_MAIN);
        lv_obj_set_style_pad_all(idleStatusLabel, 8, LV_PART_MAIN);
        lv_obj_set_style_radius(idleStatusLabel, 6, LV_PART_MAIN);
        lv_obj_clear_flag(idleStatusLabel, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(idleStatusLabel, LV_OBJ_FLAG_HIDDEN);

        // 0.0.32: bottom-bar clock (true centre) + version/uptime (left of the gear).
        // The SSID is capped so a long network name can never run under the clock.
        lv_obj_set_style_max_width(uic_LabelWifiName, 90, LV_PART_MAIN);
        lv_label_set_long_mode(uic_LabelWifiName, LV_LABEL_LONG_DOT);
        clockUiLabel = lv_label_create(uic_PanelBottom);
        lv_obj_align(clockUiLabel, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_font(clockUiLabel, &lv_font_montserrat_26, LV_PART_MAIN);
        lv_obj_set_style_text_color(clockUiLabel, lv_color_white(), LV_PART_MAIN);
        lv_label_set_text_static(clockUiLabel, "");
        lv_obj_clear_flag(clockUiLabel, LV_OBJ_FLAG_CLICKABLE);
        buildUiLabel = lv_label_create(uic_PanelBottom);
        lv_obj_align(buildUiLabel, LV_ALIGN_RIGHT_MID, -55, 0);
        lv_obj_set_style_text_font(buildUiLabel, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(buildUiLabel, lv_color_hex(0x9AA3AB), LV_PART_MAIN);
        lv_obj_set_style_text_align(buildUiLabel, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_label_set_text_static(buildUiLabel, "");
        lv_obj_clear_flag(buildUiLabel, LV_OBJ_FLAG_CLICKABLE);

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

    // 0.0.32: north reference + magnetic variation (SPIFFS is mounted by
    // CustomRules_Init above). Cache first; local WMM computation otherwise.
    NorthRef_Init(radarLat, radarLon);

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

    // 0.0.28: per-provider enable + interval. First boot after the upgrade
    // carries the old single refresh interval over to the provider in use
    // (written once; the legacy "refresh" key is left as it was).
    AircraftProvider_MigrateLegacyInterval(radarRefreshSec);

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

    // Time zone first: the schedules below (and UpdateDayNightBrightness at
    // the end of this function) read it.
    LoadTimeZoneSettings();

    uint32_t storedDayNightEnabled = 0;
    uint32_t storedDayStart = RADAR_DAY_START_HOUR_DEFAULT;
    uint32_t storedDayEnd = RADAR_DAY_END_HOUR_DEFAULT;
    uint32_t storedDayInterval = RADAR_DAY_INTERVAL_DEFAULT_SEC;
    uint32_t storedNightInterval = RADAR_NIGHT_INTERVAL_DEFAULT_SEC;

    LoadRadarU32Setting("dnenabled", &storedDayNightEnabled);
    LoadRadarU32Setting("daystart", &storedDayStart);
    LoadRadarU32Setting("dayend", &storedDayEnd);
    LoadRadarU32Setting("dayint", &storedDayInterval);
    LoadRadarU32Setting("nightint", &storedNightInterval);

    ApplyRadarDayNightSchedule(
        storedDayNightEnabled != 0,
        storedDayStart,
        storedDayEnd,
        storedDayInterval,
        storedNightInterval);

    uint32_t storedDebug = 0;

    if (LoadRadarU32Setting("openskydbg", &storedDebug))
    {
        radarOpenSkyDebugEnabled = (storedDebug != 0);
    }

    LoadSeenEvictionPolicy();

    uint32_t storedAutoSel = 0;

    if (LoadRadarU32Setting("autosel", &storedAutoSel))
    {
        Radar_SetAutoSelectClosest(storedAutoSel != 0);
    }

    uint32_t storedBrightness = RADAR_BRIGHTNESS_DEFAULT;

    if (LoadRadarU32Setting("brightness", &storedBrightness))
    {
        radarBrightnessPercent = ClampBrightness(storedBrightness);
    }

    ApplyBacklightDuty(radarBrightnessPercent);

    uint32_t storedBrDnEnabled = 0;
    uint32_t storedDayBrightness = RADAR_DAY_BRIGHTNESS_DEFAULT;
    uint32_t storedNightBrightness = RADAR_NIGHT_BRIGHTNESS_DEFAULT;

    LoadRadarU32Setting("brdnenabled", &storedBrDnEnabled);
    LoadRadarU32Setting("daybri", &storedDayBrightness);
    LoadRadarU32Setting("nightbri", &storedNightBrightness);

    ApplyRadarDayNightBrightness(
        storedBrDnEnabled != 0,
        storedDayBrightness,
        storedNightBrightness);

    uint32_t storedIdleDimEnabled = 0;
    uint32_t storedIdleDimMinutes = RADAR_IDLE_DIM_MINUTES_DEFAULT;
    uint32_t storedIdleDimPercent = RADAR_IDLE_DIM_PERCENT_DEFAULT;

    LoadRadarU32Setting("idledimen", &storedIdleDimEnabled);
    LoadRadarU32Setting("idledimmin", &storedIdleDimMinutes);
    LoadRadarU32Setting("idledimpct", &storedIdleDimPercent);

    ApplyRadarIdleDimSettings(
        storedIdleDimEnabled != 0,
        storedIdleDimMinutes,
        storedIdleDimPercent);

    // If the schedule was on at last boot, apply it immediately rather
    // than waiting for the RadarPredictTask's first pass. Falls back
    // gracefully via IsSystemTimeValid if SNTP hasn't synced yet.
    UpdateDayNightBrightness();

    IdleMaint_Init(&idleMaint);

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