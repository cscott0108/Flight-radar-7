// 0.1.8: persistent console-log capture, ESP-IDF glue. See log_capture.h and log_capture_core.h.
#include "log_capture.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "expert_debug.h"
#include "fw_version.h"
#include "tf_history.h"
#include "time_util.h"

static const char *TAG = "LOGCAP";

#define LC_NAMESPACE "diag"
#define LC_KEY "logcap"
#define LC_BOOT_KEY "logboot"
#define LC_TASK_STACK 4096
#define LC_TASK_PRIO 2
#define LC_WAKE_MS LOGCAP_WRITE_INTERVAL_MS /* 0.1.8: batch card writes (about every 5 s) */
#define LC_DRAIN_PER_WAKE 8

typedef struct {
    LogRing ring;
    LogWriter w;
    LogCapCounters ctr;
    LogCaptureStatus pub;         /* published by the writer after each wake (copied under s_pubMux) */
    char scratch[LOGCAP_LINE_MAX + 16];
    char batch[LOGCAP_WRITER_BATCH];
    uint8_t ringBuf[LOGCAP_RING_SIZE];
} LogCapState;

/* Internal .bss when capture is off: these few words only. */
static bool s_saved;
static bool s_active;
static const char *s_inactiveReason = "off (setting disabled)";
static LogCapState *s_st;              /* PSRAM, only when active */
static SemaphoreHandle_t s_fmtMutex;   /* serializes producers (the hook); never held by the writer */
static TaskHandle_t s_writer;
static vprintf_like_t s_prev = vprintf; /* valid before the hook is installed (no NULL window) */
static volatile bool s_hookOn;
static volatile bool s_notified;
static volatile bool s_shutdownReq, s_shutdownDone;
static const char *s_shutdownReason;
static char s_pinned[LOGCAP_NAME_LEN]; /* download in progress (changed under the storage lock) */
static portMUX_TYPE s_pubMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_stackMin;

/* ---------- settings ---------- */

bool LogCapture_GetSaved(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(LC_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, LC_KEY, &v);
        nvs_close(h);
    }
    return v != 0;
}

esp_err_t LogCapture_SetSaved(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(LC_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_u8(h, LC_KEY, enabled ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK)
        s_saved = enabled;
    return err;
}

bool LogCapture_Active(void) { return s_active; }

static uint32_t LoadBoot(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(LC_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u32(h, LC_BOOT_KEY, &v);
        nvs_close(h);
    }
    return v;
}

/* ---------- writer environment ---------- */

static bool EnvLock(void *ctx, uint32_t ms) { (void)ctx; return TfHistory_StorageLock(ms); }
static void EnvUnlock(void *ctx) { (void)ctx; TfHistory_StorageUnlock(); }
static bool EnvMounted(void *ctx) { (void)ctx; return TfHistory_IsMounted(); }
static bool EnvFree(void *ctx, uint64_t *out) { (void)ctx; return TfHistory_GetFreeBytes(out); }
/* The same heap the SPI driver's bounce buffers come from (spicommon_dma_setup_priv_buffer: MALLOC_CAP_DMA |
 * MALLOC_CAP_INTERNAL) and the same figure /diag shows as "DMA largest". */
static uint32_t EnvDma(void *ctx) { (void)ctx; return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL); }
static void EnvSaveBoot(void *ctx, uint32_t boot)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(LC_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u32(h, LC_BOOT_KEY, boot) == ESP_OK)
            (void)nvs_commit(h);
        nvs_close(h);
    }
}
static void EnvLocal(void *ctx, int64_t utc, char *out, size_t cap)
{
    (void)ctx;
    TimeLocal tl;
    if (utc > 0 && TimeUtil_ToLocal(utc, &tl))
        snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d %s", tl.year, tl.month, tl.day, tl.hour, tl.minute, tl.second, tl.abbr);
    else
        out[0] = 0;
}

static char s_dir[40];
static const char *ResetReasonText(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT: return "external pin";
    default: return "other";
    }
}
static LogWriterEnv s_env = {
    .lock = EnvLock, .unlock = EnvUnlock, .mounted = EnvMounted, .freeBytes = EnvFree,
    .saveBoot = EnvSaveBoot, .localTime = EnvLocal, .dmaLargest = EnvDma, .ctx = NULL, .dir = s_dir,
    .firmware = FW_VERSION_STRING, .resetReason = NULL,
};

