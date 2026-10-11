// 0.1.8: console-log capture, platform-independent part. See log_capture_core.h.
#include "log_capture_core.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ================= ring ================= */

void LogRing_Init(LogRing *r, uint8_t *buf, uint32_t size)
{
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->size = size;
}

uint32_t LogRing_Used(const LogRing *r)
{
    const uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    const uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    return h - t;
}

bool LogRing_Put(LogRing *r, const char *data, uint32_t len)
{
    const uint32_t h = r->head; /* only the (serialized) producer writes head */
    const uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    const uint32_t used = h - t;
    if (len > r->size - used)
        return false;
    const uint32_t at = h % r->size;
    const uint32_t first = (len < r->size - at) ? len : r->size - at;
    memcpy(r->buf + at, data, first);
    if (len > first)
        memcpy(r->buf, data + first, len - first);
    __atomic_store_n(&r->head, h + len, __ATOMIC_RELEASE);
    if (used + len > r->highWater)
        r->highWater = used + len;
    return true;
}

uint32_t LogRing_Peek(const LogRing *r, char *out, uint32_t max)
{
    const uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    const uint32_t t = r->tail; /* only the consumer writes tail */
    uint32_t n = h - t;
    if (n > max)
        n = max;
    const uint32_t at = t % r->size;
    const uint32_t first = (n < r->size - at) ? n : r->size - at;
    memcpy(out, r->buf + at, first);
    if (n > first)
        memcpy(out + first, r->buf, n - first);
    /* keep whole lines together when possible (a full batch with no newline is taken as is) */
    if (n == max) {
        uint32_t cut = n;
        while (cut > 0 && out[cut - 1] != '\n')
            cut--;
        if (cut > 0)
            n = cut;
    }
    return n;
}

void LogRing_Consume(LogRing *r, uint32_t n)
{
    __atomic_store_n(&r->tail, r->tail + n, __ATOMIC_RELEASE);
}

/* ================= redaction ================= */

static char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static bool IsWordChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

static bool MatchCI(const char *s, const char *end, const char *key)
{
    for (; *key; key++, s++)
        if (s >= end || Lower(*s) != *key)
            return false;
    return true;
}

static bool MatchCS(const char *s, const char *end, const char *key)
{
    size_t n = strlen(key);
    return (size_t)(end - s) >= n && memcmp(s, key, n) == 0;
}

/* Mask with '*' from p until a stop character (not masked); returns chars masked. */
static size_t MaskUntil(char *p, char *end, const char *stops)
{
    size_t n = 0;
    for (; p < end && *p != '\n' && *p != '\r' && !strchr(stops, *p); p++, n++)
        *p = '*';
    return n;
}

/* A coordinate-like number at p: keep sign, integer part and the first decimal; mask the other decimals with
 * '#'. Returns the end of the number. */
static char *MaskCoord(char *p, char *end, uint32_t *hits)
{
    if (p < end && (*p == '-' || *p == '+'))
        p++;
    while (p < end && IsDigit(*p))
        p++;
    if (p < end && *p == '.') {
        p++;
        if (p < end && IsDigit(*p))
            p++;
        bool masked = false;
        while (p < end && IsDigit(*p)) {
            *p++ = '#';
            masked = true;
        }
        if (masked)
            (*hits)++;
    }
    return p;
}

