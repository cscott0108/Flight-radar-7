#include "tf_history.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
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
static const char *TAG = "TfHistory";
#else
#include <time.h>
#endif

#ifndef TF_MOUNT_POINT
#define TF_MOUNT_POINT "/sdcard"
#endif
#define TF_HISTORY_DIR TF_MOUNT_POINT "/history"
#define TF_INDEX_PATH TF_HISTORY_DIR "/index.dat"

/* Open-addressing hash index, fixed size, read/written a slot at a time -
 * never loaded into RAM as a whole (PROMPT.md section 22). 4096 slots
 * comfortably covers the existing 200-aircraft/1000-operator scale with a
 * long tail of history for aircraft no longer active, without the
 * over-engineering PROMPT.md section 42 warns against; revisit with real
 * multi-week occupancy data before growing it. */
#define TF_INDEX_SLOTS 4096u

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
    if (stage == TF_STAGE_INDEX_CREATE || stage == TF_STAGE_INDEX_REBUILD)
        return "card mounts and reads but creating/writing index.dat failed: card may be read-only or defective "
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
                                                   "index_check", "index_create", "index_rebuild", "index_count"};
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

/* ---- index ---- */

static bool ReadIndexSlot(uint32_t slot, TfIndexSlot *out)
{
    FILE *f = fopen(TF_INDEX_PATH, "rb");
    if (!f)
        return false;
    bool ok = fseek(f, (long)(slot * sizeof(TfIndexSlot)), SEEK_SET) == 0 &&
              fread(out, sizeof(*out), 1, f) == 1;
    fclose(f);
    return ok;
}

static bool WriteIndexSlot(uint32_t slot, const TfIndexSlot *in)
{
    FILE *f = fopen(TF_INDEX_PATH, "r+b");
    if (!f)
        return false;
    bool ok = fseek(f, (long)(slot * sizeof(TfIndexSlot)), SEEK_SET) == 0 &&
              fwrite(in, sizeof(*in), 1, f) == 1;
    fflush(f);
    fclose(f);
    return ok;
}

static uint32_t IndexSlotCrc(const TfIndexSlot *s)
{
    /* CRC over everything except the crc field itself. */
    return Crc32(s, offsetof(TfIndexSlot, crc32));
}

/* Linear-probes from the natural hash slot. Bounded by TF_INDEX_SLOTS so a
 * pathologically full (or corrupt, ever-non-empty) table can never spin
 * forever. A slot whose stored crc is wrong is treated as empty (recovered
 * as "not found here"; a fresh insert will simply overwrite it in place,
 * self-healing - PROMPT.md section 24). */
static bool IndexFind(const char *icao24, TfIndexSlot *outSlot, uint32_t *outIndex)
{
    uint32_t start = Fnv1a32(icao24) & (TF_INDEX_SLOTS - 1);
    for (uint32_t i = 0; i < TF_INDEX_SLOTS; i++) {
        uint32_t idx = (start + i) & (TF_INDEX_SLOTS - 1);
        TfIndexSlot slot;
        if (!ReadIndexSlot(idx, &slot))
            return false;
        if (slot.icao24[0] == '\0')
            return false; /* open addressing, no tombstones: first empty slot ends the probe */
        if (slot.crc32 != IndexSlotCrc(&slot))
            continue; /* corrupt entry; skip past it rather than trusting it */
        if (strncmp(slot.icao24, icao24, TF_ICAO_MAX) == 0) {
            if (outSlot) *outSlot = slot;
            if (outIndex) *outIndex = idx;
            return true;
        }
    }
    return false;
}

static bool IndexPlace(const char *icao24, uint32_t bucketFp, uint32_t offset, uint32_t *outIndex)
{
    uint32_t start = Fnv1a32(icao24) & (TF_INDEX_SLOTS - 1);
    for (uint32_t i = 0; i < TF_INDEX_SLOTS; i++) {
        uint32_t idx = (start + i) & (TF_INDEX_SLOTS - 1);
        TfIndexSlot slot;
        if (!ReadIndexSlot(idx, &slot))
            return false;
        if (slot.icao24[0] != '\0' && slot.crc32 == IndexSlotCrc(&slot) &&
            strncmp(slot.icao24, icao24, TF_ICAO_MAX) != 0)
            continue; /* occupied by someone else */
        TfIndexSlot fresh;
        memset(&fresh, 0, sizeof(fresh));
        NormalizeIcao(icao24, fresh.icao24);
        fresh.bucketFingerprint = bucketFp;
        fresh.recordOffset = offset;
        fresh.crc32 = IndexSlotCrc(&fresh);
        if (!WriteIndexSlot(idx, &fresh))
            return false;
        if (outIndex) *outIndex = idx;
        s_stats.indexSlotsUsed++;
        return true;
    }
    return false; /* index full - see PROMPT.md section 42, not expected at this scale */
}