/* ---------- the hook ---------- */

static int CaptureVprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    const int r = s_prev(fmt, args); /* serial console first, exactly as before */
    LogCapState *st = s_st;
    if (s_hookOn && st) {
        if (xPortInIsrContext() || !xPortCanYield() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
            __atomic_fetch_add(&st->ctr.dropContextLines, 1u, __ATOMIC_RELAXED); /* never lock here */
        } else if (xTaskGetCurrentTaskHandle() == s_writer) {
            __atomic_fetch_add(&st->ctr.ownLines, 1u, __ATOMIC_RELAXED); /* the writer's own lines: serial only (no recursion) */
        } else if (TfHistory_StorageLockHeldByMe()) {
            /* 0.1.8 strict lock rule: the buffer lock is never taken while this task holds the TF storage lock
             * (exact ownership from the mutex itself, see tf_history.h). Serial output above is unaffected. */
            __atomic_fetch_add(&st->ctr.dropLockLines, 1u, __ATOMIC_RELAXED);
        } else if (xSemaphoreTake(s_fmtMutex, 0) != pdTRUE) {
            /* another task is formatting, or this task re-entered (e.g. a log from inside capture): never wait */
            __atomic_fetch_add(&st->ctr.dropBusyLines, 1u, __ATOMIC_RELAXED);
        } else {
            bool truncated = false;
            uint32_t red = 0;
            const size_t n = LogCap_FormatRecord(st->scratch, sizeof(st->scratch), fmt, copy, &truncated, &red);
            if (n) {
                /* atomic: the writer copies these words while the hook updates them */
                if (LogRing_Put(&st->ring, st->scratch, (uint32_t)n)) {
                    __atomic_fetch_add(&st->ctr.lines, 1u, __ATOMIC_RELAXED);
                    __atomic_fetch_add(&st->ctr.bytes, (uint32_t)n, __ATOMIC_RELAXED);
                } else {
                    __atomic_fetch_add(&st->ctr.dropFullLines, 1u, __ATOMIC_RELAXED);
                    __atomic_fetch_add(&st->ctr.dropFullBytes, (uint32_t)n, __ATOMIC_RELAXED);
                }
                if (truncated)
                    __atomic_fetch_add(&st->ctr.truncated, 1u, __ATOMIC_RELAXED);
                __atomic_fetch_add(&st->ctr.redactions, red, __ATOMIC_RELAXED);
            }
            const bool wake = !s_notified && LogRing_Used(&st->ring) > st->ring.size / 2u;
            if (wake)
                s_notified = true;
            xSemaphoreGive(s_fmtMutex);
            if (wake && s_writer)
                xTaskNotifyGive(s_writer);
        }
    }
    va_end(copy);
    return r;
}

/* ---------- writer task ---------- */

static void Publish(LogCapState *st)
{
    LogCaptureStatus p;
    memset(&p, 0, sizeof(p));
    const LogWriter *w = &st->w;
    p.state = w->state;
    snprintf(p.reason, sizeof(p.reason), "%s", w->reason);
    p.boot = w->boot;
    p.seg = w->seg;
    if (w->haveFile)
        memcpy(p.file, w->file, sizeof(p.file));
    p.fileSize = (uint32_t)w->fileSize;
    p.bytesWritten = w->bytesWritten;
    p.writes = w->writes;
    p.writeErrors = w->writeErrors;
    p.closeErrors = w->closeErrors;
    p.lockTimeouts = w->lockTimeouts;
    p.filesCreated = w->filesCreated;
    p.filesDeleted = w->filesDeleted;
    p.retentionFailures = w->retentionFailures;
    p.interruptedMarked = w->interruptedMarked;
    p.spaceSuspends = w->spaceSuspends;
    snprintf(p.lastError, sizeof(p.lastError), "%s", w->lastError);
    p.lastErrorUptimeS = w->lastErrorUptimeS;
    p.haveFreeBytes = w->haveFreeBytes;
    p.lastFreeBytes = w->lastFreeBytes;
    p.writerStackMinBytes = s_stackMin;
    p.deferredCycles = w->deferredCycles;
    p.deferEpisodes = w->deferEpisodes;
    p.deferring = w->deferring;
    p.deferStartS = w->deferStartS;
    p.longestDeferS = w->longestDeferS;
    p.lastDeferDmaBytes = w->lastDeferDmaBytes;
    p.writerRunning = true;
    taskENTER_CRITICAL(&s_pubMux);
    st->pub = p;
    taskEXIT_CRITICAL(&s_pubMux);
}

