#pragma once

/* Persistent "Seen Aircraft" history.
 *
 * One record per ICAO24, regardless of which provider reported it (the same
 * hex from OpenSky and adsb.lol is the same aircraft, so switching providers
 * never duplicates history).
 *
 * Data flow (see PROJECT_STATE.md "Seen Aircraft history"):
 *
 *   provider poll -> gAircraft[] -> SeenAircraft_ObservePoll()
 *     -> RAM table (updated every poll, never touches flash)
 *     -> per-record dirty flag
 *     -> SeenAircraft_FlushIfDue(): at most once per SEEN_FLUSH_INTERVAL_SEC
 *        while there are unsaved changes, writes ONLY the records that
 *        actually changed (typically a few dozen out of up to 1000) plus a
 *        small header, directly into fixed-size slots in
 *        /spiffs/seen_aircraft.dat - not a whole-file rewrite.
 *
 * This replaced an earlier full-CSV-rewrite-every-flush design (measured at
 * ~5 ms/record - roughly 5 seconds at 1000 records, almost entirely SPIFFS
 * write overhead, not formatting or heap pressure - see PROJECT_STATE.md
 * "Persistence timing diagnostics"). The binary file is not meant to be
 * human-read directly; /seen/export still produces a full CSV, generated
 * on demand from the in-memory table, unaffected by this change. The old
 * text CSV (SEEN_CSV_PATH) is read once, if present and the binary file is
 * not, to migrate existing history forward; it is left in place afterwards
 * (harmless, doubles as a human-readable dump) rather than deleted.
 *
 * Bounded storage: at most SEEN_MAX_RECORDS records (about 252 KB in the
 * worst case - SeenRecord is 84 bytes - and about the same on the 1.378 MB
 * SPIFFS partition as a flat file). The in-memory table lives well past the
 * 16 KB threshold past which ESP-IDF's SPIRAM_MALLOC_ALWAYSINTERNAL routes
 * the backing realloc to PSRAM instead of scarce internal heap, so this is a
 * PSRAM cost (6 MB free, typically), not an internal-heap one; see
 * PROJECT_STATE.md for the exact crossover point and the derivation behind
 * this specific cap. When full, the least-recently-seen record is replaced.
 * A write is skipped unless SPIFFS has clear headroom for it.
 *
 * What is stored vs. derived: the file holds the observation facts (ICAO24,
 * call sign, craft/aircraft type, operator + source, first/last seen as UTC
 * epoch seconds, seen count). Registry note, registry prefix and "already
 * configured" are derived from the live registry/operator lists whenever they
 * are displayed or exported (SeenAircraft_Describe), so they can never go
 * stale when the user edits a rule. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aircraft_provider.h"
#include "craft_types.h"
#include "custom_rules.h"

/* Raised from 1000 during this session's capacity audit. This is a
 * concrete, derived safety limit, not a "7 days sounds right" guess:
 * - Routine (incremental) flush cost does NOT depend on this number - it
 *   scales with how many records changed in the last 10 minutes (typically
 *   10s), not with table size, since only dirty slots are ever rewritten.
 *   Confirmed on real hardware at 1000/1000: seek+write time is a function
 *   of dirty count, not total count (see PROJECT_STATE.md).
 * - RAM/PSRAM: 3000 records = ~252 KB, which crosses the 16 KB
 *   SPIRAM_MALLOC_ALWAYSINTERNAL threshold by over 15x, so it is PSRAM-
 *   resident (6 MB free observed) - not a meaningful internal-heap cost.
 *   recordDirty[] adds 3000 bytes of static internal RAM either way.
 * - The one cost that DOES scale with this number is the rare full rewrite
 *   (first save / CSV migration / recovery). Its own free-space guard
 *   requires roughly 2x the file size plus a 64 KB margin free before it
 *   will even attempt to run; at 3000 records that is about 557 KB, well
 *   inside the ~1.2 MB of SPIFFS free typically observed on this device
 *   (1.378 MB partition), with real headroom left over as other files grow.
 * This is NOT a validated "7 days of history" figure - no real multi-day
 * accumulation-rate measurement exists to size that precisely (see
 * PROJECT_STATE.md). It is a 3x increase, safely inside every measured
 * constraint above, that removes the pressure of a table sitting at
 * 937-1000/1000 in normal use; revisit with real week-long field data if
 * 7 days specifically still matters. */
