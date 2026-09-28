// LVGL pool provider - see fr_lv_pool.h.

#include "fr_lv_pool.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_system.h"

static const char *TAG = "LV_POOL";

void FrLvPool_LogHeap(const char *stage)
{
    ESP_LOGI(TAG, "%s: internal free=%u largest=%u | DMA free=%u largest=%u | PSRAM free=%u largest=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

void *fr_lv_pool_alloc(size_t size)
{
    FrLvPool_LogHeap("Before LVGL pool");

    // PSRAM only. Deliberately no internal fallback: the point of this
    // provider is to keep these 64 KB out of internal DMA-capable RAM.
    void *pool = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (pool == NULL || !esp_ptr_external_ram(pool))
    {
        ESP_LOGE(TAG, "LVGL %u-byte PSRAM pool could NOT be allocated (ptr=%p, PSRAM free=%u largest=%u). "
                      "Not falling back to internal RAM; aborting.",
                 (unsigned)size, pool,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        // lv_mem_init() does not check the result: a NULL pool would make
        // lv_tlsf_create_with_pool() write its control block at address 0.
        // Stop here with an explicit reason instead (same outcome as the
        // display's other ESP_ERROR_CHECK start-up failures).
        esp_system_abort("LVGL PSRAM pool allocation failed");
    }

    ESP_LOGI(TAG, "LVGL pool: requested=%u ptr=%p PSRAM=%s (caps SPIRAM|8BIT, TLSF managed by LVGL)",
             (unsigned)size, pool, esp_ptr_external_ram(pool) ? "yes" : "NO");
    FrLvPool_LogHeap("After LVGL pool");
    return pool;
}
