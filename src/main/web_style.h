#pragma once
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
#define WEBSTYLE_MINI_CSS "html.dk{background:#14171a;color:#e4e6e8}html.dk a{color:#7fd37f}"