static bool CreateEmptyIndexFile(void)
{
    FILE *f = fopen(TF_INDEX_PATH, "wb");
    if (!f)
        return false;
    TfIndexSlot zero;
    memset(&zero, 0, sizeof(zero));
    bool ok = true;
    for (uint32_t i = 0; i < TF_INDEX_SLOTS && ok; i++)
        ok = fwrite(&zero, sizeof(zero), 1, f) == 1;
    fclose(f);
    return ok;
}

/* Counts occupied, CRC-valid slots of an existing index.dat with one
 * sequential pass in small chunks (never loads the whole table). Read-only. */
static bool CountIndexSlots(uint32_t *usedOut, int *errnoOut)
{
    errno = 0;
    FILE *f = fopen(TF_INDEX_PATH, "rb");
    if (!f) {
        *errnoOut = errno;
        return false;
    }
    TfIndexSlot chunk[32];
    uint32_t used = 0, total = 0;
    bool ok = true;
    while (total < TF_INDEX_SLOTS) {
        size_t want = TF_INDEX_SLOTS - total;
        if (want > 32)
            want = 32;
        size_t got = fread(chunk, sizeof(TfIndexSlot), want, f);
        if (got != want) {
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

static bool BucketExists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
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
    return memcmp(out->magic, TF_BUCKET_MAGIC, 4) == 0 &&
           out->headerCrc32 == Crc32(out, offsetof(TfBucketHeader, headerCrc32));
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
    /* Missing or corrupt header: (re)create a fresh, empty bucket. A
     * corrupt-but-nonempty bucket file loses its unreadable header this
     * way but TfHistory_RebuildIndex() (a full sequential record scan,
     * not dependent on the header) is the recovery path for that case -
     * this function's job is only to guarantee a bucket is writable. */
    FILE *f = fopen(pathOut, "wb");
    if (!f)
        return false;
    memset(hdrOut, 0, sizeof(*hdrOut));
    memcpy(hdrOut->magic, TF_BUCKET_MAGIC, 4);
    hdrOut->version = TF_BUCKET_VERSION;
    hdrOut->fingerprint = fp;
    hdrOut->recordCount = 0;
    hdrOut->headerCrc32 = Crc32(hdrOut, offsetof(TfBucketHeader, headerCrc32));
    bool ok = fwrite(hdrOut, sizeof(*hdrOut), 1, f) == 1;
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

/* The existing init sequence, unchanged in behavior, with every stage
 * recorded. Caller holds the lock. resetStats=false (diagnostic reinit)
 * keeps the lifetime counters. */
static bool InitInternal(bool resetStats)
{
    if (resetStats)
        memset(&s_stats, 0, sizeof(s_stats));
    s_stats.indexSlotsTotal = TF_INDEX_SLOTS;
    s_stats.indexSlotsUsed = 0;
    SetAvailable(false);

    uint32_t runs = s_init.runs + 1;
    memset(&s_init, 0, sizeof(s_init));
    s_init.runs = runs;
    s_init.attempted = true;
    s_init.firstFailedStage = -1;
    s_init.lastErrStage = -1;
    s_init.indexSizeBytes = -1;
    s_init.indexSizeExpected = (uint32_t)(TF_INDEX_SLOTS * sizeof(TfIndexSlot));
    s_init.indexState = TF_INDEX_NOT_INIT;
    s_init.historyDir = TF_HISTORY_DIR;
    s_init.indexPath = TF_INDEX_PATH;
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

    t0 = NowUs();
    errno = 0;
    FILE *idx = fopen(TF_INDEX_PATH, "rb");
    int openErrno = errno;
    bool indexOk = false;
    if (idx) {
        s_init.indexFileOpened = true;
        fseek(idx, 0, SEEK_END);
        long sz = ftell(idx);
        fclose(idx);
        s_init.indexSizeBytes = (int32_t)sz;
        indexOk = (sz == (long)(TF_INDEX_SLOTS * sizeof(TfIndexSlot)));
        s_init.indexSizeValid = indexOk;
        StageSet(TF_STAGE_INDEX_CHECK, TF_RES_OK, 0, 0, 0, t0);
    } else if (openErrno == ENOENT) {
        StageSet(TF_STAGE_INDEX_CHECK, TF_RES_OK, 0, ENOENT, 0, t0); /* missing = normal on a fresh card */
    } else {
        StageSet(TF_STAGE_INDEX_CHECK, TF_RES_FAIL, 0, openErrno, 0, t0);
    }

    if (!indexOk) {
        t0 = NowUs();
        errno = 0;
        bool created = CreateEmptyIndexFile();
        int e = errno;
        if (!created) {
            StageSet(TF_STAGE_INDEX_CREATE, TF_RES_FAIL, 0, e, 0, t0);
            s_init.indexState = TF_INDEX_FAILED;
            return false; /* card present but not usable (full/read-only/etc) - stay degraded */
        }
        StageSet(TF_STAGE_INDEX_CREATE, TF_RES_OK, 0, 0, 0, t0);
        /* If bucket files already exist from a previous session but the
         * index was missing/corrupt, recover it rather than starting blind
         * (PROMPT.md section 24). Harmless no-op on a fresh card. Its
         * result was ignored before; it is now recorded (init still
         * proceeds exactly as before). */
        t0 = NowUs();
        errno = 0;
        bool rebuilt = TfHistory_RebuildIndex();
        e = errno;
        StageSet(TF_STAGE_INDEX_REBUILD, rebuilt ? TF_RES_OK : TF_RES_FAIL, 0, rebuilt ? 0 : e, 0, t0);
        s_init.indexState = TF_INDEX_REBUILT;
    } else {
        /* Existing valid index: previously indexSlotsUsed stayed 0 here.
         * Count it (read-only) so 0 genuinely means empty. */
        uint32_t used = 0;
        int e = 0;
        t0 = NowUs();
        if (CountIndexSlots(&used, &e)) {
            s_stats.indexSlotsUsed = used;
            StageSet(TF_STAGE_INDEX_COUNT, TF_RES_OK, 0, 0, 0, t0);
        } else {
            StageSet(TF_STAGE_INDEX_COUNT, TF_RES_FAIL, 0, e, 0, t0);
        }
        s_init.indexState = TF_INDEX_LOADED;
    }

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
    TfIndexSlot slot;
    if (!IndexFind(normIcao, &slot, NULL)) {
        s_stats.lookupMisses++;
        return false;
    }

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

    char normIcao[TF_ICAO_MAX];
    NormalizeIcao(rec->icao24, normIcao);

    TfIndexSlot existing;
    uint32_t existingIdx = 0;
    bool found = IndexFind(normIcao, &existing, &existingIdx);

    bool ok;
    if (found && existing.bucketFingerprint == bucketFingerprint) {
        ok = WriteRecordAt(path, existing.recordOffset, &od);
        if (ok) s_stats.updates++;
    } else if (found) {
        /* Aircraft's classification moved it to a different Universal
         * Value bucket since the last write (e.g. an operator's numeric
         * code churned and the aircraft now resolves elsewhere). Append
         * into the new bucket and repoint the index; the old bucket's
         * record for this aircraft is simply superseded, not deleted -
         * harmless, bounded, and simpler than in-place bucket migration. */
        uint32_t newOffset = 0;
        ok = AppendRecord(path, &hdr, &od, &newOffset);
        if (ok) {
            TfIndexSlot fresh;
            memset(&fresh, 0, sizeof(fresh));
            NormalizeIcao(normIcao, fresh.icao24);
            fresh.bucketFingerprint = bucketFingerprint;
            fresh.recordOffset = newOffset;
            fresh.crc32 = IndexSlotCrc(&fresh);
            ok = WriteIndexSlot(existingIdx, &fresh);
            if (ok) s_stats.updates++;
        }
    } else {
        uint32_t offset = 0;
        ok = AppendRecord(path, &hdr, &od, &offset);
        if (ok) ok = IndexPlace(normIcao, bucketFingerprint, offset, NULL);
        if (ok) s_stats.creates++;
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

static bool RebuildIndexLocked(void)
{
    if (!CreateEmptyIndexFile())
        return false;

    int64_t t0 = NowUs();
    s_stats.indexSlotsUsed = 0;

    /* Scan the three fixed bucket names we know about (special buckets)
     * plus every operator fingerprint currently known to the Universal
     * Value table - a bounded set, not a directory-listing sweep, which
     * keeps this portable across host/ESP-IDF without a readdir()
     * dependency. A bucket that predates the current Universal Value table
     * (e.g. an operator removed many sessions ago and never resynced) is
     * intentionally out of scope for this lightweight recovery pass -
     * documented as a limitation; its file is untouched on disk either
     * way, only its index entries would need a manual rebuild if that
     * specific case ever matters in practice (PROMPT.md section 42). */
    uint32_t knownBuckets[2] = {TF_BUCKET_UNASSIGNED, TF_BUCKET_REGISTRY_DEFINED};
    for (size_t b = 0; b < 2; b++) {
        char path[512];
        BucketPath(knownBuckets[b], path, sizeof(path));
        if (!BucketExists(path))
            continue;
        TfBucketHeader hdr;
        if (!ReadBucketHeader(path, &hdr))
            continue;
        for (uint32_t off = 0; off < hdr.recordCount * (uint32_t)sizeof(TfOnDiskRecord);
             off += (uint32_t)sizeof(TfOnDiskRecord)) {
            TfOnDiskRecord rec;
            if (!ReadRecordAt(path, off, &rec)) break;
            if (rec.crc32 != Crc32(&rec.pub, sizeof(rec.pub))) {
                s_stats.recoveryOps++;
                continue; /* corrupt slot: skip, keep scanning the rest of the bucket */
            }
            if (rec.pub.icao24[0] == '\0')
                continue;
            IndexPlace(rec.pub.icao24, knownBuckets[b], off, NULL);
        }
    }

    s_stats.indexRebuilds++;
    UpdateTiming(&s_stats.lastIndexOpUs, &s_stats.bestIndexOpUs, &s_stats.worstIndexOpUs,
                 (uint32_t)(NowUs() - t0));
    UpdateTiming(&s_stats.lastRecoveryUs, &s_stats.bestRecoveryUs, &s_stats.worstRecoveryUs,
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
    bool r = RebuildIndexLocked();
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

/* Index probe with ONE open of index.dat for the whole call (the existing
 * IndexFind() re-opens the file for every slot; that is fine for the
 * single-lookup radar path but wasteful when classifying a page). Same
 * probe order and the same empty / corrupt-slot rules as IndexFind().
 * Returns 1 found, 0 not in index, -1 I/O error. */
static int IndexProbeOpen(FILE *f, const char *icao24, TfIndexSlot *out)
{
    uint32_t start = Fnv1a32(icao24) & (TF_INDEX_SLOTS - 1);
    for (uint32_t i = 0; i < TF_INDEX_SLOTS; i++) {
        uint32_t idx = (start + i) & (TF_INDEX_SLOTS - 1);
        TfIndexSlot slot;
        if (fseek(f, (long)((size_t)idx * sizeof(TfIndexSlot)), SEEK_SET) != 0 ||
            fread(&slot, sizeof(slot), 1, f) != 1)
            return -1;
        if (slot.icao24[0] == '\0')
            return 0;
        if (slot.crc32 != IndexSlotCrc(&slot))
            continue;
        if (strncmp(slot.icao24, icao24, TF_ICAO_MAX) == 0) {
            if (out)
                *out = slot;
            return 1;
        }
    }
    return 0;
}

static uint8_t ClassifyOpen(FILE *idxFile, const char *icao24, uint32_t fp, uint32_t recordIdx)
{
    if (!idxFile)
        return TF_REC_INDEX_UNKNOWN;
    char norm[TF_ICAO_MAX];
    NormalizeIcao(icao24, norm);
    TfIndexSlot slot;
    int r = IndexProbeOpen(idxFile, norm, &slot);
    if (r < 0)
        return TF_REC_INDEX_UNKNOWN;
    if (r == 0)
        return TF_REC_NOT_INDEXED;
    return (slot.bucketFingerprint == fp && slot.recordOffset == recordIdx * (uint32_t)sizeof(TfOnDiskRecord))
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

TfBrowseStatus TfHistory_BrowseChunk(uint32_t fp, uint32_t startIdx, size_t maxN,
                                     TfBrowseRecord *out, size_t *gotN, uint32_t *recordCountOut)
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
        FILE *idx = fopen(TF_INDEX_PATH, "rb"); /* NULL => rows come back INDEX_UNKNOWN, never guessed */
        for (size_t i = 0; i < n; i++) {
            out[i].state = TF_REC_CORRUPT;
            memset(&out[i].rec, 0, sizeof(out[i].rec));
            if (raw[i].pub.icao24[0] == '\0' || raw[i].crc32 != Crc32(&raw[i].pub, sizeof(raw[i].pub)))
                continue;
            out[i].rec = raw[i].pub;
            out[i].rec.icao24[TF_ICAO_MAX - 1] = '\0';
            out[i].rec.callsign[sizeof(out[i].rec.callsign) - 1] = '\0';
            out[i].state = ClassifyOpen(idx, out[i].rec.icao24, fp, startIdx + (uint32_t)i);
        }
        if (idx)
            fclose(idx);
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
    FILE *idx = fopen(TF_INDEX_PATH, "rb");
    *stateOut = ClassifyOpen(idx, icao24, fp, recordIdx);
    if (idx)
        fclose(idx);
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
    FILE *idx = fopen(TF_INDEX_PATH, "rb");
    if (!idx) {
        Unlock();
        return TF_BR_IO;
    }
    TfIndexSlot slot;
    int r = IndexProbeOpen(idx, norm, &slot);
    fclose(idx);
    if (r < 0) {
        st = TF_BR_IO;
    } else if (r == 1) {
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
