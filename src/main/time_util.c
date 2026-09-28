#include "time_util.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Calendar math (proleptic Gregorian, integer only). days_from_civil /
 * civil_from_days are the well-known public-domain algorithms by Howard
 * Hinnant; day 0 is 1970-01-01, a Thursday.
 * ------------------------------------------------------------------------- */

static int64_t DaysFromCivil(int year, int month, int day)
{
    int64_t y = year - (month <= 2 ? 1 : 0);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t mp = (month > 2) ? (month - 3) : (month + 9);
    int64_t doy = (153 * mp + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void CivilFromDays(int64_t z, int *year, int *month, int *day)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = (mp < 10) ? (mp + 3) : (mp - 9);
    *year = (int)(y + (m <= 2 ? 1 : 0));
    *month = (int)m;
    *day = (int)d;
}

static int WeekdayFromDays(int64_t days)
{
    return (int)(((days % 7) + 7 + 4) % 7); /* 1970-01-01 was a Thursday (4) */
}

static int64_t FloorDiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

/* Day number of the nth (1-4) or last (5) given weekday of a month. */
static int64_t NthWeekdayDays(int year, int month, int week, int weekday)
{
    int64_t first = DaysFromCivil(year, month, 1);
    int offset = (weekday - WeekdayFromDays(first) + 7) % 7;
    int64_t day = first + offset;
    if (week < 5)
        return day + 7 * (week - 1);

    day += 7 * 4; /* fifth candidate; step back if it spilled into next month */
    int y, m, d;
    CivilFromDays(day, &y, &m, &d);
    if (m != month)
        day -= 7;
    return day;
}

/* ---------------------------------------------------------------------------
 * DST rules. Each transition is "nth weekday of a month at a time of day".
 * Times are wall-clock in the zone (standard time for the start, daylight
 * time for the end) unless utcBased, as in the EU where both transitions are
 * at 01:00 UTC.
 * ------------------------------------------------------------------------- */

typedef struct {
    uint8_t startMonth, startWeek, startWeekday;
    int32_t startSec;
    uint8_t endMonth, endWeek, endWeekday;
    int32_t endSec;
    bool utcBased;
} DstRule;

enum { RULE_NONE = 0, RULE_US, RULE_EU, RULE_AU, RULE_NZ };

/* Index by the RULE_* enum; RULE_NONE's slot is unused. */
static const DstRule dstRules[] = {
    [RULE_NONE] = {0, 0, 0, 0, 0, 0, 0, 0, false},
    /* US & Canada: 2nd Sunday of March 02:00 -> 1st Sunday of November 02:00. */
    [RULE_US] = {3, 2, 0, 2 * 3600, 11, 1, 0, 2 * 3600, false},
    /* EU/UK/Ireland/Portugal: last Sunday of March -> last Sunday of October,
     * both at 01:00 UTC. */
    [RULE_EU] = {3, 5, 0, 1 * 3600, 10, 5, 0, 1 * 3600, true},
    /* Australia (NSW/Vic/Tas/SA): 1st Sunday of October 02:00 standard ->
     * 1st Sunday of April 03:00 daylight. Southern hemisphere (wraps the
     * year end). */
    [RULE_AU] = {10, 1, 0, 2 * 3600, 4, 1, 0, 3 * 3600, false},
    /* New Zealand: last Sunday of September 02:00 standard -> 1st Sunday of
     * April 03:00 daylight. */
    [RULE_NZ] = {9, 5, 0, 2 * 3600, 4, 1, 0, 3 * 3600, false},
};

/* DST is always exactly one hour ahead of standard in this table. */
#define DST_SAVE_MINUTES 60

static bool DstActive(int rule, int stdOffsetMinutes, int64_t utc)
{
    if (rule <= RULE_NONE)
        return false;
    const DstRule *r = &dstRules[rule];

    /* The year is taken in standard local time so a transition never lands
     * on the wrong side of a year boundary for either hemisphere. */
    int year, month, day;
    CivilFromDays(FloorDiv(utc + (int64_t)stdOffsetMinutes * 60, 86400), &year, &month, &day);

    int64_t start = NthWeekdayDays(year, r->startMonth, r->startWeek, r->startWeekday) * 86400 + r->startSec;
    int64_t end = NthWeekdayDays(year, r->endMonth, r->endWeek, r->endWeekday) * 86400 + r->endSec;
    if (!r->utcBased) {
        start -= (int64_t)stdOffsetMinutes * 60;
        end -= (int64_t)(stdOffsetMinutes + DST_SAVE_MINUTES) * 60;
    }

    if (start < end)
        return utc >= start && utc < end; /* northern hemisphere */
    return utc >= start || utc < end;     /* southern: DST spans New Year */
}

/* ---------------------------------------------------------------------------
 * Zone table. Index 0 must stay "UTC" (the default); the rest are sorted by
 * id (checked by the host tests). Abbreviations follow the IANA tzdata
 * convention, including numeric ones such as "+03" where no letters exist.
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *id;
    const char *stdAbbr;
    const char *dstAbbr; /* NULL when the zone has no DST rule */
    int16_t stdMinutes;
    uint8_t rule;
} ZoneDef;

