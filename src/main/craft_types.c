#include "craft_types.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *csvName;
    uint32_t colorRgb;
    CraftMarker marker;
    bool hasBorder;      /* true only for Important: adds a border/outline on
                          * top of the fill (yellow fill, red border) */
    uint32_t borderRgb;
} CraftTypeDef;

/* Fixed-wing marker outline width when a craft type defines a border
 * (currently only Important). Reuses the same ring mechanism the helicopter
 * marker already uses (see CraftAppearance.ringWidthPx/ringRgb). */
#define CRAFT_BORDER_WIDTH_PX 2

/* Indexed by CraftType. */
static const CraftTypeDef defs[CRAFT_TYPE_COUNT] = {
    [CRAFT_PERSONAL]    = {"Personal",           "PERSONAL",    0xFFFFFF, CRAFT_MARKER_SMALL_OUTLINE, false, 0},       /* White */
    [CRAFT_PRIVATE]     = {"Private",            "PRIVATE",     0x9E9E9E, CRAFT_MARKER_TRIANGLE,      false, 0},       /* Grey */
    [CRAFT_BUSINESS]    = {"Business",           "BUSINESS",    0xB0E0E6, CRAFT_MARKER_TRIANGLE,      false, 0},       /* Powder Blue */
    [CRAFT_COMMERCIAL]  = {"Commercial",         "COMMERCIAL",  0xFF9800, CRAFT_MARKER_TRIANGLE,      false, 0},       /* Orange */
    [CRAFT_CARGO]       = {"Cargo",              "CARGO",       0xA855F7, CRAFT_MARKER_TRIANGLE,      false, 0},       /* Purple */
    [CRAFT_MILITARY]    = {"Military",           "MILITARY",    0x8A9A20, CRAFT_MARKER_TRIANGLE,      false, 0},       /* Olive */
    [CRAFT_POLICE]      = {"Police",             "POLICE",      0x238BFF, CRAFT_MARKER_TRIANGLE,      false, 0},       /* Blue */
    [CRAFT_EMERGENCY]   = {"Emergency Services", "EMERGENCY",   0xFF3030, CRAFT_MARKER_TRIANGLE,      false, 0},       /* Red */
    [CRAFT_INTERESTING] = {"Interesting",        "INTERESTING", 0xFF6EB4, CRAFT_MARKER_TRIANGLE,      false, 0},       /* Pink (Important's old color) */
    [CRAFT_IMPORTANT]   = {"Important",          "IMPORTANT",   0xFFFF00, CRAFT_MARKER_TRIANGLE,      true,  0xFF3030},/* Yellow fill, red border */
};

/* Short aliases for hand-editing CSV files. Canonical names are matched from
 * the table above. PRIVATE / PRV are handled separately (legacy rule). */
static const struct { const char *alias; CraftType type; } aliases[] = {
    {"PER", CRAFT_PERSONAL}, {"PERS", CRAFT_PERSONAL},
    {"BUS", CRAFT_BUSINESS}, {"BIZ", CRAFT_BUSINESS},
    {"COM", CRAFT_COMMERCIAL}, {"COMM", CRAFT_COMMERCIAL},
    {"CGO", CRAFT_CARGO}, {"FRT", CRAFT_CARGO},
    {"MIL", CRAFT_MILITARY},
    {"POL", CRAFT_POLICE}, {"LEO", CRAFT_POLICE},
    {"EMERGENCY SERVICES", CRAFT_EMERGENCY}, {"EMG", CRAFT_EMERGENCY}, {"ES", CRAFT_EMERGENCY},
    {"INT", CRAFT_INTERESTING},
    {"IMP", CRAFT_IMPORTANT},
};

size_t CraftType_Count(void) { return CRAFT_TYPE_COUNT; }

bool CraftType_IsValid(int type) { return type >= 0 && type < CRAFT_TYPE_COUNT; }

static const CraftTypeDef *Def(CraftType type)
{
    return &defs[CraftType_IsValid((int)type) ? type : CRAFT_PERSONAL];
}

const char *CraftType_Name(CraftType type) { return Def(type)->name; }
const char *CraftType_CsvName(CraftType type) { return Def(type)->csvName; }
uint32_t CraftType_ColorRgb(CraftType type) { return Def(type)->colorRgb; }
CraftMarker CraftType_Marker(CraftType type) { return Def(type)->marker; }

void CraftType_ColorHtml(CraftType type, char out[8])
{
    snprintf(out, 8, "#%06X", (unsigned)(Def(type)->colorRgb & 0xFFFFFFu));
}

bool CraftType_Parse(const char *token, bool legacyFile, CraftType *out)
{
    char upper[24];
    if (!token || !out)
        return false;
    size_t length = strlen(token);
    if (length == 0 || length >= sizeof(upper))
        return false;
    for (size_t i = 0; i < length; i++) {
        char c = token[i];
        upper[i] = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
    }
    upper[length] = '\0';

    if (!strcmp(upper, "PRIVATE") || !strcmp(upper, "PRV")) {
        *out = legacyFile ? CRAFT_PERSONAL : CRAFT_PRIVATE;
        return true;
    }
    for (int i = 0; i < CRAFT_TYPE_COUNT; i++) {
        if (!strcmp(upper, defs[i].csvName)) {
            *out = (CraftType)i;
            return true;
        }
        /* Also accept the display name, upper-cased ("EMERGENCY SERVICES"). */
        char nameUpper[24];
        size_t n = strlen(defs[i].name);
        if (n < sizeof(nameUpper)) {
            for (size_t k = 0; k <= n; k++) {
                char c = defs[i].name[k];
                nameUpper[k] = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
            }
            if (!strcmp(upper, nameUpper)) {
                *out = (CraftType)i;
                return true;
            }
        }
    }
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (!strcmp(upper, aliases[i].alias)) {
            *out = aliases[i].type;
            return true;
        }
    }
    return false;
}

