#include "history_manager.h"

#include <ctype.h>
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
    /* 0.1.4 call-sign history: a record for this aircraft is stored on the card
     * (restored by lookup or written by a flush), and the in-RAM record is a NEW
     * record to append because the call sign changed since then. */
    bool stored;
    bool newRecord;
    char storedCallsign[TF_CALLSIGN_MAX]; /* call sign of the record on the card */
    uint32_t bkFirstSeen, bkLastSeen, bkSeenCount; /* that record's statistics while a new one is pending */
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

/* 0.1.5 eviction bookkeeping (poll task only; /diag reads it unlocked, informational). */
static uint32_t s_evictClean, s_evictAfterWrite, s_evictWriteFail, s_admitSkipped;
/* After a failed eviction write no further synchronous write is attempted until the
 * next flush pass (which retries every dirty slot anyway): bounds the extra TF work
 * to one attempt per pass while the card is failing. */
static bool s_evictWriteBlocked;

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
    s_evictClean = s_evictAfterWrite = s_evictWriteFail = s_admitSkipped = 0;
    s_evictWriteBlocked = false;

    UniversalValue_Init(); /* degrades to "no Universal Values" internally on its own PSRAM failure */

    if (!s_slots) {
        s_tfReady = false; /* no shadow table to populate; stay fully degraded */
        return false;
    }

    s_tfReady = TfHistory_Init(); /* false = degraded mode; never fatal (PROMPT.md section 27) */
    return s_tfReady;
}

/* 0.1.4: call signs compared without trailing spaces (OpenSky pads to 8 characters,
 * adsb.lol trims) and not case-sensitive; all-blank = no call sign. */
static size_t CsLen(const char *cs)
{
    size_t n = cs ? strnlen(cs, TF_CALLSIGN_MAX - 1) : 0;
    while (n && cs[n - 1] == ' ')
        n--;
    return n;
}

static bool SameCs(const char *a, const char *b)
{
    const size_t n = CsLen(a);
    if (n != CsLen(b))
        return false;
    for (size_t i = 0; i < n; i++)
        if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i]))
            return false;
    return true;
}

static void NoteStored(HmSlot *s)
{
    s->stored = true;
    s->newRecord = false;
    memcpy(s->storedCallsign, s->rec.callsign, sizeof(s->storedCallsign));
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

/* Writes one dirty slot immediately (eviction path - see AllocSlot). True only when
 * the record is on the card, so the caller may reuse the slot. Nothing is changed on
 * failure: the slot stays dirty and is retried by the next flush. */
static bool FlushSlotNow(HmSlot *s)
{
    if (!s->dirty)
        return true;
    if (s->bucketFingerprint == TF_BUCKET_RESERVED)
        return false;
    if (!TfHistory_UpsertRecord(s->bucketFingerprint, &s->rec, s->newRecord))
        return false;
    s->dirty = false;
    s->haveTf = true;
    NoteStored(s);
    return true;
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
    /* Table full (0.1.5). Unsaved history is never dropped to make room:
     * 1. reuse the least-recently-seen CLEAN slot (its latest state is already on
     *    the card; a later sighting restores it by lookup);
     * 2. only if every slot holds unsaved changes, write the least-recently-seen one
     *    now and reuse it if - and only if - that write succeeded;
     * 3. otherwise (write failed, TF paused/unavailable) keep every slot and do not
     *    admit the new aircraft this time: NULL makes Observe skip it; it is still in
     *    Hot Seen and is offered again on its next poll. */
    size_t oldestIdx = HM_CAP;
    uint32_t oldestSeen = UINT32_MAX;
    for (size_t i = 0; i < HM_CAP; i++) {
        if (!s_slots[i].dirty && (oldestIdx == HM_CAP || s_slots[i].rec.lastSeen < oldestSeen)) {
            oldestSeen = s_slots[i].rec.lastSeen;
            oldestIdx = i;
        }
    }
    if (oldestIdx < HM_CAP) {
        s_evictClean++;
    } else {
        oldestIdx = 0;
        for (size_t i = 1; i < HM_CAP; i++)
            if (s_slots[i].rec.lastSeen < s_slots[oldestIdx].rec.lastSeen)
                oldestIdx = i;
        const bool canWrite = !s_evictWriteBlocked && s_tfReady && TfHistory_IsAvailable();
        if (!canWrite || !FlushSlotNow(&s_slots[oldestIdx])) {
            if (canWrite) {
                s_evictWriteFail++;
                s_evictWriteBlocked = true;
            }
            s_admitSkipped++;
            return NULL; /* every pending record kept */
        }
        s_evictAfterWrite++;
    }
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
                NoteStored(s);
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

    /* 0.1.4 call-sign history: a different (non-empty) call sign than the
     * record being kept starts a NEW record once a record for this aircraft is
     * stored, so the stored one is never overwritten with another call sign.
     * The new record gets its own first seen and Seen count (the stored record
     * keeps its own; nothing is carried over or merged). Only the call sign
     * present at a flush is written: several changes within one flush interval
     * keep the last. A bucket change with the same call sign (reclassification,
     * or the boot-time UNASSIGNED fallback before Universal Values sync) keeps
     * the existing move semantics. */
    if (s->stored && CsLen(callsign) && CsLen(s->storedCallsign)) {
        if (!SameCs(callsign, s->storedCallsign)) {
            if (!s->newRecord || !SameCs(callsign, s->rec.callsign)) {
                if (!s->newRecord) { /* keep the stored record's figures in case it comes back before a flush */
                    s->bkFirstSeen = s->rec.firstSeen;
                    s->bkLastSeen = s->rec.lastSeen;
                    s->bkSeenCount = s->rec.seenCount;
                }
                s->rec.firstSeen = (nowUtc > 0) ? (uint32_t)nowUtc : 0;
                s->rec.lastSeen = 0; /* the visit below counts as this record's first */
                s->rec.seenCount = 0;
                s->newRecord = true;
            }
        } else if (s->newRecord) { /* A -> B -> A before a flush: still the stored record, no new one */
            s->rec.firstSeen = s->bkFirstSeen;
            s->rec.lastSeen = s->bkLastSeen;
            s->rec.seenCount = s->bkSeenCount;
            s->newRecord = false;
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

    if (CsLen(callsign)) { /* 0.1.4: an all-blank call sign no longer replaces a real one */
        memset(s->rec.callsign, 0, sizeof(s->rec.callsign));
        strncpy(s->rec.callsign, callsign, TF_CALLSIGN_MAX - 1);
    }
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
    s_evictWriteBlocked = false; /* this pass retries everything; eviction may try again after it */
    uint32_t t0 = DiagTelemetry_NowMs();
    *written = *failed = *remaining = 0;
    for (size_t i = 0; i < HM_CAP; i++) {
        HmSlot *s = &s_slots[i];
        if (!s->used || !s->dirty)
            continue;
        if (s->bucketFingerprint == TF_BUCKET_RESERVED)
            continue;
        if (TfHistory_UpsertRecord(s->bucketFingerprint, &s->rec, s->newRecord)) {
            s->dirty = false;
            s->haveTf = true;
            NoteStored(s);
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
    s_evictWriteBlocked = false;
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
    out->evictClean = s_evictClean;
    out->evictAfterWrite = s_evictAfterWrite;
    out->evictWriteFail = s_evictWriteFail;
    out->admitSkipped = s_admitSkipped;
    out->evictWriteBlocked = s_evictWriteBlocked;
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
