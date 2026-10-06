#include "web_wifi.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "diag_telemetry.h"
#include "main.h"
#include "web_style.h"
#include "web_util.h"
#include "wifi_profiles.h"

static esp_err_t Send(httpd_req_t *req, const char *s) { return httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN); }

static esp_err_t SendFormat(httpd_req_t *req, const char *format, ...) __attribute__((format(printf, 2, 3)));
static esp_err_t SendFormat(httpd_req_t *req, const char *format, ...)
{
    char buf[1280]; /* widest row: two escaped 32-char SSIDs (<= 6 bytes per character) + fixed markup */
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n < 0 || n >= (int)sizeof(buf))
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, buf, (ssize_t)n);
}

/* Messages shown after a POST. The page is redirected to /wifi?m=<code>; the code is looked up here
 * (never echoed), so no user data is reflected into the page. Codes 20+ = WifiProfileResult errors. */
#define MSG_SAVED 1
#define MSG_DELETED 2
#define MSG_CONNECTING 3
#define MSG_BAD_FORM 4
#define MSG_ERR_BASE 20

static const char *MessageText(int m)
{
    switch (m) {
    case MSG_SAVED: return "Saved.";
    case MSG_DELETED: return "Deleted.";
    case MSG_CONNECTING:
        return "Connecting. The device may leave this network while it joins the other one; "
               "reconnect to the network it joined (or to Flight-Radar-Setup if it could not).";
    case MSG_BAD_FORM: return "The form was incomplete or too large.";
    default:
        if (m >= MSG_ERR_BASE && m < MSG_ERR_BASE + 8)
            return WifiProfiles_ResultText((WifiProfileResult)(m - MSG_ERR_BASE));
        return NULL;
    }
}

void WebWifi_StatusLine(char *out, size_t cap)
{
    char ssid[WIFI_SSID_MAX + 1];
    int slot = -1;
    bool connected = WifiGetStatus(ssid, sizeof(ssid), &slot);
    char esc[WIFI_SSID_MAX * 6 + 1];
    WebUtil_EscapeHtml(esc, sizeof(esc), ssid);
    if (connected && slot >= 0)
        snprintf(out, cap, "Connected to <b>%s</b> (saved network %d of %d)", esc, slot + 1, WIFI_PROFILE_SLOTS);
    else if (connected)
        snprintf(out, cap, "Connected to <b>%s</b> (not saved)", esc);
    else
        snprintf(out, cap, "Not connected (%d saved network%s)", WifiProfiles_Count(), WifiProfiles_Count() == 1 ? "" : "s");
}

