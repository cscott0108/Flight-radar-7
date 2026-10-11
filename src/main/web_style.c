#include "web_style.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "feature_flags.h"
#include "tf_history.h"
#include "north_ref.h"
#include "time_util.h"
#include "ui_prefs.h"

#include <stdlib.h>
#include <time.h>

static void (*s_appearanceHook)(void);

void WebStyle_SetAppearanceChangedHook(void (*hook)(void))
{
    s_appearanceHook = hook;
}

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
    /* 0.1.4: the header (navigation + clock; on Setup also its title and section links) stays at
     * the top while scrolling. Sticky keeps it in the flow, so nothing is covered at the top of a
     * page; the opaque background hides what scrolls underneath. scroll-padding-top keeps anchor
     * targets (#features, #tf, #g<fp>, ...) clear of it (a wrapped header on a narrow screen is taller). */
    "header.sk{position:sticky;top:0;z-index:20;background:var(--bg);margin:0 -.8em;padding:.1em .8em .1em;"
    "border-bottom:1px solid var(--bd)}header.sk nav.nv{margin:.3em 0}"
    "html{scroll-padding-top:4.5em}@media(max-width:640px){html{scroll-padding-top:8em}}"
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
    "#dmsg,#smsg{min-height:1.2em}[hidden]{display:none!important}"
    "nav.nv small.nt{margin-left:auto;align-self:center;color:var(--mut);white-space:nowrap}";

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

/* HOSTTEST:BEGIN clockjs (extracted and run under node by host_tests/web_clock_test.js) */
/* 0.1.3: advances the nav clock (#nt) in the browser from the device instant and
 * offsets in its data attributes, using only the browser's elapsed time (never its
 * clock or time zone). Updates on each minute boundary; touches only #nt; no
 * requests. Same text as WebStyle_FormatAt: "h:mm AM PDT" or "HH:mm PDT". */
static const char kClockJs[] =
    "<script>(function(){var e=document.getElementById('nt');if(!e||!e.dataset.t)return;"
    "var d=e.dataset,t0=+d.t,n=+d.n,end=+d.e,h24=d.f=='1',p0=Date.now();"
    "function f(){var u=t0+Math.floor((Date.now()-p0)/1000);"
    "if(u>=end){e.title='Reload the page for the current device time';return 0}"
    "var nx=n&&u>=n,o=nx?+d.p:+d.o,a=nx?d.b:d.a,l=u+o*60,m=Math.floor(l/60),mi=((m%60)+60)%60,"
    "h=((Math.floor(m/60)%24)+24)%24;"
    "e.textContent=(h24?(h<10?'0':'')+h:(h%12||12))+':'+(mi<10?'0':'')+mi+(h24?'':(h<12?' AM':' PM'))+' '+a;"
    "return (60-(((l%60)+60)%60))*1000+50}"
    "function s(){var w=f();if(w)setTimeout(s,w)}s()})();</script>";
/* HOSTTEST:END clockjs */

static esp_err_t SendHeadImpl(httpd_req_t *req, const char *title, WebPageId page, const char *extraCss, bool keepOpen);

esp_err_t WebStyle_SendHead(httpd_req_t *req, const char *title, WebPageId page, const char *extraCss)
{
    return SendHeadImpl(req, title, page, extraCss, false);
}

esp_err_t WebStyle_SendHeadOpen(httpd_req_t *req, const char *title, WebPageId page, const char *extraCss)
{
    return SendHeadImpl(req, title, page, extraCss, true);
}

static esp_err_t SendHeadImpl(httpd_req_t *req, const char *title, WebPageId page, const char *extraCss, bool keepOpen)
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
    /* 0.0.32: WebUI accent color (its own setting, independent of the radar's).
     * Only emitted when it differs from the stylesheet default, so the default
     * page is byte-identical to earlier firmware. */
    const uint32_t acc = UiPrefs_WebAccentRgb();
    if (acc != UI_ACCENT_DEFAULT_RGB) {
        n = snprintf(buf, sizeof(buf), ":root,html.dk{--acc:#%06X}", (unsigned)(acc & 0xFFFFFFu));
        if (n <= 0 || n >= (int)sizeof(buf) || httpd_resp_send_chunk(req, buf, n) != ESP_OK)
            return ESP_FAIL;
    }

    n = snprintf(buf, sizeof(buf), "</style></head><body><header class='sk'><nav class='nv'>");
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
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
        return ESP_FAIL;
    /* 0.0.32: the device's local time (server-side, configured zone/DST and 12h/24h).
     * 0.1.3: kept current in the browser by kClockJs from the instant, UTC offset and
     * next DST change embedded here - the device is never contacted again for it. */
    const int64_t nowUtc = (int64_t)time(NULL);
    char now[40];
    WebStyle_FormatAt(nowUtc, now, sizeof(now));
    WebClockData cd;
    const bool live = WebStyle_ClockData(nowUtc, &cd);
    if (!live)
        n = snprintf(buf, sizeof(buf), "<small class='nt'>%s</small></nav>", now); /* not synced: as before, no script */
    else
        n = snprintf(buf, sizeof(buf),
                     "<small class='nt' id='nt' title='Device local time (configured time zone), kept current by this page' "
                     "data-t='%lld' data-o='%d' data-a='%s' data-n='%lld' data-p='%d' data-b='%s' data-e='%lld' data-f='%d'>%s</small></nav>",
                     (long long)cd.utc, cd.offMin, cd.abbr, (long long)cd.nextUtc, cd.nextOffMin, cd.nextAbbr,
                     (long long)cd.validUntil, UiPrefs_ClockFormat() == UI_CLOCK_24H ? 1 : 0, now);
    if (n <= 0 || n >= (int)sizeof(buf) || httpd_resp_send_chunk(req, buf, n) != ESP_OK)
        return ESP_FAIL;
    if (live && httpd_resp_send_chunk(req, kClockJs, sizeof(kClockJs) - 1) != ESP_OK)
        return ESP_FAIL;
    return keepOpen ? ESP_OK : httpd_resp_send_chunk(req, "</header>", HTTPD_RESP_USE_STRLEN);
}