static const ZoneDef zones[] = {
    {"UTC", "UTC", NULL, 0, RULE_NONE},

    {"Africa/Accra", "GMT", NULL, 0, RULE_NONE},
    {"Africa/Algiers", "CET", NULL, 60, RULE_NONE},
    {"Africa/Johannesburg", "SAST", NULL, 120, RULE_NONE},
    {"Africa/Lagos", "WAT", NULL, 60, RULE_NONE},
    {"Africa/Nairobi", "EAT", NULL, 180, RULE_NONE},

    {"America/Anchorage", "AKST", "AKDT", -540, RULE_US},
    {"America/Argentina/Buenos_Aires", "-03", NULL, -180, RULE_NONE},
    {"America/Bogota", "-05", NULL, -300, RULE_NONE},
    {"America/Caracas", "-04", NULL, -240, RULE_NONE},
    {"America/Chicago", "CST", "CDT", -360, RULE_US},
    {"America/Costa_Rica", "CST", NULL, -360, RULE_NONE},
    {"America/Denver", "MST", "MDT", -420, RULE_US},
    {"America/Edmonton", "MST", "MDT", -420, RULE_US},
    {"America/Guatemala", "CST", NULL, -360, RULE_NONE},
    {"America/Halifax", "AST", "ADT", -240, RULE_US},
    {"America/Jamaica", "EST", NULL, -300, RULE_NONE},
    {"America/Lima", "-05", NULL, -300, RULE_NONE},
    {"America/Los_Angeles", "PST", "PDT", -480, RULE_US},
    {"America/Mexico_City", "CST", NULL, -360, RULE_NONE},
    {"America/New_York", "EST", "EDT", -300, RULE_US},
    {"America/Panama", "EST", NULL, -300, RULE_NONE},
    {"America/Phoenix", "MST", NULL, -420, RULE_NONE},
    {"America/Puerto_Rico", "AST", NULL, -240, RULE_NONE},
    {"America/Regina", "CST", NULL, -360, RULE_NONE},
    {"America/Sao_Paulo", "-03", NULL, -180, RULE_NONE},
    {"America/St_Johns", "NST", "NDT", -210, RULE_US},
    {"America/Toronto", "EST", "EDT", -300, RULE_US},
    {"America/Vancouver", "PST", "PDT", -480, RULE_US},
    {"America/Winnipeg", "CST", "CDT", -360, RULE_US},

    {"Asia/Baghdad", "+03", NULL, 180, RULE_NONE},
    {"Asia/Bangkok", "+07", NULL, 420, RULE_NONE},
    {"Asia/Dhaka", "+06", NULL, 360, RULE_NONE},
    {"Asia/Dubai", "+04", NULL, 240, RULE_NONE},
    {"Asia/Ho_Chi_Minh", "+07", NULL, 420, RULE_NONE},
    {"Asia/Hong_Kong", "HKT", NULL, 480, RULE_NONE},
    {"Asia/Jakarta", "WIB", NULL, 420, RULE_NONE},
    {"Asia/Karachi", "PKT", NULL, 300, RULE_NONE},
    {"Asia/Kathmandu", "+0545", NULL, 345, RULE_NONE},
    {"Asia/Kolkata", "IST", NULL, 330, RULE_NONE},
    {"Asia/Kuala_Lumpur", "+08", NULL, 480, RULE_NONE},
    {"Asia/Manila", "PST", NULL, 480, RULE_NONE},
    {"Asia/Riyadh", "+03", NULL, 180, RULE_NONE},
    {"Asia/Seoul", "KST", NULL, 540, RULE_NONE},
    {"Asia/Shanghai", "CST", NULL, 480, RULE_NONE},
    {"Asia/Singapore", "+08", NULL, 480, RULE_NONE},
    {"Asia/Taipei", "CST", NULL, 480, RULE_NONE},
    {"Asia/Tashkent", "+05", NULL, 300, RULE_NONE},
    {"Asia/Tehran", "+0330", NULL, 210, RULE_NONE},
    {"Asia/Tokyo", "JST", NULL, 540, RULE_NONE},

    {"Atlantic/Reykjavik", "GMT", NULL, 0, RULE_NONE},

    {"Australia/Adelaide", "ACST", "ACDT", 570, RULE_AU},
    {"Australia/Brisbane", "AEST", NULL, 600, RULE_NONE},
    {"Australia/Darwin", "ACST", NULL, 570, RULE_NONE},
    {"Australia/Hobart", "AEST", "AEDT", 600, RULE_AU},
    {"Australia/Melbourne", "AEST", "AEDT", 600, RULE_AU},
    {"Australia/Perth", "AWST", NULL, 480, RULE_NONE},
    {"Australia/Sydney", "AEST", "AEDT", 600, RULE_AU},

    {"Europe/Amsterdam", "CET", "CEST", 60, RULE_EU},
    {"Europe/Athens", "EET", "EEST", 120, RULE_EU},
    {"Europe/Berlin", "CET", "CEST", 60, RULE_EU},
    {"Europe/Brussels", "CET", "CEST", 60, RULE_EU},
    {"Europe/Bucharest", "EET", "EEST", 120, RULE_EU},
    {"Europe/Budapest", "CET", "CEST", 60, RULE_EU},
    {"Europe/Copenhagen", "CET", "CEST", 60, RULE_EU},
    {"Europe/Dublin", "GMT", "IST", 0, RULE_EU},
    {"Europe/Helsinki", "EET", "EEST", 120, RULE_EU},
    {"Europe/Istanbul", "+03", NULL, 180, RULE_NONE},
    {"Europe/Kyiv", "EET", "EEST", 120, RULE_EU},
    {"Europe/Lisbon", "WET", "WEST", 0, RULE_EU},
    {"Europe/London", "GMT", "BST", 0, RULE_EU},
    {"Europe/Madrid", "CET", "CEST", 60, RULE_EU},
    {"Europe/Minsk", "+03", NULL, 180, RULE_NONE},
    {"Europe/Moscow", "MSK", NULL, 180, RULE_NONE},
    {"Europe/Oslo", "CET", "CEST", 60, RULE_EU},
    {"Europe/Paris", "CET", "CEST", 60, RULE_EU},
    {"Europe/Prague", "CET", "CEST", 60, RULE_EU},
    {"Europe/Rome", "CET", "CEST", 60, RULE_EU},
    {"Europe/Stockholm", "CET", "CEST", 60, RULE_EU},
    {"Europe/Vienna", "CET", "CEST", 60, RULE_EU},
    {"Europe/Warsaw", "CET", "CEST", 60, RULE_EU},
    {"Europe/Zurich", "CET", "CEST", 60, RULE_EU},

    {"Pacific/Auckland", "NZST", "NZDT", 720, RULE_NZ},
    {"Pacific/Fiji", "+12", NULL, 720, RULE_NONE},
    {"Pacific/Guam", "ChST", NULL, 600, RULE_NONE},
    {"Pacific/Honolulu", "HST", NULL, -600, RULE_NONE},
};

