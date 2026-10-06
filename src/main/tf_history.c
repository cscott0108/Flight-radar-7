#include "tf_history.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
static const char *TAG = "TfHistory";
#else
#include <time.h>
#endif

#ifndef TF_MOUNT_POINT
#define TF_MOUNT_POINT "/sdcard"
#endif
#define TF_HISTORY_DIR TF_MOUNT_POINT "/history"
/* Index files. Version 2 (current): indexv2.dat = 32-byte header + TF_INDEX_SLOTS
 * slots, self-describing and committed with a single header write. The legacy
 * 4096-slot index.dat (no header) is never modified or deleted: after the
 * one-time migration it is left in place, read-only, as a fallback/evidence. */
#define TF_INDEX_LEGACY_PATH TF_HISTORY_DIR "/index.dat"
#define TF_INDEX_PATH TF_HISTORY_DIR "/indexv2.dat"

/* Open-addressing hash index, fixed size, read/written a slot at a time -
 * never loaded into RAM as a whole (PROMPT.md section 22). 32768 slots (power
 * of two: the hash is FNV-1a masked) = 672 KB on the card, zero RAM. The load
 * limits protect probe length: WARN at 70%, and a hard cap at 90% above which
 * NEW aircraft are not written to History at all (nothing is appended to a
 * bucket, so nothing can become an unindexed orphan or be duplicated by a
 * retry). The table can therefore never reach 100%. */
#ifndef TF_INDEX_SLOTS /* host tests may override with a smaller power of two to exercise the caps quickly */
#define TF_INDEX_SLOTS 32768u
#endif
#define TF_INDEX_LEGACY_SLOTS 4096u
#define TF_INDEX_WARN_AT ((TF_INDEX_SLOTS * 70u) / 100u)
#define TF_INDEX_HARD_CAP ((TF_INDEX_SLOTS * 90u) / 100u)
_Static_assert((TF_INDEX_SLOTS & (TF_INDEX_SLOTS - 1)) == 0, "index slot count must be a power of two (hash mask)");

typedef struct __attribute__((packed)) {
    char icao24[TF_ICAO_MAX]; /* icao24[0] == 0 means empty slot */
    uint32_t bucketFingerprint;
    uint32_t recordOffset; /* byte offset within the bucket file, after its header */
    uint32_t crc32;        /* protects this index entry itself */
} TfIndexSlot;

typedef struct __attribute__((packed)) {
    char magic[4]; /* "FR7B" */
    uint16_t version;
    uint16_t reserved;
    uint32_t fingerprint;
    uint32_t recordCount;
    uint32_t headerCrc32;
} TfBucketHeader;

#define TF_BUCKET_MAGIC "FR7B"
#define TF_BUCKET_VERSION 1

/* On-disk record wrapper: the public TfHistoryRecord plus its own CRC, so a
 * from-scratch index rebuild (PROMPT.md section 24) can validate each
 * record directly against the bucket file alone, without depending on the
 * index that is being rebuilt. */
typedef struct __attribute__((packed)) {
    TfHistoryRecord pub;
    uint32_t crc32;
} TfOnDiskRecord;

static volatile bool s_available = false;
static TfHistoryStats s_stats;

static int64_t NowUs(void); /* defined in "small platform shims" below */

/* ---- diagnostics state (tf_history.h "TF diagnostics") ---- */
static TfInitInfo s_init;
static TfReinitTrace s_trace;
static TfSelfTestResult s_selftest;
static char s_rtErr[48];

/* A record whose index slot write failed (the only way a record can end up
 * unindexed now): remembered so the retry rewrites THAT record instead of
 * appending a duplicate. One entry; cleared on success and on re-init. */
static struct {
    bool valid;
    char icao24[TF_ICAO_MAX];
    uint32_t fp;
    uint32_t offset;
} s_pending;

#define TF_LOCK_FOREVER UINT32_MAX
#define TF_LOCK_DIAG_MS 3000u
#define TF_LOCK_BROWSE_MS 1000u /* /history: give up (page shows "busy") rather than queue behind radar writes */
#define TF_SELFTEST_PATH TF_HISTORY_DIR "/_SELFTST.TMP" /* 8.3 name: CONFIG_FATFS_LFN_NONE=y */

/* One recursive mutex serializes Lookup/Upsert/Rebuild against Init, the
 * diagnostic self-test and unmount/reinit. Static storage (no heap). It is
 * created by the first Lock()/Init(); Lookup/Upsert test s_available before
 * locking, and s_available can only become true after Init has run. */
#ifdef ESP_PLATFORM
static StaticSemaphore_t s_lockBuf;
static SemaphoreHandle_t s_lock = NULL;
static void LockInit(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lockBuf);
}
static bool Lock(uint32_t ms)
{
    LockInit();
    if (!s_lock)
        return false;
    return xSemaphoreTakeRecursive(s_lock, ms == TF_LOCK_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(ms)) == pdTRUE;
}
static void Unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}
#else
static void LockInit(void) {}
static bool Lock(uint32_t ms)
{
    (void)ms;
    return true;
}
static void Unlock(void) {}
#endif

static void SetAvailable(bool v)
{
    s_available = v;
    s_stats.historyAvailable = v;
}

static const char *EspName(int e)
{
#ifdef ESP_PLATFORM
    return esp_err_to_name((esp_err_t)e);
#else
    return e == 0 ? "OK" : "err";
#endif
}

static const char *Hint(int stage, int espErr, int err)
{
#ifdef ESP_PLATFORM
    if (stage == TF_STAGE_SPI_BUS)
        return "SPI bus init failed (pin/host conflict or no internal DMA memory)";
    if (stage == TF_STAGE_MOUNT) {
        switch (espErr) {
        case ESP_ERR_NO_MEM: return "out of memory during mount (internal heap/DMA)";
        case ESP_ERR_TIMEOUT:
        case ESP_ERR_INVALID_RESPONSE:
        case ESP_ERR_INVALID_CRC: return "card not responding correctly (seating, pins, CS, card)";
        case ESP_FAIL: return "card answered but FAT mount failed (not FAT-formatted or corrupt); this firmware never formats";
        case ESP_ERR_INVALID_STATE: return "already mounted or SPI slot in use";
        default: return "see esp_err";
        }
    }
#else
    (void)stage;
    (void)espErr;
#endif
    if (stage == TF_STAGE_INDEX_WRITE || stage == TF_STAGE_INDEX_COMMIT)
        return "card mounts and reads but writing the new index failed: card may be read-only or defective "
               "(a dead card once reported errno=ENOENT here); run the TF self-test";
    switch (err) {
    case 0: return "no errno captured";
    case ENOENT: return "file or path not found";
    case ENOSPC: return "card full";
    case EIO: return "low-level I/O error";
    case EROFS:
    case EACCES: return "read-only or protected";
    case EMFILE:
    case ENFILE:
    case ENOMEM: return "out of file handles or memory";
    case ENAMETOOLONG:
    case EINVAL: return "invalid or non-8.3 file name";
    default: return "see errno";
    }
}

static void NoteLastError(int stage, int espErr, int err, int fr)
{
    s_init.lastEspErr = espErr;
    s_init.lastErrno = err;
    s_init.lastErrStage = (int8_t)stage;
    snprintf(s_init.lastError, sizeof(s_init.lastError), "stage=%s esp_err=%s errno=%d(%s) fatfs=%d: %s",
             TfHistory_StageName(stage), EspName(espErr), err, err ? strerror(err) : "-", fr, Hint(stage, espErr, err));
}

static void FillStep(TfStepInfo *st, TfResult r, int espErr, int err, int fr, int64_t t0)
{
    st->result = (uint8_t)r;
    st->espErr = espErr;
    st->errnoVal = (int16_t)err;
    st->fatfsErr = (uint8_t)fr;
    st->us = t0 ? (uint32_t)(NowUs() - t0) : 0;
}

static void StageSet(TfInitStage stage, TfResult r, int espErr, int err, int fr, int64_t t0)
{
    FillStep(&s_init.stage[stage], r, espErr, err, fr, t0);
    if (r == TF_RES_FAIL) {
        if (s_init.firstFailedStage < 0)
            s_init.firstFailedStage = (int8_t)stage;
        NoteLastError(stage, espErr, err, fr);
    }
}

static void NoteRuntimeFail(const char *op)
{
    snprintf(s_rtErr, sizeof(s_rtErr), "%s errno=%d", op, errno);
}

const char *TfHistory_StageName(int stage)
{
    static const char *const n[TF_STAGE_COUNT] = {"spi_bus_init", "sd_mount", "fs_info", "history_dir",
                                                   "index_check", "index_scan", "index_write", "index_verify",
                                                   "index_commit", "index_count"};
    return (stage >= 0 && stage < TF_STAGE_COUNT) ? n[stage] : "?";
}

const char *TfHistory_TraceStepName(int step)
{
    static const char *const n[TF_TR_COUNT] = {"unmount requested", "filesystem unmount", "SPI bus freed", "reinit requested",
                                                "SPI bus init", "SD mount", "history/index init"};
    return (step >= 0 && step < TF_TR_COUNT) ? n[step] : "?";
}

const char *TfHistory_SelfTestStepName(int step)
{
    static const char *const n[TF_ST_COUNT] = {"lock", "card mounted", "history dir", "create scratch", "write 64 B", "flush+fsync",
                                                "close", "reopen", "read 64 B", "verify", "close (read)", "delete scratch"};
    return (step >= 0 && step < TF_ST_COUNT) ? n[step] : "?";
}

/* ---- small platform shims ---- */

static int64_t NowUs(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

static void UpdateTiming(uint32_t *last, uint32_t *best, uint32_t *worst, uint32_t us)
{
    *last = us;
    if (*best == 0 || us < *best)
        *best = us;
    if (us > *worst)
        *worst = us;
}

/* ---- FNV-1a 32-bit and CRC-32 ----
 *
 * Duplicated (not shared) from universal_value.c on purpose: this file must
 * also build/host-test independently of that module, and both copies are
 * a few lines of stable, unchanging math. See universal_value.c's own note. */
static uint32_t Fnv1a32(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        h ^= (uint32_t)(*p);
        h *= 0x01000193u;
    }
    return h;
}

