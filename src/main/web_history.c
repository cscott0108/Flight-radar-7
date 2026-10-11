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

#if defined(HIST_TEST_ALLOC) /* host tests only: lets a test make the PSRAM allocations fail */
void *HistTestAlloc(size_t n);
#define HIST_PSRAM_ALLOC(n) HistTestAlloc(n)
#elif defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#define HIST_PSRAM_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define HIST_PSRAM_ALLOC(n) malloc(n)
#endif

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
 * Old links ?b=FP[&o=N][&only=1] open group FP expanded at N.
 * 0.1.4:
 * /history?q=ICAO&sf=i[&sb=FP&si=N]  every record of that exact ICAO24 in every group
 *                              (scan, resumable), with call signs summarized per group
 * /history?q=CS&sf=c&ex=1      exact call sign (whole value, not case-sensitive)
 * /history?s=d|a[&p=N]         groups ordered by record count (d = most first,
 *                              a = fewest first; ties by fingerprint), N = position
 * &x=1 on an expanded group    also check the page's aircraft for other call signs
 *                              in this group and records in other groups (bounded scan) */

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
    bool icaoAll;            /* 0.1.4 "sf=i": every record of this exact ICAO24 */
    bool exact;              /* 0.1.4 "ex=1": exact call sign / registration */
    char sort;               /* 0.1.4 "s": 0 = default order, 'd' / 'a' = record count */
    uint32_t sortPos;        /* 0.1.4 "p": first group shown in a sorted list */
    bool check;              /* 0.1.4 "x=1": cross-check the expanded page's aircraft */
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
        char ex[4];
        /* 0.1.4: an exact search (a clicked stored value) keeps every printable character;
         * typed searches keep the original filter. Always escaped/encoded on output. */
        const bool exactQ = httpd_query_key_value(query, "ex", ex, sizeof(ex)) == ESP_OK && !strcmp(ex, "1");
        size_t used = 0;
        for (const char *p = value; *p && used < sizeof(q->text) - 1; p++)
            if (isalnum((unsigned char)*p) || *p == '-' || *p == '~' || *p == '?' || *p == '*' ||
                (exactQ && (unsigned char)*p > ' ' && (unsigned char)*p < 0x7F))
                q->text[used++] = *p;
        q->text[used] = '\0';
        if (httpd_query_key_value(query, "s", value, sizeof(value)) == ESP_OK && (!strcmp(value, "d") || !strcmp(value, "a")))
            q->sort = value[0]; /* kept so "All groups" links return to the chosen order */
        if (q->text[0]) {
            q->textMode = httpd_query_key_value(query, "m", value, sizeof(value)) == ESP_OK && !strcmp(value, "t");
            if (httpd_query_key_value(query, "sf", value, sizeof(value)) == ESP_OK) {
                q->icaoAll = !strcmp(value, "i");
                q->field = SearchField_FromToken(value);
            }
            q->exact = httpd_query_key_value(query, "ex", value, sizeof(value)) == ESP_OK && !strcmp(value, "1");
            q->patternMode = httpd_query_key_value(query, "pm", value, sizeof(value)) == ESP_OK && !strcmp(value, "1");
            if (httpd_query_key_value(query, "sb", value, sizeof(value)) == ESP_OK && ParseHex8(value, &q->scanFp)) {
                char idx[16];
                q->haveScanCursor = httpd_query_key_value(query, "si", idx, sizeof(idx)) == ESP_OK &&
                                    ParseU32(idx, &q->scanIdx);
            }
            return; /* search mode ignores the browse cursor */
        }
    }
    if (httpd_query_key_value(query, "s", value, sizeof(value)) == ESP_OK && (!strcmp(value, "d") || !strcmp(value, "a"))) {
        q->sort = value[0];
        if (httpd_query_key_value(query, "p", value, sizeof(value)) == ESP_OK)
            (void)ParseU32(value, &q->sortPos);
    }
    q->check = httpd_query_key_value(query, "x", value, sizeof(value)) == ESP_OK && !strcmp(value, "1");
    if (!q->sort && httpd_query_key_value(query, "a", value, sizeof(value)) == ESP_OK && ParseHex8(value, &q->after))
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
        return "<span class='bd' title='A newer record for this aircraft exists (it changed call sign or group). "
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

