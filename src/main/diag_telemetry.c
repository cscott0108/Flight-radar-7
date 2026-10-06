#include "diag_telemetry.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "time_util.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "adv_diag.h"
#include "expert_debug.h"
static const char *TAG = "DIAGT";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define DT_LOCK() portENTER_CRITICAL(&s_mux)
#define DT_UNLOCK() portEXIT_CRITICAL(&s_mux)
#else
#include <pthread.h>
static pthread_mutex_t s_mux = PTHREAD_MUTEX_INITIALIZER;
#define DT_LOCK() pthread_mutex_lock(&s_mux)
#define DT_UNLOCK() pthread_mutex_unlock(&s_mux)
#endif

#define DT_CLOCK_MARKER_SEC 300u
#define DT_ROUTINE_OK_SEC 300u

/* All state is fixed-size; every critical section is a short struct copy (no logging or I/O inside). */
static DiagMinHeap s_heap[DT_HEAP_COUNT];
static DiagMinStack s_stack[DT_MAX_STACKS];
static DiagOpStats s_op[DT_OP_COUNT];
static uint32_t s_routineLastSec[DT_OP_COUNT];
static bool s_routineSeen[DT_OP_COUNT];
static bool s_syncAnnounced = false;
static bool s_haveMarker = false;
static uint32_t s_lastMarkerSec = 0;
static DiagTelemetryHooks s_hooks;

static const char *const kOpName[DT_OP_COUNT] = {
    "Provider refresh", "OpenSky OAuth token", "Seen flush (SPIFFS)", "History flush (TF)"};

static uint32_t DefaultUpMs(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000);
#else
    return 0;
#endif
}

static int64_t DefaultUtc(void)
{
    int64_t now = (int64_t)time(NULL);
    return TimeUtil_IsSynced(now) ? now : 0;
}

static bool DefaultEventsOn(void)
{
#ifdef ESP_PLATFORM
    return AdvDiag_Active() || ExpertDebug_Active();
#else
    return false;
#endif
}

static void DefaultSink(const char *line)
{
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "%s", line);
#else
    (void)line;
#endif
}

static uint32_t UpMs(void) { return s_hooks.upMs ? s_hooks.upMs() : DefaultUpMs(); }
static DiagStamp NowStamp(void)
{
    DiagStamp s;
    s.uptimeSec = UpMs() / 1000u;
    s.utc = s_hooks.utc ? s_hooks.utc() : DefaultUtc();
    return s;
}

void DiagTelemetry_SetHooks(const DiagTelemetryHooks *h)
{
    if (h)
        s_hooks = *h;
    else
        memset(&s_hooks, 0, sizeof(s_hooks));
}

void DiagTelemetry_ResetForTest(void)
{
    DT_LOCK();
    memset(s_heap, 0, sizeof(s_heap));
    memset(s_stack, 0, sizeof(s_stack));
    memset(s_op, 0, sizeof(s_op));
    memset(s_routineLastSec, 0, sizeof(s_routineLastSec));
    memset(s_routineSeen, 0, sizeof(s_routineSeen));
    s_syncAnnounced = false;
    s_haveMarker = false;
    s_lastMarkerSec = 0;
    DT_UNLOCK();
}

uint32_t DiagTelemetry_NowMs(void) { return UpMs(); }

