#include "radar.h"
#include "main.h"
#include "draw_aircraft.h"
#include "airports.h"
#include "visibility_policy.h"
#include "auto_select.h"
#include "north_ref.h"
#include "ui_prefs.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static lv_obj_t *radarObject = NULL;

static float radarCenterLat = 12.9716f;
static float radarCenterLon = 77.5946f;
static float radarRadiusKm = 100.0f;

static float sweepAngle = 0.0f;

bool showAircraftLabels = true;

char selectedIcao24[16] = "";

int selectedAircraft = -1;
static bool autoSelectClosest = false;

/* Forward declaration: full definition (with the "one authoritative
 * geographic-offset calculation" doc comment) is below, next to
 * Radar_ProjectPosition and Radar_GeoBearingAndDistance, which share it. */
static bool Radar_GeoOffsetKm(float lat, float lon, float centerLat, float centerLon,
                              float *eastKmOut, float *northKmOut);

/* HOSTTEST:BEGIN sel (extracted verbatim by host_tests/radar_glue_test.c) */
static AutoSelectState autoState;
static AutoSelectCand autoCands[MAX_AIRCRAFT]; /* only touched with the LVGL lock held */

static uint32_t NowMs(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static float AircraftDistanceKm(const Aircraft *aircraft)
{
    // Use the dead-reckoned position so auto-select-closest tracks aircraft
    // between API polls rather than only at poll time. Shares the one
    // authoritative geo-offset calculation with Radar_ProjectPosition and
    // Radar_GeoBearingAndDistance (defined below in this file) instead of
    // keeping its own copy of the east/north-km math.
    float eastKm = 0.0f, northKm = 0.0f;
    Radar_GeoOffsetKm(aircraft->predictedLat, aircraft->predictedLon,
                      radarCenterLat, radarCenterLon, &eastKm, &northKm);
    return sqrtf(eastKm * eastKm + northKm * northKm);
}

/* Auto-select: classification, then radar ring, then distance (auto_select.c); ties rotate
 * by ICAO24, independent of the order of gAircraft[]. Returns an index into gAircraft[]. */
static int AutoSelectGet(void)
{
    int n = gAircraftCount;
    if (n > MAX_AIRCRAFT)
        n = MAX_AIRCRAFT;
    for (int i = 0; i < n; i++)
    {
        CraftType t = evaluateAircraftType(gAircraft[i].callsign, gAircraft[i].icao24);
        autoCands[i].icao24 = gAircraft[i].icao24;
        autoCands[i].cls = (t == CRAFT_IMPORTANT)     ? AUTOSEL_CLASS_IMPORTANT
                           : (t == CRAFT_INTERESTING) ? AUTOSEL_CLASS_INTERESTING
                                                      : AUTOSEL_CLASS_OTHER;
        autoCands[i].distKm = AircraftDistanceKm(&gAircraft[i]);
    }
    return AutoSelect_Choose(&autoState, autoCands, n, radarRadiusKm, NowMs());
}

static void ReconcileSelectionInner(void)
{
    if (gAircraftCount <= 0)
    {
        selectedAircraft = -1;
        selectedIcao24[0] = '\0';
        AutoSelect_Choose(&autoState, autoCands, 0, radarRadiusKm, NowMs());
        return;
    }

    if (autoSelectClosest)
    {
        int pick = AutoSelectGet();
        if (pick >= 0)
        {
            selectedAircraft = pick;
            strcpy(selectedIcao24, gAircraft[pick].icao24);
        }
        return;
    }

    // First aircraft ever loaded
    if (selectedIcao24[0] == '\0')
    {
        selectedAircraft = 0;

        strcpy(
            selectedIcao24,
            gAircraft[0].icao24);

        return;
    }

    // Try to find previously selected aircraft
    for (int i = 0; i < gAircraftCount; i++)
    {
        if (strcmp(
                gAircraft[i].icao24,
                selectedIcao24) == 0)
        {
            selectedAircraft = i;
            return;
        }
    }

    // Previously selected aircraft disappeared
    selectedAircraft = 0;

    strcpy(
        selectedIcao24,
        gAircraft[0].icao24);
}

/* Callers hold the LVGL lock. Whenever the selected identity changes (auto-select moving to another
 * aircraft, the old one leaving, ...) the Selected Craft panel is repainted here, so the panel can
 * never lag behind the highlight on the radar. */
void Radar_ReconcileSelection(void)
{
    char before[sizeof(selectedIcao24)];
    strcpy(before, selectedIcao24);
    ReconcileSelectionInner();
    if (strcmp(before, selectedIcao24) != 0)
        UpdateSelectedAircraftUI();
}

void Radar_NoteManualSelection(void)
{
    AutoSelect_NoteManual(&autoState, selectedIcao24, NowMs());
}
/* HOSTTEST:END sel */

Aircraft *Radar_GetSelectedAircraft(void)
{
    if (selectedAircraft < 0 ||
        selectedAircraft >= gAircraftCount)
    {
        return NULL;
    }

    return &gAircraft[selectedAircraft];
}

void Radar_SetAutoSelectClosest(bool enabled)
{
    autoSelectClosest = enabled;
    Radar_ReconcileSelection();
}

bool Radar_GetAutoSelectClosest(void)
{
    return autoSelectClosest;
}

void Radar_PredictAircraft(void)
{
    for (int i = 0;
         i < gAircraftCount;
         i++)
    {
        Aircraft *a =
            &gAircraft[i];

        // A stale (retained, no longer reported) aircraft stays at its last
        // reported position instead of being dead-reckoned onward.
        if (!a->valid || a->visState == AIRCRAFT_VIS_STALE)
            continue;

        uint32_t now =
            xTaskGetTickCount() *
            portTICK_PERIOD_MS;

        float dt =
            (now - a->lastUpdateMs) /
            1000.0f;

        a->lastUpdateMs = now;

        float speedKmh =
            a->velocity * 3.6f;

        float distanceKm =
            speedKmh * dt / 3600.0f;

        float headingRad =
            a->trackTrueDeg *
            0.0174532925f;

        float northKm =
            cosf(headingRad) *
            distanceKm;

        float eastKm =
            sinf(headingRad) *
            distanceKm;

        a->predictedLat +=
            northKm / 111.0f;

        float lonScale =
            111.0f *
            cosf(
                a->predictedLat *
                0.0174532925f);

        if (lonScale > 0.001f)
        {
            a->predictedLon +=
                eastKm / lonScale;
        }
    }
}

/* HOSTTEST:BEGIN idle (extracted verbatim by host_tests/display_idle_test.c) */
/* Zero-traffic display idle. The LCD being dimmed is a display/power-saving state only: polling,
 * tracking, the WebUI and every task keep running. While BOTH idle dim is active (main.c,
 * UpdateDayNightBrightness) AND there are zero aircraft, the 30 ms sweep stops advancing and stops
 * invalidating the 380x380 radar area, so LVGL renders nothing there. The aircraft count is read live
 * on every call, so the first tick/refresh after aircraft return redraws at once, independently of
 * the backlight's 250 ms wake path. Setters never touch LVGL: safe from any task. */
static volatile bool idleDimActive = false;
static volatile bool redrawPending = false; /* one frame wanted while frozen (e.g. range changed) */
static bool renderFrozen = false;            /* LVGL timer context only */

void Radar_SetIdleDimActive(bool active)
{
    idleDimActive = active;
}

bool Radar_IsDisplayIdle(void)
{
    return idleDimActive && gAircraftCount == 0;
}

void Radar_RequestRedraw(void)
{
    redrawPending = true;
}

/* LVGL timer context (30 ms). */
void Radar_SweepTick(void)
{
    if (Radar_IsDisplayIdle())
    {
        /* Render once, then freeze: the tick that enters the idle state draws one final frame of the
         * current (empty) radar - rings, labels, airports - and after that no sweep animation and no
         * repaint. A pending request (range/airport change) still gets exactly one frame. */
        if (!renderFrozen)
        {
            renderFrozen = true;
            redrawPending = true;
        }
        if (redrawPending && radarObject)
        {
            redrawPending = false;
            lv_obj_invalidate(radarObject);
        }
        return;
    }
    renderFrozen = false;  /* aircraft back (or idle dim ended): sweep resumes on this very tick */
    redrawPending = false; /* the normal invalidate below covers it */

    sweepAngle += 3.0f;

    if (sweepAngle >= 360.0f)
    {
        sweepAngle -= 360.0f;
    }

    if (radarObject)
    {
        lv_obj_invalidate(
            radarObject);
    }
}

/* Callers hold the LVGL lock. While display-idle there is nothing new to draw (zero aircraft), so this
 * is a no-op; with aircraft present (including the first poll after they return) it always repaints. */
void Radar_Refresh(void)
{
    if (Radar_IsDisplayIdle())
        return;

    if (radarObject)
    {
        lv_obj_invalidate(
            radarObject);
    }
}
/* HOSTTEST:END idle */

/* The one authoritative geographic-offset calculation (flat-earth
 * approximation, valid at radar ranges): east/north km from
 * (centerLat,centerLon) to (lat,lon). Radar_ProjectPosition (screen
 * position) and Radar_GeoBearingAndDistance (bearing/range, for the
 * off-screen indicator) both derive from this and nothing else, so the
 * coordinate system can never disagree with itself. */
/* HOSTTEST:BEGIN geo */
static bool Radar_GeoOffsetKm(float lat, float lon, float centerLat, float centerLon,
                              float *eastKmOut, float *northKmOut)
{
    if (!eastKmOut || !northKmOut || !isfinite(lat) || !isfinite(lon) ||
        !isfinite(centerLat) || !isfinite(centerLon))
        return false;

    float dx =
        lon - centerLon;

    float dy =
        lat - centerLat;

    float kmPerDegLat =
        111.0f;

    float kmPerDegLon =
        111.0f *
        cosf(
            centerLat *
            3.14159265f /
            180.0f);

    *eastKmOut = dx * kmPerDegLon;
    *northKmOut = dy * kmPerDegLat;
    return true;
}

/* HOSTTEST:END geo */

/* HOSTTEST:BEGIN proj (extracted verbatim by host_tests/locations_test.c) */
/* 0.0.32: the one display rotation. Geography is true; when the radar is
 * magnetic-up (north_ref.h) the screen is rotated so a point at true bearing B
 * appears at B - R (R = Radar_DisplayRotationDeg(), 0 when true-up). Every
 * screen position and display bearing goes through here. */
static void Radar_RotateToDisplay(float *eastKm, float *northKm)
{
    const float r = Radar_DisplayRotationDeg();
    if (r == 0.0f)
        return;
    const float rad = r * 0.0174532925f, c = cosf(rad), s = sinf(rad);
    const float e = *eastKm, n = *northKm;
    *eastKm = e * c - n * s;
    *northKm = n * c + e * s;
}

bool Radar_ProjectPosition(float lat, float lon, float centerLat, float centerLon,
                           float radiusKm, int radiusPixels, int *x, int *y)
{
    if (!x || !y || !isfinite(radiusKm) || radiusKm <= 0 || radiusPixels <= 0)
        return false;

    float eastKm, northKm;
    if (!Radar_GeoOffsetKm(lat, lon, centerLat, centerLon, &eastKm, &northKm))
        return false;
    Radar_RotateToDisplay(&eastKm, &northKm);

    float distance =
        sqrtf(
            eastKm * eastKm +
            northKm * northKm);

    if (distance > radiusKm)
    {
        return false;
    }

    *x =
        (int)((eastKm / radiusKm) *
              radiusPixels);

    *y =
        (int)((-northKm / radiusKm) *
              radiusPixels);

    return true;
}
/* HOSTTEST:END proj */

bool Radar_GeoBearingAndDistance(float lat, float lon, float centerLat, float centerLon,
                                 float *bearingDegOut, float *distanceKmOut)
{
    if (!bearingDegOut || !distanceKmOut)
        return false;

    float eastKm, northKm;
    if (!Radar_GeoOffsetKm(lat, lon, centerLat, centerLon, &eastKm, &northKm))
        return false;
    Radar_RotateToDisplay(&eastKm, &northKm); /* display bearing (off-screen indicator) */

    *distanceKmOut = sqrtf(eastKm * eastKm + northKm * northKm);

    /* Compass bearing (0=N, 90=E): atan2(east, north), same north-up
     * convention as DrawAircraft's heading math (x=sin(bearing)*r,
     * y=-cos(bearing)*r) and DrawCompassLabel, normalized to [0,360). */
    float bearing = atan2f(eastKm, northKm) * (180.0f / 3.14159265f);
    if (bearing < 0.0f)
        bearing += 360.0f;
    *bearingDegOut = bearing;
    return true;
}

/* Radar accent color (0.0.32, Setup > Features & appearance; default the green
 * this file always used). Rings, sweep, compass/range labels, call signs. */
static lv_color_t Radar_AccentColor(void)
{
    return lv_color_hex(UiPrefs_RadarAccentRgb());
}

float Radar_DisplayRotationDeg(void)
{
    return NorthRef_DisplayRotationDeg();
}

// Draws a two-digit compass heading label (e.g. "36" for North, meaning
// 360 degrees / 10) at the given true bearing, just inside the outer
// ring. Bearing follows compass convention (0=N, 90=E, 180=S, 270=W),
// which is converted here into the math convention (0=East, CCW) used by
// the rest of this file's trig, so it lines up with the sweep spokes.
static void DrawCompassLabel(
    lv_draw_ctx_t *draw_ctx,
    int cx,
    int cy,
    int radius,
    float bearingDeg,
    const char *text)
{
    float rad =
        (90.0f - bearingDeg) *
        0.0174532925f;

    int labelRadius = radius - 14;

    int x =
        cx +
        (int)(cosf(rad) * labelRadius);

    int y =
        cy -
        (int)(sinf(rad) * labelRadius);

    lv_draw_label_dsc_t label;

    lv_draw_label_dsc_init(&label);

    label.color =
        Radar_AccentColor();

    label.font = &lv_font_montserrat_12;
    label.align = LV_TEXT_ALIGN_CENTER;

    lv_area_t area =
        {
            .x1 = x - 12,
            .y1 = y - 7,
            .x2 = x + 12,
            .y2 = y + 7};

    lv_draw_label(
        draw_ctx,
        &label,
        &area,
        text,
        NULL);
}

/* 0.0.32: which north the ring's "36" points to, as RESOLVED (AUTO never shows
 * as "A"): up-arrow + M (magnetic) or T (true). 0.1.1: drawn in the upper-left
 * corner of the radar view (4 px inset from its square bounds, outside the
 * outer ring), no longer beside "36". */
static void DrawNorthRefIndicator(lv_draw_ctx_t *draw_ctx, int cx, int cy, int radius)
{
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.color = Radar_AccentColor();
    label.font = &lv_font_montserrat_12;
    label.align = LV_TEXT_ALIGN_LEFT;
    const int x1 = cx - radius + 4;
    const int y1 = cy - radius + 4;
    lv_area_t area = {.x1 = x1, .y1 = y1, .x2 = x1 + 32, .y2 = y1 + 14};
    lv_draw_label(draw_ctx, &label, &area,
                  NorthRef_Resolved() == NORTH_RESOLVED_MAGNETIC ? LV_SYMBOL_UP "M" : LV_SYMBOL_UP "T", NULL);
}

// Distance label for one range ring, placed a few pixels outside the
// ring itself along a fixed bearing (northeast, between the N and E
// compass labels) so all three read like a ruler from center to edge,
// the way real radar displays annotate their range rings - rather than
// scattering them around the circle where they'd clash with the
// compass letters or the sweep line.
static void DrawRangeLabel(
    lv_draw_ctx_t *draw_ctx,
    int cx,
    int cy,
    int ringRadius,
    const char *text)
{
    const float bearingDeg = 45.0f;

    float rad =
        (90.0f - bearingDeg) *
        0.0174532925f;

    int labelRadius = ringRadius + 4;

    int x =
        cx +
        (int)(cosf(rad) * labelRadius);

    int y =
        cy -
        (int)(sinf(rad) * labelRadius);

    lv_draw_label_dsc_t label;

    lv_draw_label_dsc_init(&label);

    label.color =
        Radar_AccentColor();

    label.font = &lv_font_montserrat_12;
    label.align = LV_TEXT_ALIGN_CENTER;

    lv_area_t area =
        {
            .x1 = x - 18,
            .y1 = y - 7,
            .x2 = x + 18,
            .y2 = y + 7};

    lv_draw_label(
        draw_ctx,
        &label,
        &area,
        text,
        NULL);
}

// LVGL's built-in Montserrat fonts only ship a regular weight (there is
// no bold variant compiled in), so "bold" text is faked by drawing the
// label twice, offset by one pixel horizontally. This thickens the
// strokes enough to read as bold at this size without needing a new
// font asset baked into the firmware.
static void DrawBoldLabel(
    lv_draw_ctx_t *draw_ctx,
    lv_draw_label_dsc_t *dsc,
    const lv_area_t *coords,
    const char *text)
{
    lv_draw_label(
        draw_ctx,
        dsc,
        coords,
        text,
        NULL);

    lv_area_t shifted = *coords;

    shifted.x1 += 1;
    shifted.x2 += 1;

    lv_draw_label(
        draw_ctx,
        dsc,
        &shifted,
        text,
        NULL);
}

// "Count only" airport mode: the number of on-ground aircraft at an airport
// (latest poll), drawn just above-right of the airport marker. Same label
// drawing as the compass/range labels; no aircraft rendering involved.
static void DrawAirportCountLabel(lv_draw_ctx_t *draw_ctx, int x, int y, unsigned count)
{
    char text[8];
    snprintf(text, sizeof(text), "%u", count);
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.color = lv_color_white();
    label.font = &lv_font_montserrat_12;
    label.align = LV_TEXT_ALIGN_LEFT;
    lv_area_t area = {.x1 = x + 6, .y1 = y - 16, .x2 = x + 36, .y2 = y - 2};
    lv_draw_label(draw_ctx, &label, &area, text, NULL);
}

// Draws one location marker (airport, heliport H, special-location square or
// generic dot) at the given screen position. Dot mode (or a
// directional marker whose runway text doesn't currently resolve to a valid
// axis - see Airport_ParseRunwayAxis) draws the original plain dot;
// otherwise draws the "[=( )=]"-style runway-axis marker: a short line along
// the runway's physical axis, a perpendicular end-cap tick at each end, and
// a small center dot, all in the airport's own configured color. Geometry
// (axis math, line/cap lengths) is shared with the /airports SVG preview via
// airports.h/.c - this function is only the LVGL-specific draw calls.
// axisOverrideDeg (may be NULL): exact runway axis for a Directional marker,
// used for built-in airports (database axis or a 1-degree user override).
static void DrawAirportMarker(
    lv_draw_ctx_t *draw_ctx,
    int cx,
    int cy,
    const AirportMarker *airport,
    const float *axisOverrideDeg)
{
    const lv_color_t color = lv_color_hex(airport->color);
    float axisDeg = 0.0f;
    const AirportMarkerMode shape = Airport_EffectiveShape(airport, &axisDeg);
    if (shape == AIRPORT_MARKER_DIRECTIONAL && axisOverrideDeg)
        axisDeg = *axisOverrideDeg;

    if (shape == AIRPORT_MARKER_HELIPORT)
    {
        // "H": geometry shared with the web preview (Airport_HeliportSegments).
        int seg[3][4];
        Airport_HeliportSegments(airport->diameter, seg);
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = color;
        line.width = 2;
        for (int i = 0; i < 3; i++)
        {
            const lv_point_t a = {cx + seg[i][0], cy + seg[i][1]};
            const lv_point_t b = {cx + seg[i][2], cy + seg[i][3]};
            lv_draw_line(draw_ctx, &line, &a, &b);
        }
        return;
    }

    int centerDiameter = airport->diameter;
    if (shape == AIRPORT_MARKER_DIRECTIONAL)
    {
        float dx1, dy1, dx2, dy2;
        const float lineLength = airport->diameter * AIRPORT_RUNWAY_LINE_LENGTH_FACTOR;
        Airport_RunwayAxisOffsets(axisDeg, lineLength, &dx1, &dy1, &dx2, &dy2);

        const lv_point_t end1 = {cx + (int)lroundf(dx1), cy + (int)lroundf(dy1)};
        const lv_point_t end2 = {cx + (int)lroundf(dx2), cy + (int)lroundf(dy2)};

        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = color;
        line.width = 2;
        lv_draw_line(draw_ctx, &line, &end1, &end2);

        // End-cap ticks: short segments perpendicular to the axis (+90 deg),
        // centered on each runway end - the "[" and "]" of "[=( )=]".
        float cdx, cdy, unusedX, unusedY;
        const float capLength = airport->diameter * AIRPORT_RUNWAY_CAP_LENGTH_FACTOR;
        Airport_RunwayAxisOffsets(axisDeg + 90.0f, capLength, &cdx, &cdy, &unusedX, &unusedY);

        const lv_point_t cap1a = {end1.x - (int)lroundf(cdx), end1.y - (int)lroundf(cdy)};
        const lv_point_t cap1b = {end1.x + (int)lroundf(cdx), end1.y + (int)lroundf(cdy)};
        lv_draw_line(draw_ctx, &line, &cap1a, &cap1b);

        const lv_point_t cap2a = {end2.x - (int)lroundf(cdx), end2.y - (int)lroundf(cdy)};
        const lv_point_t cap2b = {end2.x + (int)lroundf(cdx), end2.y + (int)lroundf(cdy)};
        lv_draw_line(draw_ctx, &line, &cap2a, &cap2b);

        centerDiameter = (int)(airport->diameter * AIRPORT_RUNWAY_CENTER_DIAMETER_FACTOR);
        if (centerDiameter < 4)
            centerDiameter = 4;
    }

    const int half = centerDiameter / 2;
    lv_area_t dotArea = {
        .x1 = cx - half, .y1 = cy - half,
        .x2 = cx - half + centerDiameter - 1,
        .y2 = cy - half + centerDiameter - 1
    };
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = color;
    dot.bg_opa = LV_OPA_COVER;
    // Square = special aviation-interest location (contextual only); every
    // other shape keeps the original round dot.
    dot.radius = (shape == AIRPORT_MARKER_SQUARE) ? 0 : LV_RADIUS_CIRCLE;
    lv_draw_rect(draw_ctx, &dot, &dotArea);
}

// Off-screen aircraft boundary indicators (PHASE 6). Bounded so a burst of
// out-of-range traffic can't grow this LVGL-draw-callback's stack usage
// unpredictably (the LVGL task runs on a deliberately small 6 KB stack - see
// PROJECT_STATE.md capacity audit); any indicators beyond this are simply
// not drawn; nothing else about the radar is affected.
#define MAX_OFFSCREEN_INDICATORS 24

typedef struct
{
    float bearingDeg;
    uint32_t colorRgb;
} OffscreenIndicator;

// Small triangular "blip" on the outer ring, pointing outward, at the given
// true bearing from radar center. Position is geographic-bearing-only (never
// aircraft heading, per PHASE 6) and uses the same Ring 3 pixel radius the
// rings/sweep/on-screen aircraft already use - so it moves correctly with
// radar range without any range value ever being hard-coded here.
static void DrawOffscreenIndicator(
    lv_draw_ctx_t *draw_ctx,
    int cx,
    int cy,
    int ring3RadiusPx,
    float bearingDeg,
    uint32_t colorRgb)
{
    const float rad = bearingDeg * 0.0174532925f;
    const float ux = sinf(rad);  // outward unit vector, north-up convention
    const float uy = -cosf(rad);
    const float px = -uy; // perpendicular unit vector
    const float py = ux;

    const float apexR = ring3RadiusPx + 7.0f;
    const float baseR = ring3RadiusPx - 3.0f;
    const float halfWidth = 4.0f;

    const lv_point_t apex = {
        cx + (int)lroundf(ux * apexR),
        cy + (int)lroundf(uy * apexR)};
    const lv_point_t base1 = {
        cx + (int)lroundf(ux * baseR + px * halfWidth),
        cy + (int)lroundf(uy * baseR + py * halfWidth)};
    const lv_point_t base2 = {
        cx + (int)lroundf(ux * baseR - px * halfWidth),
        cy + (int)lroundf(uy * baseR - py * halfWidth)};

    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = lv_color_hex(colorRgb);
    fill.bg_opa = LV_OPA_COVER;
    lv_point_t tri[3] = {apex, base1, base2};
    lv_draw_triangle(draw_ctx, &fill, tri);
}

// Ascending-bearing insertion sort followed by a single forward pass that
// nudges any indicator less than minGapDeg past its predecessor - a simple,
// bounded separation pass ("do not over-engineer" per PROJECT_STATE.md), not
// a clustering/grouping algorithm. count is always <= MAX_OFFSCREEN_INDICATORS.
static void SeparateOffscreenIndicators(OffscreenIndicator *indicators, int count)
{
    const float minGapDeg = 3.5f;

    for (int i = 1; i < count; i++)
    {
        OffscreenIndicator key = indicators[i];
        int j = i - 1;
        while (j >= 0 && indicators[j].bearingDeg > key.bearingDeg)
        {
            indicators[j + 1] = indicators[j];
            j--;
        }
        indicators[j + 1] = key;
    }

    for (int i = 1; i < count; i++)
    {
        if (indicators[i].bearingDeg - indicators[i - 1].bearingDeg < minGapDeg)
            indicators[i].bearingDeg = indicators[i - 1].bearingDeg + minGapDeg;
    }
}

static void radar_draw_cb(
    lv_event_t *e)
{
    lv_obj_t *obj =
        lv_event_get_target(e);

    lv_draw_ctx_t *draw_ctx =
        lv_event_get_draw_ctx(e);

    lv_area_t area;

    lv_obj_get_content_coords(
        obj,
        &area);

    int width =
        area.x2 - area.x1;

    int height =
        area.y2 - area.y1;

    int cx =
        area.x1 + width / 2;

    int cy =
        area.y1 + height / 2;

    int radius =
        LV_MIN(
            width,
            height) /
        2;

    lv_draw_arc_dsc_t arc;

    lv_draw_arc_dsc_init(
        &arc);

    arc.color =
        Radar_AccentColor();

    arc.width = 2;

    lv_point_t center =
        {
            .x = cx,
            .y = cy};

    lv_draw_arc(
        draw_ctx,
        &arc,
        &center,
        radius,
        0,
        360);
    lv_draw_arc(
        draw_ctx,
        &arc,
        &center,
        radius * 2 / 3,
        0,
        360);

    lv_draw_arc(
        draw_ctx,
        &arc,
        &center,
        radius / 3,
        0,
        360);

    // Compass headings, so it's clear which way the radar is pointing:
    // N=36, E=09, S=18, W=27 (true bearing / 10, matching aviation
    // heading-tape notation).
    DrawCompassLabel(draw_ctx, cx, cy, radius, 0.0f, "36");
    DrawNorthRefIndicator(draw_ctx, cx, cy, radius);
    DrawCompassLabel(draw_ctx, cx, cy, radius, 90.0f, "09");
    DrawCompassLabel(draw_ctx, cx, cy, radius, 180.0f, "18");
    DrawCompassLabel(draw_ctx, cx, cy, radius, 270.0f, "27");

    // Range-ring distance labels. The three rings are always drawn at
    // 1/3, 2/3, and 3/3 (the full configured range) of the radar's
    // radius - see the three lv_draw_arc calls above - so the labels
    // are computed live from radarRadiusKm rather than hard-coded,
    // and stay correct for whatever range the user has set.
    char innerKmText[16];
    char middleKmText[16];
    char outerKmText[16];

    snprintf(innerKmText, sizeof(innerKmText), "%.0f km", radarRadiusKm / 3.0f);
    snprintf(middleKmText, sizeof(middleKmText), "%.0f km", radarRadiusKm * 2.0f / 3.0f);
    snprintf(outerKmText, sizeof(outerKmText), "%.0f km", radarRadiusKm);

    DrawRangeLabel(draw_ctx, cx, cy, radius / 3, innerKmText);
    DrawRangeLabel(draw_ctx, cx, cy, radius * 2 / 3, middleKmText);
    DrawRangeLabel(draw_ctx, cx, cy, radius, outerKmText);

    lv_draw_line_dsc_t line;

    lv_draw_line_dsc_init(
        &line);

    line.color =
        Radar_AccentColor();

    line.width = 2;

    for (int i = 0; i < 12; i++)
    {
        float angle =
            sweepAngle -
            (i * 4);

        while (angle < 0)
        {
            angle += 360;
        }

        float rad =
            angle *
            0.0174532925f;

        int x2 =
            cx +
            (int)(cosf(rad) * radius);

        int y2 =
            cy -
            (int)(sinf(rad) * radius);

        lv_draw_line_dsc_t d;

        lv_draw_line_dsc_init(
            &d);

        d.color =
            Radar_AccentColor();

        d.width = 1;

        d.opa =
            255 -
            (i * 20);

        lv_point_t p1 =
            {
                .x = cx,
                .y = cy};

        lv_point_t p2 =
            {
                .x = x2,
                .y = y2};

        lv_draw_line(
            draw_ctx,
            &d,
            &p1,
            &p2);
    }

    // Off-screen aircraft boundary indicators (PHASE 6): known aircraft
    // beyond Ring 3, placed purely by geographic bearing from radar center
    // (never aircraft heading), on Ring 3 itself. Drawn above the sweep/
    // rings but below airports and on-screen aircraft. Uses the same
    // gAircraft[] tracking/validity state and the same Ring 3 geographic
    // radius (radarRadiusKm) and pixel radius (radius) as everything else -
    // no second aircraft list, no hard-coded range.
    {
        static OffscreenIndicator indicators[MAX_OFFSCREEN_INDICATORS];
        int indicatorCount = 0;

        for (int i = 0; i < gAircraftCount && indicatorCount < MAX_OFFSCREEN_INDICATORS; i++)
        {
            Aircraft *a = &gAircraft[i];
            if (!a->valid)
                continue;

            float bearingDeg, distanceKm;
            if (!Radar_GeoBearingAndDistance(a->predictedLat, a->predictedLon,
                                             radarCenterLat, radarCenterLon,
                                             &bearingDeg, &distanceKm))
                continue; // non-finite predicted position: never guess

            if (distanceKm <= radarRadiusKm)
                continue; // inside Ring 3: the normal marker below covers it

            const CraftAppearance look =
                ResolveAircraftAppearanceWithHint(a->callsign, a->icao24,
                                                   a->providerTypeHint, a->hasProviderTypeHint);
            indicators[indicatorCount].bearingDeg = bearingDeg;
            indicators[indicatorCount].colorRgb = look.colorRgb;
            indicatorCount++;
        }

        SeparateOffscreenIndicators(indicators, indicatorCount);

        for (int i = 0; i < indicatorCount; i++)
        {
            DrawOffscreenIndicator(draw_ctx, cx, cy, radius,
                                   indicators[i].bearingDeg, indicators[i].colorRgb);
        }
    }

    // Locations sit above the sweep, rings and off-screen indicators, but
    // below aircraft icons. Built-in airports (regionally selected for the
    // current center/range, see Airports_SelectBuiltins) are drawn first so
    // user-defined locations always sit on top of them.
    /* 0.0.32: runway axes in display degrees (designator = magnetic -> true -> display). */
    float declDeg = 0.0f;
    (void)NorthRef_Declination(&declDeg);
    const float rotDeg = Radar_DisplayRotationDeg();
    size_t builtinCount = Airports_SelectBuiltins(radarCenterLat, radarCenterLon, radarRadiusKm);
    for (size_t i = 0; i < builtinCount; i++)
    {
        AirportMarker airport;
        AirportBuiltinView view;
        int px, py;
        // Overrides are already applied: hidden ones stay selected but are
        // not drawn; position and rotation overrides replace the defaults.
        if (!Airports_GetActiveBuiltinView(i, &airport, &view) || view.hidden ||
            !Radar_ProjectPosition(airport.latitude, airport.longitude,
                                   radarCenterLat, radarCenterLon,
                                   radarRadiusKm, radius, &px, &py))
            continue;
        float axis;
        DrawAirportMarker(draw_ctx, cx + px, cy + py, &airport,
                          Airport_DisplayAxisDeg(&airport, &view, declDeg, rotDeg, &axis) ? &axis : NULL);
    }

    size_t airportCount = Airports_Count();
    for (size_t i = 0; i < airportCount; i++)
    {
        AirportMarker airport;
        int px, py;
        if (!Airports_Get(i, &airport) ||
            !Radar_ProjectPosition(airport.latitude, airport.longitude,
                                   radarCenterLat, radarCenterLon,
                                   radarRadiusKm, radius, &px, &py))
            continue;
        float axis;
        DrawAirportMarker(draw_ctx, cx + px, cy + py, &airport,
                          Airport_DisplayAxisDeg(&airport, NULL, declDeg, rotDeg, &axis) ? &axis : NULL);
    }

    // Airport aircraft "Count only": per-airport on-ground counts from the
    // latest poll (visibility_policy.c), drawn next to the airport.
    {
        VisSettings vis;
        VisPolicy_GetSettings(&vis);
        if (vis.airportMode == VIS_AIRPORT_COUNT)
        {
            size_t n = VisPolicy_AirportCountTotal();
            for (size_t i = 0; i < n; i++)
            {
                VisAirportCount c;
                int px, py;
                if (!VisPolicy_GetAirportCount(i, &c) || c.associated == 0 ||
                    !Radar_ProjectPosition(c.latitude, c.longitude,
                                           radarCenterLat, radarCenterLon,
                                           radarRadiusKm, radius, &px, &py))
                    continue;
                DrawAirportCountLabel(draw_ctx, cx + px, cy + py, c.associated);
            }
        }
    }

    for (int i = 0;
         i < gAircraftCount;
         i++)
    {
        Aircraft *a =
            &gAircraft[i];

        if (!a->valid)
        {
            continue;
        }

        int px;
        int py;

        if (!Radar_ProjectPosition(a->predictedLat, a->predictedLon,
                                   radarCenterLat, radarCenterLon,
                                   radarRadiusKm, radius, &px, &py))
        {
            continue;
        }

        /* One decision point: classification -> color, aircraft type -> shape
         * (registry override > provider hint > Fixed-Wing default). */
        const CraftAppearance look =
            ResolveAircraftAppearanceWithHint(a->callsign, a->icao24,
                                               a->providerTypeHint, a->hasProviderTypeHint);

        DrawAircraft(
            draw_ctx,
            cx + px,
            cy + py,
            NorthRef_TrueToDisplay(a->trackTrueDeg), /* raw true track, drawn in the display reference */
            &look,
            i == selectedAircraft);

        if (showAircraftLabels && strlen(a->callsign) > 0)
        {
            lv_draw_label_dsc_t label;

            lv_draw_label_dsc_init(&label);

            label.color =
                Radar_AccentColor();

            label.font =
                &lv_font_montserrat_14;

            lv_area_t txt_area =
                {
                    .x1 = cx + px + 6,
                    .y1 = cy + py - 8,
                    .x2 = cx + px + 80,
                    .y2 = cy + py + 8};

            DrawBoldLabel(
                draw_ctx,
                &label,
                &txt_area,
                a->callsign);
        }
    }
}

void Radar_AttachToObject(
    lv_obj_t *obj)
{
    radarObject = obj;

    lv_obj_add_event_cb(
        radarObject,
        radar_draw_cb,
        LV_EVENT_DRAW_MAIN,
        NULL);
}

void Radar_SetCenter(
    float lat,
    float lon,
    float radiusKm)
{
    radarCenterLat = lat;
    radarCenterLon = lon;
    radarRadiusKm = radiusKm;
    Radar_RequestRedraw(); /* range labels/airports must update even while the sweep is frozen */
}
