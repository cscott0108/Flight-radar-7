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
#include <stddef.h>
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

/* Rebuilds the index from scratch by scanning EVERY bucket file in the
 * history folder (special buckets and all operator-fingerprint buckets) and
 * keeping, per ICAO24, the newest record (highest lastSeen, then highest
 * seenCount, then highest (fingerprint, offset)). Never modifies a bucket
 * file or the legacy index.dat. History is unavailable while it runs.
 * Used by TfHistory_Init() when indexv2.dat is missing/building/invalid
 * (first boot after upgrading from the 4096-slot index = migration). */
bool TfHistory_RebuildIndex(void);

/* Index load state (index capacity is TF_INDEX_SLOTS; see tf_history.c). */
typedef enum {
    TF_LOAD_OK = 0,
    TF_LOAD_WARN,  /* >= 70% of slots used */
    TF_LOAD_CAP    /* >= 90%: hard cap, NEW aircraft are not written to History */
} TfIndexLoad;

#define TF_PROBE_HIST_BINS 6 /* probes per index operation: 1, 2-3, 4-7, 8-15, 16-31, 32+ */

typedef struct {
    bool mounted;          /* filesystem mount succeeded (esp_vfs_fat_sdspi_mount returned OK) and has not been unmounted */
    bool historyAvailable; /* TfHistory_IsAvailable(): mounted AND directory/index usable, i.e. writes are accepted */
    uint64_t capacityBytes;
    uint64_t usedBytes;
    uint64_t freeBytes;
    uint32_t indexSlotsTotal;
    uint32_t indexSlotsUsed;   /* distinct aircraft in the index (exact: counted at load, +1 per new aircraft) */
    uint32_t indexWarnAt;      /* slots used at which the load state becomes WARN (70%) */
    uint32_t indexHardCap;     /* slots used at which NEW aircraft are refused (90%) */
    uint8_t indexLoadState;    /* TfIndexLoad */

    /* Insertion outcomes (radar write path only) */
    uint32_t indexInsertCapRejected; /* new-aircraft writes refused at the hard cap: NOTHING was written to a bucket */
    uint32_t indexInsertIoFail;      /* record written but the index slot write failed (orphan, adopted on retry) */
    uint32_t orphanAdopted;          /* retries that reused the orphan record instead of appending a duplicate */

    /* Probe behavior (radar Lookup/Upsert only; /history browsing is not counted) */
    uint32_t probeHitOps, probeMissOps;
    uint64_t probeHitTotal, probeMissTotal; /* slots read: avg = total / ops */
    uint32_t probeMax, probeLast;
    uint32_t probeHist[TF_PROBE_HIST_BINS];

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

/* ===================================================================
 * TF diagnostics (read by /diag; nothing here changes storage behavior)
 * ===================================================================
 *
 * Every init stage records its own result so "mounted yes, index 0/4096"
 * can be told apart from "init failed at stage X with esp_err/errno Y".
 * `espErr` is an esp_err_t, `errnoVal` is errno captured immediately after
 * the failing call, `fatfsErr` is a FRESULT where the API exposes one
 * (f_getfree); 0 means "not applicable / OK". */

typedef enum {
    TF_RES_NOT_RUN = 0,
    TF_RES_OK = 1,
    TF_RES_FAIL = 2,
    TF_RES_SKIPPED = 3
} TfResult;

typedef struct {
    uint8_t result;   /* TfResult */
    uint8_t fatfsErr; /* FRESULT, 0 = none */
    int16_t errnoVal;
    int32_t espErr;
    uint32_t us;
} TfStepInfo;

typedef enum {
    TF_STAGE_SPI_BUS = 0,     /* spi_bus_initialize (INVALID_STATE = already initialized, tolerated) */
    TF_STAGE_MOUNT,           /* esp_vfs_fat_sdspi_mount */
    TF_STAGE_FS_INFO,         /* f_getfree: capacity/free/filesystem type */
    TF_STAGE_HIST_DIR,        /* /sdcard/history exists or mkdir */
    TF_STAGE_INDEX_CHECK,     /* open indexv2.dat: valid header, committed, exact size */
    TF_STAGE_INDEX_SCAN,      /* (only if no valid index) scan every bucket file into the new index */
    TF_STAGE_INDEX_WRITE,     /* write indexv2.dat in state "building" (legacy index.dat and buckets untouched) */
    TF_STAGE_INDEX_VERIFY,    /* re-read and verify the new index before it is committed */
    TF_STAGE_INDEX_COMMIT,    /* single header write (+fsync) flips building -> committed */
    TF_STAGE_INDEX_COUNT,     /* count occupied slots of an existing committed index */
    TF_STAGE_COUNT
} TfInitStage;

typedef enum {
    TF_INDEX_NOT_INIT = 0, /* init never reached the index (or unmounted) */
    TF_INDEX_LOADED,       /* existing committed indexv2.dat; slots counted */
    TF_INDEX_REBUILT,      /* indexv2.dat was missing/unfinished/invalid: rebuilt from all bucket files */
    TF_INDEX_FAILED,       /* indexv2.dat could not be built/verified/committed */
    TF_INDEX_MIGRATED,     /* first boot after upgrade: legacy 4096-slot index.dat found, new index built beside it */
    TF_INDEX_FRESH         /* no index and no bucket files: new empty index created */
} TfIndexState;

typedef struct {
    bool attempted;        /* TfHistory_Init/Reinit has run at least once */
    bool ok;               /* last init made history available */
    int8_t firstFailedStage; /* TfInitStage of the first failing stage in the last init, -1 = none */
    uint8_t indexState;    /* TfIndexState */
    uint8_t fsType;        /* 0 unknown, 1 FAT12, 2 FAT16, 3 FAT32, 4 exFAT (FATFS fs_type) */
    bool busAlreadyInit;   /* spi_bus_initialize said INVALID_STATE (tolerated by the existing code) */
    bool dirExisted;       /* history dir was already there before init tried mkdir */
    bool indexFileOpened;  /* indexv2.dat could be opened for reading */
    bool indexSizeValid;   /* header valid + committed + exact size */
    int32_t indexSizeBytes;     /* -1 = unknown/missing */
    uint32_t indexSizeExpected;
    int32_t legacyIndexSizeBytes; /* legacy index.dat size, -1 = not present (left untouched, read-only) */
    int32_t stackFreeMinBytes;    /* calling task's minimum free stack after init (app_main has only 3584 B); -1 = n/a */
    uint32_t runs;         /* number of init runs (boot + diagnostic reinit) */
    TfStepInfo stage[TF_STAGE_COUNT];
    int8_t lastErrStage;   /* stage of lastError, -1 = none */
    int32_t lastEspErr;
    int32_t lastErrno;
    char lastError[128];   /* human-readable, e.g. "stage=index_create esp_err=ESP_OK errno=28(...) ..." */
    char lastRuntimeError[48]; /* last failed Upsert/Lookup op (best-effort errno), empty if none */
    const char *historyDir;
    const char *indexPath;
    const char *legacyIndexPath;
    const char *scratchPath;
} TfInitInfo;

/* Result of the last index build (boot-time migration, rebuild or fresh create). */
typedef enum { TF_BUILD_NONE = 0, TF_BUILD_FRESH, TF_BUILD_MIGRATE, TF_BUILD_REBUILD } TfBuildMode;

typedef struct {
    bool ran;
    uint8_t mode;             /* TfBuildMode */
    bool ok;
    bool usedStaging;         /* true = transient PSRAM staging; false = on-disk fallback */
    uint32_t stagingBytes;
    uint32_t buckets;         /* bucket files scanned */
    uint32_t headerFallbackBuckets; /* buckets whose header was bad: record count taken from file size (records still CRC-checked) */
    uint32_t bucketsUnreadable;
    uint32_t recordsScanned;
    uint32_t recordsCorrupt;  /* failed CRC / empty: NOT indexed */
    uint32_t duplicatesResolved; /* extra records for an ICAO24 already seen in the scan */
    uint32_t aircraftIndexed;
    uint32_t rejectedAtCap;   /* aircraft left out because the hard cap was reached */
    bool legacyChecked;
    uint32_t legacyValid, legacySame, legacyNewer, legacyMissing; /* legacy entries vs the new index (report only) */
    uint32_t verifySampled, verifyMismatch;
    uint32_t scanUs, writeUs, verifyUs, commitUs, totalUs;
    char error[96];
} TfIndexBuildReport;

void TfHistory_GetIndexBuildReport(TfIndexBuildReport *out);

#ifndef ESP_PLATFORM
/* Host-test hooks only (never compiled into firmware). */
void TfHistory_HostTestNoStaging(bool force);     /* force the on-disk fallback builder */
void TfHistory_HostTestAbortBuild(int phase);     /* 1 = stop after write, 2 = stop after verify (power-loss simulation) */
void TfHistory_HostTestFailSlotWrites(int n);     /* make the next n index slot writes fail (I/O error simulation) */
#endif

const char *TfHistory_StageName(int stage);
void TfHistory_GetInitInfo(TfInitInfo *out);

/* ---- unmount / reinitialize (real teardown only; no GPIO/power control) ----
 *
 * Unmount: esp_vfs_fat_sdcard_unmount() then spi_bus_free(). Reinit runs the
 * normal init path again (spi_bus_initialize + mount + history dir + index).
 * Both are serialized against Lookup/Upsert by an internal mutex. Neither
 * formats, erases or deletes anything on the card. The caller (web_diag.c)
 * pauses/resumes History Manager around them. */
typedef enum {
    TF_TR_REQUEST = 0,    /* unmount requested */
    TF_TR_FS_UNMOUNT,     /* esp_vfs_fat_sdcard_unmount */
    TF_TR_BUS_FREE,       /* spi_bus_free */
    TF_TR_REMOUNT_REQ,    /* reinit requested (refused if still mounted) */
    TF_TR_BUS_INIT,       /* spi_bus_initialize */
    TF_TR_MOUNT,          /* esp_vfs_fat_sdspi_mount */
    TF_TR_HISTORY_INIT,   /* history dir + index (history available again?) */
    TF_TR_COUNT
} TfTraceStep;

typedef struct {
    bool active;     /* at least one unmount/reinit requested since boot */
    uint32_t runs;   /* number of requests */
    TfStepInfo step[TF_TR_COUNT];
} TfReinitTrace;

bool TfHistory_Unmount(void);
bool TfHistory_Reinit(void);
const char *TfHistory_TraceStepName(int step);
void TfHistory_GetReinitTrace(TfReinitTrace *out);

/* ---- non-destructive filesystem self-test ----
 *
 * Uses ONLY a scratch file (TfInitInfo.scratchPath): create, write 64 known
 * bytes, fflush+fsync, close, reopen, read, verify, delete. Never touches
 * index.dat, bucket files, Seen data or History Manager state. Needs the
 * card mounted; does not need history to be available. */
typedef enum {
    TF_ST_LOCK = 0,
    TF_ST_MOUNTED,
    TF_ST_DIR,
    TF_ST_OPEN_W,
    TF_ST_WRITE,
    TF_ST_FLUSH,
    TF_ST_CLOSE_W,
    TF_ST_OPEN_R,
    TF_ST_READ,
    TF_ST_VERIFY,
    TF_ST_CLOSE_R,
    TF_ST_DELETE,
    TF_ST_COUNT
} TfSelfTestStep;

typedef struct {
    bool ran;
    bool passed;
    int8_t failedStep; /* TfSelfTestStep, -1 = none */
    uint32_t runs;
    uint32_t totalUs, writeUs, readUs;
    TfStepInfo step[TF_ST_COUNT];
} TfSelfTestResult;

bool TfHistory_RunSelfTest(void);
const char *TfHistory_SelfTestStepName(int step);
void TfHistory_GetSelfTest(TfSelfTestResult *out);

/* ===================================================================
 * Read-only browsing (used by /history; web_history.c)
 * ===================================================================
 *
 * Strictly observational: none of these calls writes, renames or deletes
 * anything, none touches the index or bucket files except to read them,
 * and none updates the lifetime counters in TfHistoryStats.
 *
 * Resource bounds (by construction, not by convention):
 *   - no heap allocation; at most TF_BROWSE_CHUNK_MAX records (8 x 52 B on
 *     disk) are in RAM per call;
 *   - the TF mutex is held for ONE bounded call (one bucket read, or one
 *     directory pass) and never while any HTTP output is produced;
 *   - every call re-checks availability, so an unmount/reinit between
 *     chunks simply ends the page with TF_BR_UNAVAILABLE.
 *
 * Buckets are visited in ascending fingerprint order (00000000 =
 * unassigned first, FFFFFFFE = registry-defined last) by finding "the
 * smallest fingerprint greater than X" with one directory pass each time:
 * O(1) RAM, stable order, no list of buckets is ever held. */

#define TF_BROWSE_CHUNK_MAX 8u

typedef enum {
    TF_BR_OK = 0,
    TF_BR_UNAVAILABLE, /* card not mounted / history not available (e.g. during diagnostic unmount) */
    TF_BR_BUSY,        /* could not get the TF mutex within the browse wait; nothing was read */
    TF_BR_END,         /* no further bucket / record */
    TF_BR_IO           /* directory or file could not be read */
} TfBrowseStatus;

typedef enum {
    TF_REC_LIVE = 0,       /* the index points at exactly this record: authoritative */
    TF_REC_SUPERSEDED,     /* the index points at a different record (other bucket or a later one) for this aircraft */
    TF_REC_NOT_INDEXED,    /* index is readable but has no entry for this aircraft (e.g. index rebuilt without this bucket) */
    TF_REC_INDEX_UNKNOWN,  /* index.dat could not be read: liveness not determined */
    TF_REC_CORRUPT         /* CRC mismatch, short read or empty slot: contents not trusted/shown */
} TfRecState;

typedef struct {
    TfHistoryRecord rec; /* valid unless state == TF_REC_CORRUPT */
    uint8_t state;       /* TfRecState */
} TfBrowseRecord;

/* Smallest bucket fingerprint strictly greater than `after` (or the smallest
 * of all when afterValid is false). *headerOk is false if that bucket's
 * 20-byte header is missing or fails its CRC (then *recordCount is 0). */
TfBrowseStatus TfHistory_NextBucket(bool afterValid, uint32_t after, uint32_t *fpOut,
                                    uint32_t *recordCountOut, bool *headerOkOut);

/* Header record count of one specific bucket. TF_BR_END if the file does not
 * exist or its header is unreadable. */
TfBrowseStatus TfHistory_BucketInfo(uint32_t fp, uint32_t *recordCountOut);

/* Reads up to maxN (<= TF_BROWSE_CHUNK_MAX) consecutive records of bucket fp
 * starting at record index startIdx and classifies each against the index.
 * *gotN = records returned; *recordCountOut = the bucket's current count.
 * TF_BR_END with *gotN == 0 means startIdx is at/after the end. */
TfBrowseStatus TfHistory_BrowseChunk(uint32_t fp, uint32_t startIdx, size_t maxN,
                                     TfBrowseRecord *out, size_t *gotN, uint32_t *recordCountOut);

/* Up to maxN (<= TF_LIST_MAX) bucket fingerprints strictly greater than
 * `after` (all when afterValid is false), in ascending order, from ONE
 * directory pass; no file is opened. *totalOut = number of buckets on the
 * card, *moreOut = more buckets follow the last one returned. Counts come
 * from TfHistory_BucketInfo (one header read each, separate lock). */
#define TF_LIST_MAX 50u
TfBrowseStatus TfHistory_ListBuckets(bool afterValid, uint32_t after, size_t maxN, uint32_t *fps,
                                     size_t *gotN, uint32_t *totalOut, bool *moreOut);

/* Same as TfHistory_BrowseChunk without the index check: for /history search
 * scans (records CRC-checked; state is TF_REC_CORRUPT or TF_REC_INDEX_UNKNOWN,
 * the caller classifies matches with TfHistory_IsLive). */
TfBrowseStatus TfHistory_ScanChunk(uint32_t fp, uint32_t startIdx, size_t maxN,
                                   TfBrowseRecord *out, size_t *gotN, uint32_t *recordCountOut);

/* Is the record at (fp, recordIdx) the one the index currently considers
 * authoritative for icao24? Pure read of index.dat; result in *stateOut
 * (TfRecState: LIVE, SUPERSEDED, NOT_INDEXED or INDEX_UNKNOWN). */
TfBrowseStatus TfHistory_IsLive(const char *icao24, uint32_t fp, uint32_t recordIdx, uint8_t *stateOut);

/* Exact ICAO24 lookup through the existing index WITHOUT touching the
 * lookup/miss counters or timings (unlike TfHistory_Lookup, which is the
 * History Manager's counted path). TF_BR_END = not in the index. */
TfBrowseStatus TfHistory_FindForBrowse(const char *icao24, TfBrowseRecord *out, uint32_t *fpOut, uint32_t *recordIdxOut);