static uint32_t Crc32(const void *data, size_t len)
{
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ---- platform mount ---- */

#ifdef ESP_PLATFORM

/* TF/microSD wiring for the CrowPanel 7.0" Basic's on-board TF slot.
 * These SPI pins follow the commonly published pinout for this board
 * family; they have not been independently re-verified against a
 * schematic in this session. Override at build time if the real board
 * differs - a wrong pin here simply fails to mount (handled below as a
 * normal degraded-mode failure), it never blocks the radar. */
#ifndef TF_PIN_MOSI
#define TF_PIN_MOSI 11
#endif
#ifndef TF_PIN_MISO
#define TF_PIN_MISO 13
#endif
#ifndef TF_PIN_SCLK
#define TF_PIN_SCLK 12
#endif
#ifndef TF_PIN_CS
#define TF_PIN_CS 10
#endif
#ifndef TF_SPI_HOST
#define TF_SPI_HOST SPI2_HOST
#endif

static sdmmc_card_t *s_card = NULL;

static bool MountTfCard(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, /* never silently erase an existing card */
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = TF_SPI_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = TF_PIN_MOSI,
        .miso_io_num = TF_PIN_MISO,
        .sclk_io_num = TF_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* Our records are 52 bytes and every read/write is a single
         * in-place or appended record - never a multi-sector transfer - so
         * one FAT sector (512 bytes) is all this bus ever needs. This
         * matters beyond throughput: the SPI/DMA driver reserves an
         * internal (non-PSRAM) DMA-capable bounce buffer sized to
         * max_transfer_sz for the life of the app, since PSRAM can't be
         * DMA'd to directly on this chip. An earlier, needlessly generous
         * value here (4000 bytes) measurably ate into the largest
         * contiguous internal block available to mbedtls's hardware-AES
         * path during a later TLS handshake ("esp-aes: Failed to allocate
         * memory") - the same class of internal-RAM pressure as the
         * airports.c fix and the DRAM-overflow fix above, just showing up
         * at runtime instead of link time. */
        .max_transfer_sz = 512,
    };
    int64_t t0 = NowUs();
    errno = 0;
    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    int e = errno;
    if (ret == ESP_OK) {
        StageSet(TF_STAGE_SPI_BUS, TF_RES_OK, ret, 0, 0, t0);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        s_init.busAlreadyInit = true; /* tolerated, exactly as before */
        StageSet(TF_STAGE_SPI_BUS, TF_RES_OK, ret, 0, 0, t0);
    } else {
        StageSet(TF_STAGE_SPI_BUS, TF_RES_FAIL, ret, e, 0, t0);
        ESP_LOGW(TAG, "TF SPI bus init failed (%s) - persistent history disabled, radar unaffected", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = TF_PIN_CS;
    slot_config.host_id = (spi_host_device_t)host.slot;

    /* Local out-pointer: s_card only ever holds a card that really mounted. */
    sdmmc_card_t *card = NULL;
    t0 = NowUs();
    errno = 0;
    ret = esp_vfs_fat_sdspi_mount(TF_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    e = errno;
    if (ret != ESP_OK) {
        StageSet(TF_STAGE_MOUNT, TF_RES_FAIL, ret, e, 0, t0);
        ESP_LOGW(TAG, "TF card mount failed (%s) - persistent history disabled, radar unaffected", esp_err_to_name(ret));
        return false;
    }
    s_card = card;
    StageSet(TF_STAGE_MOUNT, TF_RES_OK, ret, 0, 0, t0);
    return true;
}

static void GetSpace(uint64_t *capacity, uint64_t *used, uint64_t *free_)
{
    FATFS *fs = NULL;
    DWORD freeClusters = 0;
    *capacity = *used = *free_ = 0;
    if (!s_card)
        return;
    int64_t t0 = NowUs();
    FRESULT fr = f_getfree("0:", &freeClusters, &fs);
    if (fr != FR_OK || !fs) {
        StageSet(TF_STAGE_FS_INFO, TF_RES_FAIL, 0, 0, (int)fr, t0);
        return;
    }
    uint64_t totalSectors = (uint64_t)(fs->n_fatent - 2) * fs->csize;
    uint64_t freeSectors = (uint64_t)freeClusters * fs->csize;
    uint64_t sectorSize = 512; /* FF_MIN_SS default */
    *capacity = totalSectors * sectorSize;
    *free_ = freeSectors * sectorSize;
    *used = *capacity - *free_;
    /* FATFS fs_type: 1 FAT12, 2 FAT16, 3 FAT32, 4 exFAT (literal values: ff.h macros differ across FatFs revisions) */
    s_init.fsType = (fs->fs_type >= 1 && fs->fs_type <= 4) ? (uint8_t)fs->fs_type : 0;
    StageSet(TF_STAGE_FS_INFO, TF_RES_OK, 0, 0, 0, t0);
}
#else
static bool MountTfCard(void)
{
    /* Host build: exercise the storage/index/bucket logic against a plain
     * local directory instead of a real card. */
    StageSet(TF_STAGE_SPI_BUS, TF_RES_SKIPPED, 0, 0, 0, 0);
    StageSet(TF_STAGE_MOUNT, TF_RES_SKIPPED, 0, 0, 0, 0);
    return true;
}
static void GetSpace(uint64_t *capacity, uint64_t *used, uint64_t *free_)
{
    *capacity = *used = *free_ = 0;
    StageSet(TF_STAGE_FS_INFO, TF_RES_SKIPPED, 0, 0, 0, 0);
}
#endif

/* ---- path helpers ---- */

static void BucketPath(uint32_t fp, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%08" PRIX32 ".dat", TF_HISTORY_DIR, fp);
}

static void NormalizeIcao(const char *in, char out[TF_ICAO_MAX])
{
    size_t n = 0;
    for (const char *p = in ? in : ""; *p && n + 1 < TF_ICAO_MAX; p++)
        out[n++] = (char)*p;
    out[n] = '\0';
}

/* ---- index (v2) ---- */

#define TF_INDEX_MAGIC "FR7I"
#define TF_INDEX_VERSION 2u
#define TF_IDX_STATE_BUILDING 0x444C5542u  /* "BULD": being built, NOT usable */
#define TF_IDX_STATE_COMMITTED 0x544D4F43u /* "COMT": verified and committed */

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t slotSize;
    uint32_t slotCount;
    uint32_t state;
    uint32_t reserved[3];
    uint32_t headerCrc32; /* over the 28 bytes above */
} TfIndexHeader;
_Static_assert(sizeof(TfIndexHeader) == 32, "index header must be 32 bytes");
#define TF_INDEX_FILE_BYTES (sizeof(TfIndexHeader) + (size_t)TF_INDEX_SLOTS * sizeof(TfIndexSlot))

static uint32_t IndexSlotCrc(const TfIndexSlot *s)
{
    /* CRC over everything except the crc field itself. */
    return Crc32(s, offsetof(TfIndexSlot, crc32));
}

static void IndexHeaderMake(TfIndexHeader *h, uint32_t state)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, TF_INDEX_MAGIC, 4);
    h->version = TF_INDEX_VERSION;
    h->slotSize = (uint16_t)sizeof(TfIndexSlot);
    h->slotCount = TF_INDEX_SLOTS;
    h->state = state;
    h->headerCrc32 = Crc32(h, offsetof(TfIndexHeader, headerCrc32));
}

static bool IndexHeaderValid(const TfIndexHeader *h, bool requireCommitted)
{
    if (memcmp(h->magic, TF_INDEX_MAGIC, 4) != 0 || h->version != TF_INDEX_VERSION ||
        h->slotSize != sizeof(TfIndexSlot) || h->slotCount != TF_INDEX_SLOTS)
        return false;
    if (h->headerCrc32 != Crc32(h, offsetof(TfIndexHeader, headerCrc32)))
        return false;
    return requireCommitted ? h->state == TF_IDX_STATE_COMMITTED : true;
}

static void IdxMakeSlot(TfIndexSlot *s, const char *icao24, uint32_t fp, uint32_t off)
{
    memset(s, 0, sizeof(*s));
    NormalizeIcao(icao24, s->icao24);
    s->bucketFingerprint = fp;
    s->recordOffset = off;
    s->crc32 = IndexSlotCrc(s);
}

/* One index handle for the duration of ONE locked operation: opened once,
 * used for every probe of that operation, closed before any other TF file
 * is opened (so there is never more than one open file, same peak as the old
 * per-slot open/close). Never kept across operations. */
typedef struct {
    FILE *f;
} TfIdx;

static bool IdxOpen(TfIdx *h, bool rw)
{
    h->f = fopen(TF_INDEX_PATH, rw ? "r+b" : "rb");
    return h->f != NULL;
}

static void IdxClose(TfIdx *h)
{
    if (h->f) {
        fclose(h->f);
        h->f = NULL;
    }
}

static long IdxSlotOffset(uint32_t idx)
{
    return (long)(sizeof(TfIndexHeader) + (size_t)idx * sizeof(TfIndexSlot));
}

static bool IdxReadSlot(TfIdx *h, uint32_t idx, TfIndexSlot *out)
{
    return fseek(h->f, IdxSlotOffset(idx), SEEK_SET) == 0 && fread(out, sizeof(*out), 1, h->f) == 1;
}

#ifndef ESP_PLATFORM
static int s_hostFailSlotWrites = 0;
void TfHistory_HostTestFailSlotWrites(int n) { s_hostFailSlotWrites = n; }
#endif

static bool IdxWriteSlot(TfIdx *h, uint32_t idx, const TfIndexSlot *in)
{
#ifndef ESP_PLATFORM
    if (s_hostFailSlotWrites > 0) {
        s_hostFailSlotWrites--;
        return false;
    }
#endif
    return fseek(h->f, IdxSlotOffset(idx), SEEK_SET) == 0 && fwrite(in, sizeof(*in), 1, h->f) == 1 &&
           fflush(h->f) == 0;
}

typedef enum { IDXP_FOUND = 0, IDXP_ABSENT, IDXP_ERR } IdxProbeStatus;

typedef struct {
    uint32_t idx;     /* FOUND: slot index of the entry */
    uint32_t freeIdx; /* ABSENT: where a new entry for this ICAO24 belongs (first corrupt-or-empty slot of its chain) */
    uint32_t probes;  /* slots read */
    TfIndexSlot slot; /* FOUND: the entry */
} IdxProbeResult;

/* Linear probe from the natural hash slot (same rules as before): the first
 * empty slot ends the chain (no tombstones); a slot with a bad crc is skipped
 * when searching, and is the preferred insertion point if the ICAO24 is not
 * found (self-healing). Bounded by TF_INDEX_SLOTS. One pass therefore answers
 * both "is it indexed?" and "which slot would a new entry take?". */
static IdxProbeStatus IdxFindOrSlot(TfIdx *h, const char *icao24, IdxProbeResult *r)
{
    uint32_t start = Fnv1a32(icao24) & (TF_INDEX_SLOTS - 1);
    bool haveCorrupt = false;
    uint32_t firstCorrupt = 0;
    r->probes = 0;
    for (uint32_t i = 0; i < TF_INDEX_SLOTS; i++) {
        uint32_t idx = (start + i) & (TF_INDEX_SLOTS - 1);
        TfIndexSlot slot;
        if (!IdxReadSlot(h, idx, &slot))
            return IDXP_ERR;
        r->probes++;
        if (slot.icao24[0] == '\0') {
            r->freeIdx = haveCorrupt ? firstCorrupt : idx;
            return IDXP_ABSENT;
        }
        if (slot.crc32 != IndexSlotCrc(&slot)) {
            if (!haveCorrupt) {
                haveCorrupt = true;
                firstCorrupt = idx;
            }
            continue;
        }
        if (strncmp(slot.icao24, icao24, TF_ICAO_MAX) == 0) {
            r->idx = idx;
            r->slot = slot;
            return IDXP_FOUND;
        }
    }
    if (haveCorrupt) {
        r->freeIdx = firstCorrupt;
        return IDXP_ABSENT;
    }
    return IDXP_ERR; /* table completely full of other aircraft: cannot happen below the hard cap */
}

static uint8_t IndexLoadFor(uint32_t used)
{
    if (used >= TF_INDEX_HARD_CAP)
        return TF_LOAD_CAP;
    if (used >= TF_INDEX_WARN_AT)
        return TF_LOAD_WARN;
    return TF_LOAD_OK;
}

static void IndexRefreshLoad(void)
{
    s_stats.indexLoadState = IndexLoadFor(s_stats.indexSlotsUsed);
}

/* Probe statistics for the radar write/lookup path only. */
static void RecordProbe(bool hit, uint32_t probes)
{
    if (hit) {
        s_stats.probeHitOps++;
        s_stats.probeHitTotal += probes;
    } else {
        s_stats.probeMissOps++;
        s_stats.probeMissTotal += probes;
    }
    s_stats.probeLast = probes;
    if (probes > s_stats.probeMax)
        s_stats.probeMax = probes;
    int bin = probes <= 1 ? 0 : probes <= 3 ? 1 : probes <= 7 ? 2 : probes <= 15 ? 3 : probes <= 31 ? 4 : 5;
    s_stats.probeHist[bin]++;
}

/* Counts occupied, CRC-valid slots of the committed index with one
 * sequential pass in small chunks (never loads the whole table). Read-only. */
static bool CountIndexSlots(uint32_t *usedOut, int *errnoOut)
{
    errno = 0;
    FILE *f = fopen(TF_INDEX_PATH, "rb");
    if (!f) {
        *errnoOut = errno;
        return false;
    }
    TfIndexSlot chunk[16];
    uint32_t used = 0, total = 0;
    bool ok = fseek(f, (long)sizeof(TfIndexHeader), SEEK_SET) == 0;
    while (ok && total < TF_INDEX_SLOTS) {
        size_t got = fread(chunk, sizeof(TfIndexSlot), 16, f);
        if (got != 16) {
            ok = false;
            break;
        }
        for (size_t i = 0; i < got; i++)
            if (chunk[i].icao24[0] != '\0' && chunk[i].crc32 == IndexSlotCrc(&chunk[i]))
                used++;
        total += (uint32_t)got;
    }
    *errnoOut = ok ? 0 : (errno ? errno : EIO);
    fclose(f);
    *usedOut = used;
    return ok;
}

/* ---- buckets ---- */

static bool BucketHeaderOk(const TfBucketHeader *h)
{
    return memcmp(h->magic, TF_BUCKET_MAGIC, 4) == 0 &&
           h->headerCrc32 == Crc32(h, offsetof(TfBucketHeader, headerCrc32));
}

static bool ReadBucketHeader(const char *path, TfBucketHeader *out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    bool ok = fread(out, sizeof(*out), 1, f) == 1;
    fclose(f);
    if (!ok)
        return false;
    return BucketHeaderOk(out);
}

static bool WriteBucketHeader(const char *path, TfBucketHeader *hdr)
{
    hdr->headerCrc32 = Crc32(hdr, offsetof(TfBucketHeader, headerCrc32));
    FILE *f = fopen(path, "r+b");
    if (!f)
        return false;
    bool ok = fwrite(hdr, sizeof(*hdr), 1, f) == 1;
    fflush(f);
    fclose(f);
    return ok;
}

static bool EnsureBucket(uint32_t fp, char pathOut[512], TfBucketHeader *hdrOut)
{
    BucketPath(fp, pathOut, 512);
    if (ReadBucketHeader(pathOut, hdrOut))
        return true;

    /* Missing header, or header that fails its CRC. If the file already holds
     * bytes beyond a header, REPAIR the header in place from the file size
     * (record count = whole records present) instead of truncating the file:
     * the index rebuild may have indexed those records (each still passes its
     * own CRC when read), and truncating would destroy them. Only a missing
     * file, or one shorter than a header (nothing to lose), is created fresh. */
    long size = -1;
    FILE *probe = fopen(pathOut, "rb");
    if (probe) {
        if (fseek(probe, 0, SEEK_END) == 0)
            size = ftell(probe);
        fclose(probe);
    }
    uint32_t recovered = 0;
    const bool repair = size >= (long)sizeof(TfBucketHeader);
    if (repair)
        recovered = (uint32_t)(((size_t)size - sizeof(TfBucketHeader)) / sizeof(TfOnDiskRecord));

    memset(hdrOut, 0, sizeof(*hdrOut));
    memcpy(hdrOut->magic, TF_BUCKET_MAGIC, 4);
    hdrOut->version = TF_BUCKET_VERSION;
    hdrOut->fingerprint = fp;
    hdrOut->recordCount = recovered;
    hdrOut->headerCrc32 = Crc32(hdrOut, offsetof(TfBucketHeader, headerCrc32));
    FILE *f = fopen(pathOut, repair ? "r+b" : "wb");
    if (!f)
        return false;
    bool ok = fseek(f, 0, SEEK_SET) == 0 && fwrite(hdrOut, sizeof(*hdrOut), 1, f) == 1 && fflush(f) == 0;
    fclose(f);
    return ok;
}

static bool ReadRecordAt(const char *path, uint32_t offset, TfOnDiskRecord *out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    bool ok = fseek(f, (long)(sizeof(TfBucketHeader) + offset), SEEK_SET) == 0 &&
              fread(out, sizeof(*out), 1, f) == 1;
    fclose(f);
    return ok;
}

static bool WriteRecordAt(const char *path, uint32_t offset, const TfOnDiskRecord *in)
{
    FILE *f = fopen(path, "r+b");
    if (!f)
        return false;
    bool ok = fseek(f, (long)(sizeof(TfBucketHeader) + offset), SEEK_SET) == 0 &&
              fwrite(in, sizeof(*in), 1, f) == 1;
    fflush(f);
    fclose(f);
    return ok;
}

static bool AppendRecord(const char *path, TfBucketHeader *hdr, const TfOnDiskRecord *in, uint32_t *offsetOut)
{
    uint32_t offset = hdr->recordCount * (uint32_t)sizeof(TfOnDiskRecord);
    if (!WriteRecordAt(path, offset, in))
        return false;
    hdr->recordCount++;
    if (!WriteBucketHeader(path, hdr))
        return false;
    if (offsetOut) *offsetOut = offset;
    return true;
}

/* ---- public API ---- */

/* =====================================================================
 * Index builder: first-boot migration from the legacy 4096-slot index.dat,
 * rebuild after a missing/unfinished/invalid index, or fresh create.
 *
 * Principles:
 *  - The index is DERIVED data; the bucket files are the source of truth.
 *    The builder only READS bucket files and the legacy index.dat.
 *  - The new index is built beside the legacy one (indexv2.dat), verified,
 *    and only then committed by one header write (+fsync). A power cut at any
 *    earlier point leaves state "building" (or a bad header CRC), which the
 *    next boot simply rebuilds. indexv2.dat is the builder's own file; it is
 *    the only file it ever truncates or rewrites.
 *  - EVERY bucket file in the history folder is scanned (not just the two
 *    special ones). Per ICAO24 the winner is the newest record: highest
 *    lastSeen, then highest seenCount, then highest (fingerprint, offset) -
 *    a total order, so the result does not depend on directory order.
 *  - Records must pass their own CRC (and have an ICAO24) to be indexed. A
 *    bucket with a bad header is still scanned (record count from the file
 *    size) but each record is individually validated.
 *  - Working memory: transient PSRAM staging of the whole table (28 B per
 *    slot, ~0.9 MB) so duplicates can be resolved without disk reads and the
 *    file is written sequentially. If that allocation fails the builder falls
 *    back to building directly on disk (slower, same result). Nothing here is
 *    retained after the build.
 *  - Stack use is deliberately tiny (app_main has 3584 B): all buffers live
 *    in the transient work block.
 * ===================================================================== */

typedef struct {
    uint32_t fp;
    uint32_t off;
    uint32_t lastSeen;
    uint32_t seenCount;
    char icao24[12]; /* icao24[0] == 0: empty */
} TfStageEntry;
_Static_assert(TF_ICAO_MAX <= 12, "staging entry too small for an ICAO24");

typedef struct {
    TfOnDiskRecord raw[8];  /* 416 B: one scan chunk */
    TfIndexSlot chunk[16];  /* 336 B: one file write/verify chunk */
    struct {
        char icao24[TF_ICAO_MAX];
        uint32_t fp;
        uint32_t off;
    } sample[16];
} TfBuildWork;

typedef struct {
    TfBuildWork *work;
    TfStageEntry *stage; /* NULL = on-disk fallback */
    TfIdx disk;          /* on-disk fallback: index handle held open for the scan */
    TfIndexBuildReport *rep;
    uint32_t used;
    uint32_t yieldCtr;
    bool ioError;
} TfBuildCtx;

static TfIndexBuildReport s_build;
#ifndef ESP_PLATFORM
static bool s_hostNoStaging = false;
static int s_hostAbortPhase = 0;
void TfHistory_HostTestNoStaging(bool force) { s_hostNoStaging = force; }
void TfHistory_HostTestAbortBuild(int phase) { s_hostAbortPhase = phase; }
#endif

void TfHistory_GetIndexBuildReport(TfIndexBuildReport *out)
{
    if (out)
        *out = s_build;
}

static void *BuildAlloc(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); /* CPU-only data: PSRAM, never internal/DMA RAM */
    return p;
#else
    return malloc(n);
#endif
}

