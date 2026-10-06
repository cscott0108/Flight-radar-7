#include "web_style.h"

#include <stdio.h>
#include <string.h>

#include "feature_flags.h"
#include "tf_history.h"

/* Shared stylesheet. Colors are variables on :root (light, the original look)
 * and are redefined under html.dk. Sent with httpd_resp_send_chunk, so '%' is
 * not special here. */
static const char kCss[] =
    ":root{--bg:#fff;--fg:#222;--mut:#666;--bd:#ccc;--th:#eee;--card:#f7f7f7;--lnk:#1f6f1f;"
    "--acc:#4CAF50;--inb:#fff;--btn:#f0f0f0;--okb:#e6f4ea;--okbd:#8c8;--wb:#fff4d6;--wbd:#e0b040;"
    "--er:#b00;--erb:#fdecea}"
    "html.dk{--bg:#14171a;--fg:#e4e6e8;--mut:#9aa3ab;--bd:#3a4047;--th:#232930;--card:#1b2026;"
    "--lnk:#7fd37f;--inb:#0f1215;--btn:#2a3038;--okb:#183222;--okbd:#2f6b45;--wb:#3a3114;"
    "--wbd:#8a7424;--er:#ff8a80;--erb:#3a1d1b}"
    "html{background:var(--bg);color-scheme:light}html.dk{color-scheme:dark}"
    "body{font:15px/1.4 sans-serif;max-width:1000px;margin:.6em auto;padding:0 .8em;"
    "color:var(--fg);background:var(--bg)}"
    "a{color:var(--lnk)}h1{margin:.2em 0;font-size:1.5em}"
    "h2{border-bottom:2px solid var(--acc);padding-bottom:3px;margin:1em 0 .5em;font-size:1.25em}"
    "nav.nv{display:flex;flex-wrap:wrap;gap:.3em;margin:.3em 0 .7em}"
    "nav.nv a,nav.nv span{padding:.15em .65em;border:1px solid var(--bd);border-radius:6px;"
    "text-decoration:none;background:var(--card)}"
    "nav.nv span{background:var(--acc);border-color:var(--acc);color:#fff;font-weight:bold}"
    "table{border-collapse:collapse;width:100%}"
    "td,th{padding:.2em .45em;border:1px solid var(--bd);text-align:left;line-height:1.3}th{background:var(--th)}"
    "table{font-size:.93em}"
    "small,.mut{color:var(--mut)}"
    "button,input,select,textarea{font:inherit;color:var(--fg);background:var(--inb);"
    "border:1px solid var(--bd);border-radius:4px;padding:.2em .5em}"
    "button{background:var(--btn);cursor:pointer;padding:.25em .8em;margin:.1em}"
    "button:disabled{opacity:.5;cursor:default}"
    "fieldset{border:1px solid var(--bd);border-radius:6px;margin:.7em 0;padding:.4em .8em .7em}"
    "legend{font-weight:bold;padding:0 6px}"
    "details{border:1px solid var(--bd);border-radius:6px;margin:.5em 0;padding:.1em .8em}"
    "details summary{cursor:pointer;font-weight:bold;padding:4px 0}"
    "dialog{max-width:26em;width:90%;color:var(--fg);background:var(--bg);"
    "border:1px solid var(--bd);border-radius:6px}"
    "dialog::backdrop{background:rgba(0,0,0,.5)}"
    "dialog.help{max-width:40em;max-height:85vh;overflow:auto}dialog.help code{white-space:nowrap}"
    "dialog.help ul{padding-left:1.2em;margin:.3em 0}dialog.help h4{margin:.7em 0 .2em}"
    "dialog label{display:block;margin:.6em 0}"
    "dialog input[type=text],dialog select{width:100%;box-sizing:border-box}"
    ".w{overflow-x:auto}form.inline{display:inline}"
    ".ati{background:#1b1b1b;border-radius:4px;vertical-align:middle;margin-right:4px}"
    ".ok,.wn,.er,#banner{padding:.5em;border-radius:4px;margin:.6em 0}"
    ".ok,#banner{background:var(--okb);border:1px solid var(--okbd)}"
    ".wn{background:var(--wb);border:1px solid var(--wbd)}"
    ".er,#dmsg,#smsg{color:var(--er)}.er{background:var(--erb);border:1px solid var(--er)}"
    "#dmsg,#smsg{min-height:1.2em}[hidden]{display:none!important}";

