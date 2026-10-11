#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* Shared WebUI look: one CSS block (CSS variables, light default, dark via
 * <html class='dk'>) and one navigation bar. Everything is streamed from
 * constant strings, so no page-sized buffer is needed. Navigation is plain
 * links and works without JavaScript; dark mode is pure CSS. */
typedef enum {
    WEBPAGE_NONE = 0,
    WEBPAGE_SETUP,
    WEBPAGE_CURRENT,
    WEBPAGE_REGISTERED,
    WEBPAGE_OPERATORS,
    WEBPAGE_SEEN,
    WEBPAGE_HISTORY
} WebPageId;

/* Sets the content type and sends doctype..<body> plus the nav bar.
 * title must be a plain (non-escaped-needed) string; extraCss may be NULL
 * (page-specific rules appended after the shared CSS, sent as-is). */
esp_err_t WebStyle_SendHead(httpd_req_t *req, const char *title, WebPageId page, const char *extraCss);
/* 0.1.4: the same, but the sticky <header class='sk'> (navigation + clock) is left
 * open so the page can add its own title/section links to the part that stays
 * visible while scrolling; the page MUST then send "</header>" itself. */
esp_err_t WebStyle_SendHeadOpen(httpd_req_t *req, const char *title, WebPageId page, const char *extraCss);

/* "Features & appearance" form for the Setup page (posts to /features). */
esp_err_t WebStyle_SendFeatureControls(httpd_req_t *req);

/* Registers POST /features. */
esp_err_t WebStyle_Register(httpd_handle_t server);

/* Shared help popup (0.0.28): a link that opens the <dialog> with dialogId
 * over the current page (no navigation; Close or Esc returns to the page as it
 * was), and the dialog itself. bodyHtml is static, trusted page text. */
esp_err_t WebStyle_SendHelpLink(httpd_req_t *req, const char *dialogId, const char *linkText);
esp_err_t WebStyle_SendHelpDialog(httpd_req_t *req, const char *dialogId, const char *title, const char *bodyHtml);

/* For the few small standalone pages built with snprintf (confirmation /
 * redirect pages): " class='dk'" when Dark Mode is on, else "". Pair with
 * WEBSTYLE_MINI_CSS inside their own <style>. */
const char *WebStyle_HtmlAttr(void);
/* 0.0.32: local time for the nav bar ("6:32 PM PDT"; "time not synced"). */
void WebStyle_FormatNowShort(char *out, size_t cap);
/* 0.1.3: same text for an explicit UTC instant ("time not synced" when unsynced). */
void WebStyle_FormatAt(int64_t utcSeconds, char *out, size_t cap);

/* 0.1.3 live nav clock: what the page embeds so the browser can keep the device's
 * local time current without contacting the device. False when the clock is not
 * synced (the nav then shows "time not synced" and no script). nextUtc = 0: no
 * offset change before validUntil. */
#define WEB_CLOCK_HORIZON_SEC (8LL * 24 * 3600)
typedef struct {
    int64_t utc;        /* device UTC time when the page was generated */
    int offMin;         /* UTC offset in effect then (DST included), minutes */
    char abbr[12];
    int64_t nextUtc;    /* first offset change after utc (0 = none within the horizon) */
    int nextOffMin;
    char nextAbbr[12];
    int64_t validUntil; /* the script stops updating here (horizon end or a second change) */
} WebClockData;
bool WebStyle_ClockData(int64_t nowUtc, WebClockData *out);
/* 0.0.32: called after the Setup display settings are saved (main.c: radar redraw). */
void WebStyle_SetAppearanceChangedHook(void (*hook)(void));
#define WEBSTYLE_MINI_CSS "html.dk{background:#14171a;color:#e4e6e8}html.dk a{color:#7fd37f}"
