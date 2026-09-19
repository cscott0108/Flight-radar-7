#include "web_rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "custom_rules.h"
#include "opensky_client.h"

static esp_err_t SendChunk(httpd_req_t *req, const char *text)
{
    return httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t SendEscaped(httpd_req_t *req, const char *text)
{
    char buffer[128];
    size_t used = 0;
    for (const char *p = text; *p; p++) {
        const char *escape = NULL;
        switch (*p) {
        case '&': escape = "&amp;"; break;
        case '<': escape = "&lt;"; break;
        case '>': escape = "&gt;"; break;
        case '"': escape = "&quot;"; break;
        case '\'': escape = "&#39;"; break;
        default: break;
        }
        const char *piece = escape ? escape : p;
        size_t length = escape ? strlen(escape) : 1;
        if (used + length > sizeof(buffer)) {
            if (httpd_resp_send_chunk(req, buffer, used) != ESP_OK)
                return ESP_FAIL;
            used = 0;
        }
        memcpy(buffer + used, piece, length);
        used += length;
    }
    return used ? httpd_resp_send_chunk(req, buffer, used) : ESP_OK;
}

static esp_err_t RulesPage(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (SendChunk(req,
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Craft Type Rules</title><style>body{font:16px sans-serif;max-width:720px;margin:2em auto;padding:0 1em}"
        "table{border-collapse:collapse;width:100%}td,th{padding:.4em;border:1px solid #ccc;text-align:left}"
        "form{display:inline}</style></head><body><p><a href='/'>Back to setup</a></p>"
        "<h1>Craft Type Rules</h1><p>Prefix matches ignore case and include any following flight number. "
        "Use ? for one unknown letter or digit (for example S?NFRD). The longest custom rule wins. "
        "Blank call signs are Private.</p>"
        "<form method='post' action='/add'>"
        "<label>Call sign pattern <input name='prefix' maxlength='15' pattern='[A-Za-z0-9?]+' required></label> "
        "<label>Craft type <select name='type'>"
        "<option value='0'>Private</option><option value='1'>Commercial</option>"
        "<option value='2'>Police</option><option value='3'>Military</option>"
        "<option value='4'>Emergency Services</option></select></label> "
        "<button>Add or update</button></form><h2>Saved rules</h2>"
        "<table><tr><th>Prefix</th><th>Craft Type</th><th></th></tr>") != ESP_OK)
        return ESP_FAIL;

    size_t count = CustomRules_Count();
    if (!count && SendChunk(req, "<tr><td colspan='3'>No custom rules</td></tr>") != ESP_OK)
        return ESP_FAIL;
    for (size_t i = 0; i < count; i++) {
        CustomRule rule;
        if (!CustomRules_Get(i, &rule))
            continue;
        if (SendChunk(req, "<tr><td>") != ESP_OK ||
            SendEscaped(req, rule.prefix) != ESP_OK ||
            SendChunk(req, "</td><td>") != ESP_OK ||
            SendEscaped(req, CustomType_Name(rule.type)) != ESP_OK ||
            SendChunk(req, "</td><td><form method='post' action='/delete'>"
                           "<input type='hidden' name='prefix' value='") != ESP_OK ||
            SendEscaped(req, rule.prefix) != ESP_OK ||
            SendChunk(req, "'><button>Delete</button></form></td></tr>") != ESP_OK)
            return ESP_FAIL;
    }
    if (SendChunk(req, "</table><h2>Commercial call signs</h2>"
                       "<p>Built-in airline and operator codes do not use the 100 craft-rule slots. "
                       "Add up to 50 more letter codes here. A code such as DAL matches DAL123. "
                       "Use a craft rule above to override any built-in code.</p>"
                       "<form method='post' action='/commercial/add'>"
                       "<label>Commercial code <input name='code' maxlength='4' pattern='[A-Za-z]{2,4}' required></label> "
                       "<button>Add</button></form><h3>My commercial codes</h3><table>"
                       "<tr><th>Code</th><th></th></tr>") != ESP_OK)
        return ESP_FAIL;
    size_t commercialCount = CommercialRules_Count();
    if (!commercialCount && SendChunk(req, "<tr><td colspan='2'>No added codes</td></tr>") != ESP_OK)
        return ESP_FAIL;
    for (size_t i = 0; i < commercialCount; i++) {
        char code[MAX_COMMERCIAL_CODE + 1];
        if (!CommercialRules_Get(i, code))
            continue;
        if (SendChunk(req, "<tr><td>") != ESP_OK || SendEscaped(req, code) != ESP_OK ||
            SendChunk(req, "</td><td><form method='post' action='/commercial/delete'>"
                           "<input type='hidden' name='code' value='") != ESP_OK ||
            SendEscaped(req, code) != ESP_OK ||
            SendChunk(req, "'><button>Delete</button></form></td></tr>") != ESP_OK)
            return ESP_FAIL;
    }
    if (SendChunk(req, "</table><h3>Built-in codes</h3><p>") != ESP_OK)
        return ESP_FAIL;
    for (size_t i = 0; i < CommercialBuiltIn_Count(); i++) {
        if ((i && SendChunk(req, ", ") != ESP_OK) ||
            SendEscaped(req, CommercialBuiltIn_Get(i)) != ESP_OK)
            return ESP_FAIL;
    }
    if (SendChunk(req, "</p><h2>Current aircraft</h2><table>"
                       "<tr><th>Call Sign</th><th>Craft Type</th></tr>") != ESP_OK)
        return ESP_FAIL;
    int aircraftCount = gAircraftCount;
    if (aircraftCount < 0) aircraftCount = 0;
    if (aircraftCount > MAX_AIRCRAFT) aircraftCount = MAX_AIRCRAFT;
    if (!aircraftCount && SendChunk(req, "<tr><td colspan='2'>No aircraft in range</td></tr>") != ESP_OK)
        return ESP_FAIL;
    for (int i = 0; i < aircraftCount; i++) {
        char callsign[sizeof(gAircraft[i].callsign)];
        char hex[sizeof(gAircraft[i].icao24)];
        memcpy(callsign, gAircraft[i].callsign, sizeof(callsign));
        memcpy(hex, gAircraft[i].icao24, sizeof(hex));
        callsign[sizeof(callsign) - 1] = '\0';
        hex[sizeof(hex) - 1] = '\0';
        if (SendChunk(req, "<tr><td>") != ESP_OK ||
            SendEscaped(req, callsign[0] ? callsign : "(empty)") != ESP_OK ||
            SendChunk(req, "</td><td>") != ESP_OK ||
            SendEscaped(req, CustomType_Name(evaluateAircraftType(callsign, hex))) != ESP_OK ||
            SendChunk(req, "</td></tr>") != ESP_OK)
            return ESP_FAIL;
    }
    if (SendChunk(req, "</table><p><a href='/'>Back to setup</a></p></body></html>") != ESP_OK)
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool ReadForm(httpd_req_t *req, char body[96])
{
    if (req->content_len <= 0 || req->content_len >= 96)
        return false;
    size_t offset = 0;
    while (offset < (size_t)req->content_len) {
        int read = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (read <= 0)
            return false;
        offset += read;
    }
    body[offset] = '\0';
    return true;
}

static esp_err_t RedirectRules(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/rules");
    return httpd_resp_sendstr(req, "Saved. Return to /rules.");
}

static esp_err_t AddRule(httpd_req_t *req)
{
    char body[96], prefix[32], typeText[8];
    if (!ReadForm(req, body) ||
        httpd_query_key_value(body, "prefix", prefix, sizeof(prefix)) != ESP_OK ||
        httpd_query_key_value(body, "type", typeText, sizeof(typeText)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    /* HTML forms percent-encode '?', which is our single-character wildcard. */
    for (char *p = prefix; *p; p++) {
        if (p[0] == '%' && p[1] == '3' && (p[2] == 'F' || p[2] == 'f')) {
            *p = '?';
            memmove(p + 1, p + 3, strlen(p + 3) + 1);
        }
    }
    char *end;
    long type = strtol(typeText, &end, 10);
    if (end == typeText || *end || type < TYPE_PRIVATE || type > TYPE_EMERGENCY)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid craft type");
    char normalized[MAX_RULE_PREFIX + 1];
    if (!CustomRules_NormalizePrefix(prefix, normalized))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Pattern must be 1-15 letters, digits, or ? characters");
    if (!CustomRules_Add(normalized, (CustomType)type))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not save rule (storage error or 100-rule limit)");
    return RedirectRules(req);
}

static esp_err_t DeleteRule(httpd_req_t *req)
{
    char body[96], prefix[32];
    if (!ReadForm(req, body) ||
        httpd_query_key_value(body, "prefix", prefix, sizeof(prefix)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    if (!CustomRules_Delete(prefix))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Rule not found or storage error");
    return RedirectRules(req);
}

static esp_err_t AddCommercial(httpd_req_t *req)
{
    char body[96], code[16], normalized[MAX_COMMERCIAL_CODE + 1];
    if (!ReadForm(req, body) ||
        httpd_query_key_value(body, "code", code, sizeof(code)) != ESP_OK ||
        !CommercialRules_NormalizeCode(code, normalized))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Code must be 2-4 letters");
    if (!CommercialRules_Add(normalized))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not save code (storage error or 50-code limit)");
    return RedirectRules(req);
}

static esp_err_t DeleteCommercial(httpd_req_t *req)
{
    char body[96], code[16];
    if (!ReadForm(req, body) ||
        httpd_query_key_value(body, "code", code, sizeof(code)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    if (!CommercialRules_Delete(code))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Code not found or storage error");
    return RedirectRules(req);
}

esp_err_t WebRules_Register(httpd_handle_t server)
{
    const httpd_uri_t page = {.uri = "/rules", .method = HTTP_GET, .handler = RulesPage};
    const httpd_uri_t add = {.uri = "/add", .method = HTTP_POST, .handler = AddRule};
    const httpd_uri_t remove = {.uri = "/delete", .method = HTTP_POST, .handler = DeleteRule};
    const httpd_uri_t commercialAdd = {.uri = "/commercial/add", .method = HTTP_POST, .handler = AddCommercial};
    const httpd_uri_t commercialDelete = {.uri = "/commercial/delete", .method = HTTP_POST, .handler = DeleteCommercial};
    esp_err_t err = httpd_register_uri_handler(server, &page);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &add);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &remove);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &commercialAdd);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &commercialDelete);
    return err;
}
