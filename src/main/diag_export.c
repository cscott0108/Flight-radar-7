// 0.1.7: streaming HTML -> text / JSON converter for the /diag export (see diag_export.h).
#include "diag_export.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { LEX_TEXT = 0, LEX_TAG, LEX_ENT };
enum { IT_NONE = 0, IT_GROUP, IT_HEADING, IT_ROW, IT_TEXT };

/* ---- output ---- */

bool DiagExport_Flush(DiagExport *ex)
{
    if (ex->failed)
        return false;
    if (ex->outLen) {
        if (ex->sink(ex->sinkCtx, ex->out, ex->outLen) != 0)
            ex->failed = true;
        ex->outLen = 0;
    }
    return !ex->failed;
}

bool DiagExport_WriteN(DiagExport *ex, const char *s, size_t n)
{
    while (n && !ex->failed) {
        size_t room = sizeof(ex->out) - ex->outLen;
        if (!room) {
            DiagExport_Flush(ex);
            continue;
        }
        size_t k = n < room ? n : room;
        memcpy(ex->out + ex->outLen, s, k);
        ex->outLen += k;
        s += k;
        n -= k;
    }
    return !ex->failed;
}

bool DiagExport_Write(DiagExport *ex, const char *s)
{
    return DiagExport_WriteN(ex, s, strlen(s));
}

bool DiagExport_Printf(DiagExport *ex, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return false;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1; /* callers only print short numeric fields; never reached in practice */
    return DiagExport_WriteN(ex, buf, (size_t)n);
}

static void JsonChar(DiagExport *ex, unsigned char c)
{
    char esc[8];
    if (c == '"' || c == '\\') {
        esc[0] = '\\';
        esc[1] = (char)c;
        DiagExport_WriteN(ex, esc, 2);
    } else if (c < 0x20) {
        snprintf(esc, sizeof(esc), "\\u%04x", c);
        DiagExport_WriteN(ex, esc, 6);
    } else {
        DiagExport_WriteN(ex, (const char *)&c, 1);
    }
}

bool DiagExport_JsonString(DiagExport *ex, const char *s)
{
    if (!s)
        return DiagExport_Write(ex, "null");
    DiagExport_Write(ex, "\"");
    for (; *s; s++)
        JsonChar(ex, (unsigned char)*s);
    return DiagExport_Write(ex, "\"");
}

/* ---- items ---- */

static void ItemPrefixJson(DiagExport *ex)
{
    DiagExport_Write(ex, ex->firstItem ? "\n" : ",\n");
    ex->firstItem = false;
}

static void CloseCell(DiagExport *ex)
{
    if (!ex->inCell)
        return;
    if (ex->fmt == DIAG_EXPORT_JSON)
        DiagExport_Write(ex, "\"");
    ex->inCell = false;
}

static void CloseItem(DiagExport *ex)
{
    switch (ex->item) {
    case IT_GROUP:
        ex->capture[ex->captureLen] = 0;
        snprintf(ex->group, sizeof(ex->group), "%s", ex->capture);
        ex->section[0] = 0;
        DiagExport_Write(ex, ex->fmt == DIAG_EXPORT_JSON ? "\"}" : " ==\n");
        break;
    case IT_HEADING:
        ex->capture[ex->captureLen] = 0;
        if (ex->headingLevel >= 3)
            snprintf(ex->section, sizeof(ex->section), "%s", ex->capture);
        if (ex->fmt == DIAG_EXPORT_JSON)
            DiagExport_Write(ex, "\"}");
        else
            DiagExport_Write(ex, ex->headingLevel >= 4 ? " --\n" : ex->headingLevel == 3 ? " ---\n" : "\n");
        break;
    case IT_ROW:
        CloseCell(ex);
        if (ex->fmt == DIAG_EXPORT_JSON)
            DiagExport_Printf(ex, "],\"header\":%s}", (ex->rowHasCell && ex->rowAllHeader) ? "true" : "false");
        else
            DiagExport_Write(ex, "\n");
        ex->rows++;
        break;
    case IT_TEXT:
        DiagExport_Write(ex, ex->fmt == DIAG_EXPORT_JSON ? "\"}" : "\n");
        break;
    default:
        return;
    }
    ex->item = IT_NONE;
    ex->items++;
}

