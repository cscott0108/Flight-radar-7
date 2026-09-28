#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_AIRPORTS 100
#define AIRPORT_NAME_LENGTH 24
#define AIRPORT_DEFAULT_DIAMETER 12
#define AIRPORT_DEFAULT_COLOR 0xFF0000 /* red, the original fixed airport color */

/* Longest accepted runway designator text, e.g. "36R" (2 digits + optional
 * L/C/R suffix). Stored exactly as entered; the physical axis is derived
 * live from it (see Airport_ParseRunwayAxis) rather than cached, so it can
 * never go stale relative to what's on screen. */
#define AIRPORT_RUNWAY_LENGTH 3

typedef enum {
    AIRPORT_MARKER_DOT = 0,        /* original fixed dot (always available) */
    AIRPORT_MARKER_DIRECTIONAL = 1 /* dot + runway-axis line, when orientation is known */
} AirportMarkerMode;

typedef struct {
    char name[AIRPORT_NAME_LENGTH + 1];
    float latitude;
    float longitude;
    uint8_t diameter;
    uint32_t color; /* 0xRRGGBB */
    uint8_t markerMode; /* AirportMarkerMode; dot is always the fallback */
    char runway[AIRPORT_RUNWAY_LENGTH + 1]; /* e.g. "09", "27L", "" = none/unknown */
} AirportMarker;

/* Call after nvs_flash_init(), before the web server or radar starts. */
bool Airports_Init(void);
size_t Airports_Count(void);
bool Airports_Get(size_t index, AirportMarker *out);
bool Airports_Save(int index, const AirportMarker *marker); /* -1 appends */
bool Airports_Delete(size_t index);

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