static void BuildFree(void *p)
{
    free(p); /* heap_caps_free() == free() */
}

static void BuildYield(TfBuildCtx *c)
{
    /* The task watchdog watches the idle tasks: let them run now and then. */
    if (++c->yieldCtr >= 256) {
        c->yieldCtr = 0;
#ifdef ESP_PLATFORM
        vTaskDelay(1);
#endif
    }
}

/* a strictly newer than b? (total order) */
static bool BuildBetter(uint32_t aLast, uint32_t aSeen, uint32_t aFp, uint32_t aOff,
                        uint32_t bLast, uint32_t bSeen, uint32_t bFp, uint32_t bOff)
{
    if (aLast != bLast)
        return aLast > bLast;
    if (aSeen != bSeen)
        return aSeen > bSeen;
    if (aFp != bFp)
        return aFp > bFp;
    return aOff > bOff;
}

static void BuildInsert(TfBuildCtx *c, const TfOnDiskRecord *r, uint32_t fp, uint32_t off)
{
    char icao[TF_ICAO_MAX];
    NormalizeIcao(r->pub.icao24, icao);
    const uint32_t mask = TF_INDEX_SLOTS - 1;

    if (c->stage) {
        uint32_t i = Fnv1a32(icao) & mask;
        for (uint32_t n = 0; n < TF_INDEX_SLOTS; n++, i = (i + 1) & mask) {
            TfStageEntry *e = &c->stage[i];
            if (e->icao24[0] == '\0') {
                if (c->used >= TF_INDEX_HARD_CAP) {
                    c->rep->rejectedAtCap++; /* never fill the table past the hard cap */
                    return;
                }
                memset(e, 0, sizeof(*e));
                memcpy(e->icao24, icao, TF_ICAO_MAX);
                e->fp = fp;
                e->off = off;
                e->lastSeen = r->pub.lastSeen;
                e->seenCount = r->pub.seenCount;
                c->used++;
                return;
            }
            if (strncmp(e->icao24, icao, TF_ICAO_MAX) == 0) {
                c->rep->duplicatesResolved++;
                if (BuildBetter(r->pub.lastSeen, r->pub.seenCount, fp, off, e->lastSeen, e->seenCount, e->fp, e->off)) {
                    e->fp = fp;
                    e->off = off;
                    e->lastSeen = r->pub.lastSeen;
                    e->seenCount = r->pub.seenCount;
                }
                return;
            }
        }
        c->rep->rejectedAtCap++;
        return;
    }

    /* On-disk fallback: same rules, the current winner's lastSeen/seenCount
     * are read back from its bucket record (only needed on a duplicate). */
    IdxProbeResult pr;
    IdxProbeStatus st = IdxFindOrSlot(&c->disk, icao, &pr);
    if (st == IDXP_ABSENT) {
        if (c->used >= TF_INDEX_HARD_CAP) {
            c->rep->rejectedAtCap++;
            return;
        }
        TfIndexSlot slot;
        IdxMakeSlot(&slot, icao, fp, off);
        if (!IdxWriteSlot(&c->disk, pr.freeIdx, &slot))
            c->ioError = true;
        else
            c->used++;
    } else if (st == IDXP_FOUND) {
        c->rep->duplicatesResolved++;
        char path[96];
        TfOnDiskRecord cur;
        BucketPath(pr.slot.bucketFingerprint, path, sizeof(path));
        bool curOk = ReadRecordAt(path, pr.slot.recordOffset, &cur) && cur.crc32 == Crc32(&cur.pub, sizeof(cur.pub));
        if (!curOk || BuildBetter(r->pub.lastSeen, r->pub.seenCount, fp, off, cur.pub.lastSeen, cur.pub.seenCount,
                                  pr.slot.bucketFingerprint, pr.slot.recordOffset)) {
            TfIndexSlot slot;
            IdxMakeSlot(&slot, icao, fp, off);
            if (!IdxWriteSlot(&c->disk, pr.idx, &slot))
                c->ioError = true;
        }
    } else {
        c->ioError = true;
    }
}

