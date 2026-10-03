#include "web_history.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "craft_types.h"
#include "custom_rules.h"
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

/* ---- query ---- */

typedef struct {
    bool haveBucket;
    uint32_t bucket;
    uint32_t offset;
    bool onlyBucket;
    char icao[TF_ICAO_MAX]; /* "" = browse */
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

static void ParseQuery(httpd_req_t *req, HistQuery *q)
{
    memset(q, 0, sizeof(*q));
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return;
    char value[24];
    if (httpd_query_key_value(query, "q", value, sizeof(value)) == ESP_OK) {
        WebUtil_UrlDecodeInPlace(value);
        size_t used = 0;
        for (const char *p = value; *p && used < TF_ICAO_MAX - 1; p++)
            if (isalnum((unsigned char)*p))
                q->icao[used++] = *p;
        q->icao[used] = '\0';
        if (q->icao[0])
            return; /* lookup mode ignores the browse cursor */
    }
    if (httpd_query_key_value(query, "b", value, sizeof(value)) == ESP_OK && ParseHex8(value, &q->bucket))
        q->haveBucket = true;
    if (q->haveBucket && httpd_query_key_value(query, "o", value, sizeof(value)) == ESP_OK) {
        char *end = NULL;
        unsigned long o = strtoul(value, &end, 10);
        if (end && *end == '\0' && o < 0x7FFFFFFFul)
            q->offset = (uint32_t)o;
    }
    if (httpd_query_key_value(query, "only", value, sizeof(value)) == ESP_OK && strcmp(value, "1") == 0)
        q->onlyBucket = true;
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

    char eIcao[TF_ICAO_MAX * 6 + 1], eCs[TF_CALLSIGN_MAX * 6 + 1];
    WebUtil_EscapeHtml(eIcao, sizeof(eIcao), r->rec.icao24);
    WebUtil_EscapeHtml(eCs, sizeof(eCs), r->rec.callsign);

    const char *ac = AircraftType_IsValid(r->rec.aircraftType) ? AircraftType_Name((AircraftType)r->rec.aircraftType) : "Unknown";
    const char *ct = CraftType_IsValid(r->rec.craftType) ? CraftType_Name((CraftType)r->rec.craftType) : "Unknown";

    char first[64], last[64];
    TimeUtil_FormatLocal((int64_t)r->rec.firstSeen, first, sizeof(first));
    TimeUtil_FormatLocal((int64_t)r->rec.lastSeen, last, sizeof(last));

    return SendFormat(req,
        "<tr%s><td>%s%s<small><a href='/history?b=%08" PRIX32 "&amp;only=1'>group %08" PRIX32 "</a> #%" PRIu32 "</small></td>"
        "<td>%s</td><td title='Classified by: %s'>%s / %s</td><td>%s</td><td>%s</td><td>%s</td><td>%" PRIu32 "</td></tr>",
        r->state == TF_REC_SUPERSEDED ? " class='sup'" : "", eIcao, StateBadge(r->state), fp, fp, idx,
        r->rec.callsign[0] ? eCs : "&mdash;",
        CraftSource_Name((CraftSource)r->rec.classificationSource), ac, ct,
        bl->html, first, last, r->rec.seenCount);
}

static esp_err_t SendGroupHeader(httpd_req_t *req, uint32_t fp, uint32_t count, uint32_t startIdx, const BucketLabel *bl)
{
    const bool plain = (fp == TF_BUCKET_UNASSIGNED || fp == TF_BUCKET_REGISTRY_DEFINED);
    return SendFormat(req,
        "<tr class='bk'><td colspan='7'>%s%s%s &middot; group %08" PRIX32 " &middot; %" PRIu32 " record%s%s</td></tr>",
        BucketTitle(fp), plain ? "" : ": ", plain ? "" : bl->html, fp, count, count == 1 ? "" : "s",
        startIdx ? " (continued)" : "");
}

static esp_err_t SendNotice(httpd_req_t *req, const char *cls, const char *text)
{
    return SendFormat(req, "<p class='%s'>%s</p>", cls, text);
}

static const char *kHeadCss =
    "body{max-width:1100px}table{font-size:.9em}td,th{padding:.35em;white-space:nowrap}"
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

/* ---- exact lookup ---- */

static esp_err_t LookupMode(httpd_req_t *req, const HistQuery *q)
{
    /* Stored ICAO24 case follows the provider; try as typed, lower, upper. */
    char tries[3][TF_ICAO_MAX];
    snprintf(tries[0], TF_ICAO_MAX, "%s", q->icao);
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

    if (st == TF_BR_END) {
        char e[TF_ICAO_MAX * 6 + 1];
        WebUtil_EscapeHtml(e, sizeof(e), q->icao);
        return SendFormat(req, "<p class='meta'>No history record for <b>%s</b> in the index.</p>", e);
    }
    if (st != TF_BR_OK)
        return SendStatusNotice(req, st);

    BucketLabel bl;
    BuildBucketLabel(&bl, fp);
    if (SendChunk(req, "<div class='w'><table><tr><th>ICAO24</th><th>Call Sign</th><th>Aircraft / Class</th>"
                       "<th>Operator</th><th>First seen</th><th>Last seen</th><th>Seen</th></tr>") != ESP_OK ||
        SendRow(req, &rec, fp, idx, &bl) != ESP_OK ||
        SendChunk(req, "</table></div>") != ESP_OK)
        return ESP_FAIL;
    return ESP_OK;
}

/* ---- browse ---- */

static esp_err_t BrowseMode(httpd_req_t *req, const HistQuery *q)
{
    uint32_t fp = 0, count = 0, idx = 0;
    bool headerOk = true;
    TfBrowseStatus st;

    if (q->haveBucket) {
        fp = q->bucket;
        idx = q->offset;
        st = TfHistory_BucketInfo(fp, &count);
        if (st == TF_BR_END) {
            return SendNotice(req, "wn", "That group does not exist (or its header is unreadable). "
                                         "<a href='/history'>Back to all history</a>");
        }
        if (st != TF_BR_OK)
            return SendStatusNotice(req, st);
    } else {
        st = TfHistory_NextBucket(false, 0, &fp, &count, &headerOk);
        if (st == TF_BR_END)
            return SendNotice(req, "meta", "No history records have been stored on the card yet.");
        if (st != TF_BR_OK)
            return SendStatusNotice(req, st);
    }

    if (SendChunk(req, "<div class='w'><table><tr><th>ICAO24</th><th>Call Sign</th><th>Aircraft / Class</th>"
                       "<th>Operator</th><th>First seen</th><th>Last seen</th><th>Seen</th></tr>") != ESP_OK)
        return ESP_FAIL;

    BucketLabel bl = {.valid = false};
    bool needHeader = true;
    uint32_t rows = 0;
    bool aborted = false;
    bool moreAfter = false;
    uint32_t nextFp = 0; /* start of the next page when it begins in another group */
    bool nextIsNewGroup = false;

    while (rows < HISTORY_PAGE_ROWS) {
        if (!bl.valid || bl.fp != fp)
            BuildBucketLabel(&bl, fp);

        if (!headerOk) {
            if (SendFormat(req, "<tr class='sup'><td colspan='7'>Group %08" PRIX32
                                " has an unreadable header; its records are not listed.</td></tr>", fp) != ESP_OK)
                return ESP_FAIL;
            rows++;
            goto advance;
        }
        if (idx >= count)
            goto advance;

        if (needHeader) {
            if (SendGroupHeader(req, fp, count, idx, &bl) != ESP_OK)
                return ESP_FAIL;
            needHeader = false;
        }

        {
            TfBrowseRecord recs[TF_BROWSE_CHUNK_MAX];
            size_t got = 0;
            size_t want = HISTORY_PAGE_ROWS - rows;
            if (want > TF_BROWSE_CHUNK_MAX)
                want = TF_BROWSE_CHUNK_MAX;
            /* The TF mutex is held only inside this call; the rows are sent after it returns. */
            st = TfHistory_BrowseChunk(fp, idx, want, recs, &got, &count);
            if (st == TF_BR_UNAVAILABLE || st == TF_BR_BUSY || st == TF_BR_IO) {
                if (SendStatusNotice(req, st) != ESP_OK)
                    return ESP_FAIL;
                aborted = true;
                break;
            }
            if (st == TF_BR_END || got == 0) {
                idx = count; /* bucket shrank/vanished under us: move on */
                goto advance;
            }
            for (size_t i = 0; i < got; i++)
                if (SendRow(req, &recs[i], fp, idx + (uint32_t)i, &bl) != ESP_OK)
                    return ESP_FAIL;
            idx += (uint32_t)got;
            rows += (uint32_t)got;
        }
        continue;

    advance:
        if (headerOk && idx < count)
            continue;
        if (q->onlyBucket) {
            idx = count;
            break;
        }
        {
            uint32_t nfp = 0, ncount = 0;
            bool nok = true;
            st = TfHistory_NextBucket(true, fp, &nfp, &ncount, &nok);
            if (st == TF_BR_END) {
                idx = count;
                break;
            }
            if (st != TF_BR_OK) {
                if (SendStatusNotice(req, st) != ESP_OK)
                    return ESP_FAIL;
                aborted = true;
                break;
            }
            fp = nfp;
            count = ncount;
            headerOk = nok;
            idx = 0;
            needHeader = true;
        }
    }

    if (SendChunk(req, "</table></div>") != ESP_OK)
        return ESP_FAIL;
    if (aborted)
        return ESP_OK;

    /* Is there anything after the last row we showed? */
    if (headerOk && idx < count) {
        moreAfter = true;
    } else if (!q->onlyBucket) {
        uint32_t nfp = 0, ncount = 0;
        bool nok = true;
        if (TfHistory_NextBucket(true, fp, &nfp, &ncount, &nok) == TF_BR_OK) {
            moreAfter = true;
            nextFp = nfp;
            nextIsNewGroup = true;
        }
    }

    if (q->onlyBucket)
        return SendFormat(req, "<p>%s%s</p>",
                          moreAfter ? "" : "End of this group. ", "<a href='/history'>All history</a>");

    const uint32_t linkFp = nextIsNewGroup ? nextFp : fp;
    const uint32_t linkOff = nextIsNewGroup ? 0 : idx;
    if (moreAfter)
        return SendFormat(req, "<p><a href='/history'>&laquo; First page</a> &nbsp; "
                               "<a href='/history?b=%08" PRIX32 "&amp;o=%" PRIu32 "'>Next &raquo;</a></p>",
                          linkFp, linkOff);
    return SendNotice(req, "meta", q->haveBucket ? "End of history. <a href='/history'>First page</a>" : "End of history.");
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
        char eq[TF_ICAO_MAX * 6 + 1];
        WebUtil_EscapeHtml(eq, sizeof(eq), q.icao);
        if (SendFormat(req,
                "<form method='get' action='/history'><label>ICAO24 <input name='q' value='%s' maxlength='%d' "
                "placeholder='e.g. a1b2c3'></label> <button>Look up</button> <a href='/history'>Browse all</a></form>"
                "<p class='meta'>%u aircraft in the index (limit %u). Groups are listed in fingerprint order, "
                "not by time. &middot; <a href='/diag#tf'>Storage diagnostics</a></p>",
                eq, (int)(TF_ICAO_MAX - 1), (unsigned)tf.indexSlotsUsed, (unsigned)tf.indexSlotsTotal) != ESP_OK)
            return ESP_FAIL;
        err = q.icao[0] ? LookupMode(req, &q) : BrowseMode(req, &q);
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
