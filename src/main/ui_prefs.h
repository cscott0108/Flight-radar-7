#pragma once
/*
 * Appearance preferences (0.0.32), stored in the existing NVS namespace "radar"
 * like every other radar setting. A missing key means the default, so upgraded
 * devices look exactly as before without any migration.
 *
 *  - Radar accent color (key "acc_radar", u32 0xRRGGBB, default 0x4CAF50 =
 *    LVGL's palette green the radar always used): range rings, sweep line and
 *    trail, compass and range labels, the north-reference indicator, aircraft
 *    call-sign labels and the idle status text. Never classification colors,
 *    location marker colors or status colors (e.g. "Connected").
 *  - WebUI accent color (key "acc_web", default 0x4CAF50 = the stylesheet's
 *    --acc): navigation highlight, heading underlines. Independent of the radar
 *    color: changing one never changes the other.
 *  - Device clock format (key "clockfmt", u8: 1 = 12-hour (default), 2 = 24-hour; also used by the
 *    WebUI clock). Stored 0 is the pre-0.1.6 "Off" choice: read as 12-hour + hidden.
 *  - Device clock visibility (0.1.6, key "clockshow", u8: 1 = shown (default, also when the key is
 *    missing), 0 = hidden). Display only: time keeping, time zone and DST are unaffected, and the
 *    format is kept while the clock is hidden. A pre-0.1.6 "Off" (clockfmt 0) with no clockshow key
 *    loads as hidden, so upgraded devices look exactly as before.
 *  - Screen orientation (0.1.2, key "screenrot", u8: 0 = Normal (default, also
 *    when the key is missing), 1 = Rotated 180 degrees). The display itself is
 *    switched by lvgl_port_set_rotation_180(); this module only stores the
 *    choice, and only after the display switch succeeded.
 */
#include <stdbool.h>
#include <stdint.h>

#define UI_ACCENT_DEFAULT_RGB 0x4CAF50u

typedef enum {
    UI_CLOCK_OFF = 0, /* legacy (pre-0.1.6) value only: UiPrefs_ClockFormat() never returns it */
    UI_CLOCK_12H = 1,
    UI_CLOCK_24H = 2,
    UI_CLOCK_FORMAT_COUNT
} UiClockFormat;

void UiPrefs_Init(void); /* once at boot, after nvs_flash_init */
uint32_t UiPrefs_RadarAccentRgb(void);
uint32_t UiPrefs_WebAccentRgb(void);
UiClockFormat UiPrefs_ClockFormat(void);
/* Persist (only when changed) and apply immediately. False: invalid value or NVS
 * error (the in-RAM value is still applied for an NVS error). */
bool UiPrefs_SetRadarAccentRgb(uint32_t rgb);
bool UiPrefs_SetWebAccentRgb(uint32_t rgb);
/* UI_CLOCK_12H / UI_CLOCK_24H set the format; UI_CLOCK_OFF (an older form) hides the clock and keeps
 * the format, exactly like UiPrefs_SetClockVisible(false). */
bool UiPrefs_SetClockFormat(UiClockFormat fmt);
/* 0.1.6 device-screen clock visibility (default true). */
bool UiPrefs_ClockVisible(void);
bool UiPrefs_SetClockVisible(bool visible);
/* Saved screen orientation (true = Rotated 180). The setter writes NVS first and
 * changes the in-RAM value only if the write succeeded (false = not saved). */
bool UiPrefs_ScreenRot180(void);
bool UiPrefs_SetScreenRot180(bool rotated);
/* Monotonic counter bumped by every change (radar redraw / label restyle checks). */
uint32_t UiPrefs_Revision(void);

/* "#RRGGBB" (case-insensitive) -> 0xRRGGBB. */
bool UiPrefs_ParseColor(const char *text, uint32_t *rgbOut);
/* Device clock text for local time `tl`-equivalent fields; "" for UI_CLOCK_OFF. */
void UiPrefs_FormatClock(UiClockFormat fmt, int hour, int minute, char *out, unsigned cap);
/* 0.1.6 device-screen clock text: "" when hidden (the label is then hidden), "--:--" while the local
 * time is not known yet, otherwise UiPrefs_FormatClock. */
void UiPrefs_DeviceClockText(bool visible, UiClockFormat fmt, bool timeKnown, int hour, int minute, char *out, unsigned cap);