uint32_t LogCap_Redact(char *s, size_t len)
{
    static const char *const kSecret[] = {"access_token", "refresh_token", "client_secret", "client_id", "password", "passwd",
                                          "authorization", "api_key", "apikey", "secret", "token"};
    static const char *const kCoord[] = {"latitude=", "longitude=", "lat=", "lon=", "lng=", "lamin=", "lamax=", "lomin=", "lomax="};
    uint32_t hits = 0;
    char *end = s + len;
    for (char *p = s; p < end; p++) {
        const bool boundary = (p == s) || !IsWordChar(p[-1]);
        /* credentials: key [quote] [spaces] (:|=) [spaces] [quote] value */
        if (boundary) {
            for (size_t k = 0; k < sizeof(kSecret) / sizeof(kSecret[0]); k++) {
                const size_t kl = strlen(kSecret[k]);
                if (!MatchCI(p, end, kSecret[k]) || (p + kl < end && IsWordChar(p[kl])))
                    continue;
                char *v = p + kl;
                if (v < end && *v == '"')
                    v++;
                while (v < end && *v == ' ')
                    v++;
                if (v >= end || (*v != ':' && *v != '='))
                    continue;
                v++;
                while (v < end && *v == ' ')
                    v++;
                const bool quoted = v < end && *v == '"';
                if (quoted)
                    v++;
                /* "Authorization: Bearer <token>" has a space inside its value: mask to the end of the line */
                const bool wholeLine = !quoted && strcmp(kSecret[k], "authorization") == 0;
                if (MaskUntil(v, end, quoted ? "\"" : wholeLine ? "\"" : " &,;})\"") > 0)
                    hits++;
                p = v;
                break;
            }
            if (MatchCI(p, end, "bearer ")) {
                if (MaskUntil(p + 7, end, " \"',;") > 0)
                    hits++;
                continue;
            }
            /* coordinates */
            for (size_t k = 0; k < sizeof(kCoord) / sizeof(kCoord[0]); k++) {
                if (MatchCI(p, end, kCoord[k])) {
                    char *v = p + strlen(kCoord[k]);
                    while (v < end && *v == ' ')
                        v++;
                    p = MaskCoord(v, end, &hits) - 1;
                    break;
                }
            }
            /* BSSID: bssid [spaces] (=|:) [spaces] MAC */
            if (p >= s && p < end && MatchCI(p, end, "bssid")) {
                char *v = p + 5;
                while (v < end && *v == ' ')
                    v++;
                if (v < end && (*v == '=' || *v == ':')) {
                    v++;
                    while (v < end && *v == ' ')
                        v++;
                    bool any = false;
                    for (; v < end && (IsDigit(*v) || (Lower(*v) >= 'a' && Lower(*v) <= 'f') || *v == ':'); v++)
                        if (*v != ':') {
                            *v = '*';
                            any = true;
                        }
                    if (any)
                        hits++;
                    p = v - 1;
                }
                continue;
            }
            /* ssid=/ssid: values */
            if (MatchCI(p, end, "ssid") && p + 4 < end && (p[4] == '=' || p[4] == ':')) {
                char *v = p + 5;
                while (v < end && *v == ' ')
                    v++;
                if (MaskUntil(v, end, ",;)") > 0)
                    hits++;
                continue;
            }
        }
        /* adsb.lol URL: /point/<lat>/<lon>/<radius> */
        if (MatchCS(p, end, "/point/")) {
            char *v = MaskCoord(p + 7, end, &hits);
            if (v < end && *v == '/')
                v = MaskCoord(v + 1, end, &hits);
            p = v - 1;
            continue;
        }
        /* SSIDs in known log lines (ESP-IDF Wi-Fi driver and main.c) */
        if (MatchCS(p, end, "connected with ")) {
            if (MaskUntil(p + 15, end, ",") > 0)
                hits++;
            continue;
        }
        if (MatchCS(p, end, "saved network ")) {
            char *v = p + 14;
            while (v < end && IsDigit(*v))
                v++;
            if (MatchCS(v, end, " (") && MaskUntil(v + 2, end, ")") > 0)
                hits++;
            continue;
        }
        if (MatchCS(p, end, "AP started: ")) {
            char *v = p + 12;
            char *stop = v;
            while (stop < end && *stop != '\n' && !MatchCS(stop, end, " (channel"))
                stop++;
            if (stop > v) {
                memset(v, '*', (size_t)(stop - v));
                hits++;
            }
            continue;
        }
    }
    return hits;
}

size_t LogCap_FormatRecord(char *out, size_t cap, const char *fmt, va_list ap, bool *truncated, uint32_t *redactions)
{
    static const char kTrunc[] = "...[truncated]\n";
    *truncated = false;
    *redactions = 0;
    if (!out || cap < sizeof(kTrunc) + 1 || !fmt)
        return 0;
    const size_t lim = cap - 1 < LOGCAP_LINE_MAX ? cap - 1 : LOGCAP_LINE_MAX;
    int n = vsnprintf(out, lim + 1, fmt, ap);
    if (n <= 0)
        return 0;
    size_t len = (size_t)n;
    if (len > lim) {
        len = lim - (sizeof(kTrunc) - 1);
        memcpy(out + len, kTrunc, sizeof(kTrunc) - 1);
        len = lim;
        out[len] = 0;
        *truncated = true;
    }
    *redactions = LogCap_Redact(out, len);
    return len;
}

