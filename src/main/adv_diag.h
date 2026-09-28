#pragma once

/* Advanced Diagnostics (heap topology / allocation / TLS lifecycle).
 *
 * Controlled by a persistent NVS setting (namespace "diag", key "adv") that
 * is read ONCE at boot by AdvDiag_LoadAtBoot(). The active state never
 * changes mid-run: toggling it on /diag only rewrites NVS and takes effect
 * after the next reboot, so the memory behaviour being measured is never
 * altered halfway through a run.
 *
 * Three tiers (see PROJECT_STATE_COMPACT.md "Advanced Diagnostics"):
 *  - Always compiled, dormant unless active: heap checkpoints, HDIAG heap
 *    walk diff, PCBTRACE closing-socket trace. When inactive every hook is a
 *    single bool test; no PSRAM tables, timers or extra logs are created.
 *  - Compile-time only: allocation-owner trace (linker --wrap on malloc and
 *    heap_caps_*). Only built when CONFIG_FR_ALLOC_TRACE=y (default n);
 *    even then it records only while Advanced Diagnostics is active.
 *  - Normal (always on): the pre-existing "Before OAuth/aircraft TLS" heap
 *    lines and the /diag RAM rows.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Reads the NVS setting into the per-boot active flag. Call once, right after
// nvs_flash_init() and before any warm-up, so boot transitions are captured.
void AdvDiag_LoadAtBoot(void);

// Active for this boot (fixed at AdvDiag_LoadAtBoot).
bool AdvDiag_Active(void);

// Persisted setting (what the next boot will use).
bool AdvDiag_GetSaved(void);
esp_err_t AdvDiag_SetSaved(bool enabled);

// True when the allocation-owner trace is compiled into this firmware.
bool AdvDiag_AllocTraceCompiled(void);

// Logs internal free / largest, DMA-capable free and largest, and PSRAM free, and keeps
// the latest values for /diag. No-op unless active.
void AdvDiag_HeapCheckpoint(const char *stage);

// HDIAG: snapshot used DMA-capable internal blocks (first call only), then
// log blocks that are new since that snapshot plus free blocks >= 1 KB.
void AdvDiag_HeapBaseline(void);
void AdvDiag_HeapDiff(const char *stage);

// PCBTRACE: after a TLS connection is closed, follow the closing TCP PCB at
// ~0/1/5/20/60/120 s and log its state with heap figures. First call only.
void AdvDiag_PcbTraceStart(void);

// Allocation-owner trace window (only when compiled in AND active). Begin
// arms recording; End prints the internal allocations made since Begin that
// are still live (ptr, size, 6-frame backtrace), then disarms.
void AdvDiag_AllocTraceBegin(const char *window);
void AdvDiag_AllocTraceEnd(const char *window);

// Expert Debug aircraft-TLS allocation accounting (needs CONFIG_FR_ALLOC_TRACE
// and Expert Debug Mode ON; otherwise all three are no-ops).
//  - ExpertTraceInit: boot-time setup (PSRAM tables, timer); called by
//    ExpertDebug_LoadAtBoot() only when Expert Debug is ON.
//  - AircraftTraceBegin: at "Before aircraft TLS"; arms the recorder and a
//    1 s dump of the in-progress connect.
//  - AircraftTraceEnd: when the request returns, before client cleanup;
//    dumps again and disarms. After one failed attempt is captured the
//    window no longer re-arms this boot.
// ACTRACE output: window stats per heap region (allocated / freed / live),
// every live internal allocation (size, region, requested caps, whether it
// asked for DMA, 6-frame backtrace), and live bytes grouped by caller.
void AdvDiag_ExpertTraceInit(void);
void AdvDiag_AircraftTraceBegin(void);
void AdvDiag_AircraftTraceEnd(bool failed);

// Read access for /diag: the most recent checkpoints (oldest first).
typedef struct
{
    const char *stage;
    uint32_t uptimeMs;
    uint32_t internalFree;
    uint32_t internalLargest;
    uint32_t dmaFree;    // total DMA-capable internal free (exhaustion)
    uint32_t dmaLargest; // largest DMA-capable internal block (fragmentation)
    uint32_t psramFree;
} AdvDiagCheckpoint;

size_t AdvDiag_GetCheckpoints(AdvDiagCheckpoint *out, size_t max);

// Free blocks >= minSize in DMA-capable internal RAM (address, size), largest
// first is NOT guaranteed; returns the count written. Walks the heap briefly.
typedef struct
{
    uintptr_t addr;
    uint32_t size;
} AdvDiagFreeBlock;

size_t AdvDiag_GetDmaFreeBlocks(AdvDiagFreeBlock *out, size_t max, size_t minSize);