/* extra: optional HTML (already escaped) added under the call sign (0.1.4 cross-check). */
static esp_err_t SendRow(httpd_req_t *req, const TfBrowseRecord *r, uint32_t fp, uint32_t idx, const BucketLabel *bl,
                         const char *extra)
{
    if (r->state == TF_REC_CORRUPT)
        return SendFormat(req,
            "<tr class='sup'><td colspan='7'>Record #%" PRIu32 " in group %08" PRIX32
            " is unreadable (checksum mismatch, empty or truncated slot).</td></tr>", idx, fp);

    char eIcao[TF_ICAO_MAX * 6 + 1], eCs[TF_CALLSIGN_MAX * 6 + 1], eReg[TF_REGISTRY_MAX * 6 + 1];
    WebUtil_EscapeHtml(eIcao, sizeof(eIcao), r->rec.icao24);
    WebUtil_EscapeHtml(eCs, sizeof(eCs), r->rec.callsign);
    WebUtil_EscapeHtml(eReg, sizeof(eReg), r->rec.registry);
    /* 0.1.4: the ICAO24 opens every record of that aircraft; each call sign runs an exact
     * call-sign search. Values are URL-encoded for the link and HTML-escaped for the text. */
    char uIcao[TF_ICAO_MAX * 3 + 1], uCs[TF_CALLSIGN_MAX * 3 + 1];
    WebUtil_UrlEncode(uIcao, sizeof(uIcao), r->rec.icao24);
    WebUtil_UrlEncode(uCs, sizeof(uCs), r->rec.callsign);
    char icaoCell[sizeof(uIcao) + sizeof(eIcao) + 96], csCell[sizeof(uCs) + sizeof(eCs) + 96];
    if (r->rec.icao24[0])
        snprintf(icaoCell, sizeof(icaoCell), "<a href='/history?q=%s&amp;sf=i' title='Every record of this aircraft'>%s</a>", uIcao, eIcao);
    else
        snprintf(icaoCell, sizeof(icaoCell), "%s", eIcao);
    if (r->rec.callsign[0])
        snprintf(csCell, sizeof(csCell), "<a href='/history?q=%s&amp;sf=c&amp;ex=1' title='Search this call sign'>%s</a>", uCs, eCs);
    else
        snprintf(csCell, sizeof(csCell), "&mdash;");

    const char *ac = AircraftType_IsValid(r->rec.aircraftType) ? AircraftType_Name((AircraftType)r->rec.aircraftType) : "Unknown";
    const char *ct = CraftType_IsValid(r->rec.craftType) ? CraftType_Name((CraftType)r->rec.craftType) : "Unknown";

    char first[64], last[64];
    TimeUtil_FormatLocal((int64_t)r->rec.firstSeen, first, sizeof(first));
    TimeUtil_FormatLocal((int64_t)r->rec.lastSeen, last, sizeof(last));

    /* Sent in two parts with the optional note between them, so neither part can
     * outgrow SendFormat's buffer. */
    if (SendFormat(req,
            "<tr%s><td>%s%s<small><a href='/history?g=%08" PRIX32 "&amp;o=%" PRIu32 "#g%08" PRIX32 "'>group %08" PRIX32 "</a> #%" PRIu32 "</small></td>"
            "<td>%s%s%s%s",
            r->state == TF_REC_SUPERSEDED ? " class='sup'" : "", icaoCell, StateBadge(r->state), fp,
            idx - idx % HISTORY_PAGE_ROWS, fp, fp, idx,
            csCell,
            r->rec.registry[0] ? "<br><small>reg " : "", r->rec.registry[0] ? eReg : "", r->rec.registry[0] ? "</small>" : "") != ESP_OK ||
        (extra && extra[0] && SendChunk(req, extra) != ESP_OK))
        return ESP_FAIL;
    return SendFormat(req,
        "</td><td title='Classified by: %s'>%s / %s</td><td>%s</td><td>%s</td><td>%s</td><td>%" PRIu32 "</td></tr>",
        CraftSource_Name((CraftSource)r->rec.classificationSource), ac, ct,
        bl->html, first, last, r->rec.seenCount);
}

/* Query text that keeps the group list where it is: default order "a=FP"
 * (list cursor) or 0.1.4 sorted order "s=d|a&p=N". withAmp ends in "&amp;"
 * (for links that add more parameters), bare does not; both "" at the start. */
static void ListState(const HistQuery *q, char *withAmp, size_t capA, char *bare, size_t capB)
{
    if (q->sort)
        snprintf(bare, capB, "s=%c&amp;p=%" PRIu32, q->sort, q->sortPos);
    else if (q->haveAfter)
        snprintf(bare, capB, "a=%08" PRIX32, q->after);
    else
        bare[0] = '\0';
    snprintf(withAmp, capA, "%s%s", bare, bare[0] ? "&amp;" : "");
}

/* One fingerprint group row: collapsed (link expands it) or expanded (link
 * collapses it). Built from the bucket header only - no record is read. */
