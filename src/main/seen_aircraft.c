#include "seen_aircraft.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "time_util.h"

static const char *TAG = "SeenAircraft";

/* Phase 2 audit instrumentation (see seen_aircraft.h SeenPersistStats).
 * Updated only inside WriteBinaryLocked(), which already runs under seenLock,
 * so no separate lock is needed for these. */
static SeenPersistStats persistStats = {0};

#define SEEN_INITIAL_CAPACITY 64

static SeenRecord *records = NULL;
static size_t recordCount = 0;
static size_t recordCapacity = 0;
static bool dirty = false;
static bool dirtySinceSet = false;
static uint32_t dirtySinceSec = 0;
static SemaphoreHandle_t seenLock = NULL;
static const char *csvPath = SEEN_CSV_PATH;
static const char *tmpPath = SEEN_CSV_TMP_PATH;

/* Incremental persistence: which in-memory slots have changed since the last
 * successful write. Fixed-size (matches SEEN_MAX_RECORDS - a few KB static,
 * negligible) so it needs no allocation and stays in lockstep with `records[]` by index -
 * indices are stable (see PickEvictionVictim's comment), so this is safe. */
static bool recordDirty[SEEN_MAX_RECORDS];

static void MarkRecordDirty(size_t idx)
{
    if (idx < SEEN_MAX_RECORDS)
        recordDirty[idx] = true;
    dirty = true;
}

/* ---- naming ---- */

const char *SeenOperatorSource_Name(SeenOperatorSource s)
{
    switch (s) {
    case SEEN_OPSRC_PROVIDER: return "Provider";
    case SEEN_OPSRC_CONFIGURED: return "Configured";
    default: return "";
    }
}

const char *SeenOperatorSource_CsvName(SeenOperatorSource s)
{
    switch (s) {
    case SEEN_OPSRC_PROVIDER: return "PROVIDER";
    case SEEN_OPSRC_CONFIGURED: return "CONFIGURED";
    default: return "NONE";
    }
}

static bool ParseOperatorSource(const char *token, SeenOperatorSource *out)
{
    if (strcasecmp(token, "NONE") == 0) { *out = SEEN_OPSRC_NONE; return true; }
    if (strcasecmp(token, "PROVIDER") == 0) { *out = SEEN_OPSRC_PROVIDER; return true; }
    if (strcasecmp(token, "CONFIGURED") == 0) { *out = SEEN_OPSRC_CONFIGURED; return true; }
    return false;
}

/* ---- normalization ---- */

/* ICAO24 is 6 hex digits; ADSBExchange-style feeds may prefix "~" for
 * non-ICAO (TIS-B) addresses. Stored upper-case so one aircraft is one key
 * no matter which provider (or letter case) reported it. */
bool SeenAircraft_NormalizeIcao(const char *input, char out[SEEN_ICAO_MAX])
{
    out[0] = '\0';
    if (!input)
        return false;
    while (*input == ' ')
        input++;
    size_t used = 0;
    for (const char *p = input; *p && *p != ' '; p++) {
        char c = *p;
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
                  (c == '~' && used == 0);
        if (!ok || used + 1 >= SEEN_ICAO_MAX) {
            out[0] = '\0';
            return false;
        }
        out[used++] = (c >= 'a' && c <= 'f') ? (char)(c - ('a' - 'A')) : c;
    }
    out[used] = '\0';
    return used > 0;
}

static void NormalizeCallsign(const char *input, char out[SEEN_CALLSIGN_MAX])
{
    char tmp[32];
    AircraftText_Sanitize(tmp, sizeof(tmp), input);
    size_t n = strlen(tmp);
    if (n >= SEEN_CALLSIGN_MAX)
        n = SEEN_CALLSIGN_MAX - 1;
    memcpy(out, tmp, n);
    out[n] = '\0';
}

/* ---- table management (caller holds the lock) ---- */

static bool EnsureCapacity(size_t needed)
{
    if (needed <= recordCapacity)
        return true;
    size_t newCap = recordCapacity ? recordCapacity : SEEN_INITIAL_CAPACITY;
    while (newCap < needed)
        newCap *= 2;
    if (newCap > SEEN_MAX_RECORDS)
        newCap = SEEN_MAX_RECORDS;
    if (newCap < needed)
        return false;
    /* Phase 2 audit instrumentation: confirm where this realloc actually
     * lands. sdkconfig has CONFIG_SPIRAM_USE_MALLOC=y with
     * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384, so any single malloc/realloc
     * request over 16 KB should be routed to PSRAM automatically, below that
     * it's internal-only. At 84 bytes/record that threshold falls between
     * 128 and 256 records - this log is the one-time proof, not an estimate.
     * One-shot per growth step (growth is geometric and capped at
     * SEEN_MAX_RECORDS, so this fires only a handful of times ever). */
    size_t newBytes = newCap * sizeof(SeenRecord);
    unsigned internalBefore = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned psramBefore = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    SeenRecord *grown = realloc(records, newBytes);
    if (!grown) {
        ESP_LOGE(TAG, "Out of memory growing the seen-aircraft table to %u records", (unsigned)newCap);
        return false;
    }
    unsigned internalAfter = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned psramAfter = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,
             "Seen table grown to %u records (%u bytes): internal heap %d -> %d (delta %d), "
             "PSRAM %u -> %u (delta %d)",
             (unsigned)newCap, (unsigned)newBytes,
             internalBefore, internalAfter, (int)internalBefore - (int)internalAfter,
             psramBefore, psramAfter, (int)psramBefore - (int)psramAfter);
    records = grown;
    recordCapacity = newCap;
    return true;
}

