#pragma once
/* 0.1.8: persistent console-log capture, the platform-independent part (host-tested).
 *
 *  - LogRing:   single-producer / single-consumer byte ring (the producer side is serialized by the capture
 *               mutex in log_capture.c; the consumer is the writer task). No locks, no allocation.
 *  - Redaction: bounded, in-place masking of SSIDs/BSSIDs, coordinates and credentials in one captured line.
 *  - Names:     8.3 file names L<boot:5><segment:2>.LOG in /sdcard/FR7LOG (CONFIG_FATFS_LFN_NONE).
 *  - LogWriter: one writer cycle (markers, rotation, retention, free-space floor, error back-off, clean end),
 *               POSIX file calls only; locking, mount state and free space come from LogWriterEnv.
 * See log_capture.h for the ESP-IDF glue (hook, task, NVS) and README "Console log capture". */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOGCAP_DIR_NAME "FR7LOG"
#define LOGCAP_RING_SIZE 32768u              /* PSRAM, power of two not required */
#define LOGCAP_LINE_MAX 256u                 /* one captured record incl. newline; longer lines are truncated */
#define LOGCAP_BATCH_MAX 4096u               /* bytes per append */
#define LOGCAP_SEGMENT_MAX (1024u * 1024u)   /* 1 MiB per file */
#define LOGCAP_TOTAL_MAX (32ull * 1024u * 1024u)
#define LOGCAP_FILES_MAX 64u
#define LOGCAP_FREE_MIN (64ull * 1024u * 1024u)
#define LOGCAP_SEG_MAX 99u                   /* segments per boot: L<boot>01 .. L<boot>99 */
#define LOGCAP_BOOT_MOD 100000u              /* boot numbers 1..99999, then wrap to 1 */
#define LOGCAP_MARK_INTERVAL_S 300u
#define LOGCAP_RETRY_S 60u                   /* after a write/open error */
#define LOGCAP_SPACE_RECHECK_S 600u          /* while suspended for space/retention */
#define LOGCAP_SPACE_CHECK_BATCHES 64u
#define LOGCAP_LOCK_MS 500u
#define LOGCAP_NAME_LEN 13                   /* "L0004201.LOG" + NUL */
#define LOGCAP_MARK_MAX 1024u
#define LOGCAP_WRITER_BATCH (LOGCAP_BATCH_MAX + 2u * LOGCAP_MARK_MAX)
/* 0.1.8: card writes are batched (the writer wakes about every 5 s, or earlier when the buffer is half full) and a
 * write cycle is deferred while the largest free internal DMA-capable block is below this size: every 512-byte
 * block written to the card over SPI takes a short-lived internal DMA bounce buffer (spi_master, PSRAM source). */
#define LOGCAP_WRITE_INTERVAL_MS 5000u
#define LOGCAP_DMA_MIN_BYTES 4096u

/* ---- ring ---- */
typedef struct {
    uint8_t *buf;
    uint32_t size;
    uint32_t head; /* producer: total bytes ever put */
    uint32_t tail; /* consumer: total bytes ever consumed */
    uint32_t highWater;
} LogRing;

void LogRing_Init(LogRing *r, uint8_t *buf, uint32_t size);
uint32_t LogRing_Used(const LogRing *r);
/* Producer: all of it or nothing (false = no room; nothing written). */
bool LogRing_Put(LogRing *r, const char *data, uint32_t len);
/* Consumer: copy up to max bytes from the tail without consuming; cut after the last newline when the copy
 * holds at least one complete line. */
uint32_t LogRing_Peek(const LogRing *r, char *out, uint32_t max);
void LogRing_Consume(LogRing *r, uint32_t n);

/* ---- one captured record ---- */
/* Formats (vsnprintf), truncates to LOGCAP_LINE_MAX with "...[truncated]\n", redacts. Returns the length of
 * out (0 = nothing to capture). *truncated / *redactions receive counts for this record. */
size_t LogCap_FormatRecord(char *out, size_t cap, const char *fmt, va_list ap, bool *truncated, uint32_t *redactions);
/* In-place masking (length preserving). Returns the number of masked items. */
uint32_t LogCap_Redact(char *s, size_t len);

/* ---- names ---- */
bool LogCap_ParseName(const char *name, uint32_t *boot, uint32_t *seg);
void LogCap_MakeName(uint32_t boot, uint32_t seg, char out[LOGCAP_NAME_LEN]);
/* How many boots ago `boot` was, seen from `current` (circular, 0 = this boot). */
uint32_t LogCap_Age(uint32_t current, uint32_t boot);
uint32_t LogCap_NextBoot(uint32_t boot);