static esp_err_t SendGroupRow(httpd_req_t *req, const HistQuery *q, uint32_t fp, bool countKnown, uint32_t count,
                              bool expanded, const BucketLabel *bl)
{
    const bool plain = (fp == TF_BUCKET_UNASSIGNED || fp == TF_BUCKET_REGISTRY_DEFINED);
    char cursor[40], collapse[36];
    ListState(q, cursor, sizeof(cursor), collapse, sizeof(collapse));
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
        SendRow(req, &rec, fp, idx, &bl, NULL) != ESP_OK || SendChunk(req, "</table></div>") != ESP_OK)
        return ESP_FAIL;
    return ESP_OK;
}

/* 0.1.4: whole-value comparison, not case-sensitive, trailing spaces ignored
 * (call signs can be space-padded) - for "exact" clicks and ICAO24 matches. */
static bool SameValue(const char *value, const char *text)
{
    size_t n = strlen(value), m = strlen(text);
    while (n && value[n - 1] == ' ')
        n--;
    if (n != m || n == 0)
        return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)value[i]) != tolower((unsigned char)text[i]))
            return false;
    return true;
}

/* 0.1.4: call signs actually found in the matched records of an ICAO24 scan,
 * kept per fingerprint (never merged across groups), in the order found. */
typedef struct {
    uint32_t fp;
    char cs[TF_CALLSIGN_MAX];
} CsHit;

static esp_err_t SendCallsignSummary(httpd_req_t *req, const char *eText, const CsHit *hits, size_t n, bool complete)
{
    if (SendFormat(req, "<h3>Call signs recorded for %s, by fingerprint</h3>"
                        "<p class='meta'>Only call signs stored in the records above; each group is listed separately%s.</p><ul>",
                   eText, complete ? "" : " (records found in this pass only - continue the search for more)") != ESP_OK)
        return ESP_FAIL;
    BucketLabel bl = {.valid = false};
    for (size_t i = 0; i < n; i++) {
        bool seenFp = false;
        for (size_t j = 0; j < i; j++)
            seenFp |= hits[j].fp == hits[i].fp;
        if (seenFp)
            continue; /* this group's line was already written */
        BuildBucketLabel(&bl, hits[i].fp);
        const bool plain = (hits[i].fp == TF_BUCKET_UNASSIGNED || hits[i].fp == TF_BUCKET_REGISTRY_DEFINED);
        if (SendFormat(req, "<li><a href='/history?g=%08" PRIX32 "#g%08" PRIX32 "'>%08" PRIX32 "</a> %s%s%s: ",
                       hits[i].fp, hits[i].fp, hits[i].fp, BucketTitle(hits[i].fp), plain ? "" : " &middot; ",
                       plain ? "" : bl.html) != ESP_OK)
            return ESP_FAIL;
        size_t shown = 0;
        for (size_t k = i; k < n; k++) {
            if (hits[k].fp != hits[i].fp || !hits[k].cs[0])
                continue;
            bool dup = false;
            for (size_t j = i; j < k; j++)
                dup |= hits[j].fp == hits[k].fp && !strcmp(hits[j].cs, hits[k].cs);
            if (dup)
                continue;
            char e[TF_CALLSIGN_MAX * 6 + 1], u[TF_CALLSIGN_MAX * 3 + 1];
            WebUtil_EscapeHtml(e, sizeof(e), hits[k].cs);
            WebUtil_UrlEncode(u, sizeof(u), hits[k].cs);
            if (SendFormat(req, "%s<a href='/history?q=%s&amp;sf=c&amp;ex=1'>%s</a>", shown ? " &middot; " : "", u, e) != ESP_OK)
                return ESP_FAIL;
            shown++;
        }
        if (SendChunk(req, shown ? "</li>" : "<i>no call sign recorded</i></li>") != ESP_OK)
            return ESP_FAIL;
    }
    return SendChunk(req, "</ul>");
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
    if (q->icaoAll && !LooksLikeIcao(q->text))
        return SendNotice(req, "meta", "That is not an ICAO24 address (up to 6 hex digits, optionally starting with ~).");
    if (!q->icaoAll && !q->exact && mode == PATTERN_MODE_NORMAL && !Pattern_HasWildcards(q->text) && strlen(q->text) < 2)
        return SendNotice(req, "meta", "Enter at least 2 characters to search call signs and registrations.");
    if (q->icaoAll) {
        if (SendFormat(req, "<h2>Every record of ICAO24 <b>%s</b></h2><p class='meta'>All groups (fingerprints) are "
                            "searched; each record is its own row with its own first seen, last seen and Seen count. "
                            "Superseded rows are older records kept as history.</p>", eText) != ESP_OK)
            return ESP_FAIL;
    } else if (SendFormat(req, "<h2>%s matches for <b>%s</b></h2>",
                          q->exact ? (q->field == SEARCH_FIELD_REGISTRATION ? "Exact registration" : "Exact call sign")
                                   : "Call sign / registration", eText) != ESP_OK)
        return ESP_FAIL;
    CsHit hits[HISTORY_SEARCH_MAX_MATCHES];
    size_t nHits = 0;

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
            bool hit;
            if (q->icaoAll) {
                hit = SameValue(r->rec.icao24, q->text);
            } else if (q->exact) {
                hit = (q->field != SEARCH_FIELD_REGISTRATION && SameValue(r->rec.callsign, q->text)) ||
                      (q->field != SEARCH_FIELD_CALLSIGN && r->rec.registry[0] && SameValue(r->rec.registry, q->text));
            } else {
                const bool csHit = q->field != SEARCH_FIELD_REGISTRATION && Pattern_Match(r->rec.callsign, q->text, mode);
                hit = csHit || (q->field != SEARCH_FIELD_CALLSIGN && r->rec.registry[0] &&
                                Pattern_Match(r->rec.registry, q->text, mode));
            }
            if (!hit)
                continue;
            if (q->icaoAll && nHits < HISTORY_SEARCH_MAX_MATCHES) {
                hits[nHits].fp = fp;
                snprintf(hits[nHits].cs, sizeof(hits[nHits].cs), "%s", r->rec.callsign);
                nHits++;
            }
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
            if (SendRow(req, &shown, fp, idx + (uint32_t)i, &bl, NULL) != ESP_OK)
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
    if (!matches && SendFormat(req, "<p class='meta'>No %s <b>%s</b>%s.</p>",
                               q->icaoAll ? "record for ICAO24" : "call sign or registration matching",
                               eText, finished ? "" : " in the records searched so far") != ESP_OK)
        return ESP_FAIL;
    if (q->icaoAll && nHits && SendCallsignSummary(req, eText, hits, nHits, finished) != ESP_OK)
        return ESP_FAIL;
    if (unreadable && SendFormat(req, "<p class='meta'>%" PRIu32 " unreadable record%s skipped "
                                      "(checksum mismatch, empty or truncated slot).</p>",
                                 unreadable, unreadable == 1 ? " was" : "s were") != ESP_OK)
        return ESP_FAIL;
    if (finished)
        return SendFormat(req, "<p class='meta'>End of search: %" PRIu32 " match%s in this pass. "
                               "<a href='/history%s'>All groups</a></p>", matches, matches == 1 ? "" : "es",
                          q->sort == 'd' ? "?s=d" : q->sort == 'a' ? "?s=a" : "");
    return SendFormat(req,
        "<p class='meta'>Searched %" PRIu32 " records. <a href='/history?q=%s&amp;m=t&amp;sf=%s%s%s%s&amp;sb=%08" PRIX32 "&amp;si=%" PRIu32 "'>"
        "Continue search &raquo;</a></p>", scanned, eUrl, q->icaoAll ? "i" : SearchField_Token(q->field),
        q->patternMode ? "&amp;pm=1" : "", q->exact ? "&amp;ex=1" : "",
        q->sort == 'd' ? "&amp;s=d" : q->sort == 'a' ? "&amp;s=a" : "", fp, idx);
}

