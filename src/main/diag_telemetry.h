#pragma once

/* Diagnostics telemetry (step 7): small, fixed-size, RAM-only records that answer
 * "when did it get that low / how often does it fail / how long does it take".
 * No heap allocation, no persistence, no background task: producers call in from
 * the tasks that already run; /diag reads snapshots.
 *
 *  - min-ever heap (internal / DMA-capable / PSRAM) and up to DT_MAX_STACKS task
 *    stack high-water marks, each stamped when a LOWER value was first observed
 *    (the stamp is as precise as the sampling cadence, normally one poll slice);
 *  - four recurring operations with run/failure/skip counters, last and worst
 *    duration and the last failure retained;
 *  - stamped events, silent unless event logging is enabled (Advanced or Expert
 *    diagnostics active). Never put SSIDs, passwords or tokens in event text. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DT_MAX_STACKS 6
#define DT_NAME_MAX 16
#define DT_REASON_MAX 48

/* uptimeSec: seconds since boot. utc: 0 means "wall clock was not synced yet". */
typedef struct {
    uint32_t uptimeSec;
    int64_t utc;
} DiagStamp;

typedef enum {
    DT_HEAP_INTERNAL = 0,
    DT_HEAP_DMA,
    DT_HEAP_PSRAM,
    DT_HEAP_COUNT
} DiagHeapKind;

typedef struct {
    bool valid;
    uint32_t minFree; /* lowest free bytes observed by sampling */
    DiagStamp when;   /* when that value was first observed */
} DiagMinHeap;

typedef struct {
    bool valid;
    char name[DT_NAME_MAX];
    uint32_t minFreeBytes;
    DiagStamp when;
} DiagMinStack;

typedef enum {
    DT_OP_PROVIDER_REFRESH = 0,
    DT_OP_OPENSKY_TOKEN,
    DT_OP_SEEN_FLUSH,
    DT_OP_TF_FLUSH,
    DT_OP_COUNT
} DiagOp;

typedef enum { DT_RES_OK = 0, DT_RES_FAIL, DT_RES_SKIPPED } DiagOpResult;

typedef struct {
    uint32_t runs;     /* completed attempts (ok + failed); skipped are separate */
    uint32_t ok;
    uint32_t failures;
    uint32_t skipped;
    uint32_t consecutiveFailures;
    uint32_t lastMs;
    uint32_t worstMs;
    DiagStamp worstWhen;
    DiagStamp lastRun;
    bool haveFailure;
    DiagStamp lastFailureWhen;
    char lastFailure[DT_REASON_MAX]; /* fixed short text from the call site, never user data */
} DiagOpStats;

/* ---- core recording (pure: values are passed in, so tests need no hardware) ---- */
void DiagTelemetry_NoteHeap(DiagHeapKind kind, uint32_t minFreeNow);
void DiagTelemetry_NoteStack(const char *name, uint32_t minFreeBytesNow);
void DiagTelemetry_OpDone(DiagOp op, DiagOpResult res, uint32_t durationMs, const char *reason);

/* Convenience for call sites: t0 = DiagTelemetry_NowMs() before, then OpEnd. */
uint32_t DiagTelemetry_NowMs(void);
void DiagTelemetry_OpEnd(DiagOp op, uint32_t t0Ms, bool ok, const char *reasonIfFailed);

/* ---- sampling (firmware: reads the heap allocator; host: no-op) ---- */
void DiagTelemetry_SampleHeaps(void);
/* Call from inside the task whose stack is measured (uxTaskGetStackHighWaterMark(NULL)). */
void DiagTelemetry_SampleThisTaskStack(const char *name);

/* Call from the poll loop each slice: samples heaps, emits the 5-minute clock
 * marker and the one-time "wall clock synchronized" announcement (events only). */
void DiagTelemetry_Tick(void);

/* ---- events ---- */
bool DiagTelemetry_EventsEnabled(void);
/* Stamped line, e.g. "[T+812s 2026-10-05 14:03:22Z] provider refresh failed: HTTP 429". */
void DiagTelemetry_Event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Routine success lines: at most one per op per 5 minutes. */
void DiagTelemetry_EventRoutineOk(DiagOp op, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- readers for /diag (consistent copies) ---- */
bool DiagTelemetry_GetHeap(DiagHeapKind kind, DiagMinHeap *out);
size_t DiagTelemetry_GetStacks(DiagMinStack *out, size_t max);
void DiagTelemetry_GetOp(DiagOp op, DiagOpStats *out);
const char *DiagTelemetry_OpName(DiagOp op);
/* An operation needs attention after this many failures in a row. */
#define DT_ATTENTION_CONSECUTIVE 3
bool DiagTelemetry_NeedsAttention(void);
/* "T+812s" or "T+812s, 2026-10-05 14:03:22 UTC" (clock was synced) into out. */
void DiagTelemetry_FormatStamp(const DiagStamp *s, char *out, size_t cap);

/* ---- host-test hooks ---- */
typedef struct {
    uint32_t (*upMs)(void);
    int64_t (*utc)(void);
    bool (*eventsOn)(void);
    void (*sink)(const char *line);
} DiagTelemetryHooks;
void DiagTelemetry_SetHooks(const DiagTelemetryHooks *h); /* NULL restores defaults */
void DiagTelemetry_ResetForTest(void);
