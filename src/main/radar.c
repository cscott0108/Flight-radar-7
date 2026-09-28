#include "radar.h"
#include "main.h"
#include "draw_aircraft.h"
#include "airports.h"

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

void Radar_ReconcileSelection(void)
{
    if (gAircraftCount <= 0)
    {
        selectedAircraft = -1;
        selectedIcao24[0] = '\0';
        return;
    }

    if (autoSelectClosest)
    {
        int closest = 0;
        float closestDistance = AircraftDistanceKm(&gAircraft[0]);
        for (int i = 1; i < gAircraftCount; i++)
        {
            float distance = AircraftDistanceKm(&gAircraft[i]);
            if (distance < closestDistance)
            {
                closest = i;
                closestDistance = distance;
            }
        }

        selectedAircraft = closest;
        strcpy(selectedIcao24, gAircraft[closest].icao24);
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

        if (!a->valid)
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
            a->heading *
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

void Radar_SweepTick(void)
{
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

/* The one authoritative geographic-offset calculation (flat-earth
 * approximation, valid at radar ranges): east/north km from
 * (centerLat,centerLon) to (lat,lon). Radar_ProjectPosition (screen
 * position) and Radar_GeoBearingAndDistance (bearing/range, for the
 * off-screen indicator) both derive from this and nothing else, so the
 * coordinate system can never disagree with itself. */
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

bool Radar_ProjectPosition(float lat, float lon, float centerLat, float centerLon,
                           float radiusKm, int radiusPixels, int *x, int *y)
{
    if (!x || !y || !isfinite(radiusKm) || radiusKm <= 0 || radiusPixels <= 0)
        return false;

    float eastKm, northKm;
    if (!Radar_GeoOffsetKm(lat, lon, centerLat, centerLon, &eastKm, &northKm))
        return false;

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

bool Radar_GeoBearingAndDistance(float lat, float lon, float centerLat, float centerLon,
                                 float *bearingDegOut, float *distanceKmOut)
{
    if (!bearingDegOut || !distanceKmOut)
        return false;

    float eastKm, northKm;
    if (!Radar_GeoOffsetKm(lat, lon, centerLat, centerLon, &eastKm, &northKm))
        return false;

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
        lv_palette_main(
            LV_PALETTE_GREEN);

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
        lv_palette_main(
            LV_PALETTE_GREEN);

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

// Draws one airport marker at the given screen position. Dot mode (or a
// directional marker whose runway text doesn't currently resolve to a valid
// axis - see Airport_ParseRunwayAxis) draws the original plain dot;
// otherwise draws the "[=( )=]"-style runway-axis marker: a short line along
// the runway's physical axis, a perpendicular end-cap tick at each end, and
// a small center dot, all in the airport's own configured color. Geometry
// (axis math, line/cap lengths) is shared with the /airports SVG preview via
// airports.h/.c - this function is only the LVGL-specific draw calls.
static void DrawAirportMarker(
    lv_draw_ctx_t *draw_ctx,
    int cx,
    int cy,
    const AirportMarker *airport)
{
    const lv_color_t color = lv_color_hex(airport->color);
    float axisDeg;
    const bool directional =
        airport->markerMode == AIRPORT_MARKER_DIRECTIONAL &&
        Airport_ParseRunwayAxis(airport->runway, &axisDeg);

    int centerDiameter = airport->diameter;
    if (directional)
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
    dot.radius = LV_RADIUS_CIRCLE;
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
        lv_palette_main(
            LV_PALETTE_GREEN);

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
        lv_palette_main(
            LV_PALETTE_GREEN);

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
            lv_palette_main(
                LV_PALETTE_GREEN);

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

    // Airports sit above the sweep, rings and off-screen indicators, but
    // below aircraft icons.
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
        DrawAirportMarker(draw_ctx, cx + px, cy + py, &airport);
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
            a->heading,
            &look,
            i == selectedAircraft);

        if (showAircraftLabels && strlen(a->callsign) > 0)
        {
            lv_draw_label_dsc_t label;

            lv_draw_label_dsc_init(&label);

            label.color =
                lv_palette_main(
                    LV_PALETTE_GREEN);

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
}

void Radar_Refresh(void)
{
    if (radarObject)
    {
        lv_obj_invalidate(
            radarObject);
    }
}