/* ================= names ================= */

bool LogCap_ValidName(const char *name)
{
    if (!name || strlen(name) != 12 || name[0] != 'L' || strcmp(name + 8, ".LOG") != 0)
        return false;
    for (int i = 1; i < 8; i++)
        if (!IsDigit(name[i]))
            return false;
    return true;
}

bool LogCap_ParseName(const char *name, uint32_t *boot, uint32_t *seg)
{
    if (!LogCap_ValidName(name))
        return false;
    uint32_t b = 0, s = 0;
    for (int i = 1; i < 6; i++)
        b = b * 10u + (uint32_t)(name[i] - '0');
    for (int i = 6; i < 8; i++)
        s = s * 10u + (uint32_t)(name[i] - '0');
    if (b == 0 || s == 0 || s > LOGCAP_SEG_MAX)
        return false;
    if (boot)
        *boot = b;
    if (seg)
        *seg = s;
    return true;
}

void LogCap_MakeName(uint32_t boot, uint32_t seg, char out[LOGCAP_NAME_LEN])
{
    snprintf(out, LOGCAP_NAME_LEN, "L%05u%02u.LOG", (unsigned)(boot % LOGCAP_BOOT_MOD), (unsigned)(seg % 100u));
}

uint32_t LogCap_Age(uint32_t current, uint32_t boot)
{
    return (current + LOGCAP_BOOT_MOD - (boot % LOGCAP_BOOT_MOD)) % LOGCAP_BOOT_MOD;
}

uint32_t LogCap_NextBoot(uint32_t boot)
{
    boot = (boot + 1u) % LOGCAP_BOOT_MOD;
    return boot ? boot : 1u;
}

/* ================= directory ================= */

size_t LogCap_List(const char *dir, LogFileInfo *out, size_t max, size_t *total)
{
    size_t n = 0, all = 0;
    DIR *d = opendir(dir);
    if (!d) {
        if (total)
            *total = 0;
        return 0;
    }
    struct dirent *e;
    char path[96];
    while ((e = readdir(d)) != NULL) {
        if (!LogCap_ParseName(e->d_name, NULL, NULL))
            continue;
        all++;
        if (n >= max)
            continue;
        LogFileInfo *f = &out[n++];
        memcpy(f->name, e->d_name, LOGCAP_NAME_LEN); /* validated: 12 chars + NUL */
        snprintf(path, sizeof(path), "%.80s/%.12s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0) {
            f->size = (uint32_t)st.st_size;
            f->mtime = (int64_t)st.st_mtime;
        } else {
            f->size = 0;
            f->mtime = 0;
        }
    }
    closedir(d);
    if (total)
        *total = all;
    return n;
}

typedef struct {
    uint32_t count;
    uint64_t total;
    bool haveOldest;
    char oldest[LOGCAP_NAME_LEN];
    uint32_t oldestAge, oldestSeg;
    bool havePrev;
    char newestPrev[LOGCAP_NAME_LEN];
    uint32_t prevAge, prevSeg;
    bool bootUsed;
} DirScan;

