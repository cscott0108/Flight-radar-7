#pragma once

/* Provider for LVGL's 64 KB built-in memory pool.
 *
 * LVGL (LV_MEM_CUSTOM=0) keeps its own TLSF allocator; lv_mem_init() asks
 * LV_MEM_POOL_ALLOC(LV_MEM_SIZE) once for the block TLSF manages. Without it
 * LVGL uses a static .bss array (work_mem_int) in internal, DMA-capable DIRAM.
 * fr_lv_pool_alloc() places that one block in PSRAM instead, so ~64 KB of
 * internal DIRAM stays in the heap. It never falls back to internal RAM: if
 * PSRAM cannot provide the pool it logs an ERROR and aborts (LVGL would
 * otherwise build its TLSF control structure at address 0).
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called by LVGL's lv_mem_init() (via LV_MEM_POOL_ALLOC). Returns a PSRAM
// block of `size` bytes or does not return.
void *fr_lv_pool_alloc(size_t size);

// One line: internal / DMA-capable internal / PSRAM free and largest.
void FrLvPool_LogHeap(const char *stage);

#ifdef __cplusplus
}
#endif
