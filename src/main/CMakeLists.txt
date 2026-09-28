# Collect all UI C files under src/main/ui/
file(GLOB_RECURSE SRC_UI "ui/*.c")

idf_component_register(
    SRCS 
        "main.c"
        "waveshare_rgb_lcd_port.c" 
        "lvgl_port.c"
        "bm8563_min.c"
        "opensky_client.c"
        "adsblol_client.c"
        "adsblol_category.c"
        "aircraft_provider.c"
        "craft_types.c"
        "custom_rules.c"
        "airports.c"
        "draw_aircraft.c"
        "webserver.c"
        "web_rules.c"
        "web_airports.c"
        "web_diag.c"
        "web_seen.c"
        "web_util.c"
        "seen_aircraft.c"
        "time_util.c"
        "radar.c"
        "universal_value.c"
        "tf_history.c"
        "history_manager.c"
        "adv_diag.c"
        "boot_warmup.c"
        "expert_debug.c"
        ${SRC_UI}
    INCLUDE_DIRS 
        "."
        "ui"
)

# Apply warning suppressions for LVGL if loaded
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
if(lvgl_lib)
    target_compile_options(${lvgl_lib} PRIVATE -Wno-format)

    # LVGL 64 KB built-in (TLSF) pool in PSRAM instead of the static internal
    # .bss array work_mem_int (components/fr_lvgl_pool). LVGL keeps its TLSF
    # allocator; only the one 64 KB block it manages moves. LV_MEM_CUSTOM and
    # LV_MEM_SIZE are unchanged. Only lv_mem.c uses these two macros.
    idf_component_get_property(fr_lv_pool_lib fr_lvgl_pool COMPONENT_LIB)
    target_compile_definitions(${lvgl_lib} PRIVATE
        "LV_MEM_POOL_INCLUDE=\"fr_lv_pool.h\""
        "LV_MEM_POOL_ALLOC=fr_lv_pool_alloc")
    target_link_libraries(${lvgl_lib} PUBLIC ${fr_lv_pool_lib})
endif()

# Advanced Diagnostics allocation-owner trace (adv_diag.c). Compile-time only:
# the wrappers sit on every malloc/heap_caps_* call, so they are linked in only
# when CONFIG_FR_ALLOC_TRACE=y (default n). Even then they record only while
# Advanced Diagnostics is enabled on /diag (takes effect after reboot).
if(CONFIG_FR_ALLOC_TRACE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=realloc" "-Wl,--wrap=free"
        "-Wl,--wrap=heap_caps_malloc" "-Wl,--wrap=heap_caps_calloc" "-Wl,--wrap=heap_caps_realloc"
        "-Wl,--wrap=heap_caps_free" "-Wl,--wrap=heap_caps_aligned_alloc" "-Wl,--wrap=heap_caps_aligned_calloc")
endif()
