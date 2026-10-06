#include "web_airports.h"
#include "web_style.h"

#include <ctype.h>
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
#include "visibility_policy.h"

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

/* Same geometry logic as radar.c's DrawAirportMarker: Airport_EffectiveShape,
 * Airport_HeliportSegments, Airport_ParseRunwayAxis
 * and Airport_RunwayAxisOffsets (both in airports.c) are the one
 * authoritative runway-orientation calculation, shared by both renderers -
 * this function only differs from the panel's in emitting SVG elements
 * instead of LVGL draw calls, and in inlining the fallback-to-dot check the
 * same way DrawAirportMarker does. */
/* axisOverrideDeg (may be NULL): exact axis for a Directional built-in airport
 * (database axis or a 1-degree user override), as in radar.c. */
static esp_err_t SendAirportMarker(httpd_req_t *req, const AirportMarker *marker, int x, int y,
                                   const float *axisOverrideDeg)
{
    char fill[8];
    snprintf(fill, sizeof(fill), "#%06X", (unsigned)(marker->color & 0xFFFFFFu));

    float axisDeg = 0.0f;
    const AirportMarkerMode shape = Airport_EffectiveShape(marker, &axisDeg);
    if (shape == AIRPORT_MARKER_DIRECTIONAL && axisOverrideDeg)
        axisDeg = *axisOverrideDeg;

    if (shape == AIRPORT_MARKER_HELIPORT) {
        int seg[3][4];
        Airport_HeliportSegments(marker->diameter, seg);
        for (int i = 0; i < 3; i++)
            if (SendFormat(req, "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='%s' stroke-width='2'/>",
                           x + seg[i][0], y + seg[i][1], x + seg[i][2], y + seg[i][3], fill) != ESP_OK)
                return ESP_FAIL;
        return ESP_OK;
    }
    if (shape == AIRPORT_MARKER_SQUARE) {
        const int d = marker->diameter, half = d / 2;
        return SendFormat(req, "<rect x='%d' y='%d' width='%d' height='%d' fill='%s'/>",
                          x - half, y - half, d, d, fill);
    }

    int centerDiameter = marker->diameter;
    if (shape == AIRPORT_MARKER_DIRECTIONAL) {
        float dx1, dy1, dx2, dy2;
        const float lineLength = marker->diameter * AIRPORT_RUNWAY_LINE_LENGTH_FACTOR;
        Airport_RunwayAxisOffsets(axisDeg, lineLength, &dx1, &dy1, &dx2, &dy2);
        const int x1 = x + (int)lroundf(dx1), y1 = y + (int)lroundf(dy1);
        const int x2 = x + (int)lroundf(dx2), y2 = y + (int)lroundf(dy2);

        float cdx, cdy, unusedX, unusedY;
        const float capLength = marker->diameter * AIRPORT_RUNWAY_CAP_LENGTH_FACTOR;
        Airport_RunwayAxisOffsets(axisDeg + 90.0f, capLength, &cdx, &cdy, &unusedX, &unusedY);
        const int cix = (int)lroundf(cdx), ciy = (int)lroundf(cdy);

        if (SendFormat(req, "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='%s' stroke-width='2'/>",
                       x1, y1, x2, y2, fill) != ESP_OK ||
            SendFormat(req, "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='%s' stroke-width='2'/>",
                       x1 - cix, y1 - ciy, x1 + cix, y1 + ciy, fill) != ESP_OK ||
            SendFormat(req, "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='%s' stroke-width='2'/>",
                       x2 - cix, y2 - ciy, x2 + cix, y2 + ciy, fill) != ESP_OK)
            return ESP_FAIL;

        centerDiameter = (int)(marker->diameter * AIRPORT_RUNWAY_CENTER_DIAMETER_FACTOR);
        if (centerDiameter < 4)
            centerDiameter = 4;
    }

    return SendFormat(req, "<circle cx='%d' cy='%d' r='%d' fill='%s'/>",
                      x, y, centerDiameter / 2, fill);
}

/* "X of N showing" for the active built-in airports, followed by the reasons
 * (only the ones that apply) that explain airports the user may expect:
 *   - active but not drawn: hidden by an override, or moved outside the
 *     radius by a position override (both counted in N, not in X);
 *   - in the radar area but not active: replaced by a user location naming
 *     the same ident (existing dedupe), or beyond the active-location limit.
 * Airports outside the radar area are never mentioned. */
static esp_err_t SendBuiltinCountLine(httpd_req_t *req, size_t showing, size_t active, unsigned hiddenByOverride,
                                      unsigned movedOutside, unsigned notDrawnOther)
{
    uint32_t replaced = 0, overCapacity = 0;
    Airports_GetBuiltinSelectionStats(&replaced, &overCapacity);
    char why[320] = "";
    size_t used = 0;
#define WHY(cond, ...)                                                                                   \
    do {                                                                                                 \
        if (cond && used < sizeof(why)) {                                                                \
            int n_ = snprintf(why + used, sizeof(why) - used, "%s", used ? ", " : "");                   \
            if (n_ > 0) used += (size_t)n_;                                                              \
            if (used < sizeof(why)) {                                                                    \
                n_ = snprintf(why + used, sizeof(why) - used, __VA_ARGS__);                              \
                if (n_ > 0) used += (size_t)n_;                                                          \
            }                                                                                            \
        }                                                                                                \
    } while (0)
    WHY(hiddenByOverride, "%u hidden by override", hiddenByOverride);
    WHY(movedOutside, "%u moved outside the radius by a position override", movedOutside);
    WHY(notDrawnOther, "%u not drawn", notDrawnOther);
    WHY(replaced, "%u replaced by a manual entry", (unsigned)replaced);
    WHY(overCapacity, "%u not active: %u-location limit reached", (unsigned)overCapacity, (unsigned)AIRPORT_ACTIVE_MAX);
#undef WHY
    if (used >= sizeof(why))
        why[sizeof(why) - 1] = '\0';
    if (SendFormat(req, "<p id='builtinShowing'><b>%u of %u showing</b>%s%s%s ",
                   (unsigned)showing, (unsigned)active, why[0] ? " (" : "", why, why[0] ? ")" : "") != ESP_OK)
        return ESP_FAIL;
    return Send(req, "<small>(active = selected for the current radar center and radius; showing = drawn on the "
                     "radar. A manual entry is a user-defined location whose name starts with the airport's ident; "
                     "it replaces the built-in airport.)</small></p>");
}

/* ---- built-in airport overrides ---- */