static void FillFailInfo(LogCapCounters *c)
{
    c->failedAllocs = ExpertDebug_FailedAllocCount();
    if (c->failedAllocs) {
        c->failSize = ExpertDebug_LastFailedSize();
        c->failCaps = ExpertDebug_LastFailedCaps();
        snprintf(c->failTask, sizeof(c->failTask), "%s", ExpertDebug_LastFailedTask());
        int64_t us = 0;
        c->failTimeValid = ExpertDebug_LastFailedUptimeUs(&us);
        c->failUptimeS = (uint32_t)(us / 1000000);
    }
}

static void WriterTask(void *arg)
{
    LogCapState *st = (LogCapState *)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LC_WAKE_MS));
        s_notified = false;
        const bool shut = s_shutdownReq;
        LogCapCounters c = st->ctr; /* plain word copies; the hook only increments */
        FillFailInfo(&c);
        const bool wasDeferring = st->w.deferring;
        for (int i = 0; i < (shut ? 64 : LC_DRAIN_PER_WAKE); i++) {
            const uint32_t nowS = (uint32_t)(esp_timer_get_time() / 1000000);
            const int64_t t = (int64_t)time(NULL);
            const int64_t utc = TimeUtil_IsSynced(t) ? t : 0;
            if (!LogWriter_Cycle(&st->w, &s_env, &st->ring, st->batch, nowS, utc, &c, shut, s_shutdownReason))
                break;
            if (!shut && LogRing_Used(&st->ring) == 0)
                break;
            taskYIELD();
        }
        /* one serial line per deferral episode start and end (the writer's own lines are never captured; the
         * file gets a WRITE DEFERRED marker with the next successful write) */
        if (st->w.deferring && !wasDeferring)
            ESP_LOGW(TAG, "card writes deferred: largest internal DMA block %u B < %u B (lines stay buffered)",
                     (unsigned)st->w.lastDeferDmaBytes, (unsigned)LOGCAP_DMA_MIN_BYTES);
        else if (!st->w.deferring && wasDeferring)
            ESP_LOGI(TAG, "card writes resumed after a DMA-memory deferral");
        s_stackMin = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        Publish(st);
        if (shut)
            s_shutdownDone = true;
    }
}

/* ---------- boot ---------- */

void LogCapture_LoadAtBoot(void)
{
    s_saved = LogCapture_GetSaved();
    if (!s_saved) {
        s_inactiveReason = "off (setting disabled; takes effect at boot)";
        ESP_LOGI(TAG, "Console log capture: OFF");
        return;
    }
    snprintf(s_dir, sizeof(s_dir), "%s/" LOGCAP_DIR_NAME, TfHistory_MountPoint());
    s_env.resetReason = ResetReasonText();

    LogCapState *st = heap_caps_calloc(1, sizeof(LogCapState), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!st) {
        s_inactiveReason = "unavailable: could not allocate the PSRAM capture buffer";
        ESP_LOGE(TAG, "Console log capture unavailable: no PSRAM for the %u-byte buffer", (unsigned)sizeof(LogCapState));
        return;
    }
    s_fmtMutex = xSemaphoreCreateMutex();
    if (!s_fmtMutex) {
        heap_caps_free(st);
        s_inactiveReason = "unavailable: could not create the capture mutex";
        ESP_LOGE(TAG, "Console log capture unavailable: no memory for its mutex");
        return;
    }
    LogRing_Init(&st->ring, st->ringBuf, LOGCAP_RING_SIZE);
    LogWriter_Init(&st->w, LogCap_NextBoot(LoadBoot()));
    st->w.pinned = s_pinned;
    s_st = st;
    /* INTERNAL stack on purpose: the SD-over-SPI driver passes stack buffers to SPI DMA transfers; from a PSRAM
     * stack every such transfer would need a new internal DMA bounce allocation (spi_master.c). */
    if (xTaskCreatePinnedToCore(WriterTask, "LogWr", LC_TASK_STACK, st, LC_TASK_PRIO, &s_writer, tskNO_AFFINITY) != pdPASS) {
        s_st = NULL;
        vSemaphoreDelete(s_fmtMutex);
        s_fmtMutex = NULL;
        heap_caps_free(st);
        s_inactiveReason = "unavailable: could not create the writer task (internal RAM)";
        ESP_LOGE(TAG, "Console log capture unavailable: writer task not created");
        return;
    }
    const vprintf_like_t prev = esp_log_set_vprintf(CaptureVprintf);
    if (prev && prev != CaptureVprintf)
        s_prev = prev;
    s_hookOn = true;
    s_active = true;
    ESP_LOGI(TAG, "Console log capture: ON (boot %u, %u-byte PSRAM buffer, writer stack %u B internal) -> %s",
             (unsigned)st->w.boot, (unsigned)LOGCAP_RING_SIZE, (unsigned)LC_TASK_STACK, s_dir);
}

