#include "web_diag.h"
#include "web_style.h"
#include "feature_flags.h"
#include "fw_version.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "adv_diag.h"
#include "diag_export.h"
#include "aircraft_provider.h"
#include "boot_warmup.h"
#include "expert_debug.h"
#include "airports.h"
#include "custom_rules.h"
#include "diag_telemetry.h"
#include "history_manager.h"
#include "main.h"
#include "opensky_client.h"
#include "seen_aircraft.h"
#include "reboot_flush.h"
#include "tf_history.h"
#include "time_util.h"
#include "north_ref.h"
#include "ui_prefs.h"
#include "universal_value.h"
#include "web_util.h"
#include "log_capture.h"
#include "web_access.h"

/* ---- 0.1.7: one set of core values per /diag request, shared by the HTML page and the exports ----
 * Read once at the start of the request (getter copies, nothing is reset or started). The HTML rows that show
 * these metrics and the JSON "metrics" object both use these values, so the two can never disagree. */
typedef struct {
    int64_t uptimeUs;
    int64_t utc; /* 0 = clock not synchronized */
    uint32_t intFree, intLargest, intMin;
    uint32_t dmaFree, dmaLargest, dmaMin;
    uint32_t psFree, psLargest, psMin;
    uint32_t httpdStackMin;
    uint32_t fails, failSize, failCaps;
    char failTask[20];
    bool failTimeValid;
    int64_t failUs;
    bool minHeapValid[DT_HEAP_COUNT];
    DiagMinHeap minHeap[DT_HEAP_COUNT];
    size_t nStacks;
    DiagMinStack stacks[DT_MAX_STACKS];
    DiagOpStats ops[DT_OP_COUNT];
    int aircraft, aircraftMax;
    TfHistoryStats tf;
    HistoryManagerStats hm;
    SeenPersistStats seen;
    size_t seenCount;
    LogCaptureStatus logcap; /* 0.1.8 console-log capture */
    bool havePoll[AIRCRAFT_PROVIDER_COUNT]; /* 0.1.9 latest provider poll */
    ProviderPollDiag poll[AIRCRAFT_PROVIDER_COUNT];
    WebIf via;               /* interface this request arrived on (log management is station-only) */
} DiagCore;

#define LOG_DL_CHUNK 4096

typedef struct {
    DiagCore core;
    DiagExport ex;
    char disposition[96]; /* must outlive the response headers */
    LogFileInfo logFiles[LOGCAP_FILES_MAX]; /* /diag log list */
    char chunk[LOG_DL_CHUNK];               /* /diag?log= download buffer (PSRAM, never a stack buffer) */
} DiagReportCtx;

static DiagReportCtx *s_ctx;     /* PSRAM, allocated on the first /diag request and kept (no churn) */
static const DiagCore *s_core;   /* valid only while a /diag request is being rendered (single httpd task) */
static DiagExport *s_export;     /* non-NULL while the page body is being converted for an export */

static void DiagCore_Collect(DiagCore *c)
{
    memset(c, 0, sizeof(*c));
    c->uptimeUs = esp_timer_get_time();
    const int64_t now = (int64_t)time(NULL);
    c->utc = TimeUtil_IsSynced(now) ? now : 0;
    c->intFree = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    c->intLargest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    c->intMin = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    c->dmaFree = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    c->dmaLargest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    c->dmaMin = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    c->psFree = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    c->psLargest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    c->psMin = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    c->httpdStackMin = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    c->fails = ExpertDebug_FailedAllocCount();
    if (c->fails) {
        c->failSize = ExpertDebug_LastFailedSize();
        c->failCaps = ExpertDebug_LastFailedCaps();
        snprintf(c->failTask, sizeof(c->failTask), "%s", ExpertDebug_LastFailedTask());
        c->failTimeValid = ExpertDebug_LastFailedUptimeUs(&c->failUs);
    }
    for (int k = 0; k < DT_HEAP_COUNT; k++)
        c->minHeapValid[k] = DiagTelemetry_GetHeap((DiagHeapKind)k, &c->minHeap[k]);
    c->nStacks = DiagTelemetry_GetStacks(c->stacks, DT_MAX_STACKS);
    for (int i = 0; i < DT_OP_COUNT; i++)
        DiagTelemetry_GetOp((DiagOp)i, &c->ops[i]);
    c->aircraft = gAircraftCount;
    c->aircraftMax = GetMaxAircraftCountSinceBoot();
    TfHistory_GetStats(&c->tf);
    HistoryManager_GetStats(&c->hm);
    SeenAircraft_GetPersistStats(&c->seen);
    c->seenCount = SeenAircraft_Count();
    LogCapture_GetStatus(&c->logcap);
    for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++)
        c->havePoll[p] = ProviderDiag_GetPoll((AircraftProviderType)p, &c->poll[p]);
}

/* 0.1.9: a stamp for an uptime second recorded earlier (wall time derived from the snapshot's clock). */
static DiagStamp CoreStampAt(const DiagCore *c, uint32_t uptimeS)
{
    const uint32_t nowS = (uint32_t)(c->uptimeUs / 1000000);
    DiagStamp st = {uptimeS, 0};
    if (c->utc > 0 && nowS >= uptimeS)
        st.utc = c->utc - (int64_t)(nowS - uptimeS);
    return st;
}

/* Same rule as DiagTelemetry_NeedsAttention(), on the request's snapshot. */
static bool CoreNeedsAttention(const DiagCore *c)
{
    for (int i = 0; i < DT_OP_COUNT; i++)
        if (c->ops[i].consecutiveFailures >= DT_ATTENTION_CONSECUTIVE)
            return true;
    return false;
}

/* Every byte of the page goes through here: to the browser, or (export) into the converter. */
static esp_err_t Emit(httpd_req_t *req, const char *data, size_t len)
{
    if (s_export)
        return DiagExport_Feed(s_export, data, len) ? ESP_OK : ESP_FAIL;
    return httpd_resp_send_chunk(req, data, (ssize_t)len);
}

static esp_err_t Send(httpd_req_t *req, const char *value)
{
    return Emit(req, value, strlen(value));
}

static esp_err_t SendFormat(httpd_req_t *req, const char *format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length < 0 || length >= (int)sizeof(buffer))
        return ESP_FAIL;
    return Emit(req, buffer, (size_t)length);
}

/* Defensive default case: esp_reset_reason_t has gained new members across
 * ESP-IDF versions (USB, JTAG, EFUSE, power-glitch, CPU-lockup, ...) and
 * this project's exact IDF version's full enum wasn't independently
 * confirmed here - only the long-stable, common causes are named; anything
 * else still reports usefully rather than risking referencing an enumerator
 * that might not exist in this build. */
static const char *ResetReasonName(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software (esp_restart)";
    case ESP_RST_PANIC: return "panic/exception";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "other/unknown (see esp_reset_reason_t)";
    }
}

/* One <tr> of the report. value/evidence are both plain text, never come
 * from user input, so no escaping is needed here (unlike web_seen.c etc). */
/* ---- 0.0.29: application partition / storage usage ----
 *
 * The app image size is what ESP-IDF itself reads from the image on flash
 * (esp_image_get_metadata: headers only, no verify, no load): image_len is the
 * image length including the checksum and the appended SHA-256, i.e. the
 * flashed .bin size. It cannot change while the firmware runs, so it is read
 * once (httpd task only) and cached until the next reboot/reflash. */
typedef struct {
    bool tried, haveImage;
    uint32_t partAddr, partSize, imageLen;
    char label[17];
} AppImageInfo;
static AppImageInfo s_appImage;

static const AppImageInfo *AppImage(void)
{
    if (s_appImage.tried)
        return &s_appImage;
    s_appImage.tried = true;
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (!part)
        return &s_appImage;
    s_appImage.partAddr = part->address;
    s_appImage.partSize = part->size;
    snprintf(s_appImage.label, sizeof(s_appImage.label), "%s", part->label);
    const esp_partition_pos_t pos = {.offset = part->address, .size = part->size};
    esp_image_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    if (esp_image_get_metadata(&pos, &meta) == ESP_OK && meta.image_len > 0 && meta.image_len <= part->size) {
        s_appImage.imageLen = meta.image_len;
        s_appImage.haveImage = true;
    }
    return &s_appImage;
}

/* used / total as two percentages with one decimal, rounded once (used) so the
 * pair always adds up to exactly 100.0%. "n/a" when it cannot be computed
 * (total 0, or used > total): never a division by zero or a negative free. */
static void FormatUsagePercents(uint64_t used, uint64_t total, char *usedOut, char *freeOut, size_t cap)
{
    if (total == 0 || used > total) {
        snprintf(usedOut, cap, "n/a");
        snprintf(freeOut, cap, "n/a");
        return;
    }
    const unsigned usedTenths = (unsigned)((used * 1000u + total / 2u) / total);
    const unsigned freeTenths = 1000u - usedTenths;
    snprintf(usedOut, cap, "%u.%u%%", usedTenths / 10u, usedTenths % 10u);
    snprintf(freeOut, cap, "%u.%u%%", freeTenths / 10u, freeTenths % 10u);
}

/* 0.1.6: "<count>" plus when the latest failure happened. Distinguishes "none since boot" from a
 * failure whose time could not be read. Read-only: nothing is reset by viewing the page. */
/* 0.1.7: the wall-clock time of the last failure, derived from the snapshot (0 = not available). */
static int64_t CoreFailUtc(const DiagCore *c)
{
    if (!c->fails || !c->failTimeValid || !c->utc || c->uptimeUs < c->failUs)
        return 0;
    return c->utc - (c->uptimeUs - c->failUs) / 1000000;
}

static void FailedAllocText(char *buf, size_t cap)
{
    const uint32_t fails = s_core->fails;
    const int64_t failUs = s_core->failUs;
    if (fails == 0) {
        snprintf(buf, cap, "0 (none since boot)");
    } else if (!s_core->failTimeValid) {
        snprintf(buf, cap, "%u; time of the last failure unavailable", (unsigned)fails);
    } else {
        DiagStamp st = {(uint32_t)(failUs / 1000000), CoreFailUtc(s_core)};
        char when[64];
        DiagTelemetry_FormatStampLocal(&st, when, sizeof(when));
        snprintf(buf, cap, "%u; last at %s", (unsigned)fails, when);
    }
}

static esp_err_t Row(httpd_req_t *req, const char *label, const char *value, const char *evidence)
{
    return SendFormat(req,
        "<tr><td>%s</td><td>%s</td><td class='ev'>%s</td></tr>",
        label, value, evidence);
}

/* Last-run timing of one recurring operation, shown in that operation's own section (0.0.31).
 * Reads the existing DiagTelemetry record only: finish/worst stamps, durations, result; start is
 * derived (finish - duration). "Last success" appears only while the latest run is a failure. */
static esp_err_t OpTimingRows(httpd_req_t *req, DiagOp op, const char *lastLabel, bool withCounts)
{
    const DiagOpStats o = s_core->ops[op];
    char buf[360]; /* room for an escaped failure reason (up to 6x its length) plus the fixed text */
    if (withCounts) {
        snprintf(buf, sizeof(buf), "%u / %u / %u", (unsigned)o.runs, (unsigned)o.failures, (unsigned)o.skipped);
        if (Row(req, "Runs / failed / skipped (since boot)", buf, "Measured (DiagTelemetry; skipped = could not start)") != ESP_OK) return ESP_FAIL;
    }
    if (o.runs == 0)
        return Row(req, lastLabel, "none since boot", "Measured (DiagTelemetry)");
    DiagTelemetry_FormatRunHtml(&o.lastRun, o.lastMs, buf, sizeof(buf));
    if (Row(req, lastLabel, buf, "Measured: finish stamped when the run ended; start = finish - duration (to the second)") != ESP_OK) return ESP_FAIL;
    const bool failed = o.consecutiveFailures > 0;
    if (failed) {
        char esc[DT_REASON_MAX * 6];
        WebUtil_EscapeHtml(esc, sizeof(esc), o.lastFailure);
        snprintf(buf, sizeof(buf), "<b style='color:#c00'>FAILED</b> (%s), %u in a row", esc, (unsigned)o.consecutiveFailures);
    } else {
        snprintf(buf, sizeof(buf), "OK");
    }
    if (Row(req, "Result", buf, "Measured (result of the run above)") != ESP_OK) return ESP_FAIL;
    if (failed) {
        if (o.haveOk)
            DiagTelemetry_FormatStampLocal(&o.lastOkWhen, buf, sizeof(buf));
        else
            snprintf(buf, sizeof(buf), "none since boot");
        if (Row(req, "Last success", buf, "Measured (finish time of the most recent successful run)") != ESP_OK) return ESP_FAIL;
    }
    DiagTelemetry_FormatRunHtml(&o.worstWhen, o.worstMs, buf, sizeof(buf));
    return Row(req, "Worst run", buf, "Measured (longest run since boot: start &rarr; finish, duration)");
}

/* Collapsible group (<details>). `open` = initially expanded. Deep links such as
 * /diag#tf still work: the script at the end of the page opens the group that
 * contains the target anchor. No extra RAM: the markup is streamed. */
static esp_err_t GroupOpen(httpd_req_t *req, const char *id, const char *title, bool open)
{
    return SendFormat(req, "<details class='gp' id='g-%s'%s><summary>%s</summary>", id, open ? " open" : "", title);
}

static esp_err_t GroupClose(httpd_req_t *req)
{
    return Send(req, "</details>");
}

static esp_err_t SendDiagNotice(httpd_req_t *req, const char *anchor, const char *text);
static esp_err_t LogForbidden(httpd_req_t *req, WebIf via);

// ---- Debugging (serial-log switches; take effect immediately, persisted in NVS) ----
// One place for both OpenSky field logging and provider diagnostics (they used to live
// on the setup page). The form always posts both values, so an absent checkbox means off.
static esp_err_t DebugSection(httpd_req_t *req)
{
    ProviderDebugLevel lvl = AircraftProvider_GetDebugLevel();
    char form[900];
    int n = snprintf(form, sizeof(form),
        "<h3 id='debug'>Debugging</h3>"
        "<form method='POST' action='/diag/advanced'>"
        "<input type='hidden' name='mode' value='debug'>"
        "<label><input name='osdbg' type='checkbox' value='1'%s> Log raw OpenSky fields for the selected aircraft</label>"
        "<p class='ev'>Dumps every field OpenSky returns for the Selected Craft aircraft to the serial console on each poll.</p>"
        "<label>Provider diagnostics <select name='pdbg'>"
        "<option value='OFF'%s>Off</option><option value='NORMAL'%s>Normal</option>"
        "<option value='VERBOSE'%s>Verbose</option><option value='RAW'%s>Raw</option></select></label>",
        GetRadarOpenSkyDebugEnabled() ? " checked" : "",
        lvl == PROVIDER_DEBUG_OFF ? " selected" : "", lvl == PROVIDER_DEBUG_NORMAL ? " selected" : "",
        lvl == PROVIDER_DEBUG_VERBOSE ? " selected" : "", lvl == PROVIDER_DEBUG_RAW ? " selected" : "");
    if (n <= 0 || n >= (int)sizeof(form))
        return ESP_FAIL;
    if (Send(req, form) != ESP_OK)
        return ESP_FAIL;
    return Send(req,
        "<p class='ev'>Off: no optional provider output. Normal: request start/done lines with request number and "
        "elapsed time, response status and counts, request failures (timeout, connect/TLS with the esp-tls code, "
        "incomplete, truncated, HTTP status) and the adsb.lol heap figures before its TLS session. Verbose: adds "
        "per-aircraft type resolution, request detail and the parse stages (malformed, no id, no position, bad "
        "position, on ground, not examined). Raw: adds a bounded, credential-redacted response preview and up to 8 "
        "record samples per poll. Applies to every enabled provider; the latest poll of each is also under "
        "<a href='#provpoll'>Provider last poll</a>, whatever the level.</p>"
        "<button type='submit'>Save debugging settings</button></form>");
}