static esp_err_t SearchMode(httpd_req_t *req, const HistQuery *q)
{
    char eText[sizeof(q->text) * 6 + 1];
    WebUtil_EscapeHtml(eText, sizeof(eText), q->text);
    if (!q->textMode && !q->patternMode && !q->icaoAll && !q->exact && q->field == SEARCH_FIELD_EITHER && LooksLikeIcao(q->text)) {
        bool found = false;
        if (IcaoLookup(req, q, &found) != ESP_OK)
            return ESP_FAIL;
        if (!found && SendFormat(req, "<p class='meta'>No history record for ICAO24 <b>%s</b> in the index.</p>", eText) != ESP_OK)
            return ESP_FAIL;
        /* Hex text can also be a call sign: offer the scan explicitly. 0.1.4: and every record of the aircraft. */
        return SendFormat(req, "<p><a href='/history?q=%s&amp;sf=i'>Every record of ICAO24 %s in all groups</a> &middot; "
                               "<a href='/history?q=%s&amp;m=t&amp;sf=e'>Search call signs and registrations for %s</a> "
                               "<small>(both scan the card; slower than the ICAO24 index)</small></p>", eText, eText, eText, eText);
    }
    return TextScan(req, q);
}

/* ---- browse: collapsed fingerprint groups, one expanded on demand ---- */

/* 0.1.4 cross-check (&x=1) of the aircraft on one expanded page: for each
 * distinct ICAO24 on the page, the call signs of ITS records in THIS group and
 * how many of its records are in OTHER groups - from the stored records only
 * (one bounded scan of the card, HISTORY_CHECK_RECORD_BUDGET records, TF lock
 * per chunk, nothing sent while scanning). Table in PSRAM (CPU-only, freed). */
