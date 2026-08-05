
file(GLOB_RECURSE SRC_UI ${CMAKE_SOURCE_DIR} "ui/*.c")



idf_component_register(
    SRCS 
    "waveshare_rgb_lcd_port.c" 
    "main.c" 
    "lvgl_port.c"
    "bm8563_min.c"
    "opensky_client.c"
    "webserver.c"
    "radar.c"
    SRCS ${app_sources}
    ${SRC_UI}
    INCLUDE_DIRS 
    "."
    "./ui")

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