static int FindIndex(const char *icao24)
{
    for (size_t i = 0; i < recordCount; i++) {
        if (strcmp(records[i].icao24, icao24) == 0)
            return (int)i;
    }
    return -1;
}

/* Pruning policy: least-recently-seen. The victim is the record with the
 * oldest lastSeen; unsynchronized records (lastSeen 0) are therefore the
 * first to go, then ties fall to the lower Seen Count, then the earlier slot.
 * Replacing in place keeps every other record's index stable. */
static size_t PickEvictionVictim(void)
{
    size_t victim = 0;
    for (size_t i = 1; i < recordCount; i++) {
        const SeenRecord *a = &records[i];
        const SeenRecord *b = &records[victim];
        if (a->lastSeen < b->lastSeen || (a->lastSeen == b->lastSeen && a->seenCount < b->seenCount))
            victim = i;
    }
    return victim;
}

/* Returns a slot for a NEW record (evicting if the table is full). */
static SeenRecord *AllocateSlot(void)
{
    if (recordCount < SEEN_MAX_RECORDS) {
        if (!EnsureCapacity(recordCount + 1))
            return NULL;
        return &records[recordCount++];
    }
    size_t victim = PickEvictionVictim();
    ESP_LOGD(TAG, "History full (%u): pruning least-recently-seen %s", (unsigned)SEEN_MAX_RECORDS, records[victim].icao24);
    return &records[victim];
}

static uint32_t ClampTime(int64_t t)
{
    if (!TimeUtil_IsSynced(t) || t > (int64_t)UINT32_MAX)
        return 0;
    return (uint32_t)t;
}

/* ---- observation ---- */

void SeenAircraft_Observe(const SeenObservation *obs, int64_t nowUtc)
{
    if (!obs || !seenLock)
        return;
    char icao[SEEN_ICAO_MAX];
    if (!SeenAircraft_NormalizeIcao(obs->icao24, icao))
        return;
    char callsign[SEEN_CALLSIGN_MAX];
    NormalizeCallsign(obs->callsign, callsign);
    char opName[AIRCRAFT_OPERATOR_NAME_MAX];
    AircraftText_Sanitize(opName, sizeof(opName), obs->operatorName);

    const uint32_t now = ClampTime(nowUtc); /* 0 when the clock is not synchronized */

    xSemaphoreTake(seenLock, portMAX_DELAY);
    int idx = FindIndex(icao);
    if (idx >= 0) {
        SeenRecord *r = &records[idx];
        if (now) {
            /* Seen Count counts visits: a new one starts only after the aircraft
             * has been absent for SEEN_VISIT_GAP_SEC. A record created while
             * the clock was unsynchronized (lastSeen 0) was already counted
             * once, so its first stamped observation does not add another. */
            if (r->lastSeen != 0 && now >= r->lastSeen && (now - r->lastSeen) >= (uint32_t)SEEN_VISIT_GAP_SEC)
                r->seenCount++;
            if (r->firstSeen == 0)
                r->firstSeen = now; /* first time we could stamp it - never a made-up earlier time */
            if (now > r->lastSeen)
                r->lastSeen = now;
        }
        if (callsign[0])
            memcpy(r->callsign, callsign, sizeof(r->callsign));
        if (opName[0]) {
            memcpy(r->operatorName, opName, sizeof(r->operatorName));
            r->operatorSource = obs->operatorSource;
        }
        r->craftType = obs->craftType;
        r->aircraftType = obs->aircraftType;
        MarkRecordDirty((size_t)idx);
    } else {
        SeenRecord *r = AllocateSlot();
        if (r) {
            memset(r, 0, sizeof(*r));
            memcpy(r->icao24, icao, sizeof(r->icao24));
            memcpy(r->callsign, callsign, sizeof(r->callsign));
            memcpy(r->operatorName, opName, sizeof(r->operatorName));
            r->operatorSource = opName[0] ? obs->operatorSource : SEEN_OPSRC_NONE;
            r->craftType = obs->craftType;
            r->aircraftType = obs->aircraftType;
            r->firstSeen = now;
            r->lastSeen = now;
            r->seenCount = 1;
            MarkRecordDirty((size_t)(r - records));
        }
    }
    dirty = true;
    xSemaphoreGive(seenLock);
}