void WebStyle_FormatAt(int64_t utcSeconds, char *out, size_t cap)
{
    if (!out || !cap)
        return;
    TimeLocal tl;
    if (!TimeUtil_ToLocal(utcSeconds, &tl)) {
        snprintf(out, cap, "time not synced");
        return;
    }
    char hm[16];
    UiClockFormat fmt = UiPrefs_ClockFormat();
    UiPrefs_FormatClock(fmt == UI_CLOCK_24H ? UI_CLOCK_24H : UI_CLOCK_12H, tl.hour, tl.minute, hm, sizeof(hm));
    snprintf(out, cap, "%s %s", hm, tl.abbr);
}

void WebStyle_FormatNowShort(char *out, size_t cap)
{
    WebStyle_FormatAt((int64_t)time(NULL), out, cap);
}

/* 0.1.3: data for the nav clock script. The configured zone's UTC offset now and
 * at its next change (DST), found by an hourly scan of the next WEB_CLOCK_HORIZON_SEC
 * and a bisection to the second (about 200 pure conversions, once per page). The
 * script stops at validUntil (horizon end, or a second change inside it). */
bool WebStyle_ClockData(int64_t nowUtc, WebClockData *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    TimeZoneConfig cfg;
    TimeUtil_GetConfig(&cfg);
    TimeLocal a, b;
    if (!TimeUtil_IsSynced(nowUtc) || !TimeUtil_Convert(&cfg, nowUtc, &a))
        return false;
    out->utc = nowUtc;
    out->offMin = a.utcOffsetMinutes;
    snprintf(out->abbr, sizeof(out->abbr), "%s", a.abbr);
    out->validUntil = nowUtc + WEB_CLOCK_HORIZON_SEC;
    int changes = 0;
    int64_t prev = nowUtc;
    int prevOff = a.utcOffsetMinutes;
    for (int64_t t = nowUtc + 3600; t <= nowUtc + WEB_CLOCK_HORIZON_SEC && changes < 2; t += 3600) {
        if (!TimeUtil_Convert(&cfg, t, &b))
            break;
        if (b.utcOffsetMinutes != prevOff) {
            int64_t lo = prev, hi = t; /* offset(lo) == prevOff, offset(hi) != prevOff */
            while (hi - lo > 1) {
                const int64_t mid = lo + (hi - lo) / 2;
                TimeLocal m;
                if (TimeUtil_Convert(&cfg, mid, &m) && m.utcOffsetMinutes == prevOff)
                    lo = mid;
                else
                    hi = mid;
            }
            if (changes == 0) {
                TimeUtil_Convert(&cfg, hi, &b);
                out->nextUtc = hi;
                out->nextOffMin = b.utcOffsetMinutes;
                snprintf(out->nextAbbr, sizeof(out->nextAbbr), "%s", b.abbr);
            } else {
                out->validUntil = hi; /* a second change: stop there (reload) */
            }
            changes++;
            prevOff = b.utcOffsetMinutes;
        }
        prev = t;
    }
    return true;
}

const char *WebStyle_HtmlAttr(void)
{
    return Features_DarkMode() ? " class='dk'" : "";
}

