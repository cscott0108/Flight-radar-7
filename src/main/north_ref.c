#include "north_ref.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvs.h"
#include "time_util.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
static const char *TAG = "NorthRef";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define NR_LOCK() portENTER_CRITICAL(&s_mux)
#define NR_UNLOCK() portEXIT_CRITICAL(&s_mux)
#define NR_WORK_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define NR_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define NR_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#include <pthread.h>
static pthread_mutex_t s_mux = PTHREAD_MUTEX_INITIALIZER;
#define NR_LOCK() pthread_mutex_lock(&s_mux)
#define NR_UNLOCK() pthread_mutex_unlock(&s_mux)
#define NR_WORK_ALLOC(n) malloc(n)
#define NR_LOGI(...) ((void)0)
#define NR_LOGW(...) ((void)0)
#endif

/* ===================== WMM2025 ===================== */

typedef struct {
    int8_t n, m;
    float g, h, gd, hd;
} WmmCoef;

#include "wmm2025_coef.inc"

#define WMM_MAXORD 12
#define WMM_SIZE (WMM_MAXORD + 1)

/* Transient work area (doubles, ~8.5 KB): the coefficient normalisation and the
 * evaluation share it, so nothing stays allocated between computations. */
typedef struct {
    double c[WMM_SIZE][WMM_SIZE], cd[WMM_SIZE][WMM_SIZE], tc[WMM_SIZE][WMM_SIZE];
    double dp[WMM_SIZE][WMM_SIZE], k[WMM_SIZE][WMM_SIZE];
    double p[WMM_SIZE * WMM_SIZE];
    double sp[WMM_SIZE], cp[WMM_SIZE], pp[WMM_SIZE], fn[WMM_SIZE], fm[WMM_SIZE];
} WmmWork;

double Wmm_DecimalYear(int64_t utc)
{
    time_t t = (time_t)utc;
    struct tm tmv;
    if (!gmtime_r(&t, &tmv))
        return 0.0;
    const int y = tmv.tm_year + 1900;
    const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    const double daySec = (double)tmv.tm_yday * 86400.0 + tmv.tm_hour * 3600.0 + tmv.tm_min * 60.0 + tmv.tm_sec;
    return y + daySec / ((leap ? 366.0 : 365.0) * 86400.0);
}

/* Port of the NOAA WMM reference algorithm (geomag.c / pygeomag GeoMag.calculate,
 * public domain), degree 12. */