#define ZONE_COUNT (sizeof(zones) / sizeof(zones[0]))

size_t TimeUtil_ZoneCount(void) { return ZONE_COUNT; }

const char *TimeUtil_ZoneId(size_t index)
{
    return index < ZONE_COUNT ? zones[index].id : "";
}

bool TimeUtil_ZoneFind(const char *id, size_t *index)
{
    if (!id)
        return false;
    for (size_t i = 0; i < ZONE_COUNT; i++) {
        if (strcmp(zones[i].id, id) == 0) {
            if (index)
                *index = i;
            return true;
        }
    }
    return false;
}

bool TimeUtil_ZoneHasDst(size_t index)
{
    return index < ZONE_COUNT && zones[index].rule != RULE_NONE;
}

/* Copies src into a caller buffer of any size, always NUL-terminating. Every
 * runtime-sized output below is formatted into a fixed local buffer first and
 * then copied with this, so -Wformat-truncation=2 (which cannot reason about a
 * runtime "cap") stays clean and truncation is well defined. */
static void CopyOut(char *out, size_t cap, const char *src)
{
    if (!out || cap == 0)
        return;
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, src, n);
    out[n] = '\0';
}

static void FormatOffset(int minutes, char *out, size_t cap)
{
    int absMinutes = minutes < 0 ? -minutes : minutes;
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "UTC%c%02d:%02d", minutes < 0 ? '-' : '+', (absMinutes / 60) % 100, absMinutes % 60);
    CopyOut(out, cap, tmp);
}

