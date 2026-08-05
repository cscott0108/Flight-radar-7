#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Auto-generated top-down aircraft silhouette, 32x32px, nose pointing
 * up (north) so lv_img_set_angle(icon, (int16_t)(heading_deg * 10)) rotates
 * it directly to match a compass heading (clockwise from north), with no
 * extra trig needed at the call site.
 *
 * Format: LV_IMG_CF_TRUE_COLOR_ALPHA, generated for LV_COLOR_DEPTH == 16
 * (RGB565 + 8-bit alpha, 3 bytes/px). If your project uses a different
 * LV_COLOR_DEPTH, regenerate from plane_icon_preview.png with LVGL's
 * official converter instead: https://lvgl.io/tools/imageconverter
 */
extern const lv_img_dsc_t plane_icon;

#ifdef __cplusplus
}
#endif
