#pragma once

/* Single source of truth for craft types: name, CSV token, color and marker
 * shape. Everything else (rules, operators, radar drawing, airport-preview
 * page, web dropdowns, CSV parsing) resolves a CraftType and asks this module
 * how it looks - no other file may hard-code a per-type color or marker.
 *
 * Numeric values are NOT persisted anywhere (CSV files store CSV tokens), so
 * the enum order is free to change; it is also the dropdown order. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CRAFT_PERSONAL = 0,
    CRAFT_PRIVATE,
    CRAFT_BUSINESS,
    CRAFT_COMMERCIAL,
    CRAFT_CARGO,
    CRAFT_MILITARY,
    CRAFT_POLICE,
    CRAFT_EMERGENCY,
    CRAFT_INTERESTING,
    CRAFT_IMPORTANT,
    CRAFT_TYPE_COUNT
} CraftType;

typedef enum {
    CRAFT_MARKER_SMALL_OUTLINE = 0, /* small outlined triangle (Personal) */
    CRAFT_MARKER_TRIANGLE,          /* standard filled triangle (all others) */
    CRAFT_MARKER_SOLID_CIRCLE,      /* helicopter: filled circle with a gray ring */
    CRAFT_MARKER_DIAMOND            /* other: classification-colored diamond with a directional tip */
} CraftMarker;

/* Aircraft type is a MANUAL designation, independent of the craft type
 * (classification). Classification decides the color; aircraft type decides
 * the shape. It is never inferred from OpenSky data. Names/tokens are
 * persisted as CSV tokens, so the enum order is free. */
typedef enum {
    AIRCRAFT_FIXED_WING = 0, /* default: the existing marker for the craft type */
    AIRCRAFT_HELICOPTER,     /* solid circle in the craft type's color */
    AIRCRAFT_OTHER,          /* uncommon aircraft (airship, autogyro, etc): diamond */
    AIRCRAFT_TYPE_COUNT
} AircraftType;

/* Helicopter marker geometry (screen pixels; LVGL coordinates on this panel
 * are pixels and the marker does not scale with radar range). */
#define HELI_MARKER_DIAMETER_PX 18
#define HELI_RING_WIDTH_PX 3         /* band drawn inside the 18 px circle */
#define HELI_RING_RGB 0x707070       /* gray band; sets helicopters apart from airport dots */

/* Other marker geometry. sizePx mirrors the standard filled-triangle marker's
 * vertex-distance scale (see CraftType_Appearance) so it reads as a similar
 * footprint on the radar, just a different silhouette. The forward tip is a
 * fixed gray/black indicator - it reuses the existing helicopter-ring gray
 * rather than inventing a new configurable color, and is never
 * classification-specific (unlike the diamond's fill/outline, which are). */
#define OTHER_MARKER_SIZE_PX 10
#define OTHER_TIP_RGB HELI_RING_RGB

/* The one authoritative answer to "how do I draw this aircraft". */
typedef struct {
    CraftMarker marker;
    uint32_t colorRgb;     /* fill: always the craft type's own color */
    int sizePx;            /* triangles: vertex distance from center; circle: diameter */
    int ringWidthPx;       /* 0 = no ring/border. Used for the helicopter ring
                             * (always) and, for craft types that define a
                             * border (currently only Important), also as the
                             * fixed-wing triangle's outline width. */
    uint32_t ringRgb;
    int selectRadiusPx;    /* radius of the yellow "selected" ring */
} CraftAppearance;

size_t CraftType_Count(void);
bool CraftType_IsValid(int type);

const char *CraftType_Name(CraftType type);    /* "Emergency Services" */
const char *CraftType_CsvName(CraftType type); /* "EMERGENCY" - canonical file/form token */
uint32_t CraftType_ColorRgb(CraftType type);   /* 0xRRGGBB */
CraftMarker CraftType_Marker(CraftType type);
void CraftType_ColorHtml(CraftType type, char out[8]); /* "#RRGGBB" */

/* classification -> color, aircraft type -> shape. Every renderer (radar,
 * web preview) must go through this function. */
CraftAppearance CraftType_Appearance(CraftType type, AircraftType aircraftType);

size_t AircraftType_Count(void);
bool AircraftType_IsValid(int aircraftType);
const char *AircraftType_Name(AircraftType t);    /* "Fixed-Wing", "Helicopter", "Other" */
const char *AircraftType_CsvName(AircraftType t); /* "FIXED", "HELI", "OTHER" - canonical file/form token */
/* Case-insensitive; accepts FIXED, FIXED-WING, FW, HELI, HELICOPTER, HELO, OTHER, OTH. */
bool AircraftType_Parse(const char *token, AircraftType *out);

/* Case-insensitive; accepts canonical names, CSV tokens and short aliases.
 * legacyFile=true reads a pre-version-2 CSV, where PRIVATE (and PRV) meant
 * the old small-aircraft category and therefore map to Personal. */
bool CraftType_Parse(const char *token, bool legacyFile, CraftType *out);
