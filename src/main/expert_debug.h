#pragma once

/* Expert Debug Mode - heavyweight forensic instrumentation.
 *
 * Also owns the aircraft-TLS allocation accounting window (ACTRACE, see
 * adv_diag.h AdvDiag_AircraftTrace*), which additionally needs a build with
 * CONFIG_FR_ALLOC_TRACE=y because it relies on the linker-wrapped allocator
 * recorder; production builds without that option carry none of it.
 *
 * Diagnostic tiers (all persistent in NVS namespace "diag", all read ONCE at
 * boot, all changed only via /diag and effective after reboot):
 *   1. Normal diagnostics  - always on, lightweight (/diag rows, the
 *                            "Before OAuth/aircraft TLS" heap lines).
 *   2. Advanced Diagnostics - key "adv"    (adv_diag.h): heap checkpoints,
 *                            HDIAG, PCBTRACE, optional allocation trace.
 *   3. Expert Debug Mode   - key "expert" (this file), OFF by default.
 *                            Hooks that act inside the allocator or other
 *                            hot paths and may print from any task/ISR.
 *
 * Kept permanently so future forensic work needs no code changes. When OFF,
 * only a counting failed-allocation hook is registered (it runs solely after
 * an allocation has already failed, never allocates and never prints), so
 * Normal /diag can report failures; allocation behaviour is unchanged.
 *
 * Current Expert Debug instrumentation:
 *   - FAILED_ALLOC: heap_caps_register_failed_alloc_callback() hook that
 *     prints every failed heap allocation (size, requested caps, largest
 *     free block for those caps, task, 8-frame backtrace) with
 *     esp_rom_printf() only. It never allocates. The first
 *     EXPERT_FAILED_ALLOC_VERBOSE failures print in full; after that one
 *     short line per EXPERT_FAILED_ALLOC_EVERY failures, so a retry storm
 *     (e.g. Wi-Fi RX) cannot flood the UART and change timing.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define EXPERT_FAILED_ALLOC_VERBOSE 32
#define EXPERT_FAILED_ALLOC_EVERY 64

// Reads the NVS setting and, if ON, registers the Expert Debug hooks. Call
// once in app_main right after nvs_flash_init() (before any warm-up).
void ExpertDebug_LoadAtBoot(void);

// Active for this boot (fixed at ExpertDebug_LoadAtBoot).
bool ExpertDebug_Active(void);

// Persisted setting (what the next boot will use).
bool ExpertDebug_GetSaved(void);
esp_err_t ExpertDebug_SetSaved(bool enabled);

// Failed allocations seen since boot (counted in every mode), and
// the size/caps of the most recent one (for ACTRACE correlation).
uint32_t ExpertDebug_FailedAllocCount(void);
uint32_t ExpertDebug_LastFailedSize(void);
uint32_t ExpertDebug_LastFailedCaps(void);
// Name of the task whose allocation failed most recently ("" if none, "ISR").
const char *ExpertDebug_LastFailedTask(void);
// 0.1.6: uptime (esp_timer, microseconds since boot) of the most recent failure. False when no time
// was stored or no consistent value could be read ("unavailable"; not the same as "no failures").
bool ExpertDebug_LastFailedUptimeUs(int64_t *uptimeUs);
