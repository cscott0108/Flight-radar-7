#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* "Airports and Special Air Traffic" locations.
 *
 * This is the one saved-location store for every radar reference marker:
 * airports/runways, heliports, and special aviation-interest places such as
 * stadiums or major venues. The type and function names still say "Airport"
 * because they predate the generalization; the storage, cap and blob layout
 * are unchanged (see airports.c).
 *
 * Locations are CONTEXTUAL MARKERS ONLY. Nothing here, and nothing that reads
 * these markers, may classify, tag, track or otherwise change the meaning of
 * an aircraft because it is near a location - normal traffic routinely passes
 * over stadiums (e.g. SJC departures near Levi's Stadium). Aircraft
 * significance comes only from Registered Aircraft / Operators configuration.
 * Event-window awareness is future work and is not implemented. */

/* User-defined location store (NVS blob, unchanged layout): at most 100. */
#define MAX_AIRPORTS 100
/* Active location capacity: all user-defined locations plus the regionally
 * selected built-in airports (builtin_airports.h) together never exceed this.
 * Built-ins only ever use the slots the user locations leave free, so at
 * least AIRPORT_ACTIVE_MAX - MAX_AIRPORTS (150) remain for built-ins. */
#define AIRPORT_ACTIVE_MAX 250
#define AIRPORT_BUILTIN_TIER1_DIAMETER 12 /* major airports */
#define AIRPORT_BUILTIN_TIER2_DIAMETER 8  /* regional airports */
#define AIRPORT_BUILTIN_COLOR AIRPORT_DEFAULT_COLOR
#define AIRPORT_NAME_LENGTH 24
#define AIRPORT_DEFAULT_DIAMETER 12
#define AIRPORT_DEFAULT_COLOR 0xFF0000 /* red, the original fixed airport color */

/* Longest accepted runway designator text, e.g. "36R" (2 digits + optional
 * L/C/R suffix). Stored exactly as entered; the physical axis is derived
 * live from it (see Airport_ParseRunwayAxis) rather than cached, so it can
 * never go stale relative to what's on screen. */
#define AIRPORT_RUNWAY_LENGTH 3

/* Marker type (stored in AirportMarker.markerMode). Values 0 and 1 are the
 * original on-disk values and must never change; new types are appended. */
typedef enum {
    AIRPORT_MARKER_DOT = 0,         /* generic location: plain filled dot (always available) */
    AIRPORT_MARKER_DIRECTIONAL = 1, /* airport/runway: dot + runway-axis line, when orientation is known */
    AIRPORT_MARKER_HELIPORT = 2,    /* heliport / helicopter location: an "H" */
    AIRPORT_MARKER_SQUARE = 3,      /* special aviation-interest location (stadium, venue): filled square */
    AIRPORT_MARKER_TYPE_COUNT
} AirportMarkerMode;

typedef struct {
    char name[AIRPORT_NAME_LENGTH + 1];
    float latitude;
    float longitude;
    uint8_t diameter;
    uint32_t color; /* 0xRRGGBB */
    uint8_t markerMode; /* AirportMarkerMode (marker type); dot is always the fallback */
    char runway[AIRPORT_RUNWAY_LENGTH + 1]; /* e.g. "09", "27L", "" = none/unknown */
} AirportMarker;

/* Call after nvs_flash_init(), before the web server or radar starts. */
bool Airports_Init(void);
size_t Airports_Count(void);
bool Airports_Get(size_t index, AirportMarker *out);
bool Airports_Save(int index, const AirportMarker *marker); /* -1 appends */
bool Airports_Delete(size_t index);
/* Why the last Save/Delete returned false (0.0.32): ESP_ERR_INVALID_ARG = invalid
 * location, ESP_ERR_INVALID_STATE = 100-location limit, ESP_ERR_NOT_FOUND = no
 * such entry, ESP_ERR_NO_MEM = scratch buffer, otherwise the NVS error
 * (e.g. ESP_ERR_NVS_NOT_ENOUGH_SPACE). ESP_OK after a success. */
esp_err_t Airports_LastError(void);
/* Bytes the stored location blob takes for the current count (header + used markers). */
size_t Airports_StoredBlobBytes(void);

/* ---- built-in airports: regional selection ----
 *
 * Global curated database -> airports inside the radar's current range
 * (same flat-earth distance as Radar_ProjectPosition, so exactly the ones the
 * radar can draw) -> skip any whose ident a user location already names
 * (user entry wins) -> priority: tier 1 (major) before tier 2 (regional),
 * nearest first, then table order -> keep at most AIRPORT_ACTIVE_MAX minus
 * the number of user locations. User-defined locations are always all active
 * and are never displaced by built-ins.
 *
 * Recomputed only when the center, range or user list changed; otherwise
 * this is a cheap cache check. The selection is a small index array in RAM;
 * the airport data itself stays in flash. Returns the number of active
 * built-in airports. Thread-safe (airportsLock). */
size_t Airports_SelectBuiltins(float centerLat, float centerLon, float rangeKm);
/* Active built-in airport i (0 <= i < the last Airports_SelectBuiltins()
 * result) as a marker: name "ICAO Name", tier-based size,
 * AIRPORT_BUILTIN_COLOR, Directional when the runway axis is known (else Dot),
 * with any override's position applied. A rotation override is only exact via
 * Airports_GetActiveBuiltinView (the runway text holds the nearest 10 deg). */
bool Airports_GetActiveBuiltin(size_t index, AirportMarker *out);
/* Built-in airport overrides (0.0.25).
 *
 * The database stays read-only. A per-airport override, keyed by the
 * airport's ident, replaces only the fields marked in `fields`; everything
 * else keeps the database value. Overrides are applied AFTER regional
 * selection (selection, priority and dedupe always use the database values),
 * so a hidden airport stays selected and listed but is not drawn. Overrides
 * persist (NVS namespace "airports", key "bi_ovr", only the records in use)
 * whether or not the airport is currently in range, never use a user-defined
 * location slot, and a record with no fields left is deleted. */
#define AIRPORT_OVERRIDE_MAX 100
#define AIRPORT_OVR_HIDDEN 0x01   /* hide on the radar (default: shown) */
#define AIRPORT_OVR_LAT 0x02      /* latE5 replaces the database latitude */
#define AIRPORT_OVR_LON 0x04      /* lonE5 replaces the database longitude */
#define AIRPORT_OVR_ROTATION 0x08 /* rotationDeg replaces the database runway axis */
#define AIRPORT_OVR_COLOR 0x10    /* color replaces the built-in display color (0.0.28) */
#define AIRPORT_OVR_ALL 0x1F

typedef struct {
    char icao[4];         /* built-in ident, NUL-padded (same as BuiltinAirport.icao) */
    uint8_t fields;       /* AIRPORT_OVR_* */
    uint8_t reserved;     /* 0 */
    uint16_t rotationDeg; /* physical axis 0-179 deg, valid with AIRPORT_OVR_ROTATION */
    int32_t latE5;        /* latitude  * 1e5, valid with AIRPORT_OVR_LAT */
    int32_t lonE5;        /* longitude * 1e5, valid with AIRPORT_OVR_LON */
    /* 0.0.28: display color 0xRRGGBB, valid with AIRPORT_OVR_COLOR - the same
     * AirportMarker.color a user-defined location stores and the renderers use.
     * Display only: selection, dedupe, counts and priority never read it. */
    uint32_t color;
} AirportBuiltinOverride;

/* How an active built-in airport is drawn, overrides applied. */
typedef struct {
    bool hidden;            /* listed but not drawn */
    bool hasAxis;           /* draw Directional along axisDeg */
    float axisDeg;          /* physical runway axis, 0-179 deg */
    uint8_t overrideFields; /* AIRPORT_OVR_* in effect; 0 = pure database default */
} AirportBuiltinView;

/* Same as Airports_GetActiveBuiltin plus the drawing view (hidden flag and
 * the exact axis, which may be a 1-degree override). view may be NULL. */
bool Airports_GetActiveBuiltinView(size_t index, AirportMarker *out, AirportBuiltinView *view);
/* The database values of one built-in airport (no override applied), and its
 * database axis (NAN when unknown). False if the ident is not in the database. */
bool Airports_GetBuiltinDefaults(const char *icao, AirportMarker *out, float *axisDegOut);
/* Stored overrides, in insertion order (including airports not in range now). */
size_t Airports_OverrideCount(void);
bool Airports_GetOverride(size_t index, AirportBuiltinOverride *out);
bool Airports_FindOverride(const char *icao, AirportBuiltinOverride *out);
/* Adds/replaces the override for ovr->icao (case-insensitive ident). Only the
 * fields in ovr->fields are kept; fields == 0 deletes the record (same as
 * reset). False for an ident not in the database, out-of-range values, a full
 * table (AIRPORT_OVERRIDE_MAX) or a storage error (state then unchanged). */
bool Airports_SetOverride(const AirportBuiltinOverride *ovr);
/* Reset to Default: deletes the record. True if nothing is stored afterwards. */
bool Airports_ResetOverride(const char *icao);

/* Optional details of the same entry (either pointer may be NULL). */
bool Airports_GetActiveBuiltinInfo(size_t index, uint8_t *tierOut, float *distanceKmOut);
/* Why in-range built-in airports are NOT in the last selection (for the
 * /airports explanation only; selection itself is unchanged):
 *   replacedByUser = in range, but a user location names the ident (dedupe);
 *   overCapacity   = in range and not replaced, but beyond AIRPORT_ACTIVE_MAX
 *                    minus the user-location count.
 * Airports outside the radar area are never counted. Either pointer may be NULL. */
void Airports_GetBuiltinSelectionStats(uint32_t *replacedByUser, uint32_t *overCapacity);
/* Number of airports in the compiled-in database. */
size_t Airports_BuiltinTotal(void);

/* True for every marker type this firmware understands. */
bool Airport_MarkerTypeValid(unsigned markerMode);
/* Short UI label: "Dot", "Directional", "H (heliport)", "Square". */
const char *Airport_MarkerTypeName(unsigned markerMode);

/* What both renderers (radar.c LVGL and the web SVG preview) actually draw
 * for a marker. DIRECTIONAL only when the runway text parses to an axis
 * (axisDegOut set); a directional marker without a usable runway draws the
 * plain DOT, exactly as before. Unknown types also fall back to DOT. */
AirportMarkerMode Airport_EffectiveShape(const AirportMarker *marker, float *axisDegOut);

/* Shared "H" geometry for the heliport marker: three line segments
 * (left bar, right bar, crossbar) as pixel offsets from the marker center,
 * fitting a diameter x diameter box. seg[i] = {x1, y1, x2, y2}. */
void Airport_HeliportSegments(int diameter, int seg[3][4]);

/* ---- shared directional-marker geometry ----
 *
 * The single authoritative runway-orientation calculation, used by both the
 * main radar (radar.c, LVGL draw) and the /airports SVG preview
 * (web_airports.c) so the two renderers can never disagree about which way a
 * runway marker points.
 *
 * A runway designator names one end of a physical strip (e.g. "09" is the
 * approach heading landing to the east); the reciprocal end ("27") names the
 * same physical strip from the other direction. Flight-radar-7 draws the
 * PHYSICAL AXIS, not a one-way heading, so 09 and 27 (and 09L/27L/09C/27C/
 * 09R/27R) must all resolve to the same axis. Runway numbers are true/
 * magnetic heading / 10, rounded to the nearest 10 degrees; the physical axis
 * is that heading modulo 180.
 *
 * Returns false (axisDegOut left unchanged) if runwayText is empty, not a
 * valid 1-36 runway number optionally followed by one L/C/R suffix, or
 * otherwise malformed. Callers MUST treat a false return as "no known
 * orientation" and fall back to the plain dot - never draw a guessed axis. */
bool Airport_ParseRunwayAxis(const char *runwayText, float *axisDegOut);

/* 0.0.32: the runway axis to DRAW, in display degrees (0 = screen up). One
 * rule for the radar and the /airports preview:
 *   - a 1-degree rotation override (view->overrideFields & AIRPORT_OVR_ROTATION)
 *     is an exact TRUE axis;
 *   - otherwise the axis comes from the runway designator (built-in database
 *     axis / user "Primary runway"), which is MAGNETIC: true = axis + declDeg;
 *   - display = true - rotationDeg (rotationDeg = declination when the radar is
 *     magnetic-up, 0 when true-up).
 * declDeg = 0 when no declination is known (designator then drawn as before).
 * view = NULL for user-defined locations. False when the marker is not drawn
 * Directional (no usable axis). Pure function. */
bool Airport_DisplayAxisDeg(const AirportMarker *marker, const AirportBuiltinView *view, float declDeg,
                            float rotationDeg, float *displayAxisOut);

/* Given a physical axis bearing (0=N/vertical, 90=E/horizontal, compass
 * convention, same as Radar_ProjectPosition/DrawAircraft's heading math) and
 * a total end-to-end length in pixels, returns the pixel offsets (from the
 * marker's center) of the two ends of the runway line. Pure geometry, no
 * drawing-API dependency, so both the LVGL renderer and the SVG preview can
 * call it and stay pixel-identical in orientation. */
void Airport_RunwayAxisOffsets(float axisDeg, float totalLength,
                               float *dx1, float *dy1, float *dx2, float *dy2);

/* Geometry constants for the directional marker "[=( )=]": a runway-axis
 * line through a small center dot, with a short perpendicular end-cap tick
 * at each end. Expressed as multiples of the marker's configured dot
 * diameter, exactly like the existing dot already scales with `diameter`, so
 * one set of numbers drives both renderers. */
#define AIRPORT_RUNWAY_LINE_LENGTH_FACTOR 2.2f /* total line length = diameter * this */
#define AIRPORT_RUNWAY_CAP_LENGTH_FACTOR 0.9f  /* end-cap tick length = diameter * this */
#define AIRPORT_RUNWAY_CENTER_DIAMETER_FACTOR 0.75f /* center dot diameter = diameter * this */