static bool Scan(const LogWriter *w, const char *dir, uint32_t boot, DirScan *s)
{
    memset(s, 0, sizeof(*s));
    DIR *d = opendir(dir);
    if (!d)
        return false;
    struct dirent *e;
    char path[96];
    while ((e = readdir(d)) != NULL) {
        uint32_t b, sg;
        if (!LogCap_ParseName(e->d_name, &b, &sg))
            continue;
        snprintf(path, sizeof(path), "%.80s/%.12s", dir, e->d_name);
        struct stat st;
        const uint64_t size = stat(path, &st) == 0 ? (uint64_t)st.st_size : 0;
        s->count++;
        s->total += size;
        const uint32_t age = LogCap_Age(boot, b);
        if (b == boot)
            s->bootUsed = true;
        if (age > 0 && (!s->havePrev || age < s->prevAge || (age == s->prevAge && sg > s->prevSeg))) {
            s->havePrev = true;
            s->prevAge = age;
            s->prevSeg = sg;
            memcpy(s->newestPrev, e->d_name, LOGCAP_NAME_LEN);
        }
        const bool active = w->haveFile && strcmp(e->d_name, w->file) == 0;
        const bool pinned = w->pinned && w->pinned[0] && strcmp(e->d_name, w->pinned) == 0;
        if (active || pinned)
            continue;
        if (!s->haveOldest || age > s->oldestAge || (age == s->oldestAge && sg < s->oldestSeg)) {
            s->haveOldest = true;
            s->oldestAge = age;
            s->oldestSeg = sg;
            memcpy(s->oldest, e->d_name, LOGCAP_NAME_LEN);
        }
    }
    closedir(d);
    return true;
}

/* ================= writer ================= */

const char *LogWriterState_Name(LogWriterState s)
{
    switch (s) {
    case LW_NOT_STARTED: return "starting (nothing written yet)";
    case LW_WRITING: return "writing";
    case LW_NO_CARD: return "waiting for the TF card";
    case LW_BACKOFF: return "write error, retrying";
    case LW_SUSPENDED: return "suspended";
    case LW_SEGMENT_LIMIT: return "stopped (segment limit for this boot)";
    case LW_ENDED: return "ended (clean shutdown)";
    case LW_DEFERRED: return "deferred (internal DMA memory low)";
    default: return "?";
    }
}

void LogWriter_Init(LogWriter *w, uint32_t boot)
{
    memset(w, 0, sizeof(*w));
    w->boot = boot ? boot % LOGCAP_BOOT_MOD : 1u;
    if (!w->boot)
        w->boot = 1;
    w->state = LW_NOT_STARTED;
}

static bool Reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

static void TimeText(const LogWriterEnv *env, int64_t utc, char *out, size_t cap)
{
    out[0] = 0;
    if (utc > 0 && env->localTime)
        env->localTime(env->ctx, utc, out, cap);
    if (!out[0])
        snprintf(out, cap, "clock not synchronized");
}

static void Fail(LogWriter *w, uint32_t now, const char *what, int err)
{
    w->writeErrors++;
    w->lastErrno = err;
    w->lastErrorUptimeS = now;
    snprintf(w->lastError, sizeof(w->lastError), "%s (errno %d)", what, err);
    snprintf(w->reason, sizeof(w->reason), "%s; retry in %u s", what, (unsigned)LOGCAP_RETRY_S);
    w->state = LW_BACKOFF;
    w->retryAtS = now + LOGCAP_RETRY_S;
    w->retryNote = true;
}

static void Suspend(LogWriter *w, uint32_t now, const char *why)
{
    snprintf(w->reason, sizeof(w->reason), "%s; re-checked every %u s", why, (unsigned)LOGCAP_SPACE_RECHECK_S);
    w->state = LW_SUSPENDED;
    w->retryAtS = now + LOGCAP_SPACE_RECHECK_S;
}

/* fopen/fwrite/fclose; 0 = ok, 1 = open/short write (nothing reliable written), 2 = close failed after a full write */
static int Append(const char *path, const char *data, size_t len, int *err)
{
    errno = 0;
    FILE *f = fopen(path, "ab");
    if (!f) {
        *err = errno;
        return 1;
    }
    size_t wr = len ? fwrite(data, 1, len, f) : 0;
    int e = errno;
    int rc = fclose(f);
    if (wr != len) {
        *err = e ? e : EIO;
        return 1;
    }
    if (rc != 0) {
        *err = errno ? errno : EIO;
        return 2;
    }
    return 0;
}

static void PathOf(const LogWriterEnv *env, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%.80s/%.12s", env->dir, name);
}

/* Free-space floor (+ room for one more segment when about to create one). */
static bool SpaceOk(LogWriter *w, const LogWriterEnv *env, uint32_t now, bool newSegment)
{
    uint64_t f = 0;
    if (!env->freeBytes || !env->freeBytes(env->ctx, &f)) {
        Suspend(w, now, "TF free space could not be read");
        w->spaceSuspends++;
        return false;
    }
    w->lastFreeBytes = f;
    w->haveFreeBytes = true;
    if (f < LOGCAP_FREE_MIN + (newSegment ? LOGCAP_SEGMENT_MAX : 0u)) {
        Suspend(w, now, "TF card free space is below the 64 MiB floor");
        w->spaceSuspends++;
        return false;
    }
    return true;
}

