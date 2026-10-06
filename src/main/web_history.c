#include "web_history.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "craft_types.h"
#include "custom_rules.h"
#include "pattern_match.h"
#include "tf_history.h"
#include "time_util.h"
#include "universal_value.h"
#include "web_style.h"
#include "web_util.h"

#define HISTORY_PAGE_ROWS 50u

static esp_err_t SendChunk(httpd_req_t *req, const char *text)
{
    return httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
}

/* One bounded stack buffer; only used with literal formats. Every dynamic
 * string argument is HTML-escaped (or a fixed literal) before it gets here. */
static esp_err_t SendFormat(httpd_req_t *req, const char *format, ...) __attribute__((format(printf, 2, 3)));
static esp_err_t SendFormat(httpd_req_t *req, const char *format, ...)
{
    char buf[1536]; /* widest row (worst-case escaping) is about 1.2 KB; a too-small buffer would abort the page mid-stream */
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n < 0 || n >= (int)sizeof(buf))
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, buf, (ssize_t)n);
}

/* ---- query ----
 *
 * /history                     collapsed fingerprint list (first 50 groups)
 * /history?a=FP                group list continuing after fingerprint FP
 * /history?g=FP[&o=N][&a=..]   same list with group FP expanded, records N..N+49
 * /history?q=TEXT              search: exact ICAO24 through the index when TEXT
 *                              is hex; call sign / registration scan otherwise
 * /history?q=TEXT&m=t[&sb=FP&si=N]  call sign / registration scan (resumable)
 * Old links ?b=FP[&o=N][&only=1] open group FP expanded at N. */

typedef struct {
    bool haveAfter;
    uint32_t after;          /* group list starts after this fingerprint */
    bool haveGroup;
    uint32_t group;          /* expanded group */
    uint32_t offset;         /* first record shown in the expanded group */
    char text[TF_REGISTRY_MAX + 4]; /* search text ("" = browse) */
    bool textMode;           /* call sign / registration scan */
    SearchField field;       /* "sf": e / c / r (0.0.27) */
    bool patternMode;        /* "pm=1": L = letter, N = number (0.0.27) */
    bool haveScanCursor;
    uint32_t scanFp;
    uint32_t scanIdx;
} HistQuery;
static bool ParseHex8(const char *s, uint32_t *out)
{
    if (!s || strlen(s) != 8)
        return false;
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isxdigit(c))
            return false;
        v = (v << 4) | (uint32_t)(c <= '9' ? c - '0' : (toupper(c) - 'A') + 10);
    }
    *out = v;
    return true;
}

static bool ParseU32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!end || end == s || *end != '\0' || v >= 0x7FFFFFFFul)
        return false;
    *out = (uint32_t)v;
    return true;
}

/* Search Help popup (0.0.28): what SearchMode / TextScan below actually do.
 * Example links use the existing query parameters (q, sf, pm). */
static const char kHistorySearchHelp[] =
    "<h4>ICAO24 (fast)</h4>"
    "<p>A hex address of up to 6 characters (e.g. <code>a1b2c3</code>, or <code>~</code> plus hex for a non-ICAO "
    "address) with <b>In: Either</b> and Pattern mode off is looked up directly in the history index, so it answers at "
    "once. The result also offers a call sign / registration search for the same text, because hex-looking text can "
    "also be a call sign.</p>"
    "<h4>Call sign and registration (scan)</h4>"
    "<p>Everything else reads the stored history group by group: up to 20 matches or 1,500 records per page, then "
    "<i>Continue search</i> picks up where it stopped. Slower than an ICAO24 lookup. Plain text needs at least 2 "
    "characters and matches anywhere, not case-sensitive: <a href='/history?q=UAL1&amp;sf=c'>UAL1</a>.</p>"
    "<p><b>In</b>: <b>Either</b> = call sign or registration; <b>Call sign</b> only; <b>Registration</b> only. "
    "Registration is the tail number the adsb.lol feed reports (kept since 0.0.27); records without one are only "
    "found by call sign.</p>"
    "<h4>Wildcards</h4><ul>"
    "<li><code>?</code> = exactly one character, <code>*</code> = zero or more; the whole value must match.</li>"
    "<li><a href='/history?q=UAL*&amp;sf=c'>UAL*</a> = call signs starting UAL; "
    "<a href='/history?q=N*&amp;sf=r'>N*</a> = registrations starting N; "
    "<a href='/history?q=ABC%3F&amp;sf=c'>ABC?</a> = four characters starting ABC; "
    "<a href='/history?q=*ABC*'>*ABC*</a> = contains ABC.</li></ul>"
    "<h4>Pattern mode</h4>"
    "<p>Only when <b>Pattern mode</b> is ticked: <code>L</code> = one letter, <code>N</code> = one digit, plus "
    "<code>?</code>/<code>*</code>; the whole value must match. "
    "<a href='/history?q=LLLNNNNL&amp;sf=c&amp;pm=1'>LLLNNNNL</a> = three letters, four digits, one letter; "
    "<a href='/history?q=LNNNNN&amp;sf=r&amp;pm=1'>LNNNNN</a> = registrations like N12345. Without Pattern mode "
    "<a href='/history?q=LLL123'>LLL123</a> means the text LLL123.</p>"
    "<p><small>Registered Aircraft rules on /registered use their own syntax, not this one.</small></p>";