static bool ParseBucketName(const char *name, uint32_t *fpOut);

static void BuildScanBucket(TfBuildCtx *c, uint32_t fp)
{
    char path[96];
    BucketPath(fp, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        c->rep->bucketsUnreadable++;
        return;
    }
    c->rep->buckets++;

    TfBucketHeader hdr;
    bool hdrOk = fread(&hdr, sizeof(hdr), 1, f) == 1 && BucketHeaderOk(&hdr);
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    uint32_t physical = size >= (long)sizeof(TfBucketHeader)
                            ? (uint32_t)(((size_t)size - sizeof(TfBucketHeader)) / sizeof(TfOnDiskRecord))
                            : 0;
    uint32_t count;
    if (hdrOk) {
        count = hdr.recordCount < physical ? hdr.recordCount : physical; /* never trust a count beyond the file */
    } else {
        count = physical; /* header unusable: count whole records present; each is still CRC-checked */
        c->rep->headerFallbackBuckets++;
    }
    if (fseek(f, (long)sizeof(TfBucketHeader), SEEK_SET) != 0) {
        c->rep->bucketsUnreadable++;
        fclose(f);
        return;
    }

    uint32_t idx = 0;
    while (idx < count) {
        size_t want = count - idx;
        if (want > 8)
            want = 8;
        size_t got = fread(c->work->raw, sizeof(TfOnDiskRecord), want, f);
        if (got == 0) {
            c->rep->bucketsUnreadable++; /* count said more records than could be read */
            break;
        }
        for (size_t i = 0; i < got; i++) {
            const TfOnDiskRecord *r = &c->work->raw[i];
            c->rep->recordsScanned++;
            if (r->pub.icao24[0] == '\0' || r->crc32 != Crc32(&r->pub, sizeof(r->pub))) {
                c->rep->recordsCorrupt++; /* unverifiable: never treated as valid History */
                continue;
            }
            BuildInsert(c, r, fp, (uint32_t)((idx + i) * sizeof(TfOnDiskRecord)));
        }
        idx += (uint32_t)got;
        BuildYield(c);
    }
    fclose(f);
}

/* One readdir pass over the history folder; every XXXXXXXX.DAT is a bucket. */
static bool BuildScanAll(TfBuildCtx *c)
{
    DIR *d = opendir(TF_HISTORY_DIR);
    if (!d)
        return false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t fp;
        if (ParseBucketName(e->d_name, &fp))
            BuildScanBucket(c, fp);
    }
    closedir(d);
    return true;
}

static void BuildFail(TfBuildCtx *c, TfInitStage stage, int err, const char *msg, int64_t t0)
{
    snprintf(c->rep->error, sizeof(c->rep->error), "%s", msg);
    StageSet(stage, TF_RES_FAIL, 0, err, 0, t0);
}

