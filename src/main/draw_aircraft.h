#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "custom_rules.h"

void DrawAircraft(lv_draw_ctx_t *draw_ctx, int x, int y,
                  float heading, CustomType type, bool selected);