static void ParseQuery(httpd_req_t *req, HistQuery *q)
{
    memset(q, 0, sizeof(*q));
    char query[160];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return;
    char value[32];
    if (httpd_query_key_value(query, "q", value, sizeof(value)) == ESP_OK) {
        WebUtil_UrlDecodeInPlace(value);
        size_t used = 0;
        for (const char *p = value; *p && used < sizeof(q->text) - 1; p++)
            if (isalnum((unsigned char)*p) || *p == '-' || *p == '~' || *p == '?' || *p == '*')
                q->text[used++] = *p;
        q->text[used] = '\0';
        if (q->text[0]) {
            q->textMode = httpd_query_key_value(query, "m", value, sizeof(value)) == ESP_OK && !strcmp(value, "t");
            if (httpd_query_key_value(query, "sf", value, sizeof(value)) == ESP_OK)
                q->field = SearchField_FromToken(value);
            q->patternMode = httpd_query_key_value(query, "pm", value, sizeof(value)) == ESP_OK && !strcmp(value, "1");
            if (httpd_query_key_value(query, "sb", value, sizeof(value)) == ESP_OK && ParseHex8(value, &q->scanFp)) {
                char idx[16];
                q->haveScanCursor = httpd_query_key_value(query, "si", idx, sizeof(idx)) == ESP_OK &&
                                    ParseU32(idx, &q->scanIdx);
            }
            return; /* search mode ignores the browse cursor */
        }
    }
    if (httpd_query_key_value(query, "a", value, sizeof(value)) == ESP_OK && ParseHex8(value, &q->after))
        q->haveAfter = true;
    if ((httpd_query_key_value(query, "g", value, sizeof(value)) == ESP_OK ||
         httpd_query_key_value(query, "b", value, sizeof(value)) == ESP_OK) && ParseHex8(value, &q->group))
        q->haveGroup = true;
    if (q->haveGroup && httpd_query_key_value(query, "o", value, sizeof(value)) == ESP_OK)
        (void)ParseU32(value, &q->offset);
}

/* ---- operator label (derived from the bucket fingerprint, never stored) ---- */

typedef struct {
    uint32_t fp;
    bool valid;
    char html[300]; /* already escaped */
} BucketLabel;

static void BuildBucketLabel(BucketLabel *b, uint32_t fp)
{
    b->fp = fp;
    b->valid = true;
    if (fp == TF_BUCKET_UNASSIGNED) {
        snprintf(b->html, sizeof(b->html), "&mdash;");
    } else if (fp == TF_BUCKET_REGISTRY_DEFINED) {
        snprintf(b->html, sizeof(b->html), "Registry rule");
    } else {
        UniversalValue uv;
        if (UniversalValue_FindByFingerprint(fp, &uv)) {
            /* No lock around the table (same as the other read-only pages):
             * force-terminate in case a periodic sync was mid-update. */
            uv.displayName[sizeof(uv.displayName) - 1] = '\0';
            uv.operatorCode[sizeof(uv.operatorCode) - 1] = '\0';
            char name[sizeof(uv.displayName) * 6 + 1];
            char code[sizeof(uv.operatorCode) * 6 + 1];
            WebUtil_EscapeHtml(name, sizeof(name), uv.displayName[0] ? uv.displayName : uv.operatorCode);
            WebUtil_EscapeHtml(code, sizeof(code), uv.operatorCode);
            snprintf(b->html, sizeof(b->html), "%s%s%s%s", name, code[0] ? " (" : "", code[0] ? code : "",
                     code[0] ? ")" : "");
            if (!uv.active) {
                size_t len = strlen(b->html);
                snprintf(b->html + len, sizeof(b->html) - len, " <small>(operator removed)</small>");
            }
        } else {
            snprintf(b->html, sizeof(b->html),
                     "<small title='This bucket belongs to an operator that is not in the current operator list'>"
                     "unknown operator</small>");
        }
    }
}

