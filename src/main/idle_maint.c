#include "idle_maint.h"

#include <string.h>

#include "esp_log.h"
#include "history_manager.h"
#include "seen_aircraft.h"

static const char *TAG = "IDLEMAINT";

void IdleMaint_Init(IdleMaint *m)
{
    memset(m, 0, sizeof(*m));
    m->state = IDLE_MAINT_HOLDOFF;
}

bool IdleMaint_OnSuccessfulPoll(IdleMaint *m, int64_t uptimeUs, int aircraftCount, int32_t localDayKey)
{
    bool holdoff = uptimeUs < IDLE_MAINT_HOLDOFF_US;

    m->announce = false; /* a completion status is shown until the next successful poll */

    if (aircraftCount > 0) {
        /* Aircraft present: leave any idle state at once and re-arm the per-period guard. A pending
         * attempt is NOT cancelled: its History request is already queued and finishes on its own
         * (IdleMaint_PollResult still consumes the result). The daily guard is unaffected. */
        m->zeroPolls = 0;
        m->drainedThisPeriod = false;
        m->state = holdoff ? IDLE_MAINT_HOLDOFF : IDLE_MAINT_NORMAL;
        return false;
    }

    if (m->zeroPolls < UINT32_MAX)
        m->zeroPolls++;

    if (m->resultPending && m->state == IDLE_MAINT_DRAIN)
        return false; /* still running: never start another */
    if (m->drainedThisPeriod) {
        m->state = IDLE_MAINT_DORMANT;
        return false;
    }
    if (holdoff) {
        m->state = IDLE_MAINT_HOLDOFF;
        return false;
    }
    if (m->zeroPolls < IDLE_MAINT_ZERO_POLLS || localDayKey <= 0) {
        m->state = IDLE_MAINT_CONFIRMING; /* not confirmed yet, or the local date is unknown */
        return false;
    }
    if (localDayKey == m->lastDrainDay) {
        m->state = IDLE_MAINT_DORMANT;    /* today's attempt has already been used */
        return false;
    }
    if (m->resultPending)
        return false;                     /* an earlier attempt's result is still outstanding */
    m->candidateDay = localDayKey;
    m->state = IDLE_MAINT_DRAIN;
    return true;
}

static void Complete(IdleMaint *m, bool ok, const char *why)
{
    bool wasRunning = (m->state == IDLE_MAINT_DRAIN);
    m->resultPending = false;
    m->lastOk = ok;
    if (wasRunning) {
        m->state = IDLE_MAINT_DORMANT;
        m->announce = true; /* only when still at zero aircraft; aircraft returning is not a failure */
    }
    ESP_LOGI(TAG, "Zero-aircraft maintenance %s%s%s", ok ? "complete" : "complete with warnings",
             why ? ": " : "", why ? why : "");
}

void IdleMaint_Drain(IdleMaint *m, bool historyEnabled, int64_t uptimeUs)
{
    /* Mark the attempt first: whatever happens below, neither this zero period nor this local date
     * drains again. */
    m->drainedThisPeriod = true;
    m->lastDrainDay = m->candidateDay;
    m->drains++;
    m->state = IDLE_MAINT_DRAIN;
    m->drainStartUs = uptimeUs;
    m->resultPending = true;
    m->announce = false;

    bool seenDirty = SeenAircraft_IsDirty();
    m->seenOk = seenDirty ? SeenAircraft_Flush() : true;

    m->historyRequested = historyEnabled;
    if (historyEnabled) {
        m->historySeq = HistoryManager_RequestFlush(); /* served by this task's next FlushIfDue slice */
        ESP_LOGI(TAG, "Zero-aircraft maintenance started: Seen %s, History flush requested",
                 seenDirty ? (m->seenOk ? "saved" : "NOT saved") : "clean");
    } else {
        Complete(m, m->seenOk, m->seenOk ? NULL : "Seen write failed");
    }
}

bool IdleMaint_PollResult(IdleMaint *m, int64_t uptimeUs)
{
    if (!m->resultPending)
        return false;

    HistoryFlushResult r;
    if (m->historyRequested && HistoryManager_GetFlushResult(m->historySeq, &r)) {
        bool ok = m->seenOk && r.ok;
        Complete(m, ok, ok ? NULL : (!m->seenOk ? "Seen write failed" : (r.tfUnavailable ? "TF card unavailable" : "some History entries not written")));
        return true;
    }
    if (uptimeUs - m->drainStartUs >= IDLE_MAINT_RESULT_TIMEOUT_US) {
        Complete(m, false, "History result timed out");
        return true;
    }
    return false;
}

IdleMaintPhase IdleMaint_Phase(const IdleMaint *m)
{
    if (m->state == IDLE_MAINT_DRAIN) /* from the decision until the result (also while Seen is flushing) */
        return IDLE_MAINT_PHASE_RUNNING;
    if (m->announce)
        return m->lastOk ? IDLE_MAINT_PHASE_DONE_OK : IDLE_MAINT_PHASE_DONE_WARN;
    return IDLE_MAINT_PHASE_NONE;
}

const char *IdleMaint_StateName(IdleMaintState s)
{
    switch (s) {
    case IDLE_MAINT_HOLDOFF: return "holdoff";
    case IDLE_MAINT_NORMAL: return "normal";
    case IDLE_MAINT_CONFIRMING: return "confirming zero";
    case IDLE_MAINT_DRAIN: return "drain";
    case IDLE_MAINT_DORMANT: return "dormant";
    }
    return "?";
}

IdleStatus IdleStatus_Select(int aircraftCount, bool displayIdle, IdleMaintPhase phase)
{
    if (aircraftCount > 0)
        return IDLE_STATUS_NONE;
    switch (phase) {
    case IDLE_MAINT_PHASE_RUNNING: return IDLE_STATUS_MAINT_RUNNING;
    case IDLE_MAINT_PHASE_DONE_OK: return IDLE_STATUS_MAINT_DONE;
    case IDLE_MAINT_PHASE_DONE_WARN: return IDLE_STATUS_MAINT_WARN;
    case IDLE_MAINT_PHASE_NONE: break;
    }
    return displayIdle ? IDLE_STATUS_STANDBY : IDLE_STATUS_NONE;
}

const char *IdleStatus_Text(IdleStatus s)
{
    switch (s) {
    case IDLE_STATUS_STANDBY:
        return "NO AIRCRAFT DETECTED\nRadar standing by...\nWill resume when aircraft are detected.";
    case IDLE_STATUS_MAINT_RUNNING:
        return "NO AIRCRAFT DETECTED\nPERFORMING MAINTENANCE...\nRadar will resume when aircraft are detected.";
    case IDLE_STATUS_MAINT_DONE:
        return "NO AIRCRAFT DETECTED\nMAINTENANCE COMPLETE\nRadar will resume when aircraft are detected.";
    case IDLE_STATUS_MAINT_WARN:
        return "NO AIRCRAFT DETECTED\nMAINTENANCE COMPLETE WITH WARNINGS\nRadar will resume when aircraft are detected.";
    default:
        return "";
    }
}
