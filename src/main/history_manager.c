#include "history_manager.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#else
#include <stdlib.h>
#endif

#include "aircraft_provider.h" /* MAX_AIRCRAFT */
#include "diag_telemetry.h"
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
static volatile bool s_tfReady = false; /* volatile: also toggled from the web task (diag pause/resume) */
static uint32_t s_lastSyncMonotonicSec = 0;
static bool s_haveLastSync = false;

/* Manual flush hand-off. The web task only bumps s_flushReq and waits; the
 * poll task (sole owner of the shadow table) performs the pass from
 * FlushIfDue and publishes the result BEFORE advancing s_flushServed
 * (release/acquire), so a requester that sees its sequence served also sees
 * that pass's result. Nothing here touches the periodic schedule. */
static _Atomic uint32_t s_flushReq = 0;
static _Atomic uint32_t s_flushServed = 0;
static _Atomic uint32_t s_resOk = 0, s_resWritten = 0, s_resFailed = 0, s_resRemaining = 0, s_resReason = 0;

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

void HistoryManager_ObserveRegistration(const char *icao24, const char *registration)
{
    if (!icao24 || !registration || !registration[0])
        return;
    char norm[TF_ICAO_MAX];
    if (!SeenAircraft_NormalizeIcao(icao24, norm))
        return;
    HmSlot *s = FindSlot(norm);
    if (!s || strncmp(s->rec.registry, registration, TF_REGISTRY_MAX - 1) == 0)
        return;
    memset(s->rec.registry, 0, sizeof(s->rec.registry));
    strncpy(s->rec.registry, registration, TF_REGISTRY_MAX - 1);
    s->dirty = true;
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

/* One write pass over the dirty shadow slots (poll task only). */
static bool FlushDirtySlots(uint32_t *written, uint32_t *failed, uint32_t *remaining)
{
    bool wroteAny = false;
    uint32_t t0 = DiagTelemetry_NowMs();
    *written = *failed = *remaining = 0;
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
            (*written)++;
        } else {
            (*failed)++;
            (*remaining)++;
        }
    }
    /* Only passes that actually had dirty entries count as a run (idle passes would drown the numbers). */
    if (*written + *failed > 0)
        DiagTelemetry_OpEnd(DT_OP_TF_FLUSH, t0, *failed == 0, "TF write failed");
    return wroteAny;
}

static bool AnyDirty(void)
{
    for (size_t i = 0; s_slots && i < HM_CAP; i++)
        if (s_slots[i].used && s_slots[i].dirty)
            return true;
    return false;
}

static void ServeManualFlush(void)
{
    uint32_t req = atomic_load_explicit(&s_flushReq, memory_order_acquire);
    if (req == atomic_load_explicit(&s_flushServed, memory_order_relaxed))
        return;
    uint32_t w = 0, f = 0, rem = 0, reason = 0;
    if (!s_slots || !s_tfReady || !TfHistory_IsAvailable()) {
        reason = 1; /* TF unavailable: nothing could be written */
        if (AnyDirty())
            DiagTelemetry_OpDone(DT_OP_TF_FLUSH, DT_RES_SKIPPED, 0, NULL);
    }
    else
        FlushDirtySlots(&w, &f, &rem);
    atomic_store_explicit(&s_resWritten, w, memory_order_relaxed);
    atomic_store_explicit(&s_resFailed, f, memory_order_relaxed);
    atomic_store_explicit(&s_resRemaining, rem, memory_order_relaxed);
    atomic_store_explicit(&s_resReason, reason, memory_order_relaxed);
    atomic_store_explicit(&s_resOk, (reason == 0 && f == 0) ? 1u : 0u, memory_order_relaxed);
    atomic_store_explicit(&s_flushServed, req, memory_order_release);
}

bool HistoryManager_FlushIfDue(uint32_t nowMonotonicSec)
{
    ServeManualFlush(); /* honoured every slice, independent of the interval gate */

    if (s_haveLastSync && (nowMonotonicSec - s_lastSyncMonotonicSec) < HM_SYNC_INTERVAL_SEC)
        return false;
    s_lastSyncMonotonicSec = nowMonotonicSec;
    s_haveLastSync = true;

    /* Keep Universal Values current against operators.csv before writing,
     * so web_rules.c needs no changes of its own (PROMPT.md section 7/8). */
    UniversalValue_Sync();

    if (!s_slots || !s_tfReady || !TfHistory_IsAvailable()) {
        if (AnyDirty())
            DiagTelemetry_OpDone(DT_OP_TF_FLUSH, DT_RES_SKIPPED, 0, NULL); /* due, but TF is unavailable */
        return false;
    }

    uint32_t w, f, rem;
    return FlushDirtySlots(&w, &f, &rem);
}

uint32_t HistoryManager_RequestFlush(void)
{
    return atomic_fetch_add_explicit(&s_flushReq, 1u, memory_order_acq_rel) + 1u;
}

bool HistoryManager_GetFlushResult(uint32_t seq, HistoryFlushResult *out)
{
    /* Wrap-safe "served >= seq". */
    uint32_t served = atomic_load_explicit(&s_flushServed, memory_order_acquire);
    if ((int32_t)(served - seq) < 0)
        return false;
    if (out) {
        out->ok = atomic_load_explicit(&s_resOk, memory_order_relaxed) != 0;
        out->written = atomic_load_explicit(&s_resWritten, memory_order_relaxed);
        out->failed = atomic_load_explicit(&s_resFailed, memory_order_relaxed);
        out->remaining = atomic_load_explicit(&s_resRemaining, memory_order_relaxed);
        out->tfUnavailable = atomic_load_explicit(&s_resReason, memory_order_relaxed) != 0;
    }
    return true;
}

void HistoryManager_TfPause(void)
{
    s_tfReady = false; /* Observe/Flush skip TF; dirty shadow slots are kept untouched */
}

void HistoryManager_TfResume(void)
{
    /* Ready again only if a TF (re)init really made history available. */
    s_tfReady = (s_slots != NULL) && TfHistory_IsAvailable();
}

bool HistoryManager_IsTfReady(void)
{
    return s_tfReady;
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