static void OpenItem(DiagExport *ex, uint8_t kind, uint8_t level)
{
    CloseItem(ex);
    ex->item = kind;
    ex->headingLevel = level;
    ex->pendingSpace = false;
    ex->itemHasText = false;
    ex->captureLen = 0;
    const bool json = ex->fmt == DIAG_EXPORT_JSON;
    if (json)
        ItemPrefixJson(ex);
    switch (kind) {
    case IT_GROUP:
        DiagExport_Write(ex, json ? "{\"type\":\"group\",\"text\":\"" : "\n\n== ");
        break;
    case IT_HEADING:
        if (json)
            DiagExport_Printf(ex, "{\"type\":\"heading\",\"level\":%u,\"text\":\"", (unsigned)level);
        else
            DiagExport_Write(ex, level >= 4 ? "\n-- " : level == 3 ? "\n--- " : "\n# ");
        break;
    case IT_ROW:
        ex->rowHasCell = false;
        ex->rowAllHeader = true;
        ex->firstCell = true;
        ex->inCell = false;
        if (json) {
            DiagExport_Write(ex, "{\"type\":\"row\",\"group\":");
            DiagExport_JsonString(ex, ex->group);
            DiagExport_Write(ex, ",\"section\":");
            DiagExport_JsonString(ex, ex->section);
            DiagExport_Write(ex, ",\"cells\":[");
        }
        break;
    case IT_TEXT:
        if (json) {
            DiagExport_Write(ex, "{\"type\":\"text\",\"group\":");
            DiagExport_JsonString(ex, ex->group);
            DiagExport_Write(ex, ",\"section\":");
            DiagExport_JsonString(ex, ex->section);
            DiagExport_Write(ex, ",\"text\":\"");
        }
        break;
    default:
        break;
    }
}

static void OpenCell(DiagExport *ex, bool header)
{
    if (ex->item != IT_ROW)
        OpenItem(ex, IT_ROW, 0);
    CloseCell(ex);
    if (ex->fmt == DIAG_EXPORT_JSON)
        DiagExport_Write(ex, ex->firstCell ? "\"" : ",\"");
    else if (!ex->firstCell)
        DiagExport_Write(ex, " | ");
    ex->firstCell = false;
    ex->inCell = true;
    ex->rowHasCell = true;
    if (!header)
        ex->rowAllHeader = false;
    ex->pendingSpace = false;
    ex->itemHasText = false;
}

/* One decoded output byte of page text. */
static void TextByte(DiagExport *ex, unsigned char c)
{
    if (ex->skipDepth)
        return;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ex->pendingSpace = true;
        return;
    }
    if (ex->item == IT_ROW && !ex->inCell)
        return; /* whitespace/stray text between cells */
    if (ex->item == IT_NONE)
        OpenItem(ex, IT_TEXT, 0);
    const bool json = ex->fmt == DIAG_EXPORT_JSON;
    if (ex->pendingSpace && ex->itemHasText) {
        DiagExport_WriteN(ex, " ", 1);
        if ((ex->item == IT_GROUP || ex->item == IT_HEADING) && (size_t)ex->captureLen + 1u < sizeof(ex->capture))
            ex->capture[ex->captureLen++] = ' ';
    }
    ex->pendingSpace = false;
    ex->itemHasText = true;
    if (json)
        JsonChar(ex, c);
    else
        DiagExport_WriteN(ex, (const char *)&c, 1);
    if ((ex->item == IT_GROUP || ex->item == IT_HEADING) && (size_t)ex->captureLen + 1u < sizeof(ex->capture))
        ex->capture[ex->captureLen++] = (char)c;
}

static void TextStr(DiagExport *ex, const char *s)
{
    for (; *s; s++)
        TextByte(ex, (unsigned char)*s);
}

/* ---- tags ---- */

static bool TagIs(const DiagExport *ex, const char *name)
{
    return strcmp(ex->tag, name) == 0;
}