bool Wmm_Compute(double glat, double glon, double alt, double decYear, WmmResult *out)
{
    if (!out || !isfinite(glat) || !isfinite(glon) || !isfinite(alt) || !isfinite(decYear) ||
        glat < -90.0 || glat > 90.0 || glon < -180.0 || glon > 180.0 || alt < -1.0 || alt > 850.0)
        return false;
    const double dt = decYear - WMM_EPOCH;
    if (dt < 0.0 || dt > WMM_LIFE_YEARS)
        return false;
    WmmWork *w = NR_WORK_ALLOC(sizeof(WmmWork));
    if (!w)
        return false;
    memset(w, 0, sizeof(*w));

    /* coefficients -> unnormalised (Schmidt) */
    for (size_t i = 0; i < sizeof(kWmm2025) / sizeof(kWmm2025[0]); i++) {
        const int n = kWmm2025[i].n, m = kWmm2025[i].m;
        w->c[m][n] = kWmm2025[i].g;
        w->cd[m][n] = kWmm2025[i].gd;
        if (m != 0) {
            w->c[n][m - 1] = kWmm2025[i].h;
            w->cd[n][m - 1] = kWmm2025[i].hd;
        }
    }
    double *snorm = w->p; /* same storage as the reference implementation */
    snorm[0] = 1.0;
    w->fm[0] = 0.0;
    for (int n = 1; n <= WMM_MAXORD; n++) {
        snorm[n] = snorm[n - 1] * (double)(2 * n - 1) / (double)n;
        int j = 2;
        for (int m = 0; m <= n; m++) {
            w->k[m][n] = (double)((n - 1) * (n - 1) - m * m) / (double)((2 * n - 1) * (2 * n - 3));
            if (m > 0) {
                const double flnmj = (double)((n - m + 1) * j) / (double)(n + m);
                snorm[n + m * WMM_SIZE] = snorm[n + (m - 1) * WMM_SIZE] * sqrt(flnmj);
                j = 1;
                w->c[n][m - 1] = snorm[n + m * WMM_SIZE] * w->c[n][m - 1];
                w->cd[n][m - 1] = snorm[n + m * WMM_SIZE] * w->cd[n][m - 1];
            }
            w->c[m][n] = snorm[n + m * WMM_SIZE] * w->c[m][n];
            w->cd[m][n] = snorm[n + m * WMM_SIZE] * w->cd[m][n];
        }
        w->fn[n] = (double)(n + 1);
        w->fm[n] = (double)n;
    }
    w->k[1][1] = 0.0;

    const double a = 6378.137, b = 6356.7523142, re = 6371.2;
    const double a2 = a * a, b2 = b * b, c2 = a2 - b2, a4 = a2 * a2, b4 = b2 * b2, c4 = a4 - b4;
    w->sp[0] = 0.0;
    w->cp[0] = w->pp[0] = 1.0;
    w->dp[0][0] = 0.0;
    const double rlon = glon * M_PI / 180.0, rlat = glat * M_PI / 180.0;
    const double srlon = sin(rlon), srlat = sin(rlat), crlon = cos(rlon), crlat = cos(rlat);
    const double srlat2 = srlat * srlat, crlat2 = crlat * crlat;
    w->sp[1] = srlon;
    w->cp[1] = crlon;

    /* geodetic -> spherical */
    const double q = sqrt(a2 - c2 * srlat2);
    const double q1 = alt * q;
    const double q2 = ((q1 + a2) / (q1 + b2)) * ((q1 + a2) / (q1 + b2));
    const double ct = srlat / sqrt(q2 * crlat2 + srlat2);
    const double st = sqrt(1.0 - ct * ct);
    const double r2 = alt * alt + 2.0 * q1 + (a4 - c4 * srlat2) / (q * q);
    const double r = sqrt(r2);
    const double d = sqrt(a2 * crlat2 + b2 * srlat2);
    const double ca = (alt + d) / r;
    const double sa = c2 * crlat * srlat / (r * d);
    for (int m = 2; m <= WMM_MAXORD; m++) {
        w->sp[m] = w->sp[1] * w->cp[m - 1] + w->cp[1] * w->sp[m - 1];
        w->cp[m] = w->cp[1] * w->cp[m - 1] - w->sp[1] * w->sp[m - 1];
    }
    const double aor = re / r;
    double ar = aor * aor;
    double br = 0.0, bt = 0.0, bp = 0.0, bpp = 0.0;
    double *p = w->p;
    for (int n = 1; n <= WMM_MAXORD; n++) {
        ar = ar * aor;
        for (int m = 0; m <= n; m++) {
            if (n == m) {
                p[n + m * WMM_SIZE] = st * p[n - 1 + (m - 1) * WMM_SIZE];
                w->dp[m][n] = st * w->dp[m - 1][n - 1] + ct * p[n - 1 + (m - 1) * WMM_SIZE];
            } else if (n == 1 && m == 0) {
                p[n + m * WMM_SIZE] = ct * p[n - 1 + m * WMM_SIZE];
                w->dp[m][n] = ct * w->dp[m][n - 1] - st * p[n - 1 + m * WMM_SIZE];
            } else if (n > 1 && n != m) {
                if (m > n - 2) {
                    p[n - 2 + m * WMM_SIZE] = 0.0;
                    w->dp[m][n - 2] = 0.0;
                }
                p[n + m * WMM_SIZE] = ct * p[n - 1 + m * WMM_SIZE] - w->k[m][n] * p[n - 2 + m * WMM_SIZE];
                w->dp[m][n] = ct * w->dp[m][n - 1] - st * p[n - 1 + m * WMM_SIZE] - w->k[m][n] * w->dp[m][n - 2];
            }
            w->tc[m][n] = w->c[m][n] + dt * w->cd[m][n];
            if (m != 0)
                w->tc[n][m - 1] = w->c[n][m - 1] + dt * w->cd[n][m - 1];
            const double par = ar * p[n + m * WMM_SIZE];
            double temp1, temp2;
            if (m == 0) {
                temp1 = w->tc[m][n] * w->cp[m];
                temp2 = w->tc[m][n] * w->sp[m];
            } else {
                temp1 = w->tc[m][n] * w->cp[m] + w->tc[n][m - 1] * w->sp[m];
                temp2 = w->tc[m][n] * w->sp[m] - w->tc[n][m - 1] * w->cp[m];
            }
            bt = bt - ar * temp1 * w->dp[m][n];
            bp += w->fm[m] * temp2 * par;
            br += w->fn[n] * temp1 * par;
            if (st == 0.0 && m == 1) { /* geographic poles */
                if (n == 1)
                    w->pp[n] = w->pp[n - 1];
                else
                    w->pp[n] = ct * w->pp[n - 1] - w->k[m][n] * w->pp[n - 2];
                bpp += w->fm[m] * temp2 * (ar * w->pp[n]);
            }
        }
    }
    bp = (st == 0.0) ? bpp : bp / st;
    free(w);

    const double bx = -bt * ca - br * sa;
    const double by = bp;
    const double bz = bt * sa - br * ca;
    const double bh = sqrt(bx * bx + by * by);
    out->horizNt = bh;
    out->totalNt = sqrt(bh * bh + bz * bz);
    out->declDeg = atan2(by, bx) * 180.0 / M_PI;
    out->inclDeg = atan2(bz, bh) * 180.0 / M_PI;
    out->blackout = bh < WMM_BLACKOUT_NT;
    out->caution = bh < WMM_CAUTION_NT;
    return isfinite(out->declDeg);
}

