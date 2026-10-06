#include "web_seen.h"
#include "esp_heap_caps.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "craft_types.h"
#include "custom_rules.h"
#include "feature_flags.h"
#include "pattern_match.h"
#include "seen_aircraft.h"
#include "time_util.h"
#include "web_style.h"
#include "web_util.h"

static const char *TAG = "WebSeen";

#define SEEN_PAGE_SIZE 50
#define SEARCH_MAX 40

static esp_err_t SendChunk(httpd_req_t *req, const char *text)
{
    return httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
}

/* Sends a formatted fragment through one bounded stack buffer. Only used with
 * literal formats; every dynamic value is an escaped %s argument, so a '%'
 * inside user data can never reach the format string. */
static esp_err_t SendFormat(httpd_req_t *req, const char *format, ...) __attribute__((format(printf, 2, 3)));
static esp_err_t SendFormat(httpd_req_t *req, const char *format, ...)
{
    char buf[2560]; /* the widest row is ~1.7 KB with worst-case escaping */
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n < 0 || n >= (int)sizeof(buf))
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, buf, (ssize_t)n);
}

/* ---- query parameters ---- */

static const struct { const char *token; SeenSortKey key; const char *label; bool defaultDescending; } sortChoices[] = {
    {"last", SEEN_SORT_LAST_SEEN, "Last seen", true},
    {"first", SEEN_SORT_FIRST_SEEN, "First seen", true},
    {"count", SEEN_SORT_COUNT, "Seen count", true},
    {"call", SEEN_SORT_CALLSIGN, "Call sign", false},
    {"icao", SEEN_SORT_ICAO24, "ICAO24", false},
    {"op", SEEN_SORT_OPERATOR, "Operator", false},
};
#define SORT_CHOICES (sizeof(sortChoices) / sizeof(sortChoices[0]))

static const struct { const char *token; SeenFilter filter; const char *label; } filterChoices[] = {
    {"all", SEEN_FILTER_ALL, "All aircraft"},
    {"cfg", SEEN_FILTER_CONFIGURED, "Configured (registry or operator)"},
    {"not", SEEN_FILTER_NOT_CONFIGURED, "Not configured"},
};
#define FILTER_CHOICES (sizeof(filterChoices) / sizeof(filterChoices[0]))

typedef struct {
    char search[SEARCH_MAX + 1];
    SearchField searchField; /* "sf": e / c / r */
    bool patternMode;        /* "pm=1" */
    size_t sortIndex;
    size_t filterIndex;
    bool descending;
    size_t page; /* zero-based */
} PageQuery;

static void ParseQuery(httpd_req_t *req, PageQuery *pq)
{
    memset(pq, 0, sizeof(*pq));
    pq->descending = sortChoices[0].defaultDescending;

    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return;

    char value[SEARCH_MAX * 3 + 1];
    if (httpd_query_key_value(query, "q", value, sizeof(value)) == ESP_OK) {
        WebUtil_UrlDecodeInPlace(value);
        /* Printable ASCII only, trimmed; the rest is dropped. */
        size_t used = 0;
        for (const char *p = value; *p && used < SEARCH_MAX; p++) {
            if ((unsigned char)*p >= 0x20 && (unsigned char)*p <= 0x7E && !(used == 0 && *p == ' '))
                pq->search[used++] = *p;
        }
        while (used > 0 && pq->search[used - 1] == ' ')
            used--;
        pq->search[used] = '\0';
    }
    if (httpd_query_key_value(query, "sf", value, sizeof(value)) == ESP_OK)
        pq->searchField = SearchField_FromToken(value);
    if (httpd_query_key_value(query, "pm", value, sizeof(value)) == ESP_OK)
        pq->patternMode = strcmp(value, "1") == 0;
    if (httpd_query_key_value(query, "f", value, sizeof(value)) == ESP_OK) {
        for (size_t i = 0; i < FILTER_CHOICES; i++)
            if (strcmp(value, filterChoices[i].token) == 0)
                pq->filterIndex = i;
    }
    if (httpd_query_key_value(query, "s", value, sizeof(value)) == ESP_OK) {
        for (size_t i = 0; i < SORT_CHOICES; i++)
            if (strcmp(value, sortChoices[i].token) == 0) {
                pq->sortIndex = i;
                pq->descending = sortChoices[i].defaultDescending;
            }
    }
    if (httpd_query_key_value(query, "d", value, sizeof(value)) == ESP_OK) {
        if (strcmp(value, "d") == 0) pq->descending = true;
        else if (strcmp(value, "a") == 0) pq->descending = false;
    }
    if (httpd_query_key_value(query, "p", value, sizeof(value)) == ESP_OK) {
        long p = strtol(value, NULL, 10);
        if (p > 0 && p < 100000)
            pq->page = (size_t)(p - 1);
    }
}