void SeenAircraft_ObservePoll(const Aircraft *list, int count, int64_t nowUtc)
{
    if (!list || count <= 0)
        return;
    if (count > MAX_AIRCRAFT)
        count = MAX_AIRCRAFT;
    for (int i = 0; i < count; i++) {
        const Aircraft *a = &list[i];
        if (!a->valid || !a->icao24[0])
            continue;

        SeenObservation obs;
        memset(&obs, 0, sizeof(obs));
        snprintf(obs.icao24, sizeof(obs.icao24), "%.8s", a->icao24);
        snprintf(obs.callsign, sizeof(obs.callsign), "%.8s", a->callsign);

        /* The same resolution the radar and Current Aircraft use. */
        CraftResolution res = ResolveAircraftWithHint(a->callsign, a->icao24, a->providerTypeHint, a->hasProviderTypeHint);
        obs.craftType = res.type;
        obs.aircraftType = res.aircraftType;

        /* Operator: a provider-supplied name wins; otherwise the configured
         * operator matching the call sign's ICAO code. */
        if (a->operatorName[0]) {
            memcpy(obs.operatorName, a->operatorName, sizeof(obs.operatorName));
            obs.operatorSource = SEEN_OPSRC_PROVIDER;
        } else {
            char code[MAX_OPERATOR_CODE + 1] = "";
            if (res.source == CRAFT_SRC_OPERATOR)
                snprintf(code, sizeof(code), "%s", res.operatorCode);
            else
                Operators_CodeFromCallsign(a->callsign, code);
            OperatorInfo op;
            if (code[0] && Operators_Find(code, &op)) {
                AircraftText_Sanitize(obs.operatorName, sizeof(obs.operatorName), op.name);
                obs.operatorSource = SEEN_OPSRC_CONFIGURED;
            }
        }
        SeenAircraft_Observe(&obs, nowUtc);
    }
}

/* ---- derived registry/operator status ---- */

void SeenAircraft_Describe(const SeenRecord *record, SeenConfigInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->source = CRAFT_SRC_FALLBACK;
    if (!record)
        return;
    CraftResolution res = ResolveAircraft(record->callsign, record->icao24);
    out->source = res.source;
    if (res.source == CRAFT_SRC_REGISTRY) {
        out->configured = true;
        snprintf(out->registryPrefix, sizeof(out->registryPrefix), "%s", res.registryPrefix);
        CustomRule rule;
        if (CustomRules_Find(res.registryPrefix, &rule))
            snprintf(out->registryNote, sizeof(out->registryNote), "%s", rule.notes);
    }
    char code[MAX_OPERATOR_CODE + 1] = "";
    if (res.source == CRAFT_SRC_OPERATOR)
        snprintf(code, sizeof(code), "%s", res.operatorCode);
    else
        Operators_CodeFromCallsign(record->callsign, code);
    OperatorInfo op;
    if (code[0] && Operators_Find(code, &op)) {
        snprintf(out->operatorCode, sizeof(out->operatorCode), "%s", code);
        snprintf(out->configuredOperator, sizeof(out->configuredOperator), "%s", op.name);
        out->configured = true;
    }
}

/* ---- persistence: legacy CSV parsing (still used to migrate old history) ---- */

static bool ParseU32(const char *s, uint32_t *out)
{
    if (!s || !*s)
        return false;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
    }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!end || *end != '\0' || v > UINT32_MAX)
        return false;
    *out = (uint32_t)v;
    return true;
}

/* Columns: ICAO24,CALLSIGN,CRAFT,AIRCRAFT,OPERATOR,OP_SOURCE,FIRST_SEEN,LAST_SEEN,COUNT
 * Timestamps are UTC epoch seconds (0 = unknown). Malformed lines are skipped,
 * never fatal. */