static const struct {
    WebPageId id;
    const char *href;
    const char *label;
} kNav[] = {
    {WEBPAGE_SETUP, "/", "Setup"},
    {WEBPAGE_CURRENT, "/current", "Current Aircraft"},
    {WEBPAGE_REGISTERED, "/registered", "Registered Aircraft"},
    {WEBPAGE_OPERATORS, "/operators", "Operators"},
    {WEBPAGE_SEEN, "/seen", "Seen"},
    {WEBPAGE_HISTORY, "/history", "History"},
};

/* One shared navigation for every page. A link is shown only when its capability is available:
 * optional subsystems that are switched off in Features are hidden, and History is hidden when the
 * persistent-history layer never initialised (compiled out). A merely missing/unmounted TF card keeps
 * the link so the History page can say why and point to Diagnostics. The page being viewed is always
 * shown (as the current-page marker) so the bar never loses its place. */
static bool NavVisible(WebPageId id, WebPageId current)
{
    if (id == current)
        return true;
    switch (id) {
    case WEBPAGE_CURRENT: return Features_CurrentEnabled();
    case WEBPAGE_REGISTERED: return Features_RegisteredEnabled();
    case WEBPAGE_OPERATORS: return Features_OperatorsEnabled();
    case WEBPAGE_SEEN: return Features_SeenEnabled();
    case WEBPAGE_HISTORY: {
        TfInitInfo info;
        TfHistory_GetInitInfo(&info);
        return info.attempted;
    }
    default: return true;
    }
}

esp_err_t WebStyle_SendHead(httpd_req_t *req, const char *title, WebPageId page, const char *extraCss)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "<!doctype html><html%s><head><meta name='viewport' "
                     "content='width=device-width,initial-scale=1'><title>%s</title><style>",
                     Features_DarkMode() ? " class='dk'" : "", title ? title : "Flight Radar");
    if (n <= 0 || n >= (int)sizeof(buf))
        return ESP_FAIL;
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK ||
        httpd_resp_send_chunk(req, kCss, sizeof(kCss) - 1) != ESP_OK)
        return ESP_FAIL;
    if (extraCss && extraCss[0] && httpd_resp_send_chunk(req, extraCss, HTTPD_RESP_USE_STRLEN) != ESP_OK)
        return ESP_FAIL;

    n = snprintf(buf, sizeof(buf), "</style></head><body><nav class='nv'>");
    for (size_t i = 0; i < sizeof(kNav) / sizeof(kNav[0]) && n > 0 && n < (int)sizeof(buf); i++) {
        int w;
        if (!NavVisible(kNav[i].id, page))
            continue;
        if (kNav[i].id == page)
            w = snprintf(buf + n, sizeof(buf) - n, "<span aria-current='page'>%s</span>", kNav[i].label);
        else
            w = snprintf(buf + n, sizeof(buf) - n, "<a href='%s'>%s</a>", kNav[i].href, kNav[i].label);
        if (w < 0)
            return ESP_FAIL;
        n += w;
    }
    if (n <= 0 || n >= (int)sizeof(buf) - 8)
        return ESP_FAIL;
    n += snprintf(buf + n, sizeof(buf) - n, "</nav>");
    return httpd_resp_send_chunk(req, buf, n);
}

const char *WebStyle_HtmlAttr(void)
{
    return Features_DarkMode() ? " class='dk'" : "";
}

