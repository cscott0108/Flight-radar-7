#pragma once
/* Zero-aircraft idle maintenance: one bounded persistence drain per zero-aircraft period, and at most
 * one per local calendar day.
 *
 *   HOLDOFF     first IDLE_MAINT_HOLDOFF_US after boot (64-bit monotonic esp_timer_get_time(), reset by
 *               every reboot): never drains, whatever the aircraft count.
 *   NORMAL      the last successful poll reported aircraft.
 *   CONFIRMING  zero aircraft, but fewer than IDLE_MAINT_ZERO_POLLS consecutive successful zero polls
 *               (or the local date is not known yet because the clock has not synchronized).
 *   DRAIN       holdoff over AND zero confirmed AND not already run on this local date: the caller runs
 *               IdleMaint_Drain(); the state stays DRAIN while that attempt's History result is pending.
 *   DORMANT     nothing more to do in this zero period: the attempt finished (whatever its result), or
 *               the daily guard says today's attempt has already been used.
 *
 * Two independent guards: drainedThisPeriod (re-armed whenever aircraft return) and lastDrainDay (the
 * local date of the last attempt, RAM only, reset by a reboot - which performs its own final Seen + History
 * flush). A failed, partial or timed-out attempt still uses up that day's opportunity.
 *
 * Driven only by SUCCESSFUL provider polls (RadarTask, after a valid parse). A failed or malformed
 * response is simply not reported here, so it never counts as a zero confirmation and never resets the
 * count. No timer, queue or task. The existing 600 s SeenAircraft_FlushIfDue / HistoryManager_FlushIfDue
 * schedule is untouched and keeps handling new dirty data and retries.
 *
 * Independent of the display idle-dim state: this never reads the backlight. IdleStatus_Select() combines
 * the two for the on-radar status text. */
#include <stdbool.h>
#include <stdint.h>

#define IDLE_MAINT_HOLDOFF_US (300LL * 1000000LL)
#define IDLE_MAINT_ZERO_POLLS 2u
/* RadarTask serves the History request itself on its next 250 ms slice; this only bounds the wait for a
 * result that can never arrive (e.g. History compiled out). Expiry counts as "completed with warnings". */
#define IDLE_MAINT_RESULT_TIMEOUT_US (20LL * 1000000LL)

typedef enum {
    IDLE_MAINT_HOLDOFF = 0,
    IDLE_MAINT_NORMAL,
    IDLE_MAINT_CONFIRMING,
    IDLE_MAINT_DRAIN,
    IDLE_MAINT_DORMANT,
} IdleMaintState;

/* What the display should know about maintenance (published by RadarTask, read by the UI). */
typedef enum {
    IDLE_MAINT_PHASE_NONE = 0,
    IDLE_MAINT_PHASE_RUNNING,   /* this zero period's attempt is in progress */
    IDLE_MAINT_PHASE_DONE_OK,   /* it finished and everything pending was written (until the next poll) */
    IDLE_MAINT_PHASE_DONE_WARN, /* it finished, but something could not be written / timed out (until the next poll) */
} IdleMaintPhase;

typedef struct {
    IdleMaintState state;
    uint32_t zeroPolls;      /* consecutive successful polls with zero aircraft */
    bool drainedThisPeriod;  /* the one drain of the current zero period has been attempted */
    int32_t lastDrainDay;    /* local date (YYYYMMDD) of the last attempt; 0 = none since boot */
    int32_t candidateDay;    /* local date of the poll that triggered the pending drain */
    uint32_t drains;         /* drains attempted since boot (diagnostics/tests) */
    /* result of the current/last attempt */
    bool resultPending;
    bool historyRequested;
    uint32_t historySeq;
    bool seenOk;
    int64_t drainStartUs;
    bool lastOk;
    bool announce;           /* show the completion status until the next successful poll */
} IdleMaint;

void IdleMaint_Init(IdleMaint *m);

/* Report one successful poll. localDayKey = local calendar date as YYYYMMDD from the time layer, or 0
 * while the clock is not synchronized (no drain is started without a known date). Returns true exactly
 * when the caller should run IdleMaint_Drain() now. */
bool IdleMaint_OnSuccessfulPoll(IdleMaint *m, int64_t uptimeUs, int aircraftCount, int32_t localDayKey);

/* Starts the single attempt (RadarTask only): records today's date, flushes Hot Seen if dirty, then asks the
 * History Manager for a flush through HistoryManager_RequestFlush() (served by RadarTask's next 250 ms slice
 * inside HistoryManager_FlushIfDue, same owner/locking as the /diag manual flush). Never waits. */
void IdleMaint_Drain(IdleMaint *m, bool historyEnabled, int64_t uptimeUs);

/* RadarTask, once per 250 ms slice AFTER HistoryManager_FlushIfDue: completes a pending attempt from the
 * real History result (an O(1) flag test when nothing is pending). Returns true when it completed now. */
bool IdleMaint_PollResult(IdleMaint *m, int64_t uptimeUs);

IdleMaintPhase IdleMaint_Phase(const IdleMaint *m);
const char *IdleMaint_StateName(IdleMaintState s);

/* ---- on-radar status text (pure) ---- */
typedef enum {
    IDLE_STATUS_NONE = 0,     /* normal radar, no overlay */
    IDLE_STATUS_STANDBY,      /* zero aircraft, radar frozen: waiting */
    IDLE_STATUS_MAINT_RUNNING,
    IDLE_STATUS_MAINT_DONE,
    IDLE_STATUS_MAINT_WARN,
    IDLE_STATUS_COUNT
} IdleStatus;

/* aircraft present -> NONE; maintenance running -> RUNNING; just finished -> DONE/WARN; radar frozen
 * (display idle) -> STANDBY; otherwise NONE. */
IdleStatus IdleStatus_Select(int aircraftCount, bool displayIdle, IdleMaintPhase phase);
const char *IdleStatus_Text(IdleStatus s);
