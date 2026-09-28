# Project-local provider for LVGL's built-in (TLSF) memory pool.
# LVGL 8.4 lv_mem_init() calls LV_MEM_POOL_ALLOC(LV_MEM_SIZE) exactly once and
# builds its TLSF heap inside the returned block; see main/CMakeLists.txt for
# where LV_MEM_POOL_INCLUDE / LV_MEM_POOL_ALLOC are set on the LVGL target.
idf_component_register(SRCS "fr_lv_pool.c"
                       INCLUDE_DIRS "."
                       REQUIRES heap log esp_hw_support esp_system)
