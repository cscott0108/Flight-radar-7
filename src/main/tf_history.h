#pragma once

/* TF (microSD) persistent aircraft history - the WARM/long-term layer
 * beneath the existing RAM "Hot Seen" cache (seen_aircraft.h), which is
 * unchanged by this file (PROMPT.md section 18-19).
 *
 * Storage model (PROMPT.md sections 9-24):
 *
 *   /sdcard/history/index.dat        one fixed-size open-addressing hash
 *                                     table (icao24 -> bucket + offset),
 *                                     read/written a slot at a time - never
 *                                     loaded into RAM as a whole.
 *   /sdcard/history/<FPHEX8>.dat      one bucket per Universal Value
 *                                     fingerprint (e.g. 7A3F91C2.dat),
 *                                     append-only + in-place-update record
 *                                     store, holding every aircraft ever
 *                                     associated with that Universal Value.
 *   /sdcard/history/00000000.dat      UNASSIGNED bucket: aircraft that
 *                                     resolve to no registry rule and no
 *                                     Universal Value.
 *   /sdcard/history/FFFFFFFE.dat      REGISTRY_DEFINED bucket: aircraft
 *                                     classified by a Registry Rule
 *                                     (custom_rules.csv), independent of
 *                                     the Universal Value table.
 *   FFFFFFFF (RESERVED) is never written.
 *
 * This groups/indexes aircraft records instead of one file per aircraft
 * (PROMPT.md section 21) and keeps normal Universal Value storage keyed by
 * the stable 32-bit FNV-1a fingerprint, never the reusable 16-bit code
 * (PROMPT.md section 10).
 *
 * Reliability: TF absent/unmountable/full/corrupt never blocks the radar -
 * every function here degrades to returning false rather than blocking or
 * aborting (PROMPT.md section 27). A corrupt record is detected by its
 * stored CRC-32 and treated as absent (safe miss) rather than trusted; it
 * self-heals the next time that aircraft is next written (PROMPT.md
 * section 24). A missing/corrupt index is rebuilt by scanning the existing
 * bucket files (still bounded - one sequential pass, not held in RAM). */

#include <stdbool.h>
#include <stdint.h>

#define TF_ICAO_MAX 9
#define TF_REGISTRY_MAX 12
#define TF_CALLSIGN_MAX 9
#define TF_IDENTITY_MAX 40

/* ---- special bucket fingerprints (PROMPT.md section 13-15) ---- */
#define TF_BUCKET_UNASSIGNED        0x00000000u
#define TF_BUCKET_REGISTRY_DEFINED  0xFFFFFFFEu
#define TF_BUCKET_RESERVED          0xFFFFFFFFu

typedef struct {
    char icao24[TF_ICAO_MAX];
    char registry[TF_REGISTRY_MAX]; /* may be empty - not always known */
    char callsign[TF_CALLSIGN_MAX]; /* most recent, may be empty */
    uint16_t universalValueCode;    /* informational snapshot only; the bucket fingerprint is authoritative */
    uint8_t craftType;              /* CraftType at last write */
    uint8_t aircraftType;           /* AircraftType at last write */
    uint8_t classificationSource;   /* CraftSource at last write, diagnostics only */
    uint8_t reserved;
    uint32_t firstSeen;             /* UTC epoch seconds */
    uint32_t lastSeen;              /* UTC epoch seconds */
    uint32_t seenCount;
} TfHistoryRecord;

/* Call once at boot, after CustomRules_Init()/SeenAircraft_Init(). Attempts
 * to mount the TF card and open/validate the index; ALWAYS returns
 * (mount failure is logged and leaves the module in degraded mode, never
 * fatal - PROMPT.md section 27). */
bool TfHistory_Init(void);

/* False in every degraded case (absent, unmounted, full, index unusable). */
bool TfHistory_IsAvailable(void);

/* Finds the persisted record for icao24 in ANY bucket. Used to restore an
 * aircraft's true history when it returns after aging out of the RAM Hot
 * Seen cache (PROMPT.md section 19). */
bool TfHistory_Lookup(const char *icao24, TfHistoryRecord *out, uint32_t *bucketOut);

/* Creates or updates the record for rec->icao24 in the named bucket
 * (a Universal Value fingerprint, or one of the two special buckets above;
 * TF_BUCKET_RESERVED is refused). The caller (history_manager.c) is
 * responsible for coalescing - this always performs the write it is asked
 * to do (PROMPT.md section 23). */
bool TfHistory_Upsert(uint32_t bucketFingerprint, const TfHistoryRecord *rec);

/* Rebuilds index.dat from scratch by scanning every existing bucket file.
 * Called automatically by TfHistory_Init() when the index is missing or
 * fails its size/header sanity check; also exposed for /diag. */
bool TfHistory_RebuildIndex(void);

typedef struct {
    bool mounted;
    uint64_t capacityBytes;
    uint64_t usedBytes;
    uint64_t freeBytes;
    uint32_t indexSlotsTotal;
    uint32_t indexSlotsUsed;

    uint32_t writes;     /* successful Upsert calls (create + update) */
    uint32_t creates;
    uint32_t updates;
    uint32_t lookups;
    uint32_t lookupMisses;
    uint32_t errors;      /* I/O or CRC failures encountered and safely handled */
    uint32_t recoveryOps; /* records treated as corrupt (bad CRC) */
    uint32_t indexRebuilds;

    uint32_t lastLookupUs, bestLookupUs, worstLookupUs;
    uint32_t lastWriteUs, bestWriteUs, worstWriteUs;
    uint32_t lastIndexOpUs, bestIndexOpUs, worstIndexOpUs;
    uint32_t lastRecoveryUs, bestRecoveryUs, worstRecoveryUs;
} TfHistoryStats;

void TfHistory_GetStats(TfHistoryStats *out);