static bool IsSkipTag(const DiagExport *ex)
{
    static const char *const kSkip[] = {"script", "style", "form", "button", "select", "textarea", "head", "title"};
    for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); i++)
        if (TagIs(ex, kSkip[i]))
            return true;
    return false;
}

static void HandleTag(DiagExport *ex)
{
    ex->tag[ex->tagLen] = 0;
    if (!ex->tag[0] || ex->tag[0] == '!')
        return; /* comment / doctype */
    if (IsSkipTag(ex)) {
        if (ex->tagClose) {
            if (ex->skipDepth)
                ex->skipDepth--;
        } else if (!ex->tagSelfClose) {
            if (!ex->skipDepth)
                CloseItem(ex); /* controls end the current paragraph */
            ex->skipDepth++;
        }
        return;
    }
    if (ex->skipDepth)
        return;

    const bool close = ex->tagClose;
    if (TagIs(ex, "summary")) {
        if (close) {
            if (ex->item == IT_GROUP)
                CloseItem(ex);
        } else {
            OpenItem(ex, IT_GROUP, 0);
        }
    } else if (ex->tag[0] == 'h' && ex->tag[1] >= '1' && ex->tag[1] <= '6' && !ex->tag[2]) {
        if (close) {
            if (ex->item == IT_HEADING)
                CloseItem(ex);
        } else {
            OpenItem(ex, IT_HEADING, (uint8_t)(ex->tag[1] - '0'));
        }
    } else if (TagIs(ex, "tr")) {
        if (close) {
            if (ex->item == IT_ROW)
                CloseItem(ex);
        } else {
            OpenItem(ex, IT_ROW, 0);
        }
    } else if (TagIs(ex, "td") || TagIs(ex, "th")) {
        if (close)
            CloseCell(ex);
        else
            OpenCell(ex, TagIs(ex, "th"));
    } else if (TagIs(ex, "br")) {
        if (ex->item != IT_NONE && ex->itemHasText) {
            ex->pendingSpace = true;
            TextStr(ex, "/");
            ex->pendingSpace = true;
        }
    } else if (TagIs(ex, "table") || TagIs(ex, "p") || TagIs(ex, "div") || TagIs(ex, "ul") || TagIs(ex, "ol") ||
               TagIs(ex, "li") || TagIs(ex, "details") || TagIs(ex, "body") || TagIs(ex, "html") || TagIs(ex, "header") ||
               TagIs(ex, "nav") || TagIs(ex, "fieldset") || TagIs(ex, "legend")) {
        if (ex->inCell)
            ex->pendingSpace = true; /* block inside a cell: just a separator */
        else
            CloseItem(ex);
    } else {
        /* inline (b, span, small, a, code, label, i, em, strong, input, ...): transparent; a word boundary only
         * where whitespace already was */
    }
}

/* ---- entities ---- */