static bool ParseLine(char *line, SeenRecord *out)
{
    char *fields[9];
    size_t n = 0;
    fields[n++] = line;
    for (char *p = line; *p; p++) {
        if (*p == ',') {
            if (n >= 9)
                return false; /* too many columns */
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    if (n != 9)
        return false;

    memset(out, 0, sizeof(*out));
    if (!SeenAircraft_NormalizeIcao(fields[0], out->icao24))
        return false;
    NormalizeCallsign(fields[1], out->callsign);
    if (!CraftType_Parse(fields[2], false, &out->craftType))
        return false;
    if (!AircraftType_Parse(fields[3], &out->aircraftType))
        return false;
    AircraftText_Sanitize(out->operatorName, sizeof(out->operatorName), fields[4]);
    if (!ParseOperatorSource(fields[5], &out->operatorSource))
        return false;
    if (!out->operatorName[0])
        out->operatorSource = SEEN_OPSRC_NONE;

    uint32_t first, last, count;
    if (!ParseU32(fields[6], &first) || !ParseU32(fields[7], &last) || !ParseU32(fields[8], &count))
        return false;
    /* A non-zero time before the validity threshold can only be corruption. */
    out->firstSeen = TimeUtil_IsSynced(first) ? first : 0;
    out->lastSeen = TimeUtil_IsSynced(last) ? last : 0;
    if (out->firstSeen && out->lastSeen && out->lastSeen < out->firstSeen)
        out->firstSeen = out->lastSeen;
    out->seenCount = count ? count : 1;
    return true;
}

/* Folds one loaded record in: duplicates of an ICAO24 (only possible in a
 * hand-edited file) merge to the widest first/last window and the higher
 * count; a full table replaces the least-recently-seen record. Caller holds
 * the lock. */
static void MergeLoaded(const SeenRecord *in)
{
    int idx = FindIndex(in->icao24);
    if (idx >= 0) {
        SeenRecord *r = &records[idx];
        if (in->lastSeen >= r->lastSeen) {
            memcpy(r->callsign, in->callsign, sizeof(r->callsign));
            memcpy(r->operatorName, in->operatorName, sizeof(r->operatorName));
            r->operatorSource = in->operatorSource;
            r->craftType = in->craftType;
            r->aircraftType = in->aircraftType;
        }
        if (in->firstSeen && (r->firstSeen == 0 || in->firstSeen < r->firstSeen))
            r->firstSeen = in->firstSeen;
        if (in->lastSeen > r->lastSeen)
            r->lastSeen = in->lastSeen;
        if (in->seenCount > r->seenCount)
            r->seenCount = in->seenCount;
        return;
    }
    if (recordCount >= SEEN_MAX_RECORDS) {
        /* File holds more than we keep: keep the most recently seen. */
        size_t victim = PickEvictionVictim();
        const SeenRecord *v = &records[victim];
        if (in->lastSeen < v->lastSeen || (in->lastSeen == v->lastSeen && in->seenCount <= v->seenCount))
            return; /* the incoming record is the least recent: drop it */
        records[victim] = *in;
        return;
    }
    if (EnsureCapacity(recordCount + 1))
        records[recordCount++] = *in;
}

static bool LoadFile(const char *path, size_t *goodLines, size_t *badLines)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
            /* Absurdly long line: drop it and the rest of it. */
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {}
            (*badLines)++;
            continue;
        }
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#')
            continue;
        SeenRecord rec;
        if (ParseLine(line, &rec)) {
            MergeLoaded(&rec);
            (*goodLines)++;
        } else {
            (*badLines)++;
        }
    }
    fclose(f);
    return true;
}

static bool FileExists(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    fclose(f);
    return true;
}

/* ---- persistence: binary read/write (current format) ----
 *
 * Fixed 12-byte header, then up to SEEN_MAX_RECORDS raw SeenRecord slots at
 * fixed offsets by index. This is a private working file for this exact
 * firmware build - no cross-version/cross-platform format contract is
 * intended, which is why records are written as a raw struct dump rather
 * than field-by-field. Human-readable export is handled separately, on
 * demand, by /seen/export (web_seen.c), generated from the in-memory table -
 * this file is never read by anything else. */
typedef struct {
    char magic[4];
    uint8_t version;
    uint8_t reserved[3];
    uint32_t recordCount;
} SeenBinHeader;

#define SEEN_BIN_MAGIC "SAB1"
#define SEEN_BIN_SLOT_OFFSET(i) (sizeof(SeenBinHeader) + (size_t)(i) * sizeof(SeenRecord))

static bool WriteBinaryHeader(FILE *f, size_t count)
{
    SeenBinHeader hdr = {0};
    memcpy(hdr.magic, SEEN_BIN_MAGIC, 4);
    hdr.version = SEEN_BIN_FORMAT_VERSION;
    hdr.recordCount = (uint32_t)count;
    return fseek(f, 0, SEEK_SET) == 0 && fwrite(&hdr, sizeof(hdr), 1, f) == 1;
}

/* Populates records[]/recordCount from the binary file. Caller holds the
 * lock (called only from SeenAircraft_InitWithPaths, before anything else
 * can touch the table). */
static bool LoadBinaryFile(const char *path, size_t *loadedCount)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    SeenBinHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, SEEN_BIN_MAGIC, 4) != 0 ||
        hdr.version != SEEN_BIN_FORMAT_VERSION) {
        ESP_LOGW(TAG, "%s is not a valid seen-aircraft file (bad header); ignoring it", path);
        fclose(f);
        return false;
    }
    size_t count = hdr.recordCount;
    if (count > SEEN_MAX_RECORDS) {
        ESP_LOGW(TAG, "%s claims %u records, more than the %u cap; clamping", path, (unsigned)count, (unsigned)SEEN_MAX_RECORDS);
        count = SEEN_MAX_RECORDS;
    }
    if (!EnsureCapacity(count)) {
        fclose(f);
        return false;
    }
    size_t got = count ? fread(records, sizeof(SeenRecord), count, f) : 0;
    fclose(f);
    recordCount = got;
    if (loadedCount)
        *loadedCount = got;
    return got == count;
}

