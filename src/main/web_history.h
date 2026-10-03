#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* GET /history - read-only view of the persistent TF history layer
 * (tf_history.c): every aircraft record stored in the on-card bucket files,
 * not just the recent/hot Seen list and not a diagnostics page.
 *
 *   /history                  browse all buckets, 50 rows per page
 *   /history?b=<fp>&o=<n>     continue from bucket <fp> (8 hex digits), record n
 *   /history?b=<fp>&only=1    one bucket only
 *   /history?q=<icao24>       exact aircraft lookup through the index
 *
 * Memory/IO discipline (see tf_history.h "Read-only browsing"): streamed
 * rows, no heap allocation, at most 8 records in RAM, the TF mutex is taken
 * for one short read at a time and never held while sending HTTP output.
 * The page never writes to the card. */
esp_err_t WebHistory_Register(httpd_handle_t server);