#ifndef HISTORY_CHECK_RECORD_BUDGET
#define HISTORY_CHECK_RECORD_BUDGET 4000u /* host tests override it */
#endif
#define HISTORY_CHECK_CS_MAX 6u
typedef struct {
    char icao[TF_ICAO_MAX];
    uint8_t nCs;
    bool moreCs;
    bool annotated;
    uint16_t other;              /* records of this ICAO24 in other groups */
    char cs[HISTORY_CHECK_CS_MAX][TF_CALLSIGN_MAX];
} PageAircraft;
typedef struct {
    PageAircraft ac[HISTORY_PAGE_ROWS];
    size_t n;
    uint32_t scanned;
    bool complete;
    bool failed;
    char note[2048]; /* one row's note, built here instead of on the httpd stack */
    uint32_t fps[TF_LIST_MAX];          /* scan state kept here, not on the httpd stack */
    TfBrowseRecord recs[TF_BROWSE_CHUNK_MAX];
} PageCheck;

static PageAircraft *FindAc(PageCheck *pc, const char *icao)
{
    for (size_t i = 0; i < pc->n; i++)
        if (SameValue(pc->ac[i].icao, icao))
            return &pc->ac[i];
    return NULL;
}

static void RunPageCheck(PageCheck *pc, uint32_t fp, uint32_t first, uint32_t count)
{
    memset(pc, 0, sizeof(*pc));
    TfBrowseRecord *recs = pc->recs;
    uint32_t *fps = pc->fps;
    /* 1. the page's aircraft */
    for (uint32_t idx = first; idx < count && idx < first + HISTORY_PAGE_ROWS;) {
        size_t got = 0;
        uint32_t want = first + HISTORY_PAGE_ROWS - idx;
        if (want > TF_BROWSE_CHUNK_MAX)
            want = TF_BROWSE_CHUNK_MAX;
        TfBrowseStatus st = TfHistory_ScanChunk(fp, idx, want, recs, &got, &count);
        if (st != TF_BR_OK || got == 0) {
            pc->failed = st != TF_BR_END;
            break;
        }
        for (size_t i = 0; i < got; i++)
            if (recs[i].state != TF_REC_CORRUPT && recs[i].rec.icao24[0] && !FindAc(pc, recs[i].rec.icao24) &&
                pc->n < HISTORY_PAGE_ROWS)
                snprintf(pc->ac[pc->n++].icao, TF_ICAO_MAX, "%s", recs[i].rec.icao24);
        idx += (uint32_t)got;
    }
    if (pc->failed || pc->n == 0)
        return;
    /* 2. every group, in fingerprint order, within the record budget */
    size_t nFps = 0;
    bool more = false, afterValid = false;
    uint32_t after = 0;
    for (;;) {
        TfBrowseStatus st = TfHistory_ListBuckets(afterValid, after, TF_LIST_MAX, fps, &nFps, NULL, &more);
        if (st == TF_BR_END)
            break;
        if (st != TF_BR_OK) {
            pc->failed = true;
            return;
        }
        for (size_t g = 0; g < nFps; g++) {
            for (uint32_t idx = 0;;) {
                if (pc->scanned >= HISTORY_CHECK_RECORD_BUDGET)
                    return; /* complete stays false */
                size_t got = 0;
                uint32_t n = 0;
                st = TfHistory_ScanChunk(fps[g], idx, TF_BROWSE_CHUNK_MAX, recs, &got, &n);
                if (st == TF_BR_UNAVAILABLE || st == TF_BR_BUSY || st == TF_BR_IO) {
                    pc->failed = true;
                    return;
                }
                if (st == TF_BR_END || got == 0)
                    break;
                for (size_t i = 0; i < got; i++) {
                    PageAircraft *a = recs[i].state == TF_REC_CORRUPT ? NULL : FindAc(pc, recs[i].rec.icao24);
                    if (!a)
                        continue;
                    if (fps[g] != fp) {
                        if (a->other < UINT16_MAX)
                            a->other++;
                        continue;
                    }
                    if (!recs[i].rec.callsign[0])
                        continue;
                    bool dup = false;
                    for (size_t k = 0; k < a->nCs; k++)
                        dup |= !strcmp(a->cs[k], recs[i].rec.callsign);
                    if (dup)
                        continue;
                    if (a->nCs < HISTORY_CHECK_CS_MAX)
                        snprintf(a->cs[a->nCs++], TF_CALLSIGN_MAX, "%s", recs[i].rec.callsign);
                    else
                        a->moreCs = true;
                }
                pc->scanned += (uint32_t)got;
                idx += (uint32_t)got;
            }
        }
        if (!more || nFps == 0)
            break;
        afterValid = true;
        after = fps[nFps - 1];
    }
    pc->complete = true;
}

