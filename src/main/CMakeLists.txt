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
        "radar.c"
        ${SRC_UI}
    INCLUDE_DIRS 
        "."
        "ui"
)

# Apply warning suppressions for LVGL if loaded
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
if(lvgl_lib)
    target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
endif()
