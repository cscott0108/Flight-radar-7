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
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "adv_diag.h"
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
#include "universal_value.h"
#include "web_util.h"

static esp_err_t Send(httpd_req_t *req, const char *value)
{
    return httpd_resp_send_chunk(req, value, HTTPD_RESP_USE_STRLEN);
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
    return httpd_resp_send_chunk(req, buffer, length);
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

static esp_err_t Row(httpd_req_t *req, const char *label, const char *value, const char *evidence)
{
    return SendFormat(req,
        "<tr><td>%s</td><td>%s</td><td class='ev'>%s</td></tr>",
        label, value, evidence);
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
        "<option value='VERBOSE'%s>Verbose</option><option value='RAW'%s>Raw</option></select></label>"
        "<p class='ev'>Off: nothing. Normal: request/response status and counts. Verbose: adds per-aircraft type resolution. "
        "Raw: adds a bounded, credential-redacted response preview. Applies to every enabled provider.</p>"
        "<button type='submit'>Save debugging settings</button></form>",
        GetRadarOpenSkyDebugEnabled() ? " checked" : "",
        lvl == PROVIDER_DEBUG_OFF ? " selected" : "", lvl == PROVIDER_DEBUG_NORMAL ? " selected" : "",
        lvl == PROVIDER_DEBUG_VERBOSE ? " selected" : "", lvl == PROVIDER_DEBUG_RAW ? " selected" : "");
    if (n <= 0 || n >= (int)sizeof(form))
        return ESP_FAIL;
    return Send(req, form);
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
        snprintf(buf, sizeof(buf), "%u", (unsigned)ExpertDebug_FailedAllocCount());
        if (Row(req, "Failed heap allocations since boot", buf,
                "Counted by the FAILED_ALLOC hook (details on the serial console)") != ESP_OK) return ESP_FAIL;
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
    // Same endpoint serves both persistent diagnostic modes; no extra httpd slot.
    bool expert = httpd_query_key_value(body, "mode", val, sizeof(val)) == ESP_OK && strcmp(val, "expert") == 0;
    const char *modeName = expert ? "Expert Debug Mode" : "Advanced Diagnostics";
    const char *anchor = expert ? "expert" : "advanced";

    if ((expert ? ExpertDebug_SetSaved(enable) : AdvDiag_SetSaved(enable)) != ESP_OK)
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
    TfHistoryStats tf;
    TfHistory_GetStats(&tf);
    TfInitInfo in;
    TfHistory_GetInitInfo(&in);
    HistoryManagerStats hm;
    HistoryManager_GetStats(&hm);
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
        DiagMinHeap h;
        if (DiagTelemetry_GetHeap((DiagHeapKind)k, &h)) {
            snprintf(buf, sizeof(buf), "%u bytes", (unsigned)h.minFree);
            DiagTelemetry_FormatStamp(&h.when, when, sizeof(when));
        } else {
            snprintf(buf, sizeof(buf), "not sampled yet");
            when[0] = 0;
        }
        if (Row(req, kHeapName[k], buf, when) != ESP_OK) return ESP_FAIL;
    }
    DiagMinStack st[DT_MAX_STACKS];
    size_t n = DiagTelemetry_GetStacks(st, DT_MAX_STACKS);
    for (size_t i = 0; i < n; i++) {
        char label[48];
        snprintf(label, sizeof(label), "Stack min-free: %s", st[i].name);
        snprintf(buf, sizeof(buf), "%u bytes", (unsigned)st[i].minFreeBytes);
        DiagTelemetry_FormatStamp(&st[i].when, when, sizeof(when));
        if (Row(req, label, buf, when) != ESP_OK) return ESP_FAIL;
    }
    return Send(req, "</table><p class='ev'>Stack rows appear once that task has run a sampling pass. Times are seconds since boot, plus UTC once the clock has synced.</p>");
}