#define SEEN_MAX_RECORDS 3000
#define SEEN_CSV_PATH "/spiffs/seen_aircraft.csv"       /* legacy format, read once for migration only */
#define SEEN_CSV_TMP_PATH "/spiffs/seen_aircraft.tmp"   /* legacy temp-swap recovery path, same purpose */
#define SEEN_BIN_PATH "/spiffs/seen_aircraft.dat"       /* current format: fixed-size header + slots */
#define SEEN_BIN_TMP_PATH "/spiffs/seen_aircraft.dat.tmp" /* used only for the rare full rewrite */
#define SEEN_FORMAT_VERSION 1 /* legacy CSV format tag; kept only as documentation of the migrated-from format */
#define SEEN_BIN_FORMAT_VERSION 1

/* Unsaved changes are written at most this often. Because only dirty records
 * are rewritten (not the whole table), the worst case is bounded by how many
 * distinct aircraft changed in the interval - typically tens, not thousands -
 * rather than a fixed multi-MB/day figure. A power loss can lose at most this
 * many minutes of sightings. */
#define SEEN_FLUSH_INTERVAL_SEC 600

/* Seen Count counts visits, not polls: an aircraft in range for an hour
 * polled every 30 s is one sighting, not 120. A returning aircraft is a new
 * visit once it has been absent at least this long. Set to 0 to count every
 * poll instead. */
#define SEEN_VISIT_GAP_SEC 1800

/* Refuse to write the CSV unless SPIFFS has at least the file's worst-case
 * size (counted twice: the temp file coexists with the old one) plus this
 * margin free. */
#define SEEN_MIN_FREE_MARGIN_BYTES (64 * 1024)

#define SEEN_ICAO_MAX 9      /* up to 8 characters + NUL ("~" TIS-B addresses allowed) */
#define SEEN_CALLSIGN_MAX 9  /* up to 8 characters + NUL */

typedef enum {
    SEEN_OPSRC_NONE = 0,
    SEEN_OPSRC_PROVIDER,   /* operator name supplied by the aircraft provider */
    SEEN_OPSRC_CONFIGURED, /* name from the user's/built-in operator list */
    SEEN_OPSRC_COUNT
} SeenOperatorSource;

const char *SeenOperatorSource_Name(SeenOperatorSource s);    /* "Provider" / "Configured" / "" */
const char *SeenOperatorSource_CsvName(SeenOperatorSource s); /* "PROVIDER" / "CONFIGURED" / "NONE" */

typedef struct {
    char icao24[SEEN_ICAO_MAX];
    char callsign[SEEN_CALLSIGN_MAX];
    char operatorName[AIRCRAFT_OPERATOR_NAME_MAX];
    CraftType craftType;
    AircraftType aircraftType;
    SeenOperatorSource operatorSource;
    uint32_t firstSeen; /* UTC epoch seconds; 0 = clock was not synchronized */
    uint32_t lastSeen;  /* UTC epoch seconds; 0 = clock was not synchronized */
    uint32_t seenCount;
} SeenRecord;

/* One observation, already resolved by the caller. */
typedef struct {
    char icao24[SEEN_ICAO_MAX];
    char callsign[SEEN_CALLSIGN_MAX];
    char operatorName[AIRCRAFT_OPERATOR_NAME_MAX]; /* empty = no new operator information */
    SeenOperatorSource operatorSource;
    CraftType craftType;
    AircraftType aircraftType;
} SeenObservation;

/* Registry/operator status, derived live (never stored). */
typedef struct {
    bool configured; /* a registry rule OR an operator entry covers this aircraft */
    CraftSource source;
    char registryPrefix[MAX_RULE_PREFIX + 1]; /* set when source == CRAFT_SRC_REGISTRY */
    char registryNote[MAX_RULE_NOTES + 1];
    char operatorCode[MAX_OPERATOR_CODE + 1]; /* ICAO code from the call sign, if it matches an entry */
    char configuredOperator[MAX_OPERATOR_NAME + 1];
} SeenConfigInfo;

typedef enum {
    SEEN_SORT_LAST_SEEN = 0,
    SEEN_SORT_FIRST_SEEN,
    SEEN_SORT_COUNT,
    SEEN_SORT_CALLSIGN,
    SEEN_SORT_ICAO24,
    SEEN_SORT_OPERATOR,
    SEEN_SORT_KEY_COUNT
} SeenSortKey;

