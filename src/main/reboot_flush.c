#include "reboot_flush.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "diag_telemetry.h"
#include "history_manager.h"
#include "seen_aircraft.h"
#include "log_capture.h"

static const char *TAG = "REBOOT";

void RebootFlush_BeforeRestart(RebootFlushResult *out)
{
    RebootFlushResult r = {0};

    /* 1. Existing behaviour: pending Hot Seen changes. */
    r.seenWasDirty = SeenAircraft_IsDirty();
    r.seenOk = r.seenWasDirty ? SeenAircraft_Flush() : true;

    /* 2./3. Pending TF History via the poll task; bounded wait, no lock held. */
    uint32_t seq = HistoryManager_RequestFlush();
    HistoryFlushResult h = {0};
    while (!(r.historyAnswered = HistoryManager_GetFlushResult(seq, &h)) && r.waitedMs < REBOOT_HISTORY_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(REBOOT_HISTORY_POLL_MS));
        r.waitedMs += REBOOT_HISTORY_POLL_MS;
    }
    r.historyOk = r.historyAnswered && h.ok;

    if (!r.historyAnswered) {
        ESP_LOGW(TAG, "History flush not answered within %u ms; rebooting anyway (pending History entries may be lost)",
                 (unsigned)REBOOT_HISTORY_WAIT_MS);
        DiagTelemetry_Event("Reboot: History flush not answered within %u ms", (unsigned)REBOOT_HISTORY_WAIT_MS);
    } else if (!h.ok) {
        ESP_LOGW(TAG, "History flush before reboot incomplete: wrote %u, failed %u, pending %u%s",
                 (unsigned)h.written, (unsigned)h.failed, (unsigned)h.remaining, h.tfUnavailable ? " (TF unavailable)" : "");
        DiagTelemetry_Event("Reboot: History flush incomplete (wrote %u, failed %u, pending %u%s)",
                            (unsigned)h.written, (unsigned)h.failed, (unsigned)h.remaining, h.tfUnavailable ? ", TF unavailable" : "");
    } else {
        ESP_LOGI(TAG, "History flushed before reboot (%u written)", (unsigned)h.written);
    }
    if (!r.seenOk)
        ESP_LOGW(TAG, "Seen flush before reboot failed");

    /* 4. 0.1.8: console-log capture (if active this boot): buffered lines + a clean end marker, bounded wait. */
    LogCapture_Shutdown(REBOOT_LOG_WAIT_MS, "reboot requested");

    if (out)
        *out = r;
}