/* ---- aircraft type / appearance ---- */

CraftAppearance CraftType_Appearance(CraftType type, AircraftType aircraftType)
{
    CraftAppearance a;
    a.colorRgb = CraftType_ColorRgb(type); /* color always comes from the classification */
    const CraftTypeDef *def = Def(type);
    if (aircraftType == AIRCRAFT_HELICOPTER) {
        a.marker = CRAFT_MARKER_SOLID_CIRCLE;
        a.sizePx = HELI_MARKER_DIAMETER_PX;
        a.ringWidthPx = HELI_RING_WIDTH_PX;
        /* The ring is always present (never confused with an airport dot),
         * but a craft type with its own border (currently only Important)
         * uses that border color here instead of the fixed gray. The
         * heading-direction line drawn inside the ring (draw_aircraft.c)
         * always matches this same ringRgb, so Important's line is red like
         * its ring and every other classification's line is the usual gray -
         * this is not a new classification-specific rule, just the existing
         * one applied to one more visual element on the same marker. */
        a.ringRgb = def->hasBorder ? def->borderRgb : HELI_RING_RGB;
        a.selectRadiusPx = HELI_MARKER_DIAMETER_PX / 2 + 6;
    } else if (aircraftType == AIRCRAFT_OTHER) {
        a.marker = CRAFT_MARKER_DIAMOND;
        a.sizePx = OTHER_MARKER_SIZE_PX; /* vertex distance from center, like the triangle marker */
        /* Diamond outline follows the same craft-type border rule the
         * fixed-wing triangle already uses (currently only Important); the
         * forward tip is unconditional and always gray/black, drawn by
         * draw_aircraft.c using OTHER_TIP_RGB rather than this field, so it
         * never becomes classification-specific even when a border is set
         * here. */
        if (def->hasBorder) {
            a.ringWidthPx = CRAFT_BORDER_WIDTH_PX;
            a.ringRgb = def->borderRgb;
        } else {
            a.ringWidthPx = 0;
            a.ringRgb = 0;
        }
        a.selectRadiusPx = a.sizePx + 6;
    } else {
        /* Fixed-Wing keeps the existing marker exactly as before, plus an
         * outline for craft types that define a border (Important only). */
        a.marker = CraftType_Marker(type);
        a.sizePx = a.marker == CRAFT_MARKER_SMALL_OUTLINE ? 5 : 10;
        if (def->hasBorder) {
            a.ringWidthPx = CRAFT_BORDER_WIDTH_PX;
            a.ringRgb = def->borderRgb;
        } else {
            a.ringWidthPx = 0;
            a.ringRgb = 0;
        }
        a.selectRadiusPx = a.sizePx + 6;
    }
    return a;
}

size_t AircraftType_Count(void) { return AIRCRAFT_TYPE_COUNT; }

bool AircraftType_IsValid(int aircraftType)
{
    return aircraftType >= 0 && aircraftType < AIRCRAFT_TYPE_COUNT;
}

const char *AircraftType_Name(AircraftType t)
{
    if (t == AIRCRAFT_HELICOPTER) return "Helicopter";
    if (t == AIRCRAFT_OTHER) return "Other";
    return "Fixed-Wing";
}

const char *AircraftType_CsvName(AircraftType t)
{
    if (t == AIRCRAFT_HELICOPTER) return "HELI";
    if (t == AIRCRAFT_OTHER) return "OTHER";
    return "FIXED";
}

bool AircraftType_Parse(const char *token, AircraftType *out)
{
    static const struct { const char *token; AircraftType type; } accepted[] = {
        {"FIXED", AIRCRAFT_FIXED_WING}, {"FIXED-WING", AIRCRAFT_FIXED_WING},
        {"FIXED_WING", AIRCRAFT_FIXED_WING}, {"FIXEDWING", AIRCRAFT_FIXED_WING},
        {"FIXED WING", AIRCRAFT_FIXED_WING}, {"FW", AIRCRAFT_FIXED_WING},
        {"HELI", AIRCRAFT_HELICOPTER}, {"HELICOPTER", AIRCRAFT_HELICOPTER},
        {"HELO", AIRCRAFT_HELICOPTER},
        {"OTHER", AIRCRAFT_OTHER}, {"OTH", AIRCRAFT_OTHER}, {"MISC", AIRCRAFT_OTHER},
    };
    char upper[16];
    if (!token || !out)
        return false;
    size_t length = strlen(token);
    if (length == 0 || length >= sizeof(upper))
        return false;
    for (size_t i = 0; i < length; i++) {
        char c = token[i];
        upper[i] = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
    }
    upper[length] = '\0';
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
        if (!strcmp(upper, accepted[i].token)) {
            *out = accepted[i].type;
            return true;
        }
    }
    return false;
}