/* Keep <= 64 files and <= 32 MiB including one more 1 MiB segment: delete the oldest completed log files
 * (never the active file or a file being downloaded). */
static bool MakeRoom(LogWriter *w, const LogWriterEnv *env, uint32_t now)
{
    char path[96];
    for (int guard = 0; guard < 2 * (int)LOGCAP_FILES_MAX + 16; guard++) {
        DirScan s;
        if (!Scan(w, env->dir, w->boot, &s)) {
            Fail(w, now, "cannot read the log directory", errno);
            return false;
        }
        if (s.count + 1u <= LOGCAP_FILES_MAX && s.total + LOGCAP_SEGMENT_MAX <= LOGCAP_TOTAL_MAX)
            return true;
        if (!s.haveOldest) {
            w->retentionFailures++;
            Suspend(w, now, "log retention limits reached and no log file can be deleted (active or being downloaded)");
            return false;
        }
        PathOf(env, s.oldest, path, sizeof(path));
        if (remove(path) != 0) {
            w->retentionFailures++;
            Fail(w, now, "could not delete the oldest log file", errno);
            return false;
        }
        w->filesDeleted++;
    }
    w->retentionFailures++;
    Suspend(w, now, "log retention did not converge");
    return false;
}

static bool CreateSegment(LogWriter *w, const LogWriterEnv *env, uint32_t now, int64_t utc)
{
    if (!MakeRoom(w, env, now) || !SpaceOk(w, env, now, true))
        return false;
    LogCap_MakeName(w->boot, w->seg, w->file);
    char path[96], when[48];
    char *const hdr = w->scratch;
    const size_t hdrCap = sizeof(w->scratch);
    PathOf(env, w->file, path, sizeof(path));
    TimeText(env, utc, when, sizeof(when));
    int n = snprintf(hdr, hdrCap,
                     "=== FR7LOG v1 file=%s boot=%u segment=%u firmware=%s reset=%s uptime=%us time=%s ===\n"
                     "=== Records: ESP-IDF log lines \"<level> (<ms since start>) <tag>: <message>\". Redacted: SSID/BSSID, "
                     "coordinates (to 0.1 deg), credentials. Not captured: bootloader/early boot, panic output, "
                     "esp_rom_printf (FAILED_ALLOC detail), LVGL printf ===\n",
                     w->file, (unsigned)w->boot, (unsigned)w->seg, env->firmware ? env->firmware : "?",
                     env->resetReason ? env->resetReason : "?", (unsigned)now, when);
    if (n < 0 || n >= (int)hdrCap)
        n = (int)strlen(hdr);
    int err = 0;
    const int rc = Append(path, hdr, (size_t)n, &err);
    if (rc == 1) {
        Fail(w, now, "cannot create the log file", err);
        return false;
    }
    struct stat st;
    w->fileSize = stat(path, &st) == 0 ? (uint64_t)st.st_size : (uint64_t)n;
    w->haveFile = true;
    w->segEndWritten = false;
    w->filesCreated++;
    w->bytesWritten += (uint64_t)n;
    if (rc == 2) {
        w->closeErrors++;
        Fail(w, now, "closing the new log file failed", err);
        return false;
    }
    return true;
}

/* First write of the boot: directory, a boot number not used by any existing file, the previous session's
 * last file marked when it has no clean end, then segment 1. */