/* "KSFO San Francisco Intl" -> "KSFO" (built-in marker names start with the ident). */
static void IdentOfBuiltinName(const char *name, char out[5])
{
    size_t n = 0;
    while (n < 4 && name[n] && name[n] != ' ') {
        out[n] = name[n];
        n++;
    }
    out[n] = '\0';
}

/* Which values come from the database and which are user overrides. */
static esp_err_t SendOverrideState(httpd_req_t *req, uint8_t fields)
{
    if (!fields)
        return Send(req, "Database default");
    return SendFormat(req, "<b>Overridden:</b> %s%s%s%s%s<br><small>other values: database default</small>",
                      (fields & AIRPORT_OVR_HIDDEN) ? "hidden " : "",
                      (fields & AIRPORT_OVR_LAT) ? "latitude " : "",
                      (fields & AIRPORT_OVR_LON) ? "longitude " : "",
                      (fields & AIRPORT_OVR_ROTATION) ? "rotation " : "",
                      (fields & AIRPORT_OVR_COLOR) ? "display color" : "");
}

/* Override / Edit Override button (prefilled from the database defaults and the
 * stored override) and, when one exists, the Reset to Default form. */
static esp_err_t SendOverrideActions(httpd_req_t *req, const char *icao)
{
    AirportMarker def;
    float dbAxis = NAN;
    if (!Airports_GetBuiltinDefaults(icao, &def, &dbAxis))
        return ESP_OK;
    AirportBuiltinOverride o;
    const bool has = Airports_FindOverride(icao, &o);
    char dLat[24], dLon[24], oLat[24] = "", oLon[24] = "", dRot[8] = "", oRot[8] = "";
    FormatCoordinate(def.latitude, dLat);
    FormatCoordinate(def.longitude, dLon);
    if (isfinite(dbAxis))
        snprintf(dRot, sizeof(dRot), "%ld", lroundf(dbAxis));
    if (has && (o.fields & AIRPORT_OVR_LAT))
        FormatCoordinate((float)o.latE5 / 1e5f, oLat);
    if (has && (o.fields & AIRPORT_OVR_LON))
        FormatCoordinate((float)o.lonE5 / 1e5f, oLon);
    if (has && (o.fields & AIRPORT_OVR_ROTATION))
        snprintf(oRot, sizeof(oRot), "%u", (unsigned)o.rotationDeg);
    char dCol[8], oCol[8] = "";
    snprintf(dCol, sizeof(dCol), "#%06X", (unsigned)(def.color & 0xFFFFFFu));
    if (has && (o.fields & AIRPORT_OVR_COLOR))
        snprintf(oCol, sizeof(oCol), "#%06X", (unsigned)(o.color & 0xFFFFFFu));
    if (SendFormat(req,
            "<button type='button' data-icao='%s' data-hidden='%s' data-dlat='%s' data-dlon='%s' data-drot='%s' "
            "data-olat='%s' data-olon='%s' data-orot='%s' data-dcol='%s' data-ocol='%s' data-name='",
            icao, has && (o.fields & AIRPORT_OVR_HIDDEN) ? "1" : "0", dLat, dLon, dRot, oLat, oLon, oRot,
            dCol, oCol) != ESP_OK ||
        SendEscaped(req, def.name) != ESP_OK ||
        SendFormat(req, "' onclick='ovrEdit(this)'>%s</button>", has ? "Edit Override" : "Override") != ESP_OK)
        return ESP_FAIL;
    if (!has)
        return ESP_OK;
    return SendFormat(req,
        "<form class='inline' method='post' action='/airports/override'>"
        "<input type='hidden' name='icao' value='%s'><input type='hidden' name='action' value='reset'>"
        "<button type='submit'>Reset to Default</button></form>", icao);
}

/* The override editor (opened by an Override button) and the list of every
 * stored override, including airports outside the current radar area. */
static esp_err_t SendOverrideEditor(httpd_req_t *req)
{
    if (Send(req,
        "<div id='ovrEditor' hidden><h3>Override built-in airport <span id='ovrName'></span></h3>"
        "<form id='ovrForm' method='post' action='/airports/override'>"
        "<input type='hidden' name='icao' value=''><input type='hidden' name='action' value='save'>"
        "<label>Visibility <select name='vis'><option value='show'>Show (database default)</option>"
        "<option value='hide'>Hide (listed here, not drawn on the radar)</option></select></label>"
        "<label>Latitude <input name='olat' type='number' step='any' min='-90' max='90'> "
        "<small>blank = database default</small></label>"
        "<label>Longitude <input name='olon' type='number' step='any' min='-180' max='180'> "
        "<small>blank = database default</small></label>"
        "<label>Rotation <input name='orot' type='number' step='1' min='0' max='359'> degrees "
        "<small>runway axis; blank = database default. 142 and 322 are the same axis. "
        "Setting it shows the airport with the Directional marker.</small></label>"
        "<label><input type='checkbox' name='ocolset' value='1'> Override display color "
        "<input name='ocolor' type='color' value='#FF0000'> "
        "<small>the color this airport is drawn in, chosen like a user-defined location's color. Display only: it stays "
        "a built-in airport with the same selection, counts and priority. Unticked = built-in default color.</small></label>"
        "<button type='submit'>Save override</button></form>"
        "<p><small>Saving with every field at its default removes the override.</small></p></div>") != ESP_OK)
        return ESP_FAIL;
    const size_t n = Airports_OverrideCount();
    if (SendFormat(req, "<h3>Stored overrides (%u of %u)</h3>", (unsigned)n, (unsigned)AIRPORT_OVERRIDE_MAX) != ESP_OK)
        return ESP_FAIL;
    if (!n)
        return Send(req, "<p><small>None. Every built-in airport uses its database values.</small></p>");
    if (Send(req, "<table><tr><th>Airport</th><th>Values</th><th></th></tr>") != ESP_OK)
        return ESP_FAIL;
    for (size_t i = 0; i < n; i++) {
        AirportBuiltinOverride o;
        AirportMarker def;
        if (!Airports_GetOverride(i, &o))
            continue;
        char icao[5];
        memcpy(icao, o.icao, 4);
        icao[4] = '\0';
        const bool known = Airports_GetBuiltinDefaults(icao, &def, NULL);
        if (Send(req, "<tr><td>") != ESP_OK || SendEscaped(req, known ? def.name : icao) != ESP_OK ||
            Send(req, "</td><td>") != ESP_OK || SendOverrideState(req, o.fields) != ESP_OK ||
            Send(req, "</td><td>") != ESP_OK)
            return ESP_FAIL;
        if (known ? SendOverrideActions(req, icao) != ESP_OK
                  : SendFormat(req, "<form class='inline' method='post' action='/airports/override'>"
                                    "<input type='hidden' name='icao' value='%s'><input type='hidden' name='action' value='reset'>"
                                    "<button type='submit'>Reset to Default</button></form>", icao) != ESP_OK)
            return ESP_FAIL;
        if (Send(req, "</td></tr>") != ESP_OK)
            return ESP_FAIL;
    }
    return Send(req, "</table>");
}

