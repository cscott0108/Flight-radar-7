#include "history_manager.h"

#include <stdint.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#else
#include <stdlib.h>
#endif

#include "aircraft_provider.h" /* MAX_AIRCRAFT */
#include "seen_aircraft.h"     /* SeenAircraft_NormalizeIcao, SEEN_VISIT_GAP_SEC - reused, not duplicated */
#include "tf_history.h"
#include "universal_value.h"

/* One shadow slot per currently-tracked aircraft, capped the same as the
 * radar's own gAircraft[] - bounded RAM, never the whole archive
 * (PROMPT.md section 19, 42). This is intentionally separate from, and
 * does not replace, the existing Seen Aircraft RAM table. */
#define HM_CAP MAX_AIRCRAFT

/* Piggybacks the same coalescing cadence already proven on this hardware
 * for Seen persistence (PROMPT.md section 23, 25) rather than inventing a
 * second write-interval constant to reason about. */
#define HM_SYNC_INTERVAL_SEC SEEN_FLUSH_INTERVAL_SEC

typedef struct {
    bool used;
    char icao24[TF_ICAO_MAX];
    bool haveTf;               /* true once this slot has a known TF-side record (existing or freshly created) */
    uint32_t bucketFingerprint;
    TfHistoryRecord rec;
    bool dirty;
} HmSlot;

/* Allocated from PSRAM at Init() time, not declared `static`: a plain
 * static HM_CAP-entry array is small on its own (~16 KB), but combined
 * with universal_value.c's table it was enough to overflow this chip's
 * internal DRAM at link time - see universal_value.c's note. A NULL
 * `s_slots` (PSRAM absent/exhausted) degrades History Manager to a no-op
 * (every public function below guards on it) rather than crashing; the
 * radar and Hot Seen are unaffected either way. */
static HmSlot *s_slots = NULL;
static bool s_tfReady = false;
static uint32_t s_lastSyncMonotonicSec = 0;
static bool s_haveLastSync = false;

