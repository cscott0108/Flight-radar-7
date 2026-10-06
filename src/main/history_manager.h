#pragma once

/* History Manager: the seam between aircraft tracking/classification and
 * persistent TF history (tf_history.h) - PROMPT.md section 20.
 *
 *   Tracking -> Classification -> History Manager -> Universal Value -> TF
 *
 * Radar/UI code never needs to know about buckets, fingerprints or the TF
 * index; it only ever sees the existing RAM Hot Seen cache
 * (seen_aircraft.h), which this module does NOT modify or replace -
 * PROMPT.md section 3 and section 18 both require the existing Seen
 * behavior to keep working exactly as before, just described as "hot"
 * relative to this new "warm" layer underneath it.
 *
 * Bounded RAM: one shadow slot per currently-tracked aircraft (capped at
 * MAX_AIRCRAFT, the same cap the radar itself already uses), NOT one per
 * historical aircraft ever seen - the archive itself lives on TF and is
 * never loaded into RAM (PROMPT.md section 19, 42). */

#include <stdbool.h>
#include <stdint.h>

#include "custom_rules.h" /* CraftResolution */

/* Call once at boot, after CustomRules_Init() and SeenAircraft_Init().
 * Initializes the Universal Value table and attempts to mount TF; a TF
 * mount failure leaves history in RAM-only degraded mode and is never
 * fatal (PROMPT.md section 27) - the return value is purely informational. */
bool HistoryManager_Init(void);

/* Call once per valid aircraft per poll (main.c, alongside the existing
 * SeenAircraft_ObservePoll - that call is untouched). `resolution` is
 * whatever ResolveAircraftWithHint() already returned for this aircraft,
 * so classification is computed exactly once per aircraft per poll, not
 * twice. Cheap: normally RAM-only; performs at most one TF index lookup,
 * only the first time a given aircraft is seen since boot. */
/* 0.0.27: records the provider-supplied registration (tail number) for an
 * aircraft already observed this poll. Empty = unknown, ignored (a known
 * registration is never erased). Only marks the record dirty when it changes. */
void HistoryManager_ObserveRegistration(const char *icao24, const char *registration);

void HistoryManager_Observe(
    const char *icao24,
    const char *callsign,
    CraftResolution resolution,
    int64_t nowUtc);

/* Cheap; call every poll-loop slice alongside the existing
 * SeenAircraft_FlushIfDue(). Writes only shadow entries marked dirty, at
 * most once per HM_SYNC_INTERVAL_SEC, and also re-syncs the Universal
 * Value table against the current operator list first (so web_rules.c
 * needs no changes to keep Universal Values current - PROMPT.md section
 * 7/8). Returns true if anything was written to TF this call. */
bool HistoryManager_FlushIfDue(uint32_t nowMonotonicSec);

/* Diagnostic hooks (web_diag.c TF unmount/reinit). Pause stops all TF access
 * from History Manager without touching its shadow slots; Resume re-enables
 * it only if TfHistory_IsAvailable(). No other History Manager behavior changes. */
void HistoryManager_TfPause(void);
void HistoryManager_TfResume(void);
bool HistoryManager_IsTfReady(void);

typedef struct {
    uint32_t shadowSlotsUsed;
    uint32_t shadowSlotsCap;
    uint32_t dirtyNow;
} HistoryManagerStats;

void HistoryManager_GetStats(HistoryManagerStats *out);

/* Manual flush (diagnostics). The web task must not touch the shadow table
 * (the poll task owns it), so it posts a request and waits:
 *   seq = HistoryManager_RequestFlush();
 *   while (!HistoryManager_GetFlushResult(seq, &r)) { wait ... }
 * The poll task serves the request from FlushIfDue on its next slice, outside
 * the periodic interval gate (which is left undisturbed). GetFlushResult
 * returns false until that pass has completed. Dirty slots that could not be
 * written stay dirty (counted in `remaining`) and are retried by the normal
 * periodic flush. */
typedef struct {
    bool ok;            /* pass completed with no failed writes and TF available */
    bool tfUnavailable; /* TF not ready: nothing was attempted */
    uint32_t written;
    uint32_t failed;
    uint32_t remaining; /* dirty slots still unwritten after this pass */
} HistoryFlushResult;

uint32_t HistoryManager_RequestFlush(void);
bool HistoryManager_GetFlushResult(uint32_t seq, HistoryFlushResult *out);