// ---- Expert Debug Mode (forensic; persistent NVS toggle, effective after reboot) ----
static esp_err_t ExpertSection(httpd_req_t *req)
{
    bool active = ExpertDebug_Active();
    bool saved = ExpertDebug_GetSaved();
    char buf[64];

    if (Send(req, "<h3 id='expert'>Expert Debug Mode (forensic diagnostics)</h3>"
                  "<p><b>For forensic debugging only - leave OFF in normal use.</b> Installs hooks that run "
                  "inside the heap allocator and print directly to the serial console (tag FAILED_ALLOC: every "
                  "failed allocation with size, requested capabilities, task and backtrace). Independent of "
                  "Advanced Diagnostics; takes effect after a reboot.</p>"
                  "<table><tr><th>Item</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    if (Row(req, "Active this boot", active ? "ON" : "OFF", "Read once from NVS at boot") != ESP_OK) return ESP_FAIL;
    if (Row(req, "Saved setting (next boot)", saved ? "ON" : "OFF",
            saved != active ? "NVS - reboot required to apply" : "NVS") != ESP_OK) return ESP_FAIL;
    if (active)
    {
        FailedAllocText(buf, sizeof(buf));
        if (Row(req, "Failed heap allocations since boot", buf,
                "Counted by the FAILED_ALLOC hook (details on the serial console); time as in Memory / Resources") != ESP_OK) return ESP_FAIL;
    }
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    char form[420];
    snprintf(form, sizeof(form),
             "<form method='POST' action='/diag/advanced'>"
             "<input type='hidden' name='mode' value='expert'>"
             "<input type='hidden' name='enable' value='%d'>"
             "<button type='submit' name='reboot' value='0'>%s Expert Debug Mode (reboot later)</button> "
             "<button type='submit' name='reboot' value='1'>%s and reboot now</button>"
             "</form>",
             saved ? 0 : 1, saved ? "Disable" : "Enable", saved ? "Disable" : "Enable");
    return Send(req, form);
}

// ---- Advanced Diagnostics (persistent NVS toggle, effective after reboot) ----
// The toggle only rewrites NVS; the active state is fixed at boot so the
// memory behaviour being measured never changes mid-run.
static esp_err_t AdvancedSection(httpd_req_t *req)
{
    char form[400];
    bool active = AdvDiag_Active();
    bool saved = AdvDiag_GetSaved();

    if (Send(req, "<h3 id='advanced'>Advanced Diagnostics</h3>"
                  "<p>Heap-topology, allocation and TLS-lifecycle instrumentation (serial log tags ADVDIAG, "
                  "HDIAG, PCBTRACE, AOT). Changing it takes effect after a reboot, so a run is never measured "
                  "with the setting switched halfway through.</p>"
                  "<table><tr><th>Item</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    if (Row(req, "Active this boot", active ? "ON" : "OFF", "Read once from NVS at boot") != ESP_OK) return ESP_FAIL;
    if (Row(req, "Saved setting (next boot)", saved ? "ON" : "OFF",
            saved != active ? "NVS - reboot required to apply" : "NVS") != ESP_OK) return ESP_FAIL;
    if (Row(req, "Allocation-owner trace", AdvDiag_AllocTraceCompiled() ? (active ? "compiled in, armed per window" : "compiled in, idle")
                                                                          : "not compiled in",
            "Compile-time option CONFIG_FR_ALLOC_TRACE (default off)") != ESP_OK) return ESP_FAIL;
    if (Row(req, "Crypto warm-up (boot)", BootWarmup_StatusText(BootWarmup_CryptoStatus()),
            "PSA AES-CTR on PSRAM + secp256r1 keygen right after nvs_flash_init") != ESP_OK) return ESP_FAIL;
    if (Row(req, "RadarTask network warm-up", BootWarmup_StatusText(BootWarmup_NetStatus()),
            "getaddrinfo(\"127.0.0.1\") in RadarTask before its first fetch") != ESP_OK) return ESP_FAIL;
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    if (active)
    {
        AdvDiagCheckpoint cps[16];
        size_t n = AdvDiag_GetCheckpoints(cps, 16);
        if (Send(req, "<h4>Heap checkpoints (most recent 16)</h4><table><tr><th>Stage</th><th>Uptime</th>"
                      "<th>Internal free</th><th>Internal largest</th><th>DMA free</th><th>DMA largest</th><th>PSRAM free</th></tr>") != ESP_OK)
            return ESP_FAIL;
        for (size_t i = 0; i < n; i++)
        {
            char esc[96];
            WebUtil_EscapeHtml(esc, sizeof(esc), cps[i].stage ? cps[i].stage : "?");
            if (SendFormat(req, "<tr><td>%s</td><td>%u ms</td><td>%u</td><td>%u</td><td>%u</td><td>%u</td><td>%u</td></tr>",
                           esc, (unsigned)cps[i].uptimeMs, (unsigned)cps[i].internalFree,
                           (unsigned)cps[i].internalLargest, (unsigned)cps[i].dmaFree, (unsigned)cps[i].dmaLargest,
                           (unsigned)cps[i].psramFree) != ESP_OK)
                return ESP_FAIL;
        }
        AdvDiagFreeBlock fb[24];
        size_t nf = AdvDiag_GetDmaFreeBlocks(fb, 24, 1024);
        if (Send(req, "</table><h4>DMA-capable internal free blocks &ge; 1 KB (now)</h4>"
                      "<table><tr><th>Address</th><th>Size</th></tr>") != ESP_OK)
            return ESP_FAIL;
        for (size_t i = 0; i < nf; i++)
            if (SendFormat(req, "<tr><td>0x%08x</td><td>%u</td></tr>", (unsigned)fb[i].addr, (unsigned)fb[i].size) != ESP_OK)
                return ESP_FAIL;
        if (Send(req, "</table>") != ESP_OK)
            return ESP_FAIL;
    }

    snprintf(form, sizeof(form),
             "<form method='POST' action='/diag/advanced'>"
             "<input type='hidden' name='mode' value='adv'>"
             "<input type='hidden' name='enable' value='%d'>"
             "<button type='submit' name='reboot' value='0'>%s Advanced Diagnostics (reboot later)</button> "
             "<button type='submit' name='reboot' value='1'>%s and reboot now</button>"
             "</form>",
             saved ? 0 : 1, saved ? "Disable" : "Enable", saved ? "Disable" : "Enable");
    if (Send(req, form) != ESP_OK)
        return ESP_FAIL;
    return ExpertSection(req);
}

static esp_err_t AdvancedPost(httpd_req_t *req)
{
    char body[96] = {0};
    int total = 0;
    if (req->content_len >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form too large");
    while (total < (int)sizeof(body) - 1 && total < (int)req->content_len)
    {
        int got = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (got <= 0)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Could not read form");
        total += got;
    }
    body[total] = '\0';

    char val[16];
    if (httpd_query_key_value(body, "mode", val, sizeof(val)) == ESP_OK && strcmp(val, "debug") == 0)
    {
        // Both debug switches live in this one form; an absent checkbox means off.
        SetRadarOpenSkyDebugEnabled(httpd_query_key_value(body, "osdbg", val, sizeof(val)) == ESP_OK && val[0] == '1');
        ProviderDebugLevel level;
        if (httpd_query_key_value(body, "pdbg", val, sizeof(val)) == ESP_OK && ProviderDebugLevel_Parse(val, &level))
            AircraftProvider_SetDebugLevel(level);
        return SendDiagNotice(req, "debug", "Debugging settings saved (effective immediately).");
    }
    bool enable = httpd_query_key_value(body, "enable", val, sizeof(val)) == ESP_OK && val[0] == '1';
    bool reboot = httpd_query_key_value(body, "reboot", val, sizeof(val)) == ESP_OK && val[0] == '1';
    // Same endpoint serves all persistent diagnostic modes; no extra httpd slot.
    char mode[16] = "";
    (void)httpd_query_key_value(body, "mode", mode, sizeof(mode));
    const bool expert = strcmp(mode, "expert") == 0;
    const bool logcap = strcmp(mode, "logcap") == 0; /* 0.1.8: save console logs to TF card */
    const char *modeName = logcap ? "Save Console Logs to TF Card" : expert ? "Expert Debug Mode" : "Advanced Diagnostics";
    const char *anchor = logcap ? "logs" : expert ? "expert" : "advanced";
    if (logcap) {
        const WebIf via = WebAccess_RequestInterface(req);
        if (via != WEBIF_STATION)
            return LogForbidden(req, via);
    }

    if ((logcap ? LogCapture_SetSaved(enable) : expert ? ExpertDebug_SetSaved(enable) : AdvDiag_SetSaved(enable)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save setting");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char refresh[80] = "";
    if (reboot)
        snprintf(refresh, sizeof(refresh), "<meta http-equiv='refresh' content='20;url=/diag#%s'>", anchor);
    char page[896];
    snprintf(page, sizeof(page),
             "<!doctype html><html%s><head><meta name='viewport' content='width=device-width,initial-scale=1'>%s"
             "<title>Diagnostics</title><style>" WEBSTYLE_MINI_CSS "</style></head>"
             "<body style='font:16px sans-serif;max-width:750px;margin:1em auto;padding:0 1em'>"
             "<p>%s saved as <b>%s</b>. %s</p><p><a href='/diag#%s'>Back to diagnostics</a></p>"
             "</body></html>",
             WebStyle_HtmlAttr(), refresh, modeName, enable ? "ON" : "OFF",
             reboot ? "Rebooting now; this page returns to /diag in about 20 seconds."
                    : "Reboot required: the current run keeps its existing setting until the next restart.",
             anchor);
    httpd_resp_sendstr(req, page);

    if (reboot)
    {
        // Same order as the other deliberate restarts in webserver.c:
        // let the response go out, flush pending Seen + TF History (bounded), restart.
        vTaskDelay(pdMS_TO_TICKS(500));
        RebootFlush_BeforeRestart(NULL);
        esp_restart();
    }
    return ESP_OK;
}

// Small "saved" page that links back to /diag#anchor (no reboot involved).
static esp_err_t SendDiagNotice(httpd_req_t *req, const char *anchor, const char *text)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char page[800];
    snprintf(page, sizeof(page),
             "<!doctype html><html%s><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<meta http-equiv='refresh' content='2;url=/diag#%s'>"
             "<title>Diagnostics</title><style>" WEBSTYLE_MINI_CSS "</style></head>"
             "<body style='font:16px sans-serif;max-width:750px;margin:1em auto;padding:0 1em'>"
             "<p>%s</p><p><a href='/diag#%s'>Back to diagnostics</a></p></body></html>",
             WebStyle_HtmlAttr(), anchor, text, anchor);
    return httpd_resp_sendstr(req, page);
}

// ---- TF diagnostics (see tf_history.h "TF diagnostics") ----
static const char *ResText(uint8_t r)
{
    switch (r)
    {
    case TF_RES_OK: return "OK";
    case TF_RES_FAIL: return "<b>FAILED</b>";
    case TF_RES_SKIPPED: return "skipped";
    default: return "not run";
    }
}

static const char *FsTypeText(uint8_t t)
{
    switch (t)
    {
    case 1: return "FAT12";
    case 2: return "FAT16";
    case 3: return "FAT32";
    case 4: return "exFAT";
    default: return "unknown (not read)";
    }
}

static const char *IndexStateText(uint8_t st)
{
    switch (st)
    {
    case TF_INDEX_LOADED: return "loaded (committed indexv2.dat, slots counted)";
    case TF_INDEX_REBUILT: return "rebuilt from all bucket files (index was missing, unfinished or invalid)";
    case TF_INDEX_MIGRATED: return "MIGRATED from the legacy 4096-slot index.dat (left untouched)";
    case TF_INDEX_FRESH: return "created new (no index and no history yet)";
    case TF_INDEX_FAILED: return "FAILED (new index could not be built, verified or committed; history data untouched)";
    default: return "not initialized";
    }
}

// One row for a step/stage: value = result, evidence = the raw codes.
static esp_err_t StepRow(httpd_req_t *req, const char *label, const TfStepInfo *s)
{
    char ev[160];
    if (s->result == TF_RES_NOT_RUN || s->result == TF_RES_SKIPPED)
        snprintf(ev, sizeof(ev), "-");
    else
        snprintf(ev, sizeof(ev), "esp_err=%s (%d) errno=%d fatfs=%u time=%u us",
                 esp_err_to_name((esp_err_t)s->espErr), (int)s->espErr, (int)s->errnoVal,
                 (unsigned)s->fatfsErr, (unsigned)s->us);
    return Row(req, label, ResText(s->result), ev);
}

static esp_err_t TfSection(httpd_req_t *req)
{
    const TfHistoryStats tf = s_core->tf;
    TfInitInfo in;
    TfHistory_GetInitInfo(&in);
    const HistoryManagerStats hm = s_core->hm;
    char buf[160];
    char esc[192];

    if (Send(req, "<h3 id='tf'>TF history (persistent, warm layer under Hot Seen)</h3>") != ESP_OK)
        return ESP_FAIL;
    if (tf.indexLoadState == TF_LOAD_CAP &&
        Send(req, "<p class='er'>History index is at its hard cap: NEW aircraft are no longer written to History "
                  "(they remain in Hot Seen). Existing aircraft keep updating.</p>") != ESP_OK)
        return ESP_FAIL;
    if (tf.indexLoadState == TF_LOAD_WARN &&
        Send(req, "<p class='wn'>History index is above 70% full.</p>") != ESP_OK)
        return ESP_FAIL;
    if (Send(req, "<table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    if (Row(req, "TF card mounted (filesystem)", tf.mounted ? "yes" : "no",
            "Measured (esp_vfs_fat_sdspi_mount result; cleared by a diagnostic unmount). Says nothing about the history layer - see the next rows") != ESP_OK) return ESP_FAIL;
    if (Row(req, "Filesystem", tf.mounted ? FsTypeText(in.fsType) : "-", "Measured (FATFS fs_type via f_getfree)") != ESP_OK) return ESP_FAIL;
    if (tf.mounted)
    {
        // Own (larger) buffer: three %llu fields size against uint64's full range for -Werror=format-truncation.
        char tfSpaceBuf[128];
        snprintf(tfSpaceBuf, sizeof(tfSpaceBuf), "%llu / %llu bytes (%llu free)",
                 (unsigned long long)tf.usedBytes, (unsigned long long)tf.capacityBytes, (unsigned long long)tf.freeBytes);
        if (Row(req, "TF used / capacity", tfSpaceBuf, "Measured (f_getfree)") != ESP_OK) return ESP_FAIL;
    }
    snprintf(buf, sizeof(buf), "%s (%s)", in.historyDir ? in.historyDir : "?",
             in.dirExisted ? "existed at init" : ResText(in.stage[TF_STAGE_HIST_DIR].result));
    if (Row(req, "History path", buf, "Existing constant TF_HISTORY_DIR; init stat()s it, else mkdir()s it (stage history_dir below)") != ESP_OK) return ESP_FAIL;

    if (!in.attempted)
        snprintf(buf, sizeof(buf), "not attempted");
    else if (!in.ok)
        snprintf(buf, sizeof(buf), "FAILED at stage %s", in.firstFailedStage >= 0 ? TfHistory_StageName(in.firstFailedStage) : "?");
    else if (in.firstFailedStage >= 0)
        snprintf(buf, sizeof(buf), "OK, but stage %s recorded a failure", TfHistory_StageName(in.firstFailedStage));
    else
        snprintf(buf, sizeof(buf), "OK");
    if (Row(req, "History init", buf, "Measured (TfHistory_Init / diagnostic reinit; per-stage detail below)") != ESP_OK) return ESP_FAIL;
    if (Row(req, "History available (writes accepted)", tf.historyAvailable ? "yes" : "no", "Measured (TfHistory_IsAvailable)") != ESP_OK) return ESP_FAIL;
    if (Row(req, "History Manager TF-ready", HistoryManager_IsTfReady() ? "yes" : "no (no TF writes/lookups are made)", "Measured (set from the init result; paused during unmount)") != ESP_OK) return ESP_FAIL;

    char idxState[200];
    if (in.indexSizeBytes >= 0)
        snprintf(idxState, sizeof(idxState), "%s; indexv2.dat %d B (expected %u B)", IndexStateText(in.indexState), (int)in.indexSizeBytes, (unsigned)in.indexSizeExpected);
    else
        snprintf(idxState, sizeof(idxState), "%s; indexv2.dat not found at init (expected %u B)", IndexStateText(in.indexState), (unsigned)in.indexSizeExpected);
    if (Row(req, "Index state", idxState, "Measured (header + size check of indexv2.dat at init; format v2 = 32 B header + 21 B slots)") != ESP_OK) return ESP_FAIL;
    if (in.legacyIndexSizeBytes >= 0)
        snprintf(buf, sizeof(buf), "present, %d B, never modified or deleted by this firmware", (int)in.legacyIndexSizeBytes);
    else
        snprintf(buf, sizeof(buf), "not present");
    if (Row(req, "Legacy index.dat (4096 slots)", buf, "Measured (stat at init). Kept as-is after the migration; cleanup is a separate decision") != ESP_OK) return ESP_FAIL;
    {
        const unsigned permille = tf.indexSlotsTotal ? (unsigned)(((uint64_t)tf.indexSlotsUsed * 1000u) / tf.indexSlotsTotal) : 0;
        const char *ls = tf.indexLoadState == TF_LOAD_CAP ? "<b>FULL (hard cap)</b>" : tf.indexLoadState == TF_LOAD_WARN ? "<b>WARN</b>" : "OK";
        char slotBuf[120];
        snprintf(slotBuf, sizeof(slotBuf), "%u / %u (%u.%u%%) - %s", (unsigned)tf.indexSlotsUsed, (unsigned)tf.indexSlotsTotal,
                 permille / 10u, permille % 10u, ls);
        if (Row(req, "Index slots used / total (load)", slotBuf,
                in.indexState == TF_INDEX_NOT_INIT ? "0 here means the index was NOT initialized (not that it is empty)"
                                                   : "Distinct aircraft in the index: counted at load, +1 per new aircraft. 0 = genuinely empty") != ESP_OK) return ESP_FAIL;
        snprintf(slotBuf, sizeof(slotBuf), "WARN at %u slots (70%%), hard cap at %u slots (90%%)", (unsigned)tf.indexWarnAt, (unsigned)tf.indexHardCap);
        if (Row(req, "Index limits", slotBuf, "Fixed design limits. At the hard cap NEW aircraft are not written to History (they stay in Hot Seen); nothing is appended, so no orphan or duplicate records") != ESP_OK) return ESP_FAIL;
        snprintf(slotBuf, sizeof(slotBuf), "%u refused at cap / %u slot-write failures / %u orphans reused",
                 (unsigned)tf.indexInsertCapRejected, (unsigned)tf.indexInsertIoFail, (unsigned)tf.orphanAdopted);
        if (Row(req, "Index insert problems", slotBuf, "Measured. 'refused at cap' counts attempts (the History Manager retries each flush), not distinct aircraft") != ESP_OK) return ESP_FAIL;
        const unsigned long long hitAvg100 = tf.probeHitOps ? (tf.probeHitTotal * 100ull) / tf.probeHitOps : 0;
        const unsigned long long missAvg100 = tf.probeMissOps ? (tf.probeMissTotal * 100ull) / tf.probeMissOps : 0;
        snprintf(slotBuf, sizeof(slotBuf), "found %llu.%02llu / not found %llu.%02llu avg; max %u, last %u",
                 hitAvg100 / 100ull, hitAvg100 % 100ull, missAvg100 / 100ull, missAvg100 % 100ull,
                 (unsigned)tf.probeMax, (unsigned)tf.probeLast);
        if (Row(req, "Index probes (slots read per operation)", slotBuf, "Measured on radar Lookup/Upsert (page browsing is not counted). Average near 1-2 is healthy") != ESP_OK) return ESP_FAIL;
        snprintf(slotBuf, sizeof(slotBuf), "1: %u, 2-3: %u, 4-7: %u, 8-15: %u, 16-31: %u, 32+: %u",
                 (unsigned)tf.probeHist[0], (unsigned)tf.probeHist[1], (unsigned)tf.probeHist[2],
                 (unsigned)tf.probeHist[3], (unsigned)tf.probeHist[4], (unsigned)tf.probeHist[5]);
        if (Row(req, "Index probe histogram", slotBuf, "Measured (operations by number of slots read)") != ESP_OK) return ESP_FAIL;
    }
    if (in.stackFreeMinBytes >= 0) {
        snprintf(buf, sizeof(buf), "%d bytes", (int)in.stackFreeMinBytes);
        if (Row(req, "Init task stack, minimum free", buf, "Measured at the end of the last init, for the task that ran it (app_main has only 3584 B at boot)") != ESP_OK) return ESP_FAIL;
    }
    if (OpTimingRows(req, DT_OP_TF_FLUSH, "Last History flush", true) != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)tf.writes);
    if (Row(req, "Writes since boot (creates + updates)", buf, "Measured") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u / %u", (unsigned)tf.creates, (unsigned)tf.updates);
    if (Row(req, "  creates / updates", buf, "Measured") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u / %u", (unsigned)tf.lookups, (unsigned)tf.lookupMisses);
    if (Row(req, "Lookups / misses", buf, "Measured") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)tf.errors);
    if (Row(req, "I/O or CRC errors handled", buf, "Measured (a corrupt record is treated as absent and self-heals on next write). Not counted while history is unavailable") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)tf.recoveryOps);
    if (Row(req, "Corrupt records skipped during scans", buf, "Measured") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)tf.indexRebuilds);
    if (Row(req, "Index builds since boot", buf, "Measured (migration, rebuild or first create of indexv2.dat)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u us / %u us / %u us", (unsigned)tf.lastLookupUs, (unsigned)tf.bestLookupUs, (unsigned)tf.worstLookupUs);
    if (Row(req, "Lookup time: last / best / worst", buf, "Measured (esp_timer_get_time around the index probe + record read)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u us / %u us / %u us", (unsigned)tf.lastWriteUs, (unsigned)tf.bestWriteUs, (unsigned)tf.worstWriteUs);
    if (Row(req, "Write time: last / best / worst", buf, "Measured (esp_timer_get_time around the record + header write)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u us / %u us / %u us", (unsigned)tf.lastRecoveryUs, (unsigned)tf.bestRecoveryUs, (unsigned)tf.worstRecoveryUs);
    if (Row(req, "Index build time: last / best / worst", buf, "Measured") != ESP_OK) return ESP_FAIL;

    WebUtil_EscapeHtml(esc, sizeof(esc), in.lastError[0] ? in.lastError : "none");
    if (Row(req, "Last init error", esc, "First failing stage of the most recent init, with esp_err / errno / FATFS code") != ESP_OK) return ESP_FAIL;
    WebUtil_EscapeHtml(esc, sizeof(esc), in.lastRuntimeError[0] ? in.lastRuntimeError : "none");
    if (Row(req, "Last runtime error (Upsert/Lookup)", esc, "Best-effort errno at the failing call") != ESP_OK) return ESP_FAIL;

    snprintf(buf, sizeof(buf), "%u / %u (%u dirty)", (unsigned)hm.shadowSlotsUsed, (unsigned)hm.shadowSlotsCap, (unsigned)hm.dirtyNow);
    if (Row(req, "History Manager RAM shadow slots / cap", buf, "Measured / Calculated (one slot per currently-tracked aircraft, not the archive)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u clean, %u after a successful write", (unsigned)hm.evictClean, (unsigned)hm.evictAfterWrite);
    if (Row(req, "History Manager slot reuse (since boot)", buf,
            "Counted (0.1.5). A full shadow table reuses the least-recently-seen slot whose state is already on the card; "
            "only if every slot has unsaved changes is the oldest written first. Unsaved history is never dropped") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%s%u failed write%s, %u new aircraft not admitted%s",
             (hm.evictWriteFail || hm.admitSkipped) ? "ATTENTION: " : "", (unsigned)hm.evictWriteFail,
             hm.evictWriteFail == 1 ? "" : "s", (unsigned)hm.admitSkipped, hm.evictWriteBlocked ? " (waiting for the next flush)" : "");
    if (Row(req, "History pending protection (since boot)", buf,
            "Counted (0.1.5). All slots held unsaved changes and the card could not take the oldest (write error, full, "
            "paused or unavailable): every pending record was kept for the next flush and the new aircraft was left to "
            "Hot Seen until a slot frees. 0 = never happened") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)(hm.shadowSlotsCap * (unsigned)(sizeof(TfHistoryRecord) + 16)));
    if (Row(req, "History Manager allocation (PSRAM heap, approx.)", buf, "Calculated") != ESP_OK) return ESP_FAIL;
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    // ---- init stages ----
    snprintf(buf, sizeof(buf), "<h3>TF init stages (init run #%u)</h3>"
                               "<table><tr><th>Stage</th><th>Result</th><th>Codes</th></tr>", (unsigned)in.runs);
    if (Send(req, buf) != ESP_OK)
        return ESP_FAIL;
    for (int i = 0; i < TF_STAGE_COUNT; i++)
        if (StepRow(req, TfHistory_StageName(i), &in.stage[i]) != ESP_OK)
            return ESP_FAIL;
    if (Send(req, "</table><p class='ev'>index_check: errno=2 with result OK means indexv2.dat was missing (normal on a fresh card or the first boot after the upgrade). "
                  "spi_bus_init OK with esp_err=ESP_ERR_INVALID_STATE means the bus was already initialized (tolerated). "
                  "index_scan/index_write/index_verify/index_commit only run when there is no valid index (migration, rebuild or first create); "
                  "index_count only when a committed index was loaded.</p>") != ESP_OK)
        return ESP_FAIL;

    // ---- last index build (migration / rebuild / first create) ----
    TfIndexBuildReport br;
    TfHistory_GetIndexBuildReport(&br);
    if (br.ran)
    {
        const char *mode = br.mode == TF_BUILD_MIGRATE ? "migration from the legacy 4096-slot index" : br.mode == TF_BUILD_REBUILD ? "rebuild from bucket files" : "first create";
        char eb[200];
        WebUtil_EscapeHtml(eb, sizeof(eb), br.error[0] ? br.error : "none");
        snprintf(buf, sizeof(buf), "<h3>TF index build: %s - %s</h3><table><tr><th>Item</th><th>Value</th><th>Evidence</th></tr>",
                 mode, br.ok ? "committed" : "<b>NOT committed</b>");
        if (Send(req, buf) != ESP_OK)
            return ESP_FAIL;
        char v[160];
        snprintf(v, sizeof(v), "%s%s", br.usedStaging ? "PSRAM staging" : "on-disk fallback", br.usedStaging ? "" : " (staging allocation failed)");
        if (Row(req, "Method", v, "Transient PSRAM table is freed after the build; nothing is kept in RAM") != ESP_OK) return ESP_FAIL;
        if (br.usedStaging) {
            snprintf(v, sizeof(v), "%u bytes (transient)", (unsigned)br.stagingBytes);
            if (Row(req, "Staging memory", v, "Measured (PSRAM, released at the end of the build)") != ESP_OK) return ESP_FAIL;
        }
        snprintf(v, sizeof(v), "%u bucket files, %u records scanned, %u corrupt/unverifiable skipped", (unsigned)br.buckets, (unsigned)br.recordsScanned, (unsigned)br.recordsCorrupt);
        if (Row(req, "Scan", v, "Every bucket file in the history folder; records must pass their own CRC") != ESP_OK) return ESP_FAIL;
        snprintf(v, sizeof(v), "%u buckets with an unreadable header scanned by file size, %u buckets unreadable", (unsigned)br.headerFallbackBuckets, (unsigned)br.bucketsUnreadable);
        if (Row(req, "Bucket headers", v, "A bucket that cannot be read makes the build fail rather than silently dropping aircraft") != ESP_OK) return ESP_FAIL;
        snprintf(v, sizeof(v), "%u aircraft indexed, %u duplicate records resolved, %u left out at the hard cap", (unsigned)br.aircraftIndexed, (unsigned)br.duplicatesResolved, (unsigned)br.rejectedAtCap);
        if (Row(req, "Result", v, "Winner per aircraft: newest lastSeen, then seenCount, then (fingerprint, offset)") != ESP_OK) return ESP_FAIL;
        if (br.legacyChecked) {
            snprintf(v, sizeof(v), "%u legacy entries: %u identical, %u differ (new index picked another record), %u missing from new",
                     (unsigned)br.legacyValid, (unsigned)br.legacySame, (unsigned)br.legacyNewer, (unsigned)br.legacyMissing);
            if (Row(req, "Legacy index cross-check", v, "Report only; does not gate the commit. The legacy file is only read") != ESP_OK) return ESP_FAIL;
        }
        snprintf(v, sizeof(v), "%u entries spot-checked against their records, %u mismatches", (unsigned)br.verifySampled, (unsigned)br.verifyMismatch);
        if (Row(req, "Verification", v, "Full re-read of the new file plus sampled record checks before commit") != ESP_OK) return ESP_FAIL;
        snprintf(v, sizeof(v), "scan %u us, write %u us, verify %u us, commit %u us, total %u us",
                 (unsigned)br.scanUs, (unsigned)br.writeUs, (unsigned)br.verifyUs, (unsigned)br.commitUs, (unsigned)br.totalUs);
        if (Row(req, "Timing", v, "Measured (esp_timer_get_time); boot is blocked for this long") != ESP_OK) return ESP_FAIL;
        if (Row(req, "Build error", eb, "Reason the build was not committed, if any") != ESP_OK) return ESP_FAIL;
        if (Send(req, "</table>") != ESP_OK)
            return ESP_FAIL;
    }

    // ---- self-test result ----
    TfSelfTestResult st;
    TfHistory_GetSelfTest(&st);
    if (st.ran)
    {
        if (st.passed)
            snprintf(buf, sizeof(buf), "<b>PASSED</b>");
        else if (st.failedStep >= TF_ST_OPEN_W && st.failedStep <= TF_ST_FLUSH)
            snprintf(buf, sizeof(buf), "<b>FAILED</b> at step: %s - the card mounts but cannot be written; it may be "
                                       "read-only or defective", TfHistory_SelfTestStepName(st.failedStep));
        else
            snprintf(buf, sizeof(buf), "<b>FAILED</b> at step: %s", TfHistory_SelfTestStepName(st.failedStep));
        if (Send(req, "<h3>TF filesystem self-test (last run)</h3><table><tr><th>Step</th><th>Result</th><th>Codes</th></tr>") != ESP_OK ||
            Row(req, "Overall", buf, "Scratch file only; index.dat, buckets and Seen data are never touched") != ESP_OK)
            return ESP_FAIL;
        char tm[96];
        snprintf(tm, sizeof(tm), "total %u us, write %u us, read %u us (run #%u)",
                 (unsigned)st.totalUs, (unsigned)st.writeUs, (unsigned)st.readUs, (unsigned)st.runs);
        if (Row(req, "Timing", tm, in.scratchPath ? in.scratchPath : "") != ESP_OK)
            return ESP_FAIL;
        for (int i = 0; i < TF_ST_COUNT; i++)
            if (StepRow(req, TfHistory_SelfTestStepName(i), &st.step[i]) != ESP_OK)
                return ESP_FAIL;
        if (Send(req, "</table>") != ESP_OK)
            return ESP_FAIL;
    }

    // ---- unmount / reinit trace ----
    TfReinitTrace tr;
    TfHistory_GetReinitTrace(&tr);
    if (tr.active)
    {
        snprintf(buf, sizeof(buf), "<h3>TF unmount / reinitialize sequence (request #%u)</h3>"
                                   "<table><tr><th>Transition</th><th>Result</th><th>Codes</th></tr>", (unsigned)tr.runs);
        if (Send(req, buf) != ESP_OK)
            return ESP_FAIL;
        for (int i = 0; i < TF_TR_COUNT; i++)
            if (StepRow(req, TfHistory_TraceStepName(i), &tr.step[i]) != ESP_OK)
                return ESP_FAIL;
        if (Send(req, "</table>") != ESP_OK)
            return ESP_FAIL;
    }

    // ---- actions ----
    return Send(req,
        "<p class='ev'>Browse the stored aircraft records on the <a href='/history'>History</a> page.</p>"
        "<h3>TF diagnostic actions</h3>"
        "<form method='POST' action='/diag/tf' style='display:inline'>"
        "<input type='hidden' name='act' value='selftest'><button type='submit'>Run TF self-test</button></form> "
        "<form method='POST' action='/diag/tf' style='display:inline' "
        "onsubmit=\"return confirm('Unmount the TF card?\\nHistory writes pause until you press Reinitialize TF. No data is erased.')\">"
        "<input type='hidden' name='act' value='unmount'><button type='submit'>Unmount TF</button></form> "
        "<form method='POST' action='/diag/tf' style='display:inline'>"
        "<input type='hidden' name='act' value='reinit'><button type='submit'>Reinitialize TF</button></form>"
        "<p class='ev'>Self-test: writes, flushes, re-reads and deletes one 64-byte scratch file in the history folder. "
        "Unmount closes the filesystem and frees the SPI bus (nothing is formatted or deleted); Reinitialize then runs the normal "
        "mount + history/index initialization again and shows which transition succeeded. Reinitialize is refused while the card is "
        "still mounted - Unmount first.</p>");
}

static esp_err_t RebootSection(httpd_req_t *req)
{
    return Send(req,
        "<h3 id='reboot'>Device</h3>"
        "<form method='POST' action='/diag/tf' "
        "onsubmit=\"return confirm('Reboot the Flight Radar?\\nAll persistent data will be preserved.')\">"
        "<input type='hidden' name='act' value='reboot'><button type='submit'>Reboot Device</button></form>"
        "<p class='ev'>Normal software restart (esp_restart). Settings (NVS), SPIFFS files, Seen data and the TF card contents are kept; "
        "pending Seen changes are saved first and the TF card is unmounted cleanly. The page returns in about 20 seconds.</p>");
}

// ---- Min-ever with timestamps (step 7) ----
// Values come from the sampler in the poll loop (DiagTelemetry_Tick), so the time is "first observed", with about
// 250 ms resolution; the allocator's own min-ever counters above are the authoritative values.
static esp_err_t MinTimingSection(httpd_req_t *req)
{
    char buf[120], when[80];
    if (Send(req, "<h3 id='mintime'>Minimums and when they happened</h3><table><tr><th>Metric</th><th>Lowest value</th><th>First seen</th></tr>") != ESP_OK)
        return ESP_FAIL;
    static const char *const kHeapName[DT_HEAP_COUNT] = {"Internal heap min-free", "DMA-capable internal min-free", "PSRAM min-free"};
    for (int k = 0; k < DT_HEAP_COUNT; k++) {
        const DiagMinHeap h = s_core->minHeap[k];
        if (s_core->minHeapValid[k]) {
            snprintf(buf, sizeof(buf), "%u bytes", (unsigned)h.minFree);
            DiagTelemetry_FormatStampLocal(&h.when, when, sizeof(when));
        } else {
            snprintf(buf, sizeof(buf), "not sampled yet");
            when[0] = 0;
        }
        if (Row(req, kHeapName[k], buf, when) != ESP_OK) return ESP_FAIL;
    }
    const DiagMinStack *st = s_core->stacks;
    const size_t n = s_core->nStacks;
    for (size_t i = 0; i < n; i++) {
        char label[48];
        snprintf(label, sizeof(label), "Stack min-free: %s", st[i].name);
        snprintf(buf, sizeof(buf), "%u bytes", (unsigned)st[i].minFreeBytes);
        DiagTelemetry_FormatStampLocal(&st[i].when, when, sizeof(when));
        if (Row(req, label, buf, when) != ESP_OK) return ESP_FAIL;
    }
    return Send(req, "</table><p class='ev'>Stack rows appear once that task has run a sampling pass. Times are in the configured time zone, or seconds since boot (T+) until the clock has synced.</p>");
}

/* 0.0.32: the device's local time when this snapshot was generated. */
static esp_err_t LocalTimeRow(httpd_req_t *req)
{
    const DiagStamp now = {(uint32_t)(s_core->uptimeUs / 1000000), s_core->utc};
    char text[64];
    DiagTelemetry_FormatStampLocal(&now, text, sizeof(text));
    return Row(req, "Local time (this snapshot)", text,
               "Measured (system clock via time_util: configured zone and DST; reload for a new value)");
}

/* 0.0.32: radar north reference and magnetic variation (north_ref.c). */
static esp_err_t NorthRefSection(httpd_req_t *req)
{
    NorthRefStatus st;
    NorthRef_GetStatus(&st);
    char buf[288], when[80];
    if (Send(req, "<h3 id='northref'>Heading reference</h3><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    if (Row(req, "Selected mode", NorthRefMode_Name(st.mode), "Setting (NVS radar/northref; Setup &gt; Features &amp; appearance; default AUTO)") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%s%s", st.resolved == NORTH_RESOLVED_MAGNETIC ? "MAGNETIC" : "TRUE",
             st.fallback ? " (FALLBACK)" : "");
    if (Row(req, "Resolved reference (radar up)", buf, "Derived (AUTO = magnetic when WMM is valid and reliable)") != ESP_OK)
        return ESP_FAIL;
    if (st.fallback && Row(req, "Fallback reason", st.fallbackReason ? st.fallbackReason : "-", "Derived") != ESP_OK)
        return ESP_FAIL;
    if (st.varValid) {
        snprintf(buf, sizeof(buf), "%.2f&deg; %s (H %.0f nT%s)", (double)fabsf(st.declDeg), st.declDeg >= 0 ? "E" : "W",
                 (double)st.horizNt, st.blackout ? ", BLACKOUT zone" : st.caution ? ", caution zone" : "");
    } else {
        snprintf(buf, sizeof(buf), "unavailable");
    }
    if (Row(req, "Magnetic variation", buf, "WMM2025 (NOAA/BGS, computed on the device; true = magnetic + variation)") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%.4f, %.4f%s", (double)st.lat, (double)st.lon, st.varValid ? "" : " (no value)");
    if (Row(req, "Location used", buf, "The radar centre the value was computed for (recomputed when the centre moves &gt; 0.01&deg;)") != ESP_OK)
        return ESP_FAIL;
    if (st.varValid) {
        DiagStamp c = {0, st.computedUtc};
        if (st.computedUtc > 0)
            DiagTelemetry_FormatStampLocal(&c, when, sizeof(when));
        snprintf(buf, sizeof(buf), "model date %.2f; %s%s%s", (double)st.decYear,
                 st.dateEstimated ? "estimated date (clock not synced yet)" : st.computedUtc > 0 ? "computed " : "",
                 st.computedUtc > 0 ? when : "", st.stale ? " - STALE, refresh in the next zero-aircraft maintenance window" : "");
        if (Row(req, "Value date", buf, "Recomputed yearly (stale after 365 days), on location change and after first clock sync if estimated") != ESP_OK)
            return ESP_FAIL;
    }
    snprintf(buf, sizeof(buf), "%s%s", st.cacheState ? st.cacheState : "-", st.fromCache ? " (value in use came from the cache)" : "");
    if (Row(req, "Cache (" NORTHREF_CACHE_PATH ")", buf, "SPIFFS file: location, value, date, CRC") != ESP_OK)
        return ESP_FAIL;
    if (st.lastAttemptUptimeSec || st.lastAttemptUtc) {
        DiagStamp a = {st.lastAttemptUptimeSec, st.lastAttemptUtc};
        DiagTelemetry_FormatStampLocal(&a, when, sizeof(when));
        snprintf(buf, sizeof(buf), "%s: %s (%s; %u since boot)", st.lastAttemptOk ? "OK" : "FAILED",
                 st.lastResult ? st.lastResult : "-", when, (unsigned)st.computeCount);
    } else {
        snprintf(buf, sizeof(buf), "none this boot (%s)", st.lastResult ? st.lastResult : "-");
    }
    if (Row(req, "Last computation", buf, "Measured (local WMM evaluation, no network)") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "radar #%06X, WebUI #%06X", (unsigned)UiPrefs_RadarAccentRgb(), (unsigned)UiPrefs_WebAccentRgb());
    if (Row(req, "Accent colors", buf, "Settings (NVS radar/acc_radar, acc_web; independent)") != ESP_OK)
        return ESP_FAIL;
    return Send(req, "</table>");
}

// ---- Recurring operations (step 7) ----
static esp_err_t OpsSection(httpd_req_t *req)
{
    if (Send(req, "<h3 id='ops'>Recurring operations</h3><table><tr><th>Operation</th><th>Runs / failed / skipped</th>"
                  "<th>Last run (start &rarr; finish)</th><th>Result</th><th>Worst run</th><th>Last failure</th></tr>") != ESP_OK)
        return ESP_FAIL;
    for (int i = 0; i < DT_OP_COUNT; i++) {
        const DiagOpStats o = s_core->ops[i];
        char when[80], last[160], run[200], worst[200], result[160];
        if (o.haveFailure) {
            DiagTelemetry_FormatStampLocal(&o.lastFailureWhen, when, sizeof(when));
            snprintf(last, sizeof(last), "%s (%s)", o.lastFailure, when);
        } else {
            snprintf(last, sizeof(last), "none");
        }
        if (o.runs == 0) {
            snprintf(run, sizeof(run), "none since boot");
            snprintf(worst, sizeof(worst), "-");
            snprintf(result, sizeof(result), "-");
        } else {
            DiagTelemetry_FormatRunHtml(&o.lastRun, o.lastMs, run, sizeof(run));
            DiagTelemetry_FormatRunHtml(&o.worstWhen, o.worstMs, worst, sizeof(worst));
            if (o.consecutiveFailures == 0) {
                snprintf(result, sizeof(result), "OK");
            } else {
                if (o.haveOk)
                    DiagTelemetry_FormatStampLocal(&o.lastOkWhen, when, sizeof(when));
                else
                    snprintf(when, sizeof(when), "none since boot");
                snprintf(result, sizeof(result), "%sFAILED, %u in a row; last success %s",
                         o.consecutiveFailures >= DT_ATTENTION_CONSECUTIVE ? "<b style='color:#c00'>ATTENTION</b> " : "",
                         (unsigned)o.consecutiveFailures, when);
            }
        }
        /* 0.1.6 per-provider fetch rows: say whether the provider is enabled now (and any 429 backoff),
         * so an empty row reads as "disabled / not used yet", not as a fault. */
        char name[112];
        if (i == DT_OP_FETCH_OPENSKY || i == DT_OP_FETCH_ADSBLOL) {
            const AircraftProviderType pt = i == DT_OP_FETCH_OPENSKY ? AIRCRAFT_PROVIDER_OPENSKY : AIRCRAFT_PROVIDER_ADSBLOL;
            const uint32_t backoff = AircraftProvider_GetRateLimitSecondsFor(pt);
            char bo[48] = "";
            if (backoff)
                snprintf(bo, sizeof(bo), ", rate-limit backoff %u s", (unsigned)backoff);
            snprintf(name, sizeof(name), "%s<br><small>%s%s</small>", DiagTelemetry_OpName((DiagOp)i),
                     AircraftProvider_IsEnabled(pt) ? "enabled" : (o.runs || o.skipped ? "disabled now" : "disabled"), bo);
        } else {
            snprintf(name, sizeof(name), "%s", DiagTelemetry_OpName((DiagOp)i));
        }
        /* Two sends: each stays well inside SendFormat's 512-byte buffer even with both ends dated. */
        if (SendFormat(req, "<tr><td>%s</td><td>%u / %u / %u</td><td>%s</td>", name,
                       (unsigned)o.runs, (unsigned)o.failures, (unsigned)o.skipped, run) != ESP_OK ||
            SendFormat(req, "<td>%s</td><td>%s</td><td class='ev'>%s</td></tr>", result, worst, last) != ESP_OK)
            return ESP_FAIL;
    }
    return Send(req, "</table><p class='ev'>Since boot, in the configured time zone. Finish is stamped when a run ends; start = finish - duration "
                     "(to the second). A skipped run is one that could not start (rate-limit backoff, TF unavailable); "
                     "it is not a failure. History flush only counts passes that had entries to write. "
                     "ATTENTION appears after 3 failures in a row and clears on the next success. "
                     "The Seen and History flush rows are repeated in Storage / History next to their other metrics. "
                     "OpenSky fetch and adsb.lol fetch are the Provider refresh measurements split by provider (the same "
                     "aircraft-list request, timed from start to response); Provider refresh stays the combined row. "
                     "The OAuth token row is OpenSky only (adsb.lol needs no token). HTTP status, response size and "
                     "parse counts are not stored: they are written to the serial log when Provider diagnostics "
                     "(Debugging) is NORMAL or higher.</p>");
}

// Manual flush section (Storage group): saves pending Hot Seen changes and asks the poll task to write
// pending History Manager entries to TF now. Reboot paths are unchanged.
static esp_err_t FlushSection(httpd_req_t *req)
{
    return Send(req,
        "<h3 id='flush'>Flush pending data now</h3>"
        "<form method='POST' action='/diag/tf'><input type='hidden' name='act' value='flush'>"
        "<button type='submit'>Flush Seen + History now</button></form>"
        "<p class='ev'>Writes pending Hot Seen changes to SPIFFS and pending History entries to the TF card immediately, "
        "instead of waiting for the periodic flush. Waits up to 20 seconds; the periodic schedule is unaffected.</p>");
}

// Runs the flush and answers with a small result page. No storage lock is held while writing the response.
static esp_err_t FlushAction(httpd_req_t *req)
{
    bool seenOk = true;
    if (SeenAircraft_IsDirty())
        seenOk = SeenAircraft_Flush();

    uint32_t seq = HistoryManager_RequestFlush();
    HistoryFlushResult r;
    bool done = false;
    for (int i = 0; i < 200 && !(done = HistoryManager_GetFlushResult(seq, &r)); i++)
        vTaskDelay(pdMS_TO_TICKS(100));

    char msg[200];
    bool ok = seenOk && done && r.ok;
    if (!done)
        snprintf(msg, sizeof(msg), "Flush failed: the History writer did not answer within 20 seconds (it will still flush on its normal schedule).");
    else if (r.tfUnavailable)
        snprintf(msg, sizeof(msg), "Flush failed: the TF card is not available, so History was not written. Hot Seen: %s.", seenOk ? "saved" : "NOT saved");
    else
        snprintf(msg, sizeof(msg), "%s: Hot Seen %s; History wrote %u entr%s, %u failed, %u still pending.",
                 ok ? "Flush complete" : "Flush incomplete", seenOk ? "saved" : "NOT saved",
                 (unsigned)r.written, r.written == 1 ? "y" : "ies", (unsigned)r.failed, (unsigned)r.remaining);

    if (done)
        DiagTelemetry_Event("Manual flush: Seen %s, History wrote %u, failed %u, pending %u%s", seenOk ? "saved" : "NOT saved",
                            (unsigned)r.written, (unsigned)r.failed, (unsigned)r.remaining, r.tfUnavailable ? " (TF unavailable)" : "");
    else
        DiagTelemetry_Event("Manual flush: History writer did not answer within 20 s");
    if (!ok)
        httpd_resp_set_status(req, "500 Internal Server Error");
    return SendDiagNotice(req, "flush", msg);
}

/* ---- 0.1.8: Console logs on the TF card (status for everyone; files, downloads, deletion and the setting only
 * for requests that arrived on the station interface - see web_access.h) ---- */
static const char LOG_STATION_ONLY[] =
    "Console-log files, downloads, deletion and the capture setting are available only from your own Wi-Fi network. "
    "This request arrived via %s, which anyone nearby can join without a password, so they are hidden here.";

static esp_err_t LogSection(httpd_req_t *req)
{
    const LogCaptureStatus *st = &s_core->logcap;
    char buf[200], ev[160];
    if (Send(req, "<h3 id='logs'>Console logs on TF card</h3>"
                  "<p class='ev'>Optional copy of the serial console (ESP_LOGx lines) written to /sdcard/" LOGCAP_DIR_NAME
                  "/ on the TF card. Off by default; changing it takes effect after a reboot. Serial output is unchanged. "
                  "Passwords, tokens, authorization headers, SSIDs/BSSIDs and the decimals of coordinates are masked "
                  "in the saved copy.</p>"
                  "<table><tr><th>Item</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    if (Row(req, "Saved setting (next boot)", st->saved ? "ON" : "OFF",
            st->saved != st->active ? "NVS diag/logcap - reboot required to apply" : "NVS diag/logcap") != ESP_OK)
        return ESP_FAIL;
    if (st->active) {
        if (Row(req, "Active this boot", "ON", "Read once from NVS at boot") != ESP_OK) return ESP_FAIL;
    } else {
        snprintf(buf, sizeof(buf), "OFF - %s", st->inactiveReason ? st->inactiveReason : "");
        if (Row(req, "Active this boot", buf, "No buffer, task or hook exists while off") != ESP_OK) return ESP_FAIL;
    }
    if (st->active) {
        const LogCapCounters *k = &st->ctr;
        snprintf(buf, sizeof(buf), "%s%s%s", LogWriterState_Name(st->state), st->reason[0] ? " - " : "", st->reason);
        if (Row(req, "Writer state", buf, "Writer task LogWr") != ESP_OK) return ESP_FAIL;
        if (st->file[0])
            snprintf(buf, sizeof(buf), "%s (%u bytes; boot %u, segment %u)", st->file, (unsigned)st->fileSize,
                     (unsigned)st->boot, (unsigned)st->seg);
        else
            snprintf(buf, sizeof(buf), "none yet (boot %u)", (unsigned)st->boot);
        if (Row(req, "Current file", buf, "L&lt;boot&gt;&lt;segment&gt;.LOG") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u of %u bytes used now, high water %u", (unsigned)st->ringUsed,
                 (unsigned)st->ringSize, (unsigned)st->ringHighWater);
        if (Row(req, "Capture buffer (PSRAM)", buf, "Measured") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u lines, %u bytes", (unsigned)k->lines, (unsigned)k->bytes);
        if (Row(req, "Captured since boot", buf, "Counted by the log hook") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "buffer full: %u lines (%u bytes); logger busy: %u; interrupt/scheduler context: %u; "
                 "TF lock held by the logging task: %u",
                 (unsigned)k->dropFullLines, (unsigned)k->dropFullBytes, (unsigned)k->dropBusyLines,
                 (unsigned)k->dropContextLines, (unsigned)k->dropLockLines);
        if (Row(req, "Dropped lines", buf, "Each gap is also marked in the file (=== DROPPED ...)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u truncated (over %u chars), %u redactions, %u writer-own lines not saved",
                 (unsigned)k->truncated, (unsigned)(LOGCAP_LINE_MAX - 1), (unsigned)k->redactions, (unsigned)k->ownLines);
        if (Row(req, "Line handling", buf, "Counted by the log hook") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u writes, %llu bytes; %u write errors, %u close errors, %u lock timeouts",
                 (unsigned)st->writes, (unsigned long long)st->bytesWritten, (unsigned)st->writeErrors,
                 (unsigned)st->closeErrors, (unsigned)st->lockTimeouts);
        if (Row(req, "Card writes", buf, "Writer task") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u files created, %u deleted by retention, %u retention failures, %u interrupted logs "
                 "marked, %u space suspensions", (unsigned)st->filesCreated, (unsigned)st->filesDeleted,
                 (unsigned)st->retentionFailures, (unsigned)st->interruptedMarked, (unsigned)st->spaceSuspends);
        if (Row(req, "Files / retention", buf, "Writer task") != ESP_OK) return ESP_FAIL;
        if (st->deferring)
            snprintf(buf, sizeof(buf), "DEFERRING NOW since uptime %u s (largest DMA block %u B); %u cycles in %u episodes; longest %u s",
                     (unsigned)st->deferStartS, (unsigned)st->lastDeferDmaBytes, (unsigned)st->deferredCycles,
                     (unsigned)st->deferEpisodes, (unsigned)st->longestDeferS);
        else
            snprintf(buf, sizeof(buf), "%u cycles in %u episodes; longest %u s", (unsigned)st->deferredCycles,
                     (unsigned)st->deferEpisodes, (unsigned)st->longestDeferS);
        snprintf(ev, sizeof(ev), "A write cycle (about every 5 s) waits while the largest internal DMA block is below %u B",
                 (unsigned)LOGCAP_DMA_MIN_BYTES);
        if (Row(req, "Card-write deferrals (DMA memory)", buf, ev) != ESP_OK) return ESP_FAIL;
        if (st->haveFreeBytes)
            snprintf(buf, sizeof(buf), "%llu MiB", (unsigned long long)(st->lastFreeBytes / (1024u * 1024u)));
        else
            snprintf(buf, sizeof(buf), "not checked yet");
        if (Row(req, "Card free space (last check)", buf, "Checked before each new file and every 64 writes") != ESP_OK) return ESP_FAIL;
        if (st->lastError[0])
            snprintf(buf, sizeof(buf), "%s (uptime %u s)", st->lastError, (unsigned)st->lastErrorUptimeS);
        else
            snprintf(buf, sizeof(buf), "none");
        if (Row(req, "Last error", buf, "Writer task") != ESP_OK) return ESP_FAIL;
        if (st->writerStackMinBytes)
            snprintf(buf, sizeof(buf), "%u bytes", (unsigned)st->writerStackMinBytes);
        else
            snprintf(buf, sizeof(buf), "not measured yet");
        if (Row(req, "Writer stack minimum free", buf, "4096-byte internal stack, measured by the task") != ESP_OK) return ESP_FAIL;
    }
    snprintf(ev, sizeof(ev), "Oldest files are deleted first; the file being written or downloaded never is");
    if (Row(req, "Limits", "1 MiB per file; 32 MiB and 64 files in total; writing pauses below 64 MiB free card space", ev) != ESP_OK)
        return ESP_FAIL;
    if (Row(req, "Not captured", "bootloader and early boot (before the hook), panic/crash output, esp_rom_printf lines "
                                 "(including FAILED_ALLOC: a summary line is written instead), LVGL printf warnings",
            "By design") != ESP_OK)
        return ESP_FAIL;
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    if (s_core->via != WEBIF_STATION) {
        char msg[320];
        snprintf(msg, sizeof(msg), LOG_STATION_ONLY, WebIf_Name(s_core->via));
        return SendFormat(req, "<p><b>%s</b></p>", msg);
    }

    /* ---- file list (newest first) ---- */
    size_t n = 0, total = 0;
    const LogFileResult lr = LogCapture_ListFiles(s_ctx->logFiles, LOGCAP_FILES_MAX, &n, &total);
    if (lr != LOGF_OK) {
        if (SendFormat(req, "<p>Log files: unavailable (%s).</p>", LogFileResult_Text(lr)) != ESP_OK)
            return ESP_FAIL;
    } else {
        LogFileInfo *f = s_ctx->logFiles;
        for (size_t i = 1; i < n; i++) { /* insertion sort, descending name = newest boot/segment first */
            LogFileInfo t = f[i];
            size_t j = i;
            for (; j > 0 && strcmp(f[j - 1].name, t.name) < 0; j--)
                f[j] = f[j - 1];
            f[j] = t;
        }
        uint64_t bytes = 0;
        for (size_t i = 0; i < n; i++)
            bytes += f[i].size;
        if (SendFormat(req, "<h4>Log files (%u%s, %llu bytes)</h4>", (unsigned)total,
                       total > n ? " - list shows the first 64" : "", (unsigned long long)bytes) != ESP_OK)
            return ESP_FAIL;
        if (n && Send(req, "<table><tr><th>File</th><th>Size</th><th>Modified</th><th>Action</th></tr>") != ESP_OK)
            return ESP_FAIL;
        for (size_t i = 0; i < n; i++) {
            char when[40] = "unknown";
            TimeLocal tl;
            if (f[i].mtime > 1577836800 && TimeUtil_ToLocal(f[i].mtime, &tl)) /* after 2020: the clock was set */
                snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d", tl.year, tl.month, tl.day, tl.hour, tl.minute);
            const bool active = st->active && strcmp(st->file, f[i].name) == 0;
            if (SendFormat(req, "<tr><td><a href='/diag?log=%s'>%s</a></td><td>%u</td><td>%s</td><td>", f[i].name,
                           f[i].name, (unsigned)f[i].size, when) != ESP_OK)
                return ESP_FAIL;
            if (active) {
                if (Send(req, "being written (download shows it up to now)") != ESP_OK)
                    return ESP_FAIL;
            } else if (SendFormat(req, "<form method='POST' action='/diag/tf' style='display:inline'>"
                                       "<input type='hidden' name='act' value='logdel'>"
                                       "<input type='hidden' name='name' value='%s'>"
                                       "<label><input type='checkbox' name='confirm' value='1' required> confirm</label> "
                                       "<button type='submit'>Delete</button></form>", f[i].name) != ESP_OK) {
                return ESP_FAIL;
            }
            if (Send(req, "</td></tr>") != ESP_OK)
                return ESP_FAIL;
        }
        if (n && Send(req, "</table>") != ESP_OK)
            return ESP_FAIL;
        if (n && Send(req, "<form method='POST' action='/diag/tf'><input type='hidden' name='act' value='logclr'>"
                           "<label><input type='checkbox' name='confirm' value='1' required> I want to delete every "
                           "console-log file</label> <button type='submit'>Delete all log files</button></form>"
                           "<p class='ev'>Deletes only " LOGCAP_DIR_NAME "/L*.LOG files; the file being written or "
                           "downloaded is kept. Seen, History and other card data are never touched.</p>") != ESP_OK)
            return ESP_FAIL;
    }

    char form[480];
    snprintf(form, sizeof(form),
             "<form method='POST' action='/diag/advanced'>"
             "<input type='hidden' name='mode' value='logcap'>"
             "<input type='hidden' name='enable' value='%d'>"
             "<button type='submit' name='reboot' value='0'>%s console-log saving (reboot later)</button> "
             "<button type='submit' name='reboot' value='1'>%s and reboot now</button>"
             "</form>",
             st->saved ? 0 : 1, st->saved ? "Disable" : "Enable", st->saved ? "Disable" : "Enable");
    return Send(req, form);
}

static int LogResultStatus(LogFileResult r, const char **status)
{
    switch (r) {
    case LOGF_OK: *status = "200 OK"; return 200;
    case LOGF_BAD_NAME: *status = "400 Bad Request"; return 400;
    case LOGF_NOT_FOUND: *status = "404 Not Found"; return 404;
    case LOGF_ACTIVE:
    case LOGF_IN_USE: *status = "409 Conflict"; return 409;
    case LOGF_BUSY:
    case LOGF_NO_CARD: *status = "503 Service Unavailable"; return 503;
    default: *status = "500 Internal Server Error"; return 500;
    }
}

static esp_err_t LogForbidden(httpd_req_t *req, WebIf via)
{
    char msg[320];
    snprintf(msg, sizeof(msg), LOG_STATION_ONLY, WebIf_Name(via));
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, msg);
}

/* GET /diag?log=NAME: stream one console-log file in 4 KB pieces. Each piece is read under the storage lock and the
 * lock is released before it is sent. The file is pinned (retention will not delete it) until the end. The size is
 * fixed when the download starts; lines the writer appends after that are not included. */
static esp_err_t LogDownload(httpd_req_t *req, const char *name)
{
    const WebIf via = WebAccess_RequestInterface(req);
    if (via != WEBIF_STATION)
        return LogForbidden(req, via);
    uint32_t size = 0;
    LogFileResult r = LogCapture_DownloadBegin(name, &size);
    if (r != LOGF_OK) {
        const char *status;
        (void)LogResultStatus(r, &status);
        char msg[120];
        snprintf(msg, sizeof(msg), "Log file not available: %s.", LogFileResult_Text(r));
        httpd_resp_set_status(req, status);
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, msg);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    snprintf(s_ctx->disposition, sizeof(s_ctx->disposition), "attachment; filename=\"fr7-%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", s_ctx->disposition);

    esp_err_t res = ESP_OK;
    uint32_t off = 0;
    while (off < size) {
        const size_t want = size - off < LOG_DL_CHUNK ? size - off : LOG_DL_CHUNK;
        size_t got = 0;
        r = LogCapture_DownloadRead(name, off, s_ctx->chunk, want, &got);
        for (int retry = 0; r == LOGF_BUSY && retry < 3; retry++) { /* bounded: 3 more tries, 200 ms apart */
            vTaskDelay(pdMS_TO_TICKS(200));
            r = LogCapture_DownloadRead(name, off, s_ctx->chunk, want, &got);
        }
        if (r != LOGF_OK || got == 0) {
            char note[160];
            const int k = snprintf(note, sizeof(note), "\n=== DOWNLOAD INCOMPLETE: stopped at byte %u of %u (%s) ===\n",
                                   (unsigned)off, (unsigned)size, r != LOGF_OK ? LogFileResult_Text(r) : "file ended early");
            res = httpd_resp_send_chunk(req, note, k);
            break;
        }
        if (httpd_resp_send_chunk(req, s_ctx->chunk, (ssize_t)got) != ESP_OK) {
            res = ESP_FAIL; /* client gone */
            break;
        }
        off += (uint32_t)got;
    }
    LogCapture_DownloadEnd();
    if (res != ESP_OK)
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* POST /diag/tf act=logdel name=NAME confirm=1 | act=logclr confirm=1 */
static esp_err_t LogDeleteAction(httpd_req_t *req, const char *body, bool all)
{
    const WebIf via = WebAccess_RequestInterface(req);
    if (via != WEBIF_STATION)
        return LogForbidden(req, via);
    char val[16];
    if (httpd_query_key_value(body, "confirm", val, sizeof(val)) != ESP_OK || strcmp(val, "1") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return SendDiagNotice(req, "logs", "Nothing deleted: tick the confirmation box first.");
    }
    char msg[200];
    LogFileResult r;
    if (all) {
        uint32_t deleted = 0, skipped = 0;
        r = LogCapture_DeleteAll(&deleted, &skipped);
        if (r == LOGF_OK)
            snprintf(msg, sizeof(msg), "Deleted %u console-log file%s; %u kept (being written or downloaded).",
                     (unsigned)deleted, deleted == 1 ? "" : "s", (unsigned)skipped);
        else
            snprintf(msg, sizeof(msg), "Delete all: %u deleted, %u kept, then stopped: %s.", (unsigned)deleted,
                     (unsigned)skipped, LogFileResult_Text(r));
        DiagTelemetry_Event("Console logs: delete all -> %u deleted, %u kept (%s)", (unsigned)deleted, (unsigned)skipped,
                            LogFileResult_Text(r));
    } else {
        char name[16] = "";
        (void)httpd_query_key_value(body, "name", name, sizeof(name));
        r = LogCapture_Delete(name);
        if (!LogCap_ValidName(name))
            snprintf(msg, sizeof(msg), "Nothing deleted: %s.", LogFileResult_Text(r));
        else if (r == LOGF_OK)
            snprintf(msg, sizeof(msg), "Deleted %s.", name);
        else
            snprintf(msg, sizeof(msg), "%s was not deleted: %s.", name, LogFileResult_Text(r));
        if (LogCap_ValidName(name))
            DiagTelemetry_Event("Console logs: delete %s -> %s", name, LogFileResult_Text(r));
    }
    const char *status;
    if (LogResultStatus(r, &status) != 200)
        httpd_resp_set_status(req, status);
    return SendDiagNotice(req, "logs", msg);
}

// POST /diag/tf  act=selftest | unmount | reinit | reboot | flush | logdel | logclr  (one handler slot for all)
static esp_err_t TfActionPost(httpd_req_t *req)
{
    char body[96] = {0};
    int total = 0;
    if (req->content_len >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form too large");
    while (total < (int)req->content_len)
    {
        int got = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (got <= 0)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Could not read form");
        total += got;
    }
    body[total] = '\0';

    char act[16];
    if (httpd_query_key_value(body, "act", act, sizeof(act)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing action");

    if (strcmp(act, "reboot") == 0)
    {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        char page[1024];
        snprintf(page, sizeof(page),
                 "<!doctype html><html%s><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                 "<meta http-equiv='refresh' content='20;url=/diag#reboot'><title>Rebooting</title><style>" WEBSTYLE_MINI_CSS "</style></head>"
                 "<body style='font:16px sans-serif;max-width:750px;margin:1em auto;padding:0 1em'>"
                 "<p>Rebooting now; this page returns to /diag in about 20 seconds.</p>"
                 "<p><a href='/diag#reboot'>Back to diagnostics</a></p></body></html>",
                 WebStyle_HtmlAttr());
        httpd_resp_sendstr(req, page);
        // Same order as the other deliberate restarts: let the response go
        // out, flush pending Seen + TF History (bounded), then restart. The TF
        // card is unmounted first (bounded wait, best effort) so FAT is left clean.
        vTaskDelay(pdMS_TO_TICKS(500));
        RebootFlush_BeforeRestart(NULL);
        HistoryManager_TfPause();
        (void)TfHistory_Unmount();
        esp_restart();
        return ESP_OK; // not reached
    }

    if (strcmp(act, "flush") == 0)
        return FlushAction(req);
    if (strcmp(act, "logdel") == 0 || strcmp(act, "logclr") == 0)
        return LogDeleteAction(req, body, act[3] == 'c');

    if (strcmp(act, "selftest") == 0)
    {
        (void)TfHistory_RunSelfTest(); // result is stored and rendered by TfSection
    }
    else if (strcmp(act, "unmount") == 0)
    {
        HistoryManager_TfPause();
        (void)TfHistory_Unmount();
        HistoryManager_TfResume(); // stays paused unless the unmount did not actually take effect
    }
    else if (strcmp(act, "reinit") == 0)
    {
        HistoryManager_TfPause();
        (void)TfHistory_Reinit();
        HistoryManager_TfResume(); // resumes only if history became available again
    }
    else
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/diag#tf");
    return httpd_resp_send(req, NULL, 0);
}

/* ---- 0.1.9: latest poll per provider, side by side (values from s_core; the exports use the same values) ---- */
static esp_err_t ProviderPollSection(httpd_req_t *req)
{
    if (Send(req, "<h3 id='provpoll'>Provider last poll</h3><table><tr><th>Item</th>") != ESP_OK)
        return ESP_FAIL;
    for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++)
        if (SendFormat(req, "<th>%s</th>", AircraftProviderType_Name((AircraftProviderType)p)) != ESP_OK)
            return ESP_FAIL;
    if (Send(req, "</tr>") != ESP_OK)
        return ESP_FAIL;
    static const char *const rows[] = {"Enabled now", "Last request", "Request URL (coordinates shortened)", "Outcome",
                                       "HTTP status", "Body bytes (kept / received / Content-Length)", "Transport failure",
                                       "Parse stages", "Last successful poll", "Last failed poll"};
    char cell[200], esc[260], when[64];
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        if (SendFormat(req, "<tr><td>%s</td>", rows[r]) != ESP_OK)
            return ESP_FAIL;
        for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++) {
            const AircraftProviderType t = (AircraftProviderType)p;
            const ProviderPollDiag *d = &s_core->poll[p];
            const bool any = s_core->havePoll[p] && d->polled;
            cell[0] = 0;
            if (r == 0) {
                snprintf(cell, sizeof(cell), "%s", AircraftProvider_IsEnabled(t) ? "yes" : "no");
            } else if (!s_core->havePoll[p]) {
                snprintf(cell, sizeof(cell), "n/a (not recorded)");
            } else if (!any) {
                snprintf(cell, sizeof(cell), "no request this boot");
            } else if (r == 1) {
                const DiagStamp st = CoreStampAt(s_core, d->startUptimeS);
                DiagTelemetry_FormatStampLocal(&st, when, sizeof(when));
                snprintf(cell, sizeof(cell), "#%u at %s, %u ms", (unsigned)d->seq, when, (unsigned)d->durationMs);
            } else if (r == 2) {
                snprintf(cell, sizeof(cell), "GET %s", d->url);
                LogCap_Redact(cell, strlen(cell)); /* same rule as the saved logs: one decimal of the position */
            } else if (r == 3) {
                snprintf(cell, sizeof(cell), "%s", ProviderPollOutcome_Name(d->outcome));
            } else if (r == 4) {
                if (d->httpStatus)
                    snprintf(cell, sizeof(cell), "%d", d->httpStatus);
                else
                    snprintf(cell, sizeof(cell), "none (no HTTP response)");
            } else if (r == 5) {
                char cl[24];
                if (d->contentLength >= 0)
                    snprintf(cl, sizeof(cl), "%lld", (long long)d->contentLength);
                else
                    snprintf(cl, sizeof(cl), "unknown");
                snprintf(cell, sizeof(cell), "%u / %u / %s%s%s", (unsigned)d->bytes, (unsigned)d->received, cl,
                         d->truncated ? "; TRUNCATED (buffer full)" : "",
                         d->fetchOutcome == PPOLL_FETCHED && !d->complete ? "; client reports incomplete" : "");
            } else if (r == 6) {
                if (d->transport == PXPORT_NONE)
                    snprintf(cell, sizeof(cell), "none");
                else
                    snprintf(cell, sizeof(cell), "%s (esp_err 0x%x, esp-tls 0x%x)", ProviderTransportKind_Name(d->transport),
                             (unsigned)d->espErr, (unsigned)d->tlsErr);
            } else if (r == 7) {
                if (!d->haveParse) {
                    if ((d->outcome == PPOLL_JSON_INVALID || d->outcome == PPOLL_TRUNCATED) && d->jsonErrorOffset >= 0)
                        snprintf(cell, sizeof(cell), "not parsed (JSON stopped near byte %d)", d->jsonErrorOffset);
                    else
                        snprintf(cell, sizeof(cell), "not parsed");
                } else {
                    const ProviderParseStats *ps = &d->parse;
                    snprintf(cell, sizeof(cell), "%d entries: %d malformed, %d no id, %d no position, %d bad position, "
                             "%d on ground (%d shown), %d not examined (list full); %d admitted",
                             ps->entries, ps->malformed, ps->missingId, ps->missingPosition, ps->invalidPosition, ps->onGround,
                             ps->groundShown, ps->notExamined, ps->admitted);
                }
            } else if (r == 8) {
                if (d->haveSuccess) {
                    const DiagStamp st = CoreStampAt(s_core, d->lastSuccessUptimeS);
                    DiagTelemetry_FormatStampLocal(&st, cell, sizeof(cell));
                } else {
                    snprintf(cell, sizeof(cell), "none this boot");
                }
            } else {
                if (d->haveError) {
                    const DiagStamp st = CoreStampAt(s_core, d->lastErrorUptimeS);
                    DiagTelemetry_FormatStampLocal(&st, when, sizeof(when));
                    snprintf(cell, sizeof(cell), "%s; %s", when, d->lastError);
                } else {
                    snprintf(cell, sizeof(cell), "none this boot");
                }
            }
            WebUtil_EscapeHtml(esc, sizeof(esc), cell);
            if (SendFormat(req, "<td>%s</td>", esc) != ESP_OK)
                return ESP_FAIL;
        }
        if (Send(req, "</tr>") != ESP_OK)
            return ESP_FAIL;
    }
    return Send(req, "</table><p class='ev'>Recorded for every request whatever the Provider diagnostics level; the serial "
                     "log adds detail by level (Normal: request lines, failures, adsb.lol heap before TLS; Verbose: parse "
                     "stages; Raw: up to 8 record samples per poll). Parse stages count each array element once, at the "
                     "first check it fails; \"not examined\" means the 200-aircraft list was already full.</p>");
}

/* The report body: everything after the page head. Rendered once per request, either to the browser (HTML) or
 * through the export converter (0.1.7: ?fmt=txt / ?fmt=json). Values shown come from s_core or live getters. */
static esp_err_t DiagReportBody(httpd_req_t *req)
{
    if (Send(req,
        "<h1>Runtime capacity report</h1>"
        "<p>Every value below is read live from this device right now - reload the page for a fresh "
        "snapshot. \"Since boot\" values reset on reboot, not on reload. This page adds negligible "
        "overhead: it only reads existing counters when you open it, nothing runs in the background "
        "for it. Copy the numbers below into a Claude conversation for capacity/design questions - see "
        "PROJECT_STATE.md's \"Runtime diagnostics\" section for what each row is used for.</p>") != ESP_OK)
        return ESP_FAIL;
    if (!s_export &&
        Send(req, "<p id='export'><b>Download this report:</b> <a href='/diag?fmt=txt'>plain text (.txt)</a> &middot; "
                  "<a href='/diag?fmt=json'>JSON (.json)</a> <span class='ev'>- the same rows as this page, read when you "
                  "click; nothing is reset. Format: README, \"Diagnostics export\".</span></p>") != ESP_OK)
        return ESP_FAIL;

    // ---- Uptime / identity ----
    int64_t uptimeUs = s_core->uptimeUs;
    // 80, not a tighter "reasonable" bound: gcc's -Werror=format-truncation
    // can't know esp_timer_get_time() won't return something huge, so it
    // sizes against %lld's full int64 range (up to 20 digits) for every one
    // of the three values here, not just the visually-plausible ones.
    char uptimeText[80];
    char buf[80]; /* shared scratch buffer for the rest of this function; was declared
                   * later (RAM section) originally, moved up here after a real build
                   * caught this section using it before that point */
    snprintf(uptimeText, sizeof(uptimeText), "%lld s (%lld h %lld m)",
             (long long)(uptimeUs / 1000000),
             (long long)(uptimeUs / 3600000000LL),
             (long long)((uptimeUs / 60000000LL) % 60));
    char fwEsc[80];
    WebUtil_EscapeHtml(fwEsc, sizeof(fwEsc), FW_VERSION_STRING);
    if (GroupOpen(req, "system", "System", true) != ESP_OK)
        return ESP_FAIL;
    if (Send(req, "<h3>Identity</h3><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK ||
        Row(req, "Firmware Build", fwEsc, "Compiled in from src/VERSION at build time (always shown, not an Advanced Diagnostic)") != ESP_OK ||
        Row(req, "Uptime", uptimeText, "Measured (esp_timer_get_time)") != ESP_OK ||
        LocalTimeRow(req) != ESP_OK ||
        SendFormat(req, "<tr><td>Radar center</td><td>%.4f, %.4f</td><td class='ev'>Measured (current setting)</td></tr>",
                   GetRadarLat(), GetRadarLon()) != ESP_OK)
        return ESP_FAIL;
    // Same integer/fraction split FormatCoordinate() (web_airports.c) already
    // uses instead of "%f", to stay clear of the same class of
    // -Werror=format-truncation false positive the uptime fix above (and
    // this project's own real ESP-IDF build) just ran into with %lld.
    char rangeText[40];
    float range = GetRadarRange();
    long rangeScaled = lroundf(fabsf(range) * 100.0f);
    snprintf(rangeText, sizeof(rangeText), "%s%ld.%02ld km",
             range < 0 ? "-" : "", rangeScaled / 100, rangeScaled % 100);
    if (Row(req, "Radar range", rangeText, "Measured (current setting)") != ESP_OK)
        return ESP_FAIL;
    if (Row(req, "Reset reason", ResetReasonName(esp_reset_reason()), "Measured (esp_reset_reason)") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u MHz", (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    if (Row(req, "CPU frequency", buf, "Configured (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ; this device does not use dynamic frequency scaling)") != ESP_OK)
        return ESP_FAIL;
    TimeZoneConfig tzCfg;
    TimeUtil_GetConfig(&tzCfg);
    TimeLocal local;
    bool haveLocal = TimeUtil_ToLocal(time(NULL), &local);
    snprintf(buf, sizeof(buf), "%s, Automatic DST %s", tzCfg.zoneId, tzCfg.autoDst ? "on" : "off");
    if (Row(req, "Timezone / DST configuration", buf, "Measured (TimeUtil_GetConfig)") != ESP_OK)
        return ESP_FAIL;
    if (haveLocal) {
        snprintf(buf, sizeof(buf), "%s, DST %s, UTC%+03d:%02d", local.abbr, local.isDst ? "active" : "not active",
                 local.utcOffsetMinutes / 60, abs(local.utcOffsetMinutes % 60));
        if (Row(req, "Current DST state", buf, "Measured (TimeUtil_ToLocal against the current clock)") != ESP_OK) return ESP_FAIL;
    } else {
        if (Row(req, "Current DST state", "unknown (clock not synchronized yet)", "TimeUtil_IsSynced() is false") != ESP_OK) return ESP_FAIL;
    }
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    // ---- Feature switches (read from the in-RAM flags; no extra work) ----
    if (Send(req, "<h3>Features</h3><table><tr><th>Feature</th><th>State</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    for (int f = 0; f < FEATURE_COUNT; f++) {
        if (Row(req, Features_Name((FeatureId)f), Features_Get((FeatureId)f) ? "ON" : "OFF",
                "Persistent setting (NVS \"radar\"), applied immediately; Setup page > Features") != ESP_OK)
            return ESP_FAIL;
    }
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    if (RebootSection(req) != ESP_OK || GroupClose(req) != ESP_OK)
        return ESP_FAIL;

    // ---- RAM ----
    if (GroupOpen(req, "memory", "Memory / Resources", true) != ESP_OK)
        return ESP_FAIL;
    unsigned internalFree = (unsigned)s_core->intFree;
    unsigned internalLargest = (unsigned)s_core->intLargest;
    unsigned internalMinEver = (unsigned)s_core->intMin;
    unsigned psramFree = (unsigned)s_core->psFree;
    unsigned psramLargest = (unsigned)s_core->psLargest;
    unsigned psramMinEver = (unsigned)s_core->psMin;
    if (Send(req, "<h3>RAM</h3><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", internalFree);
    if (Row(req, "Internal heap free (now)", buf, "Measured (heap_caps_get_free_size)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", internalLargest);
    if (Row(req, "Internal heap largest free block", buf, "Measured (heap_caps_get_largest_free_block)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)s_core->dmaLargest);
    if (Row(req, "DMA-capable internal largest free block", buf,
            "Measured (MALLOC_CAP_DMA|INTERNAL). This, not the row above, limits TLS: hardware AES needs 1600-byte "
            "DMA bounce buffers. The row above can read ~7680 from the non-DMA RTC FAST region.") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)s_core->dmaFree);
    if (Row(req, "DMA-capable internal free (now)", buf,
            "Measured (heap_caps_get_free_size, MALLOC_CAP_DMA|INTERNAL). Compare with the largest-block row: "
            "plenty free but a small largest block means fragmentation") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)s_core->dmaMin);
    if (Row(req, "DMA-capable internal min-ever free (since boot)", buf,
            "Measured (heap_caps_get_minimum_free_size, MALLOC_CAP_DMA|INTERNAL)") != ESP_OK) return ESP_FAIL;
    {
        uint32_t fails = s_core->fails;
        FailedAllocText(buf, sizeof(buf));
        if (Row(req, "Failed heap allocations (since boot)", buf,
                "Counted by the failed-allocation hook in every mode (details on the serial console only in Expert Debug). "
                "Last-failure time: the hook stores the uptime (esp_timer, safe in that context); it is converted to "
                "local time when this page is drawn, or shown as T+seconds while the clock is not synchronized") != ESP_OK) return ESP_FAIL;
        if (fails)
        {
            char task[20];
            WebUtil_EscapeHtml(task, sizeof(task), s_core->failTask);
            snprintf(buf, sizeof(buf), "%u bytes, caps 0x%08x, task %s",
                     (unsigned)s_core->failSize, (unsigned)s_core->failCaps, task);
            if (Row(req, "Last failed allocation", buf,
                    "caps 0x80c = 8BIT|DMA|INTERNAL (Wi-Fi driver), 0x8 = DMA (AES), 0x808 = DMA|INTERNAL (SHA)") != ESP_OK) return ESP_FAIL;
        }
    }
    snprintf(buf, sizeof(buf), "%u bytes", internalMinEver);
    if (Row(req, "Internal heap min-ever free (since boot)", buf, "Measured (heap_caps_get_minimum_free_size)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", psramFree);
    if (Row(req, "PSRAM free (now)", buf, psramFree > 0 ? "Measured" : "Measured (0 = PSRAM absent, disabled, or exhausted)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", psramLargest);
    if (Row(req, "PSRAM largest free block", buf, "Measured") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", psramMinEver);
    if (Row(req, "PSRAM min-ever free (since boot)", buf, "Measured") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)s_core->httpdStackMin);
    if (Row(req, "This HTTP request's task, stack min-free (since boot)", buf,
            "Measured (uxTaskGetStackHighWaterMark; httpd task, stack_size=16384)") != ESP_OK ||
        Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    // ---- Flash / filesystem ----
    size_t spiffsTotal = 0, spiffsUsed = 0;
    bool haveSpiffs = esp_spiffs_info(NULL, &spiffsTotal, &spiffsUsed) == ESP_OK;
    if (MinTimingSection(req) != ESP_OK)
        return ESP_FAIL;

    if (Send(req, "<h3>Flash / filesystem</h3><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK ||
        Row(req, "Partition layout", "factory app 2.5 MB, spiffs 1,507,328 bytes (1.4375 MB), nvs 24 KB, phy_init 4 KB (4 MB flash total)",
            "Measured (partitions.csv, build-config)") != ESP_OK)
        return ESP_FAIL;

    /* 0.0.29: physical flash vs the application partition vs the image in it. */
    {
        uint32_t physical = 0, configured = 0;
        const bool havePhysical = esp_flash_get_physical_size(NULL, &physical) == ESP_OK && physical > 0;
        const bool haveConfigured = esp_flash_get_size(NULL, &configured) == ESP_OK && configured > 0;
        if (havePhysical)
            snprintf(buf, sizeof(buf), "%u bytes (%u MB)", (unsigned)physical, (unsigned)(physical / (1024u * 1024u)));
        else
            snprintf(buf, sizeof(buf), "unavailable");
        char ev[96];
        if (haveConfigured)
            snprintf(ev, sizeof(ev), "Measured (esp_flash_get_physical_size; image header configures %u bytes)", (unsigned)configured);
        else
            snprintf(ev, sizeof(ev), "Measured (esp_flash_get_physical_size)");
        if (Row(req, "Flash chip (physical total)", buf, ev) != ESP_OK) return ESP_FAIL;

        const AppImageInfo *app = AppImage();
        if (app->partSize) {
            snprintf(buf, sizeof(buf), "%u bytes (%s at 0x%X)", (unsigned)app->partSize, app->label, (unsigned)app->partAddr);
            if (Row(req, "App partition total", buf, "Measured (running partition, live partition table)") != ESP_OK) return ESP_FAIL;
        } else if (Row(req, "App partition total", "unavailable", "Unknown (esp_ota_get_running_partition returned none)") != ESP_OK) {
            return ESP_FAIL;
        }
        if (app->haveImage) {
            char usedPct[16], freePct[16];
            FormatUsagePercents(app->imageLen, app->partSize, usedPct, freePct, sizeof(usedPct));
            snprintf(buf, sizeof(buf), "%u / %u bytes", (unsigned)app->imageLen, (unsigned)app->partSize);
            if (Row(req, "App image used / total", buf,
                    "Measured (esp_image_get_metadata image_len: image on flash incl. checksum and SHA-256, = the flashed .bin size)") != ESP_OK)
                return ESP_FAIL;
            snprintf(buf, sizeof(buf), "%u bytes", (unsigned)(app->partSize - app->imageLen));
            if (Row(req, "App free (room left in the partition)", buf, "Calculated (partition total - image)") != ESP_OK ||
                Row(req, "App used", usedPct, "Calculated (image / partition total)") != ESP_OK ||
                Row(req, "App free", freePct, "Calculated (100% - used, one decimal)") != ESP_OK)
                return ESP_FAIL;
        } else if (Row(req, "App image used / total", "unavailable",
                       "Unknown (esp_image_get_metadata could not read the running image; nothing is estimated)") != ESP_OK) {
            return ESP_FAIL;
        }
    }

    if (haveSpiffs) {
        snprintf(buf, sizeof(buf), "%u / %u bytes", (unsigned)spiffsUsed, (unsigned)spiffsTotal);
        if (Row(req, "SPIFFS used / total", buf,
                "Measured (esp_spiffs_info; total = usable filesystem capacity, less than the 1,507,328-byte partition because of SPIFFS overhead)") != ESP_OK)
            return ESP_FAIL;
        if (spiffsUsed <= spiffsTotal) {
            char usedPct[16], freePct[16];
            FormatUsagePercents(spiffsUsed, spiffsTotal, usedPct, freePct, sizeof(usedPct));
            snprintf(buf, sizeof(buf), "%u bytes", (unsigned)(spiffsTotal - spiffsUsed));
            if (Row(req, "SPIFFS free", buf, "Calculated (total - used)") != ESP_OK ||
                Row(req, "SPIFFS used", usedPct, "Calculated (used / total)") != ESP_OK ||
                Row(req, "SPIFFS free", freePct, "Calculated (100% - used, one decimal)") != ESP_OK)
                return ESP_FAIL;
        } else if (Row(req, "SPIFFS free", "n/a", "SPIFFS reported used > total (filesystem needs a check)") != ESP_OK) {
            return ESP_FAIL;
        }
    } else if (Row(req, "SPIFFS used / total", "unavailable", "Unknown without runtime measurement (esp_spiffs_info failed - not mounted?)") != ESP_OK) {
        return ESP_FAIL;
    }
    /* NVS (settings) partition usage (0.0.32): NVS writes a changed blob's new copy
     * before erasing the old one, so a blob save needs at least its own size free. */
    {
        nvs_stats_t ns;
        if (nvs_get_stats(NULL, &ns) == ESP_OK && ns.total_entries > 0) {
            snprintf(buf, sizeof(buf), "%u used / %u free / %u total entries (%u%% used, 32 B each)",
                     (unsigned)ns.used_entries, (unsigned)ns.free_entries, (unsigned)ns.total_entries,
                     (unsigned)((ns.used_entries * 100u) / ns.total_entries));
            if (Row(req, "NVS (settings) entries", buf, "Measured (nvs_get_stats, default nvs partition; includes Wi-Fi/PHY data)") != ESP_OK)
                return ESP_FAIL;
        } else if (Row(req, "NVS (settings) entries", "unavailable", "nvs_get_stats failed") != ESP_OK) {
            return ESP_FAIL;
        }
        snprintf(buf, sizeof(buf), "%u bytes (%u location(s))", (unsigned)Airports_StoredBlobBytes(), (unsigned)Airports_Count());
        if (Row(req, "Saved locations blob", buf, "Calculated (4-byte header + 52 B per used location; NVS needs this much free to save a change)") != ESP_OK)
            return ESP_FAIL;
    }
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    if (GroupClose(req) != ESP_OK)
        return ESP_FAIL;

    // ---- Aircraft / tracking ----
    if (GroupOpen(req, "radar", CoreNeedsAttention(s_core) ? "Radar / Provider <span style='color:#c00'>- ATTENTION</span>" : "Radar / Provider", true) != ESP_OK)
        return ESP_FAIL;
    int maxEver = s_core->aircraftMax;
    if (Send(req, "<h3>Aircraft tracking</h3><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%d / %d", s_core->aircraft, MAX_AIRCRAFT);
    if (Row(req, "Current aircraft / capacity", buf, "Measured / Calculated (gAircraftCount vs MAX_AIRCRAFT)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%d", maxEver);
    if (Row(req, "Max aircraft observed since boot", buf, "Measured (tracked since this build; see main.c)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)(MAX_AIRCRAFT * sizeof(Aircraft)));
    if (Row(req, "gAircraft[] static allocation", buf, "Calculated (MAX_AIRCRAFT * sizeof(Aircraft))") != ESP_OK) return ESP_FAIL;
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;
    if (OpsSection(req) != ESP_OK || ProviderPollSection(req) != ESP_OK || NorthRefSection(req) != ESP_OK)
        return ESP_FAIL;

    if (GroupClose(req) != ESP_OK)
        return ESP_FAIL;

    // ---- Configuration storage (custom rules, operators, airports, seen history) ----
    {
        // Open the Storage group automatically when the History index needs attention.
        const TfHistoryStats tfs = s_core->tf;
        TfInitInfo tfi;
        TfHistory_GetInitInfo(&tfi);
        bool attention = tfs.indexLoadState == TF_LOAD_CAP || tfs.indexLoadState == TF_LOAD_WARN ||
                         tfi.indexState == TF_INDEX_FAILED;
        if (GroupOpen(req, "storage", "Storage / History", attention) != ESP_OK)
            return ESP_FAIL;
    }
    if (Send(req, "<h3>Configuration &amp; history storage</h3><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u / %u", (unsigned)CustomRules_Count(), (unsigned)MAX_CUSTOM_RULES);
    if (Row(req, "Custom rules / cap", buf, "Measured / Calculated") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u / %u", (unsigned)Operators_Count(), (unsigned)MAX_OPERATORS);
    if (Row(req, "Operator rows / cap", buf, "Measured / Calculated") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)Airports_Count());
    if (Row(req, "Locations configured (airports & special air traffic, cap 100)", buf, "Measured") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u / %u", (unsigned)s_core->seenCount, (unsigned)SEEN_MAX_RECORDS);
    if (Row(req, "Seen Aircraft records / cap", buf, "Measured / Calculated") != ESP_OK) return ESP_FAIL;
    if (Row(req, "Hot Seen eviction policy", SeenEvictionPolicy_Name(SeenAircraft_GetEvictionPolicy()),
            "Setting (NVS radar/seenpol); decides which record is dropped when the table is full") != ESP_OK) return ESP_FAIL;
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    // ---- Persistence timing (Phase 2 audit instrumentation) ----
    // Cheap counters updated inside the Seen persistence write; added to
    // trace that operation against the reported display flicker, and to
    // measure the incremental (dirty-records-only) rewrite that replaced
    // the original full-file-every-flush design. See PROJECT_STATE.md
    // "Persistence timing diagnostics".
    const SeenPersistStats ps = s_core->seen;
    if (Send(req, "<h3>Persistence (Seen history flush)</h3><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)ps.flushCount);
    if (Row(req, "Flushes since boot", buf, "Measured (counter in WriteBinaryLocked)") != ESP_OK) return ESP_FAIL;
    if (OpTimingRows(req, DT_OP_SEEN_FLUSH, "Last flush", false) != ESP_OK) return ESP_FAIL;
    if (ps.flushCount == 0) {
        if (Row(req, "Last / worst duration", "no flush yet", "N/A") != ESP_OK) return ESP_FAIL;
    } else {
        if (Row(req, "Last flush was a full rewrite?", ps.lastWasFullRewrite ? "yes (first save/migration)" : "no (incremental)", "Measured (which write path ran)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u ms / %u ms", (unsigned)ps.lastDurationMs, (unsigned)ps.worstDurationMs);
        if (Row(req, "Last / worst duration", buf, "Measured (esp_timer_get_time around the write)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%ld bytes / %ld bytes", (long)ps.lastInternalHeapDeltaBytes, (long)ps.worstInternalHeapDeltaBytes);
        if (Row(req, "Last / worst internal heap consumed", buf, "Measured (heap_caps_get_free_size before/after)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%ld bytes", (long)ps.lastPsramDeltaBytes);
        if (Row(req, "Last PSRAM consumed", buf, "Measured (heap_caps_get_free_size before/after)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u us (%0.1f ms) / %u us (%0.1f ms)",
                 (unsigned)ps.lastFormatUs, ps.lastFormatUs / 1000.0,
                 (unsigned)ps.lastWriteUs, ps.lastWriteUs / 1000.0);
        if (Row(req, "Last flush: seek time / write time", buf, "Measured (esp_timer_get_time around fseek vs fwrite, per changed record)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u bytes", (unsigned)ps.lastMinInternalHeapDuringBytes);
        if (Row(req, "Last flush: min internal heap seen mid-write", buf, "Measured (heap_caps_get_free_size sampled periodically during the write)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u", (unsigned)ps.lastRecordCount);
        if (Row(req, "Records actually written last flush", buf, "Measured (dirty count for an incremental flush, all of them for a full rewrite)") != ESP_OK) return ESP_FAIL;
    }
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    // ---- Universal Values ----
    // On-demand only, same as every other section: reads the existing
    // in-RAM table, no scan of TF triggered just to show this.
    UniversalValueStats uvStats;
    UniversalValue_GetStats(&uvStats);
    if (Send(req, "<h3>Universal Values</h3><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u / %u", (unsigned)UniversalValue_ActiveCount(), (unsigned)MAX_UNIVERSAL_VALUES);
    if (Row(req, "Active Universal Values / cap", buf, "Measured / Calculated (one slot per operators.csv row - see PROJECT_STATE.md)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)UniversalValue_Count());
    if (Row(req, "Table slots in use (active + inactive)", buf, "Measured (inactive entries retain identity/fingerprint for reactivation)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)(MAX_UNIVERSAL_VALUES * sizeof(UniversalValue)));
    if (Row(req, "Table allocation (PSRAM heap, not static)", buf, "Calculated (MAX_UNIVERSAL_VALUES * sizeof(UniversalValue))") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)uvStats.lookups);
    if (Row(req, "Lookups / misses", buf, "Measured") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)uvStats.lookupMisses);
    if (Row(req, "  (misses, of the above)", buf, "Measured") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)uvStats.created);
    if (Row(req, "Created since boot", buf, "Measured") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)uvStats.reactivated);
    if (Row(req, "Reactivated since boot", buf, "Measured (an inactive entry matched again by operator code)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)uvStats.codeReuseCount);
    if (Row(req, "Numeric codes reused", buf, "Measured (16-bit code handed to a different canonical identity than last held it)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)uvStats.fingerprintCollisions);
    if (Row(req, "Fingerprint collisions", buf, "Measured (two different canonical identities briefly hashing the same before deterministic resolution)") != ESP_OK) return ESP_FAIL;
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    // ---- TF (persistent) history ----
    if (TfSection(req) != ESP_OK || FlushSection(req) != ESP_OK || LogSection(req) != ESP_OK)
        return ESP_FAIL;

    if (GroupClose(req) != ESP_OK)
        return ESP_FAIL;

    if (GroupOpen(req, "debug", "Debugging", false) != ESP_OK ||
        DebugSection(req) != ESP_OK || GroupClose(req) != ESP_OK)
        return ESP_FAIL;

    if (GroupOpen(req, "adv", "Advanced / Expert", false) != ESP_OK ||
        AdvancedSection(req) != ESP_OK || GroupClose(req) != ESP_OK)
        return ESP_FAIL;

    // Deep links (/diag#tf, #advanced, #expert, #reboot, #debug) open the group that holds the anchor.
    if (Send(req, "<script>(function(){var h=location.hash.slice(1);if(!h)return;var e=document.getElementById(h);"
                  "while(e){if(e.tagName==='DETAILS')e.open=true;e=e.parentElement;}"
                  "var t=document.getElementById(h);if(t)t.scrollIntoView();})();</script>") != ESP_OK)
        return ESP_FAIL;

    if (Send(req,
        "<p style='font-size:.85em;color:#666'>Frame/render timing and per-task stack high-water marks "
        "for tasks other than this HTTP request are not shown here: ESP-IDF only reports a task's own "
        "high-water mark from within that task, and adding cross-task sampling would be exactly the "
        "\"telemetry framework\" this page is deliberately avoiding. See PROJECT_STATE.md for how to "
        "get those numbers with a serial-log capture if a future change needs them.</p>"
        "</body></html>") != ESP_OK)
        return ESP_FAIL;

    return ESP_OK;
}

/* ---- 0.1.7: /diag?fmt=txt | fmt=json ---- */

static int ExportSink(void *ctx, const char *data, size_t len)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, data, (ssize_t)len) == ESP_OK ? 0 : -1;
}

static void JsonU(DiagExport *ex, const char *key, uint64_t v, bool comma)
{
    DiagExport_Printf(ex, "%s\"%s\":%llu", comma ? "," : "", key, (unsigned long long)v);
}

static void JsonB(DiagExport *ex, const char *key, bool v, bool comma)
{
    DiagExport_Printf(ex, "%s\"%s\":%s", comma ? "," : "", key, v ? "true" : "false");
}

static void JsonS(DiagExport *ex, const char *key, const char *v, bool comma)
{
    DiagExport_Printf(ex, "%s\"%s\":", comma ? "," : "", key);
    DiagExport_JsonString(ex, v);
}

/* "<key>_uptime_s":n,"<key>_utc":n|null[,"<key>_utc_null_reason":"..."] */
static void JsonStamp(DiagExport *ex, const char *key, const DiagStamp *st, bool comma)
{
    DiagExport_Printf(ex, "%s\"%s_uptime_s\":%u,\"%s_utc\":", comma ? "," : "", key, (unsigned)st->uptimeSec, key);
    if (st->utc > 0)
        DiagExport_Printf(ex, "%lld", (long long)st->utc);
    else
        DiagExport_Printf(ex, "null,\"%s_utc_null_reason\":\"clock not synchronized when recorded\"", key);
}

static void JsonHeap(DiagExport *ex, const char *key, uint32_t freeB, uint32_t largest, uint32_t minEver, bool comma)
{
    DiagExport_Printf(ex, "%s\"%s\":{\"free_bytes\":%u,\"largest_free_block_bytes\":%u,\"min_ever_free_bytes\":%u}",
                      comma ? "," : "", key, (unsigned)freeB, (unsigned)largest, (unsigned)minEver);
}

static const char *IndexLoadName(uint8_t st)
{
    return st == TF_LOAD_CAP ? "cap" : st == TF_LOAD_WARN ? "warn" : "ok";
}

/* 0.1.8 console-log capture: ,"log_capture":{...}. Counters are null (with a reason) while capture is not active. */
static void JsonLogCapture(DiagExport *ex, const LogCaptureStatus *st)
{
    DiagExport_Write(ex, ",\"log_capture\":{");
    JsonB(ex, "saved_setting", st->saved, false);
    JsonB(ex, "active", st->active, true);
    if (!st->active) {
        JsonS(ex, "inactive_reason", st->inactiveReason ? st->inactiveReason : "", true);
        DiagExport_Write(ex, ",\"counters\":null,\"counters_null_reason\":\"capture not active this boot\"}");
        return;
    }
    const LogCapCounters *k = &st->ctr;
    JsonS(ex, "writer_state", LogWriterState_Name(st->state), true);
    JsonS(ex, "writer_reason", st->reason, true);
    JsonU(ex, "boot", st->boot, true);
    JsonU(ex, "segment", st->seg, true);
    if (st->file[0]) {
        JsonS(ex, "current_file", st->file, true);
        JsonU(ex, "current_file_bytes", st->fileSize, true);
    } else {
        DiagExport_Write(ex, ",\"current_file\":null,\"current_file_bytes\":null,\"current_file_null_reason\":\"no file open yet\"");
    }
    DiagExport_Write(ex, ",\"counters\":{");
    JsonU(ex, "buffer_bytes", st->ringSize, false);
    JsonU(ex, "buffer_used_bytes", st->ringUsed, true);
    JsonU(ex, "buffer_high_water_bytes", st->ringHighWater, true);
    JsonU(ex, "lines", k->lines, true);
    JsonU(ex, "bytes", k->bytes, true);
    JsonU(ex, "dropped_full_lines", k->dropFullLines, true);
    JsonU(ex, "dropped_full_bytes", k->dropFullBytes, true);
    JsonU(ex, "dropped_busy_lines", k->dropBusyLines, true);
    JsonU(ex, "dropped_context_lines", k->dropContextLines, true);
    JsonU(ex, "dropped_storage_lock_lines", k->dropLockLines, true);
    JsonU(ex, "writer_own_lines_not_saved", k->ownLines, true);
    JsonU(ex, "truncated_lines", k->truncated, true);
    JsonU(ex, "redactions", k->redactions, true);
    JsonU(ex, "writes", st->writes, true);
    JsonU(ex, "bytes_written", st->bytesWritten, true);
    JsonU(ex, "write_errors", st->writeErrors, true);
    JsonU(ex, "close_errors", st->closeErrors, true);
    JsonU(ex, "lock_timeouts", st->lockTimeouts, true);
    JsonU(ex, "files_created", st->filesCreated, true);
    JsonU(ex, "files_deleted_by_retention", st->filesDeleted, true);
    JsonU(ex, "retention_failures", st->retentionFailures, true);
    JsonU(ex, "interrupted_logs_marked", st->interruptedMarked, true);
    JsonU(ex, "space_suspensions", st->spaceSuspends, true);
    JsonU(ex, "dma_deferred_write_cycles", st->deferredCycles, true);
    JsonU(ex, "dma_deferral_episodes", st->deferEpisodes, true);
    JsonU(ex, "dma_longest_deferral_s", st->longestDeferS, true);
    DiagExport_Write(ex, "}");
    JsonB(ex, "dma_deferring_now", st->deferring, true);
    if (st->deferring)
        JsonU(ex, "dma_deferring_since_uptime_s", st->deferStartS, true);
    else
        DiagExport_Write(ex, ",\"dma_deferring_since_uptime_s\":null,\"dma_deferring_since_uptime_s_null_reason\":\"not deferring\"");
    JsonU(ex, "dma_min_block_for_writes_bytes", LOGCAP_DMA_MIN_BYTES, true);
    if (st->haveFreeBytes)
        JsonU(ex, "card_free_bytes_last_check", st->lastFreeBytes, true);
    else
        DiagExport_Write(ex, ",\"card_free_bytes_last_check\":null,\"card_free_bytes_last_check_null_reason\":\"not checked yet\"");
    if (st->lastError[0]) {
        JsonS(ex, "last_error", st->lastError, true);
        JsonU(ex, "last_error_uptime_s", st->lastErrorUptimeS, true);
    } else {
        DiagExport_Write(ex, ",\"last_error\":null,\"last_error_null_reason\":\"no error since boot\"");
    }
    if (st->writerStackMinBytes)
        JsonU(ex, "writer_stack_min_free_bytes", st->writerStackMinBytes, true);
    else
        DiagExport_Write(ex, ",\"writer_stack_min_free_bytes\":null,\"writer_stack_min_free_bytes_null_reason\":\"not measured yet\"");
    DiagExport_Write(ex, "}");
}

/* 0.1.9: ,"last_poll":{...} for provider p, or null with a reason. */
static void JsonLastPoll(DiagExport *ex, const DiagCore *c, int p)
{
    const ProviderPollDiag *d = &c->poll[p];
    if (!c->havePoll[p] || !d->polled) {
        DiagExport_Printf(ex, ",\"last_poll\":null,\"last_poll_null_reason\":\"%s\"",
                          c->havePoll[p] ? "no request this boot" : "not recorded (no memory for the record)");
        return;
    }
    char url[sizeof(d->url)];
    snprintf(url, sizeof(url), "%s", d->url);
    LogCap_Redact(url, strlen(url));
    DiagExport_Write(ex, ",\"last_poll\":{");
    JsonU(ex, "request_seq", d->seq, false);
    JsonU(ex, "start_uptime_s", d->startUptimeS, true);
    JsonU(ex, "duration_ms", d->durationMs, true);
    JsonS(ex, "url_redacted", url, true);
    JsonS(ex, "outcome", ProviderPollOutcome_Name(d->outcome), true);
    JsonS(ex, "fetch_outcome", ProviderPollOutcome_Name(d->fetchOutcome), true);
    if (d->httpStatus)
        DiagExport_Printf(ex, ",\"http_status\":%d", d->httpStatus);
    else
        DiagExport_Write(ex, ",\"http_status\":null,\"http_status_null_reason\":\"no HTTP response\"");
    JsonU(ex, "body_bytes_kept", d->bytes, true);
    JsonU(ex, "body_bytes_received", d->received, true);
    if (d->contentLength >= 0)
        DiagExport_Printf(ex, ",\"content_length_bytes\":%lld", (long long)d->contentLength);
    else
        DiagExport_Write(ex, ",\"content_length_bytes\":null,\"content_length_bytes_null_reason\":\"not sent (chunked) or no response\"");
    JsonB(ex, "truncated", d->truncated, true);
    JsonB(ex, "client_reports_complete", d->complete, true);
    JsonS(ex, "transport_failure", ProviderTransportKind_Name(d->transport), true);
    DiagExport_Printf(ex, ",\"esp_err\":%d,\"esp_tls_error\":%d,\"esp_tls_flags\":%d", d->espErr, d->tlsErr, d->tlsFlags);
    if (d->haveParse) {
        const ProviderParseStats *s = &d->parse;
        DiagExport_Printf(ex, ",\"parse\":{\"entries\":%d,\"malformed\":%d,\"missing_id\":%d,\"missing_position\":%d,"
                              "\"invalid_position\":%d,\"on_ground\":%d,\"ground_shown\":%d,\"not_examined_list_full\":%d,"
                              "\"admitted\":%d}",
                          s->entries, s->malformed, s->missingId, s->missingPosition, s->invalidPosition, s->onGround,
                          s->groundShown, s->notExamined, s->admitted);
    } else {
        DiagExport_Write(ex, ",\"parse\":null,\"parse_null_reason\":\"the response was not parsed (see outcome)\"");
    }
    if (d->jsonErrorOffset >= 0)
        DiagExport_Printf(ex, ",\"json_error_offset\":%d", d->jsonErrorOffset);
    if (d->haveSuccess)
        JsonU(ex, "last_success_uptime_s", d->lastSuccessUptimeS, true);
    else
        DiagExport_Write(ex, ",\"last_success_uptime_s\":null,\"last_success_uptime_s_null_reason\":\"no successful poll this boot\"");
    if (d->haveError) {
        JsonU(ex, "last_error_uptime_s", d->lastErrorUptimeS, true);
        JsonS(ex, "last_error", d->lastError, true);
    } else {
        DiagExport_Write(ex, ",\"last_error\":null,\"last_error_null_reason\":\"no failed poll this boot\"");
    }
    DiagExport_Write(ex, "}");
}

/* Typed core metrics (schema 1, documented in README "Diagnostics export"). Units are in the key names. */
static void JsonMetrics(DiagExport *ex, const DiagCore *c)
{
    DiagExport_Write(ex, ",\n\"metrics\":{");
    JsonS(ex, "firmware", FW_VERSION_STRING, false);
    JsonU(ex, "uptime_s", (uint64_t)(c->uptimeUs / 1000000), true);
    DiagExport_Write(ex, ",\"clock\":{");
    JsonB(ex, "synced", c->utc > 0, false);
    if (c->utc > 0)
        DiagExport_Printf(ex, ",\"utc\":%lld", (long long)c->utc);
    else
        DiagExport_Write(ex, ",\"utc\":null,\"utc_null_reason\":\"clock not synchronized\"");
    {
        TimeZoneConfig tz;
        TimeUtil_GetConfig(&tz);
        JsonS(ex, "time_zone", tz.zoneId, true);
        JsonB(ex, "auto_dst", tz.autoDst, true);
        TimeLocal tl;
        if (c->utc > 0 && TimeUtil_ToLocal(c->utc, &tl)) {
            char local[48];
            snprintf(local, sizeof(local), "%04d-%02d-%02d %02d:%02d:%02d %s", tl.year, tl.month, tl.day, tl.hour,
                     tl.minute, tl.second, tl.abbr);
            JsonS(ex, "local", local, true);
            JsonB(ex, "dst_active", tl.isDst, true);
            DiagExport_Printf(ex, ",\"utc_offset_min\":%d", tl.utcOffsetMinutes);
        } else {
            DiagExport_Write(ex, ",\"local\":null,\"dst_active\":null,\"utc_offset_min\":null,"
                                 "\"local_null_reason\":\"clock not synchronized\"");
        }
    }
    DiagExport_Write(ex, "},\"heap\":{");
    JsonHeap(ex, "internal", c->intFree, c->intLargest, c->intMin, false);
    JsonHeap(ex, "dma_internal", c->dmaFree, c->dmaLargest, c->dmaMin, true);
    JsonHeap(ex, "psram", c->psFree, c->psLargest, c->psMin, true);
    JsonU(ex, "httpd_stack_min_free_bytes", c->httpdStackMin, true);
    DiagExport_Write(ex, "},\"heap_minimum_first_seen\":[");
    static const char *const kHeapKey[DT_HEAP_COUNT] = {"internal", "dma_internal", "psram"};
    for (int k = 0; k < DT_HEAP_COUNT; k++) {
        DiagExport_Printf(ex, "%s{\"heap\":\"%s\"", k ? "," : "", kHeapKey[k]);
        if (c->minHeapValid[k]) {
            JsonU(ex, "min_free_bytes", c->minHeap[k].minFree, true);
            JsonStamp(ex, "first_seen", &c->minHeap[k].when, true);
        } else {
            DiagExport_Write(ex, ",\"min_free_bytes\":null,\"min_free_bytes_null_reason\":\"not sampled yet\"");
        }
        DiagExport_Write(ex, "}");
    }
    DiagExport_Write(ex, "],\"stack_minimums\":[");
    for (size_t i = 0; i < c->nStacks; i++) {
        DiagExport_Printf(ex, "%s{", i ? "," : "");
        JsonS(ex, "task", c->stacks[i].name, false);
        JsonU(ex, "min_free_bytes", c->stacks[i].minFreeBytes, true);
        JsonStamp(ex, "first_seen", &c->stacks[i].when, true);
        DiagExport_Write(ex, "}");
    }
    DiagExport_Write(ex, "],\"failed_allocations\":{");
    JsonU(ex, "count", c->fails, false);
    if (!c->fails) {
        DiagExport_Write(ex, ",\"last\":null,\"last_null_reason\":\"no failed allocation since boot\"}");
    } else {
        DiagExport_Write(ex, ",\"last\":{");
        JsonU(ex, "size_bytes", c->failSize, false);
        JsonU(ex, "caps", c->failCaps, true);
        char hex[16];
        snprintf(hex, sizeof(hex), "0x%08x", (unsigned)c->failCaps);
        JsonS(ex, "caps_hex", hex, true);
        JsonS(ex, "task", c->failTask, true);
        if (c->failTimeValid) {
            const DiagStamp st = {(uint32_t)(c->failUs / 1000000), CoreFailUtc(c)};
            JsonStamp(ex, "at", &st, true);
        } else {
            DiagExport_Write(ex, ",\"at_uptime_s\":null,\"at_utc\":null,\"at_null_reason\":\"time of the last failure unavailable\"");
        }
        DiagExport_Write(ex, "}}");
    }
    DiagExport_Write(ex, ",\"operations\":[");
    for (int i = 0; i < DT_OP_COUNT; i++) {
        const DiagOpStats *o = &c->ops[i];
        DiagExport_Printf(ex, "%s{", i ? "," : "");
        JsonS(ex, "name", DiagTelemetry_OpName((DiagOp)i), false);
        JsonU(ex, "runs", o->runs, true);
        JsonU(ex, "ok", o->ok, true);
        JsonU(ex, "failures", o->failures, true);
        JsonU(ex, "skipped", o->skipped, true);
        JsonU(ex, "consecutive_failures", o->consecutiveFailures, true);
        if (o->runs) {
            JsonU(ex, "last_ms", o->lastMs, true);
            JsonU(ex, "worst_ms", o->worstMs, true);
            JsonStamp(ex, "last_finish", &o->lastRun, true);
            JsonStamp(ex, "worst_finish", &o->worstWhen, true);
        } else {
            DiagExport_Write(ex, ",\"last_ms\":null,\"worst_ms\":null,\"last_ms_null_reason\":\"no completed run since boot\"");
        }
        if (o->haveFailure) {
            JsonS(ex, "last_failure", o->lastFailure, true);
            JsonStamp(ex, "last_failure_at", &o->lastFailureWhen, true);
        } else {
            DiagExport_Write(ex, ",\"last_failure\":null");
        }
        DiagExport_Write(ex, "}");
    }
    DiagExport_Write(ex, "],\"providers\":[");
    for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++) {
        const AircraftProviderType t = (AircraftProviderType)p;
        DiagExport_Printf(ex, "%s{", p ? "," : "");
        JsonS(ex, "name", AircraftProviderType_Name(t), false);
        JsonB(ex, "enabled", AircraftProvider_IsEnabled(t), true);
        JsonU(ex, "interval_s", AircraftProvider_GetIntervalSeconds(t), true);
        JsonU(ex, "rate_limit_backoff_s", AircraftProvider_GetRateLimitSecondsFor(t), true);
        JsonS(ex, "fetch_operation", DiagTelemetry_OpName(t == AIRCRAFT_PROVIDER_ADSBLOL ? DT_OP_FETCH_ADSBLOL : DT_OP_FETCH_OPENSKY), true);
        JsonLastPoll(ex, c, p);
        DiagExport_Write(ex, "}");
    }
    DiagExport_Write(ex, "],\"aircraft\":{");
    JsonU(ex, "current", (uint64_t)(c->aircraft > 0 ? c->aircraft : 0), false);
    JsonU(ex, "capacity", MAX_AIRCRAFT, true);
    JsonU(ex, "max_since_boot", (uint64_t)(c->aircraftMax > 0 ? c->aircraftMax : 0), true);
    DiagExport_Write(ex, "},\"storage\":{\"tf\":{");
    const TfHistoryStats *tf = &c->tf;
    JsonB(ex, "mounted", tf->mounted, false);
    JsonB(ex, "history_available", tf->historyAvailable, true);
    if (tf->mounted) {
        JsonU(ex, "capacity_bytes", tf->capacityBytes, true);
        JsonU(ex, "used_bytes", tf->usedBytes, true);
        JsonU(ex, "free_bytes", tf->freeBytes, true);
    } else {
        DiagExport_Write(ex, ",\"capacity_bytes\":null,\"used_bytes\":null,\"free_bytes\":null,"
                             "\"capacity_bytes_null_reason\":\"card not mounted\"");
    }
    JsonU(ex, "index_slots_used", tf->indexSlotsUsed, true);
    JsonU(ex, "index_slots_total", tf->indexSlotsTotal, true);
    JsonS(ex, "index_load", IndexLoadName(tf->indexLoadState), true);
    JsonU(ex, "writes", tf->writes, true);
    JsonU(ex, "creates", tf->creates, true);
    JsonU(ex, "lookups", tf->lookups, true);
    JsonU(ex, "lookup_misses", tf->lookupMisses, true);
    JsonU(ex, "io_or_crc_errors", tf->errors, true);
    JsonU(ex, "index_insert_cap_rejected", tf->indexInsertCapRejected, true);
    JsonU(ex, "index_insert_io_fail", tf->indexInsertIoFail, true);
    DiagExport_Write(ex, "},\"history_manager\":{");
    JsonU(ex, "shadow_slots_used", c->hm.shadowSlotsUsed, false);
    JsonU(ex, "shadow_slots_cap", c->hm.shadowSlotsCap, true);
    JsonU(ex, "dirty", c->hm.dirtyNow, true);
    JsonU(ex, "evict_clean", c->hm.evictClean, true);
    JsonU(ex, "evict_after_write", c->hm.evictAfterWrite, true);
    JsonU(ex, "evict_write_fail", c->hm.evictWriteFail, true);
    JsonU(ex, "admit_skipped", c->hm.admitSkipped, true);
    DiagExport_Write(ex, "},\"seen\":{");
    JsonU(ex, "records", c->seenCount, false);
    JsonU(ex, "capacity", SEEN_MAX_RECORDS, true);
    JsonU(ex, "flushes", c->seen.flushCount, true);
    if (c->seen.flushCount) {
        JsonU(ex, "last_flush_ms", c->seen.lastDurationMs, true);
        JsonU(ex, "worst_flush_ms", c->seen.worstDurationMs, true);
    } else {
        DiagExport_Write(ex, ",\"last_flush_ms\":null,\"worst_flush_ms\":null,\"last_flush_ms_null_reason\":\"no flush since boot\"");
    }
    DiagExport_Write(ex, "}}");
    JsonLogCapture(ex, &c->logcap);
    DiagExport_Write(ex, "}");
}

/* Download file name: fr7-diag_<fw>_<local YYYYMMDD-HHMMSS> or _T<uptime>s when the clock is not synchronized. */
static void ExportFileName(const DiagCore *c, bool json, char *out, size_t cap)
{
    char fw[24];
    size_t n = 0;
    for (const char *p = FW_VERSION_STRING; *p && n + 1 < sizeof(fw); p++)
        fw[n++] = ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '.' || *p == '-') ? *p : '_';
    fw[n] = 0;
    char when[24];
    TimeLocal tl;
    if (c->utc > 0 && TimeUtil_ToLocal(c->utc, &tl))
        snprintf(when, sizeof(when), "%04d%02d%02d-%02d%02d%02d", tl.year, tl.month, tl.day, tl.hour, tl.minute, tl.second);
    else
        snprintf(when, sizeof(when), "T%llus", (unsigned long long)(c->uptimeUs / 1000000));
    snprintf(out, cap, "attachment; filename=\"fr7-diag_%s_%s.%s\"", fw, when, json ? "json" : "txt");
}

static esp_err_t DiagExportRun(httpd_req_t *req, bool json)
{
    const DiagCore *c = s_core;
    httpd_resp_set_type(req, json ? "application/json" : "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ExportFileName(c, json, s_ctx->disposition, sizeof(s_ctx->disposition));
    httpd_resp_set_hdr(req, "Content-Disposition", s_ctx->disposition);

    DiagExport *ex = &s_ctx->ex;
    DiagExport_Begin(ex, json ? DIAG_EXPORT_JSON : DIAG_EXPORT_TEXT, ExportSink, req);
    char when[64];
    const DiagStamp gen = {(uint32_t)(c->uptimeUs / 1000000), c->utc};
    DiagTelemetry_FormatStampLocal(&gen, when, sizeof(when));
    if (json) {
        DiagExport_Write(ex, "{\"format\":\"flight-radar-7 diagnostics\",\"schema\":1,\"firmware\":");
        DiagExport_JsonString(ex, FW_VERSION_STRING);
        DiagExport_Printf(ex, ",\"generated\":{\"uptime_s\":%llu", (unsigned long long)(c->uptimeUs / 1000000));
        if (c->utc > 0)
            DiagExport_Printf(ex, ",\"utc\":%lld", (long long)c->utc);
        else
            DiagExport_Write(ex, ",\"utc\":null,\"utc_null_reason\":\"clock not synchronized\"");
        JsonS(ex, "display", when, true);
        DiagExport_Write(ex, ",\"note\":\"metrics were read once at the start of the export; report rows are rendered "
                             "section by section right after (not one atomic snapshot)\"}");
        JsonMetrics(ex, c);
        DiagExport_Write(ex, ",\n\"report\":[");
    } else {
        DiagExport_Printf(ex, "Flight-radar-7 runtime diagnostics export (text, format 1)\nFirmware: %s\nGenerated: %s; uptime %llu s\n",
                          FW_VERSION_STRING, when, (unsigned long long)(c->uptimeUs / 1000000));
        DiagExport_Write(ex, "Note: memory, failed-allocation, minimum, operation, aircraft and storage counters were read once at "
                             "the start; the other rows are read section by section while the report is generated "
                             "(not one atomic snapshot). Table rows are \"cell | cell | cell\".\n");
    }

    s_export = ex;
    const esp_err_t body = DiagReportBody(req);
    s_export = NULL;
    DiagExport_Finish(ex);
    if (ex->failed)
        return ESP_FAIL; /* client gone: nothing more can be sent, and no completion marker was written */

    if (json)
        DiagExport_Printf(ex, "\n],\"rows\":%u,\"items\":%u,\"render_ok\":%s%s,\"complete\":%s}\n",
                          (unsigned)ex->rows, (unsigned)ex->items, body == ESP_OK ? "true" : "false",
                          body == ESP_OK ? "" : ",\"render_error\":\"a report section could not be generated; the rows above are partial\"",
                          body == ESP_OK ? "true" : "false");
    else if (body == ESP_OK)
        DiagExport_Printf(ex, "\n=== END OF REPORT (complete, %u table rows) ===\n", (unsigned)ex->rows);
    else
        DiagExport_Write(ex, "\n=== END OF REPORT (INCOMPLETE: a report section could not be generated; the rows above are partial) ===\n");
    if (!DiagExport_Flush(ex))
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t DiagPage(httpd_req_t *req)
{
    /* Directly URL-accessible (/diag); intentionally not one of the nav pages. */
    char fmt[8] = "", logName[16] = "";
    {
        char q[96];
        const size_t ql = httpd_req_get_url_query_len(req);
        if (ql > 0 && ql < sizeof(q) && httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            (void)httpd_query_key_value(q, "fmt", fmt, sizeof(fmt));
            const esp_err_t le = httpd_query_key_value(q, "log", logName, sizeof(logName));
            if (le == ESP_ERR_NOT_FOUND)
                logName[0] = 0;
            else if (le != ESP_OK || !LogCap_ValidName(logName)) /* includes a truncated (too long) value */
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a console-log file name (L#######.LOG)");
        }
    }
    const bool txt = strcmp(fmt, "txt") == 0, json = strcmp(fmt, "json") == 0;
    if (fmt[0] && !txt && !json)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown export format (use fmt=txt or fmt=json)");

    if (!s_ctx)
        s_ctx = heap_caps_malloc(sizeof(*s_ctx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ctx) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        char msg[128];
        snprintf(msg, sizeof(msg), "Diagnostics unavailable: no memory for the report context (%u bytes of PSRAM). "
                                   "Nothing was changed; try again.", (unsigned)sizeof(DiagReportCtx));
        return httpd_resp_sendstr(req, msg);
    }
    if (logName[0])
        return LogDownload(req, logName); /* no report rendering; s_ctx supplies the 4 KB chunk buffer */
    DiagCore_Collect(&s_ctx->core);
    s_ctx->core.via = WebAccess_RequestInterface(req);
    s_core = &s_ctx->core;

    esp_err_t r;
    if (txt || json) {
        r = DiagExportRun(req, json);
    } else {
        r = WebStyle_SendHead(req, "Diagnostics", WEBPAGE_NONE,
                "body{max-width:750px}.ev{color:var(--mut);font-size:.85em}h3{margin:1em 0 .3em}h4{margin:.8em 0 .2em}"
                "details.gp{border:1px solid var(--bd);border-radius:6px;margin:.6em 0;padding:0 .7em}"
                "details.gp>summary{cursor:pointer;font-weight:bold;font-size:1.1em;padding:.5em 0}");
        if (r == ESP_OK)
            r = DiagReportBody(req);
        if (r == ESP_OK)
            r = httpd_resp_send_chunk(req, NULL, 0);
    }
    s_core = NULL;
    return r;
}

esp_err_t WebDiag_Register(httpd_handle_t server)
{
    httpd_uri_t diag_uri = {
        .uri = "/diag",
        .method = HTTP_GET,
        .handler = DiagPage,
        .user_ctx = NULL};
    esp_err_t err = httpd_register_uri_handler(server, &diag_uri);
    if (err != ESP_OK)
        return err;
    httpd_uri_t adv_uri = {
        .uri = "/diag/advanced",
        .method = HTTP_POST,
        .handler = AdvancedPost,
        .user_ctx = NULL};
    err = httpd_register_uri_handler(server, &adv_uri);
    if (err != ESP_OK)
        return err;
    httpd_uri_t tf_uri = {
        .uri = "/diag/tf",
        .method = HTTP_POST,
        .handler = TfActionPost,
        .user_ctx = NULL};
    return httpd_register_uri_handler(server, &tf_uri);
}