/* The note under a row's call sign: only on the first row of that aircraft on the
 * page, only when there is something to say. */
static void CheckNote(PageCheck *pc, const TfBrowseRecord *r, char *out, size_t cap)
{
    out[0] = '\0';
    PageAircraft *a = (pc && r->state != TF_REC_CORRUPT) ? FindAc(pc, r->rec.icao24) : NULL;
    if (!a || a->annotated)
        return;
    a->annotated = true;
    size_t n = 0;
    if (a->nCs > 1 || a->moreCs) {
        n += (size_t)snprintf(out + n, cap - n, "<small>Call signs in this group: ");
        for (size_t k = 0; k < a->nCs && n < cap; k++) {
            char e[TF_CALLSIGN_MAX * 6 + 1], u[TF_CALLSIGN_MAX * 3 + 1];
            WebUtil_EscapeHtml(e, sizeof(e), a->cs[k]);
            WebUtil_UrlEncode(u, sizeof(u), a->cs[k]);
            n += (size_t)snprintf(out + n, cap - n, "%s<a href='/history?q=%s&amp;sf=c&amp;ex=1'>%s</a>", k ? " &middot; " : "", u, e);
        }
        if (n < cap)
            n += (size_t)snprintf(out + n, cap - n, "%s</small>", a->moreCs ? " &middot; &hellip;" : "");
    }
    if (a->other && n < cap) {
        char u[TF_ICAO_MAX * 3 + 1];
        WebUtil_UrlEncode(u, sizeof(u), a->icao);
        n += (size_t)snprintf(out + n, cap - n, "<small><a href='/history?q=%s&amp;sf=i'>Additional history in other fingerprints (%u) &raquo;</a></small>",
                              u, (unsigned)a->other);
    }
    if (n >= cap)
        out[0] = '\0'; /* never send a cut tag (cannot happen with valid call signs) */
}

static esp_err_t SendExpandedGroup(httpd_req_t *req, const HistQuery *q, uint32_t fp, uint32_t count,
                                   const BucketLabel *bl)
{
    uint32_t first = q->offset;
    if (first >= count)
        first = count ? (count - 1) - (count - 1) % HISTORY_PAGE_ROWS : 0;
    char cursor[40], bare[36];
    ListState(q, cursor, sizeof(cursor), bare, sizeof(bare));
    PageCheck *pc = NULL;
    if (q->check) {
        pc = (PageCheck *)HIST_PSRAM_ALLOC(sizeof(PageCheck));
        if (!pc && SendChunk(req, "<tr><td colspan='7'>Not enough free memory for the cross-check; showing the records only.</td></tr>") != ESP_OK)
            return ESP_FAIL;
    }
    if (pc) {
        RunPageCheck(pc, fp, first, count);
        esp_err_t e = ESP_OK;
        if (pc->failed)
            e = SendChunk(req, "<tr><td colspan='7'>The cross-check could not read the card (busy or unavailable); "
                               "no other-call-sign or other-fingerprint notes are shown.</td></tr>");
        else if (!pc->complete)
            e = SendFormat(req, "<tr><td colspan='7'>Cross-check stopped after %" PRIu32 " records: the notes below may be "
                                "incomplete. An ICAO24 link lists every record of that aircraft.</td></tr>", pc->scanned);
        if (e != ESP_OK || pc->failed) {
            free(pc);
            pc = NULL;
            if (e != ESP_OK)
                return ESP_FAIL;
        }
    }
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
            free(pc);
            char row[400];
            snprintf(row, sizeof(row), "<tr><td colspan='7'>%s</td></tr>",
                     st == TF_BR_BUSY ? "The card is busy with history writes. Reload the page in a moment."
                                      : "The group's records could not be read. See <a href='/diag#tf'>Diagnostics</a>.");
            return SendChunk(req, row);
        }
        if (st == TF_BR_END || got == 0)
            break;
        for (size_t i = 0; i < got; i++) {
            if (pc)
                CheckNote(pc, &recs[i], pc->note, sizeof(pc->note));
            if (SendRow(req, &recs[i], fp, idx + (uint32_t)i, bl, pc ? pc->note : NULL) != ESP_OK) {
                free(pc);
                return ESP_FAIL;
            }
        }
        idx += (uint32_t)got;
        shown += (uint32_t)got;
    }
    free(pc);
    char prev[160] = "", next[160] = "", check[320] = "";
    if (!q->check && shown)
        snprintf(check, sizeof(check), " &nbsp; <a href='/history?%sg=%08" PRIX32 "&amp;o=%" PRIu32 "&amp;x=1#g%08" PRIX32 "' "
                 "title='Reads the card once (bounded)'>Check these aircraft for other call signs and other groups</a>",
                 cursor, fp, first, fp);
    if (first > 0)
        snprintf(prev, sizeof(prev), "<a href='/history?%sg=%08" PRIX32 "&amp;o=%" PRIu32 "#g%08" PRIX32 "'>&laquo; Previous</a> ",
                 cursor, fp, first >= HISTORY_PAGE_ROWS ? first - HISTORY_PAGE_ROWS : 0, fp);
    if (idx < count)
        snprintf(next, sizeof(next), "<a href='/history?%sg=%08" PRIX32 "&amp;o=%" PRIu32 "#g%08" PRIX32 "'>Next &raquo;</a>",
                 cursor, fp, idx, fp);
    if (shown == 0)
        return SendChunk(req, "<tr><td colspan='7'>No records in this group.</td></tr>");
    return SendFormat(req, "<tr><td colspan='7'>Showing %" PRIu32 "&ndash;%" PRIu32 " of %" PRIu32 " &nbsp; %s%s%s</td></tr>",
                      first + 1, first + shown, count, prev, next, check);
}