void TimeUtil_ZoneLabel(size_t index, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    if (index >= ZONE_COUNT) {
        out[0] = '\0';
        return;
    }
    const ZoneDef *z = &zones[index];
    char offset[16];
    char tmp[160];
    FormatOffset(z->stdMinutes, offset, sizeof(offset));
    if (z->dstAbbr)
        snprintf(tmp, sizeof(tmp), "%s (%s/%s, %s)", z->id, z->stdAbbr, z->dstAbbr, offset);
    else
        snprintf(tmp, sizeof(tmp), "%s (%s, %s)", z->id, z->stdAbbr, offset);
    CopyOut(out, cap, tmp);
}

/* ---------------------------------------------------------------------------
 * Active configuration: one packed 32-bit word so a reader in another task
 * can never observe a half-updated zone/DST/offset combination.
 *   bits 0-15  zone index, 0xFFFF = custom fixed offset
 *   bit  16    automatic DST
 *   bits 17-27 custom offset minutes + 720 (0..1560)
 * ------------------------------------------------------------------------- */

#define PACK_CUSTOM_INDEX 0xFFFFu
#define PACK(index, dst, custom) \
    ((uint32_t)(index) | ((uint32_t)((dst) ? 1 : 0) << 16) | ((uint32_t)((custom) + 720) << 17))

static volatile uint32_t activeConfig = PACK(0, true, 0); /* UTC, auto DST on */

void TimeUtil_DefaultConfig(TimeZoneConfig *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->zoneId, sizeof(cfg->zoneId), "%s", TIMEUTIL_DEFAULT_ZONE);
    cfg->autoDst = true;
    cfg->customOffsetMinutes = 0;
}

bool TimeUtil_SetConfig(const TimeZoneConfig *cfg)
{
    if (!cfg)
        return false;
    if (cfg->customOffsetMinutes < TIMEUTIL_CUSTOM_MIN_MINUTES ||
        cfg->customOffsetMinutes > TIMEUTIL_CUSTOM_MAX_MINUTES)
        return false;

    uint32_t index;
    if (strcmp(cfg->zoneId, TIMEUTIL_ZONE_CUSTOM) == 0) {
        index = PACK_CUSTOM_INDEX;
    } else {
        size_t found;
        if (!TimeUtil_ZoneFind(cfg->zoneId, &found))
            return false;
        index = (uint32_t)found;
    }
    activeConfig = PACK(index, cfg->autoDst, cfg->customOffsetMinutes);
    return true;
}

static void Unpack(uint32_t packed, TimeZoneConfig *out)
{
    uint32_t index = packed & 0xFFFFu;
    out->autoDst = ((packed >> 16) & 1u) != 0;
    out->customOffsetMinutes = (int)((packed >> 17) & 0x7FFu) - 720;
    if (index == PACK_CUSTOM_INDEX || index >= ZONE_COUNT)
        snprintf(out->zoneId, sizeof(out->zoneId), "%s", index == PACK_CUSTOM_INDEX ? TIMEUTIL_ZONE_CUSTOM : TIMEUTIL_DEFAULT_ZONE);
    else
        snprintf(out->zoneId, sizeof(out->zoneId), "%s", zones[index].id);
}

void TimeUtil_GetConfig(TimeZoneConfig *out)
{
    if (out)
        Unpack(activeConfig, out);
}

/* ---------------------------------------------------------------------------
 * Conversion
 * ------------------------------------------------------------------------- */

bool TimeUtil_IsSynced(int64_t utcSeconds)
{
    return utcSeconds >= TIMEUTIL_MIN_VALID_UTC;
}