/* Creates indexv2.dat in state "building" with every slot empty. */
static bool BuildCreateEmpty(TfBuildCtx *c, int *err)
{
    errno = 0;
    FILE *f = fopen(TF_INDEX_PATH, "wb");
    if (!f) {
        *err = errno;
        return false;
    }
    TfIndexHeader h;
    IndexHeaderMake(&h, TF_IDX_STATE_BUILDING);
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
    memset(c->work->chunk, 0, sizeof(c->work->chunk));
    for (uint32_t base = 0; ok && base < TF_INDEX_SLOTS; base += 16) {
        ok = fwrite(c->work->chunk, sizeof(TfIndexSlot), 16, f) == 16;
        BuildYield(c);
    }
    if (ok)
        ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    *err = ok ? 0 : (errno ? errno : EIO);
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

/* Staged mode: write the whole table sequentially in chunks. */
static bool BuildWriteStaged(TfBuildCtx *c, int *err)
{
    errno = 0;
    FILE *f = fopen(TF_INDEX_PATH, "wb");
    if (!f) {
        *err = errno;
        return false;
    }
    TfIndexHeader h;
    IndexHeaderMake(&h, TF_IDX_STATE_BUILDING);
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
    for (uint32_t base = 0; ok && base < TF_INDEX_SLOTS; base += 16) {
        for (uint32_t k = 0; k < 16; k++) {
            const TfStageEntry *e = &c->stage[base + k];
            if (e->icao24[0] != '\0')
                IdxMakeSlot(&c->work->chunk[k], e->icao24, e->fp, e->off);
            else
                memset(&c->work->chunk[k], 0, sizeof(TfIndexSlot));
        }
        ok = fwrite(c->work->chunk, sizeof(TfIndexSlot), 16, f) == 16;
        BuildYield(c);
    }
    if (ok)
        ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    *err = ok ? 0 : (errno ? errno : EIO);
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

/* Report-only comparison of the legacy 4096-slot index with the new one. */
static void BuildLegacyCrossCheck(TfBuildCtx *c)
{
    TfIndexBuildReport *rep = c->rep;
    FILE *lf = fopen(TF_INDEX_LEGACY_PATH, "rb");
    if (!lf)
        return;
    TfIdx n;
    if (!IdxOpen(&n, false)) {
        fclose(lf);
        return;
    }
    rep->legacyChecked = true;
    for (uint32_t base = 0; base < TF_INDEX_LEGACY_SLOTS; base += 16) {
        if (fread(c->work->chunk, sizeof(TfIndexSlot), 16, lf) != 16)
            break;
        for (uint32_t k = 0; k < 16; k++) {
            const TfIndexSlot *ls = &c->work->chunk[k];
            if (ls->icao24[0] == '\0' || ls->crc32 != IndexSlotCrc(ls))
                continue;
            rep->legacyValid++;
            IdxProbeResult pr;
            char icao[TF_ICAO_MAX];
            NormalizeIcao(ls->icao24, icao);
            if (IdxFindOrSlot(&n, icao, &pr) != IDXP_FOUND)
                rep->legacyMissing++;
            else if (pr.slot.bucketFingerprint == ls->bucketFingerprint && pr.slot.recordOffset == ls->recordOffset)
                rep->legacySame++;
            else
                rep->legacyNewer++; /* the new index chose a different (newer-or-equal by the winner rule) record */
        }
        BuildYield(c);
    }
    IdxClose(&n);
    fclose(lf);
}

static bool BuildVerify(TfBuildCtx *c, int *err, const char **why)
{
    TfIndexBuildReport *rep = c->rep;
    errno = 0;
    FILE *f = fopen(TF_INDEX_PATH, "rb");
    if (!f) {
        *err = errno;
        *why = "cannot reopen the new index for verification";
        return false;
    }
    TfIndexHeader h;
    long size = -1;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && IndexHeaderValid(&h, false) && h.state == TF_IDX_STATE_BUILDING;
    if (ok && fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (!ok || size != (long)TF_INDEX_FILE_BYTES || fseek(f, (long)sizeof(h), SEEK_SET) != 0) {
        fclose(f);
        *why = "new index header or size is wrong";
        return false;
    }

    uint32_t valid = 0, mismatch = 0, nSamples = 0, occIdx = 0;
    const uint32_t step = c->used >= 16 ? c->used / 16 : 1;
    bool readOk = true;
    for (uint32_t base = 0; base < TF_INDEX_SLOTS; base += 16) {
        if (fread(c->work->chunk, sizeof(TfIndexSlot), 16, f) != 16) {
            readOk = false;
            break;
        }
        for (uint32_t k = 0; k < 16; k++) {
            const TfIndexSlot *sl = &c->work->chunk[k];
            const bool occ = sl->icao24[0] != '\0';
            const bool crcOk = sl->crc32 == IndexSlotCrc(sl);
            if (c->stage) {
                const TfStageEntry *e = &c->stage[base + k];
                if ((e->icao24[0] != '\0') != occ)
                    mismatch++;
                else if (occ && (!crcOk || strncmp(sl->icao24, e->icao24, TF_ICAO_MAX) != 0 ||
                                 sl->bucketFingerprint != e->fp || sl->recordOffset != e->off))
                    mismatch++;
                else if (occ)
                    valid++;
            } else if (occ) {
                if (!crcOk)
                    mismatch++;
                else
                    valid++;
            }
            if (occ && crcOk) {
                if (nSamples < 16 && (occIdx % step) == 0) { /* up to 16 evenly spaced entries */
                    memcpy(c->work->sample[nSamples].icao24, sl->icao24, TF_ICAO_MAX);
                    c->work->sample[nSamples].fp = sl->bucketFingerprint;
                    c->work->sample[nSamples].off = sl->recordOffset;
                    nSamples++;
                }
                occIdx++;
            }
        }
        BuildYield(c);
    }
    fclose(f);
    rep->verifyMismatch = mismatch;
    if (!readOk) {
        *err = EIO;
        *why = "new index could not be read back completely";
        return false;
    }
    if (mismatch != 0) {
        *why = "new index does not match what was written";
        return false;
    }
    if (valid != c->used) {
        *why = "new index slot count differs from the aircraft indexed";
        return false;
    }

    /* Spot-check that sampled slots really point at a matching, CRC-valid record. */
    for (uint32_t i = 0; i < nSamples; i++) {
        char path[96];
        TfOnDiskRecord r;
        BucketPath(c->work->sample[i].fp, path, sizeof(path));
        rep->verifySampled++;
        if (!ReadRecordAt(path, c->work->sample[i].off, &r) || r.crc32 != Crc32(&r.pub, sizeof(r.pub)) ||
            strncmp(r.pub.icao24, c->work->sample[i].icao24, TF_ICAO_MAX) != 0) {
            rep->verifyMismatch++;
            *why = "a sampled index entry does not point at its record";
            return false;
        }
    }
    return true;
}

/* The commit point: ONE 32-byte header write + fsync flips building -> committed. */
static bool BuildCommit(int *err, const char **why)
{
    errno = 0;
    FILE *f = fopen(TF_INDEX_PATH, "r+b");
    if (!f) {
        *err = errno;
        *why = "cannot reopen the new index to commit it";
        return false;
    }
    TfIndexHeader h;
    IndexHeaderMake(&h, TF_IDX_STATE_COMMITTED);
    bool ok = fseek(f, 0, SEEK_SET) == 0 && fwrite(&h, sizeof(h), 1, f) == 1 && fflush(f) == 0 &&
              fsync(fileno(f)) == 0;
    *err = ok ? 0 : (errno ? errno : EIO);
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        *why = "writing the commit header failed";
        return false;
    }
    f = fopen(TF_INDEX_PATH, "rb");
    TfIndexHeader back;
    ok = f && fread(&back, sizeof(back), 1, f) == 1 && IndexHeaderValid(&back, true);
    if (f)
        fclose(f);
    if (!ok)
        *why = "the committed header did not read back";
    return ok;
}

static bool BuildIndexV2(bool legacyUsable)
{
    TfIndexBuildReport *rep = &s_build;
    memset(rep, 0, sizeof(*rep));
    rep->ran = true;
    rep->mode = legacyUsable ? TF_BUILD_MIGRATE : TF_BUILD_REBUILD;

    const int64_t tAll = NowUs();
    int64_t t0;
    int err = 0;
    const char *why = "";
    bool ok = false;

    TfBuildCtx c;
    memset(&c, 0, sizeof(c));
    c.rep = rep;
    c.work = BuildAlloc(sizeof(TfBuildWork));
    if (!c.work)
        c.work = malloc(sizeof(TfBuildWork));
    if (!c.work) {
        BuildFail(&c, TF_STAGE_INDEX_SCAN, ENOMEM, "no memory for the index build work block", tAll);
        goto out;
    }
    const size_t stageBytes = sizeof(TfStageEntry) * TF_INDEX_SLOTS;
#ifndef ESP_PLATFORM
    if (!s_hostNoStaging)
#endif
        c.stage = BuildAlloc(stageBytes);
    if (c.stage) {
        memset(c.stage, 0, stageBytes);
        rep->usedStaging = true;
        rep->stagingBytes = (uint32_t)stageBytes;
    }

    /* ---- scan every bucket ---- */
    t0 = NowUs();
    if (!c.stage) {
        if (!BuildCreateEmpty(&c, &err) || !IdxOpen(&c.disk, true)) {
            BuildFail(&c, TF_STAGE_INDEX_SCAN, err ? err : errno, "cannot create the new index file (disk fallback)", t0);
            goto out;
        }
    }
    bool scanned = BuildScanAll(&c);
    if (!c.stage) {
        if (c.disk.f && (fflush(c.disk.f) != 0 || fsync(fileno(c.disk.f)) != 0))
            c.ioError = true;
        IdxClose(&c.disk);
    }
    rep->scanUs = (uint32_t)(NowUs() - t0);
    if (!scanned) {
        BuildFail(&c, TF_STAGE_INDEX_SCAN, errno, "history folder could not be listed", t0);
        goto out;
    }
    if (rep->bucketsUnreadable > 0) {
        /* An incomplete scan would silently drop aircraft from the index: refuse to commit it. */
        BuildFail(&c, TF_STAGE_INDEX_SCAN, EIO, "some bucket files could not be read; index NOT committed (data untouched)", t0);
        goto out;
    }
    if (c.ioError) {
        BuildFail(&c, TF_STAGE_INDEX_SCAN, errno ? errno : EIO, "index slot I/O failed while building", t0);
        goto out;
    }
    StageSet(TF_STAGE_INDEX_SCAN, TF_RES_OK, 0, 0, 0, t0);
    if (!legacyUsable)
        rep->mode = rep->buckets > 0 ? TF_BUILD_REBUILD : TF_BUILD_FRESH;
    rep->aircraftIndexed = c.used;

    /* ---- write ---- */
    t0 = NowUs();
    if (c.stage && !BuildWriteStaged(&c, &err)) {
        rep->writeUs = (uint32_t)(NowUs() - t0);
        BuildFail(&c, TF_STAGE_INDEX_WRITE, err, "writing the new index failed (card full, read-only or removed?)", t0);
        goto out;
    }
    rep->writeUs = (uint32_t)(NowUs() - t0);
    StageSet(TF_STAGE_INDEX_WRITE, TF_RES_OK, 0, 0, 0, t0);
#ifndef ESP_PLATFORM
    if (s_hostAbortPhase == 1) {
        snprintf(rep->error, sizeof(rep->error), "host test: simulated power loss after write");
        goto out;
    }
#endif

    /* ---- verify (and report-only comparison with the legacy index) ---- */
    t0 = NowUs();
    if (!BuildVerify(&c, &err, &why)) {
        rep->verifyUs = (uint32_t)(NowUs() - t0);
        BuildFail(&c, TF_STAGE_INDEX_VERIFY, err, why, t0);
        goto out;
    }
    if (legacyUsable)
        BuildLegacyCrossCheck(&c);
    rep->verifyUs = (uint32_t)(NowUs() - t0);
    StageSet(TF_STAGE_INDEX_VERIFY, TF_RES_OK, 0, 0, 0, t0);
#ifndef ESP_PLATFORM
    if (s_hostAbortPhase == 2) {
        snprintf(rep->error, sizeof(rep->error), "host test: simulated power loss before commit");
        goto out;
    }
#endif

    /* ---- commit ---- */
    t0 = NowUs();
    if (!BuildCommit(&err, &why)) {
        rep->commitUs = (uint32_t)(NowUs() - t0);
        BuildFail(&c, TF_STAGE_INDEX_COMMIT, err, why, t0);
        goto out;
    }
    rep->commitUs = (uint32_t)(NowUs() - t0);
    StageSet(TF_STAGE_INDEX_COMMIT, TF_RES_OK, 0, 0, 0, t0);

    s_stats.indexSlotsUsed = c.used;
    IndexRefreshLoad();
    s_stats.indexRebuilds++;
    ok = true;

out:
    if (c.disk.f)
        IdxClose(&c.disk);
    if (c.stage)
        BuildFree(c.stage);
    if (c.work)
        BuildFree(c.work);
    rep->ok = ok;
    rep->totalUs = (uint32_t)(NowUs() - tAll);
    if (ok) {
        UpdateTiming(&s_stats.lastIndexOpUs, &s_stats.bestIndexOpUs, &s_stats.worstIndexOpUs, rep->totalUs);
        UpdateTiming(&s_stats.lastRecoveryUs, &s_stats.bestRecoveryUs, &s_stats.worstRecoveryUs, rep->totalUs);
    }
    return ok;
}

/* The existing init sequence, unchanged in behavior, with every stage
 * recorded. Caller holds the lock. resetStats=false (diagnostic reinit)
 * keeps the lifetime counters. */
static bool InitInternal(bool resetStats)
{
    if (resetStats)
        memset(&s_stats, 0, sizeof(s_stats));
    s_stats.indexSlotsTotal = TF_INDEX_SLOTS;
    s_stats.indexSlotsUsed = 0;
    s_stats.indexWarnAt = TF_INDEX_WARN_AT;
    s_stats.indexHardCap = TF_INDEX_HARD_CAP;
    s_stats.indexLoadState = TF_LOAD_OK;
    SetAvailable(false);
    s_pending.valid = false; /* a pending orphan refers to files of the previous mount */
    memset(&s_build, 0, sizeof(s_build));

    uint32_t runs = s_init.runs + 1;
    memset(&s_init, 0, sizeof(s_init));
    s_init.runs = runs;
    s_init.attempted = true;
    s_init.firstFailedStage = -1;
    s_init.lastErrStage = -1;
    s_init.indexSizeBytes = -1;
    s_init.legacyIndexSizeBytes = -1;
    s_init.stackFreeMinBytes = -1;
    s_init.indexSizeExpected = (uint32_t)TF_INDEX_FILE_BYTES;
    s_init.indexState = TF_INDEX_NOT_INIT;
    s_init.historyDir = TF_HISTORY_DIR;
    s_init.indexPath = TF_INDEX_PATH;
    s_init.legacyIndexPath = TF_INDEX_LEGACY_PATH;
    s_init.scratchPath = TF_SELFTEST_PATH;

    if (!MountTfCard())
        return false;

    /* "mounted" now means exactly "the filesystem mount succeeded"; whether
     * history is usable is the separate historyAvailable flag below. */
    s_stats.mounted = true;
    GetSpace(&s_stats.capacityBytes, &s_stats.usedBytes, &s_stats.freeBytes);

    /* History directory: same single mkdir as before. A failure is recorded
     * but (as before) does not stop init; the index stages below then fail
     * visibly if the directory really is unusable. */
    int64_t t0 = NowUs();
    struct stat st;
    if (stat(TF_HISTORY_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        s_init.dirExisted = true;
        StageSet(TF_STAGE_HIST_DIR, TF_RES_OK, 0, 0, 0, t0);
    } else {
        errno = 0;
        int rc = mkdir(TF_HISTORY_DIR, 0775);
        int e = errno;
        if (rc == 0 || e == EEXIST)
            StageSet(TF_STAGE_HIST_DIR, TF_RES_OK, 0, 0, 0, t0);
        else
            StageSet(TF_STAGE_HIST_DIR, TF_RES_FAIL, 0, e, 0, t0);
    }

    /* ---- index: use the committed indexv2.dat, or build it ---- */
    t0 = NowUs();
    bool v2Ok = false;
    {
        errno = 0;
        FILE *idx = fopen(TF_INDEX_PATH, "rb");
        int openErrno = errno;
        if (idx) {
            s_init.indexFileOpened = true;
            TfIndexHeader ih;
            bool hdrOk = fread(&ih, sizeof(ih), 1, idx) == 1 && IndexHeaderValid(&ih, true);
            long sz = -1;
            if (fseek(idx, 0, SEEK_END) == 0)
                sz = ftell(idx);
            fclose(idx);
            s_init.indexSizeBytes = (int32_t)sz;
            v2Ok = hdrOk && sz == (long)TF_INDEX_FILE_BYTES;
            s_init.indexSizeValid = v2Ok;
            StageSet(TF_STAGE_INDEX_CHECK, TF_RES_OK, 0, 0, 0, t0);
        } else if (openErrno == ENOENT) {
            StageSet(TF_STAGE_INDEX_CHECK, TF_RES_OK, 0, ENOENT, 0, t0); /* missing: normal on a fresh card or first boot after upgrade */
        } else {
            StageSet(TF_STAGE_INDEX_CHECK, TF_RES_FAIL, 0, openErrno, 0, t0);
        }
    }
    bool legacyUsable = false;
    {
        struct stat ls;
        if (stat(TF_INDEX_LEGACY_PATH, &ls) == 0) {
            s_init.legacyIndexSizeBytes = (int32_t)ls.st_size;
            legacyUsable = ls.st_size == (off_t)(TF_INDEX_LEGACY_SLOTS * sizeof(TfIndexSlot));
        }
    }

    if (!v2Ok) {
        /* Migration (legacy 4096-slot index found), rebuild (index missing,
         * unfinished or invalid) or fresh create. Builds beside the legacy
         * index and commits only after verification; blocking, one-time. */
        if (!BuildIndexV2(legacyUsable)) {
            s_init.indexState = TF_INDEX_FAILED;
            return false; /* card present but index not usable: stay degraded, data untouched */
        }
        s_init.indexState = s_build.mode == TF_BUILD_MIGRATE ? TF_INDEX_MIGRATED
                          : s_build.mode == TF_BUILD_FRESH   ? TF_INDEX_FRESH
                                                             : TF_INDEX_REBUILT;
    } else {
        uint32_t used = 0;
        int e = 0;
        t0 = NowUs();
        if (CountIndexSlots(&used, &e)) {
            s_stats.indexSlotsUsed = used;
            IndexRefreshLoad();
            StageSet(TF_STAGE_INDEX_COUNT, TF_RES_OK, 0, 0, 0, t0);
        } else {
            StageSet(TF_STAGE_INDEX_COUNT, TF_RES_FAIL, 0, e, 0, t0);
        }
        s_init.indexState = TF_INDEX_LOADED;
    }

#ifdef ESP_PLATFORM
    s_init.stackFreeMinBytes = (int32_t)uxTaskGetStackHighWaterMark(NULL); /* IDF reports bytes */
#endif
    s_init.ok = true;
    SetAvailable(true);
    return true;
}

bool TfHistory_Init(void)
{
    LockInit();
    if (!Lock(TF_LOCK_FOREVER))
        return false;
    bool ok = InitInternal(true);
    Unlock();
    return ok;
}

bool TfHistory_IsAvailable(void)
{
    return s_available;
}

static bool LookupLocked(const char *icao24, TfHistoryRecord *out, uint32_t *bucketOut)
{
    if (!s_available || !icao24 || !icao24[0])
        return false;

    char normIcao[TF_ICAO_MAX];
    NormalizeIcao(icao24, normIcao);

    int64_t t0 = NowUs();
    TfIdx ih;
    IdxProbeResult pr;
    if (!IdxOpen(&ih, false)) {
        NoteRuntimeFail("lookup_index_open");
        s_stats.errors++;
        s_stats.lookupMisses++;
        return false;
    }
    IdxProbeStatus ps = IdxFindOrSlot(&ih, normIcao, &pr);
    IdxClose(&ih);
    if (ps == IDXP_ERR) {
        NoteRuntimeFail("lookup_index_read");
        s_stats.errors++;
        s_stats.lookupMisses++;
        return false;
    }
    RecordProbe(ps == IDXP_FOUND, pr.probes);
    if (ps != IDXP_FOUND) {
        s_stats.lookupMisses++;
        return false;
    }
    const TfIndexSlot slot = pr.slot;

    char path[512];
    BucketPath(slot.bucketFingerprint, path, sizeof(path));
    TfOnDiskRecord rec;
    if (!ReadRecordAt(path, slot.recordOffset, &rec) ||
        rec.crc32 != Crc32(&rec.pub, sizeof(rec.pub))) {
        NoteRuntimeFail("lookup_read");
        s_stats.recoveryOps++;
        s_stats.errors++;
        s_stats.lookupMisses++;
        return false; /* corrupt: treated as absent; self-heals on next write */
    }

    s_stats.lookups++;
    UpdateTiming(&s_stats.lastLookupUs, &s_stats.bestLookupUs, &s_stats.worstLookupUs,
                 (uint32_t)(NowUs() - t0));

    if (out) *out = rec.pub;
    if (bucketOut) *bucketOut = slot.bucketFingerprint;
    return true;
}

static bool UpsertLocked(uint32_t bucketFingerprint, const TfHistoryRecord *rec)
{
    if (!s_available || !rec || !rec->icao24[0] || bucketFingerprint == TF_BUCKET_RESERVED)
        return false;

    int64_t t0 = NowUs();

    char normIcao[TF_ICAO_MAX];
    NormalizeIcao(rec->icao24, normIcao);

    /* 1. Probe the index FIRST (one open, closed before any other file is
     *    opened). One pass answers "indexed?" and, if not, which slot a new
     *    entry takes, so no second probe is ever needed. */
    TfIdx ih;
    IdxProbeResult pr;
    if (!IdxOpen(&ih, false)) {
        NoteRuntimeFail("upsert_index_open");
        s_stats.errors++;
        return false;
    }
    IdxProbeStatus ps = IdxFindOrSlot(&ih, normIcao, &pr);
    IdxClose(&ih);
    if (ps == IDXP_ERR) {
        NoteRuntimeFail("upsert_index_probe");
        s_stats.errors++;
        return false;
    }
    RecordProbe(ps == IDXP_FOUND, pr.probes);
    const bool found = (ps == IDXP_FOUND);

    /* 2. Hard cap: a NEW aircraft is refused BEFORE anything is written, so
     *    no unindexed orphan record exists and a History Manager retry cannot
     *    create duplicates. The aircraft stays in Hot Seen (not our concern).
     *    Aircraft already in the index keep updating normally. */
    if (!found && s_stats.indexSlotsUsed >= TF_INDEX_HARD_CAP) {
        s_stats.indexInsertCapRejected++;
        snprintf(s_rtErr, sizeof(s_rtErr), "index_cap used=%u/%u", (unsigned)s_stats.indexSlotsUsed, (unsigned)TF_INDEX_SLOTS);
        return false;
    }
    const uint32_t slotIdx = found ? pr.idx : pr.freeIdx;

    char path[512];
    TfBucketHeader hdr;
    if (!EnsureBucket(bucketFingerprint, path, &hdr)) {
        NoteRuntimeFail("upsert_bucket");
        s_stats.errors++;
        return false;
    }

    TfOnDiskRecord od;
    od.pub = *rec;
    od.crc32 = Crc32(&od.pub, sizeof(od.pub));

    bool ok;
    if (found && pr.slot.bucketFingerprint == bucketFingerprint) {
        ok = WriteRecordAt(path, pr.slot.recordOffset, &od);
        if (ok) s_stats.updates++;
    } else {
        /* New aircraft, or its classification moved it to a different Universal
         * Value bucket: append (the old bucket's record is superseded, never
         * deleted) and write the reserved/existing index slot. If this exact
         * record was appended earlier but its slot write failed, rewrite that
         * record in place instead of appending a duplicate. */
        uint32_t newOffset = 0;
        const bool adopt = s_pending.valid && s_pending.fp == bucketFingerprint &&
                           strncmp(s_pending.icao24, normIcao, TF_ICAO_MAX) == 0 &&
                           (uint64_t)s_pending.offset + sizeof(TfOnDiskRecord) <=
                               (uint64_t)hdr.recordCount * sizeof(TfOnDiskRecord);
        if (adopt) {
            newOffset = s_pending.offset;
            ok = WriteRecordAt(path, newOffset, &od);
            if (ok) s_stats.orphanAdopted++;
        } else {
            ok = AppendRecord(path, &hdr, &od, &newOffset);
        }
        if (ok) {
            TfIndexSlot fresh;
            IdxMakeSlot(&fresh, normIcao, bucketFingerprint, newOffset);
            const bool wrote = IdxOpen(&ih, true) && IdxWriteSlot(&ih, slotIdx, &fresh);
            IdxClose(&ih);
            if (wrote) {
                s_pending.valid = false;
                if (found) {
                    s_stats.updates++;
                } else {
                    s_stats.indexSlotsUsed++;
                    IndexRefreshLoad();
                    s_stats.creates++;
                }
            } else {
                s_pending.valid = true;
                NormalizeIcao(normIcao, s_pending.icao24);
                s_pending.fp = bucketFingerprint;
                s_pending.offset = newOffset;
                s_stats.indexInsertIoFail++;
                ok = false;
            }
        }
    }

    if (!ok) {
        NoteRuntimeFail("upsert_write");
        s_stats.errors++;
        return false;
    }

    s_stats.writes++;
    UpdateTiming(&s_stats.lastWriteUs, &s_stats.bestWriteUs, &s_stats.worstWriteUs,
                 (uint32_t)(NowUs() - t0));
    return true;
}

bool TfHistory_Lookup(const char *icao24, TfHistoryRecord *out, uint32_t *bucketOut)
{
    if (!s_available)
        return false; /* fast path; also guarantees Init (and the lock) exist */
    if (!Lock(TF_LOCK_FOREVER))
        return false;
    bool r = LookupLocked(icao24, out, bucketOut); /* re-checks s_available under the lock */
    Unlock();
    return r;
}

bool TfHistory_Upsert(uint32_t bucketFingerprint, const TfHistoryRecord *rec)
{
    if (!s_available)
        return false;
    if (!Lock(TF_LOCK_FOREVER))
        return false;
    bool r = UpsertLocked(bucketFingerprint, rec);
    Unlock();
    return r;
}

bool TfHistory_RebuildIndex(void)
{
    if (!Lock(TF_LOCK_FOREVER))
        return false;
    bool r = false;
    if (s_stats.mounted) {
        SetAvailable(false); /* the index is unusable while it is rebuilt */
        r = BuildIndexV2(false);
        if (r) {
            s_init.indexState = TF_INDEX_REBUILT;
            SetAvailable(true);
        } else {
            s_init.indexState = TF_INDEX_FAILED;
        }
    }
    Unlock();
    return r;
}

void TfHistory_GetStats(TfHistoryStats *out)
{
    if (out) *out = s_stats;
}

/* =====================================================================
 * Diagnostics: init info, unmount/reinit, filesystem self-test
 * (none of this changes the storage format or touches history data)
 * ===================================================================== */

void TfHistory_GetInitInfo(TfInitInfo *out)
{
    if (!out)
        return;
    *out = s_init; /* unlocked snapshot, same as TfHistory_GetStats */
    out->historyDir = TF_HISTORY_DIR;
    out->indexPath = TF_INDEX_PATH;
    out->legacyIndexPath = TF_INDEX_LEGACY_PATH;
    out->scratchPath = TF_SELFTEST_PATH;
    snprintf(out->lastRuntimeError, sizeof(out->lastRuntimeError), "%s", s_rtErr);
}

void TfHistory_GetReinitTrace(TfReinitTrace *out)
{
    if (out)
        *out = s_trace;
}

void TfHistory_GetSelfTest(TfSelfTestResult *out)
{
    if (out)
        *out = s_selftest;
}

static void TraceSet(TfTraceStep step, TfResult r, int espErr, int err, int64_t t0)
{
    FillStep(&s_trace.step[step], r, espErr, err, 0, t0);
}

static void TraceSkipFrom(int first)
{
    for (int i = first; i < TF_TR_COUNT; i++)
        if (s_trace.step[i].result == TF_RES_NOT_RUN)
            s_trace.step[i].result = TF_RES_SKIPPED;
}

#ifdef ESP_PLATFORM

bool TfHistory_Unmount(void)
{
    memset(s_trace.step, 0, sizeof(s_trace.step)); /* new request: clear the previous sequence */
    s_trace.active = true;
    s_trace.runs++;
    TraceSet(TF_TR_REQUEST, TF_RES_OK, 0, 0, 0);

    if (!Lock(TF_LOCK_DIAG_MS)) {
        TraceSet(TF_TR_FS_UNMOUNT, TF_RES_FAIL, ESP_ERR_TIMEOUT, 0, 0); /* storage busy; nothing was changed */
        TraceSkipFrom(TF_TR_BUS_FREE);
        return false;
    }

    SetAvailable(false); /* Lookup/Upsert now refuse; any in-flight op already finished (we hold the lock) */

    if (!s_card) {
        TraceSet(TF_TR_FS_UNMOUNT, TF_RES_FAIL, ESP_ERR_INVALID_STATE, 0, 0); /* nothing is mounted */
        TraceSkipFrom(TF_TR_BUS_FREE);
        Unlock();
        return false;
    }

    int64_t t0 = NowUs();
    errno = 0;
    esp_err_t r = esp_vfs_fat_sdcard_unmount(TF_MOUNT_POINT, s_card);
    int e = errno;
    if (r != ESP_OK) {
        /* Card state is now uncertain: keep s_card so a later retry is possible. */
        TraceSet(TF_TR_FS_UNMOUNT, TF_RES_FAIL, r, e, t0);
        TraceSkipFrom(TF_TR_BUS_FREE);
        ESP_LOGW(TAG, "TF unmount failed (%s)", esp_err_to_name(r));
        Unlock();
        return false;
    }
    TraceSet(TF_TR_FS_UNMOUNT, TF_RES_OK, r, 0, t0);
    s_card = NULL;
    s_stats.mounted = false;
    s_stats.capacityBytes = s_stats.usedBytes = s_stats.freeBytes = 0;
    s_init.ok = false;
    s_init.indexState = TF_INDEX_NOT_INIT;

    t0 = NowUs();
    errno = 0;
    r = spi_bus_free((spi_host_device_t)TF_SPI_HOST);
    e = errno;
    TraceSet(TF_TR_BUS_FREE, r == ESP_OK ? TF_RES_OK : TF_RES_FAIL, r, r == ESP_OK ? 0 : e, t0);
    if (r != ESP_OK)
        ESP_LOGW(TAG, "spi_bus_free failed (%s)", esp_err_to_name(r));
    /* remount steps stay "not run" until Reinit is requested */
    Unlock();
    return r == ESP_OK;
}

bool TfHistory_Reinit(void)
{
    if (!s_trace.active) {
        memset(s_trace.step, 0, sizeof(s_trace.step));
        s_trace.active = true;
        s_trace.runs++;
    }
    for (int i = TF_TR_REMOUNT_REQ; i < TF_TR_COUNT; i++)
        memset(&s_trace.step[i], 0, sizeof(s_trace.step[i]));

    if (!Lock(TF_LOCK_DIAG_MS)) {
        TraceSet(TF_TR_REMOUNT_REQ, TF_RES_FAIL, ESP_ERR_TIMEOUT, 0, 0);
        TraceSkipFrom(TF_TR_BUS_INIT);
        return false;
    }
    if (s_card) {
        /* Still mounted: refuse rather than mount twice. Unmount first. */
        TraceSet(TF_TR_REMOUNT_REQ, TF_RES_FAIL, ESP_ERR_INVALID_STATE, 0, 0);
        TraceSkipFrom(TF_TR_BUS_INIT);
        Unlock();
        return false;
    }
    TraceSet(TF_TR_REMOUNT_REQ, TF_RES_OK, 0, 0, 0);

    bool ok = InitInternal(false);

    s_trace.step[TF_TR_BUS_INIT] = s_init.stage[TF_STAGE_SPI_BUS];
    s_trace.step[TF_TR_MOUNT] = s_init.stage[TF_STAGE_MOUNT];
    if (s_init.stage[TF_STAGE_MOUNT].result != TF_RES_OK) {
        if (s_init.stage[TF_STAGE_SPI_BUS].result != TF_RES_OK)
            s_trace.step[TF_TR_MOUNT].result = TF_RES_SKIPPED; /* bus failed: mount never attempted */
        TraceSkipFrom(TF_TR_HISTORY_INIT);
    } else if (ok) {
        TraceSet(TF_TR_HISTORY_INIT, TF_RES_OK, 0, 0, 0);
    } else {
        /* mounted but history not usable: report the first failing stage after the mount */
        TfStepInfo *h = &s_trace.step[TF_TR_HISTORY_INIT];
        memset(h, 0, sizeof(*h));
        h->result = TF_RES_FAIL;
        for (int i = TF_STAGE_FS_INFO; i < TF_STAGE_COUNT; i++) {
            if (s_init.stage[i].result == TF_RES_FAIL) {
                *h = s_init.stage[i];
                break;
            }
        }
    }
    Unlock();
    return ok;
}

#else /* host build: no card to unmount; report honestly instead of faking it */

bool TfHistory_Unmount(void)
{
    memset(s_trace.step, 0, sizeof(s_trace.step));
    s_trace.active = true;
    s_trace.runs++;
    TraceSet(TF_TR_REQUEST, TF_RES_OK, 0, 0, 0);
    TraceSet(TF_TR_FS_UNMOUNT, TF_RES_FAIL, -1, ENOTSUP, 0);
    TraceSkipFrom(TF_TR_BUS_FREE);
    return false;
}

bool TfHistory_Reinit(void)
{
    TraceSet(TF_TR_REMOUNT_REQ, TF_RES_FAIL, -1, ENOTSUP, 0);
    TraceSkipFrom(TF_TR_BUS_INIT);
    return false;
}

#endif

/* ---- filesystem self-test (scratch file only) ---- */

#define TF_SELFTEST_BYTES 64u

static void SelfStep(int idx, TfResult r, int espErr, int err, int64_t t0)
{
    FillStep(&s_selftest.step[idx], r, espErr, err, 0, t0);
    if (r == TF_RES_FAIL && s_selftest.failedStep < 0)
        s_selftest.failedStep = (int8_t)idx;
}

bool TfHistory_RunSelfTest(void)
{
    TfSelfTestResult *r = &s_selftest;
    uint32_t runs = r->runs + 1;
    memset(r, 0, sizeof(*r));
    r->runs = runs;
    r->ran = true;
    r->failedStep = -1;

    const int rcFail = -1; /* ESP_FAIL on target; plain -1 keeps this host-testable */
    int64_t tAll = NowUs();
    int64_t t0;
    bool locked = false;
    bool created = false;
    FILE *f = NULL;
    uint8_t wbuf[TF_SELFTEST_BYTES], rbuf[TF_SELFTEST_BYTES];

    /* Known, non-trivial payload: tag + position-dependent bytes. */
    memset(wbuf, 0, sizeof(wbuf));
    memcpy(wbuf, "FR7-TF-SELFTEST\n", 16);
    for (size_t i = 16; i < sizeof(wbuf); i++)
        wbuf[i] = (uint8_t)(i * 37u + 11u);

    t0 = NowUs();
    locked = Lock(TF_LOCK_DIAG_MS);
    if (!locked) {
#ifdef ESP_PLATFORM
        SelfStep(TF_ST_LOCK, TF_RES_FAIL, ESP_ERR_TIMEOUT, 0, t0);
#else
        SelfStep(TF_ST_LOCK, TF_RES_FAIL, rcFail, 0, t0);
#endif
        goto done;
    }
    SelfStep(TF_ST_LOCK, TF_RES_OK, 0, 0, t0);

    t0 = NowUs();
#ifdef ESP_PLATFORM
    if (!s_card || !s_stats.mounted) {
        SelfStep(TF_ST_MOUNTED, TF_RES_FAIL, ESP_ERR_INVALID_STATE, 0, t0);
        goto done;
    }
#endif
    SelfStep(TF_ST_MOUNTED, TF_RES_OK, 0, 0, t0);

    /* Same directory the history files live in, created with the same
     * single mkdir the init path uses (no parallel directory structure). */
    t0 = NowUs();
    {
        struct stat st;
        if (stat(TF_HISTORY_DIR, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                SelfStep(TF_ST_DIR, TF_RES_OK, 0, 0, t0);
            } else {
                SelfStep(TF_ST_DIR, TF_RES_FAIL, rcFail, ENOTDIR, t0);
                goto done;
            }
        } else {
            errno = 0;
            int rc = mkdir(TF_HISTORY_DIR, 0775);
            int e = errno;
            if (rc == 0 || e == EEXIST) {
                SelfStep(TF_ST_DIR, TF_RES_OK, 0, 0, t0);
            } else {
                SelfStep(TF_ST_DIR, TF_RES_FAIL, rcFail, e, t0);
                goto done;
            }
        }
    }

    t0 = NowUs();
    errno = 0;
    f = fopen(TF_SELFTEST_PATH, "wb"); /* our own scratch file; overwrites a leftover from an interrupted earlier run */
    if (!f) {
        SelfStep(TF_ST_OPEN_W, TF_RES_FAIL, rcFail, errno, t0);
        goto done;
    }
    created = true;
    SelfStep(TF_ST_OPEN_W, TF_RES_OK, 0, 0, t0);

    t0 = NowUs();
    errno = 0;
    if (fwrite(wbuf, 1, sizeof(wbuf), f) != sizeof(wbuf)) {
        SelfStep(TF_ST_WRITE, TF_RES_FAIL, rcFail, errno ? errno : EIO, t0);
        goto done;
    }
    r->writeUs = (uint32_t)(NowUs() - t0);
    SelfStep(TF_ST_WRITE, TF_RES_OK, 0, 0, t0);

    t0 = NowUs();
    errno = 0;
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        SelfStep(TF_ST_FLUSH, TF_RES_FAIL, rcFail, errno ? errno : EIO, t0);
        goto done;
    }
    SelfStep(TF_ST_FLUSH, TF_RES_OK, 0, 0, t0);

    t0 = NowUs();
    errno = 0;
    int crc = fclose(f);
    f = NULL; /* the handle is gone after fclose even on failure */
    if (crc != 0) {
        SelfStep(TF_ST_CLOSE_W, TF_RES_FAIL, rcFail, errno ? errno : EIO, t0);
        goto done;
    }
    SelfStep(TF_ST_CLOSE_W, TF_RES_OK, 0, 0, t0);

    t0 = NowUs();
    errno = 0;
    f = fopen(TF_SELFTEST_PATH, "rb");
    if (!f) {
        SelfStep(TF_ST_OPEN_R, TF_RES_FAIL, rcFail, errno, t0);
        goto done;
    }
    SelfStep(TF_ST_OPEN_R, TF_RES_OK, 0, 0, t0);

    memset(rbuf, 0, sizeof(rbuf));
    t0 = NowUs();
    errno = 0;
    size_t got = fread(rbuf, 1, sizeof(rbuf), f);
    if (got != sizeof(rbuf)) {
        SelfStep(TF_ST_READ, TF_RES_FAIL, rcFail, errno ? errno : EIO, t0);
        goto done;
    }
    r->readUs = (uint32_t)(NowUs() - t0);
    SelfStep(TF_ST_READ, TF_RES_OK, 0, 0, t0);

    t0 = NowUs();
    {
        struct stat st;
        bool sizeOk = (stat(TF_SELFTEST_PATH, &st) == 0) && st.st_size == (off_t)sizeof(wbuf);
        bool eofOk = (fgetc(f) == EOF);
        if (memcmp(wbuf, rbuf, sizeof(wbuf)) != 0 || !sizeOk || !eofOk) {
            SelfStep(TF_ST_VERIFY, TF_RES_FAIL, rcFail, 0, t0); /* data mismatch / wrong size: not an errno condition */
            goto done;
        }
        SelfStep(TF_ST_VERIFY, TF_RES_OK, 0, 0, t0);
    }

    t0 = NowUs();
    errno = 0;
    crc = fclose(f);
    f = NULL;
    if (crc != 0) {
        SelfStep(TF_ST_CLOSE_R, TF_RES_FAIL, rcFail, errno ? errno : EIO, t0);
        goto done;
    }
    SelfStep(TF_ST_CLOSE_R, TF_RES_OK, 0, 0, t0);

done:
    if (f) {
        fclose(f); /* failure path: never leak the handle */
        f = NULL;
    }
    if (created) {
        /* Delete the scratch file on success AND on any failure after it was created. */
        struct stat st;
        t0 = NowUs();
        errno = 0;
        int rc = remove(TF_SELFTEST_PATH);
        int e = errno;
        bool gone = (rc == 0) && (stat(TF_SELFTEST_PATH, &st) != 0);
        if (gone) {
            FillStep(&r->step[TF_ST_DELETE], TF_RES_OK, 0, 0, 0, t0);
        } else {
            SelfStep(TF_ST_DELETE, TF_RES_FAIL, rcFail, e, t0);
        }
    }
    for (int i = 0; i < TF_ST_COUNT; i++)
        if (r->step[i].result == TF_RES_NOT_RUN)
            r->step[i].result = TF_RES_SKIPPED;
    r->passed = (r->failedStep < 0);
    r->totalUs = (uint32_t)(NowUs() - tAll);
    if (locked)
        Unlock();
    return r->passed;
}


/* =====================================================================
 * Read-only browsing (see tf_history.h). Nothing below writes to the card.
 * ===================================================================== */

/* "00000000.DAT" style names only: exactly 8 hex digits + ".DAT"
 * (FAT with LFN disabled returns upper-case 8.3 names; host builds keep the
 * ".dat" we create, hence the case-insensitive match). index.dat,
 * _SELFTST.TMP and anything else are ignored. 0xFFFFFFFF is never used as a
 * fingerprint, so it is skipped here as well. */
static bool ParseBucketName(const char *name, uint32_t *fpOut)
{
    if (!name || strlen(name) != 12 || name[8] != '.')
        return false;
    if (toupper((unsigned char)name[9]) != 'D' || toupper((unsigned char)name[10]) != 'A' ||
        toupper((unsigned char)name[11]) != 'T')
        return false;
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isxdigit(c))
            return false;
        v = (v << 4) | (uint32_t)(c <= '9' ? c - '0' : (toupper(c) - 'A') + 10);
    }
    if (v == 0xFFFFFFFFu)
        return false;
    *fpOut = v;
    return true;
}