// ---- Recurring operations (step 7) ----
static esp_err_t OpsSection(httpd_req_t *req)
{
    if (Send(req, "<h3 id='ops'>Recurring operations</h3><table><tr><th>Operation</th><th>Runs / failed / skipped</th>"
                  "<th>Failing in a row</th><th>Last / worst</th><th>Last failure</th></tr>") != ESP_OK)
        return ESP_FAIL;
    for (int i = 0; i < DT_OP_COUNT; i++) {
        DiagOpStats o;
        DiagTelemetry_GetOp((DiagOp)i, &o);
        char when[80], last[160];
        if (o.haveFailure) {
            DiagTelemetry_FormatStamp(&o.lastFailureWhen, when, sizeof(when));
            snprintf(last, sizeof(last), "%s (%s)", o.lastFailure, when);
        } else {
            snprintf(last, sizeof(last), "none");
        }
        if (SendFormat(req, "<tr><td>%s</td><td>%u / %u / %u</td><td>%s%u</td><td>%u ms / %u ms</td><td class='ev'>%s</td></tr>",
                       DiagTelemetry_OpName((DiagOp)i), (unsigned)o.runs, (unsigned)o.failures, (unsigned)o.skipped,
                       o.consecutiveFailures >= DT_ATTENTION_CONSECUTIVE ? "<b style='color:#c00'>ATTENTION </b>" : "",
                       (unsigned)o.consecutiveFailures, (unsigned)o.lastMs, (unsigned)o.worstMs, last) != ESP_OK)
            return ESP_FAIL;
    }
    return Send(req, "</table><p class='ev'>Since boot. A skipped run is one that could not start (rate-limit backoff, TF unavailable); "
                     "it is not a failure. History flush only counts passes that had entries to write. "
                     "ATTENTION appears after 3 failures in a row and clears on the next success.</p>");
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

// POST /diag/tf  act=selftest | unmount | reinit | reboot | flush  (one handler slot for all)
static esp_err_t TfActionPost(httpd_req_t *req)
{
    char body[48] = {0};
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

static esp_err_t DiagPage(httpd_req_t *req)
{
    /* Directly URL-accessible (/diag); intentionally not one of the nav pages. */
    if (WebStyle_SendHead(req, "Diagnostics", WEBPAGE_NONE,
            "body{max-width:750px}.ev{color:var(--mut);font-size:.85em}h3{margin:1em 0 .3em}h4{margin:.8em 0 .2em}"
            "details.gp{border:1px solid var(--bd);border-radius:6px;margin:.6em 0;padding:0 .7em}"
            "details.gp>summary{cursor:pointer;font-weight:bold;font-size:1.1em;padding:.5em 0}") != ESP_OK ||
        Send(req,
        "<h1>Runtime capacity report</h1>"
        "<p>Every value below is read live from this device right now - reload the page for a fresh "
        "snapshot. \"Since boot\" values reset on reboot, not on reload. This page adds negligible "
        "overhead: it only reads existing counters when you open it, nothing runs in the background "
        "for it. Copy the numbers below into a Claude conversation for capacity/design questions - see "
        "PROJECT_STATE.md's \"Runtime diagnostics\" section for what each row is used for.</p>") != ESP_OK)
        return ESP_FAIL;

    // ---- Uptime / identity ----
    int64_t uptimeUs = esp_timer_get_time();
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
    unsigned internalFree = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned internalLargest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned internalMinEver = (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned psramFree = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    unsigned psramLargest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    unsigned psramMinEver = (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (Send(req, "<h3>RAM</h3><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", internalFree);
    if (Row(req, "Internal heap free (now)", buf, "Measured (heap_caps_get_free_size)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", internalLargest);
    if (Row(req, "Internal heap largest free block", buf, "Measured (heap_caps_get_largest_free_block)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (Row(req, "DMA-capable internal largest free block", buf,
            "Measured (MALLOC_CAP_DMA|INTERNAL). This, not the row above, limits TLS: hardware AES needs 1600-byte "
            "DMA bounce buffers. The row above can read ~7680 from the non-DMA RTC FAST region.") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (Row(req, "DMA-capable internal free (now)", buf,
            "Measured (heap_caps_get_free_size, MALLOC_CAP_DMA|INTERNAL). Compare with the largest-block row: "
            "plenty free but a small largest block means fragmentation") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (Row(req, "DMA-capable internal min-ever free (since boot)", buf,
            "Measured (heap_caps_get_minimum_free_size, MALLOC_CAP_DMA|INTERNAL)") != ESP_OK) return ESP_FAIL;
    {
        uint32_t fails = ExpertDebug_FailedAllocCount();
        snprintf(buf, sizeof(buf), "%u", (unsigned)fails);
        if (Row(req, "Failed heap allocations (since boot)", buf,
                "Counted by the failed-allocation hook in every mode (details on the serial console only in Expert Debug)") != ESP_OK) return ESP_FAIL;
        if (fails)
        {
            char task[20];
            WebUtil_EscapeHtml(task, sizeof(task), ExpertDebug_LastFailedTask());
            snprintf(buf, sizeof(buf), "%u bytes, caps 0x%08x, task %s",
                     (unsigned)ExpertDebug_LastFailedSize(), (unsigned)ExpertDebug_LastFailedCaps(), task);
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
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
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
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    if (GroupClose(req) != ESP_OK)
        return ESP_FAIL;

    // ---- Aircraft / tracking ----
    if (GroupOpen(req, "radar", DiagTelemetry_NeedsAttention() ? "Radar / Provider <span style='color:#c00'>- ATTENTION</span>" : "Radar / Provider", true) != ESP_OK)
        return ESP_FAIL;
    int maxEver = GetMaxAircraftCountSinceBoot();
    if (Send(req, "<h3>Aircraft tracking</h3><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%d / %d", gAircraftCount, MAX_AIRCRAFT);
    if (Row(req, "Current aircraft / capacity", buf, "Measured / Calculated (gAircraftCount vs MAX_AIRCRAFT)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%d", maxEver);
    if (Row(req, "Max aircraft observed since boot", buf, "Measured (tracked since this build; see main.c)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)(MAX_AIRCRAFT * sizeof(Aircraft)));
    if (Row(req, "gAircraft[] static allocation", buf, "Calculated (MAX_AIRCRAFT * sizeof(Aircraft))") != ESP_OK) return ESP_FAIL;
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;
    if (OpsSection(req) != ESP_OK)
        return ESP_FAIL;

    if (GroupClose(req) != ESP_OK)
        return ESP_FAIL;

    // ---- Configuration storage (custom rules, operators, airports, seen history) ----
    {
        // Open the Storage group automatically when the History index needs attention.
        TfHistoryStats tfs;
        TfHistory_GetStats(&tfs);
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
    snprintf(buf, sizeof(buf), "%u / %u", (unsigned)SeenAircraft_Count(), (unsigned)SEEN_MAX_RECORDS);
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
    SeenPersistStats ps;
    SeenAircraft_GetPersistStats(&ps);
    if (Send(req, "<h3>Persistence (Seen history flush)</h3><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)ps.flushCount);
    if (Row(req, "Flushes since boot", buf, "Measured (counter in WriteBinaryLocked)") != ESP_OK) return ESP_FAIL;
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
    if (TfSection(req) != ESP_OK || FlushSection(req) != ESP_OK)
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

    return httpd_resp_send_chunk(req, NULL, 0);
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