esp_err_t WebStyle_SendFeatureControls(httpd_req_t *req)
{
    char buf[1600];
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
        "<small>Display only: aircraft tracking, filtering and classification are not affected.</small></fieldset>",
        Features_PanelAltTrend() ? " checked" : "", Features_PanelSpeed() ? " checked" : "");
    if (n <= 0 || n >= (int)sizeof(buf) || httpd_resp_send_chunk(req, buf, n) != ESP_OK)
        return ESP_FAIL;
    /* 0.0.32: north reference, accent colors, device clock. */
    const NorthRefMode mode = NorthRef_GetMode();
    const UiClockFormat cf = UiPrefs_ClockFormat();
    NorthRefStatus st;
    NorthRef_GetStatus(&st);
    char resolved[96];
    if (st.varValid)
        snprintf(resolved, sizeof(resolved), "now %s; magnetic variation %.1f&deg; %s", st.resolved == NORTH_RESOLVED_MAGNETIC ? "MAGNETIC" : "TRUE",
                 (double)fabsf(st.declDeg), st.declDeg >= 0.0f ? "E" : "W");
    else
        snprintf(resolved, sizeof(resolved), "now TRUE; no magnetic variation available");
    n = snprintf(buf, sizeof(buf),
        "<fieldset><legend>Radar reference, colors and clock</legend><input type='hidden' name='d_form' value='1'>"
        "<label>Radar north <select name='northref'>"
        "<option value='0'%s>AUTO (magnetic when available)</option><option value='1'%s>MAGNETIC</option>"
        "<option value='2'%s>TRUE (geographic)</option></select></label> <small>%s. Runway numbers are magnetic; "
        "aircraft tracks are true. The radar rotates so both agree (see README).</small>"
        "<label>Radar accent color <input type='color' name='acc_radar' value='#%06X'></label>"
        "<label>WebUI accent color <input type='color' name='acc_web' value='#%06X'></label>"
        "<small>Independent of each other; default #4CAF50 (green).</small>"
        "<input type='hidden' name='c_form' value='1'>"
        "<label><input type='checkbox' name='clockshow' value='1'%s> Show clock on the device screen</label>"
        "<label>Clock format <select name='clockfmt'>"
        "<option value='1'%s>12-hour</option><option value='2'%s>24-hour</option></select></label>"
        "<small>The format is kept while the clock is hidden and is also used by the WebUI clock. "
        "Hiding the clock does not change time keeping, time zone or DST.</small>"
        "</fieldset><button type='submit'>Save features</button></form>",
        mode == NORTH_REF_AUTO ? " selected" : "", mode == NORTH_REF_MAGNETIC ? " selected" : "",
        mode == NORTH_REF_TRUE ? " selected" : "", resolved,
        (unsigned)(UiPrefs_RadarAccentRgb() & 0xFFFFFFu), (unsigned)(UiPrefs_WebAccentRgb() & 0xFFFFFFu),
        UiPrefs_ClockVisible() ? " checked" : "", cf != UI_CLOCK_24H ? " selected" : "", cf == UI_CLOCK_24H ? " selected" : "");
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

/* Value of "<name>=" in an urlencoded body (only %XX and '+' decoding needed for
 * these fields: numbers and "#RRGGBB" colors). */
static bool FormValue(const char *body, const char *name, char *out, size_t cap)
{
    const size_t len = strlen(name);
    for (const char *p = body; p && *p;) {
        if (!strncmp(p, name, len) && p[len] == '=') {
            const char *v = p + len + 1;
            size_t o = 0;
            while (*v && *v != '&' && o + 1 < cap) {
                if (v[0] == '%' && v[1] && v[2]) {
                    char hex[3] = {v[1], v[2], 0};
                    out[o++] = (char)strtol(hex, NULL, 16);
                    v += 3;
                } else {
                    out[o++] = (*v == '+') ? ' ' : *v;
                    v++;
                }
            }
            out[o] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return false;
}

static esp_err_t FeaturesPost(httpd_req_t *req)
{
    char body[384];
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
    /* 0.0.32 display settings; an older form without them leaves them unchanged. */
    if (HasKey(body, "d_form")) {
        char v[16];
        uint32_t rgb;
        if (FormValue(body, "northref", v, sizeof(v)))
            ok = NorthRef_SetMode((NorthRefMode)atoi(v)) && ok;
        if (FormValue(body, "acc_radar", v, sizeof(v)))
            ok = UiPrefs_ParseColor(v, &rgb) && UiPrefs_SetRadarAccentRgb(rgb) && ok;
        if (FormValue(body, "acc_web", v, sizeof(v)))
            ok = UiPrefs_ParseColor(v, &rgb) && UiPrefs_SetWebAccentRgb(rgb) && ok;
        /* 0.1.6: "Show clock" checkbox + 12/24-hour format (c_form marks this form). An older form
         * (Off / 12-hour / 24-hour select, no c_form) still works: Off hides the clock and keeps the
         * format, 12/24-hour shows it in that format. */
        const bool clockForm = HasKey(body, "c_form");
        if (FormValue(body, "clockfmt", v, sizeof(v))) {
            const UiClockFormat fmt = (UiClockFormat)atoi(v);
            ok = UiPrefs_SetClockFormat(fmt) && ok;
            if (!clockForm && fmt != UI_CLOCK_OFF && (unsigned)fmt < UI_CLOCK_FORMAT_COUNT)
                ok = UiPrefs_SetClockVisible(true) && ok;
        }
        if (clockForm)
            ok = UiPrefs_SetClockVisible(HasKey(body, "clockshow")) && ok;
        if (s_appearanceHook)
            s_appearanceHook(); /* radar redraw (rotation / accent) even while frozen */
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