/* ---- page ---- */

static void ConfiguredText(const SeenConfigInfo *info, char *out, size_t cap)
{
    char escaped[128];
    if (info->source == CRAFT_SRC_REGISTRY) {
        char label[MAX_RULE_LABEL];
        CustomRules_EntryLabel(info->registryIcao24, info->registryPrefix, label, sizeof(label));
        WebUtil_EscapeHtml(escaped, sizeof(escaped), label);
        snprintf(out, cap, "Registry: %s", escaped);
    } else if (info->operatorCode[0]) {
        WebUtil_EscapeHtml(escaped, sizeof(escaped), info->operatorCode);
        snprintf(out, cap, "Operator: %s", escaped);
    } else if (info->source == CRAFT_SRC_PREFIX_DEFAULT) {
        snprintf(out, cap, "Built-in rule");
    } else {
        snprintf(out, cap, "No");
    }
}

/* Search Help popup (0.0.28). Describes exactly what MatchesSearch
 * (seen_aircraft.c) and pattern_match.c do; example links run a real search
 * with the existing query parameters (q, sf, pm). */
static const char kSeenSearchHelp[] =
    "<h4>Normal search</h4>"
    "<p>Not case-sensitive. Plain text (no <code>?</code> or <code>*</code>) matches anywhere in the value, "
    "e.g. <a href='/seen?q=N12345'>N12345</a> or <a href='/seen?q=ual&amp;sf=c'>ual</a> (any call sign containing UAL).</p>"
    "<p><b>In</b> chooses what is searched:</p><ul>"
    "<li><b>Either</b>: plain text searches the ICAO24, call sign, provider operator, your registry rule and its note, "
    "and your configured operator; a <code>?</code>/<code>*</code> or Pattern search checks the call sign and your registry rule.</li>"
    "<li><b>Call sign</b>: the call sign only.</li>"
    "<li><b>Registration</b>: Seen does not store registrations, so this searches the call sign (light aircraft often fly "
    "their registration as the call sign) and the Registered Aircraft rule that matched the aircraft.</li></ul>"
    "<h4>Wildcards</h4><ul>"
    "<li><code>?</code> = exactly one character, <code>*</code> = zero or more characters.</li>"
    "<li>With a wildcard the whole value must match: "
    "<a href='/seen?q=ABC%3F&amp;sf=c'>ABC?</a> = four characters starting ABC; "
    "<a href='/seen?q=*ABC*&amp;sf=c'>*ABC*</a> = contains ABC anywhere; "
    "<a href='/seen?q=UAL*&amp;sf=c'>UAL*</a> = starts with UAL.</li></ul>"
    "<h4>Pattern mode</h4>"
    "<p>Only when <b>Pattern mode</b> is ticked: <code>L</code> = exactly one letter, <code>N</code> = exactly one digit, "
    "plus <code>?</code> and <code>*</code>; everything else is literal and the whole value must match. "
    "<a href='/seen?q=LLLNNNNL&amp;sf=c&amp;pm=1'>LLLNNNNL</a> = three letters, four digits, one letter; "
    "<a href='/seen?q=LLL*&amp;sf=c&amp;pm=1'>LLL*</a> = starts with three letters; "
    "<a href='/seen?q=NNN*&amp;sf=c&amp;pm=1'>NNN*</a> = starts with three digits.</p>"
    "<p>Without Pattern mode, L and N are ordinary letters: <a href='/seen?q=LLL123'>LLL123</a> looks for the text LLL123.</p>"
    "<p><small>Example links start a new search (filter and sort return to their defaults). Registered Aircraft rules on "
    "/registered use their own syntax, not this one.</small></p>";