/* 0.1.4 order control for the group list (plain links, no script). */
static esp_err_t SendOrderLinks(httpd_req_t *req, char sort)
{
    return SendFormat(req, "<p class='meta'>Order: %s &middot; %s &middot; %s</p>",
                      sort == 0 ? "<b>Default (fingerprint)</b>" : "<a href='/history'>Default (fingerprint)</a>",
                      sort == 'd' ? "<b>Most records first</b>" : "<a href='/history?s=d'>Most records first</a>",
                      sort == 'a' ? "<b>Fewest records first</b>" : "<a href='/history?s=a'>Fewest records first</a>");
}

/* 0.1.4: groups ordered by their record count (the bucket header count shown in
 * each group row - the number of records in the group, not distinct aircraft or
 * call signs). Every group is listed by repeated directory passes, then one
 * header read per group (separate TF lock each, as on the default list) into a
 * transient PSRAM table of (fingerprint, count) only - no record is read.
 * Ties: fingerprint ascending (the default order), so equal counts never move.
 * Any group whose count cannot be read, or more groups than the table holds,
 * stops with an error instead of showing a wrong order. */
#define HISTORY_SORT_MAX_GROUPS 1024u
typedef struct {
    uint32_t fp;
    uint32_t count;
} GroupCount;

static char s_sortDir; /* qsort has no context argument; set right before qsort (httpd runs one request at a time) */
static int CmpGroupCount(const void *pa, const void *pb)
{
    const GroupCount *a = pa, *b = pb;
    if (a->count != b->count)
        return (s_sortDir == 'd') == (a->count > b->count) ? -1 : 1;
    return a->fp < b->fp ? -1 : a->fp > b->fp ? 1 : 0;
}

/* Pure: sorts the (fingerprint, count) table in place. */
static void SortGroupCounts(GroupCount *g, size_t n, char dir)
{
    s_sortDir = dir;
    qsort(g, n, sizeof(*g), CmpGroupCount);
}