static const char *BucketTitle(uint32_t fp)
{
    if (fp == TF_BUCKET_UNASSIGNED)
        return "Unassigned (no operator)";
    if (fp == TF_BUCKET_REGISTRY_DEFINED)
        return "Registry-defined";
    return "Operator group";
}

/* ---- rows ---- */

static const char *StateBadge(uint8_t state)
{
    switch (state) {
    case TF_REC_SUPERSEDED:
        return "<span class='bd' title='A newer record for this aircraft exists (it changed group). "
               "This older record is kept as history and is not used for lookups.'>Superseded</span>";
    case TF_REC_NOT_INDEXED:
        return "<span class='bd' title='The index has no entry for this aircraft (for example the index was rebuilt "
               "without this group).'>Not indexed</span>";
    case TF_REC_INDEX_UNKNOWN:
        return "<span class='bd' title='index.dat could not be read, so it is unknown whether this record is current.'>"
               "Unverified</span>";
    default:
        return "";
    }
}

static esp_err_t SendRow(httpd_req_t *req, const TfBrowseRecord *r, uint32_t fp, uint32_t idx, const BucketLabel *bl)
{
    if (r->state == TF_REC_CORRUPT)
        return SendFormat(req,
            "<tr class='sup'><td colspan='7'>Record #%" PRIu32 " in group %08" PRIX32
            " is unreadable (checksum mismatch, empty or truncated slot).</td></tr>", idx, fp);

    char eIcao[TF_ICAO_MAX * 6 + 1], eCs[TF_CALLSIGN_MAX * 6 + 1], eReg[TF_REGISTRY_MAX * 6 + 1];
    WebUtil_EscapeHtml(eIcao, sizeof(eIcao), r->rec.icao24);
    WebUtil_EscapeHtml(eCs, sizeof(eCs), r->rec.callsign);
    WebUtil_EscapeHtml(eReg, sizeof(eReg), r->rec.registry);

    const char *ac = AircraftType_IsValid(r->rec.aircraftType) ? AircraftType_Name((AircraftType)r->rec.aircraftType) : "Unknown";
    const char *ct = CraftType_IsValid(r->rec.craftType) ? CraftType_Name((CraftType)r->rec.craftType) : "Unknown";

    char first[64], last[64];
    TimeUtil_FormatLocal((int64_t)r->rec.firstSeen, first, sizeof(first));
    TimeUtil_FormatLocal((int64_t)r->rec.lastSeen, last, sizeof(last));

    return SendFormat(req,
        "<tr%s><td>%s%s<small><a href='/history?g=%08" PRIX32 "&amp;o=%" PRIu32 "#g%08" PRIX32 "'>group %08" PRIX32 "</a> #%" PRIu32 "</small></td>"
        "<td>%s%s%s%s</td><td title='Classified by: %s'>%s / %s</td><td>%s</td><td>%s</td><td>%s</td><td>%" PRIu32 "</td></tr>",
        r->state == TF_REC_SUPERSEDED ? " class='sup'" : "", eIcao, StateBadge(r->state), fp,
        idx - idx % HISTORY_PAGE_ROWS, fp, fp, idx,
        r->rec.callsign[0] ? eCs : "&mdash;",
        r->rec.registry[0] ? "<br><small>reg " : "", r->rec.registry[0] ? eReg : "", r->rec.registry[0] ? "</small>" : "",
        CraftSource_Name((CraftSource)r->rec.classificationSource), ac, ct,
        bl->html, first, last, r->rec.seenCount);
}

/* One fingerprint group row: collapsed (link expands it) or expanded (link
 * collapses it). Built from the bucket header only - no record is read. */
static esp_err_t SendGroupRow(httpd_req_t *req, const HistQuery *q, uint32_t fp, bool countKnown, uint32_t count,
                              bool expanded, const BucketLabel *bl)
{
    const bool plain = (fp == TF_BUCKET_UNASSIGNED || fp == TF_BUCKET_REGISTRY_DEFINED);
    char cursor[24] = "";
    if (q->haveAfter)
        snprintf(cursor, sizeof(cursor), "a=%08" PRIX32 "&amp;", q->after);
    char collapse[16] = "";
    if (q->haveAfter)
        snprintf(collapse, sizeof(collapse), "a=%08" PRIX32, q->after);
    char countText[48];
    if (countKnown)
        snprintf(countText, sizeof(countText), "%" PRIu32 " record%s", count, count == 1 ? "" : "s");
    else
        snprintf(countText, sizeof(countText), "header unreadable");
    if (expanded)
        return SendFormat(req,
            "<tr class='bk' id='g%08" PRIX32 "'><td colspan='7'><a href='/history?%s#g%08" PRIX32 "'>&#9660; %08" PRIX32 "</a>"
            " &middot; %s%s%s &middot; %s</td></tr>",
            fp, collapse, fp, fp, BucketTitle(fp), plain ? "" : ": ", plain ? "" : bl->html, countText);
    return SendFormat(req,
        "<tr class='bk' id='g%08" PRIX32 "'><td colspan='7'><a href='/history?%sg=%08" PRIX32 "#g%08" PRIX32 "'>&#9654; %08" PRIX32 "</a>"
        " &middot; %s%s%s &middot; %s</td></tr>",
        fp, cursor, fp, fp, fp, BucketTitle(fp), plain ? "" : ": ", plain ? "" : bl->html, countText);
}

