#include "web_diag.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "esp_heap_caps.h"
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
#include "history_manager.h"
#include "main.h"
#include "opensky_client.h"
#include "seen_aircraft.h"
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
static esp_err_t Row(httpd_req_t *req, const char *label, const char *value, const char *evidence)
{
    return SendFormat(req,
        "<tr><td>%s</td><td>%s</td><td class='ev'>%s</td></tr>",
        label, value, evidence);
}

// ---- Expert Debug Mode (forensic; persistent NVS toggle, effective after reboot) ----
static esp_err_t ExpertSection(httpd_req_t *req)
{
    bool active = ExpertDebug_Active();
    bool saved = ExpertDebug_GetSaved();
    char buf[64];

    if (Send(req, "<h2 id='expert'>Expert Debug Mode (forensic diagnostics)</h2>"
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

    if (Send(req, "<h2 id='advanced'>Advanced Diagnostics</h2>"
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
        if (Send(req, "<h3>Heap checkpoints (most recent 16)</h3><table><tr><th>Stage</th><th>Uptime</th>"
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
        if (Send(req, "</table><h3>DMA-capable internal free blocks &ge; 1 KB (now)</h3>"
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
    char body[64] = {0};
    int total = 0;
    while (total < (int)sizeof(body) - 1 && total < (int)req->content_len)
    {
        int got = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (got <= 0)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Could not read form");
        total += got;
    }
    body[total] = '\0';

    char val[8];
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
    char page[640];
    snprintf(page, sizeof(page),
             "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>%s"
             "<title>Diagnostics</title></head><body style='font:16px sans-serif;max-width:750px;margin:1em auto;padding:0 1em'>"
             "<p>%s saved as <b>%s</b>. %s</p><p><a href='/diag#%s'>Back to diagnostics</a></p>"
             "</body></html>",
             refresh, modeName, enable ? "ON" : "OFF",
             reboot ? "Rebooting now; this page returns to /diag in about 20 seconds."
                    : "Reboot required: the current run keeps its existing setting until the next restart.",
             anchor);
    httpd_resp_sendstr(req, page);

    if (reboot)
    {
        // Same order as the other deliberate restarts in webserver.c:
        // let the response go out, flush pending Seen history, restart.
        vTaskDelay(pdMS_TO_TICKS(500));
        if (SeenAircraft_IsDirty())
            SeenAircraft_Flush();
        esp_restart();
    }
    return ESP_OK;
}

static esp_err_t DiagPage(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    if (Send(req,
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Diagnostics</title><style>body{font:16px sans-serif;max-width:750px;margin:1em auto;padding:0 1em}"
        "table{border-collapse:collapse;width:100%}td,th{border:1px solid #bbb;padding:.35em .5em;text-align:left}"
        "th{background:#eee}.ev{color:#666;font-size:.85em}h2{margin-top:1.6em}</style></head><body>"
        "<p><a href='/'>Back to setup</a></p><h1>Runtime capacity report</h1>"
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
    if (Send(req, "<h2>Identity</h2><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK ||
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

    // ---- RAM ----
    unsigned internalFree = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned internalLargest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned internalMinEver = (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned psramFree = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    unsigned psramLargest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    unsigned psramMinEver = (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (Send(req, "<h2>RAM</h2><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
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
    if (Send(req, "<h2>Flash / filesystem</h2><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK ||
        Row(req, "Partition layout", "factory app 2.5 MB, spiffs 1,507,328 bytes (1.4375 MB), nvs 24 KB, phy_init 4 KB (4 MB flash total)",
            "Measured (partitions.csv, build-config)") != ESP_OK)
        return ESP_FAIL;
    if (haveSpiffs) {
        snprintf(buf, sizeof(buf), "%u / %u bytes", (unsigned)spiffsUsed, (unsigned)spiffsTotal);
        if (Row(req, "SPIFFS used / total", buf, "Measured (esp_spiffs_info)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u bytes", (unsigned)(spiffsTotal - spiffsUsed));
        if (Row(req, "SPIFFS free", buf, "Calculated (total - used)") != ESP_OK) return ESP_FAIL;
    } else if (Row(req, "SPIFFS used / total", "unavailable", "Unknown without runtime measurement (esp_spiffs_info failed - not mounted?)") != ESP_OK) {
        return ESP_FAIL;
    }
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    // ---- Aircraft / tracking ----
    int maxEver = GetMaxAircraftCountSinceBoot();
    if (Send(req, "<h2>Aircraft tracking</h2><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%d / %d", gAircraftCount, MAX_AIRCRAFT);
    if (Row(req, "Current aircraft / capacity", buf, "Measured / Calculated (gAircraftCount vs MAX_AIRCRAFT)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%d", maxEver);
    if (Row(req, "Max aircraft observed since boot", buf, "Measured (tracked since this build; see main.c)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)(MAX_AIRCRAFT * sizeof(Aircraft)));
    if (Row(req, "gAircraft[] static allocation", buf, "Calculated (MAX_AIRCRAFT * sizeof(Aircraft))") != ESP_OK) return ESP_FAIL;
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    // ---- Configuration storage (custom rules, operators, airports, seen history) ----
    if (Send(req, "<h2>Configuration &amp; history storage</h2><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u / %u", (unsigned)CustomRules_Count(), (unsigned)MAX_CUSTOM_RULES);
    if (Row(req, "Custom rules / cap", buf, "Measured / Calculated") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u / %u", (unsigned)Operators_Count(), (unsigned)MAX_OPERATORS);
    if (Row(req, "Operator rows / cap", buf, "Measured / Calculated") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u", (unsigned)Airports_Count());
    if (Row(req, "Airports configured (cap 100)", buf, "Measured") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u / %u", (unsigned)SeenAircraft_Count(), (unsigned)SEEN_MAX_RECORDS);
    if (Row(req, "Seen Aircraft records / cap", buf, "Measured / Calculated") != ESP_OK) return ESP_FAIL;
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
    if (Send(req, "<h2>Persistence (Seen history flush)</h2><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
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
    if (Send(req, "<h2>Universal Values</h2><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
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
    TfHistoryStats tf;
    TfHistory_GetStats(&tf);
    HistoryManagerStats hm;
    HistoryManager_GetStats(&hm);
    if (Send(req, "<h2>TF history (persistent, warm layer under Hot Seen)</h2><table><tr><th>Metric</th><th>Value</th><th>Evidence</th></tr>") != ESP_OK)
        return ESP_FAIL;
    if (Row(req, "TF card mounted", tf.mounted ? "yes" : "no (radar/Hot Seen unaffected either way)", "Measured (TfHistory_Init result)") != ESP_OK) return ESP_FAIL;
    if (tf.mounted) {
        // Own (larger) buffer, same reasoning as uptimeText above: three
        // %llu fields size against uint64's full 20-digit range for
        // -Werror=format-truncation, not against a realistic TF card size.
        char tfSpaceBuf[128];
        snprintf(tfSpaceBuf, sizeof(tfSpaceBuf), "%llu / %llu bytes (%llu free)",
                 (unsigned long long)tf.usedBytes, (unsigned long long)tf.capacityBytes, (unsigned long long)tf.freeBytes);
        if (Row(req, "TF used / capacity", tfSpaceBuf, "Measured (f_getfree)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u / %u", (unsigned)tf.indexSlotsUsed, (unsigned)tf.indexSlotsTotal);
        if (Row(req, "Index slots used / total", buf, "Measured / Calculated (fixed-size open-addressing hash index)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u", (unsigned)tf.writes);
        if (Row(req, "Writes since boot (creates + updates)", buf, "Measured") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u / %u", (unsigned)tf.creates, (unsigned)tf.updates);
        if (Row(req, "  creates / updates", buf, "Measured") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u / %u", (unsigned)tf.lookups, (unsigned)tf.lookupMisses);
        if (Row(req, "Lookups / misses", buf, "Measured") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u", (unsigned)tf.errors);
        if (Row(req, "I/O or CRC errors handled", buf, "Measured (a corrupt record is treated as absent and self-heals on next write)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u", (unsigned)tf.recoveryOps);
        if (Row(req, "Corrupt records skipped during scans", buf, "Measured") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u", (unsigned)tf.indexRebuilds);
        if (Row(req, "Index rebuilds since boot", buf, "Measured (missing/corrupt index detected at mount)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u us / %u us / %u us", (unsigned)tf.lastLookupUs, (unsigned)tf.bestLookupUs, (unsigned)tf.worstLookupUs);
        if (Row(req, "Lookup time: last / best / worst", buf, "Measured (esp_timer_get_time around the index probe + record read)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u us / %u us / %u us", (unsigned)tf.lastWriteUs, (unsigned)tf.bestWriteUs, (unsigned)tf.worstWriteUs);
        if (Row(req, "Write time: last / best / worst", buf, "Measured (esp_timer_get_time around the record + header write)") != ESP_OK) return ESP_FAIL;
        snprintf(buf, sizeof(buf), "%u us / %u us / %u us", (unsigned)tf.lastRecoveryUs, (unsigned)tf.bestRecoveryUs, (unsigned)tf.worstRecoveryUs);
        if (Row(req, "Index rebuild time: last / best / worst", buf, "Measured") != ESP_OK) return ESP_FAIL;
    }
    snprintf(buf, sizeof(buf), "%u / %u (%u dirty)", (unsigned)hm.shadowSlotsUsed, (unsigned)hm.shadowSlotsCap, (unsigned)hm.dirtyNow);
    if (Row(req, "History Manager RAM shadow slots / cap", buf, "Measured / Calculated (one slot per currently-tracked aircraft, not the archive - see PROJECT_STATE.md)") != ESP_OK) return ESP_FAIL;
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)(hm.shadowSlotsCap * (unsigned)(sizeof(TfHistoryRecord) + 16)));
    if (Row(req, "History Manager allocation (PSRAM heap, approx.)", buf, "Calculated") != ESP_OK) return ESP_FAIL;
    if (Send(req, "</table>") != ESP_OK)
        return ESP_FAIL;

    if (AdvancedSection(req) != ESP_OK)
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
    return httpd_register_uri_handler(server, &adv_uri);
}