/* ===================== state ===================== */

#define NR_NVS_NAMESPACE "radar"
#define NR_NVS_KEY "northref"
#define NR_CACHE_MAGIC 0x3152564Du /* "MVR1" */
#define NR_CACHE_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags; /* bit0 blackout, bit1 caution */
    int32_t latE5, lonE5;
    float declDeg, horizNt, decYear, modelEpoch;
    int64_t computedUtc;
    uint32_t crc;
} NorthRefCache;

static NorthRefHooks s_hooks;
static NorthRefStatus s_st;      /* guarded by s_mux */
static float s_rotDeg;           /* cached display rotation, guarded by s_mux */
static float s_centerLat = NAN, s_centerLon = NAN;

static int64_t NowUtc(void)
{
    if (s_hooks.utc)
        return s_hooks.utc();
    int64_t now = (int64_t)time(NULL);
    return TimeUtil_IsSynced(now) ? now : 0;
}

static uint32_t UpSec(void)
{
    if (s_hooks.upSec)
        return s_hooks.upSec();
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000000);
#else
    return 0;
#endif
}

static double BuildYear(void)
{
    if (s_hooks.buildYear)
        return s_hooks.buildYear();
    /* __DATE__ = "Mmm dd yyyy": the firmware cannot predate its own build. */
    static const char *const months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *dt = __DATE__;
    int month = 0;
    for (int i = 0; i < 12; i++)
        if (!strncmp(dt, months + 3 * i, 3))
            month = i;
    return atoi(dt + 7) + (month + 0.5) / 12.0;
}

static uint32_t Crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

const char *NorthRefMode_Name(NorthRefMode mode)
{
    switch (mode) {
    case NORTH_REF_AUTO: return "AUTO";
    case NORTH_REF_MAGNETIC: return "MAGNETIC";
    case NORTH_REF_TRUE: return "TRUE";
    default: return "?";
    }
}

NorthResolved NorthRef_ResolveFor(NorthRefMode mode, bool varValid, bool blackout, bool *fallbackOut,
                                  const char **reasonOut)
{
    bool fb = false;
    const char *why = NULL;
    NorthResolved r = NORTH_RESOLVED_TRUE;
    if (mode == NORTH_REF_TRUE) {
        r = NORTH_RESOLVED_TRUE;
    } else if (!varValid) {
        fb = true;
        why = "no valid magnetic variation (WMM) for this location/date";
    } else if (mode == NORTH_REF_AUTO && blackout) {
        fb = true;
        why = "WMM blackout zone (horizontal field < 2000 nT): magnetic reference unreliable";
    } else {
        r = NORTH_RESOLVED_MAGNETIC;
    }
    if (fallbackOut)
        *fallbackOut = fb;
    if (reasonOut)
        *reasonOut = why;
    return r;
}

/* Recompute the derived fields; caller holds the lock and passes `now`
 * (NowUtc() read BEFORE taking the lock). On the device NR_LOCK is a spinlock
 * critical section: no libc call that can take a lock (time(), stdio, ...) may
 * run inside it - IDF aborts (0.0.33 boot-loop fix). */