/* Writes every record fresh (header + all slots) to a temp file, then swaps
 * it in - the same temp-file/rename safety pattern the old CSV writer used.
 * This is the ONLY path that costs O(recordCount); it runs exactly once in
 * normal operation (the very first save ever, or a one-time migration from
 * the legacy CSV - see SeenAircraft_InitWithPaths) and again only if the
 * binary file is ever found missing/corrupt. Every routine flush after that
 * goes through WriteBinaryIncremental() instead. Caller holds the lock. */
static bool WriteBinaryFull(void)
{
    size_t worstCase = recordCount * sizeof(SeenRecord) + sizeof(SeenBinHeader) + 256;
    size_t total = 0, used = 0;
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
        size_t freeBytes = total > used ? total - used : 0;
        if (freeBytes < 2 * worstCase + SEEN_MIN_FREE_MARGIN_BYTES) {
            ESP_LOGW(TAG, "Not saving seen-aircraft history: only %u bytes of SPIFFS free (need %u)",
                     (unsigned)freeBytes, (unsigned)(2 * worstCase + SEEN_MIN_FREE_MARGIN_BYTES));
            return false;
        }
    }

    FILE *f = fopen(SEEN_BIN_TMP_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Could not open %s for writing", SEEN_BIN_TMP_PATH);
        return false;
    }

    bool ok = WriteBinaryHeader(f, recordCount);
    int64_t seekUs = 0, writeUs = 0;
    unsigned minInternalDuring = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    for (size_t i = 0; ok && i < recordCount; i++) {
        int64_t t0 = esp_timer_get_time();
        size_t wrote = fwrite(&records[i], sizeof(SeenRecord), 1, f);
        int64_t t1 = esp_timer_get_time();
        writeUs += (t1 - t0);
        if (wrote != 1) {
            ok = false;
            break;
        }
        if ((i % 20) == 0) {
            unsigned cur = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (cur < minInternalDuring)
                minInternalDuring = cur;
        }
    }
    persistStats.lastFormatUs = (uint32_t)seekUs; /* no formatting step for a raw struct write */
    persistStats.lastWriteUs = (uint32_t)writeUs;
    persistStats.lastMinInternalHeapDuringBytes = minInternalDuring;

    bool bad = !ok || ferror(f) != 0;
    if (fclose(f) != 0)
        bad = true;
    if (bad) {
        ESP_LOGE(TAG, "Write error saving seen-aircraft history (full rewrite); keeping the previous file");
        remove(SEEN_BIN_TMP_PATH);
        return false;
    }

    remove(SEEN_BIN_PATH);
    if (rename(SEEN_BIN_TMP_PATH, SEEN_BIN_PATH) != 0) {
        ESP_LOGE(TAG, "Could not move %s into place", SEEN_BIN_TMP_PATH);
        return false; /* the temp file stays; the next load recovers from it */
    }
    memset(recordDirty, 0, sizeof(recordDirty));
    dirty = false;
    dirtySinceSet = false;
    persistStats.lastWasFullRewrite = true;
    persistStats.lastRecordCount = (uint32_t)recordCount;
    ESP_LOGI(TAG, "Saved %u seen-aircraft records (full rewrite)", (unsigned)recordCount);
    return true;
}

/* The routine flush: writes only the slots marked dirty since the last
 * successful save, seeking directly to each one's fixed offset, plus the
 * (tiny, so unconditional) header. O(dirty count), not O(recordCount) - this
 * is what replaced the ~5 second, ~5 ms/record full CSV rewrite measured on
 * hardware (PROJECT_STATE.md "Persistence timing diagnostics"). Falls back
 * to a full rewrite if the binary file is missing (first-ever save, or the
 * file was lost). Caller holds the lock. */
