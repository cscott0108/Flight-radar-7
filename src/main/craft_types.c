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

/* ---- web UI icons ---- */

/* The icon canvas is 20x20 with the marker centered at (10,10). ICON_UNIT
 * scales the radar's "vertex distance" (10 px for the triangle) onto it. */
#define ICON_CENTER 10
#define ICON_TRI_RADIUS 8 /* triangle vertex distance in icon units */

/* Same three-vertex construction as draw_aircraft.c's triangle (nose plus two
 * rear vertices 2.5 rad off the heading), pointing up. sin(2.5)=0.598,
 * cos(2.5)=-0.801; integer math only. */
static void TriangleVertices(int radius, int *rx, int *ry)
{
    *rx = (radius * 598 + 500) / 1000;
    *ry = (radius * 801 + 500) / 1000;
}

static size_t CopyIcon(char *out, size_t cap, const char *tmp, int written, size_t tmpCap)
{
    if (!out || cap == 0)
        return 0;
    if (written < 0 || (size_t)written >= tmpCap || (size_t)written >= cap) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, tmp, (size_t)written + 1);
    return (size_t)written;
}

size_t AircraftType_IconDefs(char *out, size_t cap)
{
    char tmp[1400];
    int fx, fy, ox, oy;
    TriangleVertices(ICON_TRI_RADIUS, &fx, &fy);
    TriangleVertices(ICON_TRI_RADIUS / 2, &ox, &oy); /* Personal's marker is half size */
    const int c = ICON_CENTER;
    const int r = ICON_TRI_RADIUS;
    /* Diamond: nose r out, sides/tail 0.6r (draw_aircraft.c sideDist). Tip:
     * two points 0.5r toward +-1 rad off the nose (sin1=.841, cos1=.540). */
    const int side = (r * 6 + 5) / 10;
    const int tipD = r / 2;
    const int tipX = (tipD * 841 + 500) / 1000;
    const int tipY = (tipD * 540 + 500) / 1000;
    const int heliOuter = HELI_MARKER_DIAMETER_PX / 2;               /* 9 */
    const int heliInner = heliOuter - HELI_RING_WIDTH_PX;            /* 6 */
    const int borderW = CRAFT_BORDER_WIDTH_PX;

    int n = snprintf(tmp, sizeof(tmp),
        "<svg width='0' height='0' style='position:absolute' aria-hidden='true'><defs>"
        /* Fixed-Wing: filled triangle; border only if --rg is set (Important). */
        "<symbol id='at-tri' viewBox='0 0 20 20'><polygon points='%d,%d %d,%d %d,%d' fill='currentColor' "
        "style='stroke:var(--rg,none);stroke-width:%d;stroke-linejoin:round'/></symbol>"
        /* Fixed-Wing, Personal: small outlined triangle. */
        "<symbol id='at-tri-o' viewBox='0 0 20 20'><polygon points='%d,%d %d,%d %d,%d' fill='none' "
        "stroke='currentColor' stroke-width='2' stroke-linejoin='round'/></symbol>"
        /* Helicopter: solid disc inside a ring (--rg, gray unless the craft type has a border). */
        "<symbol id='at-heli' viewBox='0 0 20 20'><circle cx='%d' cy='%d' r='%d' style='fill:var(--rg,#%06X)'/>"
        "<circle cx='%d' cy='%d' r='%d' fill='currentColor'/></symbol>"
        /* Other: diamond with the fixed gray forward tip. */
        "<symbol id='at-diamond' viewBox='0 0 20 20'><polygon points='%d,%d %d,%d %d,%d %d,%d' fill='currentColor' "
        "style='stroke:var(--rg,none);stroke-width:%d;stroke-linejoin:round'/>"
        "<polygon points='%d,%d %d,%d %d,%d' fill='#%06X'/></symbol>"
        "</defs></svg>",
        c, c - r, c + fx, c + fy, c - fx, c + fy, borderW,
        c, c - r / 2, c + ox, c + oy, c - ox, c + oy,
        c, c, heliOuter, (unsigned)HELI_RING_RGB, c, c, heliInner,
        c, c - r, c + side, c, c, c + side, c - side, c, borderW,
        c, c - r, c + tipX, c - tipY, c - tipX, c - tipY, (unsigned)OTHER_TIP_RGB);
    return CopyIcon(out, cap, tmp, n, sizeof(tmp));
}

size_t CraftType_IconUse(CraftType type, AircraftType aircraftType, char *out, size_t cap)
{
    const CraftAppearance look = CraftType_Appearance(type, aircraftType);
    const char *symbol = "at-tri";
    switch (look.marker) {
    case CRAFT_MARKER_SMALL_OUTLINE: symbol = "at-tri-o"; break;
    case CRAFT_MARKER_SOLID_CIRCLE: symbol = "at-heli"; break;
    case CRAFT_MARKER_DIAMOND: symbol = "at-diamond"; break;
    default: break;
    }

    /* --rg carries the ring/border color. Set only when the marker has one
     * (always for the helicopter; Important's border for the others), so a
     * plain marker keeps the symbol's default (none / gray). */
    char ring[24] = "";
    if (look.ringWidthPx > 0)
        snprintf(ring, sizeof(ring), ";--rg:#%06X", (unsigned)(look.ringRgb & 0xFFFFFFu));

    char tmp[320];
    int n = snprintf(tmp, sizeof(tmp),
        "<svg class='ati' width='18' height='18' viewBox='0 0 20 20' role='img' aria-label='%s' "
        "style='color:#%06X%s'><use href='#%s'/></svg>",
        AircraftType_Name(aircraftType), (unsigned)(look.colorRgb & 0xFFFFFFu), ring, symbol);
    return CopyIcon(out, cap, tmp, n, sizeof(tmp));
}
