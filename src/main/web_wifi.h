#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* GET /wifi (manage up to 5 saved networks) and POST /wifi (add / update / delete / connect).
 * Passwords are never rendered. Storage is wifi_profiles.c (NVS namespace "wifi"). */
esp_err_t WebWifi_Register(httpd_handle_t server);

/* One-line, HTML-escaped status for the Setup page ("Connected to X (saved network 2)"). */
void WebWifi_StatusLine(char *out, size_t cap);