static esp_err_t SendNotice(httpd_req_t *req, const char *cls, const char *text)
{
    return SendFormat(req, "<p class='%s'>%s</p>", cls, text);
}

static const char *kHeadCss =
    "body{max-width:1100px}table{font-size:.9em}td,th{white-space:nowrap}"
    /* 800x480 panel: the classification and operator columns may wrap so the 7-column table fits without side-scrolling */
    "td:nth-child(3),td:nth-child(4){white-space:normal;min-width:6em}"
    "small{display:block}.meta{color:var(--mut)}"
    "tr.sup td{opacity:.6}tr.bk td{font-weight:bold;border-top:2px solid var(--bd)}"
    ".bd{display:inline-block;border:1px solid var(--bd);border-radius:4px;padding:0 .4em;font-size:.8em;margin-left:.4em}";

static esp_err_t SendStatusNotice(httpd_req_t *req, TfBrowseStatus st)
{
    switch (st) {
    case TF_BR_UNAVAILABLE:
        return SendNotice(req, "wn",
            "TF history is not available right now (card not mounted, or being reinitialized). "
            "See <a href='/diag#tf'>Diagnostics</a>.");
    case TF_BR_BUSY:
        return SendNotice(req, "wn", "The card is busy with history writes. Reload the page in a moment.");
    default:
        return SendNotice(req, "er", "The history files could not be read. See <a href='/diag#tf'>Diagnostics</a>.");
    }
}

/* ---- search ---- */

#define HISTORY_SEARCH_MAX_MATCHES 20u
#define HISTORY_SEARCH_RECORD_BUDGET 1500u /* records read per request before offering "continue" */

static const char *kTableHead =
    "<div class='w'><table><tr><th>ICAO24</th><th>Call Sign</th><th>Aircraft / Class</th>"
    "<th>Operator</th><th>First seen</th><th>Last seen</th><th>Seen</th></tr>";

static bool LooksLikeIcao(const char *t)
{
    size_t n = strlen(t), i = (t[0] == '~') ? 1 : 0;
    if (n <= i || n - i > 6)
        return false;
    for (; i < n; i++)
        if (!isxdigit((unsigned char)t[i]))
            return false;
    return true;
}

/* Exact ICAO24 through the existing index (unchanged fast path). */
static esp_err_t IcaoLookup(httpd_req_t *req, const HistQuery *q, bool *found)
{
    /* Stored ICAO24 case follows the provider; try as typed, lower, upper. */
    char tries[3][TF_ICAO_MAX];
    snprintf(tries[0], TF_ICAO_MAX, "%.*s", (int)(TF_ICAO_MAX - 1), q->text); /* LooksLikeIcao: <= 7 chars */
    for (size_t i = 0; i < TF_ICAO_MAX; i++) {
        tries[1][i] = (char)tolower((unsigned char)tries[0][i]);
        tries[2][i] = (char)toupper((unsigned char)tries[0][i]);
    }
    TfBrowseRecord rec;
    uint32_t fp = 0, idx = 0;
    TfBrowseStatus st = TF_BR_END;
    for (int t = 0; t < 3 && st == TF_BR_END; t++) {
        bool dup = false;
        for (int u = 0; u < t; u++)
            if (strcmp(tries[t], tries[u]) == 0)
                dup = true;
        if (!dup)
            st = TfHistory_FindForBrowse(tries[t], &rec, &fp, &idx);
    }
    *found = false;
    if (st == TF_BR_END)
        return ESP_OK;
    if (st != TF_BR_OK)
        return SendStatusNotice(req, st);
    *found = true;
    BucketLabel bl;
    BuildBucketLabel(&bl, fp);
    if (SendChunk(req, "<h2>ICAO24 match</h2>") != ESP_OK || SendChunk(req, kTableHead) != ESP_OK ||
        SendRow(req, &rec, fp, idx, &bl) != ESP_OK || SendChunk(req, "</table></div>") != ESP_OK)
        return ESP_FAIL;
    return ESP_OK;
}