/* Classifies one record against the index using an already-open index
 * handle (one open per browse call; same probe rules as the radar path, but
 * browsing never touches the probe counters). NULL handle => unknown. */
static uint8_t ClassifyOpen(TfIdx *ih, const char *icao24, uint32_t fp, uint32_t recordIdx)
{
    if (!ih || !ih->f)
        return TF_REC_INDEX_UNKNOWN;
    char norm[TF_ICAO_MAX];
    NormalizeIcao(icao24, norm);
    IdxProbeResult pr;
    IdxProbeStatus r = IdxFindOrSlot(ih, norm, &pr);
    if (r == IDXP_ERR)
        return TF_REC_INDEX_UNKNOWN;
    if (r == IDXP_ABSENT)
        return TF_REC_NOT_INDEXED;
    return (pr.slot.bucketFingerprint == fp && pr.slot.recordOffset == recordIdx * (uint32_t)sizeof(TfOnDiskRecord))
               ? TF_REC_LIVE
               : TF_REC_SUPERSEDED;
}

static bool BrowseEnter(void)
{
    if (!s_available)
        return false; /* also guarantees Init (and the lock) exist */
    return true;
}

TfBrowseStatus TfHistory_NextBucket(bool afterValid, uint32_t after, uint32_t *fpOut,
                                    uint32_t *recordCountOut, bool *headerOkOut)
{
    if (!fpOut)
        return TF_BR_IO;
    if (!BrowseEnter())
        return TF_BR_UNAVAILABLE;
    if (!Lock(TF_LOCK_BROWSE_MS))
        return TF_BR_BUSY;
    if (!s_available) { /* unmounted while we waited */
        Unlock();
        return TF_BR_UNAVAILABLE;
    }

    DIR *d = opendir(TF_HISTORY_DIR);
    if (!d) {
        Unlock();
        return TF_BR_IO;
    }
    bool found = false;
    uint32_t best = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t fp;
        if (!ParseBucketName(e->d_name, &fp))
            continue;
        if (afterValid && fp <= after)
            continue;
        if (!found || fp < best) {
            best = fp;
            found = true;
        }
    }
    closedir(d);

    TfBrowseStatus st = TF_BR_END;
    if (found) {
        char path[512];
        TfBucketHeader hdr;
        BucketPath(best, path, sizeof(path));
        bool ok = ReadBucketHeader(path, &hdr);
        *fpOut = best;
        if (recordCountOut)
            *recordCountOut = ok ? hdr.recordCount : 0;
        if (headerOkOut)
            *headerOkOut = ok;
        st = TF_BR_OK;
    }
    Unlock();
    return st;
}

