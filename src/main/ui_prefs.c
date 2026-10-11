#include "ui_prefs.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"

#define UP_NAMESPACE "radar"

static _Atomic uint32_t s_radarRgb = UI_ACCENT_DEFAULT_RGB;
static _Atomic uint32_t s_webRgb = UI_ACCENT_DEFAULT_RGB;
static _Atomic uint32_t s_clock = UI_CLOCK_12H;
static _Atomic uint32_t s_rev = 1;
static _Atomic uint32_t s_screenRot = 0; /* 0.1.2: 0 Normal, 1 Rotated 180 */
static _Atomic uint32_t s_clockShow = 1;  /* 0.1.6: device clock shown */
static _Atomic uint32_t s_legacyOff = 0;  /* NVS still holds the pre-0.1.6 clockfmt 0 ("Off") */

static bool LoadU32(nvs_handle_t h, const char *key, uint32_t *v)
{
    return nvs_get_u32(h, key, v) == ESP_OK;
}

void UiPrefs_Init(void)
{
    nvs_handle_t h;
    if (nvs_open(UP_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return; /* defaults */
    uint32_t v;
    if (LoadU32(h, "acc_radar", &v) && v <= 0xFFFFFFu)
        atomic_store(&s_radarRgb, v);
    if (LoadU32(h, "acc_web", &v) && v <= 0xFFFFFFu)
        atomic_store(&s_webRgb, v);
    uint8_t c;
    bool legacyOff = false;
    if (nvs_get_u8(h, "clockfmt", &c) == ESP_OK && c < UI_CLOCK_FORMAT_COUNT) {
        legacyOff = (c == UI_CLOCK_OFF);
        atomic_store(&s_clock, legacyOff ? (uint32_t)UI_CLOCK_12H : c); /* old "Off" = hidden, 12-hour */
    }
    atomic_store(&s_legacyOff, legacyOff ? 1u : 0u);
    if (nvs_get_u8(h, "clockshow", &c) == ESP_OK && c <= 1u)
        atomic_store(&s_clockShow, c);
    else
        atomic_store(&s_clockShow, legacyOff ? 0u : 1u); /* missing / invalid: as before 0.1.6 */
    if (nvs_get_u8(h, "screenrot", &c) == ESP_OK && c <= 1u) /* other values: Normal */
        atomic_store(&s_screenRot, c);
    nvs_close(h);
    atomic_fetch_add(&s_rev, 1u);
}

uint32_t UiPrefs_RadarAccentRgb(void) { return atomic_load(&s_radarRgb); }
uint32_t UiPrefs_WebAccentRgb(void) { return atomic_load(&s_webRgb); }
UiClockFormat UiPrefs_ClockFormat(void) { return (UiClockFormat)atomic_load(&s_clock); }
uint32_t UiPrefs_Revision(void) { return atomic_load(&s_rev); }

static bool Persist(const char *key, uint32_t v, bool u8)
{
    nvs_handle_t h;
    if (nvs_open(UP_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return false;
    esp_err_t err = u8 ? nvs_set_u8(h, key, (uint8_t)v) : nvs_set_u32(h, key, v);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static bool SetValue(_Atomic uint32_t *slot, const char *key, uint32_t v, bool u8)
{
    if (atomic_load(slot) == v)
        return true; /* unchanged: no flash write */
    atomic_store(slot, v);
    atomic_fetch_add(&s_rev, 1u);
    return Persist(key, v, u8);
}

bool UiPrefs_SetRadarAccentRgb(uint32_t rgb)
{
    return rgb <= 0xFFFFFFu && SetValue(&s_radarRgb, "acc_radar", rgb, false);
}

bool UiPrefs_SetWebAccentRgb(uint32_t rgb)
{
    return rgb <= 0xFFFFFFu && SetValue(&s_webRgb, "acc_web", rgb, false);
}

bool UiPrefs_ClockVisible(void) { return atomic_load(&s_clockShow) != 0u; }

/* Replaces a stored pre-0.1.6 "Off" (clockfmt 0) by the real format once the user saves a clock setting,
 * so the old value cannot come back. */
static bool ClearLegacyOff(void)
{
    if (!atomic_load(&s_legacyOff))
        return true;
    if (!Persist("clockfmt", atomic_load(&s_clock), true))
        return false;
    atomic_store(&s_legacyOff, 0u);
    return true;
}

bool UiPrefs_SetClockVisible(bool visible)
{
    const bool ok = SetValue(&s_clockShow, "clockshow", visible ? 1u : 0u, true);
    return ClearLegacyOff() && ok;
}

bool UiPrefs_SetClockFormat(UiClockFormat fmt)
{
    if (fmt == UI_CLOCK_OFF)
        return UiPrefs_SetClockVisible(false); /* older form: "Off" = hide, keep the format */
    if ((unsigned)fmt >= UI_CLOCK_FORMAT_COUNT)
        return false;
    const bool ok = SetValue(&s_clock, "clockfmt", (uint32_t)fmt, true);
    return ClearLegacyOff() && ok;
}

bool UiPrefs_ScreenRot180(void) { return atomic_load(&s_screenRot) != 0u; }

bool UiPrefs_SetScreenRot180(bool rotated)
{
    const uint32_t v = rotated ? 1u : 0u;
    if (atomic_load(&s_screenRot) == v)
        return true; /* unchanged: no flash write */
    if (!Persist("screenrot", v, true))
        return false; /* not saved: keep the old value */
    atomic_store(&s_screenRot, v);
    return true;
}

static int Hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool UiPrefs_ParseColor(const char *text, uint32_t *rgbOut)
{
    if (!text || text[0] != '#' || strlen(text) != 7)
        return false;
    uint32_t v = 0;
    for (int i = 1; i < 7; i++) {
        const int d = Hex(text[i]);
        if (d < 0)
            return false;
        v = v * 16u + (uint32_t)d;
    }
    if (rgbOut)
        *rgbOut = v;
    return true;
}

void UiPrefs_DeviceClockText(bool visible, UiClockFormat fmt, bool timeKnown, int hour, int minute, char *out, unsigned cap)
{
    if (!out || !cap)
        return;
    out[0] = 0;
    if (!visible)
        return;
    if (timeKnown)
        UiPrefs_FormatClock(fmt, hour, minute, out, cap);
    else
        snprintf(out, cap, "--:--");
}

void UiPrefs_FormatClock(UiClockFormat fmt, int hour, int minute, char *out, unsigned cap)
{
    if (!out || !cap)
        return;
    out[0] = 0;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return;
    if (fmt == UI_CLOCK_24H) {
        snprintf(out, cap, "%02d:%02d", hour, minute);
    } else if (fmt == UI_CLOCK_12H) {
        const int h12 = hour % 12 == 0 ? 12 : hour % 12;
        snprintf(out, cap, "%d:%02d %s", h12, minute, hour < 12 ? "AM" : "PM");
    }
}
