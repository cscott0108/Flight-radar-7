#include "web_airports.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airports.h"
#include "custom_rules.h"
#include "main.h"
#include "opensky_client.h"
#include "radar.h"

static esp_err_t Send(httpd_req_t *req, const char *value)
{
    return httpd_resp_send_chunk(req, value, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t SendFormat(httpd_req_t *req, const char *format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length < 0 || length >= (int)sizeof(buffer))
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, buffer, length);
}

static esp_err_t SendEscaped(httpd_req_t *req, const char *value)
{
    char buffer[128];
    size_t used = 0;
    for (const char *p = value; *p; p++) {
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
            if (httpd_resp_send_chunk(req, buffer, used) != ESP_OK) return ESP_FAIL;
            used = 0;
        }
        memcpy(buffer + used, piece, length);
        used += length;
    }
    return used ? httpd_resp_send_chunk(req, buffer, used) : ESP_OK;
}

/* Avoid the comparatively expensive embedded printf floating-point path. */
static void FormatCoordinate(float value, char out[24])
{
    long scaled = lroundf(fabsf(value) * 1000000.0f);
    snprintf(out, 24, "%s%ld.%06ld", value < 0 ? "-" : "",
             scaled / 1000000, scaled % 1000000);
}

static const char *AircraftColor(CustomType type)
{
    switch (type) {
    case TYPE_COMMERCIAL: return "#FF9800";
    case TYPE_POLICE: return "#238BFF";
    case TYPE_MILITARY: return "#00D060";
    case TYPE_EMERGENCY: return "#FF3030";
    default: return "#FFFFFF";
    }
}