TfBrowseStatus TfHistory_BucketInfo(uint32_t fp, uint32_t *recordCountOut)
{
    if (!BrowseEnter())
        return TF_BR_UNAVAILABLE;
    if (!Lock(TF_LOCK_BROWSE_MS))
        return TF_BR_BUSY;
    if (!s_available) {
        Unlock();
        return TF_BR_UNAVAILABLE;
    }
    char path[512];
    TfBucketHeader hdr;
    BucketPath(fp, path, sizeof(path));
    bool ok = ReadBucketHeader(path, &hdr);
    Unlock();
    if (!ok)
        return TF_BR_END;
    if (recordCountOut)
        *recordCountOut = hdr.recordCount;
    return TF_BR_OK;
}

static TfBrowseStatus BrowseChunkImpl(uint32_t fp, uint32_t startIdx, size_t maxN,
                                      TfBrowseRecord *out, size_t *gotN, uint32_t *recordCountOut, bool classify);

TfBrowseStatus TfHistory_ListBuckets(bool afterValid, uint32_t after, size_t maxN, uint32_t *fps,
                                     size_t *gotN, uint32_t *totalOut, bool *moreOut)
{
    if (gotN)
        *gotN = 0;
    if (totalOut)
        *totalOut = 0;
    if (moreOut)
        *moreOut = false;
    if (!fps || maxN == 0)
        return TF_BR_IO;
    if (maxN > TF_LIST_MAX)
        maxN = TF_LIST_MAX;
    if (!BrowseEnter())
        return TF_BR_UNAVAILABLE;
    if (!Lock(TF_LOCK_BROWSE_MS))
        return TF_BR_BUSY;
    if (!s_available) {
        Unlock();
        return TF_BR_UNAVAILABLE;
    }
    DIR *d = opendir(TF_HISTORY_DIR);
    if (!d) {
        Unlock();
        return TF_BR_IO;
    }
    /* One directory pass: keep the maxN smallest fingerprints > after, sorted
     * (bounded insertion into the caller's array; nothing else is held). */
    size_t n = 0;
    uint32_t total = 0, beyond = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t fp;
        if (!ParseBucketName(e->d_name, &fp))
            continue;
        total++;
        if (afterValid && fp <= after)
            continue;
        if (n == maxN && fp >= fps[n - 1]) {
            beyond++;
            continue;
        }
        size_t pos = n == maxN ? n - 1 : n;
        if (n == maxN)
            beyond++; /* the current last one drops out */
        while (pos > 0 && fps[pos - 1] > fp) {
            fps[pos] = fps[pos - 1];
            pos--;
        }
        fps[pos] = fp;
        if (n < maxN)
            n++;
    }
    closedir(d);
    Unlock();
    if (gotN)
        *gotN = n;
    if (totalOut)
        *totalOut = total;
    if (moreOut)
        *moreOut = beyond > 0;
    return n ? TF_BR_OK : TF_BR_END;
}