esp_err_t WebStyle_SendFeatureControls(httpd_req_t *req)
{
    char buf[1100];
    int n = snprintf(buf, sizeof(buf),
        "<h3 id='features'>Features &amp; appearance</h3>"
        "<form method='POST' action='/features'><fieldset><legend>Optional subsystems</legend>"
        "<label><input type='checkbox' name='f_seen' value='1'%s> Seen logging</label>"
        "<label><input type='checkbox' name='f_cur' value='1'%s> Current Aircraft page</label>"
        "<label><input type='checkbox' name='f_reg' value='1'%s> Registered Aircraft matching</label>"
        "<label><input type='checkbox' name='f_ops' value='1'%s> Registered Operators matching</label>"
        "<small>Turning one off only stops that optional processing; it never deletes data and "
        "never stops aircraft tracking or the radar display. Registered Aircraft / Operators off: "
        "aircraft use the default classification.</small></fieldset>"
        "<label><input type='checkbox' name='f_dark' value='1'%s> Dark mode (WebUI only)</label>",
        Features_SeenEnabled() ? " checked" : "", Features_CurrentEnabled() ? " checked" : "",
        Features_RegisteredEnabled() ? " checked" : "", Features_OperatorsEnabled() ? " checked" : "",
        Features_DarkMode() ? " checked" : "");
    if (n <= 0 || n >= (int)sizeof(buf) || httpd_resp_send_chunk(req, buf, n) != ESP_OK)
        return ESP_FAIL;
    /* 0.0.30: device-screen presentation options (sent separately: buf stays small). */
    n = snprintf(buf, sizeof(buf),
        "<fieldset><legend>Selected Craft panel (device screen)</legend><input type='hidden' name='p_form' value='1'>"
        "<label><input type='checkbox' name='p_trend' value='1'%s> Altitude trend arrow "
        "(climbing / descending, from the provider's reported climb rate)</label>"
        "<label><input type='checkbox' name='p_speed' value='1'%s> Speed row</label>"
        "<small>Display only: aircraft tracking, filtering and classification are not affected.</small></fieldset>"
        "<button type='submit'>Save features</button></form>",
        Features_PanelAltTrend() ? " checked" : "", Features_PanelSpeed() ? " checked" : "");
    if (n <= 0 || n >= (int)sizeof(buf))
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, buf, n);
}

/* True when the urlencoded body has "<name>=" at the start or after '&'. */
static bool HasKey(const char *body, const char *name)
{
    size_t len = strlen(name);
    for (const char *p = body; p && *p;) {
        if (!strncmp(p, name, len) && p[len] == '=')
            return true;
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return false;
}

static esp_err_t FeaturesPost(httpd_req_t *req)
{
    char body[160];
    size_t total = req->content_len;
    if (total == 0 || total >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    size_t got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
        got += (size_t)r;
    }
    body[got] = '\0';

    bool ok = Features_Set(FEATURE_SEEN, HasKey(body, "f_seen"));
    ok = Features_Set(FEATURE_CURRENT, HasKey(body, "f_cur")) && ok;
    ok = Features_Set(FEATURE_REGISTERED, HasKey(body, "f_reg")) && ok;
    ok = Features_Set(FEATURE_OPERATORS, HasKey(body, "f_ops")) && ok;
    ok = Features_Set(FEATURE_DARK_MODE, HasKey(body, "f_dark")) && ok;
    /* 0.0.30 panel options; an older form without them leaves them unchanged. */
    if (HasKey(body, "p_form")) {
        ok = Features_Set(FEATURE_PANEL_ALT_TREND, HasKey(body, "p_trend")) && ok;
        ok = Features_Set(FEATURE_PANEL_SPEED, HasKey(body, "p_speed")) && ok;
    }
    if (!ok)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings");

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_sendstr(req, "Saved.");
}

/* 0.0.28: one shared help popup (Search Help on /seen and /history, Rule Help
 * on /registered). A native <dialog> shown over the page: opening it does not
 * navigate, so the page, its scroll position and any typed search stay as they
 * were; Close (a method='dialog' form, no script) or Esc returns to them. */
esp_err_t WebStyle_SendHelpLink(httpd_req_t *req, const char *dialogId, const char *linkText)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "<a href='#' class='helpln' onclick=\"document.getElementById('%s').showModal();return false;\">%s</a>",
                     dialogId, linkText);
    if (n < 0 || n >= (int)sizeof(buf))
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, buf, n);
}

esp_err_t WebStyle_SendHelpDialog(httpd_req_t *req, const char *dialogId, const char *title, const char *bodyHtml)
{
    char head[160];
    int n = snprintf(head, sizeof(head), "<dialog id='%s' class='help'><h3>%s</h3>", dialogId, title);
    if (n < 0 || n >= (int)sizeof(head) || httpd_resp_send_chunk(req, head, n) != ESP_OK ||
        httpd_resp_send_chunk(req, bodyHtml, HTTPD_RESP_USE_STRLEN) != ESP_OK)
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, "<form method='dialog'><button>Close</button></form></dialog>", HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebStyle_Register(httpd_handle_t server)
{
    const httpd_uri_t features = {.uri = "/features", .method = HTTP_POST, .handler = FeaturesPost};
    return httpd_register_uri_handler(server, &features);
}