/* ---- ground aircraft visibility & retention (visibility_policy.c) ---- */

static esp_err_t SendVisibilitySection(httpd_req_t *req)
{
    VisSettings v;
    VisPolicy_GetSettings(&v);
    char altText[12] = "", spdText[12] = "", radText[12];
    if (v.minAltFt != VIS_THRESHOLD_DEFAULT)
        snprintf(altText, sizeof(altText), "%u", (unsigned)v.minAltFt);
    if (v.minSpeedKt10 != VIS_THRESHOLD_DEFAULT)
        snprintf(spdText, sizeof(spdText), "%u.%u", (unsigned)(v.minSpeedKt10 / 10), (unsigned)(v.minSpeedKt10 % 10));
    snprintf(radText, sizeof(radText), "%u.%u", (unsigned)(v.airportRadiusM / 1000), (unsigned)(v.airportRadiusM % 1000 / 100));
    if (Send(req,
        "<h2 id='vis'>Ground aircraft visibility</h2>"
        "<p><small>An aircraft is <b>on the ground</b> when the provider reports it (OpenSky <i>on_ground</i>, "
        "adsb.lol <i>alt_baro: ground</i>) or when it is at or below <b>both</b> thresholds below; a value the "
        "provider does not send counts as 0. On-ground aircraft are normally not tracked or shown. The options "
        "below can show some of them; airborne aircraft always keep priority for the 200 aircraft slots. "
        "Aircraft shown here are tracked in Seen and History like any other, with the same ICAO24 identity "
        "when they take off; hidden ones are not tracked, as before.</small></p>"
        "<form id='visForm' method='post' action='/airports/visibility'><input type='hidden' name='action' value='save'>") != ESP_OK ||
        SendFormat(req, "<label>Inflight altitude threshold <input name='alt' type='number' min='0' max='%u' step='1' "
                        "value='%s' placeholder='default 15 m (49 ft)'> ft <small>blank = default 15 m</small></label>",
                   (unsigned)VIS_ALT_FT_MAX, altText) != ESP_OK ||
        SendFormat(req, "<label>Inflight speed threshold <input name='spd' type='number' min='0' max='200' step='0.1' "
                        "value='%s' placeholder='default 8.5 m/s (16.5 kt)'> kt <small>blank = default 8.5 m/s</small></label>",
                   spdText) != ESP_OK ||
        SendFormat(req, "<label>Provider on-ground status <select name='pg'>"
                        "<option value='0'%s>Use when the provider reports it (default)</option>"
                        "<option value='1'%s>Ignore (thresholds only)</option></select></label>",
                   v.providerGround == 0 ? " selected" : "", v.providerGround == 1 ? " selected" : "") != ESP_OK ||
        SendFormat(req, "<label>Aircraft on the ground at airports <select name='am'>"
                        "<option value='0'%s>Off (default)</option><option value='1'%s>Count only (number next to the airport)</option>"
                        "<option value='2'%s>Show aircraft</option></select></label>",
                   v.airportMode == 0 ? " selected" : "", v.airportMode == 1 ? " selected" : "",
                   v.airportMode == 2 ? " selected" : "") != ESP_OK ||
        SendFormat(req, "<label>Airport radius <input name='rad' type='number' min='0.5' max='10' step='0.1' value='%s'> km ",
                   radText) != ESP_OK ||
        Send(req, "<small>an on-ground aircraft this close to a shown airport, heliport (H) or user Dot/Directional "
                  "location belongs to it; default 3 km</small></label>"
                  "<fieldset><legend>Keep visible while on the ground, and for the stale timeout after the provider stops "
                  "reporting them</legend>") != ESP_OK ||
        SendFormat(req, "<label><input type='checkbox' name='rh' value='1'%s> Helicopters</label>"
                        "<label><input type='checkbox' name='ri' value='1'%s> INTERESTING aircraft</label>",
                   (v.retainMask & VIS_RETAIN_HELICOPTER) ? " checked" : "",
                   (v.retainMask & VIS_RETAIN_INTERESTING) ? " checked" : "") != ESP_OK ||
        SendFormat(req, "<label><input type='checkbox' name='rm' value='1'%s> IMPORTANT aircraft</label>"
                        "<label><input type='checkbox' name='rp' value='1'%s> Police and emergency (medical) aircraft</label>",
                   (v.retainMask & VIS_RETAIN_IMPORTANT) ? " checked" : "",
                   (v.retainMask & VIS_RETAIN_POLICE_EMERGENCY) ? " checked" : "") != ESP_OK ||
        Send(req, "<small>Uses the existing classification (Registered Aircraft, operators, built-in rules) and "
                  "Aircraft Type.</small></fieldset>") != ESP_OK ||
        SendFormat(req, "<label>Stale timeout <input name='stale' type='number' min='%u' max='%u' step='1' value='%u'> minutes ",
                   (unsigned)VIS_STALE_MIN_MIN, (unsigned)VIS_STALE_MIN_MAX, (unsigned)v.staleMinutes) != ESP_OK ||
        SendFormat(req, "<small>only for the kept aircraft above: after the provider's last report they stay at their "
                        "last position this long, then disappear (default %u)</small></label>"
                        "<button type='submit'>Save visibility</button></form>", (unsigned)VIS_STALE_MIN_DEFAULT) != ESP_OK ||
        Send(req, "<form class='inline' method='post' action='/airports/visibility'><input type='hidden' name='action' "
                  "value='reset'><button type='submit'>Reset visibility to defaults</button></form>") != ESP_OK)
        return ESP_FAIL;

    uint32_t associated = 0, shown = 0, retained = 0, stale = 0;
    VisPolicy_GetTotals(&associated, &shown, &retained, &stale);
    if (SendFormat(req, "<p><b>Latest poll:</b> %u on-ground aircraft at airports (%u of %u showing); "
                        "%u kept on the ground by the options above; %u stale (no longer reported).</p>",
                   (unsigned)associated, (unsigned)shown, (unsigned)associated, (unsigned)retained, (unsigned)stale) != ESP_OK)
        return ESP_FAIL;
    const size_t n = VisPolicy_AirportCountTotal();
    if (!n)
        return v.airportMode == VIS_AIRPORT_OFF
                   ? Send(req, "<p><small>Airport aircraft are off, so on-ground aircraft at airports are not counted.</small></p>")
                   : Send(req, "<p><small>No on-ground aircraft at a shown airport in the latest poll.</small></p>");
    if (Send(req, "<table><tr><th>Airport</th><th>On the ground</th></tr>") != ESP_OK)
        return ESP_FAIL;
    for (size_t i = 0; i < n; i++) {
        VisAirportCount c;
        if (!VisPolicy_GetAirportCount(i, &c))
            continue;
        if (Send(req, "<tr><td>") != ESP_OK || SendEscaped(req, c.name) != ESP_OK ||
            SendFormat(req, "</td><td>%u of %u showing</td></tr>", (unsigned)c.shown, (unsigned)c.associated) != ESP_OK)
            return ESP_FAIL;
    }
    return Send(req, "</table>");
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
    /* Same regional selection the radar uses for this center/range. */
    const size_t builtinCount = Airports_SelectBuiltins(centerLat, centerLon, radiusKm);
    const size_t userCount = Airports_Count();
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    if (WebStyle_SendHead(req, "Airports and Special Air Traffic", WEBPAGE_NONE,
            "body{max-width:850px}"
            "svg{width:min(100%,400px);height:auto;background:#0A1024;touch-action:none;cursor:crosshair}"
            "label{display:block;margin:.5em 0}button{margin:.2em;padding:.3em .6em}") != ESP_OK ||
        Send(req,
        "<h1>Airports and Special Air Traffic</h1>"
        "<p>Locations drawn on the radar come from two sources. <b>Built-in airports</b>: a curated "
        "database of major and regional airports with scheduled airline service worldwide, compiled into "
        "the firmware. Only the ones inside the current radar range are activated (major airports first, "
        "then regional ones, nearest first); the rest of the database stays inactive. "
        "<b>User-defined locations</b>: anything else you want on the radar, such as small, municipal or "
        "private airports and airfields, heliports and helipads, special air-traffic locations such as "
        "stadiums or major venues, and other places of personal interest. User-defined locations are "
        "always active and are never displaced by built-in airports; a user location whose name starts "
        "with an airport's ident (for example <i>KSJC ...</i>) replaces that built-in airport.</p>"
        "<p>This is a snapshot of the radar when this page opened. Click inside the outer ring, name the "
        "location, and save. Reload for recent aircraft. Saved locations stay at their latitude and "
        "longitude when you change radar range or center.</p>"
        "<p><small>Markers: <b>Dot</b> = generic location; <b>H</b> = heliport / helicopter location; "
        "<b>Directional</b> = airport with runway axis; <b>Square</b> = special aviation-interest location. "
        "Locations are reference markers only: an aircraft near one is <b>not</b> classified, tagged or "
        "tracked differently because of it.</small></p>") != ESP_OK ||
        SendFormat(req, "<p>Radar center: %s, %s &middot; radius: %s km</p>",
                   latText, lonText, rangeText) != ESP_OK ||
        SendFormat(req, "<p><b>Active locations: %u of %u</b> (%u user-defined + %u built-in). "
                   "User-defined: %u of %u saved. Built-in database: %u airports, %u active within the "
                   "current radius.</p>",
                   (unsigned)(userCount + builtinCount), (unsigned)AIRPORT_ACTIVE_MAX,
                   (unsigned)userCount, (unsigned)builtinCount, (unsigned)userCount, (unsigned)MAX_AIRPORTS,
                   (unsigned)Airports_BuiltinTotal(), (unsigned)builtinCount) != ESP_OK ||
        Send(req,
        "<svg id='map' viewBox='0 0 400 400' aria-label='Radar location placement preview'>"
        "<circle cx='200' cy='200' r='190' fill='none' stroke='#00B849' stroke-width='2'/>"
        "<circle cx='200' cy='200' r='127' fill='none' stroke='#00B849' opacity='.7'/>"
        "<circle cx='200' cy='200' r='63' fill='none' stroke='#00B849' opacity='.7'/>"
        "<path d='M200 10V390M10 200H390' stroke='#00B849' opacity='.35'/>"
        "<text x='195' y='25' fill='#00E060'>N</text><text x='373' y='194' fill='#00E060'>E</text>"
        "<text x='195' y='385' fill='#00E060'>S</text><text x='15' y='194' fill='#00E060'>W</text>") != ESP_OK)
        return ESP_FAIL;

    /* Location markers precede aircraft polygons so aircraft cover them;
     * built-in airports first so user-defined locations sit on top, as on the radar.
     * builtinShowing counts the active built-ins actually drawn (same test as radar.c). */
    size_t builtinShowing = 0;
    unsigned hiddenByOverride = 0, movedOutside = 0, notDrawnOther = 0;
    for (size_t i = 0; i < builtinCount; i++) {
        AirportMarker marker;
        AirportBuiltinView view;
        int x, y;
        if (!Airports_GetActiveBuiltinView(i, &marker, &view))
            continue;
        if (view.hidden) {
            hiddenByOverride++;
            continue;
        }
        if (!Radar_ProjectPosition(marker.latitude, marker.longitude,
                                   centerLat, centerLon, radiusKm, 190, &x, &y)) {
            if (view.overrideFields & (AIRPORT_OVR_LAT | AIRPORT_OVR_LON))
                movedOutside++;
            else
                notDrawnOther++;
            continue;
        }
        builtinShowing++;
        if (SendAirportMarker(req, &marker, 200 + x, 200 + y, view.hasAxis ? &view.axisDeg : NULL) != ESP_OK)
            return ESP_FAIL;
    }
    size_t airportCount = userCount;
    for (size_t i = 0; i < airportCount; i++) {
        AirportMarker marker;
        int x, y;
        if (!Airports_Get(i, &marker) ||
            !Radar_ProjectPosition(marker.latitude, marker.longitude,
                                   centerLat, centerLon, radiusKm, 190, &x, &y))
            continue;
        if (SendAirportMarker(req, &marker, 200 + x, 200 + y, NULL) != ESP_OK)
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
        CraftResolution resolved = ResolveAircraftWithHint(
            a.callsign, a.icao24, a.providerTypeHint, a.hasProviderTypeHint);
        /* Same resolve -> appearance path radar.c uses, so this preview always
         * matches the panel exactly (helicopter ring color, Important's
         * yellow-fill/red-border, and anything added later). */
        CraftAppearance appearance = CraftType_Appearance(resolved.type, resolved.aircraftType);
        char fill[8];
        snprintf(fill, sizeof(fill), "#%06X", (unsigned)(appearance.colorRgb & 0xFFFFFFu));
        const bool haveHeading = isfinite(a.heading);
        const float h = haveHeading ? a.heading * 0.0174532925f : 0.0f;
        esp_err_t err;
        if (resolved.aircraftType == AIRCRAFT_HELICOPTER) {
            /* Same solid-circle-with-ring the radar draws, scaled to this
             * preview's SVG units (viewBox is half the panel's screen pixels). */
            const int r = HELI_MARKER_DIAMETER_PX / 4;
            err = SendFormat(req,
                "<circle cx='%d' cy='%d' r='%d' fill='%s' "
                "stroke='#%06X' stroke-width='%d'/>",
                x, y, r, fill,
                (unsigned)appearance.ringRgb, appearance.ringWidthPx / 2);
            /* Directional heading indicator, mirroring the panel: a short
             * line in the same color as the ring, only drawn for a finite
             * heading so it never implies a direction that isn't known. */
            if (err == ESP_OK && haveHeading) {
                const int lineRadius = r - appearance.ringWidthPx / 2;
                if (lineRadius > 0)
                    err = SendFormat(req,
                        "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#%06X' stroke-width='1'/>",
                        x, y, x + (int)(sinf(h) * lineRadius), y - (int)(cosf(h) * lineRadius),
                        (unsigned)appearance.ringRgb);
            }
        } else if (resolved.aircraftType == AIRCRAFT_OTHER) {
            /* Diamond, mirroring the panel's marker: classification-colored
             * body, an outline only if this classification has one
             * (Important), and a fixed gray/black forward tip when heading
             * is known. Without a heading the diamond still renders, just
             * pointed "up" and without the tip. */
            const int size = 6, side = 4;
            const int nx = x + (int)(sinf(h) * size), ny = y - (int)(cosf(h) * size);
            const int rx = x + (int)(sinf(h + 1.5707963f) * side), ry = y - (int)(cosf(h + 1.5707963f) * side);
            const int tx = x + (int)(sinf(h + 3.1415927f) * side), ty = y - (int)(cosf(h + 3.1415927f) * side);
            const int lx = x + (int)(sinf(h - 1.5707963f) * side), ly = y - (int)(cosf(h - 1.5707963f) * side);
            if (appearance.ringWidthPx > 0)
                err = SendFormat(req,
                    "<polygon points='%d,%d %d,%d %d,%d %d,%d' fill='%s' stroke='#%06X' stroke-width='1'/>",
                    nx, ny, rx, ry, tx, ty, lx, ly, fill, (unsigned)appearance.ringRgb);
            else
                err = SendFormat(req,
                    "<polygon points='%d,%d %d,%d %d,%d %d,%d' fill='%s' opacity='.9'/>",
                    nx, ny, rx, ry, tx, ty, lx, ly, fill);
            if (err == ESP_OK && haveHeading) {
                const float tipDist = size * 0.5f;
                const int tlx = x + (int)(sinf(h + 1.0f) * tipDist), tly = y - (int)(cosf(h + 1.0f) * tipDist);
                const int trx = x + (int)(sinf(h - 1.0f) * tipDist), tryY = y - (int)(cosf(h - 1.0f) * tipDist);
                err = SendFormat(req,
                    "<polygon points='%d,%d %d,%d %d,%d' fill='#%06X'/>",
                    nx, ny, tlx, tly, trx, tryY, (unsigned)OTHER_TIP_RGB);
            }
        } else if (appearance.ringWidthPx > 0) {
            /* Important: yellow fill with a red outline - the closest SVG
             * equivalent to the panel's filled-triangle-plus-stroke marker. */
            err = SendFormat(req,
                "<polygon points='%d,%d %d,%d %d,%d' fill='%s' stroke='#%06X' stroke-width='1' opacity='.95'/>",
                x, y - 6, x - 5, y + 4, x + 5, y + 4, fill, (unsigned)appearance.ringRgb);
        } else {
            err = SendFormat(req,
                "<polygon points='%d,%d %d,%d %d,%d' fill='%s' opacity='.9'/>",
                x, y - 6, x - 5, y + 4, x + 5, y + 4, fill);
        }
        if (err != ESP_OK)
            return ESP_FAIL;
    }

    if (Send(req,
        "<circle id='candidate' cx='200' cy='200' r='6' fill='#FF0000' "
        "stroke='white' stroke-width='2' visibility='hidden'/></svg>"
        "<p><small>Dots, H marks, runway markers and squares are locations (user-defined ones in their chosen color, built-in airports in red unless given an override color); triangles are fixed-wing "
        "aircraft, ringed dots are helicopters, and diamonds are other aircraft types, all at page load. "
        "The white-rimmed red dot is your unsaved placement.</small></p>"
        "<h2 id='editor'>Add or edit location</h2>"
        "<form id='airportForm' method='post' action='/airports/save'>"
        "<input id='index' name='index' type='hidden' value='-1'>"
        "<label>Name <input id='name' name='name' maxlength='24' required></label>"
        "<label>Latitude <input id='latitude' name='latitude' type='number' step='any' min='-90' max='90' required></label>"
        "<label>Longitude <input id='longitude' name='longitude' type='number' step='any' min='-180' max='180' required></label>"
        "<label>Marker size <input id='diameter' name='diameter' type='number' min='6' max='24' value='12' required> pixels</label>"
        "<label>Marker color <input id='color' name='color' type='color' value='#FF0000' required></label>"
        "<label>Marker type <select id='mode' name='mode'><option value='0'>Dot (generic location)</option>"
        "<option value='2'>H (heliport / helicopter location)</option>"
        "<option value='1'>Directional (airport, runway axis)</option>"
        "<option value='3'>Square (special aviation-interest location)</option></select></label>"
        "<label>Primary runway <input id='runway' name='runway' maxlength='3' placeholder='e.g. 09L' "
        "pattern='[0-9]{1,2}[LCRlcr]?' title='1-2 digit runway number (01-36), optional L/C/R'> "
        "<small>Only used in Directional mode. Reciprocal ends (09/27, 18/36, ...) point the same way. "
        "Left blank or unrecognized falls back to the dot automatically.</small></label>"
        "<button type='submit'>Save location</button><button type='button' onclick='newAirport()'>New location</button>"
        "</form><h2>User-defined locations</h2><table><tr><th>Name</th><th>Position</th><th>Size</th><th>Color</th><th>Marker</th><th></th></tr>") != ESP_OK)
        return ESP_FAIL;
    if (!airportCount && Send(req, "<tr><td colspan='6'>No locations saved</td></tr>") != ESP_OK)
        return ESP_FAIL;
    for (size_t i = 0; i < airportCount; i++) {
        AirportMarker marker;
        char airportLat[24], airportLon[24];
        if (!Airports_Get(i, &marker)) continue;
        FormatCoordinate(marker.latitude, airportLat);
        FormatCoordinate(marker.longitude, airportLon);
        char colorHex[8];
        snprintf(colorHex, sizeof(colorHex), "#%06X", (unsigned)(marker.color & 0xFFFFFFu));
        const bool showsDirectional =
            Airport_EffectiveShape(&marker, NULL) == AIRPORT_MARKER_DIRECTIONAL;
        if (Send(req, "<tr><td>") != ESP_OK ||
            SendEscaped(req, marker.name) != ESP_OK ||
            SendFormat(req,
                "</td><td>%s, %s</td><td>%u px</td>"
                "<td><span style='display:inline-block;width:1em;height:1em;vertical-align:middle;"
                "border:1px solid #888;background:%s'></span> %s</td>",
                airportLat, airportLon, (unsigned)marker.diameter, colorHex, colorHex) != ESP_OK)
            return ESP_FAIL;
        if (marker.markerMode == AIRPORT_MARKER_DIRECTIONAL) {
            if (SendFormat(req, "<td>Directional, runway %s%s</td>",
                           marker.runway[0] ? marker.runway : "(none)",
                           showsDirectional ? "" : " &mdash; unrecognized, showing dot") != ESP_OK)
                return ESP_FAIL;
        } else if (SendFormat(req, "<td>%s</td>", Airport_MarkerTypeName(marker.markerMode)) != ESP_OK) {
            return ESP_FAIL;
        }
        if (SendFormat(req,
                "<td><button type='button' data-index='%u' data-lat='%s' data-lon='%s' data-size='%u' "
                "data-color='%s' data-mode='%u' data-runway='%s' data-name='",
                (unsigned)i, airportLat, airportLon, (unsigned)marker.diameter, colorHex,
                (unsigned)marker.markerMode, marker.runway) != ESP_OK ||
            SendEscaped(req, marker.name) != ESP_OK ||
            Send(req, "' onclick='editAirport(this)'>Edit</button>"
                      "<form class='inline' method='post' action='/airports/delete'>") != ESP_OK ||
            SendFormat(req, "<input type='hidden' name='index' value='%u'>", (unsigned)i) != ESP_OK ||
            Send(req, "<button type='submit'>Remove</button></form></td></tr>") != ESP_OK)
            return ESP_FAIL;
    }
    if (Send(req,
        "</table><p>Up to 100 user-defined locations. Locations outside the current radar range remain saved "
        "and appear when the range includes them.</p>"
        "<h2>Built-in airports active now</h2>") != ESP_OK ||
        SendBuiltinCountLine(req, builtinShowing, builtinCount, hiddenByOverride, movedOutside, notDrawnOther) != ESP_OK ||
        Send(req,
        "<p><small>Selected automatically for the current radar center and radius; changing the radius or "
        "center changes this list. The built-in database itself is read-only, but each airport can have your "
        "own <b>override</b> (hide it, move it, set its runway rotation, or change its display color). Fields you do not override keep the "
        "database value; <i>Reset to Default</i> deletes the override. Overrides are kept when the airport "
        "leaves the radar area and do not use a user-defined location slot.</small></p>"
        "<table><tr><th>Airport</th><th>Class</th><th>Distance</th><th>Marker</th><th>Values</th><th></th></tr>") != ESP_OK)
        return ESP_FAIL;
    if (!builtinCount && Send(req, "<tr><td colspan='6'>No built-in airport within the current radius</td></tr>") != ESP_OK)
        return ESP_FAIL;
    for (size_t i = 0; i < builtinCount; i++) {
        AirportMarker marker;
        AirportBuiltinView view;
        uint8_t tier = 0;
        float distanceKm = 0.0f;
        if (!Airports_GetActiveBuiltinView(i, &marker, &view) || !Airports_GetActiveBuiltinInfo(i, &tier, &distanceKm))
            continue;
        char icao[5];
        IdentOfBuiltinName(marker.name, icao);
        char markerText[48];
        if (view.hidden)
            snprintf(markerText, sizeof(markerText), "Hidden");
        else if (view.hasAxis)
            snprintf(markerText, sizeof(markerText), "Directional %ld&deg;", lroundf(view.axisDeg));
        else
            snprintf(markerText, sizeof(markerText), "Dot");
        if (Send(req, "<tr><td>") != ESP_OK || SendEscaped(req, marker.name) != ESP_OK ||
            SendFormat(req, "</td><td>%s</td><td>%ld km</td><td>%s</td><td>",
                       tier == 1 ? "Major" : "Regional", lroundf(distanceKm), markerText) != ESP_OK ||
            SendOverrideState(req, view.overrideFields) != ESP_OK ||
            Send(req, "</td><td>") != ESP_OK ||
            SendOverrideActions(req, icao) != ESP_OK ||
            Send(req, "</td></tr>") != ESP_OK)
            return ESP_FAIL;
    }
    if (Send(req, "</table>") != ESP_OK || SendOverrideEditor(req) != ESP_OK || SendVisibilitySection(req) != ESP_OK)
        return ESP_FAIL;
    if (Send(req,
        "<script>"
        "function ovrEdit(b){var f=document.getElementById('ovrForm'),d=b.dataset;"
        "f.elements['icao'].value=d.icao;document.getElementById('ovrName').textContent=d.name;"
        "f.elements['vis'].value=d.hidden==='1'?'hide':'show';"
        "f.elements['olat'].value=d.olat;f.elements['olon'].value=d.olon;f.elements['orot'].value=d.orot;"
        "f.elements['olat'].placeholder='database: '+d.dlat;f.elements['olon'].placeholder='database: '+d.dlon;"
        "f.elements['orot'].placeholder=d.drot?('database: '+d.drot+'\u00b0'):'database: none (Dot)';"
        "f.elements['ocolset'].checked=!!d.ocol;f.elements['ocolor'].value=d.ocol||d.dcol;"
        "document.getElementById('ovrEditor').hidden=false;location.hash='ovrEditor';}"
        "</script>"
        "<script>const map=document.getElementById('map'),dot=document.getElementById('candidate');"
        "const centerLat=") != ESP_OK ||
        Send(req, latText) != ESP_OK || Send(req, ",centerLon=") != ESP_OK ||
        Send(req, lonText) != ESP_OK || Send(req, ",rangeKm=") != ESP_OK ||
        Send(req, rangeText) != ESP_OK ||
        Send(req,
        ";const f=document.getElementById('airportForm');"
        "const latInput=document.getElementById('latitude'),lonInput=document.getElementById('longitude');"
        "const nameInput=document.getElementById('name'),sizeInput=document.getElementById('diameter');"
        "const colorInput=document.getElementById('color');"
        "const modeInput=document.getElementById('mode'),runwayInput=document.getElementById('runway');"
        "const indexInput=document.getElementById('index');"
        "function preview(){let lat=Number(latInput.value),lon=Number(lonInput.value);"
        "if(!latInput.value||!lonInput.value){dot.setAttribute('visibility','hidden');return;}"
        "let east=(lon-centerLon)*111*Math.cos(centerLat*Math.PI/180),north=(lat-centerLat)*111;"
        "let x=200+east/rangeKm*190,y=200-north/rangeKm*190;"
        "if(!Number.isFinite(x)||!Number.isFinite(y)||Math.hypot(x-200,y-200)>190){"
        "dot.setAttribute('visibility','hidden');return;}"
        "dot.setAttribute('cx',x);dot.setAttribute('cy',y);dot.setAttribute('fill',colorInput.value);"
        "dot.setAttribute('r',Number(sizeInput.value)/2||6);dot.setAttribute('visibility','visible');}"
        "map.addEventListener('click',function(e){let p=map.createSVGPoint();p.x=e.clientX;p.y=e.clientY;"
        "p=p.matrixTransform(map.getScreenCTM().inverse());"
        "let dx=p.x-200,dy=p.y-200;if(Math.hypot(dx,dy)>190)return;"
        "latInput.value=(centerLat-dy/190*rangeKm/111).toFixed(6);"
        "lonInput.value=(centerLon+dx/190*rangeKm/(111*Math.cos(centerLat*Math.PI/180))).toFixed(6);"
        "preview();});"
        "function newAirport(){f.reset();indexInput.value='-1';sizeInput.value='12';colorInput.value='#FF0000';"
        "modeInput.value='0';runwayInput.value='';"
        "dot.setAttribute('visibility','hidden');location.hash='editor';}"
        "function editAirport(b){indexInput.value=b.dataset.index;nameInput.value=b.dataset.name;"
        "latInput.value=b.dataset.lat;lonInput.value=b.dataset.lon;"
        "sizeInput.value=b.dataset.size;colorInput.value=b.dataset.color;"
        "modeInput.value=b.dataset.mode;runwayInput.value=b.dataset.runway;"
        "preview();location.hash='editor';}"
        "['latitude','longitude','diameter','color'].forEach(id=>document.getElementById(id).addEventListener('input',preview));"
        "</script></body></html>") != ESP_OK)
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

/* "#RRGGBB" (the native <input type=color> format) -> 0xRRGGBB. */
static bool ParseHtmlColor(const char *text, uint32_t *out)
{
    if (text[0] != '#' || strlen(text) != 7)
        return false;
    long value = 0;
    for (int i = 1; i < 7; i++) {
        int digit = HexDigit(text[i]);
        if (digit < 0)
            return false;
        value = value * 16 + digit;
    }
    *out = (uint32_t)value;
    return true;
}

static esp_err_t SaveAirport(httpd_req_t *req)
{
    char body[256], indexText[12], name[64], latText[32], lonText[32], sizeText[12], colorText[16];
    char modeText[4], runwayText[AIRPORT_RUNWAY_LENGTH + 1];
    long index, diameter, mode;
    AirportMarker marker = {0};
    if (!ReadForm(req, body) ||
        !GetField(body, "index", indexText, sizeof(indexText)) ||
        !GetField(body, "name", name, sizeof(name)) ||
        !GetField(body, "latitude", latText, sizeof(latText)) ||
        !GetField(body, "longitude", lonText, sizeof(lonText)) ||
        !GetField(body, "diameter", sizeText, sizeof(sizeText)) ||
        !GetField(body, "color", colorText, sizeof(colorText)) ||
        !GetField(body, "mode", modeText, sizeof(modeText)) ||
        !GetField(body, "runway", runwayText, sizeof(runwayText)) ||
        !ParseLong(indexText, &index) || !ParseLong(sizeText, &diameter) ||
        !ParseLong(modeText, &mode) ||
        !ParseFloat(latText, &marker.latitude) ||
        !ParseFloat(lonText, &marker.longitude) ||
        !ParseHtmlColor(colorText, &marker.color) ||
        index < -1 || index >= MAX_AIRPORTS ||
        diameter < 6 || diameter > 24 || strlen(name) > AIRPORT_NAME_LENGTH ||
        mode < 0 || !Airport_MarkerTypeValid((unsigned)mode) ||
        strlen(runwayText) > AIRPORT_RUNWAY_LENGTH)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid location details");
    /* A runway that doesn't parse (empty, out of range, malformed) is not a
     * rejected submission - it just means this airport falls back to the
     * dot at render time, the same as if directional mode were never picked.
     * See Airport_ParseRunwayAxis / PHASE 5 "Missing / Invalid Orientation". */
    strcpy(marker.name, name);
    marker.diameter = (uint8_t)diameter;
    marker.markerMode = (uint8_t)mode;
    for (char *c = runwayText; *c; c++)
        *c = (char)toupper((unsigned char)*c);
    strcpy(marker.runway, runwayText);
    if (marker.name[0] == '\0' || marker.latitude < -90 || marker.latitude > 90 ||
        marker.longitude < -180 || marker.longitude > 180)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid location position or name");
    if (!Airports_Save((int)index, &marker))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not save location (storage error or 100-location limit)");
    Radar_RequestRedraw(); /* show it even while the zero-traffic radar is frozen */
    return Redirect(req);
}

static esp_err_t DeleteAirport(httpd_req_t *req)
{
    char body[256], indexText[12];
    long index;
    if (!ReadForm(req, body) ||
        !GetField(body, "index", indexText, sizeof(indexText)) ||
        !ParseLong(indexText, &index) || index < 0 || index >= MAX_AIRPORTS)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid location selection");
    if (!Airports_Delete((size_t)index))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not remove location");
    Radar_RequestRedraw();
    return Redirect(req);
}

/* POST /airports/override: icao, action=save|reset, vis=show|hide, olat, olon, orot,
 * ocolset=1 + ocolor=#RRGGBB (0.0.28 display color).
 * A blank field means "database default" (not stored). Saving with nothing
 * overridden removes the record, exactly like Reset to Default. */
static esp_err_t SaveOverride(httpd_req_t *req)
{
    char body[256], icao[8], action[8], vis[8] = "", latText[32] = "", lonText[32] = "", rotText[12] = "",
         colSet[4] = "", colText[16] = "";
    if (!ReadForm(req, body) || !GetField(body, "icao", icao, sizeof(icao)) ||
        !GetField(body, "action", action, sizeof(action)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid override request");
    if (!strcmp(action, "reset")) {
        if (!Airports_ResetOverride(icao))
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not reset override");
        Radar_RequestRedraw();
        return Redirect(req);
    }
    if (strcmp(action, "save"))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid override request");
    GetField(body, "vis", vis, sizeof(vis));
    GetField(body, "olat", latText, sizeof(latText));
    GetField(body, "olon", lonText, sizeof(lonText));
    GetField(body, "orot", rotText, sizeof(rotText));
    GetField(body, "ocolset", colSet, sizeof(colSet));
    GetField(body, "ocolor", colText, sizeof(colText));

    AirportBuiltinOverride o = {0};
    memcpy(o.icao, icao, strnlen(icao, sizeof(o.icao)));
    if (strlen(icao) > sizeof(o.icao))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown built-in airport");
    if (!strcmp(vis, "hide"))
        o.fields |= AIRPORT_OVR_HIDDEN;
    else if (vis[0] && strcmp(vis, "show"))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid visibility");
    float value;
    if (latText[0]) {
        if (!ParseFloat(latText, &value) || value < -90.0f || value > 90.0f)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Latitude must be -90 to 90");
        o.fields |= AIRPORT_OVR_LAT;
        o.latE5 = (int32_t)lroundf(value * 1e5f);
    }
    if (lonText[0]) {
        if (!ParseFloat(lonText, &value) || value < -180.0f || value > 180.0f)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Longitude must be -180 to 180");
        o.fields |= AIRPORT_OVR_LON;
        o.lonE5 = (int32_t)lroundf(value * 1e5f);
    }
    if (rotText[0]) {
        long rot;
        if (!ParseLong(rotText, &rot) || rot < 0 || rot > 359)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Rotation must be 0 to 359 degrees");
        o.fields |= AIRPORT_OVR_ROTATION;
        o.rotationDeg = (uint16_t)(rot % 180); /* a runway axis: 142 and 322 are the same line */
    }
    if (!strcmp(colSet, "1")) {
        /* Same "#RRGGBB" input and parser as a user-defined location's color. */
        if (!ParseHtmlColor(colText, &o.color))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Color must be #RRGGBB");
        o.fields |= AIRPORT_OVR_COLOR;
    }
    if (!Airports_SetOverride(&o))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not save override (unknown airport, 100-override limit or storage error)");
    Radar_RequestRedraw();
    return Redirect(req);
}

/* POST /airports/visibility: action=save|reset and the visibility form fields. */
static esp_err_t SaveVisibility(httpd_req_t *req)
{
    char body[256], action[8], text[16];
    if (!ReadForm(req, body) || !GetField(body, "action", action, sizeof(action)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid visibility request");
    VisSettings v;
    VisPolicy_DefaultSettings(&v);
    if (!strcmp(action, "save")) {
        long value;
        float f;
        if (GetField(body, "alt", text, sizeof(text)) && text[0]) {
            if (!ParseLong(text, &value) || value < 0 || value > (long)VIS_ALT_FT_MAX)
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Altitude threshold must be 0 to 5000 ft");
            v.minAltFt = (uint16_t)value;
        }
        if (GetField(body, "spd", text, sizeof(text)) && text[0]) {
            if (!ParseFloat(text, &f) || f < 0.0f || f > 200.0f)
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Speed threshold must be 0 to 200 kt");
            v.minSpeedKt10 = (uint16_t)lroundf(f * 10.0f);
        }
        if (GetField(body, "pg", text, sizeof(text))) {
            if (!ParseLong(text, &value) || value < 0 || value > 1)
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid provider on-ground option");
            v.providerGround = (uint8_t)value;
        }
        if (GetField(body, "am", text, sizeof(text))) {
            if (!ParseLong(text, &value) || value < 0 || value > 2)
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid airport aircraft option");
            v.airportMode = (uint8_t)value;
        }
        if (GetField(body, "rad", text, sizeof(text)) && text[0]) {
            if (!ParseFloat(text, &f) || f < 0.5f || f > 10.0f)
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Airport radius must be 0.5 to 10 km");
            v.airportRadiusM = (uint16_t)lroundf(f * 1000.0f);
        }
        if (GetField(body, "stale", text, sizeof(text)) && text[0]) {
            if (!ParseLong(text, &value) || value < (long)VIS_STALE_MIN_MIN || value > (long)VIS_STALE_MIN_MAX)
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Stale timeout must be 1 to 60 minutes");
            v.staleMinutes = (uint16_t)value;
        }
        v.retainMask = 0;
        if (GetField(body, "rh", text, sizeof(text)))
            v.retainMask |= VIS_RETAIN_HELICOPTER;
        if (GetField(body, "ri", text, sizeof(text)))
            v.retainMask |= VIS_RETAIN_INTERESTING;
        if (GetField(body, "rm", text, sizeof(text)))
            v.retainMask |= VIS_RETAIN_IMPORTANT;
        if (GetField(body, "rp", text, sizeof(text)))
            v.retainMask |= VIS_RETAIN_POLICE_EMERGENCY;
    } else if (strcmp(action, "reset")) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid visibility request");
    }
    if (!VisPolicy_SetSettings(&v))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save visibility settings");
    Radar_RequestRedraw();
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
    const httpd_uri_t override = {.uri = "/airports/override", .method = HTTP_POST, .handler = SaveOverride};
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &override);
    const httpd_uri_t visibility = {.uri = "/airports/visibility", .method = HTTP_POST, .handler = SaveVisibility};
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &visibility);
    return err;
}