TfBrowseStatus TfHistory_ScanChunk(uint32_t fp, uint32_t startIdx, size_t maxN,
                                   TfBrowseRecord *out, size_t *gotN, uint32_t *recordCountOut)
{
    return BrowseChunkImpl(fp, startIdx, maxN, out, gotN, recordCountOut, false);
}

TfBrowseStatus TfHistory_BrowseChunk(uint32_t fp, uint32_t startIdx, size_t maxN,
                                     TfBrowseRecord *out, size_t *gotN, uint32_t *recordCountOut)
{
    return BrowseChunkImpl(fp, startIdx, maxN, out, gotN, recordCountOut, true);
}

/* classify == false (search scan): records are only CRC-checked; the index is
 * not opened, so a scan step is one header read + one sequential record read. */
static TfBrowseStatus BrowseChunkImpl(uint32_t fp, uint32_t startIdx, size_t maxN,
                                      TfBrowseRecord *out, size_t *gotN, uint32_t *recordCountOut, bool classify)
{
    if (gotN)
        *gotN = 0;
    if (!out || maxN == 0)
        return TF_BR_IO;
    if (maxN > TF_BROWSE_CHUNK_MAX)
        maxN = TF_BROWSE_CHUNK_MAX;
    if (!BrowseEnter())
        return TF_BR_UNAVAILABLE;
    if (!Lock(TF_LOCK_BROWSE_MS))
        return TF_BR_BUSY;
    if (!s_available) {
        Unlock();
        return TF_BR_UNAVAILABLE;
    }

    TfBrowseStatus st = TF_BR_OK;
    char path[512];
    TfBucketHeader hdr;
    TfOnDiskRecord raw[TF_BROWSE_CHUNK_MAX]; /* 8 x 52 B = 416 B of stack: the whole working set */
    size_t n = 0;

    BucketPath(fp, path, sizeof(path));
    if (!ReadBucketHeader(path, &hdr)) {
        st = TF_BR_END;
        goto done;
    }
    if (recordCountOut)
        *recordCountOut = hdr.recordCount;
    if (startIdx >= hdr.recordCount) {
        st = TF_BR_END;
        goto done;
    }
    n = hdr.recordCount - startIdx;
    if (n > maxN)
        n = maxN;

    {
        FILE *f = fopen(path, "rb");
        if (!f) {
            st = TF_BR_IO;
            n = 0;
            goto done;
        }
        size_t got = 0;
        if (fseek(f, (long)(sizeof(TfBucketHeader) + (size_t)startIdx * sizeof(TfOnDiskRecord)), SEEK_SET) == 0)
            got = fread(raw, sizeof(TfOnDiskRecord), n, f);
        fclose(f);
        if (got == 0) { /* nothing readable at all: report it, show no rows */
            st = TF_BR_IO;
            n = 0;
            goto done;
        }
        /* A short read keeps the rows it did get; the missing ones are zeroed so they
         * show as unreadable, and the page cursor still advances past them. */
        for (size_t i = got; i < n; i++)
            memset(&raw[i], 0, sizeof(raw[i]));
    }

    {
        TfIdx idx; /* open failure => rows come back INDEX_UNKNOWN, never guessed */
        if (classify)
            (void)IdxOpen(&idx, false);
        for (size_t i = 0; i < n; i++) {
            out[i].state = TF_REC_CORRUPT;
            memset(&out[i].rec, 0, sizeof(out[i].rec));
            if (raw[i].pub.icao24[0] == '\0' || raw[i].crc32 != Crc32(&raw[i].pub, sizeof(raw[i].pub)))
                continue;
            out[i].rec = raw[i].pub;
            out[i].rec.icao24[TF_ICAO_MAX - 1] = '\0';
            out[i].rec.callsign[sizeof(out[i].rec.callsign) - 1] = '\0';
            out[i].state = classify ? ClassifyOpen(&idx, out[i].rec.icao24, fp, startIdx + (uint32_t)i)
                                    : (uint8_t)TF_REC_INDEX_UNKNOWN;
        }
        if (classify)
            IdxClose(&idx);
    }

done:
    Unlock();
    if (gotN)
        *gotN = n;
    return st;
}

TfBrowseStatus TfHistory_IsLive(const char *icao24, uint32_t fp, uint32_t recordIdx, uint8_t *stateOut)
{
    if (!stateOut || !icao24)
        return TF_BR_IO;
    if (!BrowseEnter())
        return TF_BR_UNAVAILABLE;
    if (!Lock(TF_LOCK_BROWSE_MS))
        return TF_BR_BUSY;
    if (!s_available) {
        Unlock();
        return TF_BR_UNAVAILABLE;
    }
    TfIdx idx;
    (void)IdxOpen(&idx, false);
    *stateOut = ClassifyOpen(&idx, icao24, fp, recordIdx);
    IdxClose(&idx);
    Unlock();
    return TF_BR_OK;
}

TfBrowseStatus TfHistory_FindForBrowse(const char *icao24, TfBrowseRecord *out, uint32_t *fpOut, uint32_t *recordIdxOut)
{
    if (!out || !icao24 || !icao24[0])
        return TF_BR_IO;
    if (!BrowseEnter())
        return TF_BR_UNAVAILABLE;
    if (!Lock(TF_LOCK_BROWSE_MS))
        return TF_BR_BUSY;
    if (!s_available) {
        Unlock();
        return TF_BR_UNAVAILABLE;
    }

    TfBrowseStatus st = TF_BR_END;
    char norm[TF_ICAO_MAX];
    NormalizeIcao(icao24, norm);
    TfIdx idx;
    if (!IdxOpen(&idx, false)) {
        Unlock();
        return TF_BR_IO;
    }
    IdxProbeResult pr;
    IdxProbeStatus r = IdxFindOrSlot(&idx, norm, &pr);
    IdxClose(&idx);
    const TfIndexSlot slot = pr.slot;
    if (r == IDXP_ERR) {
        st = TF_BR_IO;
    } else if (r == IDXP_FOUND) {
        char path[512];
        TfOnDiskRecord rec;
        BucketPath(slot.bucketFingerprint, path, sizeof(path));
        uint32_t recIdx = slot.recordOffset / (uint32_t)sizeof(TfOnDiskRecord);
        memset(out, 0, sizeof(*out));
        if (ReadRecordAt(path, slot.recordOffset, &rec) && rec.crc32 == Crc32(&rec.pub, sizeof(rec.pub))) {
            out->rec = rec.pub;
            out->rec.icao24[TF_ICAO_MAX - 1] = '\0';
            out->rec.callsign[sizeof(out->rec.callsign) - 1] = '\0';
            out->state = TF_REC_LIVE; /* it is what the index points at, by definition */
        } else {
            out->state = TF_REC_CORRUPT;
        }
        if (fpOut)
            *fpOut = slot.bucketFingerprint;
        if (recordIdxOut)
            *recordIdxOut = recIdx;
        st = TF_BR_OK;
    }
    Unlock();
    return st;
}