/* Call sign / registration: a bounded, resumable scan of the bucket files in
 * fingerprint order, TF_BROWSE_CHUNK_MAX records per TF call (lock held only
 * inside each call, never while sending), no index and nothing cached. Stops
 * after HISTORY_SEARCH_MAX_MATCHES matches or HISTORY_SEARCH_RECORD_BUDGET
 * records and offers a "continue" link with the exact cursor. */
static esp_err_t TextScan(httpd_req_t *req, const HistQuery *q)
{
    char eText[sizeof(q->text) * 6 + 1];
    WebUtil_EscapeHtml(eText, sizeof(eText), q->text);
    char eUrl[sizeof(q->text) * 3 + 1]; /* '?' and '*' must be URL-encoded in links */
    WebUtil_UrlEncode(eUrl, sizeof(eUrl), q->text);
    const PatternMode mode = q->patternMode ? PATTERN_MODE_PATTERN : PATTERN_MODE_NORMAL;
    if (mode == PATTERN_MODE_NORMAL && !Pattern_HasWildcards(q->text) && strlen(q->text) < 2)
        return SendNotice(req, "meta", "Enter at least 2 characters to search call signs and registrations.");
    if (SendFormat(req, "<h2>Call sign / registration matches for <b>%s</b></h2>", eText) != ESP_OK)
        return ESP_FAIL;

    uint32_t fps[TF_LIST_MAX];
    size_t nFps = 0;
    size_t fpPos = 0;
    bool moreGroups = false;
    uint32_t fp = q->haveScanCursor ? q->scanFp : 0;
    uint32_t idx = q->haveScanCursor ? q->scanIdx : 0;
    /* Groups from the cursor's group on (inclusive). */
    TfBrowseStatus st = TfHistory_ListBuckets(q->haveScanCursor && fp > 0, fp - 1, TF_LIST_MAX, fps, &nFps, NULL, &moreGroups);
    if (st == TF_BR_END)
        return SendNotice(req, "meta", "No history records have been stored on the card yet.");
    if (st != TF_BR_OK)
        return SendStatusNotice(req, st);
    if (!q->haveScanCursor || fps[0] != fp)
        idx = 0; /* cursor group vanished: start at the next one */

    uint32_t scanned = 0, matches = 0, unreadable = 0;
    bool tableOpen = false, finished = false, aborted = false;
    BucketLabel bl = {.valid = false};
    while (matches < HISTORY_SEARCH_MAX_MATCHES && scanned < HISTORY_SEARCH_RECORD_BUDGET) {
        if (fpPos >= nFps) {
            if (!moreGroups) {
                finished = true;
                break;
            }
            const uint32_t last = fps[nFps - 1];
            st = TfHistory_ListBuckets(true, last, TF_LIST_MAX, fps, &nFps, NULL, &moreGroups);
            fpPos = 0;
            if (st == TF_BR_END) {
                finished = true;
                break;
            }
            if (st != TF_BR_OK) {
                aborted = true;
                break;
            }
            idx = 0;
        }
        fp = fps[fpPos];
        TfBrowseRecord recs[TF_BROWSE_CHUNK_MAX];
        size_t got = 0;
        uint32_t count = 0;
        st = TfHistory_ScanChunk(fp, idx, TF_BROWSE_CHUNK_MAX, recs, &got, &count);
        if (st == TF_BR_UNAVAILABLE || st == TF_BR_BUSY || st == TF_BR_IO) {
            aborted = true;
            break;
        }
        if (st == TF_BR_END || got == 0) { /* end of this group (or unreadable header) */
            fpPos++;
            idx = 0;
            continue;
        }
        size_t used = 0; /* records consumed from this chunk (the cursor resumes after them) */
        for (size_t i = 0; i < got && matches < HISTORY_SEARCH_MAX_MATCHES; i++) {
            const TfBrowseRecord *r = &recs[i];
            used = i + 1;
            if (r->state == TF_REC_CORRUPT) {
                unreadable++;
                continue;
            }
            /* One shared matcher (pattern_match.h); registrations are recorded
             * from adsb.lol since 0.0.27, older records have none. */
            const bool csHit = q->field != SEARCH_FIELD_REGISTRATION && Pattern_Match(r->rec.callsign, q->text, mode);
            const bool regHit = !csHit && q->field != SEARCH_FIELD_CALLSIGN && r->rec.registry[0] &&
                                Pattern_Match(r->rec.registry, q->text, mode);
            if (!csHit && !regHit)
                continue;
            TfBrowseRecord shown = *r;
            if (TfHistory_IsLive(r->rec.icao24, fp, idx + (uint32_t)i, &shown.state) != TF_BR_OK)
                shown.state = TF_REC_INDEX_UNKNOWN;
            if (!bl.valid || bl.fp != fp)
                BuildBucketLabel(&bl, fp);
            if (!tableOpen) {
                if (SendChunk(req, kTableHead) != ESP_OK)
                    return ESP_FAIL;
                tableOpen = true;
            }
            if (SendRow(req, &shown, fp, idx + (uint32_t)i, &bl) != ESP_OK)
                return ESP_FAIL;
            matches++;
        }
        scanned += (uint32_t)used;
        idx += (uint32_t)used;
    }
    if (tableOpen && SendChunk(req, "</table></div>") != ESP_OK)
        return ESP_FAIL;
    if (aborted)
        return SendStatusNotice(req, st);
    if (!matches && SendFormat(req, "<p class='meta'>No call sign or registration matching <b>%s</b>%s.</p>",
                               eText, finished ? "" : " in the records searched so far") != ESP_OK)
        return ESP_FAIL;
    if (unreadable && SendFormat(req, "<p class='meta'>%" PRIu32 " unreadable record%s skipped "
                                      "(checksum mismatch, empty or truncated slot).</p>",
                                 unreadable, unreadable == 1 ? " was" : "s were") != ESP_OK)
        return ESP_FAIL;
    if (finished)
        return SendFormat(req, "<p class='meta'>End of search: %" PRIu32 " match%s in this pass. "
                               "<a href='/history'>All groups</a></p>", matches, matches == 1 ? "" : "es");
    return SendFormat(req,
        "<p class='meta'>Searched %" PRIu32 " records. <a href='/history?q=%s&amp;m=t&amp;sf=%s%s&amp;sb=%08" PRIX32 "&amp;si=%" PRIu32 "'>"
        "Continue search &raquo;</a></p>", scanned, eUrl, SearchField_Token(q->field), q->patternMode ? "&amp;pm=1" : "", fp, idx);
}