static void PutUtf8(DiagExport *ex, unsigned cp)
{
    unsigned char b[4];
    int n;
    if (cp < 0x80) { b[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (unsigned char)(0xC0 | (cp >> 6)); b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (unsigned char)(0xE0 | (cp >> 12)); b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                             b[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3; }
    else { b[0] = (unsigned char)(0xF0 | (cp >> 18)); b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
           b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4; }
    for (int i = 0; i < n; i++)
        TextByte(ex, b[i]);
}

static void HandleEntity(DiagExport *ex)
{
    static const struct { const char *name; unsigned cp; } kEnt[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
        {"rarr", 0x2192}, {"larr", 0x2190}, {"uarr", 0x2191}, {"darr", 0x2193}, {"deg", 0xB0}, {"mdash", 0x2014},
        {"ndash", 0x2013}, {"middot", 0xB7}, {"hellip", 0x2026}, {"ge", 0x2265}, {"le", 0x2264}, {"times", 0xD7},
        {"plusmn", 0xB1}, {"micro", 0xB5}, {"asymp", 0x2248}, {"ne", 0x2260}, {"bull", 0x2022},
    };
    ex->ent[ex->entLen] = 0;
    if (ex->ent[0] == '#') {
        unsigned cp = 0;
        bool hex = ex->ent[1] == 'x' || ex->ent[1] == 'X';
        const char *p = ex->ent + (hex ? 2 : 1);
        bool ok = *p != 0;
        for (; *p && ok; p++) {
            char c = *p;
            int d = (c >= '0' && c <= '9') ? c - '0' : (hex && c >= 'a' && c <= 'f') ? c - 'a' + 10
                    : (hex && c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0 || cp > 0x10FFFF)
                ok = false;
            else
                cp = cp * (hex ? 16u : 10u) + (unsigned)d;
        }
        if (ok && cp > 0 && cp <= 0x10FFFF) {
            PutUtf8(ex, cp);
            return;
        }
    } else {
        for (size_t i = 0; i < sizeof(kEnt) / sizeof(kEnt[0]); i++)
            if (strcmp(ex->ent, kEnt[i].name) == 0) {
                PutUtf8(ex, kEnt[i].cp);
                return;
            }
    }
    /* unknown: keep it literally */
    TextByte(ex, '&');
    TextStr(ex, ex->ent);
    TextByte(ex, ';');
}

/* ---- driver ---- */

void DiagExport_Begin(DiagExport *ex, DiagExportFormat fmt, DiagExportSink sink, void *sinkCtx)
{
    memset(ex, 0, sizeof(*ex));
    ex->fmt = fmt;
    ex->sink = sink;
    ex->sinkCtx = sinkCtx;
    ex->firstItem = true;
}

bool DiagExport_Feed(DiagExport *ex, const char *html, size_t len)
{
    for (size_t i = 0; i < len && !ex->failed; i++) {
        const char c = html[i];
        switch (ex->lex) {
        case LEX_TEXT:
            if (c == '<') {
                ex->lex = LEX_TAG;
                ex->tagLen = 0;
                ex->tagClose = ex->tagNameDone = ex->tagSelfClose = false;
                ex->quote = 0;
            } else if (c == '&' && !ex->skipDepth) {
                ex->lex = LEX_ENT;
                ex->entLen = 0;
            } else {
                TextByte(ex, (unsigned char)c);
            }
            break;
        case LEX_TAG:
            if (ex->quote) {
                if (c == ex->quote)
                    ex->quote = 0;
            } else if (c == '>') {
                ex->lex = LEX_TEXT;
                HandleTag(ex);
            } else if (!ex->tagNameDone) {
                if (c == '/' && ex->tagLen == 0) {
                    ex->tagClose = true;
                } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
                    ex->tagNameDone = ex->tagLen > 0;
                    if (c == '/')
                        ex->tagSelfClose = true;
                } else if ((size_t)ex->tagLen + 1u < sizeof(ex->tag)) {
                    ex->tag[ex->tagLen++] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
                }
            } else if (c == '\'' || c == '"') {
                ex->quote = c;
            } else if (c == '/') {
                ex->tagSelfClose = true;
            } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                ex->tagSelfClose = false; /* '/' only counts right before '>' */
            }
            break;
        case LEX_ENT:
            if (c == ';') {
                ex->lex = LEX_TEXT;
                HandleEntity(ex);
            } else if (((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#') &&
                       (size_t)ex->entLen + 1u < sizeof(ex->ent)) {
                ex->ent[ex->entLen++] = c;
            } else {
                /* not an entity: emit what we have literally, then reprocess this character as text */
                ex->lex = LEX_TEXT;
                ex->ent[ex->entLen] = 0;
                TextByte(ex, '&');
                TextStr(ex, ex->ent);
                i--;
            }
            break;
        default:
            ex->lex = LEX_TEXT;
            break;
        }
    }
    return !ex->failed;
}

bool DiagExport_Finish(DiagExport *ex)
{
    if (ex->lex == LEX_ENT) { /* dangling '&...' at the very end */
        ex->ent[ex->entLen] = 0;
        TextByte(ex, '&');
        TextStr(ex, ex->ent);
    }
    ex->lex = LEX_TEXT;
    CloseItem(ex);
    return !ex->failed;
}