static void PublishLocked(int64_t now)
{
    bool fb;
    const char *why;
    s_st.resolved = NorthRef_ResolveFor(s_st.mode, s_st.varValid, s_st.blackout, &fb, &why);
    s_st.fallback = fb;
    s_st.fallbackReason = why;
    s_rotDeg = (s_st.resolved == NORTH_RESOLVED_MAGNETIC) ? s_st.declDeg : 0.0f;
    s_st.stale = s_st.varValid && s_st.computedUtc > 0 && now > 0 && now - s_st.computedUtc >= NORTHREF_STALE_SEC;
}

static bool SamePlace(float aLat, float aLon, float bLat, float bLon)
{
    return fabsf(aLat - bLat) <= NORTHREF_SAME_PLACE_DEG && fabsf(aLon - bLon) <= NORTHREF_SAME_PLACE_DEG;
}

static bool LoadCache(NorthRefCache *c)
{
    FILE *f = fopen(NORTHREF_CACHE_PATH, "rb");
    if (!f)
        return false;
    const bool ok = fread(c, sizeof(*c), 1, f) == 1;
    fclose(f);
    return ok && c->magic == NR_CACHE_MAGIC && c->version == NR_CACHE_VERSION &&
           c->crc == Crc32(c, offsetof(NorthRefCache, crc)) && isfinite(c->declDeg) && fabsf(c->declDeg) <= 180.0f;
}

static bool WriteCache(const NorthRefCache *src)
{
    NorthRefCache c = *src;
    c.magic = NR_CACHE_MAGIC;
    c.version = NR_CACHE_VERSION;
    c.crc = Crc32(&c, offsetof(NorthRefCache, crc));
    /* temp file + rename: a power cut leaves either the old or the new cache */
    char tmp[sizeof(NORTHREF_CACHE_PATH) + 4];
    snprintf(tmp, sizeof(tmp), "%s.tmp", NORTHREF_CACHE_PATH);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fwrite(&c, sizeof(c), 1, f) == 1;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        remove(NORTHREF_CACHE_PATH);
        ok = rename(tmp, NORTHREF_CACHE_PATH) == 0;
    }
    if (!ok)
        remove(tmp);
    return ok;
}

/* Compute for (lat,lon) now; never blocks on network. writeCache only with a
 * real (synced) date. Caller must NOT hold the lock. */
static void ComputeFor(float lat, float lon, const char *why)
{
    const int64_t now = NowUtc();
    double year;
    bool estimated = false;
    if (now > 0) {
        year = Wmm_DecimalYear(now);
    } else {
        /* No clock yet: the last cached date (if any) or the build date. The
         * value is replaced once the clock syncs (NorthRef_Service). */
        NorthRefCache c;
        year = (LoadCache(&c) && c.decYear > 0) ? c.decYear : BuildYear();
        estimated = true;
    }
    WmmResult r;
    const bool ok = Wmm_Compute(lat, lon, 0.0, year, &r);
    const char *cacheState = NULL;
    if (ok && !estimated) {
        NorthRefCache c = {0};
        c.latE5 = (int32_t)lroundf(lat * 1e5f);
        c.lonE5 = (int32_t)lroundf(lon * 1e5f);
        c.declDeg = (float)r.declDeg;
        c.horizNt = (float)r.horizNt;
        c.decYear = (float)year;
        c.modelEpoch = (float)WMM_EPOCH;
        c.computedUtc = now;
        c.flags = (uint16_t)((r.blackout ? 1u : 0u) | (r.caution ? 2u : 0u));
        cacheState = WriteCache(&c) ? "written" : "write failed";
    }
    const uint32_t up = UpSec();
    NR_LOCK();
    s_st.lastAttemptUtc = now;
    s_st.lastAttemptUptimeSec = up;
    s_st.lastAttemptOk = ok;
    s_st.computeCount++;
    s_st.lat = lat;
    s_st.lon = lon;
    if (ok) {
        s_st.varValid = true;
        s_st.declDeg = (float)r.declDeg;
        s_st.horizNt = (float)r.horizNt;
        s_st.blackout = r.blackout;
        s_st.caution = r.caution;
        s_st.decYear = (float)year;
        s_st.computedUtc = estimated ? 0 : now;
        s_st.dateEstimated = estimated;
        s_st.fromCache = false;
        s_st.lastResult = why;
    } else {
        s_st.varValid = false; /* the old location's value no longer applies */
        s_st.blackout = s_st.caution = false;
        s_st.lastResult = "WMM could not compute (date outside 2025.0-2030.0 or invalid location)";
    }
    if (cacheState)
        s_st.cacheState = cacheState;
    PublishLocked(now);
    NR_UNLOCK();
    NR_LOGI("Magnetic variation %s: %s %.2f deg at %.4f,%.4f (year %.2f%s)", why, ok ? "ok" : "FAILED",
            ok ? r.declDeg : 0.0, lat, lon, year, estimated ? ", estimated date" : "");
}

