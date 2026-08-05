#include "radar.h"
#include "main.h"

#include <math.h>
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

void Radar_ReconcileSelection(void)
{
    if (gAircraftCount <= 0)
    {
        selectedAircraft = -1;
        selectedIcao24[0] = '\0';
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

    UpdateSelectedAircraftUI();
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

static bool AircraftToRadar(
    Aircraft *a,
    int radiusPixels,
    int *x,
    int *y)
{
    float dx =
        a->predictedLon -
        radarCenterLon;

    float dy =
        a->predictedLat -
        radarCenterLat;

    float kmPerDegLat =
        111.0f;

    float kmPerDegLon =
        111.0f *
        cosf(
            radarCenterLat *
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

    if (distance > radarRadiusKm)
    {
        return false;
    }

    *x =
        (int)((eastKm / radarRadiusKm) *
              radiusPixels);

    *y =
        (int)((-northKm / radarRadiusKm) *
              radiusPixels);

    return true;
}

static void DrawAircraft(
    lv_draw_ctx_t *draw_ctx,
    int x,
    int y,
    float heading,
    bool selected)
{
    float h =
        heading *
        0.0174532925f;

    int size =
        selected ? 8 : 6;

    lv_color_t color =
        selected
            ? lv_palette_main(
                  LV_PALETTE_YELLOW)
            : lv_color_white();

    lv_point_t pts[3];

    pts[0].x =
        x +
        (int)(sinf(h) * size);

    pts[0].y =
        y -
        (int)(cosf(h) * size);

    pts[1].x =
        x +
        (int)(sinf(h + 2.5f) * size);

    pts[1].y =
        y -
        (int)(cosf(h + 2.5f) * size);

    pts[2].x =
        x +
        (int)(sinf(h - 2.5f) * size);

    pts[2].y =
        y -
        (int)(cosf(h - 2.5f) * size);

    lv_draw_line_dsc_t tri;

    lv_draw_line_dsc_init(
        &tri);

    tri.color = color;
    tri.width =
        selected ? 3 : 2;

    lv_draw_line(
        draw_ctx,
        &tri,
        &pts[0],
        &pts[1]);

    lv_draw_line(
        draw_ctx,
        &tri,
        &pts[1],
        &pts[2]);

    lv_draw_line(
        draw_ctx,
        &tri,
        &pts[2],
        &pts[0]);

    if (selected)
    {
        lv_draw_arc_dsc_t ring;

        lv_draw_arc_dsc_init(
            &ring);

        ring.color = color;
        ring.width = 2;

        lv_point_t center =
            {
                .x = x,
                .y = y};

        lv_draw_arc(
            draw_ctx,
            &ring,
            &center,
            size + 6,
            0,
            360);
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

        if (!AircraftToRadar(
                a,
                radius,
                &px,
                &py))
        {
            continue;
        }

        lv_draw_rect_dsc_t dot;

        lv_draw_rect_dsc_init(
            &dot);

        dot.bg_color =
            lv_color_white();

        dot.radius = 0;

        DrawAircraft(
            draw_ctx,
            cx + px,
            cy + py,
            a->heading,
            i == selectedAircraft);

        if (showAircraftLabels && strlen(a->callsign) > 0)
        {
            lv_draw_label_dsc_t label;

            lv_draw_label_dsc_init(&label);

            label.color =
                lv_palette_main(
                    LV_PALETTE_GREEN);

            label.font =
                &lv_font_montserrat_12;

            lv_area_t txt_area =
                {
                    .x1 = cx + px + 6,
                    .y1 = cy + py - 8,
                    .x2 = cx + px + 80,
                    .y2 = cy + py + 8};

            lv_draw_label(
                draw_ctx,
                &label,
                &txt_area,
                a->callsign,
                NULL);
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