static esp_err_t SortedBrowse(httpd_req_t *req, const HistQuery *q)
{
    GroupCount *g = (GroupCount *)HIST_PSRAM_ALLOC(sizeof(GroupCount) * HISTORY_SORT_MAX_GROUPS);
    if (!g)
        return SendNotice(req, "er", "Not enough free memory to sort the groups. <a href='/history'>Default order</a>");
    size_t n = 0;
    uint32_t total = 0, fps[TF_LIST_MAX];
    size_t got = 0;
    bool more = true, afterValid = false;
    uint32_t after = 0;
    TfBrowseStatus st = TF_BR_OK;
    while (more) {
        st = TfHistory_ListBuckets(afterValid, after, TF_LIST_MAX, fps, &got, &total, &more);
        if (st != TF_BR_OK || got == 0)
            break;
        if (n + got > HISTORY_SORT_MAX_GROUPS) {
            free(g);
            return SendFormat(req, "<p class='er'>There are more than %u groups; sorting by record count is not available. "
                                   "<a href='/history'>Default order</a></p>", (unsigned)HISTORY_SORT_MAX_GROUPS);
        }
        for (size_t i = 0; i < got; i++)
            g[n++].fp = fps[i];
        afterValid = true;
        after = fps[got - 1];
    }
    if (st == TF_BR_END && n == 0) {
        free(g);
        return SendNotice(req, "meta", "No history records have been stored on the card yet.");
    }
    if (st != TF_BR_OK && st != TF_BR_END) {
        free(g);
        return SendStatusNotice(req, st);
    }
    for (size_t i = 0; i < n; i++) {
        st = TfHistory_BucketInfo(g[i].fp, &g[i].count);
        if (st == TF_BR_OK)
            continue;
        const uint32_t badFp = g[i].fp;
        free(g);
        if (st == TF_BR_UNAVAILABLE || st == TF_BR_BUSY)
            return SendStatusNotice(req, st);
        return SendFormat(req, "<p class='er'>The record count of group %08" PRIX32 " could not be read (header unreadable), "
                               "so the groups cannot be sorted by record count. <a href='/history'>Default order</a> &middot; "
                               "<a href='/diag#tf'>Diagnostics</a></p>", badFp);
    }
    SortGroupCounts(g, n, q->sort);

    uint32_t pos = q->sortPos;
    if (pos >= n)
        pos = n ? (uint32_t)((n - 1) - (n - 1) % TF_LIST_MAX) : 0;
    HistQuery eq = *q;
    eq.sortPos = pos;
    if (SendFormat(req, "<h2>Fingerprints</h2><p class='meta'>%u group%s on the card, ordered by record count (%s; equal "
                        "counts in fingerprint order). Groups %u&ndash;%u.</p>",
                   (unsigned)n, n == 1 ? "" : "s", q->sort == 'd' ? "most first" : "fewest first",
                   (unsigned)(pos + 1), (unsigned)(pos + TF_LIST_MAX < n ? pos + TF_LIST_MAX : n)) != ESP_OK ||
        SendOrderLinks(req, q->sort) != ESP_OK || SendChunk(req, kTableHead) != ESP_OK) {
        free(g);
        return ESP_FAIL;
    }
    BucketLabel bl;
    esp_err_t err = ESP_OK;
    for (size_t i = pos; i < n && i < pos + TF_LIST_MAX && err == ESP_OK; i++) {
        BuildBucketLabel(&bl, g[i].fp);
        const bool expanded = eq.haveGroup && eq.group == g[i].fp;
        err = SendGroupRow(req, &eq, g[i].fp, true, g[i].count, expanded, &bl);
        if (err == ESP_OK && expanded)
            err = SendExpandedGroup(req, &eq, g[i].fp, g[i].count, &bl);
    }
    const size_t shownEnd = pos + TF_LIST_MAX;
    free(g);
    if (err != ESP_OK || SendChunk(req, "</table></div>") != ESP_OK)
        return ESP_FAIL;
    if (pos > 0 && SendFormat(req, "<p><a href='/history?s=%c&amp;p=%u'>&laquo; Previous groups</a></p>",
                              q->sort, (unsigned)(pos >= TF_LIST_MAX ? pos - TF_LIST_MAX : 0)) != ESP_OK)
        return ESP_FAIL;
    if (shownEnd < n)
        return SendFormat(req, "<p><a href='/history?s=%c&amp;p=%u'>Next groups &raquo;</a></p>", q->sort, (unsigned)shownEnd);
    return SendNotice(req, "meta", "End of groups.");
}

static esp_err_t BrowseMode(httpd_req_t *req, const HistQuery *q)
{
    if (q->sort)
        return SortedBrowse(req, q);
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
        SendOrderLinks(req, 0) != ESP_OK || SendChunk(req, kTableHead) != ESP_OK)
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
                "<option value='r'%s>Registration</option><option value='i'%s>ICAO24 (every record)</option></select></label> "
                "<label><input type='checkbox' name='pm' value='1'%s> Pattern mode</label>%s <button>Search</button> "
                "<a href='/history%s'>All groups</a></form>",
                eq, (int)(sizeof(q.text) - 1),
                q.field == SEARCH_FIELD_EITHER && !q.icaoAll ? " selected" : "",
                q.field == SEARCH_FIELD_CALLSIGN ? " selected" : "",
                q.field == SEARCH_FIELD_REGISTRATION ? " selected" : "",
                q.icaoAll ? " selected" : "",
                q.patternMode ? " checked" : "",
                q.sort == 'd' ? "<input type='hidden' name='s' value='d'>" : q.sort == 'a' ? "<input type='hidden' name='s' value='a'>" : "",
                q.sort == 'd' ? "?s=d" : q.sort == 'a' ? "?s=a" : "") != ESP_OK ||
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
                "<p><small>Each row is a record physically stored in a group file, with its own first seen, last seen and "
                "Seen count. <b>Superseded</b> = the aircraft has a newer record: it changed operator/registry group, or "
                "(since 0.1.4) its call sign; the old record is kept and shown, never deleted. Click an ICAO24 for every "
                "record of that aircraft, grouped by fingerprint; click a call sign to search it. Call signs that changed "
                "before 0.1.4 were overwritten and cannot be recovered. <b>Not indexed</b> = the index has no entry for it. The operator is derived from the group's "
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