typedef enum {
    SEEN_FILTER_ALL = 0,
    SEEN_FILTER_CONFIGURED,
    SEEN_FILTER_NOT_CONFIGURED,
    SEEN_FILTER_COUNT
} SeenFilter;

typedef struct {
    const char *search; /* case-insensitive substring; NULL/empty = no search */
    SeenFilter filter;
    SeenSortKey sort;
    bool descending;
} SeenQuery;

/* Call after CustomRules_Init() (which mounts SPIFFS). Loads the CSV if any. */
bool SeenAircraft_Init(void);
/* Same with explicit paths; used by the host tests. */
bool SeenAircraft_InitWithPaths(const char *csvPath, const char *tmpPath);
/* Frees the table and forgets all state (host tests only). */
void SeenAircraft_Deinit(void);

/* Record one observation. nowUtc <= TIMEUTIL_MIN_VALID_UTC means "clock not
 * synchronized": the aircraft is still counted, but no timestamp is invented. */
void SeenAircraft_Observe(const SeenObservation *obs, int64_t nowUtc);

/* Resolves every valid aircraft in list[] through the normal resolution path
 * and observes it. */
void SeenAircraft_ObservePoll(const Aircraft *list, int count, int64_t nowUtc);

/* Cheap; call often (every poll-loop slice). nowMonotonicSec is any
 * monotonically increasing seconds counter (uptime), not the wall clock.
 * Writes the CSV only when there are unsaved changes and the flush interval
 * has elapsed since they first appeared. Returns true if it wrote the file. */
bool SeenAircraft_FlushIfDue(uint32_t nowMonotonicSec);
/* Unconditional write (Clear uses this). */
bool SeenAircraft_Flush(void);

size_t SeenAircraft_Count(void);
bool SeenAircraft_IsDirty(void);
/* Copies record `index` (storage order) under the lock. */
bool SeenAircraft_Get(size_t index, SeenRecord *out);
bool SeenAircraft_Find(const char *icao24, SeenRecord *out);
/* Removes every record and the CSV file. */
void SeenAircraft_Clear(void);

/* Filters, sorts and pages a snapshot. Returns the number of records copied
 * into out[] (at most limit); *totalMatches gets the match count before
 * paging. */
size_t SeenAircraft_Query(const SeenQuery *query, size_t offset, size_t limit,
                          SeenRecord *out, size_t *totalMatches);

void SeenAircraft_Describe(const SeenRecord *record, SeenConfigInfo *out);

bool SeenAircraft_NormalizeIcao(const char *input, char out[SEEN_ICAO_MAX]);

/* ---- Phase 2 audit instrumentation: lightweight persistence diagnostics ----
 *
 * Cheap counters updated in the one place the CSV is actually written
 * (WriteCsvLocked). No background task, no logging framework - just a
 * handful of statics read back on demand (see web_diag.c). Added to trace
 * the ~10-minute flush's cost against the observed display flicker; see
 * PROJECT_STATE.md "Persistence timing diagnostics" for how to read these. */
typedef struct {
    uint32_t flushCount;                 /* successful writes since boot */
    uint32_t lastDurationMs;             /* most recent write's wall time */
    uint32_t worstDurationMs;            /* worst write wall time since boot */
    int32_t  lastInternalHeapDeltaBytes; /* internal free before-write minus after-write (positive = consumed) */
    int32_t  worstInternalHeapDeltaBytes;/* largest such delta seen */
    int32_t  lastPsramDeltaBytes;        /* PSRAM free before-write minus after-write (positive = consumed) */
    uint32_t lastRecordCount;            /* records actually written last flush (dirty count, or all of them for a full rewrite) */
    uint32_t lastFormatUs;               /* total time spent formatting/copying record data (excludes I/O) */
    uint32_t lastWriteUs;                /* total time spent on the actual file I/O calls */
    uint32_t lastMinInternalHeapDuringBytes; /* smallest internal-heap-free sample taken mid-write (periodic, not exhaustive) */
    bool     lastWasFullRewrite;         /* true only for the rare full write (first-ever save or CSV migration) */
} SeenPersistStats;

void SeenAircraft_GetPersistStats(SeenPersistStats *out);
