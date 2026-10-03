# ---- Firmware build identifier -------------------------------------------
# src/VERSION is the single authoritative source. It is read here, at CMake
# configure time, and compiled into the firmware through a generated header
# (fw_version.h, FW_VERSION_STRING); /diag prints it. Nothing reads the file at
# runtime. A missing, empty or malformed file fails the build on purpose - there
# is no UNKNOWN / 0.0.0 / timestamp / git-hash fallback.
# Editing src/VERSION re-runs CMake (CMAKE_CONFIGURE_DEPENDS) and, because
# file(GENERATE) only rewrites the header when its content changes, rebuilds
# only the files that include it.
get_filename_component(FW_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../VERSION" ABSOLUTE)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FW_VERSION_FILE}")
    if(NOT EXISTS "${FW_VERSION_FILE}")
        message(FATAL_ERROR
            "Firmware version file not found: ${FW_VERSION_FILE}\n"
            "Create it as a one-line text file containing the build identifier, e.g.  07.03.12-TF1")
    endif()
    file(READ "${FW_VERSION_FILE}" FW_VERSION_RAW)
    string(STRIP "${FW_VERSION_RAW}" FW_VERSION_STR)
    if("${FW_VERSION_STR}" STREQUAL "")
        message(FATAL_ERROR
            "Firmware version file is empty: ${FW_VERSION_FILE}\n"
            "Put the build identifier on one line, e.g.  07.03.12-TF1")
    endif()
    # Opaque identifier: letters, digits, '.', '-', '_' and '+' only (also keeps it
    # safe inside a C string and a CMake generator expression). Not parsed.
    if(FW_VERSION_STR MATCHES "[^A-Za-z0-9._+-]")
        message(FATAL_ERROR
            "Firmware version \"${FW_VERSION_STR}\" in ${FW_VERSION_FILE} has unsupported characters.\n"
            "Use a single line of letters, digits, '.', '-', '_' or '+' (for example 07.03.12-TF1).")
    endif()
    string(LENGTH "${FW_VERSION_STR}" FW_VERSION_LEN)
    if(FW_VERSION_LEN GREATER 48)
        message(FATAL_ERROR "Firmware version in ${FW_VERSION_FILE} is ${FW_VERSION_LEN} characters; the limit is 48.")
    endif()
    set(FW_VERSION_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(GENERATE OUTPUT "${FW_VERSION_GEN_DIR}/fw_version.h"
        CONTENT "#pragma once\n/* Generated from src/VERSION by main/CMakeLists.txt - do not edit. */\n#define FW_VERSION_STRING \"${FW_VERSION_STR}\"\n")
    message(STATUS "Firmware build identifier (src/VERSION): ${FW_VERSION_STR}")
endif()

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
        "web_history.c"
        "web_util.c"
        "web_style.c"
        "feature_flags.c"
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

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${FW_VERSION_GEN_DIR}")
endif()

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
