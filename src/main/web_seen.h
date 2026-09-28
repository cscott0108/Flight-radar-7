#pragma once

#include "esp_http_server.h"

/* Registers /seen (history page), /seen/export (CSV download) and
 * /seen/clear (POST). */
esp_err_t WebSeen_Register(httpd_handle_t server);