static esp_err_t SearchMode(httpd_req_t *req, const HistQuery *q)
{
    char eText[sizeof(q->text) * 6 + 1];
    WebUtil_EscapeHtml(eText, sizeof(eText), q->text);
    if (!q->textMode && !q->patternMode && q->field == SEARCH_FIELD_EITHER && LooksLikeIcao(q->text)) {
        bool found = false;
        if (IcaoLookup(req, q, &found) != ESP_OK)
            return ESP_FAIL;
        if (!found && SendFormat(req, "<p class='meta'>No history record for ICAO24 <b>%s</b> in the index.</p>", eText) != ESP_OK)
            return ESP_FAIL;
        /* Hex text can also be a call sign: offer the scan explicitly. */
        return SendFormat(req, "<p><a href='/history?q=%s&amp;m=t&amp;sf=e'>Search call signs and registrations for %s</a> "
                               "<small>(scans the card; slower than the ICAO24 index)</small></p>", eText, eText);
    }
    return TextScan(req, q);
}

/* ---- browse: collapsed fingerprint groups, one expanded on demand ---- */

static esp_err_t SendExpandedGroup(httpd_req_t *req, const HistQuery *q, uint32_t fp, uint32_t count,
                                   const BucketLabel *bl)
{
    uint32_t first = q->offset;
    if (first >= count)
        first = count ? (count - 1) - (count - 1) % HISTORY_PAGE_ROWS : 0;
    uint32_t idx = first, shown = 0;
    while (shown < HISTORY_PAGE_ROWS && idx < count) {
        TfBrowseRecord recs[TF_BROWSE_CHUNK_MAX];
        size_t got = 0;
        size_t want = HISTORY_PAGE_ROWS - shown;
        if (want > TF_BROWSE_CHUNK_MAX)
            want = TF_BROWSE_CHUNK_MAX;
        /* The TF mutex is held only inside this call; the rows are sent after it returns. */
        TfBrowseStatus st = TfHistory_BrowseChunk(fp, idx, want, recs, &got, &count);
        if (st == TF_BR_UNAVAILABLE || st == TF_BR_BUSY || st == TF_BR_IO) {
            char row[400];
            snprintf(row, sizeof(row), "<tr><td colspan='7'>%s</td></tr>",
                     st == TF_BR_BUSY ? "The card is busy with history writes. Reload the page in a moment."
                                      : "The group's records could not be read. See <a href='/diag#tf'>Diagnostics</a>.");
            return SendChunk(req, row);
        }
        if (st == TF_BR_END || got == 0)
            break;
        for (size_t i = 0; i < got; i++)
            if (SendRow(req, &recs[i], fp, idx + (uint32_t)i, bl) != ESP_OK)
                return ESP_FAIL;
        idx += (uint32_t)got;
        shown += (uint32_t)got;
    }
    char cursor[24] = "";
    if (q->haveAfter)
        snprintf(cursor, sizeof(cursor), "a=%08" PRIX32 "&amp;", q->after);
    char prev[160] = "", next[160] = "";
    if (first > 0)
        snprintf(prev, sizeof(prev), "<a href='/history?%sg=%08" PRIX32 "&amp;o=%" PRIu32 "#g%08" PRIX32 "'>&laquo; Previous</a> ",
                 cursor, fp, first >= HISTORY_PAGE_ROWS ? first - HISTORY_PAGE_ROWS : 0, fp);
    if (idx < count)
        snprintf(next, sizeof(next), "<a href='/history?%sg=%08" PRIX32 "&amp;o=%" PRIu32 "#g%08" PRIX32 "'>Next &raquo;</a>",
                 cursor, fp, idx, fp);
    if (shown == 0)
        return SendChunk(req, "<tr><td colspan='7'>No records in this group.</td></tr>");
    return SendFormat(req, "<tr><td colspan='7'>Showing %" PRIu32 "&ndash;%" PRIu32 " of %" PRIu32 " &nbsp; %s%s</td></tr>",
                      first + 1, first + shown, count, prev, next);
}