void NorthRef_SetHooks(const NorthRefHooks *h)
{
    if (h)
        s_hooks = *h;
    else
        memset(&s_hooks, 0, sizeof(s_hooks));
}

void NorthRef_ResetForTest(void)
{
    NR_LOCK();
    memset(&s_st, 0, sizeof(s_st));
    s_rotDeg = 0.0f;
    s_centerLat = s_centerLon = NAN;
    NR_UNLOCK();
}

static NorthRefMode LoadMode(void)
{
    nvs_handle_t h;
    uint8_t v = NORTH_REF_AUTO;
    if (nvs_open(NR_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, NR_NVS_KEY, &v) != ESP_OK || v >= NORTH_REF_MODE_COUNT)
            v = NORTH_REF_AUTO; /* missing key = default AUTO (no migration needed) */
        nvs_close(h);
    }
    return (NorthRefMode)v;
}

void NorthRef_Init(float lat, float lon)
{
    NR_LOCK();
    memset(&s_st, 0, sizeof(s_st));
    s_st.cacheState = "missing";
    s_st.lastResult = "not computed yet";
    NR_UNLOCK();
    const NorthRefMode mode = LoadMode();
    NorthRefCache c;
    const bool haveCache = LoadCache(&c);
    /* File probe and clock read happen here, outside the lock (0.0.33: an fopen()
     * inside NR_LOCK aborted every first boot - no cache file yet). */
    bool fileExists = false;
    if (!haveCache) {
        FILE *f = fopen(NORTHREF_CACHE_PATH, "rb");
        if (f) {
            fclose(f);
            fileExists = true;
        }
    }
    const int64_t now = NowUtc();
    NR_LOCK();
    s_st.mode = mode;
    s_centerLat = lat;
    s_centerLon = lon;
    bool usable = false;
    if (haveCache) {
        const float cLat = c.latE5 / 1e5f, cLon = c.lonE5 / 1e5f;
        if (!SamePlace(cLat, cLon, lat, lon)) {
            s_st.cacheState = "other location (recomputed)";
        } else if (c.decYear < WMM_EPOCH || c.decYear > WMM_EPOCH + WMM_LIFE_YEARS) {
            s_st.cacheState = "outside model life (recomputed)";
        } else {
            usable = true;
            s_st.cacheState = "loaded";
            s_st.varValid = true;
            s_st.fromCache = true;
            s_st.declDeg = c.declDeg;
            s_st.horizNt = c.horizNt;
            s_st.blackout = (c.flags & 1u) != 0;
            s_st.caution = (c.flags & 2u) != 0;
            s_st.decYear = c.decYear;
            s_st.computedUtc = c.computedUtc;
            s_st.lat = cLat;
            s_st.lon = cLon;
            s_st.lastResult = "loaded from cache";
        }
    } else if (fileExists) {
        s_st.cacheState = "invalid (recomputed)";
    }
    PublishLocked(now);
    NR_UNLOCK();
    if (!usable)
        ComputeFor(lat, lon, "computed at boot");
}

NorthRefMode NorthRef_GetMode(void)
{
    NR_LOCK();
    NorthRefMode m = s_st.mode;
    NR_UNLOCK();
    return m;
}

