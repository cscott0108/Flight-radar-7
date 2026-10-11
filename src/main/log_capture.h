#pragma once
/* 0.1.8: "Save console logs to TF card" (persistent console-log capture), ESP-IDF glue.
 *
 * Setting: NVS namespace "diag", key "logcap" (u8, default 0 = off). Read ONCE at boot by
 * LogCapture_LoadAtBoot(); changing it (LogCapture_SetSaved) takes effect at the next boot. With the setting
 * off nothing is allocated, no task is created and no hook is installed.
 *
 * When on (this boot): one PSRAM block (40,800 B: 32 KB ring, 256-byte line buffer, 6 KB write batch, writer state),
 * one FreeRTOS mutex and a writer task "LogWr" (4096-byte INTERNAL stack, priority 2) are created, then the log
 * output function is chained with esp_log_set_vprintf(): every ESP_LOGx line still goes to the serial console
 * first (unchanged), then a bounded, redacted copy is put into the ring. The writer appends it to
 * /sdcard/FR7LOG/L<boot><seg>.LOG under the existing TF storage lock (tf_history.c), batched about every 5 s and
 * deferred while the largest internal DMA-capable block is below 4 KB. A task that holds the TF storage lock never
 * takes the buffer lock: its lines are printed but not captured (counted).
 *
 * Not captured: bootloader and early-boot output before the hook is installed, panic output, esp_rom_printf
 * (including Expert Debug FAILED_ALLOC lines; the writer adds a summary line when the failed-allocation
 * counter changes) and LVGL warnings (printf). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "log_capture_core.h"

void LogCapture_LoadAtBoot(void);
bool LogCapture_GetSaved(void);
esp_err_t LogCapture_SetSaved(bool enabled);
bool LogCapture_Active(void);

/* Before a deliberate restart: write what is buffered plus a clean end marker, waiting at most timeoutMs. */
void LogCapture_Shutdown(uint32_t timeoutMs, const char *reason);

typedef struct {
    bool saved, active;
    const char *inactiveReason; /* when !active: why (static text) */
    LogWriterState state;
    char reason[112];
    uint32_t boot, seg;
    char file[LOGCAP_NAME_LEN];
    uint32_t fileSize;
    uint64_t bytesWritten;
    uint32_t writes, writeErrors, closeErrors, lockTimeouts, filesCreated, filesDeleted, retentionFailures,
        interruptedMarked, spaceSuspends;
    char lastError[112];
    uint32_t lastErrorUptimeS;
    bool haveFreeBytes;
    uint64_t lastFreeBytes;
    uint32_t ringSize, ringUsed, ringHighWater;
    LogCapCounters ctr;
    uint32_t writerStackMinBytes; /* measured by the writer task itself; 0 = not yet */
    /* 0.1.8 DMA-pressure deferral (see LOGCAP_DMA_MIN_BYTES) */
    uint32_t deferredCycles, deferEpisodes, deferStartS, longestDeferS, lastDeferDmaBytes;
    bool deferring;
    bool writerRunning;
} LogCaptureStatus;
void LogCapture_GetStatus(LogCaptureStatus *out);

/* ---- /diag log management (httpd task). All take the TF storage lock briefly and release it before
 * returning; none holds it while the caller sends HTTP output. ---- */
typedef enum { LOGF_OK = 0, LOGF_BUSY, LOGF_NO_CARD, LOGF_BAD_NAME, LOGF_NOT_FOUND, LOGF_ACTIVE, LOGF_IN_USE, LOGF_IO } LogFileResult;
const char *LogFileResult_Text(LogFileResult r);
/* Up to max owned files (unsorted), *total = all owned files. */
LogFileResult LogCapture_ListFiles(LogFileInfo *out, size_t max, size_t *count, size_t *total);
/* The file being written right now ("" if none). */
void LogCapture_ActiveFile(char out[LOGCAP_NAME_LEN]);
/* Download: Begin pins the file (retention will not delete it) and reports its size at that moment;
 * Read copies up to cap bytes at offset (one short lock per call); End unpins. */
LogFileResult LogCapture_DownloadBegin(const char *name, uint32_t *size);
LogFileResult LogCapture_DownloadRead(const char *name, uint32_t offset, char *buf, size_t cap, size_t *got);
void LogCapture_DownloadEnd(void);
/* Delete one owned, inactive, unpinned file; or all of them. */
LogFileResult LogCapture_Delete(const char *name);
LogFileResult LogCapture_DeleteAll(uint32_t *deleted, uint32_t *skipped);