void DiagTelemetry_FormatStamp(const DiagStamp *s, char *out, size_t cap)
{
    if (!out || !cap)
        return;
    if (!s) {
        out[0] = 0;
        return;
    }
    if (s->utc > 0) {
        time_t t = (time_t)s->utc;
        struct tm tmv;
        if (gmtime_r(&t, &tmv)) {
            snprintf(out, cap, "T+%us, %04d-%02d-%02d %02d:%02d:%02d UTC", (unsigned)s->uptimeSec, tmv.tm_year + 1900,
                     tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            return;
        }
    }
    snprintf(out, cap, "T+%us (clock not synced)", (unsigned)s->uptimeSec);
}

/* ---- events ---- */

bool DiagTelemetry_EventsEnabled(void) { return s_hooks.eventsOn ? s_hooks.eventsOn() : DefaultEventsOn(); }

static void EmitLine(const char *msg)
{
    DiagStamp st = NowStamp();
    char stamp[64], line[220];
    DiagTelemetry_FormatStamp(&st, stamp, sizeof(stamp));
    snprintf(line, sizeof(line), "[%s] %s", stamp, msg);
    if (s_hooks.sink)
        s_hooks.sink(line);
    else
        DefaultSink(line);
}

static void VEvent(const char *fmt, va_list ap)
{
    char msg[160];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    EmitLine(msg);
}

void DiagTelemetry_Event(const char *fmt, ...)
{
    if (!DiagTelemetry_EventsEnabled())
        return; /* silent unless Advanced/Expert event logging is on */
    va_list ap;
    va_start(ap, fmt);
    VEvent(fmt, ap);
    va_end(ap);
}

void DiagTelemetry_EventRoutineOk(DiagOp op, const char *fmt, ...)
{
    if (!DiagTelemetry_EventsEnabled() || (unsigned)op >= DT_OP_COUNT)
        return;
    uint32_t now = UpMs() / 1000u;
    bool allow;
    DT_LOCK();
    allow = !s_routineSeen[op] || (uint32_t)(now - s_routineLastSec[op]) >= DT_ROUTINE_OK_SEC;
    if (allow) {
        s_routineSeen[op] = true;
        s_routineLastSec[op] = now;
    }
    DT_UNLOCK();
    if (!allow)
        return;
    va_list ap;
    va_start(ap, fmt);
    VEvent(fmt, ap);
    va_end(ap);
}

/* ---- recording ---- */

void DiagTelemetry_NoteHeap(DiagHeapKind kind, uint32_t minFreeNow)
{
    if ((unsigned)kind >= DT_HEAP_COUNT)
        return;
    DiagStamp st = NowStamp();
    DT_LOCK();
    DiagMinHeap *h = &s_heap[kind];
    if (!h->valid || minFreeNow < h->minFree) {
        h->valid = true;
        h->minFree = minFreeNow;
        h->when = st;
    }
    DT_UNLOCK();
}

void DiagTelemetry_NoteStack(const char *name, uint32_t minFreeBytesNow)
{
    if (!name || !name[0])
        return;
    DiagStamp st = NowStamp();
    DT_LOCK();
    DiagMinStack *slot = NULL;
    for (int i = 0; i < DT_MAX_STACKS; i++) {
        if (s_stack[i].valid && strncmp(s_stack[i].name, name, DT_NAME_MAX - 1) == 0) {
            slot = &s_stack[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < DT_MAX_STACKS; i++) {
            if (!s_stack[i].valid) {
                slot = &s_stack[i];
                slot->valid = true;
                snprintf(slot->name, sizeof(slot->name), "%s", name);
                slot->minFreeBytes = minFreeBytesNow;
                slot->when = st;
                break;
            }
        }
    } else if (minFreeBytesNow < slot->minFreeBytes) {
        slot->minFreeBytes = minFreeBytesNow;
        slot->when = st;
    }
    DT_UNLOCK(); /* table full: extra task names are ignored (bounded) */
}

void DiagTelemetry_OpDone(DiagOp op, DiagOpResult res, uint32_t durationMs, const char *reason)
{
    if ((unsigned)op >= DT_OP_COUNT)
        return;
    DiagStamp st = NowStamp();
    DT_LOCK();
    DiagOpStats *o = &s_op[op];
    if (res == DT_RES_SKIPPED) {
        o->skipped++;
    } else {
        o->runs++;
        o->lastMs = durationMs;
        o->lastRun = st;
        if (o->runs == 1 || durationMs > o->worstMs) { /* ties keep the first occurrence, like min-ever */
            o->worstMs = durationMs;
            o->worstWhen = st;
        }
        if (res == DT_RES_OK) {
            o->ok++;
            o->consecutiveFailures = 0;
        } else {
            o->failures++;
            o->consecutiveFailures++;
            o->haveFailure = true;
            o->lastFailureWhen = st;
            snprintf(o->lastFailure, sizeof(o->lastFailure), "%s", reason ? reason : "failed");
        }
    }
    DT_UNLOCK();
}

void DiagTelemetry_OpEnd(DiagOp op, uint32_t t0Ms, bool ok, const char *reasonIfFailed)
{
    uint32_t d = UpMs() - t0Ms;
    DiagTelemetry_OpDone(op, ok ? DT_RES_OK : DT_RES_FAIL, d, ok ? NULL : reasonIfFailed);
    if (!ok) {
        /* A failing recurring op must not flood the log: first, third, then every 10th consecutive failure. */
        uint32_t c = s_op[op].consecutiveFailures;
        if (c == 1 || c == 3 || c % 10 == 0)
            DiagTelemetry_Event("%s failed (%s) after %u ms, %u in a row", kOpName[op], reasonIfFailed ? reasonIfFailed : "failed",
                                (unsigned)d, (unsigned)c);
    }
}

/* ---- sampling ---- */

void DiagTelemetry_SampleHeaps(void)
{
#ifdef ESP_PLATFORM
    DiagTelemetry_NoteHeap(DT_HEAP_INTERNAL, (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DiagTelemetry_NoteHeap(DT_HEAP_DMA, (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    DiagTelemetry_NoteHeap(DT_HEAP_PSRAM, (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
}

void DiagTelemetry_SampleThisTaskStack(const char *name)
{
#ifdef ESP_PLATFORM
    DiagTelemetry_NoteStack(name, (uint32_t)uxTaskGetStackHighWaterMark(NULL)); /* IDF reports bytes */
#else
    (void)name;
#endif
}

void DiagTelemetry_Tick(void)
{
    DiagTelemetry_SampleHeaps();
    if (!DiagTelemetry_EventsEnabled())
        return;
    DiagStamp st = NowStamp();
    bool announce = false, marker = false;
    DT_LOCK();
    if (st.utc > 0 && !s_syncAnnounced) {
        s_syncAnnounced = true;
        announce = true;
    }
    if (!s_haveMarker || (uint32_t)(st.uptimeSec - s_lastMarkerSec) >= DT_CLOCK_MARKER_SEC) {
        s_haveMarker = true;
        s_lastMarkerSec = st.uptimeSec;
        marker = true;
    }
    DT_UNLOCK();
    if (announce)
        EmitLine("wall clock synchronized (SNTP); later events carry UTC");
    if (marker)
        EmitLine("clock marker");
}

/* ---- readers ---- */

bool DiagTelemetry_GetHeap(DiagHeapKind kind, DiagMinHeap *out)
{
    if ((unsigned)kind >= DT_HEAP_COUNT || !out)
        return false;
    DT_LOCK();
    *out = s_heap[kind];
    DT_UNLOCK();
    return out->valid;
}

size_t DiagTelemetry_GetStacks(DiagMinStack *out, size_t max)
{
    size_t n = 0;
    if (!out)
        return 0;
    DT_LOCK();
    for (int i = 0; i < DT_MAX_STACKS && n < max; i++)
        if (s_stack[i].valid)
            out[n++] = s_stack[i];
    DT_UNLOCK();
    return n;
}

void DiagTelemetry_GetOp(DiagOp op, DiagOpStats *out)
{
    if (!out)
        return;
    if ((unsigned)op >= DT_OP_COUNT) {
        memset(out, 0, sizeof(*out));
        return;
    }
    DT_LOCK();
    *out = s_op[op];
    DT_UNLOCK();
}

const char *DiagTelemetry_OpName(DiagOp op) { return (unsigned)op < DT_OP_COUNT ? kOpName[op] : "?"; }

bool DiagTelemetry_NeedsAttention(void)
{
    bool a = false;
    DT_LOCK();
    for (int i = 0; i < DT_OP_COUNT; i++)
        if (s_op[i].consecutiveFailures >= DT_ATTENTION_CONSECUTIVE)
            a = true;
    DT_UNLOCK();
    return a;
}
