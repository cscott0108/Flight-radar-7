#include "draw_aircraft.h"

#include <math.h>

void DrawAircraft(lv_draw_ctx_t *draw_ctx, int x, int y,
                  float heading, const CraftAppearance *look, bool selected)
{
    /* Shape, size, fill and ring all come from the resolved appearance
     * (classification -> color, aircraft type -> shape). */
    const lv_color_t color = lv_color_hex(look->colorRgb);

    if (look->marker == CRAFT_MARKER_SOLID_CIRCLE) {
        /* Helicopter: solid disc, sizePx across, with a band inside its edge
         * (LVGL 8 draws borders inside the area, so the outer size stays put). */
        const int diameter = look->sizePx;
        const int left = x - diameter / 2;
        const int top = y - diameter / 2;
        lv_area_t area = {
            .x1 = left, .y1 = top,
            .x2 = left + diameter - 1, .y2 = top + diameter - 1
        };
        lv_draw_rect_dsc_t disc;
        lv_draw_rect_dsc_init(&disc);
        disc.bg_color = color;
        disc.bg_opa = LV_OPA_COVER;
        disc.radius = LV_RADIUS_CIRCLE;
        if (look->ringWidthPx > 0) {
            disc.border_color = lv_color_hex(look->ringRgb);
            disc.border_width = look->ringWidthPx;
            disc.border_opa = LV_OPA_COVER;
        }
        lv_draw_rect(draw_ctx, &disc, &area);
    } else {
        const int size = look->sizePx;
        const float h = heading * 0.0174532925f;
        lv_point_t points[3] = {
            {x + (int)(sinf(h) * size), y - (int)(cosf(h) * size)},
            {x + (int)(sinf(h + 2.5f) * size), y - (int)(cosf(h + 2.5f) * size)},
            {x + (int)(sinf(h - 2.5f) * size), y - (int)(cosf(h - 2.5f) * size)}
        };

        if (look->marker == CRAFT_MARKER_SMALL_OUTLINE) {
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

            /* LVGL 8's triangle draw has no border of its own, so a craft
             * type that defines one (currently only Important: yellow fill,
             * red border) gets the closest visually-consistent alternative -
             * the same three-edge outline technique used for Personal's
             * small-outline marker, drawn on top of the fill. */
            if (look->ringWidthPx > 0) {
                lv_draw_line_dsc_t outline;
                lv_draw_line_dsc_init(&outline);
                outline.color = lv_color_hex(look->ringRgb);
                outline.width = look->ringWidthPx;
                lv_draw_line(draw_ctx, &outline, &points[0], &points[1]);
                lv_draw_line(draw_ctx, &outline, &points[1], &points[2]);
                lv_draw_line(draw_ctx, &outline, &points[2], &points[0]);
            }
        }
    }

    if (selected) {
        lv_draw_arc_dsc_t ring;
        lv_draw_arc_dsc_init(&ring);
        ring.color = lv_palette_main(LV_PALETTE_YELLOW);
        ring.width = 2;
        lv_point_t center = {.x = x, .y = y};
        lv_draw_arc(draw_ctx, &ring, &center, look->selectRadiusPx, 0, 360);
    }
}
