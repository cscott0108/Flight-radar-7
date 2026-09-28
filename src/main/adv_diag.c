// Advanced Diagnostics - see adv_diag.h for the tiers and the reboot rule.
//
// These are the heap/allocation/TLS tools developed while tracing the
// OpenSky aircraft-TLS "esp-aes: Failed to allocate memory" failure (lazy
// one-time ESP-IDF allocations landing mid-way through the last large
// DMA-capable block). They are kept for future troubleshooting, but stay
// dormant unless the persistent /diag setting was ON at boot.

#include "adv_diag.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

// PCBTRACE reads lwIP's PCB lists. This is a private lwIP header; it is used
// only by this dormant diagnostic, never by the normal firmware path.
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcpip.h"

static const char *TAG = "ADVDIAG";

#define ADVDIAG_NAMESPACE "diag"
#define ADVDIAG_KEY "adv"

static bool s_active; // fixed for the whole boot

bool AdvDiag_Active(void)
{
    return s_active;
}

bool AdvDiag_GetSaved(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(ADVDIAG_NAMESPACE, NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_u8(h, ADVDIAG_KEY, &v);
        nvs_close(h);
    }
    return v != 0;
}

esp_err_t AdvDiag_SetSaved(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ADVDIAG_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_u8(h, ADVDIAG_KEY, enabled ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool AdvDiag_AllocTraceCompiled(void)
{
#if CONFIG_FR_ALLOC_TRACE
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Heap checkpoints (ring kept in PSRAM, allocated only when active)
// ---------------------------------------------------------------------------

#define CP_MAX 16
static AdvDiagCheckpoint *s_cp;
static size_t s_cpNext, s_cpCount;
static portMUX_TYPE s_cpMux = portMUX_INITIALIZER_UNLOCKED;

void AdvDiag_HeapCheckpoint(const char *stage)
{
    if (!s_active)
        return;
    AdvDiagCheckpoint c = {
        .stage = stage,
        .uptimeMs = (uint32_t)(esp_timer_get_time() / 1000),
        .internalFree = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .internalLargest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .dmaFree = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
        .dmaLargest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
        .psramFree = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    ESP_LOGW(TAG, "%s: internal free=%u largest=%u, DMA free=%u largest=%u, PSRAM free=%u",
             stage, (unsigned)c.internalFree, (unsigned)c.internalLargest,
             (unsigned)c.dmaFree, (unsigned)c.dmaLargest, (unsigned)c.psramFree);
    if (!s_cp)
        return;
    portENTER_CRITICAL(&s_cpMux);
    s_cp[s_cpNext] = c;
    s_cpNext = (s_cpNext + 1) % CP_MAX;
    if (s_cpCount < CP_MAX)
        s_cpCount++;
    portEXIT_CRITICAL(&s_cpMux);
}

size_t AdvDiag_GetCheckpoints(AdvDiagCheckpoint *out, size_t max)
{
    if (!s_active || !s_cp || !out)
        return 0;
    size_t n = 0;
    portENTER_CRITICAL(&s_cpMux);
    size_t start = (s_cpNext + CP_MAX - s_cpCount) % CP_MAX;
    for (size_t i = 0; i < s_cpCount && n < max; i++)
        out[n++] = s_cp[(start + i) % CP_MAX];
    portEXIT_CRITICAL(&s_cpMux);
    return n;
}

// ---------------------------------------------------------------------------
// Free-block listing for /diag
// ---------------------------------------------------------------------------

typedef struct
{
    AdvDiagFreeBlock *out;
    size_t max, n, minSize;
} FreeWalkCtx;

static bool FreeWalkCb(walker_heap_into_t h, walker_block_info_t b, void *u)
{
    (void)h;
    FreeWalkCtx *ctx = (FreeWalkCtx *)u;
    if (!b.used && b.size >= ctx->minSize && ctx->n < ctx->max)
        ctx->out[ctx->n++] = (AdvDiagFreeBlock){(uintptr_t)b.ptr, (uint32_t)b.size};
    return true;
}

size_t AdvDiag_GetDmaFreeBlocks(AdvDiagFreeBlock *out, size_t max, size_t minSize)
{
    if (!s_active || !out || !max)
        return 0;
    FreeWalkCtx ctx = {.out = out, .max = max, .n = 0, .minSize = minSize};
    heap_caps_walk(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL, FreeWalkCb, &ctx);
    return ctx.n;
}

// ---------------------------------------------------------------------------
// HDIAG: DMA-capable internal heap block diff (tables in PSRAM)
// ---------------------------------------------------------------------------

#define HDIAG_MAX 400
typedef struct
{
    void *ptr;
    size_t size;
} HDiagBlk;
static HDiagBlk *hdiagBase, *hdiagCur, *hdiagFree;
static int hdiagBaseN = -1;
static int hdiagCurN, hdiagFreeN;

static bool HDiagWalk(walker_heap_into_t h, walker_block_info_t b, void *u)
{
    (void)h;
    (void)u;
    if (b.used)
    {
        if (hdiagCurN < HDIAG_MAX)
            hdiagCur[hdiagCurN++] = (HDiagBlk){b.ptr, b.size};
    }
    else if (b.size >= 1024 && hdiagFreeN < HDIAG_MAX)
        hdiagFree[hdiagFreeN++] = (HDiagBlk){b.ptr, b.size};
    return true;
}

static void HDiagSnapshot(void)
{
    hdiagCurN = 0;
    hdiagFreeN = 0;
    heap_caps_walk(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL, HDiagWalk, NULL);
}

void AdvDiag_HeapBaseline(void)
{
    if (!s_active || hdiagBaseN >= 0 || !hdiagBase)
        return;
    HDiagSnapshot();
    memcpy(hdiagBase, hdiagCur, sizeof(HDiagBlk) * hdiagCurN);
    hdiagBaseN = hdiagCurN;
    ESP_LOGW("HDIAG", "baseline: %d used DMA-internal blocks", hdiagBaseN);
    for (int i = 0; i < hdiagFreeN; i++)
        ESP_LOGW("HDIAG", "  baseline free %p size=%u", hdiagFree[i].ptr, (unsigned)hdiagFree[i].size);
}

void AdvDiag_HeapDiff(const char *stage)
{
    if (!s_active || hdiagBaseN < 0)
        return;
    HDiagSnapshot();
    ESP_LOGW("HDIAG", "%s: new used blocks vs baseline (tcp_pcb size=%u):", stage, (unsigned)sizeof(struct tcp_pcb));
    for (int i = 0; i < hdiagCurN; i++)
    {
        bool found = false;
        for (int j = 0; j < hdiagBaseN; j++)
            if (hdiagBase[j].ptr == hdiagCur[i].ptr && hdiagBase[j].size == hdiagCur[i].size)
            {
                found = true;
                break;
            }
        if (!found)
            ESP_LOGW("HDIAG", "  NEW %p size=%u", hdiagCur[i].ptr, (unsigned)hdiagCur[i].size);
    }
    for (int i = 0; i < hdiagFreeN; i++)
        ESP_LOGW("HDIAG", "  free %p size=%u", hdiagFree[i].ptr, (unsigned)hdiagFree[i].size);
    // Racy read of lwIP lists from this task: acceptable for a log-only
    // diagnostic (core locking is off; PCBTRACE below uses tcpip_callback).
    for (struct tcp_pcb *p = tcp_tw_pcbs; p; p = p->next)
        ESP_LOGW("HDIAG", "  TIME_WAIT pcb %p", (void *)p);
    for (struct tcp_pcb *p = tcp_active_pcbs; p; p = p->next)
        ESP_LOGW("HDIAG", "  active pcb %p state=%d", (void *)p, (int)p->state);
}

// ---------------------------------------------------------------------------
// PCBTRACE: closing-socket lifetime after the first OAuth cleanup
// ---------------------------------------------------------------------------

static esp_timer_handle_t pcbTraceTimer;
static struct tcp_pcb *pcbTracePtr;
static u16_t pcbTracePort;
static int64_t pcbTraceT0;
static int pcbTraceStep;
static bool pcbTraceStarted;
static const uint32_t pcbTraceMs[] = {0, 1000, 5000, 20000, 60000, 120000};

static void PcbTraceInspect(void *arg)
{
    (void)arg;
    int state = -1;
    const char *list = "gone";
    for (struct tcp_pcb *p = tcp_active_pcbs; p; p = p->next)
        if (p == pcbTracePtr && p->local_port == pcbTracePort)
        {
            state = p->state;
            list = "active";
            break;
        }
    if (state < 0)
        for (struct tcp_pcb *p = tcp_tw_pcbs; p; p = p->next)
            if (p == pcbTracePtr && p->local_port == pcbTracePort)
            {
                state = p->state;
                list = "tw";
                break;
            }
    ESP_LOGW("PCBTRACE", "t=%lld ms pcb=%p port=%u list=%s state=%d | internal free=%u largest=%u DMA free=%u largest=%u",
             (long long)((esp_timer_get_time() - pcbTraceT0) / 1000), (void *)pcbTracePtr, (unsigned)pcbTracePort,
             list, state,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (state < 0)
    {
        ESP_LOGW("PCBTRACE", "closing PCB gone; trace stopped");
        return;
    }
    pcbTraceStep++;
    if (pcbTraceStep < (int)(sizeof(pcbTraceMs) / sizeof(pcbTraceMs[0])))
    {
        int64_t due = pcbTraceT0 + (int64_t)pcbTraceMs[pcbTraceStep] * 1000;
        int64_t wait = due - esp_timer_get_time();
        esp_timer_start_once(pcbTraceTimer, wait > 0 ? (uint64_t)wait : 1);
    }
    else
        ESP_LOGW("PCBTRACE", "closing PCB still present at 120 s; trace stopped");
}

static void PcbTraceTimerCb(void *arg)
{
    (void)arg;
    if (tcpip_callback(PcbTraceInspect, NULL) != ERR_OK)
        ESP_LOGW("PCBTRACE", "tcpip_callback failed");
}

static void PcbTraceFind(void *arg)
{
    (void)arg;
    for (struct tcp_pcb *p = tcp_active_pcbs; p; p = p->next)
        if (TCP_STATE_IS_CLOSING(p->state))
        {
            pcbTracePtr = p;
            pcbTracePort = p->local_port;
            break;
        }
    if (!pcbTracePtr)
        for (struct tcp_pcb *p = tcp_tw_pcbs; p; p = p->next)
        {
            pcbTracePtr = p;
            pcbTracePort = p->local_port;
            break;
        }
    if (!pcbTracePtr)
    {
        ESP_LOGW("PCBTRACE", "no closing PCB found at cleanup");
        return;
    }
    PcbTraceInspect(NULL);
}

void AdvDiag_PcbTraceStart(void)
{
    if (!s_active || !pcbTraceTimer || pcbTraceStarted)
        return;
    pcbTraceStarted = true;
    pcbTraceT0 = esp_timer_get_time();
    pcbTraceStep = 0;
    tcpip_callback(PcbTraceFind, NULL);
}

// ---------------------------------------------------------------------------
// Allocation-owner trace (compile-time: CONFIG_FR_ALLOC_TRACE)
//
// Two users of the same recorder:
//  - Advanced Diagnostics windows (crypto warm-up, net warm-up, OAuth):
//    AdvDiag_AllocTraceBegin/End.
//  - Expert Debug aircraft-TLS accounting window: AdvDiag_AircraftTrace*.
//    Armed at "Before aircraft TLS" (until one failed attempt has been
//    captured), dumped 1 s later (the stalled state) and again when the
//    request returns, before the HTTP client is cleaned up.
// Only one window is armed at a time.
// ---------------------------------------------------------------------------

#if CONFIG_FR_ALLOC_TRACE
// Wrappers installed by linker --wrap (main/CMakeLists.txt, only when this
// option is on). They are IRAM and skip recording from ISRs or while the
// flash cache is disabled. No allocation is redirected or altered.
#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_memory_utils.h"
#include "esp_private/cache_utils.h"
#include "expert_debug.h"

#define AOT_MAX 600
#define AOT_DEPTH 6
#define AOT_CAPS_MALLOC 0xFFFFFFFFu // plain malloc/calloc/realloc (internal-first below ALWAYSINTERNAL)

typedef enum
{
    AOT_REG_DMA = 0, // internal, DMA-capable DRAM
    AOT_REG_INT,     // internal, not DMA-capable (RTC FAST on this board)
    AOT_REG_PSRAM,
    AOT_REG_OTHER,
    AOT_REG_COUNT
} AotRegion;

static const char *const kRegionName[AOT_REG_COUNT] = {"DMA-DRAM", "INT-nonDMA", "PSRAM", "other"};

typedef struct
{
    void *ptr;
    uint32_t size;
    uint32_t caps;
    uint32_t seq;
    uint8_t region;
    uint32_t pc[AOT_DEPTH];
} AotRec;

// Window statistics (all bytes are requested sizes).
typedef struct
{
    uint32_t allocs, frees, dropped, untrackedFrees;
    uint32_t allocBytes[AOT_REG_COUNT];
    uint32_t freedBytes[AOT_REG_COUNT];
    int32_t liveBytes[AOT_REG_COUNT];
    int32_t peakLiveDma;
} AotStats;

static AotRec *aotRecs;
static volatile bool aotArmed;
static int aotN;
static uint32_t aotSeq;
static AotStats aotStats;
static portMUX_TYPE aotMux = portMUX_INITIALIZER_UNLOCKED;

static IRAM_ATTR uint8_t AotRegionOf(const void *p)
{
    if (esp_ptr_external_ram(p))
        return AOT_REG_PSRAM;
    if (esp_ptr_dma_capable(p))
        return AOT_REG_DMA;
    if (esp_ptr_internal(p))
        return AOT_REG_INT;
    return AOT_REG_OTHER;
}

static IRAM_ATTR void AotAdd(void *ptr, size_t size, uint32_t caps)
{
    if (!ptr || !aotArmed || !aotRecs || xPortInIsrContext() || !spi_flash_cache_enabled())
        return;
    AotRec rec = {.ptr = ptr, .size = (uint32_t)size, .caps = caps, .region = AotRegionOf(ptr)};
    esp_backtrace_frame_t fr;
    esp_backtrace_get_start(&fr.pc, &fr.sp, &fr.next_pc);
    esp_backtrace_get_next_frame(&fr); // skip AotAdd
    for (int i = 0; i < AOT_DEPTH; i++)
    {
        if (!esp_backtrace_get_next_frame(&fr))
            break;
        rec.pc[i] = esp_cpu_process_stack_pc(fr.pc);
    }
    portENTER_CRITICAL_SAFE(&aotMux);
    rec.seq = ++aotSeq;
    aotStats.allocs++;
    aotStats.allocBytes[rec.region] += rec.size;
    aotStats.liveBytes[rec.region] += (int32_t)rec.size;
    if (aotStats.liveBytes[AOT_REG_DMA] > aotStats.peakLiveDma)
        aotStats.peakLiveDma = aotStats.liveBytes[AOT_REG_DMA];
    if (aotN < AOT_MAX)
        aotRecs[aotN++] = rec;
    else
        aotStats.dropped++;
    portEXIT_CRITICAL_SAFE(&aotMux);
}

static IRAM_ATTR void AotDel(void *ptr)
{
    if (!ptr || !aotArmed || !aotRecs || !spi_flash_cache_enabled())
        return;
    portENTER_CRITICAL_SAFE(&aotMux);
    bool found = false;
    for (int i = aotN - 1; i >= 0; i--)
        if (aotRecs[i].ptr == ptr)
        {
            AotRec *r = &aotRecs[i];
            aotStats.frees++;
            aotStats.freedBytes[r->region] += r->size;
            aotStats.liveBytes[r->region] -= (int32_t)r->size;
            aotRecs[i] = aotRecs[--aotN];
            found = true;
            break;
        }
    if (!found)
        aotStats.untrackedFrees++; // allocated before arm (or dropped)
    portEXIT_CRITICAL_SAFE(&aotMux);
}

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__real_heap_caps_malloc(size_t, uint32_t);
void *__real_heap_caps_calloc(size_t, size_t, uint32_t);
void *__real_heap_caps_realloc(void *, size_t, uint32_t);
void __real_heap_caps_free(void *);
void *__real_heap_caps_aligned_alloc(size_t, size_t, uint32_t);
void *__real_heap_caps_aligned_calloc(size_t, size_t, size_t, uint32_t);

IRAM_ATTR void *__wrap_malloc(size_t n) { void *p = __real_malloc(n); AotAdd(p, n, AOT_CAPS_MALLOC); return p; }
IRAM_ATTR void *__wrap_calloc(size_t a, size_t b) { void *p = __real_calloc(a, b); AotAdd(p, a * b, AOT_CAPS_MALLOC); return p; }
IRAM_ATTR void *__wrap_realloc(void *o, size_t n) { void *p = __real_realloc(o, n); if (p) { AotDel(o); AotAdd(p, n, AOT_CAPS_MALLOC); } return p; }
IRAM_ATTR void __wrap_free(void *p) { AotDel(p); __real_free(p); }
IRAM_ATTR void *__wrap_heap_caps_malloc(size_t n, uint32_t c) { void *p = __real_heap_caps_malloc(n, c); AotAdd(p, n, c); return p; }
IRAM_ATTR void *__wrap_heap_caps_calloc(size_t a, size_t b, uint32_t c) { void *p = __real_heap_caps_calloc(a, b, c); AotAdd(p, a * b, c); return p; }
IRAM_ATTR void *__wrap_heap_caps_realloc(void *o, size_t n, uint32_t c) { void *p = __real_heap_caps_realloc(o, n, c); if (p) { AotDel(o); AotAdd(p, n, c); } return p; }
IRAM_ATTR void __wrap_heap_caps_free(void *p) { AotDel(p); __real_heap_caps_free(p); }
IRAM_ATTR void *__wrap_heap_caps_aligned_alloc(size_t a, size_t n, uint32_t c) { void *p = __real_heap_caps_aligned_alloc(a, n, c); AotAdd(p, n, c); return p; }
IRAM_ATTR void *__wrap_heap_caps_aligned_calloc(size_t a, size_t m, size_t n, uint32_t c) { void *p = __real_heap_caps_aligned_calloc(a, m, n, c); AotAdd(p, m * n, c); return p; }

static bool AotEnsureTable(void)
{
    if (!aotRecs)
        aotRecs = __real_heap_caps_malloc(AOT_MAX * sizeof(AotRec), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return aotRecs != NULL;
}

static void AotArm(void)
{
    portENTER_CRITICAL(&aotMux);
    aotN = 0;
    aotSeq = 0;
    memset(&aotStats, 0, sizeof(aotStats));
    portEXIT_CRITICAL(&aotMux);
    aotArmed = true;
}

void AdvDiag_AllocTraceBegin(const char *window)
{
    if (!s_active || aotArmed || !AotEnsureTable())
        return;
    AotArm();
    ESP_LOGW("AOT", "%s: armed", window);
}

void AdvDiag_AllocTraceEnd(const char *window)
{
    if (!aotArmed)
        return;
    aotArmed = false;
    ESP_LOGW("AOT", "%s: %d allocations since arm still live (dropped=%u); internal ones in task '%s':",
             window, aotN, (unsigned)aotStats.dropped, pcTaskGetName(NULL));
    for (int i = 0; i < aotN; i++)
    {
        AotRec *r = &aotRecs[i];
        if (!esp_ptr_internal(r->ptr))
            continue;
        ESP_LOGW("AOT", "  LIVE %p size=%u bt: 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx", r->ptr, (unsigned)r->size,
                 (unsigned long)r->pc[0], (unsigned long)r->pc[1], (unsigned long)r->pc[2],
                 (unsigned long)r->pc[3], (unsigned long)r->pc[4], (unsigned long)r->pc[5]);
    }
}

// ---- Expert Debug: aircraft TLS accounting window ----

static AotRec *acSnap;           // PSRAM snapshot of the live table for printing
static esp_timer_handle_t acTimer;
static volatile bool acWindowOpen;
static bool acCaptured;          // one failed attempt captured -> stop arming
static uint32_t acAttempt;
static uint32_t acFailBase;      // FAILED_ALLOC count at arm
static SemaphoreHandle_t acDumpLock; // timer dump vs. end-of-request dump

static void CapsText(uint32_t caps, char *out, size_t cap)
{
    if (caps == AOT_CAPS_MALLOC)
    {
        snprintf(out, cap, "malloc");
        return;
    }
    snprintf(out, cap, "0x%04x%s%s%s%s", (unsigned)caps,
             (caps & MALLOC_CAP_DMA) ? " DMA" : "",
             (caps & MALLOC_CAP_INTERNAL) ? " INT" : "",
             (caps & MALLOC_CAP_SPIRAM) ? " SPIRAM" : "",
             (caps & MALLOC_CAP_DEFAULT) ? " DEF" : "");
}

// Needs DMA-capable memory by its own request?  (DMA flag set.)  Plain
// malloc never asks for DMA; see the report for the lwIP/Wi-Fi TX caveat.
static const char *NeedText(uint32_t caps)
{
    if (caps == AOT_CAPS_MALLOC)
        return "cpu-only(malloc)";
    if (caps & MALLOC_CAP_DMA)
        return "REQUIRES-DMA";
    if (caps & MALLOC_CAP_INTERNAL)
        return "requires-internal";
    return "cpu-only";
}

static void AircraftDumpLocked(const char *why);

static void AircraftDump(const char *why)
{
    if (!acSnap || !acDumpLock)
        return;
    xSemaphoreTake(acDumpLock, portMAX_DELAY);
    AircraftDumpLocked(why);
    xSemaphoreGive(acDumpLock);
}

static void AircraftDumpLocked(const char *why)
{

    // Consistent copy of the live table + stats, then print without the lock.
    AotStats st;
    int n;
    portENTER_CRITICAL(&aotMux);
    n = aotN;
    memcpy(acSnap, aotRecs, (size_t)n * sizeof(AotRec));
    st = aotStats;
    portEXIT_CRITICAL(&aotMux);

    uint32_t fails = ExpertDebug_FailedAllocCount() - acFailBase;
    ESP_LOGW("ACTRACE", "==== attempt %u: %s ====", (unsigned)acAttempt, why);
    ESP_LOGW("ACTRACE", "heap now: internal free=%u, DMA free=%u largest=%u, PSRAM free=%u; FAILED_ALLOC since arm=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)fails);
    if (fails)
        ESP_LOGW("ACTRACE", "last FAILED_ALLOC: size=%u caps=0x%08x",
                 (unsigned)ExpertDebug_LastFailedSize(), (unsigned)ExpertDebug_LastFailedCaps());
    ESP_LOGW("ACTRACE", "window: allocs=%u frees=%u dropped=%u frees-of-older-blocks=%u peak live DMA-DRAM=%d",
             (unsigned)st.allocs, (unsigned)st.frees, (unsigned)st.dropped, (unsigned)st.untrackedFrees,
             (int)st.peakLiveDma);
    for (int r = 0; r < AOT_REG_COUNT; r++)
        ESP_LOGW("ACTRACE", "  %-10s allocated=%u freed(temporary)=%u live=%d",
                 kRegionName[r], (unsigned)st.allocBytes[r], (unsigned)st.freedBytes[r], (int)st.liveBytes[r]);

    // Live internal blocks, one line each (PSRAM ones are summarised above).
    ESP_LOGW("ACTRACE", "live internal allocations (seq size region requested-caps need | bt):");
    uint32_t liveDmaMalloc = 0, liveDmaCapsDma = 0, liveDmaOther = 0;
    for (int i = 0; i < n; i++)
    {
        AotRec *r = &acSnap[i];
        if (r->region != AOT_REG_DMA && r->region != AOT_REG_INT)
            continue;
        char capsText[40];
        CapsText(r->caps, capsText, sizeof(capsText));
        if (r->region == AOT_REG_DMA)
        {
            if (r->caps == AOT_CAPS_MALLOC)
                liveDmaMalloc += r->size;
            else if (r->caps & MALLOC_CAP_DMA)
                liveDmaCapsDma += r->size;
            else
                liveDmaOther += r->size;
        }
        ESP_LOGW("ACTRACE", "  #%u %u %s %s %s | 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx",
                 (unsigned)r->seq, (unsigned)r->size, kRegionName[r->region], capsText, NeedText(r->caps),
                 (unsigned long)r->pc[0], (unsigned long)r->pc[1], (unsigned long)r->pc[2],
                 (unsigned long)r->pc[3], (unsigned long)r->pc[4], (unsigned long)r->pc[5]);
    }
    ESP_LOGW("ACTRACE", "live DMA-DRAM by request type: malloc(cpu-only)=%u  explicit-DMA=%u  other-caps=%u",
             (unsigned)liveDmaMalloc, (unsigned)liveDmaCapsDma, (unsigned)liveDmaOther);

    // Aggregate live internal bytes by immediate caller (frame 0), so the
    // owners can be symbolized with a handful of addr2line lookups.
    typedef struct { uint32_t pc, bytes, count; uint8_t region; } Grp;
    Grp grp[48];
    int ng = 0;
    for (int i = 0; i < n; i++)
    {
        AotRec *r = &acSnap[i];
        if (r->region != AOT_REG_DMA && r->region != AOT_REG_INT)
            continue;
        int g = 0;
        while (g < ng && !(grp[g].pc == r->pc[0] && grp[g].region == r->region))
            g++;
        if (g == ng)
        {
            if (ng == (int)(sizeof(grp) / sizeof(grp[0])))
                continue;
            grp[ng++] = (Grp){r->pc[0], 0, 0, r->region};
        }
        grp[g].bytes += r->size;
        grp[g].count++;
    }
    ESP_LOGW("ACTRACE", "live internal bytes by caller (frame 0):");
    for (int g = 0; g < ng; g++)
        ESP_LOGW("ACTRACE", "  caller 0x%08lx %-10s bytes=%u count=%u",
                 (unsigned long)grp[g].pc, kRegionName[grp[g].region], (unsigned)grp[g].bytes, (unsigned)grp[g].count);
    ESP_LOGW("ACTRACE", "==== end attempt %u dump ====", (unsigned)acAttempt);
}

static void AircraftTimerCb(void *arg)
{
    (void)arg;
    if (acWindowOpen)
        AircraftDump("+1 s after Before aircraft TLS (connect in progress)");
}

void AdvDiag_ExpertTraceInit(void)
{
    // Called at boot only when Expert Debug is ON: PSRAM tables + timer are
    // created now so none of them lands inside the window being measured.
    if (!AotEnsureTable())
        return;
    acSnap = __real_heap_caps_malloc(AOT_MAX * sizeof(AotRec), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    acDumpLock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t ta = {.callback = AircraftTimerCb, .name = "actrace"};
    if (esp_timer_create(&ta, &acTimer) != ESP_OK)
        acTimer = NULL;
    ESP_LOGW("ACTRACE", "aircraft TLS allocation accounting %s",
             (acSnap && acTimer && acDumpLock) ? "ready" : "unavailable (no PSRAM/timer/mutex)");
}

void AdvDiag_AircraftTraceBegin(void)
{
    if (!ExpertDebug_Active() || acCaptured || !acSnap || !acTimer || !acDumpLock || aotArmed)
        return;
    acAttempt++;
    acFailBase = ExpertDebug_FailedAllocCount();
    AotArm();
    acWindowOpen = true;
    ESP_LOGW("ACTRACE", "attempt %u: armed at Before aircraft TLS", (unsigned)acAttempt);
    esp_timer_start_once(acTimer, 1000 * 1000);
}

void AdvDiag_AircraftTraceEnd(bool failed)
{
    if (!acWindowOpen)
        return;
    esp_timer_stop(acTimer);
    AircraftDump(failed ? "request returned FAILED (before client cleanup)"
                        : "request returned OK (before client cleanup)");
    acWindowOpen = false;
    aotArmed = false;
    if (failed)
    {
        acCaptured = true;
        ESP_LOGW("ACTRACE", "failed attempt captured; accounting window will not re-arm this boot");
    }
}
#else
void AdvDiag_AllocTraceBegin(const char *window) { (void)window; }
void AdvDiag_AllocTraceEnd(const char *window) { (void)window; }
void AdvDiag_ExpertTraceInit(void)
{
    ESP_LOGW("ACTRACE", "aircraft TLS allocation accounting not compiled in (build with CONFIG_FR_ALLOC_TRACE=y)");
}
void AdvDiag_AircraftTraceBegin(void) {}
void AdvDiag_AircraftTraceEnd(bool failed) { (void)failed; }
#endif

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

void AdvDiag_LoadAtBoot(void)
{
    s_active = AdvDiag_GetSaved();
    if (!s_active)
    {
        ESP_LOGI(TAG, "Advanced Diagnostics: OFF (enable on /diag, then reboot)");
        return;
    }

    // Everything below is created only when active, early in boot, so none
    // of it lands inside the regions being measured later. Tables go to
    // PSRAM; only the esp_timer handle is a small internal allocation.
    s_cp = heap_caps_calloc(CP_MAX, sizeof(AdvDiagCheckpoint), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    hdiagBase = heap_caps_malloc(3 * HDIAG_MAX * sizeof(HDiagBlk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (hdiagBase)
    {
        hdiagCur = hdiagBase + HDIAG_MAX;
        hdiagFree = hdiagCur + HDIAG_MAX;
    }
    const esp_timer_create_args_t ta = {.callback = PcbTraceTimerCb, .name = "pcbtrace"};
    if (esp_timer_create(&ta, &pcbTraceTimer) != ESP_OK)
        pcbTraceTimer = NULL;
#if CONFIG_FR_ALLOC_TRACE
    AotEnsureTable();
#endif

    ESP_LOGW(TAG, "Advanced Diagnostics: ON for this boot (allocation trace %s)",
             AdvDiag_AllocTraceCompiled() ? "compiled in" : "not compiled in");
    if (!s_cp || !hdiagBase || !pcbTraceTimer)
        ESP_LOGW(TAG, "Some advanced diagnostic buffers could not be allocated; those features stay off");
    AdvDiag_HeapCheckpoint("Boot: after NVS init");
}