bool HistoryManager_Init(void)
{
    if (!s_slots) {
#ifdef ESP_PLATFORM
        s_slots = (HmSlot *)heap_caps_calloc(HM_CAP, sizeof(HmSlot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        s_slots = (HmSlot *)calloc(HM_CAP, sizeof(HmSlot));
#endif
    } else {
        memset(s_slots, 0, sizeof(HmSlot) * HM_CAP);
    }
    s_haveLastSync = false;

    UniversalValue_Init(); /* degrades to "no Universal Values" internally on its own PSRAM failure */

    if (!s_slots) {
        s_tfReady = false; /* no shadow table to populate; stay fully degraded */
        return false;
    }

    s_tfReady = TfHistory_Init(); /* false = degraded mode; never fatal (PROMPT.md section 27) */
    return s_tfReady;
}

static HmSlot *FindSlot(const char *icao24)
{
    if (!s_slots)
        return NULL;
    for (size_t i = 0; i < HM_CAP; i++)
        if (s_slots[i].used && strncmp(s_slots[i].icao24, icao24, TF_ICAO_MAX) == 0)
            return &s_slots[i];
    return NULL;
}

/* Flushes one slot immediately (used when evicting a dirty slot to make
 * room - see AllocSlot). Best-effort: a failure here just means that one
 * update is lost, same as any other unflushed dirty record on power loss
 * (PROMPT.md section 23-24). */
static void FlushSlotNow(HmSlot *s)
{
    if (s->dirty && s->haveTf && s->bucketFingerprint != TF_BUCKET_RESERVED) {
        if (TfHistory_Upsert(s->bucketFingerprint, &s->rec))
            s->dirty = false;
    }
}

static HmSlot *AllocSlot(const char *icao24)
{
    if (!s_slots)
        return NULL;
    for (size_t i = 0; i < HM_CAP; i++) {
        if (!s_slots[i].used) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            s_slots[i].used = true;
            strncpy(s_slots[i].icao24, icao24, TF_ICAO_MAX - 1);
            return &s_slots[i];
        }
    }
    /* Table full: evict the least-recently-seen slot (flushing it first if
     * dirty) rather than refusing a new, currently-visible aircraft. */
    size_t oldestIdx = 0;
    uint32_t oldestSeen = UINT32_MAX;
    for (size_t i = 0; i < HM_CAP; i++) {
        if (s_slots[i].rec.lastSeen < oldestSeen) {
            oldestSeen = s_slots[i].rec.lastSeen;
            oldestIdx = i;
        }
    }
    FlushSlotNow(&s_slots[oldestIdx]);
    memset(&s_slots[oldestIdx], 0, sizeof(s_slots[oldestIdx]));
    s_slots[oldestIdx].used = true;
    strncpy(s_slots[oldestIdx].icao24, icao24, TF_ICAO_MAX - 1);
    return &s_slots[oldestIdx];
}

/* Registry classification always wins its own bucket (PROMPT.md section 6,
 * 15); an active Universal Value (derived from operators.csv, section 7-8)
 * is next; anything else - including a match against only the built-in
 * prefix defaults or the plain fallback, neither of which is a configured
 * identity - lands in UNASSIGNED (section 4), useful observational
 * telemetry rather than an error bucket. */
static uint32_t DetermineBucket(CraftResolution resolution, uint16_t *uvCodeOut)
{
    *uvCodeOut = UV_CODE_UNASSIGNED;

    if (resolution.source == CRAFT_SRC_REGISTRY)
        return TF_BUCKET_REGISTRY_DEFINED;

    if (resolution.source == CRAFT_SRC_OPERATOR && resolution.operatorCode[0]) {
        UniversalValue uv;
        if (UniversalValue_FindByOperatorCode(resolution.operatorCode, &uv)) {
            *uvCodeOut = uv.code;
            return uv.fingerprint;
        }
        /* Operator matched but the Universal Value table hasn't synced yet
         * this boot (it syncs on init and every HM_SYNC_INTERVAL_SEC) -
         * falls into UNASSIGNED for now and self-heals on the next
         * successful sync + write. */
    }

    return TF_BUCKET_UNASSIGNED;
}

void HistoryManager_Observe(
    const char *icao24,
    const char *callsign,
    CraftResolution resolution,
    int64_t nowUtc)
{
    if (!icao24 || !icao24[0])
        return;

    char norm[TF_ICAO_MAX];
    if (!SeenAircraft_NormalizeIcao(icao24, norm))
        return;

    HmSlot *s = FindSlot(norm);
    uint16_t uvCode = UV_CODE_UNASSIGNED;
    uint32_t bucket = DetermineBucket(resolution, &uvCode);

    if (!s) {
        s = AllocSlot(norm);
        if (!s)
            return;

        bool restored = false;
        if (s_tfReady && TfHistory_IsAvailable()) {
            uint32_t existingBucket = 0;
            TfHistoryRecord existing;
            if (TfHistory_Lookup(norm, &existing, &existingBucket)) {
                s->rec = existing; /* restores true firstSeen/seenCount - PROMPT.md section 19 */
                s->bucketFingerprint = existingBucket;
                s->haveTf = true;
                restored = true;
            }
        }
        if (!restored) {
            memset(&s->rec, 0, sizeof(s->rec));
            strncpy(s->rec.icao24, norm, TF_ICAO_MAX - 1);
            s->rec.firstSeen = (nowUtc > 0) ? (uint32_t)nowUtc : 0;
            s->rec.seenCount = 0;
            s->haveTf = s_tfReady; /* eligible to be created on next flush, even though nothing exists yet */
            s->bucketFingerprint = bucket;
        }
    }

    /* Visit counting mirrors the existing Seen convention (a gap of at
     * least SEEN_VISIT_GAP_SEC before a new "visit" is counted) so the two
     * layers describe activity consistently - PROMPT.md section 16. */
    bool newVisit = (s->rec.lastSeen == 0) ||
                     (nowUtc > (int64_t)s->rec.lastSeen &&
                      (nowUtc - (int64_t)s->rec.lastSeen) >= SEEN_VISIT_GAP_SEC);
    if (newVisit)
        s->rec.seenCount++;

    if (nowUtc > 0)
        s->rec.lastSeen = (uint32_t)nowUtc;
    if (s->rec.firstSeen == 0 && nowUtc > 0)
        s->rec.firstSeen = (uint32_t)nowUtc;

    if (callsign && callsign[0])
        strncpy(s->rec.callsign, callsign, TF_CALLSIGN_MAX - 1);
    s->rec.craftType = (uint8_t)resolution.type;
    s->rec.aircraftType = (uint8_t)resolution.aircraftType;
    s->rec.classificationSource = (uint8_t)resolution.source;
    s->rec.universalValueCode = uvCode;

    /* A resolved bucket can legitimately change between polls (an operator
     * row edited, a registry rule added/removed); the record moves buckets
     * on the next flush - TfHistory_Upsert already handles that case. */
    s->bucketFingerprint = bucket;
    s->dirty = true;
}

bool HistoryManager_FlushIfDue(uint32_t nowMonotonicSec)
{
    if (s_haveLastSync && (nowMonotonicSec - s_lastSyncMonotonicSec) < HM_SYNC_INTERVAL_SEC)
        return false;
    s_lastSyncMonotonicSec = nowMonotonicSec;
    s_haveLastSync = true;

    /* Keep Universal Values current against operators.csv before writing,
     * so web_rules.c needs no changes of its own (PROMPT.md section 7/8). */
    UniversalValue_Sync();

    if (!s_slots || !s_tfReady || !TfHistory_IsAvailable())
        return false;

    bool wroteAny = false;
    for (size_t i = 0; i < HM_CAP; i++) {
        HmSlot *s = &s_slots[i];
        if (!s->used || !s->dirty)
            continue;
        if (s->bucketFingerprint == TF_BUCKET_RESERVED)
            continue;
        if (TfHistory_Upsert(s->bucketFingerprint, &s->rec)) {
            s->dirty = false;
            s->haveTf = true;
            wroteAny = true;
        }
    }
    return wroteAny;
}

void HistoryManager_GetStats(HistoryManagerStats *out)
{
    if (!out)
        return;
    out->shadowSlotsCap = HM_CAP;
    out->shadowSlotsUsed = 0;
    out->dirtyNow = 0;
    if (!s_slots)
        return; /* degraded: no shadow table allocated */
    for (size_t i = 0; i < HM_CAP; i++) {
        if (s_slots[i].used) {
            out->shadowSlotsUsed++;
            if (s_slots[i].dirty)
                out->dirtyNow++;
        }
    }
}