static esp_err_t BrowseMode(httpd_req_t *req, const HistQuery *q)
{
    HistQuery eq = *q;
    /* A group link without a list cursor (search result, old ?b= link) starts the
     * list at that group so it is visible and expanded. */
    if (eq.haveGroup && !eq.haveAfter && eq.group > 0) {
        eq.haveAfter = true;
        eq.after = eq.group - 1;
    }
    uint32_t fps[TF_LIST_MAX];
    size_t n = 0;
    uint32_t total = 0;
    bool more = false;
    TfBrowseStatus st = TfHistory_ListBuckets(eq.haveAfter, eq.after, TF_LIST_MAX, fps, &n, &total, &more);
    if (st == TF_BR_END && total == 0)
        return SendNotice(req, "meta", "No history records have been stored on the card yet.");
    if (st == TF_BR_END)
        return SendNotice(req, "meta", "No further groups. <a href='/history'>First groups</a>");
    if (st != TF_BR_OK)
        return SendStatusNotice(req, st);
    if (eq.haveGroup) {
        bool listed = false;
        for (size_t i = 0; i < n; i++)
            listed |= fps[i] == eq.group;
        if (!listed && SendNotice(req, "wn", "That group does not exist (or is no longer on the card). "
                                             "<a href='/history'>All groups</a>") != ESP_OK)
            return ESP_FAIL;
    }
    if (SendFormat(req, "<h2>Fingerprints</h2><p class='meta'>%u group%s on the card. Groups are collapsed: "
                        "expanding one reads only that group's records, 50 at a time.</p>",
                   (unsigned)total, total == 1 ? "" : "s") != ESP_OK ||
        SendChunk(req, kTableHead) != ESP_OK)
        return ESP_FAIL;
    BucketLabel bl;
    for (size_t i = 0; i < n; i++) {
        uint32_t count = 0;
        TfBrowseStatus hs = TfHistory_BucketInfo(fps[i], &count); /* one header read */
        if (hs == TF_BR_UNAVAILABLE || hs == TF_BR_BUSY) {
            if (SendChunk(req, "</table></div>") != ESP_OK)
                return ESP_FAIL;
            return SendStatusNotice(req, hs);
        }
        BuildBucketLabel(&bl, fps[i]);
        const bool expanded = eq.haveGroup && eq.group == fps[i];
        if (SendGroupRow(req, &eq, fps[i], hs == TF_BR_OK, count, expanded && hs == TF_BR_OK, &bl) != ESP_OK)
            return ESP_FAIL;
        if (expanded && hs == TF_BR_OK && SendExpandedGroup(req, &eq, fps[i], count, &bl) != ESP_OK)
            return ESP_FAIL;
    }
    if (SendChunk(req, "</table></div>") != ESP_OK)
        return ESP_FAIL;
    if (more)
        return SendFormat(req, "<p><a href='/history'>&laquo; First groups</a> &nbsp; "
                               "<a href='/history?a=%08" PRIX32 "'>Next groups &raquo;</a></p>", fps[n - 1]);
    return SendNotice(req, "meta", eq.haveAfter ? "End of groups. <a href='/history'>First groups</a>" : "End of groups.");
}

