#pragma once
/* Final persistence pass for deliberate reboots (httpd task, just before the existing unmount/esp_restart):
 *   1. Hot Seen: flushed if dirty (unchanged behaviour).
 *   2. TF History: requested through HistoryManager_RequestFlush() and served by RadarTask, the sole owner
 *      of the shadow table - the same hand-off the /diag manual flush uses; no second TF writer.
 *   3. Bounded wait (REBOOT_HISTORY_WAIT_MS), no storage lock held while waiting.
 * The caller reboots afterwards regardless of the outcome; a History flush that does not complete in time
 * is logged (and reported through diagnostics telemetry), never waited on indefinitely. */
#include <stdbool.h>
#include <stdint.h>

#define REBOOT_HISTORY_WAIT_MS 10000u
#define REBOOT_HISTORY_POLL_MS 100u

typedef struct {
    bool seenWasDirty;
    bool seenOk;           /* true when Seen was clean or saved */
    bool historyAnswered;  /* RadarTask served the request within the bound */
    bool historyOk;        /* answered and fully written */
    uint32_t waitedMs;
} RebootFlushResult;

void RebootFlush_BeforeRestart(RebootFlushResult *out); /* out may be NULL */