/* ---- writer ---- */
typedef enum {
    LW_NOT_STARTED = 0, /* nothing written yet this boot */
    LW_WRITING,
    LW_NO_CARD,         /* waiting for a mounted card; capture continues into the buffer */
    LW_BACKOFF,         /* last write/open failed; retry at retryAtS */
    LW_SUSPENDED,       /* free-space floor or retention could not be kept; re-checked at retryAtS */
    LW_SEGMENT_LIMIT,   /* 99 segments this boot: capture stops writing until reboot */
    LW_ENDED,           /* clean end written (shutdown) */
    LW_DEFERRED         /* 0.1.8: internal DMA memory low; writing resumes when it recovers (lines stay buffered) */
} LogWriterState;

const char *LogWriterState_Name(LogWriterState s);

typedef struct {
    bool (*lock)(void *ctx, uint32_t ms);
    void (*unlock)(void *ctx);
    bool (*mounted)(void *ctx);
    bool (*freeBytes)(void *ctx, uint64_t *out);
    void (*saveBoot)(void *ctx, uint32_t boot);           /* persist the boot number actually used */
    void (*localTime)(void *ctx, int64_t utc, char *out, size_t cap); /* "" when it cannot be converted */
    /* 0.1.8: largest free internal DMA-capable block in bytes (NULL = no check). */
    uint32_t (*dmaLargest)(void *ctx);
    void *ctx;
    const char *dir;        /* e.g. "/sdcard/FR7LOG" */
    const char *firmware;
    const char *resetReason;
} LogWriterEnv;

/* Counters kept by the capture hook (read by the writer to emit markers). */
typedef struct {
    uint32_t lines, dropFullLines, dropBusyLines, dropContextLines, truncated, redactions, ownLines;
    uint32_t dropLockLines; /* 0.1.8: logged by a task holding the TF storage lock (never captured) */
    uint32_t bytes, dropFullBytes; /* 32-bit on purpose: single-word updates from the hook */
    uint32_t failedAllocs, failSize, failCaps;
    char failTask[20];
    bool failTimeValid;
    uint32_t failUptimeS;
} LogCapCounters;

typedef struct {
    LogWriterState state;
    char reason[112];         /* why not writing (or "") */
    uint32_t boot, seg;
    char file[LOGCAP_NAME_LEN];
    uint64_t fileSize;
    bool haveFile, started, segEndWritten;
    uint64_t bytesWritten;
    uint32_t writes, writeErrors, closeErrors, lockTimeouts, filesCreated, filesDeleted, retentionFailures,
        interruptedMarked, spaceSuspends;
    int lastErrno;
    char lastError[112];
    uint32_t lastErrorUptimeS;
    uint32_t retryAtS;
    uint32_t nextMarkS;
    bool syncMarked, retryNote;
    uint32_t batchesSinceSpace;
    uint64_t lastFreeBytes;
    bool haveFreeBytes;
    uint32_t reportedDropFull, reportedDropBusy, reportedDropContext, reportedDropLock;
    /* 0.1.8 DMA deferral: cycles skipped, episodes, the current episode (if deferring) and the longest one */
    uint32_t deferredCycles, deferEpisodes, deferStartS, deferEpisodeCycles, longestDeferS, lastDeferDmaBytes;
    bool deferring;
    bool deferNote;                    /* write a WRITE DEFERRED marker with the next batch */
    uint32_t deferNoteCycles, deferNoteSecs;
    uint32_t reportedDropFullBytes;
    uint32_t reportedFails;
    const char *pinned; /* file a download is reading: never deleted (changed only under the storage lock) */
    /* 0.1.8: header / interrupted-marker text is built here (the writer state lives in PSRAM) instead of in stack
     * frames that stay live while fopen/fwrite run down through VFS, FATFS and the SD/SPI driver on the writer's
     * 4 KB internal stack. */
    char scratch[640];
} LogWriter;

void LogWriter_Init(LogWriter *w, uint32_t boot);
/* One cycle: optional markers + up to LOGCAP_BATCH_MAX bytes from the ring. `batch` must hold
 * LOGCAP_WRITER_BATCH bytes. shutdown: also write the clean end marker (call until it
 * returns false with the ring empty). Returns true if anything was written. */
bool LogWriter_Cycle(LogWriter *w, const LogWriterEnv *env, LogRing *ring, char *batch, uint32_t nowS, int64_t utc,
                     const LogCapCounters *ctr, bool shutdown, const char *shutdownReason);

/* ---- directory helpers (caller holds the storage lock) ---- */
typedef struct {
    char name[LOGCAP_NAME_LEN];
    uint32_t size;
    int64_t mtime; /* 0 = unknown */
} LogFileInfo;
/* Owned files only (name pattern), unsorted; returns the count stored (<= max), *total = all owned. */
size_t LogCap_List(const char *dir, LogFileInfo *out, size_t max, size_t *total);
/* Strict validation for HTTP parameters: exactly L#######.LOG (upper case). */
bool LogCap_ValidName(const char *name);