static esp_err_t SendRecordRow(httpd_req_t *req, const SeenRecord *r)
{
    SeenConfigInfo info;
    SeenAircraft_Describe(r, &info);

    char icon[240];
    if (CraftType_IconUse(r->craftType, r->aircraftType, icon, sizeof(icon)) == 0)
        icon[0] = '\0';

    char eIcao[64], eCs[64], eOp[256], eTitle[MAX_RULE_NOTES * 6 + 1], eCfg[192];
    WebUtil_EscapeHtml(eIcao, sizeof(eIcao), r->icao24);
    WebUtil_EscapeHtml(eCs, sizeof(eCs), r->callsign);
    WebUtil_EscapeHtml(eOp, sizeof(eOp), r->operatorName);
    /* Tooltip on the Configured cell: the registry note, else the configured operator. */
    WebUtil_EscapeHtml(eTitle, sizeof(eTitle), info.registryNote[0] ? info.registryNote : info.configuredOperator);
    ConfiguredText(&info, eCfg, sizeof(eCfg));

    /* Links into the existing Add/Edit dialog on /registered. */
    char encCs[SEEN_CALLSIGN_MAX * 3 + 1], encHx[SEEN_ICAO_MAX * 3 + 1];
    WebUtil_UrlEncode(encCs, sizeof(encCs), r->callsign);
    WebUtil_UrlEncode(encHx, sizeof(encHx), r->icao24);

    char first[64], last[64];
    TimeUtil_FormatLocal((int64_t)r->firstSeen, first, sizeof(first));
    TimeUtil_FormatLocal((int64_t)r->lastSeen, last, sizeof(last));

    char opCell[320];
    if (r->operatorName[0])
        snprintf(opCell, sizeof(opCell), "%s<small>%s</small>", eOp, SeenOperatorSource_Name(r->operatorSource));
    else
        snprintf(opCell, sizeof(opCell), "&mdash;");

    return SendFormat(req,
        "<tr><td data-s='%s'>%s%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td>"
        "<td title='%s'>%s</td><td>%lu</td><td>%s</td><td>%s</td>"
        "<td><a href='/registered?edit=%s&amp;hex=%s'>%s</a></td></tr>",
        AircraftType_CsvName(r->aircraftType), icon, AircraftType_Name(r->aircraftType),
        eIcao, r->callsign[0] ? eCs : "&mdash;", CraftType_Name(r->craftType), opCell,
        eTitle, eCfg, (unsigned long)r->seenCount, first, last,
        encCs, encHx, info.configured ? "Edit" : "Add");
}

static void AppendOptions(char *buf, size_t cap, const char *const *tokens, const char *const *labels,
                          size_t count, size_t selected)
{
    for (size_t i = 0; i < count; i++) {
        char option[160]; /* tokens/labels are short literals from the tables above */
        int n = snprintf(option, sizeof(option), "<option value='%s'%s>%s</option>",
                         tokens[i], i == selected ? " selected" : "", labels[i]);
        size_t used = strlen(buf);
        if (n <= 0 || (size_t)n >= sizeof(option) || used + (size_t)n + 1 > cap)
            return;
        memcpy(buf + used, option, (size_t)n + 1);
    }
}