void LogCapture_Shutdown(uint32_t timeoutMs, const char *reason)
{
    if (!s_active || !s_writer)
        return;
    s_shutdownReason = reason ? reason : "restart";
    s_shutdownDone = false;
    s_shutdownReq = true;
    xTaskNotifyGive(s_writer);
    for (uint32_t waited = 0; !s_shutdownDone && waited < timeoutMs; waited += 20)
        vTaskDelay(pdMS_TO_TICKS(20));
}

void LogCapture_GetStatus(LogCaptureStatus *out)
{
    memset(out, 0, sizeof(*out));
    LogCapState *st = s_st;
    if (st) {
        taskENTER_CRITICAL(&s_pubMux);
        *out = st->pub;
        taskEXIT_CRITICAL(&s_pubMux);
        out->ctr = st->ctr;
        out->ringSize = st->ring.size;
        out->ringUsed = LogRing_Used(&st->ring);
        out->ringHighWater = st->ring.highWater;
        if (!out->writerRunning) {
            out->state = LW_NOT_STARTED;
            snprintf(out->reason, sizeof(out->reason), "writer has not run yet");
        }
    }
    out->saved = s_saved;
    out->active = s_active;
    out->inactiveReason = s_active ? "" : s_inactiveReason;
}

/* ---------- /diag log management ---------- */

const char *LogFileResult_Text(LogFileResult r)
{
    switch (r) {
    case LOGF_OK: return "OK";
    case LOGF_BUSY: return "the TF card is busy (try again)";
    case LOGF_NO_CARD: return "the TF card is not mounted";
    case LOGF_BAD_NAME: return "not a console-log file name";
    case LOGF_NOT_FOUND: return "no such log file";
    case LOGF_ACTIVE: return "this file is being written right now";
    case LOGF_IN_USE: return "this file is being downloaded";
    case LOGF_IO: return "card read/write error";
    default: return "?";
    }
}

static const char *LogDir(void)
{
    if (!s_dir[0])
        snprintf(s_dir, sizeof(s_dir), "%s/" LOGCAP_DIR_NAME, TfHistory_MountPoint());
    return s_dir;
}

void LogCapture_ActiveFile(char out[LOGCAP_NAME_LEN])
{
    out[0] = 0;
    LogCapState *st = s_st;
    if (st) {
        taskENTER_CRITICAL(&s_pubMux);
        memcpy(out, st->pub.file, LOGCAP_NAME_LEN);
        taskEXIT_CRITICAL(&s_pubMux);
    }
}

/* Active = the writer's current file (read under the storage lock, so it cannot change underneath). */
static bool IsActiveLocked(const char *name)
{
    LogCapState *st = s_st;
    return st && st->w.haveFile && strcmp(st->w.file, name) == 0 && st->w.state != LW_ENDED;
}

#define LC_HTTP_LOCK_MS 1000u

LogFileResult LogCapture_ListFiles(LogFileInfo *out, size_t max, size_t *count, size_t *total)
{
    *count = 0;
    *total = 0;
    if (!TfHistory_IsMounted())
        return LOGF_NO_CARD;
    if (!TfHistory_StorageLock(LC_HTTP_LOCK_MS))
        return LOGF_BUSY;
    *count = LogCap_List(LogDir(), out, max, total);
    TfHistory_StorageUnlock();
    return LOGF_OK;
}