static esp_err_t WifiPage(httpd_req_t *req)
{
    int msg = 0;
    char query[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(query, "m", v, sizeof(v)) == ESP_OK)
            msg = atoi(v);
    }

    if (WebStyle_SendHead(req, "Wi-Fi networks", WEBPAGE_NONE,
                          "body{max-width:760px}form.ws{border:1px solid var(--bd);border-radius:6px;padding:.4em .7em;margin:.5em 0}"
                          "form.ws input{width:11em}") != ESP_OK ||
        Send(req, "<h2>Wi-Fi networks</h2>") != ESP_OK)
        return ESP_FAIL;

    const char *mt = MessageText(msg);
    if (mt && SendFormat(req, "<p class='%s'>%s</p>", msg >= MSG_ERR_BASE || msg == MSG_BAD_FORM ? "er" : "ok", mt) != ESP_OK)
        return ESP_FAIL;

    char status[160 + WIFI_SSID_MAX * 6];
    WebWifi_StatusLine(status, sizeof(status));
    if (SendFormat(req, "<p>%s</p>", status) != ESP_OK ||
        Send(req, "<p class='mut'>Up to 5 saved networks. If the current network fails 3 times in a row the device tries the "
                  "next saved one. Saved passwords are never shown; leave the password blank to keep the saved one. "
                  "To make a network open (no password), delete it and add it again with an empty password.</p>") != ESP_OK)
        return ESP_FAIL;

    int trySlot = WifiProfiles_TrySlot();
    for (int i = 0; i < WIFI_PROFILE_SLOTS; i++) {
        WifiProfileInfo info;
        if (!WifiProfiles_Info(i, &info) || !info.used)
            continue;
        char esc[WIFI_SSID_MAX * 6 + 1];
        WebUtil_EscapeHtml(esc, sizeof(esc), info.ssid);
        if (SendFormat(req,
                "<form class='ws' method='POST' action='/wifi'><input type='hidden' name='slot' value='%d'>"
                "<b>Network %d</b>%s %s<br>"
                "<input name='ssid' maxlength='32' value='%s' aria-label='Network name'> "
                "<input name='password' type='password' maxlength='64' placeholder='%s' aria-label='Password' autocomplete='off'> "
                "<button name='act' value='update'>Save</button><button name='act' value='connect'>Connect</button>"
                "<button name='act' value='delete' onclick=\"return confirm('Delete this saved network?')\">Delete</button></form>",
                i, i + 1, i == trySlot ? " &middot; <i>in use</i>" : "",
                info.hasPassword ? "" : "&middot; <i>open network</i>", esc,
                info.hasPassword ? "(unchanged)" : "(none)") != ESP_OK)
            return ESP_FAIL;
    }
    if (WifiProfiles_Count() < WIFI_PROFILE_SLOTS) {
        if (Send(req, "<form class='ws' method='POST' action='/wifi'><b>Add a network</b><br>"
                      "<input name='ssid' maxlength='32' placeholder='Network name' aria-label='Network name'> "
                      "<input name='password' type='password' maxlength='64' placeholder='Password (empty = open)' autocomplete='off'> "
                      "<button name='act' value='add'>Add</button></form>") != ESP_OK)
            return ESP_FAIL;
    } else if (Send(req, "<p class='wn'>All 5 slots are in use. Delete one to add another network.</p>") != ESP_OK) {
        return ESP_FAIL;
    }
    if (Send(req, "</body></html>") != ESP_OK)
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool FormValue(const char *body, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = body; p && *p;) {
        if (!strncmp(p, name, nl) && p[nl] == '=') {
            const char *s = p + nl + 1;
            size_t n = strcspn(s, "&");
            if (n >= cap)
                return false; /* longer than any valid value: refuse rather than truncate silently */
            memcpy(out, s, n);
            out[n] = '\0';
            WebUtil_UrlDecodeInPlace(out);
            return true;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return false;
}

static esp_err_t Redirect(httpd_req_t *req, int msg)
{
    char loc[24];
    snprintf(loc, sizeof(loc), "/wifi?m=%d", msg);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t WifiPost(httpd_req_t *req)
{
    char body[513];
    if (req->content_len == 0 || req->content_len >= sizeof(body))
        return Redirect(req, MSG_BAD_FORM);
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
        got += (size_t)r;
    }
    body[got] = '\0';

    char act[12], slotText[8], ssid[WIFI_SSID_MAX * 3 + 4], pass[WIFI_PASS_MAX * 3 + 4];
    if (!FormValue(body, "act", act, sizeof(act)))
        return Redirect(req, MSG_BAD_FORM);
    int slot = -1;
    if (FormValue(body, "slot", slotText, sizeof(slotText)) && slotText[0] >= '0' && slotText[0] <= '9' && slotText[1] == '\0')
        slot = slotText[0] - '0';
    bool haveSsid = FormValue(body, "ssid", ssid, sizeof(ssid));
    bool havePass = FormValue(body, "password", pass, sizeof(pass));

    WifiProfileResult r = WP_OK;
    int msg = MSG_SAVED;
    int evSlot = slot;
    if (!strcmp(act, "add")) {
        if (!haveSsid)
            return Redirect(req, MSG_BAD_FORM);
        int newSlot;
        newSlot = -1;
        r = WifiProfiles_Add(ssid, havePass ? pass : "", &newSlot);
        evSlot = newSlot;
    } else if (!strcmp(act, "update")) {
        if (!haveSsid)
            return Redirect(req, MSG_BAD_FORM);
        r = WifiProfiles_Update(slot, ssid, (havePass && pass[0] != '\0') ? pass : NULL);
    } else if (!strcmp(act, "delete")) {
        r = WifiProfiles_Delete(slot);
        msg = MSG_DELETED;
    } else if (!strcmp(act, "connect")) {
        if (!WifiProfiles_SlotUsed(slot))
            r = slot < 0 || slot >= WIFI_PROFILE_SLOTS ? WP_BAD_SLOT : WP_NOT_FOUND;
        else {
            WifiRequestConnect(slot);
            msg = MSG_CONNECTING;
        }
    } else {
        return Redirect(req, MSG_BAD_FORM);
    }
    /* Stamped event (only when Advanced/Expert event logging is on). Slot number and result only:
     * never the SSID or password. */
    DiagTelemetry_Event("Wi-Fi profile %s: slot %d %s", act, evSlot, r == WP_OK ? "ok" : "rejected");
    return Redirect(req, r == WP_OK ? msg : MSG_ERR_BASE + (int)r);
}

esp_err_t WebWifi_Register(httpd_handle_t server)
{
    const httpd_uri_t get = {.uri = "/wifi", .method = HTTP_GET, .handler = WifiPage, .user_ctx = NULL};
    const httpd_uri_t post = {.uri = "/wifi", .method = HTTP_POST, .handler = WifiPost, .user_ctx = NULL};
    esp_err_t err = httpd_register_uri_handler(server, &get);
    if (err == ESP_OK)
        err = httpd_register_uri_handler(server, &post);
    return err;
}