static esp_err_t SeenPage(httpd_req_t *req)
{
    PageQuery pq;
    ParseQuery(req, &pq);

    SeenQuery query = {
        .search = pq.search,
        .searchField = (uint8_t)pq.searchField,
        .searchMode = (uint8_t)(pq.patternMode ? PATTERN_MODE_PATTERN : PATTERN_MODE_NORMAL),
        .filter = filterChoices[pq.filterIndex].filter,
        .sort = sortChoices[pq.sortIndex].key,
        .descending = pq.descending,
    };

    // CPU-only copy of one page of records (SeenAircraft_Query memcpy, then
    // formatted per row): PSRAM keeps ~4.2 KB out of internal DMA-capable RAM.
    SeenRecord *rows = heap_caps_malloc(SEEN_PAGE_SIZE * sizeof(SeenRecord), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rows)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");

    size_t total = 0;
    size_t pageCount = 1;
    size_t n = SeenAircraft_Query(&query, pq.page * SEEN_PAGE_SIZE, SEEN_PAGE_SIZE, rows, &total);
    if (total > 0)
        pageCount = (total + SEEN_PAGE_SIZE - 1) / SEEN_PAGE_SIZE;
    if (n == 0 && total > 0 && pq.page >= pageCount) { /* stale page number: show the last page */
        pq.page = pageCount - 1;
        n = SeenAircraft_Query(&query, pq.page * SEEN_PAGE_SIZE, SEEN_PAGE_SIZE, rows, &total);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = ESP_OK;

    char iconDefs[900];
    if (AircraftType_IconDefs(iconDefs, sizeof(iconDefs)) == 0)
        iconDefs[0] = '\0';

    /* Clock/time-zone status: makes "not synchronized" visible instead of
     * showing dashes with no explanation. */
    time_t nowUtc = time(NULL);
    TimeZoneConfig tz;
    TimeUtil_GetConfig(&tz);
    char nowText[64];
    TimeUtil_FormatLocal((int64_t)nowUtc, nowText, sizeof(nowText));
    char clockLine[400];
    if (TimeUtil_IsSynced((int64_t)nowUtc))
        snprintf(clockLine, sizeof(clockLine),
                 "Clock synchronized. Times are shown in %s%s (now %s).",
                 strcmp(tz.zoneId, TIMEUTIL_ZONE_CUSTOM) == 0 ? "a custom UTC offset" : tz.zoneId,
                 tz.autoDst ? "" : ", daylight saving off", nowText);
    else
        snprintf(clockLine, sizeof(clockLine),
                 "<b>Clock not synchronized.</b> New sightings are counted but carry no time until the device gets network time.");

    char eSearch[SEARCH_MAX * 6 + 1];
    WebUtil_EscapeHtml(eSearch, sizeof(eSearch), pq.search);

    char filterOpts[512] = "", sortOpts[512] = "";
    {
        const char *ft[FILTER_CHOICES], *fl[FILTER_CHOICES];
        for (size_t i = 0; i < FILTER_CHOICES; i++) { ft[i] = filterChoices[i].token; fl[i] = filterChoices[i].label; }
        AppendOptions(filterOpts, sizeof(filterOpts), ft, fl, FILTER_CHOICES, pq.filterIndex);
        const char *st[SORT_CHOICES], *sl[SORT_CHOICES];
        for (size_t i = 0; i < SORT_CHOICES; i++) { st[i] = sortChoices[i].token; sl[i] = sortChoices[i].label; }
        AppendOptions(sortOpts, sizeof(sortOpts), st, sl, SORT_CHOICES, pq.sortIndex);
    }

    if (WebStyle_SendHead(req, "Seen Aircraft", WEBPAGE_SEEN,
            "body{max-width:1100px}table{font-size:.9em}td,th{padding:.35em;white-space:nowrap}"
            "small{display:block}form.f label{margin-right:.8em;display:inline-block}"
            ".meta{color:var(--mut)}") != ESP_OK ||
        SendChunk(req, "<h1>Seen Aircraft</h1>") != ESP_OK ||
        (!Features_SeenEnabled() &&
         SendChunk(req, "<p class='wn'>Seen logging is <b>OFF</b> (Setup &rarr; Features). "
                        "No new aircraft are being recorded; existing history is kept and shown below.</p>") != ESP_OK) ||
        SendChunk(req, iconDefs) != ESP_OK ||
        SendFormat(req, "<p class='meta'>%s</p>", clockLine) != ESP_OK ||
        SendFormat(req,
            "<form class='f' method='get' action='/seen'>"
            "<label>Search <input name='q' value='%s' maxlength='%d' placeholder='ICAO24, call sign, operator, registry, note'></label>"
            "<label>In <select name='sf'><option value='e'%s>Either</option><option value='c'%s>Call sign</option>"
            "<option value='r'%s>Registration</option></select></label>"
            "<label><input type='checkbox' name='pm' value='1'%s> Pattern mode</label>"
            "<label>Show <select name='f'>%s</select></label>"
            "<label>Sort by <select name='s'>%s</select></label>"
            "<label>Order <select name='d'><option value='d'%s>Descending</option><option value='a'%s>Ascending</option></select></label>"
            "<button>Apply</button> <a href='/seen'>Reset</a></form>",
            eSearch, SEARCH_MAX,
            pq.searchField == SEARCH_FIELD_EITHER ? " selected" : "", pq.searchField == SEARCH_FIELD_CALLSIGN ? " selected" : "",
            pq.searchField == SEARCH_FIELD_REGISTRATION ? " selected" : "", pq.patternMode ? " checked" : "",
            filterOpts, sortOpts,
            pq.descending ? " selected" : "", pq.descending ? "" : " selected") != ESP_OK ||
        SendChunk(req, "<p class='meta'><small>? = one character, * = any number of characters (whole value); text "
                       "without them matches anywhere. Pattern mode: L = letter, N = number (e.g. LLLNNNN, N*). Either "
                       "also searches ICAO24, operator, registry and note for plain text. Seen does not store "
                       "registrations: Registration searches the call sign and your registry rule. ") != ESP_OK ||
        WebStyle_SendHelpLink(req, "searchHelp", "Search Help") != ESP_OK ||
        SendChunk(req, "</small></p>") != ESP_OK ||
        WebStyle_SendHelpDialog(req, "searchHelp", "Seen search help", kSeenSearchHelp) != ESP_OK) {
        err = ESP_FAIL;
        goto done;
    }

    {
        size_t stored = SeenAircraft_Count();
        size_t from = total ? pq.page * SEEN_PAGE_SIZE + 1 : 0;
        size_t to = total ? from + n - 1 : 0;
        if (SendFormat(req,
                "<p class='meta'>Showing %u&ndash;%u of %u matching &middot; %u stored (limit %u) &middot; "
                "<a href='/seen/export'>Download CSV</a></p><div class='w'><table>"
                "<tr><th>Aircraft Type</th><th>ICAO24</th><th>Call Sign</th><th>Craft Type</th><th>Operator</th>"
                "<th>Configured</th><th>Seen</th><th>First seen</th><th>Last seen</th><th>Rule</th></tr>",
                (unsigned)from, (unsigned)to, (unsigned)total, (unsigned)stored, (unsigned)SEEN_MAX_RECORDS) != ESP_OK) {
            err = ESP_FAIL;
            goto done;
        }
    }

    if (n == 0 && SendChunk(req, "<tr><td colspan='10'>No aircraft match.</td></tr>") != ESP_OK) {
        err = ESP_FAIL;
        goto done;
    }
    for (size_t i = 0; i < n; i++) {
        if (SendRecordRow(req, &rows[i]) != ESP_OK) {
            err = ESP_FAIL;
            goto done;
        }
    }

    {
        char base[256], enc[SEARCH_MAX * 3 + 1];
        WebUtil_UrlEncode(enc, sizeof(enc), pq.search);
        snprintf(base, sizeof(base), "q=%s&amp;sf=%s%s&amp;f=%s&amp;s=%s&amp;d=%s", enc, SearchField_Token(pq.searchField),
                 pq.patternMode ? "&amp;pm=1" : "", filterChoices[pq.filterIndex].token,
                 sortChoices[pq.sortIndex].token, pq.descending ? "d" : "a");

        if (SendChunk(req, "</table></div><p>") != ESP_OK ||
            (pq.page > 0 && SendFormat(req, "<a href='/seen?%s&amp;p=%u'>&laquo; Previous</a> &nbsp; ", base, (unsigned)pq.page) != ESP_OK) ||
            SendFormat(req, "Page %u of %u", (unsigned)pq.page + 1, (unsigned)pageCount) != ESP_OK ||
            (pq.page + 1 < pageCount && SendFormat(req, " &nbsp; <a href='/seen?%s&amp;p=%u'>Next &raquo;</a>", base, (unsigned)pq.page + 2) != ESP_OK)) {
            err = ESP_FAIL;
            goto done;
        }
    }

    if (SendFormat(req,
            "</p><p><small>Seen counts visits: an aircraft that returns after %d minutes or more counts again. "
            "History holds up to %d aircraft; when it is full, the setup page's policy decides which one is dropped (now: %s). Times are stored in UTC and "
            "shown in the time zone set on the setup page. Changes are saved to flash about every %d minutes. "
            "Add / Edit opens the Registry / Operator dialog. The persistent record kept on the TF card is on the <a href='/history'>History</a> page.</small></p>"
            "<form method='post' action='/seen/clear' onsubmit=\"return confirm('Delete the entire seen-aircraft history? This cannot be undone.')\">"
            "<button>Clear history</button></form></body></html>",
            SEEN_VISIT_GAP_SEC / 60, SEEN_MAX_RECORDS,
            SeenEvictionPolicy_Name(SeenAircraft_GetEvictionPolicy()), SEEN_FLUSH_INTERVAL_SEC / 60) != ESP_OK) {
        err = ESP_FAIL;
    }

done:
    free(rows);
    if (err != ESP_OK)
        return err;
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---- export ---- */

static const char *ConfiguredCsv(const SeenConfigInfo *info)
{
    if (info->source == CRAFT_SRC_REGISTRY)
        return "REGISTRY";
    if (info->operatorCode[0])
        return "OPERATOR";
    return "NO";
}

/* Streams the whole history as a CSV suitable for spreadsheets. Times are
 * ISO-8601 UTC; Registry note and Configured are derived from the live rule
 * lists at export time. Records are fetched one at a time so the history lock
 * is never held while writing to the network. */
static esp_err_t SeenExport(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"seen_aircraft.csv\"");

    if (SendChunk(req, "ICAO24,CALLSIGN,CRAFT_TYPE,AIRCRAFT_TYPE,OPERATOR,OPERATOR_SOURCE,REGISTRY_NOTE,"
                       "FIRST_SEEN_UTC,LAST_SEEN_UTC,SEEN_COUNT,CONFIGURED\n") != ESP_OK)
        return ESP_FAIL;

    char batch[1024];
    size_t used = 0;
    for (size_t i = 0;; i++) {
        SeenRecord r;
        if (!SeenAircraft_Get(i, &r))
            break;
        SeenConfigInfo info;
        SeenAircraft_Describe(&r, &info);

        char note[MAX_RULE_NOTES + 1];
        AircraftText_Sanitize(note, sizeof(note), info.registryNote); /* no commas/quotes in a CSV cell */
        char first[32], last[32];
        TimeUtil_FormatIsoUtc((int64_t)r.firstSeen, first, sizeof(first));
        TimeUtil_FormatIsoUtc((int64_t)r.lastSeen, last, sizeof(last));

        char line[400];
        int len = snprintf(line, sizeof(line), "%s,%s,%s,%s,%s,%s,%s,%s,%s,%lu,%s\n",
                           r.icao24, r.callsign, CraftType_CsvName(r.craftType), AircraftType_CsvName(r.aircraftType),
                           r.operatorName, SeenOperatorSource_CsvName(r.operatorSource), note, first, last,
                           (unsigned long)r.seenCount, ConfiguredCsv(&info));
        if (len < 0 || len >= (int)sizeof(line))
            continue;
        if (used + (size_t)len > sizeof(batch)) {
            if (httpd_resp_send_chunk(req, batch, (ssize_t)used) != ESP_OK)
                return ESP_FAIL;
            used = 0;
        }
        memcpy(batch + used, line, (size_t)len);
        used += (size_t)len;
    }
    if (used && httpd_resp_send_chunk(req, batch, (ssize_t)used) != ESP_OK)
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t SeenClear(httpd_req_t *req)
{
    /* Drain any body so the connection stays clean; the form has no fields. */
    char scratch[64];
    int remaining = (int)req->content_len;
    while (remaining > 0) {
        int got = httpd_req_recv(req, scratch, remaining < (int)sizeof(scratch) ? remaining : (int)sizeof(scratch));
        if (got <= 0)
            break;
        remaining -= got;
    }
    SeenAircraft_Clear();
    ESP_LOGI(TAG, "Seen-aircraft history cleared from the web UI");
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/seen");
    return httpd_resp_sendstr(req, "Cleared. Return to /seen.");
}

esp_err_t WebSeen_Register(httpd_handle_t server)
{
    const httpd_uri_t page = {.uri = "/seen", .method = HTTP_GET, .handler = SeenPage};
    const httpd_uri_t exportCsv = {.uri = "/seen/export", .method = HTTP_GET, .handler = SeenExport};
    const httpd_uri_t clear = {.uri = "/seen/clear", .method = HTTP_POST, .handler = SeenClear};
    esp_err_t err = httpd_register_uri_handler(server, &page);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &exportCsv);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &clear);
    return err;
}