static bool WriteBinaryIncremental(void)
{
    size_t dirtyCount = 0;
    for (size_t i = 0; i < recordCount; i++) {
        if (recordDirty[i])
            dirtyCount++;
    }
    size_t worstCase = dirtyCount * sizeof(SeenRecord) + sizeof(SeenBinHeader) + 256;
    size_t total = 0, used = 0;
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
        size_t freeBytes = total > used ? total - used : 0;
        if (freeBytes < worstCase + SEEN_MIN_FREE_MARGIN_BYTES) {
            ESP_LOGW(TAG, "Not saving seen-aircraft history: only %u bytes of SPIFFS free (need %u)",
                     (unsigned)freeBytes, (unsigned)(worstCase + SEEN_MIN_FREE_MARGIN_BYTES));
            return false;
        }
    }

    FILE *f = fopen(SEEN_BIN_PATH, "r+b");
    if (!f) {
        ESP_LOGW(TAG, "%s missing for an incremental update; doing a full rewrite instead", SEEN_BIN_PATH);
        return WriteBinaryFull();
    }

    bool ok = WriteBinaryHeader(f, recordCount);
    int64_t seekUs = 0, writeUs = 0;
    unsigned minInternalDuring = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t written = 0;
    for (size_t i = 0; ok && i < recordCount; i++) {
        if (!recordDirty[i])
            continue;
        int64_t t0 = esp_timer_get_time();
        int seekRc = fseek(f, (long)SEEN_BIN_SLOT_OFFSET(i), SEEK_SET);
        int64_t t1 = esp_timer_get_time();
        seekUs += (t1 - t0);
        if (seekRc != 0) {
            ok = false;
            break;
        }
        size_t wrote = fwrite(&records[i], sizeof(SeenRecord), 1, f);
        int64_t t2 = esp_timer_get_time();
        writeUs += (t2 - t1);
        if (wrote != 1) {
            ok = false;
            break;
        }
        recordDirty[i] = false;
        written++;
        if ((written % 10) == 0) {
            unsigned cur = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (cur < minInternalDuring)
                minInternalDuring = cur;
        }
    }
    persistStats.lastFormatUs = (uint32_t)seekUs; /* "format" slot repurposed as seek time here */
    persistStats.lastWriteUs = (uint32_t)writeUs;
    persistStats.lastMinInternalHeapDuringBytes = minInternalDuring;

    bool bad = !ok || ferror(f) != 0;
    if (fclose(f) != 0)
        bad = true;
    if (bad) {
        /* Records already written this pass had their dirty flag cleared
         * above and do not need to be retried; anything not yet reached
         * (including everything, if the header write itself failed) stays
         * dirty and is retried on the next flush interval. */
        ESP_LOGE(TAG, "Write error updating seen-aircraft history; %u of %u changed records saved, rest will retry",
                 (unsigned)written, (unsigned)dirtyCount);
        return false;
    }
    dirty = false;
    dirtySinceSet = false;
    persistStats.lastWasFullRewrite = false;
    persistStats.lastRecordCount = (uint32_t)written;
    ESP_LOGI(TAG, "Saved %u changed seen-aircraft record(s) of %u total", (unsigned)written, (unsigned)recordCount);
    return true;
}

static bool WriteBinaryLockedInner(void)
{
    if (!FileExists(SEEN_BIN_PATH))
        return WriteBinaryFull();
    return WriteBinaryIncremental();
}

/* Thin timing/heap wrapper around the real write, added for the flicker
 * investigation (PROJECT_STATE.md "Persistence timing diagnostics"). Records
 * are updated whether the write succeeds or fails, since a failed attempt
 * (e.g. the SPIFFS-headroom bailout) still costs a little time and is useful
 * to see if it happens to line up with a reported flicker. Caller holds the
 * lock, same as WriteBinaryLockedInner(). */
static bool WriteBinaryLocked(void)
{
    unsigned before = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned psramBefore = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int64_t startUs = esp_timer_get_time();

    bool ok = WriteBinaryLockedInner();

    int64_t endUs = esp_timer_get_time();
    unsigned after = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned psramAfter = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    uint32_t durationMs = (uint32_t)((endUs - startUs) / 1000);
    int32_t heapDelta = (int32_t)before - (int32_t)after; /* positive = consumed */
    int32_t psramDelta = (int32_t)psramBefore - (int32_t)psramAfter;

    persistStats.flushCount++;
    persistStats.lastDurationMs = durationMs;
    if (durationMs > persistStats.worstDurationMs)
        persistStats.worstDurationMs = durationMs;
    persistStats.lastInternalHeapDeltaBytes = heapDelta;
    if (heapDelta > persistStats.worstInternalHeapDeltaBytes)
        persistStats.worstInternalHeapDeltaBytes = heapDelta;
    persistStats.lastPsramDeltaBytes = psramDelta;
    /* persistStats.lastRecordCount and lastWasFullRewrite are set inside
     * WriteBinaryFull()/WriteBinaryIncremental() themselves, since only they
     * know which path ran and how many records that path actually touched. */

    return ok;
}

void SeenAircraft_GetPersistStats(SeenPersistStats *out)
{
    if (out)
        *out = persistStats;
}

bool SeenAircraft_Flush(void)
{
    if (!seenLock)
        return false;
    xSemaphoreTake(seenLock, portMAX_DELAY);
    bool ok = WriteBinaryLocked();
    xSemaphoreGive(seenLock);
    return ok;
}

