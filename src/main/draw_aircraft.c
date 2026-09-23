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

        /* Directional heading indicator: a short line from the center toward
         * the nose, using the radar's existing north-up heading convention
         * (see the triangle branch below) and the same color as the ring
         * that's already on screen. Only drawn for a finite heading, so a
         * missing/invalid reading never paints a false direction - the
         * circle and ring above still render normally either way. */
        if (isfinite(heading)) {
            const float h = heading * 0.0174532925f;
            const int lineRadius = diameter / 2 - look->ringWidthPx;
            if (lineRadius > 0) {
                lv_point_t center = {.x = x, .y = y};
                lv_point_t tip = {
                    .x = x + (int)(sinf(h) * lineRadius),
                    .y = y - (int)(cosf(h) * lineRadius)
                };
                lv_draw_line_dsc_t headingLine;
                lv_draw_line_dsc_init(&headingLine);
                headingLine.color = lv_color_hex(look->ringRgb);
                headingLine.width = 2;
                lv_draw_line(draw_ctx, &headingLine, &center, &tip);
            }
        }
    } else if (look->marker == CRAFT_MARKER_DIAMOND) {
        /* Other: a diamond built from two triangles sharing the nose/tail
         * axis, oriented along the same north-up heading convention as the
         * triangle marker below. A finite heading also gets a small gray/
         * black forward tip (OTHER_TIP_RGB - fixed, never classification-
         * specific) painted over the nose; without a valid heading the
         * diamond still renders, just pointed "up" and without the tip, so
         * it never implies a direction that isn't known. */
        const bool haveHeading = isfinite(heading);
        const float h = haveHeading ? heading * 0.0174532925f : 0.0f;
        const int size = look->sizePx;
        const float sideDist = size * 0.6f;

        const lv_point_t nose  = {x + (int)(sinf(h) * size),
                                   y - (int)(cosf(h) * size)};
        const lv_point_t right = {x + (int)(sinf(h + 1.5707963f) * sideDist),
                                   y - (int)(cosf(h + 1.5707963f) * sideDist)};
        const lv_point_t tail  = {x + (int)(sinf(h + 3.1415927f) * sideDist),
                                   y - (int)(cosf(h + 3.1415927f) * sideDist)};
        const lv_point_t left  = {x + (int)(sinf(h - 1.5707963f) * sideDist),
                                   y - (int)(cosf(h - 1.5707963f) * sideDist)};

        lv_draw_rect_dsc_t fill;
        lv_draw_rect_dsc_init(&fill);
        fill.bg_color = color;
        fill.bg_opa = LV_OPA_COVER;
        lv_point_t triNoseRightTail[3] = {nose, right, tail};
        lv_point_t triNoseTailLeft[3] = {nose, tail, left};
        lv_draw_triangle(draw_ctx, &fill, triNoseRightTail);
        lv_draw_triangle(draw_ctx, &fill, triNoseTailLeft);

        /* Diamond outline: the same craft-type border rule the fixed-wing
         * triangle uses (currently only Important), extended to this shape. */
        if (look->ringWidthPx > 0) {
            lv_draw_line_dsc_t outline;
            lv_draw_line_dsc_init(&outline);
            outline.color = lv_color_hex(look->ringRgb);
            outline.width = look->ringWidthPx;
            lv_draw_line(draw_ctx, &outline, &nose, &right);
            lv_draw_line(draw_ctx, &outline, &right, &tail);
            lv_draw_line(draw_ctx, &outline, &tail, &left);
            lv_draw_line(draw_ctx, &outline, &left, &nose);
        }

        if (haveHeading) {
            const float tipDist = size * 0.5f;
            const lv_point_t tipLeft  = {x + (int)(sinf(h + 1.0f) * tipDist),
                                          y - (int)(cosf(h + 1.0f) * tipDist)};
            const lv_point_t tipRight = {x + (int)(sinf(h - 1.0f) * tipDist),
                                          y - (int)(cosf(h - 1.0f) * tipDist)};
            lv_draw_rect_dsc_t tipFill;
            lv_draw_rect_dsc_init(&tipFill);
            tipFill.bg_color = lv_color_hex(OTHER_TIP_RGB);
            tipFill.bg_opa = LV_OPA_COVER;
            lv_point_t tipTri[3] = {nose, tipLeft, tipRight};
            lv_draw_triangle(draw_ctx, &tipFill, tipTri);
        }
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