/* ---- page ---- */

static esp_err_t HistoryPage(httpd_req_t *req)
{
    HistQuery q;
    ParseQuery(req, &q);

    TfHistoryStats tf;
    TfHistory_GetStats(&tf);

    if (WebStyle_SendHead(req, "History", WEBPAGE_HISTORY, kHeadCss) != ESP_OK ||
        SendChunk(req, "<h1>History</h1><p class='meta'>Persistent aircraft history stored on the TF card. "
                       "<b>Seen</b> is the recent/hot list; this page shows what the card has kept.</p>") != ESP_OK)
        return ESP_FAIL;

    esp_err_t err = ESP_OK;
    if (!tf.historyAvailable) {
        err = SendNotice(req, "wn",
            "TF history is not available (no card, card not writable, or history not initialized). "
            "See <a href='/diag#tf'>Diagnostics &rarr; TF history</a> for the exact stage and error.");
    } else {
        if (tf.indexLoadState == TF_LOAD_CAP &&
            SendNotice(req, "er", "The history index is at its hard cap: new aircraft are not being added to History "
                                  "(they remain in Hot Seen). Existing aircraft keep updating. See "
                                  "<a href='/diag#tf'>Diagnostics</a>.") != ESP_OK)
            return ESP_FAIL;
        if (tf.indexLoadState == TF_LOAD_WARN &&
            SendNotice(req, "wn", "The history index is above 70% full.") != ESP_OK)
            return ESP_FAIL;
        char eq[sizeof(q.text) * 6 + 1];
        WebUtil_EscapeHtml(eq, sizeof(eq), q.text);
        if (SendFormat(req,
                "<form method='get' action='/history'><label>Search <input name='q' value='%s' maxlength='%d' "
                "placeholder='ICAO24, call sign or registration'></label> "
                "<label>In <select name='sf'><option value='e'%s>Either</option><option value='c'%s>Call sign</option>"
                "<option value='r'%s>Registration</option></select></label> "
                "<label><input type='checkbox' name='pm' value='1'%s> Pattern mode</label> <button>Search</button> "
                "<a href='/history'>All groups</a></form>",
                eq, (int)(sizeof(q.text) - 1),
                q.field == SEARCH_FIELD_EITHER ? " selected" : "",
                q.field == SEARCH_FIELD_CALLSIGN ? " selected" : "",
                q.field == SEARCH_FIELD_REGISTRATION ? " selected" : "",
                q.patternMode ? " checked" : "") != ESP_OK ||
            SendChunk(req,
                "<p class='meta'><small>Search: plain text matches anywhere in the value. <b>?</b> = exactly one "
                "character, <b>*</b> = any number of characters (wildcards match the whole value, e.g. "
                "<code>UAL*</code>, <code>N12?</code>). Pattern mode adds <b>L</b> = one letter and <b>N</b> = one "
                "digit (e.g. <code>LLLNNNN</code>). Registration = the registration reported by the feed. ") != ESP_OK ||
            WebStyle_SendHelpLink(req, "searchHelp", "Search Help") != ESP_OK ||
            SendChunk(req, "</small></p>") != ESP_OK ||
            WebStyle_SendHelpDialog(req, "searchHelp", "History search help", kHistorySearchHelp) != ESP_OK ||
            SendFormat(req,
                "<p class='meta'>%u aircraft in the index (limit %u). ICAO24 searches use the index; call sign and "
                "registration searches scan the card. &middot; <a href='/diag#tf'>Storage diagnostics</a></p>",
                (unsigned)tf.indexSlotsUsed, (unsigned)tf.indexSlotsTotal) != ESP_OK)
            return ESP_FAIL;
        err = q.text[0] ? SearchMode(req, &q) : BrowseMode(req, &q);
        if (err == ESP_OK)
            err = SendChunk(req,
                "<p><small>Each row is a record physically stored in a group file. <b>Superseded</b> = the aircraft has a "
                "newer record elsewhere (it changed operator/registry group); the old record is kept and shown, never "
                "deleted. <b>Not indexed</b> = the index has no entry for it. The operator is derived from the group's "
                "fingerprint and the current operator list, so a group whose operator was removed shows as unknown. "
                "Times are shown in the zone set on the setup page. This page is read-only.</small></p>");
    }
    if (err != ESP_OK)
        return err;
    if (SendChunk(req, "</body></html>") != ESP_OK)
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t WebHistory_Register(httpd_handle_t server)
{
    httpd_uri_t uri = {.uri = "/history", .method = HTTP_GET, .handler = HistoryPage, .user_ctx = NULL};
    return httpd_register_uri_handler(server, &uri);
}