bool NorthRef_SetMode(NorthRefMode mode)
{
    if ((unsigned)mode >= NORTH_REF_MODE_COUNT)
        return false;
    nvs_handle_t h;
    bool ok = nvs_open(NR_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_u8(h, NR_NVS_KEY, (uint8_t)mode) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    const int64_t now = NowUtc();
    NR_LOCK();
    s_st.mode = mode; /* applies for this run even if persisting failed */
    PublishLocked(now);
    NR_UNLOCK();
    return ok;
}

void NorthRef_OnLocationChanged(float lat, float lon)
{
    const int64_t now = NowUtc();
    NR_LOCK();
    const bool same = isfinite(s_centerLat) && SamePlace(s_centerLat, s_centerLon, lat, lon);
    s_centerLat = lat;
    s_centerLon = lon;
    const bool applies = s_st.varValid && SamePlace(s_st.lat, s_st.lon, lat, lon);
    if (!applies) {
        s_st.varValid = false; /* invalidate at once: never show the old location's value */
        PublishLocked(now);
    }
    NR_UNLOCK();
    if (same && applies)
        return;
    NorthRefCache c;
    if (LoadCache(&c) && SamePlace(c.latE5 / 1e5f, c.lonE5 / 1e5f, lat, lon) && c.decYear >= WMM_EPOCH &&
        c.decYear <= WMM_EPOCH + WMM_LIFE_YEARS) {
        NR_LOCK();
        s_st.varValid = true;
        s_st.fromCache = true;
        s_st.declDeg = c.declDeg;
        s_st.horizNt = c.horizNt;
        s_st.blackout = (c.flags & 1u) != 0;
        s_st.caution = (c.flags & 2u) != 0;
        s_st.decYear = c.decYear;
        s_st.computedUtc = c.computedUtc;
        s_st.dateEstimated = false;
        s_st.lat = c.latE5 / 1e5f;
        s_st.lon = c.lonE5 / 1e5f;
        s_st.cacheState = "loaded";
        s_st.lastResult = "location changed: reused cached value for this location";
        PublishLocked(now);
        NR_UNLOCK();
        return;
    }
    ComputeFor(lat, lon, "location changed");
}

void NorthRef_Service(bool maintenanceWindow)
{
    const int64_t now = NowUtc();
    NR_LOCK();
    const bool estimated = s_st.varValid && s_st.dateEstimated;
    const bool missing = !s_st.varValid && isfinite(s_centerLat);
    PublishLocked(now); /* refreshes `stale` against the clock */
    const bool stale = s_st.stale;
    const float lat = s_centerLat, lon = s_centerLon;
    const uint32_t lastTry = s_st.lastAttemptUptimeSec;
    NR_UNLOCK();
    if (!isfinite(lat))
        return;
    const bool synced = now > 0;
    if (estimated && synced) {
        ComputeFor(lat, lon, "clock synced: replaced estimated-date value");
    } else if (missing && synced && UpSec() - lastTry >= 3600u) {
        ComputeFor(lat, lon, "retry"); /* at most hourly; WMM is local, this cannot fail for network reasons */
    } else if (stale && maintenanceWindow) {
        ComputeFor(lat, lon, "yearly refresh in maintenance window");
    }
}

NorthResolved NorthRef_Resolved(void)
{
    NR_LOCK();
    NorthResolved r = s_st.resolved;
    NR_UNLOCK();
    return r;
}

float NorthRef_DisplayRotationDeg(void)
{
    NR_LOCK();
    float r = s_rotDeg;
    NR_UNLOCK();
    return r;
}

bool NorthRef_Declination(float *declDegOut)
{
    NR_LOCK();
    const bool ok = s_st.varValid;
    const float d = s_st.declDeg;
    NR_UNLOCK();
    if (ok && declDegOut)
        *declDegOut = d;
    return ok;
}

static float Norm360(float b)
{
    b = fmodf(b, 360.0f);
    if (b < 0.0f)
        b += 360.0f;
    return b;
}

float NorthRef_TrueToDisplay(float trueBearingDeg)
{
    if (!isfinite(trueBearingDeg))
        return trueBearingDeg;
    return Norm360(trueBearingDeg - NorthRef_DisplayRotationDeg());
}

float NorthRef_DesignatorToTrue(float magneticAxisDeg)
{
    float d;
    if (!isfinite(magneticAxisDeg) || !NorthRef_Declination(&d))
        return magneticAxisDeg;
    return Norm360(magneticAxisDeg + d);
}

void NorthRef_GetStatus(NorthRefStatus *out)
{
    if (!out)
        return;
    const int64_t now = NowUtc();
    NR_LOCK();
    PublishLocked(now);
    *out = s_st;
    NR_UNLOCK();
}
