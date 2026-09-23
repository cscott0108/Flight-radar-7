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

static float AircraftDistanceKm(const Aircraft *aircraft)
{
    // Use the dead-reckoned position so auto-select-closest tracks aircraft
    // between API polls rather than only at poll time.
    float eastKm = (aircraft->predictedLon - radarCenterLon) *
                   111.0f * cosf(radarCenterLat * 0.0174532925f);
    float northKm = (aircraft->predictedLat - radarCenterLat) * 111.0f;
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

bool Radar_ProjectPosition(float lat, float lon, float centerLat, float centerLon,
                           float radiusKm, int radiusPixels, int *x, int *y)
{
    if (!x || !y || !isfinite(lat) || !isfinite(lon) || !isfinite(centerLat) ||
        !isfinite(centerLon) || !isfinite(radiusKm) || radiusKm <= 0 || radiusPixels <= 0)
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

    float eastKm =
        dx * kmPerDegLon;

    float northKm =
        dy * kmPerDegLat;

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

    // Airports sit above the sweep and rings, but below aircraft icons.
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
        int half = airport.diameter / 2;
        lv_area_t dotArea = {
            .x1 = cx + px - half, .y1 = cy + py - half,
            .x2 = cx + px - half + airport.diameter - 1,
            .y2 = cy + py - half + airport.diameter - 1
        };
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = lv_color_hex(airport.color);
        dot.bg_opa = LV_OPA_COVER;
        dot.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(draw_ctx, &dot, &dotArea);
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