LogFileResult LogCapture_DownloadBegin(const char *name, uint32_t *size)
{
    if (!LogCap_ValidName(name))
        return LOGF_BAD_NAME;
    if (!TfHistory_IsMounted())
        return LOGF_NO_CARD;
    if (!TfHistory_StorageLock(LC_HTTP_LOCK_MS))
        return LOGF_BUSY;
    char path[64];
    snprintf(path, sizeof(path), "%.40s/%.12s", LogDir(), name);
    FILE *f = fopen(path, "rb");
    LogFileResult r = LOGF_NOT_FOUND;
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            *size = sz > 0 ? (uint32_t)sz : 0;
            memcpy(s_pinned, name, LOGCAP_NAME_LEN);
            r = LOGF_OK;
        } else {
            r = LOGF_IO;
        }
        fclose(f);
    }
    TfHistory_StorageUnlock();
    return r;
}

LogFileResult LogCapture_DownloadRead(const char *name, uint32_t offset, char *buf, size_t cap, size_t *got)
{
    *got = 0;
    if (!LogCap_ValidName(name))
        return LOGF_BAD_NAME;
    if (!TfHistory_IsMounted())
        return LOGF_NO_CARD;
    if (!TfHistory_StorageLock(LC_HTTP_LOCK_MS))
        return LOGF_BUSY;
    char path[64];
    snprintf(path, sizeof(path), "%.40s/%.12s", LogDir(), name);
    LogFileResult r = LOGF_NOT_FOUND;
    FILE *f = fopen(path, "rb");
    if (f) {
        r = LOGF_IO;
        if (fseek(f, (long)offset, SEEK_SET) == 0) {
            *got = fread(buf, 1, cap, f);
            r = (*got > 0 || !ferror(f)) ? LOGF_OK : LOGF_IO;
        }
        fclose(f);
    }
    TfHistory_StorageUnlock();
    return r;
}

void LogCapture_DownloadEnd(void)
{
    if (TfHistory_StorageLock(UINT32_MAX)) {
        s_pinned[0] = 0;
        TfHistory_StorageUnlock();
    }
}

static LogFileResult DeleteLocked(const char *name)
{
    if (IsActiveLocked(name))
        return LOGF_ACTIVE;
    if (s_pinned[0] && strcmp(s_pinned, name) == 0)
        return LOGF_IN_USE;
    char path[64];
    snprintf(path, sizeof(path), "%.40s/%.12s", LogDir(), name);
    struct stat st;
    if (stat(path, &st) != 0)
        return LOGF_NOT_FOUND;
    return remove(path) == 0 ? LOGF_OK : LOGF_IO;
}

LogFileResult LogCapture_Delete(const char *name)
{
    if (!LogCap_ValidName(name))
        return LOGF_BAD_NAME;
    if (!TfHistory_IsMounted())
        return LOGF_NO_CARD;
    if (!TfHistory_StorageLock(LC_HTTP_LOCK_MS))
        return LOGF_BUSY;
    const LogFileResult r = DeleteLocked(name);
    TfHistory_StorageUnlock();
    return r;
}

LogFileResult LogCapture_DeleteAll(uint32_t *deleted, uint32_t *skipped)
{
    *deleted = *skipped = 0;
    if (!TfHistory_IsMounted())
        return LOGF_NO_CARD;
    if (!TfHistory_StorageLock(LC_HTTP_LOCK_MS))
        return LOGF_BUSY;
    LogFileResult worst = LOGF_OK;
    for (int pass = 0; pass < 8; pass++) { /* names collected 16 at a time (bounded stack) */
        LogFileInfo files[16];
        size_t total = 0;
        const size_t n = LogCap_List(LogDir(), files, 16, &total);
        uint32_t removedThisPass = 0;
        for (size_t i = 0; i < n; i++) {
            const LogFileResult r = DeleteLocked(files[i].name);
            if (r == LOGF_OK) {
                (*deleted)++;
                removedThisPass++;
            } else if (r == LOGF_IO) {
                worst = LOGF_IO;
            }
        }
        if (!removedThisPass)
            break;
    }
    size_t left = 0;
    LogFileInfo one;
    (void)LogCap_List(LogDir(), &one, 1, &left);
    *skipped = (uint32_t)left;
    TfHistory_StorageUnlock();
    return worst;
}
