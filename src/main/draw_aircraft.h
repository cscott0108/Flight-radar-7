#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "craft_types.h"

/* Draws one aircraft marker from an already-resolved appearance (see
 * ResolveAircraftAppearance()). Heading orients the triangle and diamond
 * markers and draws a short directional line inside the helicopter's ring;
 * an invalid/non-finite heading (see isfinite() checks in the .c file) is
 * never drawn as a false direction - the circle/diamond body still renders,
 * just without the directional cue. */
void DrawAircraft(lv_draw_ctx_t *draw_ctx, int x, int y,
                  float heading, const CraftAppearance *look, bool selected);
