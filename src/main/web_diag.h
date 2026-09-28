#pragma once

#include "esp_http_server.h"

/* PHASE 1/13 lightweight runtime capacity page: GET /diag. Read-only, no
 * persistence, no polling of its own - every value is read live from
 * existing ESP-IDF accessors or existing project counters (main.c,
 * airports.c, seen_aircraft.c, custom_rules.c) at request time, the same
 * pattern web_rules.c's LogHttpdMemory already uses for its own heap
 * logging, just surfaced to the browser instead of only the serial log.
 * See PROJECT_STATE.md "Runtime diagnostics" for what to copy into a future
 * Claude session when more capacity headroom is needed. */
esp_err_t WebDiag_Register(httpd_handle_t server);
