// Expert Debug Mode - see expert_debug.h.

#include "expert_debug.h"

#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/cache_utils.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "adv_diag.h"
#include "esp_timer.h"
#include "failed_alloc_stamp.h"

static const char *TAG = "EXPERT";

#define EXPERT_NAMESPACE "diag"
#define EXPERT_KEY "expert"

static bool s_active; // fixed for the whole boot

// DRAM so the callback can update it even while the flash cache is off.
static DRAM_ATTR volatile uint32_t s_failedAllocCount;
static DRAM_ATTR volatile uint32_t s_lastFailedSize;
static DRAM_ATTR volatile uint32_t s_lastFailedCaps;
static DRAM_ATTR char s_lastFailedTask[16];
static DRAM_ATTR FailedAllocStamp s_lastFailedWhen; /* 0.1.6: uptime of the latest failure (see failed_alloc_stamp.h) */

bool ExpertDebug_Active(void)
{
    return s_active;
}

uint32_t ExpertDebug_FailedAllocCount(void)
{
    return s_failedAllocCount;
}

uint32_t ExpertDebug_LastFailedSize(void)
{
    return s_lastFailedSize;
}

uint32_t ExpertDebug_LastFailedCaps(void)
{
    return s_lastFailedCaps;
}

const char *ExpertDebug_LastFailedTask(void)
{
    return s_lastFailedTask;
}

bool ExpertDebug_LastFailedUptimeUs(int64_t *uptimeUs)
{
    return FailedAllocStamp_Read(&s_lastFailedWhen, uptimeUs);
}

bool ExpertDebug_GetSaved(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(EXPERT_NAMESPACE, NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_u8(h, EXPERT_KEY, &v);
        nvs_close(h);
    }
    return v != 0;
}