static esp_err_t AirportsPage(httpd_req_t *req)
{
    const float centerLat = GetRadarLat();
    const float centerLon = GetRadarLon();
    const float radiusKm = GetRadarRange();
    char latText[24], lonText[24], rangeText[24];
    FormatCoordinate(centerLat, latText);
    FormatCoordinate(centerLon, lonText);
    FormatCoordinate(radiusKm, rangeText);
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    if (Send(req,
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Airport Dots</title><style>body{font:16px sans-serif;max-width:850px;margin:1em auto;padding:0 1em}"
        "svg{width:min(100%,400px);height:auto;background:#0A1024;touch-action:none;cursor:crosshair}"
        "label{display:block;margin:.5em 0}input{font:inherit}table{border-collapse:collapse;width:100%}"
        "td,th{border:1px solid #bbb;padding:.4em;text-align:left}form.inline{display:inline}"
        "button{margin:.2em;padding:.3em .6em}</style></head><body>"
        "<p><a href='/'>Back to setup</a></p><h1>Airport dots</h1>"
        "<p>This is a snapshot of the radar when this page opened. Click inside the outer ring, "
        "name the airport, and save. Reload for recent aircraft. Saved dots stay at their latitude "
        "and longitude when you change radar range or center.</p>") != ESP_OK ||
        SendFormat(req, "<p>Radar center: %s, %s &middot; radius: %s km</p>",
                   latText, lonText, rangeText) != ESP_OK ||
        Send(req,
        "<svg id='map' viewBox='0 0 400 400' aria-label='Radar airport placement preview'>"
        "<circle cx='200' cy='200' r='190' fill='none' stroke='#00B849' stroke-width='2'/>"
        "<circle cx='200' cy='200' r='127' fill='none' stroke='#00B849' opacity='.7'/>"
        "<circle cx='200' cy='200' r='63' fill='none' stroke='#00B849' opacity='.7'/>"
        "<path d='M200 10V390M10 200H390' stroke='#00B849' opacity='.35'/>"
        "<text x='195' y='25' fill='#00E060'>N</text><text x='373' y='194' fill='#00E060'>E</text>"
        "<text x='195' y='385' fill='#00E060'>S</text><text x='15' y='194' fill='#00E060'>W</text>") != ESP_OK)
        return ESP_FAIL;

    /* Airport circles precede aircraft polygons so aircraft cover them. */
    size_t airportCount = Airports_Count();
    for (size_t i = 0; i < airportCount; i++) {
        AirportMarker marker;
        int x, y;
        if (!Airports_Get(i, &marker) ||
            !Radar_ProjectPosition(marker.latitude, marker.longitude,
                                   centerLat, centerLon, radiusKm, 190, &x, &y))
            continue;
        if (SendFormat(req, "<circle cx='%d' cy='%d' r='%u' fill='#FF0000'/>",
                       200 + x, 200 + y, (unsigned)(marker.diameter / 2)) != ESP_OK)
            return ESP_FAIL;
    }
    int count = gAircraftCount;
    if (count < 0) count = 0;
    if (count > MAX_AIRCRAFT) count = MAX_AIRCRAFT;
    for (int i = 0; i < count; i++) {
        Aircraft a = gAircraft[i];
        int x, y;
        if (!a.valid || !Radar_ProjectPosition(a.predictedLat, a.predictedLon,
                    centerLat, centerLon, radiusKm, 190, &x, &y))
            continue;
        x += 200; y += 200;
        if (SendFormat(req,
                "<polygon points='%d,%d %d,%d %d,%d' fill='%s' opacity='.9'/>",
                x, y - 6, x - 5, y + 4, x + 5, y + 4,
                AircraftColor(evaluateAircraftType(a.callsign, a.icao24))) != ESP_OK)
            return ESP_FAIL;
    }

    if (Send(req,
        "<circle id='candidate' cx='200' cy='200' r='6' fill='#FF0000' "
        "stroke='white' stroke-width='2' visibility='hidden'/></svg>"
        "<p><small>Red circles are saved airports; triangles are aircraft at page load. "
        "The white-rimmed red dot is your unsaved placement.</small></p>"
        "<h2 id='editor'>Add or edit airport</h2>"
        "<form id='airportForm' method='post' action='/airports/save'>"
        "<input id='index' name='index' type='hidden' value='-1'>"
        "<label>Name <input id='name' name='name' maxlength='24' required></label>"
        "<label>Latitude <input id='latitude' name='latitude' type='number' step='any' min='-90' max='90' required></label>"
        "<label>Longitude <input id='longitude' name='longitude' type='number' step='any' min='-180' max='180' required></label>"
        "<label>Dot diameter <input id='diameter' name='diameter' type='number' min='6' max='24' value='12' required> pixels</label>"
        "<button type='submit'>Save airport</button><button type='button' onclick='newAirport()'>New dot</button>"
        "</form><h2>Saved airports</h2><table><tr><th>Name</th><th>Position</th><th>Size</th><th></th></tr>") != ESP_OK)
        return ESP_FAIL;
    if (!airportCount && Send(req, "<tr><td colspan='4'>No airports saved</td></tr>") != ESP_OK)
        return ESP_FAIL;
    for (size_t i = 0; i < airportCount; i++) {
        AirportMarker marker;
        char airportLat[24], airportLon[24];
        if (!Airports_Get(i, &marker)) continue;
        FormatCoordinate(marker.latitude, airportLat);
        FormatCoordinate(marker.longitude, airportLon);
        if (Send(req, "<tr><td>") != ESP_OK ||
            SendEscaped(req, marker.name) != ESP_OK ||
            SendFormat(req,
                "</td><td>%s, %s</td><td>%u px</td><td>"
                "<button type='button' data-index='%u' data-lat='%s' data-lon='%s' data-size='%u' data-name='",
                airportLat, airportLon, (unsigned)marker.diameter, (unsigned)i,
                airportLat, airportLon, (unsigned)marker.diameter) != ESP_OK ||
            SendEscaped(req, marker.name) != ESP_OK ||
            Send(req, "' onclick='editAirport(this)'>Edit</button>"
                      "<form class='inline' method='post' action='/airports/delete'>") != ESP_OK ||
            SendFormat(req, "<input type='hidden' name='index' value='%u'>", (unsigned)i) != ESP_OK ||
            Send(req, "<button type='submit'>Remove</button></form></td></tr>") != ESP_OK)
            return ESP_FAIL;
    }
    if (Send(req,
        "</table><p>Up to 10 airports. Dots outside the current radar range remain saved "
        "and appear when the range includes them.</p>"
        "<script>const map=document.getElementById('map'),dot=document.getElementById('candidate');"
        "const centerLat=") != ESP_OK ||
        Send(req, latText) != ESP_OK || Send(req, ",centerLon=") != ESP_OK ||
        Send(req, lonText) != ESP_OK || Send(req, ",rangeKm=") != ESP_OK ||
        Send(req, rangeText) != ESP_OK ||
        Send(req,
        ";const f=document.getElementById('airportForm');"
        "const latInput=document.getElementById('latitude'),lonInput=document.getElementById('longitude');"
        "const nameInput=document.getElementById('name'),sizeInput=document.getElementById('diameter');"
        "const indexInput=document.getElementById('index');"
        "function preview(){let lat=Number(latInput.value),lon=Number(lonInput.value);"
        "if(!latInput.value||!lonInput.value){dot.setAttribute('visibility','hidden');return;}"
        "let east=(lon-centerLon)*111*Math.cos(centerLat*Math.PI/180),north=(lat-centerLat)*111;"
        "let x=200+east/rangeKm*190,y=200-north/rangeKm*190;"
        "if(!Number.isFinite(x)||!Number.isFinite(y)||Math.hypot(x-200,y-200)>190){"
        "dot.setAttribute('visibility','hidden');return;}"
        "dot.setAttribute('cx',x);dot.setAttribute('cy',y);"
        "dot.setAttribute('r',Number(sizeInput.value)/2||6);dot.setAttribute('visibility','visible');}"
        "map.addEventListener('click',function(e){let p=map.createSVGPoint();p.x=e.clientX;p.y=e.clientY;"
        "p=p.matrixTransform(map.getScreenCTM().inverse());"
        "let dx=p.x-200,dy=p.y-200;if(Math.hypot(dx,dy)>190)return;"
        "latInput.value=(centerLat-dy/190*rangeKm/111).toFixed(6);"
        "lonInput.value=(centerLon+dx/190*rangeKm/(111*Math.cos(centerLat*Math.PI/180))).toFixed(6);"
        "preview();});"
        "function newAirport(){f.reset();indexInput.value='-1';sizeInput.value='12';"
        "dot.setAttribute('visibility','hidden');location.hash='editor';}"
        "function editAirport(b){indexInput.value=b.dataset.index;nameInput.value=b.dataset.name;"
        "latInput.value=b.dataset.lat;lonInput.value=b.dataset.lon;"
        "sizeInput.value=b.dataset.size;preview();location.hash='editor';}"
        "['latitude','longitude','diameter'].forEach(id=>document.getElementById(id).addEventListener('input',preview));"
        "</script><p><a href='/'>Back to setup</a></p></body></html>") != ESP_OK)
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool GetField(const char *body, const char *key, char *out, size_t capacity)
{
    if (httpd_query_key_value(body, key, out, capacity) != ESP_OK)
        return false;
    char *read = out, *write = out;
    while (*read) {
        if (*read == '+') {
            *write++ = ' ';
            read++;
        } else if (read[0] == '%' && read[1] && read[2] &&
                   HexDigit(read[1]) >= 0 && HexDigit(read[2]) >= 0) {
            *write++ = (char)((HexDigit(read[1]) << 4) | HexDigit(read[2]));
            read += 3;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
    return true;
}

static bool ReadForm(httpd_req_t *req, char body[256])
{
    if (req->content_len <= 0 || req->content_len >= 256)
        return false;
    size_t offset = 0;
    while (offset < (size_t)req->content_len) {
        int read = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (read <= 0) return false;
        offset += read;
    }
    body[offset] = '\0';
    return true;
}

static bool ParseLong(const char *text, long *value)
{
    char *end;
    *value = strtol(text, &end, 10);
    return end != text && *end == '\0';
}

static bool ParseFloat(const char *text, float *value)
{
    char *end;
    *value = strtof(text, &end);
    return end != text && *end == '\0' && isfinite(*value);
}

static esp_err_t Redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/airports");
    return httpd_resp_sendstr(req, "Saved. Return to /airports.");
}

static esp_err_t SaveAirport(httpd_req_t *req)
{
    char body[256], indexText[12], name[64], latText[32], lonText[32], sizeText[12];
    long index, diameter;
    AirportMarker marker = {0};
    if (!ReadForm(req, body) ||
        !GetField(body, "index", indexText, sizeof(indexText)) ||
        !GetField(body, "name", name, sizeof(name)) ||
        !GetField(body, "latitude", latText, sizeof(latText)) ||
        !GetField(body, "longitude", lonText, sizeof(lonText)) ||
        !GetField(body, "diameter", sizeText, sizeof(sizeText)) ||
        !ParseLong(indexText, &index) || !ParseLong(sizeText, &diameter) ||
        !ParseFloat(latText, &marker.latitude) ||
        !ParseFloat(lonText, &marker.longitude) ||
        index < -1 || index >= MAX_AIRPORTS ||
        diameter < 6 || diameter > 24 || strlen(name) > AIRPORT_NAME_LENGTH)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid airport details");
    strcpy(marker.name, name);
    marker.diameter = (uint8_t)diameter;
    if (marker.name[0] == '\0' || marker.latitude < -90 || marker.latitude > 90 ||
        marker.longitude < -180 || marker.longitude > 180)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid airport position or name");
    if (!Airports_Save((int)index, &marker))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not save airport (storage error or 10-airport limit)");
    return Redirect(req);
}

static esp_err_t DeleteAirport(httpd_req_t *req)
{
    char body[256], indexText[12];
    long index;
    if (!ReadForm(req, body) ||
        !GetField(body, "index", indexText, sizeof(indexText)) ||
        !ParseLong(indexText, &index) || index < 0 || index >= MAX_AIRPORTS)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid airport selection");
    if (!Airports_Delete((size_t)index))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not remove airport");
    return Redirect(req);
}

esp_err_t WebAirports_Register(httpd_handle_t server)
{
    const httpd_uri_t page = {.uri = "/airports", .method = HTTP_GET, .handler = AirportsPage};
    const httpd_uri_t save = {.uri = "/airports/save", .method = HTTP_POST, .handler = SaveAirport};
    const httpd_uri_t remove = {.uri = "/airports/delete", .method = HTTP_POST, .handler = DeleteAirport};
    esp_err_t err = httpd_register_uri_handler(server, &page);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &save);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &remove);
    return err;
}
