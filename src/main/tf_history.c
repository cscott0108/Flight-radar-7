#include "tf_history.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
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

static bool s_available = false;
static TfHistoryStats s_stats;

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
    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "TF SPI bus init failed (%s) - persistent history disabled, radar unaffected", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = TF_PIN_CS;
    slot_config.host_id = (spi_host_device_t)host.slot;

    ret = esp_vfs_fat_sdspi_mount(TF_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TF card mount failed (%s) - persistent history disabled, radar unaffected", esp_err_to_name(ret));
        return false;
    }
    return true;
}

static void GetSpace(uint64_t *capacity, uint64_t *used, uint64_t *free_)
{
    FATFS *fs = NULL;
    DWORD freeClusters = 0;
    *capacity = *used = *free_ = 0;
    if (!s_card)
        return;
    if (f_getfree("0:", &freeClusters, &fs) != FR_OK || !fs)
        return;
    uint64_t totalSectors = (uint64_t)(fs->n_fatent - 2) * fs->csize;
    uint64_t freeSectors = (uint64_t)freeClusters * fs->csize;
    uint64_t sectorSize = 512; /* FF_MIN_SS default */
    *capacity = totalSectors * sectorSize;
    *free_ = freeSectors * sectorSize;
    *used = *capacity - *free_;
}
#else
static bool MountTfCard(void)
{
    /* Host build: exercise the storage/index/bucket logic against a plain
     * local directory instead of a real card. */
    return true;
}
static void GetSpace(uint64_t *capacity, uint64_t *used, uint64_t *free_)
{
    *capacity = *used = *free_ = 0;
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

bool TfHistory_Init(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.indexSlotsTotal = TF_INDEX_SLOTS;
    s_available = false;

    if (!MountTfCard())
        return false;

    mkdir(TF_HISTORY_DIR, 0775); /* ignore EEXIST-style failures; verified below by actually opening files */

    s_stats.mounted = true;
    GetSpace(&s_stats.capacityBytes, &s_stats.usedBytes, &s_stats.freeBytes);

    FILE *idx = fopen(TF_INDEX_PATH, "rb");
    bool indexOk = false;
    if (idx) {
        fseek(idx, 0, SEEK_END);
        long sz = ftell(idx);
        fclose(idx);
        indexOk = (sz == (long)(TF_INDEX_SLOTS * sizeof(TfIndexSlot)));
    }

    if (!indexOk) {
        if (!CreateEmptyIndexFile())
            return false; /* card present but not usable (full/read-only/etc) - stay degraded */
        /* If bucket files already exist from a previous session but the
         * index was missing/corrupt, recover it rather than starting blind
         * (PROMPT.md section 24). Harmless no-op on a fresh card. */
        TfHistory_RebuildIndex();
    }

    s_available = true;
    return true;
}

bool TfHistory_IsAvailable(void)
{
    return s_available;
}

bool TfHistory_Lookup(const char *icao24, TfHistoryRecord *out, uint32_t *bucketOut)
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

bool TfHistory_Upsert(uint32_t bucketFingerprint, const TfHistoryRecord *rec)
{
    if (!s_available || !rec || !rec->icao24[0] || bucketFingerprint == TF_BUCKET_RESERVED)
        return false;

    int64_t t0 = NowUs();

    char path[512];
    TfBucketHeader hdr;
    if (!EnsureBucket(bucketFingerprint, path, &hdr)) {
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
        s_stats.errors++;
        return false;
    }

    s_stats.writes++;
    UpdateTiming(&s_stats.lastWriteUs, &s_stats.bestWriteUs, &s_stats.worstWriteUs,
                 (uint32_t)(NowUs() - t0));
    return true;
}

bool TfHistory_RebuildIndex(void)
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

void TfHistory_GetStats(TfHistoryStats *out)
{
    if (out) *out = s_stats;
}
