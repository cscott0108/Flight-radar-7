#include "draw_aircraft.h"

#include <math.h>

void DrawAircraft(lv_draw_ctx_t *draw_ctx, int x, int y,
                  float heading, CustomType type, bool selected)
{
    const int size = type == TYPE_PRIVATE ? 5 : 10;
    const float h = heading * 0.0174532925f;
    lv_point_t points[3] = {
        {x + (int)(sinf(h) * size), y - (int)(cosf(h) * size)},
        {x + (int)(sinf(h + 2.5f) * size), y - (int)(cosf(h + 2.5f) * size)},
        {x + (int)(sinf(h - 2.5f) * size), y - (int)(cosf(h - 2.5f) * size)}
    };

    lv_color_t color;
    switch (type) {
    case TYPE_POLICE: color = lv_color_hex(0x238BFF); break;
    case TYPE_MILITARY: color = lv_color_hex(0x00D060); break;
    case TYPE_EMERGENCY: color = lv_color_hex(0xFF3030); break;
    case TYPE_COMMERCIAL: color = lv_color_hex(0xFF9800); break;
    case TYPE_PRIVATE:
    default: color = lv_color_white(); break;
    }

    if (type == TYPE_PRIVATE) {
        lv_draw_line_dsc_t outline;
        lv_draw_line_dsc_init(&outline);
        outline.color = color;
        outline.width = 2;
        lv_draw_line(draw_ctx, &outline, &points[0], &points[1]);
        lv_draw_line(draw_ctx, &outline, &points[1], &points[2]);
        lv_draw_line(draw_ctx, &outline, &points[2], &points[0]);
    } else {
        lv_draw_rect_dsc_t fill;
        lv_draw_rect_dsc_init(&fill);
        fill.bg_color = color;
        fill.bg_opa = LV_OPA_COVER;
        lv_draw_triangle(draw_ctx, &fill, points);
    }

    if (selected) {
        lv_draw_arc_dsc_t ring;
        lv_draw_arc_dsc_init(&ring);
        ring.color = lv_palette_main(LV_PALETTE_YELLOW);
        ring.width = 2;
        lv_point_t center = {.x = x, .y = y};
        lv_draw_arc(draw_ctx, &ring, &center, size + 6, 0, 360);
    }
}