static bool StartSession(LogWriter *w, const LogWriterEnv *env, uint32_t now, int64_t utc)
{
    errno = 0;
    struct stat st;
    if (stat(env->dir, &st) != 0 && mkdir(env->dir, 0775) != 0) {
        Fail(w, now, "cannot create the log directory", errno);
        return false;
    }
    DirScan s;
    if (!Scan(w, env->dir, w->boot, &s)) {
        Fail(w, now, "cannot read the log directory", errno);
        return false;
    }
    for (int i = 0; i < 100 && s.bootUsed; i++) { /* NVS was reset, or the numbers wrapped */
        w->boot = LogCap_NextBoot(w->boot);
        if (!Scan(w, env->dir, w->boot, &s)) {
            Fail(w, now, "cannot read the log directory", errno);
            return false;
        }
    }
    if (env->saveBoot)
        env->saveBoot(env->ctx, w->boot);
    if (s.havePrev) {
        char path[96];
        char *const tail = w->scratch; /* 200 bytes of it; reused for the marker text below */
        const size_t tailCap = 200;
        PathOf(env, s.newestPrev, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (f) {
            size_t got = 0;
            if (fseek(f, 0, SEEK_END) == 0) {
                long sz = ftell(f);
                long from = sz > (long)tailCap - 1 ? sz - (long)tailCap + 1 : 0;
                if (fseek(f, from, SEEK_SET) == 0)
                    got = fread(tail, 1, tailCap - 1, f);
            }
            fclose(f);
            tail[got] = 0;
            if (!strstr(tail, "=== CAPTURE END") && !strstr(tail, "=== INTERRUPTED")) {
                char *const note = w->scratch; /* the tail is no longer needed */
                int n = snprintf(note, sizeof(w->scratch),
                                 "\n=== INTERRUPTED: no clean end marker (power loss, crash, watchdog or a reset that did not "
                                 "go through the WebUI reboot); noticed at boot %u ===\n", (unsigned)w->boot);
                int err = 0;
                if (n > 0 && Append(path, note, (size_t)n, &err) == 0)
                    w->interruptedMarked++;
            }
        }
    }
    w->seg = 1;
    if (!CreateSegment(w, env, now, utc))
        return false;
    w->nextMarkS = now + LOGCAP_MARK_INTERVAL_S;
    return true;
}

typedef struct {
    uint32_t dropFull, dropBusy, dropContext, dropLock, fails;
    uint32_t dropFullBytes;
    bool mark, sync;
} Pending;

static size_t Markers(const LogWriter *w, const LogWriterEnv *env, char *out, size_t cap, uint32_t now, int64_t utc,
                      const LogCapCounters *c, Pending *p)
{
    size_t n = 0;
    char when[48];
    memset(p, 0, sizeof(*p));
#define ADD(...) do { int k_ = snprintf(out + n, cap - n, __VA_ARGS__); if (k_ > 0 && (size_t)k_ < cap - n) n += (size_t)k_; } while (0)
    if (w->retryNote)
        ADD("=== WRITE RETRY after: %s; lines around this point may be missing or repeated ===\n", w->lastError);
    if (w->deferNote)
        ADD("=== WRITE DEFERRED: %u write cycle(s) over %u s while the largest free internal DMA block was below %u bytes; "
            "lines were kept in the buffer (any overflow is counted under DROPPED) ===\n",
            (unsigned)w->deferNoteCycles, (unsigned)w->deferNoteSecs, (unsigned)LOGCAP_DMA_MIN_BYTES);
    if (c) {
        p->dropFull = c->dropFullLines;
        p->dropFullBytes = c->dropFullBytes;
        p->dropBusy = c->dropBusyLines;
        p->dropContext = c->dropContextLines;
        p->dropLock = c->dropLockLines;
        p->fails = c->failedAllocs;
        if (p->dropFull != w->reportedDropFull || p->dropBusy != w->reportedDropBusy || p->dropContext != w->reportedDropContext ||
            p->dropLock != w->reportedDropLock)
            ADD("=== DROPPED since the last write: %u lines (%u bytes) buffer full, %u lines logger busy, %u lines unsafe context, "
                "%u lines logged while the TF storage lock was held ===\n",
                (unsigned)(p->dropFull - w->reportedDropFull), (unsigned)(p->dropFullBytes - w->reportedDropFullBytes),
                (unsigned)(p->dropBusy - w->reportedDropBusy), (unsigned)(p->dropContext - w->reportedDropContext),
                (unsigned)(p->dropLock - w->reportedDropLock));
        if (p->fails != w->reportedFails) {
            if (c->failTimeValid)
                ADD("=== FAILED_ALLOC count=%u (+%u since last report); last: %u bytes caps 0x%08x task %s at uptime %us "
                    "(details are on the serial console only) ===\n",
                    (unsigned)p->fails, (unsigned)(p->fails - w->reportedFails), (unsigned)c->failSize, (unsigned)c->failCaps,
                    c->failTask, (unsigned)c->failUptimeS);
            else
                ADD("=== FAILED_ALLOC count=%u (+%u since last report); last: %u bytes caps 0x%08x task %s, time unavailable ===\n",
                    (unsigned)p->fails, (unsigned)(p->fails - w->reportedFails), (unsigned)c->failSize, (unsigned)c->failCaps, c->failTask);
        }
    }
    if (utc > 0 && !w->syncMarked) {
        TimeText(env, utc, when, sizeof(when));
        ADD("=== CLOCK SYNCHRONIZED uptime=%us time=%s ===\n", (unsigned)now, when);
        p->sync = true;
    }
    if (w->started && Reached(now, w->nextMarkS)) { /* 0.1.8: started, not a state (DEFERRED/NO_CARD can precede the first file) */
        TimeText(env, utc, when, sizeof(when));
        ADD("=== MARK uptime=%us time=%s ===\n", (unsigned)now, when);
        p->mark = true;
    }
#undef ADD
    return n;
}

bool LogWriter_Cycle(LogWriter *w, const LogWriterEnv *env, LogRing *ring, char *batch, uint32_t nowS, int64_t utc,
                     const LogCapCounters *ctr, bool shutdown, const char *shutdownReason)
{
    if (w->state == LW_ENDED || w->state == LW_SEGMENT_LIMIT)
        return false;
    if ((w->state == LW_BACKOFF || w->state == LW_SUSPENDED) && !Reached(nowS, w->retryAtS))
        return false;
    if (!env->mounted(env->ctx)) {
        if (w->state != LW_NO_CARD) {
            w->state = LW_NO_CARD;
            snprintf(w->reason, sizeof(w->reason), "TF card not mounted; capture continues into the buffer (drops are counted)");
        }
        return false;
    }
    /* 0.1.8: DMA safeguard. One heap query per cycle; nothing is retried here (the caller waits for its next wake),
     * so a long shortage only accumulates buffered lines (bounded ring, overflow counted) and the deferral counters. */
    if (env->dmaLargest) {
        const uint32_t dma = env->dmaLargest(env->ctx);
        if (dma < LOGCAP_DMA_MIN_BYTES) {
            if (!w->deferring) {
                w->deferring = true;
                w->deferStartS = nowS;
                w->deferEpisodeCycles = 0;
                w->deferEpisodes++;
            }
            w->deferredCycles++;
            w->deferEpisodeCycles++;
            w->lastDeferDmaBytes = dma;
            if (w->state != LW_BACKOFF && w->state != LW_SUSPENDED && w->state != LW_NO_CARD)
                w->state = LW_DEFERRED;
            snprintf(w->reason, sizeof(w->reason), "since uptime %us: largest internal DMA block %u B < %u B",
                     (unsigned)w->deferStartS, (unsigned)dma, (unsigned)LOGCAP_DMA_MIN_BYTES);
            return false;
        }
        if (w->deferring) {
            const uint32_t secs = nowS - w->deferStartS;
            w->deferring = false;
            if (secs > w->longestDeferS)
                w->longestDeferS = secs;
            /* accumulate until a marker is actually written */
            w->deferNoteCycles += w->deferEpisodeCycles;
            w->deferNoteSecs += secs;
            w->deferNote = true;
            if (w->state == LW_DEFERRED) {
                w->state = w->started ? LW_WRITING : LW_NOT_STARTED;
                w->reason[0] = 0;
            }
        }
    }

    Pending p;
    const size_t m = Markers(w, env, batch, LOGCAP_MARK_MAX, nowS, utc, ctr, &p);
    const uint32_t n = LogRing_Peek(ring, batch + m, LOGCAP_BATCH_MAX);
    const bool last = shutdown && n == LogRing_Used(ring);
    size_t endLen = 0;
    if (last) {
        char when[48];
        TimeText(env, utc, when, sizeof(when));
        int k = snprintf(batch + m + n, LOGCAP_MARK_MAX, "=== CAPTURE END: clean shutdown (%s) uptime=%us time=%s ===\n",
                         shutdownReason ? shutdownReason : "requested", (unsigned)nowS, when);
        if (k > 0 && k < (int)LOGCAP_MARK_MAX)
            endLen = (size_t)k;
    }
    const size_t total = m + n + endLen; /* only n bytes come from (and are consumed from) the ring */
    if (total == 0)
        return false;

    if (!env->lock(env->ctx, LOGCAP_LOCK_MS)) {
        w->lockTimeouts++;
        return false;
    }
    bool ok = false, closeFailed = false;
    do {
        if (!env->mounted(env->ctx)) { /* unmounted while we waited for the lock */
            w->state = LW_NO_CARD;
            snprintf(w->reason, sizeof(w->reason), "TF card not mounted; capture continues into the buffer (drops are counted)");
            break;
        }
        if (!w->started) {
            if (!StartSession(w, env, nowS, utc))
                break;
            w->started = true;
        } else if (!w->haveFile) {
            if (!CreateSegment(w, env, nowS, utc))
                break;
        } else if (w->state == LW_SUSPENDED) {
            if (!SpaceOk(w, env, nowS, false))
                break;
        }
        if (w->fileSize + total > LOGCAP_SEGMENT_MAX) {
            char next[LOGCAP_NAME_LEN], path[96], note[96];
            if (w->seg >= LOGCAP_SEG_MAX) {
                w->state = LW_SEGMENT_LIMIT;
                snprintf(w->reason, sizeof(w->reason), "%u segments written this boot; capture stops until the next boot",
                         (unsigned)LOGCAP_SEG_MAX);
                break;
            }
            LogCap_MakeName(w->boot, w->seg + 1u, next);
            PathOf(env, w->file, path, sizeof(path));
            if (!w->segEndWritten) {
                int k = snprintf(note, sizeof(note), "=== SEGMENT END: continues in %s ===\n", next);
                int err = 0;
                if (Append(path, note, (size_t)k, &err) == 1) {
                    Fail(w, nowS, "cannot finish the log segment", err);
                    break;
                }
                w->segEndWritten = true;
            }
            /* w->file still names the finished segment while room is made for the next one, so retention can
             * never delete it in the same step (and never deletes the file being written). */
            w->seg++;
            if (!CreateSegment(w, env, nowS, utc)) {
                w->seg--; /* retry the same rotation later */
                LogCap_MakeName(w->boot, w->seg, w->file);
                break;
            }
        }
        if (++w->batchesSinceSpace >= LOGCAP_SPACE_CHECK_BATCHES) {
            w->batchesSinceSpace = 0;
            if (!SpaceOk(w, env, nowS, false))
                break;
        }
        char path[96];
        PathOf(env, w->file, path, sizeof(path));
        int err = 0;
        const int rc = Append(path, batch, total, &err);
        if (rc == 1) {
            Fail(w, nowS, "write to the log file failed", err);
            break;
        }
        w->fileSize += total;
        w->bytesWritten += total;
        w->writes++;
        if (rc == 2) {
            w->closeErrors++;
            Fail(w, nowS, "closing the log file after a write failed", err);
            closeFailed = true;
            /* the data was handed to the filesystem: consume it rather than repeat it */
        }
        ok = true;
    } while (0);
    env->unlock(env->ctx);

    if (!ok)
        return false;
    LogRing_Consume(ring, n);
    w->reportedDropFull = p.dropFull;
    w->reportedDropFullBytes = p.dropFullBytes;
    w->reportedDropBusy = p.dropBusy;
    w->reportedDropContext = p.dropContext;
    w->reportedDropLock = p.dropLock;
    if (w->deferNote) {
        w->deferNote = false;
        w->deferNoteCycles = w->deferNoteSecs = 0;
    }
    w->reportedFails = p.fails;
    if (p.sync)
        w->syncMarked = true;
    if (p.mark || w->nextMarkS == 0)
        w->nextMarkS = nowS + LOGCAP_MARK_INTERVAL_S;
    if (!closeFailed) {
        w->retryNote = false;
        w->state = last ? LW_ENDED : LW_WRITING;
        w->reason[0] = 0;
    }
    return true;
}