esp_err_t ExpertDebug_SetSaved(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(EXPERT_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_u8(h, EXPERT_KEY, enabled ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------------------
// FAILED_ALLOC hook
//
// Called by ESP-IDF's heap_caps_alloc_failed() (IRAM) in the context of the
// failing allocation - any task, possibly an ISR, possibly with the flash
// cache disabled - after the heap locks have been released.
// Rules followed here:
//   - IRAM function, counter in DRAM;
//   - if the flash cache is disabled: count only, return (format strings,
//     function-name strings and pcTaskGetName may live in flash);
//   - output only via esp_rom_printf (ROM, no allocation, no locks taken by
//     the allocator);
//   - no malloc/heap_caps_malloc, no ESP_LOG, no FreeRTOS calls that allocate;
//   - heap_caps_get_largest_free_block() only reads heap metadata under the
//     heap's own lock (not held at this point) and never allocates.
// ---------------------------------------------------------------------------

static IRAM_ATTR void PrintCaps(uint32_t caps)
{
    // Names for the capability bits that matter for this project.
    if (caps & MALLOC_CAP_32BIT) esp_rom_printf(" 32BIT");
    if (caps & MALLOC_CAP_8BIT) esp_rom_printf(" 8BIT");
    if (caps & MALLOC_CAP_DMA) esp_rom_printf(" DMA");
    if (caps & MALLOC_CAP_SPIRAM) esp_rom_printf(" SPIRAM");
    if (caps & MALLOC_CAP_INTERNAL) esp_rom_printf(" INTERNAL");
    if (caps & MALLOC_CAP_DEFAULT) esp_rom_printf(" DEFAULT");
    if (caps & MALLOC_CAP_IRAM_8BIT) esp_rom_printf(" IRAM_8BIT");
    if (caps & MALLOC_CAP_RETENTION) esp_rom_printf(" RETENTION");
    if (caps & MALLOC_CAP_RTCRAM) esp_rom_printf(" RTCRAM");
}

static IRAM_ATTR void FailedAllocHook(size_t size, uint32_t caps, const char *function_name)
{
    s_lastFailedSize = (uint32_t)size;
    s_lastFailedCaps = caps;
    /* 0.1.6: uptime only (IRAM systimer read, no lock); stored before the count so a reader that sees
     * the new count normally sees this failure's time. Wall time is derived at /diag render. */
    FailedAllocStamp_Record(&s_lastFailedWhen, esp_timer_get_time());
    uint32_t n = ++s_failedAllocCount;

    if (!spi_flash_cache_enabled())
        return; // count only; nothing below is guaranteed to be in IRAM/DRAM

    // Remember which task failed (copied: the TCB may be gone later).
    {
        const char *t = xPortInIsrContext() ? "ISR" : pcTaskGetName(NULL);
        size_t i = 0;
        for (; t && t[i] && i < sizeof(s_lastFailedTask) - 1; i++)
            s_lastFailedTask[i] = t[i];
        s_lastFailedTask[i] = '\0';
    }

    // Normal mode: counting only. Everything below (printing) is Expert Debug.
    if (!s_active)
        return;

    bool verbose = n <= EXPERT_FAILED_ALLOC_VERBOSE;
    if (!verbose && (n % EXPERT_FAILED_ALLOC_EVERY) != 0)
        return;

    bool inIsr = xPortInIsrContext();
    const char *task = inIsr ? "ISR" : pcTaskGetName(NULL);

    // For the requested caps, how big is the biggest block still available?
    // (Tells "fragmented" apart from "exhausted" for this exact request.)
    size_t largestForCaps = heap_caps_get_largest_free_block(caps);
    size_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!verbose)
    {
        esp_rom_printf("FAILED_ALLOC #%u size=%u caps=0x%08x task=%s (summary; %u failures so far)\n",
                       (unsigned)n, (unsigned)size, (unsigned)caps, task ? task : "?", (unsigned)n);
        return;
    }

    esp_rom_printf("FAILED_ALLOC #%u size=%u caps=0x%08x [", (unsigned)n, (unsigned)size, (unsigned)caps);
    PrintCaps(caps);
    esp_rom_printf(" ] fn=%s task=%s\n", function_name ? function_name : "?", task ? task : "?");
    esp_rom_printf("FAILED_ALLOC #%u largest_for_caps=%u dma_free=%u dma_largest=%u\n",
                   (unsigned)n, (unsigned)largestForCaps, (unsigned)dmaFree, (unsigned)dmaLargest);

    // 8-frame backtrace. Frame 0 is this hook's caller chain inside the heap
    // (heap_caps_alloc_failed / heap_caps_*alloc); the owner follows.
    esp_rom_printf("FAILED_ALLOC #%u bt:", (unsigned)n);
    esp_backtrace_frame_t fr;
    esp_backtrace_get_start(&fr.pc, &fr.sp, &fr.next_pc);
    for (int i = 0; i < 8; i++)
    {
        if (!esp_backtrace_get_next_frame(&fr))
            break;
        esp_rom_printf(" 0x%08x", (unsigned)esp_cpu_process_stack_pc(fr.pc));
    }
    esp_rom_printf("\n");
}

void ExpertDebug_LoadAtBoot(void)
{
    s_active = ExpertDebug_GetSaved();

    // The failed-allocation hook is registered in every mode so Normal /diag
    // can show a failure count and the last failure (size, caps, task). With
    // Expert Debug OFF it only updates those counters (IRAM, no printing, no
    // allocation, runs only when an allocation has already failed). The
    // FAILED_ALLOC printing and ACTRACE remain Expert-only.
    esp_err_t err = heap_caps_register_failed_alloc_callback(FailedAllocHook);
    if (!s_active)
    {
        ESP_LOGI(TAG, "Expert Debug Mode: OFF (failed-allocation counter %s)",
                 err == ESP_OK ? "registered" : "registration FAILED");
        return;
    }

    ESP_LOGW(TAG, "Expert Debug Mode: ON for this boot (forensic). FAILED_ALLOC hook %s",
             err == ESP_OK ? "registered" : "registration FAILED");
    AdvDiag_ExpertTraceInit(); // aircraft-TLS allocation accounting (ACTRACE)
}