bool SeenAircraft_FlushIfDue(uint32_t nowMonotonicSec)
{
    if (!seenLock)
        return false;
    xSemaphoreTake(seenLock, portMAX_DELAY);
    bool due = false;
    if (dirty) {
        if (!dirtySinceSet) {
            dirtySinceSet = true;
            dirtySinceSec = nowMonotonicSec;
        }
        due = (nowMonotonicSec - dirtySinceSec) >= (uint32_t)SEEN_FLUSH_INTERVAL_SEC;
    }
    bool wrote = false;
    if (due) {
        wrote = WriteBinaryLocked();
        if (!wrote)
            dirtySinceSec = nowMonotonicSec; /* retry after another full interval */
    }
    xSemaphoreGive(seenLock);
    return wrote;
}

/* ---- init / accessors ---- */

bool SeenAircraft_InitWithPaths(const char *csv, const char *tmp)
{
    SeenAircraft_Deinit();
    csvPath = csv;
    tmpPath = tmp;
    seenLock = xSemaphoreCreateMutex();
    if (!seenLock)
        return false;

    xSemaphoreTake(seenLock, portMAX_DELAY);
    /* Prefer the current binary format. Only if that is missing/corrupt
     * (fresh install, or upgrading from a build that only ever wrote the
     * legacy CSV) do we fall back to the old text format - same "real file,
     * else recover from the temp file" logic as before - and then mark
     * every loaded record dirty so the next flush performs the one-time
     * migration write into the binary file. */
    size_t loadedFromBinary = 0;
    bool loaded = LoadBinaryFile(SEEN_BIN_PATH, &loadedFromBinary);
    size_t good = 0, bad = 0;
    if (!loaded) {
        loaded = LoadFile(csvPath, &good, &bad);
        if (!loaded && FileExists(tmpPath)) {
            ESP_LOGW(TAG, "Recovering seen-aircraft history from %s", tmpPath);
            loaded = LoadFile(tmpPath, &good, &bad);
        }
        if (loaded) {
            for (size_t i = 0; i < recordCount; i++)
                recordDirty[i] = true;
            dirty = true;
            ESP_LOGI(TAG, "Migrating %u seen-aircraft record(s) from legacy CSV to %s",
                     (unsigned)recordCount, SEEN_BIN_PATH);
        }
    }
    xSemaphoreGive(seenLock);

    if (loaded && loadedFromBinary)
        ESP_LOGI(TAG, "Loaded %u seen-aircraft records from %s", (unsigned)loadedFromBinary, SEEN_BIN_PATH);
    else if (loaded)
        ESP_LOGI(TAG, "Loaded %u seen-aircraft records (%u malformed lines skipped)", (unsigned)good, (unsigned)bad);
    else
        ESP_LOGI(TAG, "No saved seen-aircraft history yet");
    return true;
}

bool SeenAircraft_Init(void)
{
    return SeenAircraft_InitWithPaths(SEEN_CSV_PATH, SEEN_CSV_TMP_PATH);
}

void SeenAircraft_Deinit(void)
{
    free(records);
    records = NULL;
    recordCount = 0;
    recordCapacity = 0;
    dirty = false;
    dirtySinceSet = false;
    dirtySinceSec = 0;
    memset(recordDirty, 0, sizeof(recordDirty));
    /* The mutex handle is left allocated on purpose: on the target the module
     * is initialized once and never torn down. */
}

size_t SeenAircraft_Count(void)
{
    if (!seenLock)
        return 0;
    xSemaphoreTake(seenLock, portMAX_DELAY);
    size_t n = recordCount;
    xSemaphoreGive(seenLock);
    return n;
}

bool SeenAircraft_IsDirty(void)
{
    if (!seenLock)
        return false;
    xSemaphoreTake(seenLock, portMAX_DELAY);
    bool d = dirty;
    xSemaphoreGive(seenLock);
    return d;
}

bool SeenAircraft_Get(size_t index, SeenRecord *out)
{
    if (!seenLock || !out)
        return false;
    xSemaphoreTake(seenLock, portMAX_DELAY);
    bool ok = index < recordCount;
    if (ok)
        *out = records[index];
    xSemaphoreGive(seenLock);
    return ok;
}

bool SeenAircraft_Find(const char *icao24, SeenRecord *out)
{
    char icao[SEEN_ICAO_MAX];
    if (!seenLock || !SeenAircraft_NormalizeIcao(icao24, icao))
        return false;
    xSemaphoreTake(seenLock, portMAX_DELAY);
    int idx = FindIndex(icao);
    if (idx >= 0 && out)
        *out = records[idx];
    xSemaphoreGive(seenLock);
    return idx >= 0;
}

