#pragma once

/* Centralized time-zone / DST / formatting layer.
 *
 * Everything user-facing that shows a time (Seen Aircraft page, /radar time
 * zone section, day/night schedules) goes through this module, so no page can
 * calculate PST/PDT differently from another.
 *
 *   UTC epoch seconds (internal + stored)
 *     -> TimeUtil_Convert()  [configured zone + Automatic DST]
 *     -> TimeLocal / formatted string (display only, never stored)
 *
 * Design notes (see PROJECT_STATE.md "Time zone and DST"):
 *  - Zones are IANA identifiers ("America/Los_Angeles") looked up in a small
 *    built-in table (about 90 entries, a few KB of flash) instead of the full
 *    tz database. Each entry carries its standard offset, its abbreviations
 *    and one of a handful of DST rules (none / US / EU / Australia / New
 *    Zealand). The evaluation is self-contained integer math: it does not use
 *    setenv("TZ")/localtime_r, so there is no process-global state to race
 *    and it behaves identically on the host and on the ESP32.
 *  - Zones whose DST law is irregular or has changed recently (for example
 *    Egypt, Israel, Chile, Iran) are intentionally NOT in the table; those
 *    users can pick "Custom fixed UTC offset".
 *  - The rules are valid for years 2007 and later (the current US rule); the
 *    device only converts times after TIMEUTIL_MIN_VALID_UTC anyway.
 *  - "Automatic DST" off means: use the zone's standard offset all year.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Anything before this (about Nov 2023) is treated as "SNTP has not synced":
 * an unset ESP32 clock reads close to the 1970 epoch. Same threshold the
 * day/night schedule has always used. */
#define TIMEUTIL_MIN_VALID_UTC 1700000000LL

#define TIMEUTIL_ZONE_ID_MAX 40          /* including the NUL */
#define TIMEUTIL_ZONE_CUSTOM "CUSTOM"    /* persisted token for the fixed-offset choice */
#define TIMEUTIL_DEFAULT_ZONE "UTC"      /* no location is assumed for new installs */
#define TIMEUTIL_CUSTOM_MIN_MINUTES (-720)
#define TIMEUTIL_CUSTOM_MAX_MINUTES 840

typedef struct {
    int year;
    int month;   /* 1-12 */
    int day;     /* 1-31 */
    int hour;    /* 0-23 */
    int minute;
    int second;
    int weekday; /* 0 = Sunday */
    bool isDst;
    int utcOffsetMinutes; /* offset in effect at this instant, DST included */
    char abbr[12];        /* "PDT", "UTC+05:30", ... */
} TimeLocal;

typedef struct {
    char zoneId[TIMEUTIL_ZONE_ID_MAX]; /* IANA id or TIMEUTIL_ZONE_CUSTOM */
    bool autoDst;
    int customOffsetMinutes;           /* used only when zoneId is CUSTOM */
} TimeZoneConfig;

/* ---- zone table ---- */
size_t TimeUtil_ZoneCount(void);
const char *TimeUtil_ZoneId(size_t index);
bool TimeUtil_ZoneFind(const char *id, size_t *index);
bool TimeUtil_ZoneHasDst(size_t index);
/* "America/New_York (EST/EDT, UTC-05:00)"; for the option list. */
void TimeUtil_ZoneLabel(size_t index, char *out, size_t cap);

/* ---- configuration (single packed word: safe to read from any task) ---- */
void TimeUtil_DefaultConfig(TimeZoneConfig *cfg);
/* Validates (known zone id or CUSTOM, custom offset in range). Returns false
 * and leaves the active configuration unchanged if invalid. */
bool TimeUtil_SetConfig(const TimeZoneConfig *cfg);
void TimeUtil_GetConfig(TimeZoneConfig *out);

/* ---- conversion ---- */
bool TimeUtil_IsSynced(int64_t utcSeconds);
/* Pure conversion with an explicit config (used by tests and by the wrappers
 * below). Returns false, leaving *out untouched, when utcSeconds is not a
 * synchronized time. */
bool TimeUtil_Convert(const TimeZoneConfig *cfg, int64_t utcSeconds, TimeLocal *out);
/* Same, using the active configuration. */
bool TimeUtil_ToLocal(int64_t utcSeconds, TimeLocal *out);
/* Local minutes since midnight in the active zone, or -1 if not synced. */
int TimeUtil_LocalMinutesOfDay(int64_t utcSeconds);

/* ---- formatting (display only; never store these strings) ---- */
/* "Sep 23, 2026 2:14 PM PDT". Unsynchronized/zero times yield "Not synchronized"
 * (or "-" for utcSeconds == 0, meaning "never recorded"). Always NUL-terminates. */
void TimeUtil_FormatLocal(int64_t utcSeconds, char *out, size_t cap);
void TimeUtil_FormatLocalWithConfig(const TimeZoneConfig *cfg, int64_t utcSeconds, char *out, size_t cap);
/* "2026-09-23T21:14:00Z" for CSV export; empty string if not a valid time. */
void TimeUtil_FormatIsoUtc(int64_t utcSeconds, char *out, size_t cap);
