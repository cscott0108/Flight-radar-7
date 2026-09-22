#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "craft_types.h"

/* Draws one aircraft marker from an already-resolved appearance (see
 * ResolveAircraftAppearance()). The heading only matters for triangles; the
 * helicopter circle is non-directional. */
void DrawAircraft(lv_draw_ctx_t *draw_ctx, int x, int y,
                  float heading, const CraftAppearance *look, bool selected);