bool TimeUtil_Convert(const TimeZoneConfig *cfg, int64_t utcSeconds, TimeLocal *out)
{
    if (!cfg || !out || !TimeUtil_IsSynced(utcSeconds))
        return false;

    int stdMinutes = 0;
    int rule = RULE_NONE;
    char abbrStd[12] = "UTC";
    char abbrDst[12] = "";

    if (strcmp(cfg->zoneId, TIMEUTIL_ZONE_CUSTOM) == 0) {
        stdMinutes = cfg->customOffsetMinutes;
        if (stdMinutes < TIMEUTIL_CUSTOM_MIN_MINUTES)
            stdMinutes = TIMEUTIL_CUSTOM_MIN_MINUTES;
        if (stdMinutes > TIMEUTIL_CUSTOM_MAX_MINUTES)
            stdMinutes = TIMEUTIL_CUSTOM_MAX_MINUTES;
        FormatOffset(stdMinutes, abbrStd, sizeof(abbrStd));
    } else {
        size_t index = 0; /* an unknown id degrades to UTC rather than failing */
        if (!TimeUtil_ZoneFind(cfg->zoneId, &index))
            index = 0;
        stdMinutes = zones[index].stdMinutes;
        rule = zones[index].rule;
        snprintf(abbrStd, sizeof(abbrStd), "%s", zones[index].stdAbbr);
        if (zones[index].dstAbbr)
            snprintf(abbrDst, sizeof(abbrDst), "%s", zones[index].dstAbbr);
    }

    bool dst = cfg->autoDst && DstActive(rule, stdMinutes, utcSeconds);
    int offset = stdMinutes + (dst ? DST_SAVE_MINUTES : 0);

    int64_t localSeconds = utcSeconds + (int64_t)offset * 60;
    int64_t days = FloorDiv(localSeconds, 86400);
    int64_t secOfDay = localSeconds - days * 86400;

    CivilFromDays(days, &out->year, &out->month, &out->day);
    out->hour = (int)(secOfDay / 3600);
    out->minute = (int)((secOfDay % 3600) / 60);
    out->second = (int)(secOfDay % 60);
    out->weekday = WeekdayFromDays(days);
    out->isDst = dst;
    out->utcOffsetMinutes = offset;
    snprintf(out->abbr, sizeof(out->abbr), "%s", dst ? abbrDst : abbrStd);
    return true;
}

bool TimeUtil_ToLocal(int64_t utcSeconds, TimeLocal *out)
{
    TimeZoneConfig cfg;
    TimeUtil_GetConfig(&cfg);
    return TimeUtil_Convert(&cfg, utcSeconds, out);
}

int TimeUtil_LocalMinutesOfDay(int64_t utcSeconds)
{
    TimeLocal t;
    if (!TimeUtil_ToLocal(utcSeconds, &t))
        return -1;
    return t.hour * 60 + t.minute;
}

/* ---------------------------------------------------------------------------
 * Formatting
 * ------------------------------------------------------------------------- */

static const char *const monthNames[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void TimeUtil_FormatLocalWithConfig(const TimeZoneConfig *cfg, int64_t utcSeconds, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    if (utcSeconds == 0) { /* 0 = "never recorded", not a real time */
        CopyOut(out, cap, "-");
        return;
    }
    TimeLocal t;
    if (!TimeUtil_Convert(cfg, utcSeconds, &t)) {
        CopyOut(out, cap, "Not synchronized");
        return;
    }
    int hour12 = t.hour % 12;
    if (hour12 == 0)
        hour12 = 12;
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s %d, %d %d:%02d %s %s",
             monthNames[(t.month - 1) % 12], t.day, t.year, hour12, t.minute,
             t.hour < 12 ? "AM" : "PM", t.abbr);
    CopyOut(out, cap, tmp);
}

void TimeUtil_FormatLocal(int64_t utcSeconds, char *out, size_t cap)
{
    TimeZoneConfig cfg;
    TimeUtil_GetConfig(&cfg);
    TimeUtil_FormatLocalWithConfig(&cfg, utcSeconds, out, cap);
}

void TimeUtil_FormatIsoUtc(int64_t utcSeconds, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (!TimeUtil_IsSynced(utcSeconds))
        return;
    int64_t days = FloorDiv(utcSeconds, 86400);
    int64_t secOfDay = utcSeconds - days * 86400;
    int year, month, day;
    CivilFromDays(days, &year, &month, &day);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month, day,
             (int)(secOfDay / 3600), (int)((secOfDay % 3600) / 60), (int)(secOfDay % 60));
    CopyOut(out, cap, tmp);
}
