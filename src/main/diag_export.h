#pragma once
/* 0.1.7: /diag export (GET /diag?fmt=txt | fmt=json).
 *
 * The /diag page is generated once, as HTML, by web_diag.c. For an export the same byte stream is fed through
 * this streaming converter instead of being sent to the browser, so every row, label, unit, timestamp and
 * evidence text comes from exactly the same code that draws the page: no metric is computed twice.
 *
 *  - TEXT: "== Group ==", "--- Section ---", table rows as "cell | cell | cell", paragraphs as plain lines.
 *  - JSON: an ordered array of items {"type":"group"|"heading"|"row"|"text", ...}; rows carry their cells as
 *          strings plus the group and section they belong to. The caller writes the typed "metrics" object
 *          and the surrounding document (see web_diag.c and README "Diagnostics export").
 *
 * Controls are not report content: <form>, <button>, <select>, <script>, <style>, <head>, <title> and
 * <textarea> contents are skipped. HTML entities used by the page are decoded to UTF-8.
 *
 * Pure C, no allocation: the caller owns the state (web_diag.c keeps it in PSRAM). Output is batched in a
 * small buffer and handed to a sink; the first sink failure is sticky and stops all further output. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { DIAG_EXPORT_TEXT = 0, DIAG_EXPORT_JSON = 1 } DiagExportFormat;

/* Returns 0 on success; anything else marks the export as failed (e.g. the client disconnected). */
typedef int (*DiagExportSink)(void *ctx, const char *data, size_t len);

#define DIAG_EXPORT_NAME_MAX 96
#define DIAG_EXPORT_OUTBUF 512

typedef struct {
    DiagExportFormat fmt;
    DiagExportSink sink;
    void *sinkCtx;
    bool failed;

    /* tokenizer */
    uint8_t lex;            /* text / tag / entity */
    char tag[16];
    uint8_t tagLen;
    bool tagClose, tagNameDone, tagSelfClose;
    char quote;             /* inside an attribute value */
    char ent[12];
    uint8_t entLen;
    uint16_t skipDepth;     /* inside skipped containers */

    /* structure */
    uint8_t item;           /* open output item: none / group / heading / row / text */
    uint8_t headingLevel;
    bool inCell, cellIsHeader, rowHasCell, rowAllHeader;
    bool pendingSpace, itemHasText;
    bool firstItem, firstCell;
    char group[DIAG_EXPORT_NAME_MAX];
    char section[DIAG_EXPORT_NAME_MAX];
    char capture[DIAG_EXPORT_NAME_MAX]; /* text of an open group/heading */
    uint8_t captureLen;

    uint32_t rows, items;

    char out[DIAG_EXPORT_OUTBUF];
    size_t outLen;
} DiagExport;

void DiagExport_Begin(DiagExport *ex, DiagExportFormat fmt, DiagExportSink sink, void *sinkCtx);
/* Feed a chunk of the page's HTML. Returns false once the sink has failed. */
bool DiagExport_Feed(DiagExport *ex, const char *html, size_t len);
/* Close any open item (JSON: the item array is NOT closed; the caller writes "]" and the rest). */
bool DiagExport_Finish(DiagExport *ex);

/* Raw output helpers for the caller's own framing (header, typed metrics, trailer). */
bool DiagExport_Write(DiagExport *ex, const char *s);
bool DiagExport_WriteN(DiagExport *ex, const char *s, size_t n);
bool DiagExport_Printf(DiagExport *ex, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* A JSON string literal (with quotes); NULL writes null. */
bool DiagExport_JsonString(DiagExport *ex, const char *s);
bool DiagExport_Flush(DiagExport *ex);