void SeenAircraft_Clear(void)
{
    if (!seenLock)
        return;
    xSemaphoreTake(seenLock, portMAX_DELAY);
    recordCount = 0;
    dirty = false;
    dirtySinceSet = false;
    memset(recordDirty, 0, sizeof(recordDirty));
    remove(csvPath);
    remove(tmpPath);
    remove(SEEN_BIN_PATH);
    remove(SEEN_BIN_TMP_PATH);
    xSemaphoreGive(seenLock);
    ESP_LOGI(TAG, "Seen-aircraft history cleared");
}

/* ---- query ---- */

static bool ContainsNoCase(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0)
        return true;
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, nl) == 0)
            return true;
    }
    return false;
}

static bool MatchesSearch(const SeenRecord *r, const char *search, bool haveInfo, const SeenConfigInfo *info)
{
    if (ContainsNoCase(r->icao24, search) || ContainsNoCase(r->callsign, search) ||
        ContainsNoCase(r->operatorName, search))
        return true;
    return haveInfo && (ContainsNoCase(info->registryPrefix, search) || ContainsNoCase(info->registryNote, search) ||
                        ContainsNoCase(info->configuredOperator, search) || ContainsNoCase(info->operatorCode, search));
}

/* qsort has no context argument; the lock is held for the whole query, so a
 * file-scope context is safe. */
static SeenSortKey sortKey;
static bool sortDescending;

static int CompareU32(uint32_t a, uint32_t b) { return (a > b) - (a < b); }

/* Empty strings sort last when ascending. */
static int CompareText(const char *a, const char *b)
{
    if (!a[0] && !b[0]) return 0;
    if (!a[0]) return 1;
    if (!b[0]) return -1;
    return strcasecmp(a, b);
}

static int CompareIndex(const void *pa, const void *pb)
{
    uint16_t ia = *(const uint16_t *)pa, ib = *(const uint16_t *)pb;
    const SeenRecord *a = &records[ia], *b = &records[ib];
    int c = 0;
    switch (sortKey) {
    case SEEN_SORT_FIRST_SEEN: c = CompareU32(a->firstSeen, b->firstSeen); break;
    case SEEN_SORT_COUNT: c = CompareU32(a->seenCount, b->seenCount); break;
    case SEEN_SORT_CALLSIGN: c = CompareText(a->callsign, b->callsign); break;
    case SEEN_SORT_ICAO24: c = strcmp(a->icao24, b->icao24); break;
    case SEEN_SORT_OPERATOR: c = CompareText(a->operatorName, b->operatorName); break;
    case SEEN_SORT_LAST_SEEN:
    default: c = CompareU32(a->lastSeen, b->lastSeen); break;
    }
    if (sortDescending)
        c = -c;
    if (c != 0)
        return c;
    c = strcmp(a->icao24, b->icao24); /* deterministic tie-break */
    return c ? c : (ia > ib) - (ia < ib);
}

size_t SeenAircraft_Query(const SeenQuery *query, size_t offset, size_t limit,
                          SeenRecord *out, size_t *totalMatches)
{
    if (totalMatches)
        *totalMatches = 0;
    if (!seenLock || !query || !out)
        return 0;

    const char *search = query->search ? query->search : "";
    bool needInfo = (query->filter != SEEN_FILTER_ALL);

    xSemaphoreTake(seenLock, portMAX_DELAY);
    uint16_t *matches = recordCount ? malloc(recordCount * sizeof(uint16_t)) : NULL;
    if (recordCount && !matches) {
        xSemaphoreGive(seenLock);
        return 0;
    }

    size_t matchCount = 0;
    for (size_t i = 0; i < recordCount; i++) {
        const SeenRecord *r = &records[i];
        SeenConfigInfo info = {0};
        bool haveInfo = false;
        /* The registry/operator lookup is only paid for when a filter or an
         * unsuccessful cheap search needs it. */
        if (needInfo) {
            SeenAircraft_Describe(r, &info);
            haveInfo = true;
            if ((query->filter == SEEN_FILTER_CONFIGURED) != info.configured)
                continue;
        }
        if (search[0]) {
            bool hit = MatchesSearch(r, search, haveInfo, &info);
            if (!hit && !haveInfo) {
                SeenAircraft_Describe(r, &info);
                hit = MatchesSearch(r, search, true, &info);
            }
            if (!hit)
                continue;
        }
        matches[matchCount++] = (uint16_t)i;
    }

    sortKey = query->sort < SEEN_SORT_KEY_COUNT ? query->sort : SEEN_SORT_LAST_SEEN;
    sortDescending = query->descending;
    if (matchCount > 1)
        qsort(matches, matchCount, sizeof(uint16_t), CompareIndex);

    size_t copied = 0;
    for (size_t i = offset; i < matchCount && copied < limit; i++)
        out[copied++] = records[matches[i]];

    free(matches);
    xSemaphoreGive(seenLock);
    if (totalMatches)
        *totalMatches = matchCount;
    return copied;
}
