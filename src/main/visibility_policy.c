#include "visibility_policy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airports.h"
#include "custom_rules.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "main.h"
#include "nvs.h"

static const char *TAG = "VisPolicy";

#define VIS_NAMESPACE "radar" /* existing settings namespace */
#define VIS_KEY "gndvis"
#define VIS_VERSION 1

/* ---- settings (guarded by s_lock; loaded lazily on first use) ---- */
static SemaphoreHandle_t s_lock;
static bool s_loaded;
static VisSettings s_settings;

/* ---- per-poll state: RadarTask only (BeginPoll / Admit / EndPoll) ---- */
static VisSettings s_poll;   /* settings snapshot for this poll */
static float s_pollAltM, s_pollSpeedMs;

typedef struct {
    char name[25];
    float latitude;
    float longitude;
    uint16_t associated;
    uint16_t shown;
} PollAirport;
#define VIS_POLL_AIRPORTS_MAX (AIRPORT_ACTIVE_MAX + MAX_AIRPORTS)
static PollAirport *s_airports; /* PSRAM, allocated once when airport mode is first used */
static size_t s_airportCount;
static uint32_t s_pollRetained;

/* Retained aircraft remembered across polls (PSRAM, allocated once when a
 * retention policy is first enabled). The copy is what is shown while the
 * provider no longer reports the aircraft. */
typedef struct {
    bool used;
    uint32_t reportedPoll;   /* s_pollSeq of the last poll whose response contained it */
    uint32_t lastObservedMs;
    Aircraft last;
} RetainedEntry;
static RetainedEntry *s_retained;
static uint32_t s_pollSeq;

/* ---- published per-poll results (guarded by s_lock) ---- */
static VisAirportCount *s_published; /* PSRAM, VIS_AIRPORT_COUNTS_MAX */
static size_t s_publishedCount;
static uint32_t s_totAssociated, s_totShown, s_totRetained, s_totStale;

/* 0.0.28: each provider's latest per-airport counts (PSRAM, allocated only
 * when providers are combined), so a combined view can take the maximum. */
#define VIS_PROVIDERS 2
_Static_assert(VIS_PROVIDERS == AIRCRAFT_PROVIDER_COUNT, "one count slot per provider");
static VisAirportCount *s_provCounts[VIS_PROVIDERS];
static size_t s_provCountN[VIS_PROVIDERS];
static uint32_t s_provRetained[VIS_PROVIDERS];

static void EnsureLock(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
}

void VisPolicy_DefaultSettings(VisSettings *out)
{
    memset(out, 0, sizeof(*out));
    out->version = VIS_VERSION;
    out->providerGround = VIS_PROVIDER_GROUND_USE;
    out->airportMode = VIS_AIRPORT_OFF;
    out->retainMask = 0;
    out->minAltFt = VIS_THRESHOLD_DEFAULT;
    out->minSpeedKt10 = VIS_THRESHOLD_DEFAULT;
    out->staleMinutes = VIS_STALE_MIN_DEFAULT;
    out->airportRadiusM = VIS_RADIUS_M_DEFAULT;
}

static bool ValidSettings(const VisSettings *s)
{
    return s->version == VIS_VERSION && s->providerGround <= VIS_PROVIDER_GROUND_IGNORE &&
           s->airportMode <= VIS_AIRPORT_SHOW && (s->retainMask & ~VIS_RETAIN_ALL) == 0 &&
           (s->minAltFt == VIS_THRESHOLD_DEFAULT || s->minAltFt <= VIS_ALT_FT_MAX) &&
           (s->minSpeedKt10 == VIS_THRESHOLD_DEFAULT || s->minSpeedKt10 <= VIS_SPEED_KT10_MAX) &&
           s->staleMinutes >= VIS_STALE_MIN_MIN && s->staleMinutes <= VIS_STALE_MIN_MAX &&
           s->airportRadiusM >= VIS_RADIUS_M_MIN && s->airportRadiusM <= VIS_RADIUS_M_MAX;
}

/* Caller holds s_lock. */
static void LoadLocked(void)
{
    if (s_loaded)
        return;
    s_loaded = true;
    VisPolicy_DefaultSettings(&s_settings);
    nvs_handle_t h;
    if (nvs_open(VIS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return;
    VisSettings stored;
    size_t size = sizeof(stored);
    esp_err_t err = nvs_get_blob(h, VIS_KEY, &stored, &size);
    nvs_close(h);
    if (err == ESP_OK && size == sizeof(stored) && ValidSettings(&stored))
        s_settings = stored;
    else if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW(TAG, "Ignoring invalid visibility settings; using defaults");
}

void VisPolicy_GetSettings(VisSettings *out)
{
    EnsureLock();
    if (!s_lock || !out) {
        if (out)
            VisPolicy_DefaultSettings(out);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    LoadLocked();
    *out = s_settings;
    xSemaphoreGive(s_lock);
}

bool VisPolicy_SetSettings(const VisSettings *in)
{
    EnsureLock();
    if (!s_lock || !in || !ValidSettings(in))
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    LoadLocked();
    nvs_handle_t h;
    bool ok = nvs_open(VIS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(h, VIS_KEY, in, sizeof(*in)) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    if (ok)
        s_settings = *in;
    xSemaphoreGive(s_lock);
    return ok;
}

float VisPolicy_AltThresholdM(const VisSettings *s)
{
    return s->minAltFt == VIS_THRESHOLD_DEFAULT ? VIS_DEFAULT_ALT_M : (float)s->minAltFt * 0.3048f;
}

float VisPolicy_SpeedThresholdMs(const VisSettings *s)
{
    return s->minSpeedKt10 == VIS_THRESHOLD_DEFAULT ? VIS_DEFAULT_SPEED_MS
                                                    : (float)s->minSpeedKt10 * 0.1f * 0.514444f;
}

/* ---- poll ---- */

int VisPolicy_Passes(void)
{
    return (s_poll.airportMode != VIS_AIRPORT_OFF || s_poll.retainMask != 0) ? 2 : 1;
}

/* Same flat-earth distance the radar uses (Radar_GeoOffsetKm). */
static float DistanceKm(float lat, float lon, float centerLat, float centerLon)
{
    const float east = (lon - centerLon) * 111.0f * cosf(centerLat * 3.14159265f / 180.0f);
    const float north = (lat - centerLat) * 111.0f;
    return sqrtf(east * east + north * north);
}

static void AddPollAirport(const AirportMarker *m)
{
    if (s_airportCount >= VIS_POLL_AIRPORTS_MAX)
        return;
    PollAirport *p = &s_airports[s_airportCount++];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", m->name);
    p->latitude = m->latitude;
    p->longitude = m->longitude;
}

/* Airports an on-ground aircraft can be "at": displayed built-in airports
 * selected for the current radar area (not hidden by an override) and
 * user-defined Dot / Directional / H locations (Square = venue, not an airfield). */
static void SnapshotAirports(void)
{
    s_airportCount = 0;
    if (s_poll.airportMode == VIS_AIRPORT_OFF)
        return;
    if (!s_airports) {
        s_airports = heap_caps_malloc(sizeof(PollAirport) * VIS_POLL_AIRPORTS_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_airports) {
            ESP_LOGE(TAG, "No memory for the airport snapshot; airport aircraft counting disabled");
            return;
        }
    }
    const size_t builtins = Airports_SelectBuiltins(GetRadarLat(), GetRadarLon(), GetRadarRange());
    for (size_t i = 0; i < builtins; i++) {
        AirportMarker m;
        AirportBuiltinView v;
        if (Airports_GetActiveBuiltinView(i, &m, &v) && !v.hidden)
            AddPollAirport(&m);
    }
    const size_t users = Airports_Count();
    for (size_t i = 0; i < users; i++) {
        AirportMarker m;
        if (Airports_Get(i, &m) && (m.markerMode == AIRPORT_MARKER_DOT || m.markerMode == AIRPORT_MARKER_DIRECTIONAL ||
                                    m.markerMode == AIRPORT_MARKER_HELIPORT))
            AddPollAirport(&m);
    }
}

void VisPolicy_BeginPoll(void)
{
    VisPolicy_GetSettings(&s_poll);
    s_pollAltM = VisPolicy_AltThresholdM(&s_poll);
    s_pollSpeedMs = VisPolicy_SpeedThresholdMs(&s_poll);
    s_pollRetained = 0;
    s_pollSeq++;
    SnapshotAirports();
}

static bool RetentionEligible(const CraftResolution *r, uint8_t mask)
{
    if ((mask & VIS_RETAIN_HELICOPTER) && r->aircraftType == AIRCRAFT_HELICOPTER)
        return true;
    if ((mask & VIS_RETAIN_INTERESTING) && r->type == CRAFT_INTERESTING)
        return true;
    if ((mask & VIS_RETAIN_IMPORTANT) && r->type == CRAFT_IMPORTANT)
        return true;
    if ((mask & VIS_RETAIN_POLICE_EMERGENCY) && (r->type == CRAFT_POLICE || r->type == CRAFT_EMERGENCY))
        return true;
    return false;
}

/* Remember that the provider still reports this aircraft (admitted or not):
 * only aircraft it no longer reports can become stale. */
static void NoteReported(const Aircraft *a)
{
    if (!s_retained || s_poll.retainMask == 0)
        return;
    for (size_t k = 0; k < VIS_RETAIN_TABLE_MAX; k++)
        if (s_retained[k].used && !strcmp(s_retained[k].last.icao24, a->icao24))
            s_retained[k].reportedPoll = s_pollSeq;
}

bool VisPolicy_Admit(Aircraft *a, bool flagOnGround, int pass)
{
    if (pass == 0)
        NoteReported(a);
    /* The 0.0.25 rule: provider flag OR (low AND slow); missing values are 0. */
    const bool onGround = (s_poll.providerGround == VIS_PROVIDER_GROUND_USE && flagOnGround) ||
                          (a->altitude <= s_pollAltM && a->velocity <= s_pollSpeedMs);
    if (pass == 0) {
        if (onGround)
            return false;
        a->visState = AIRCRAFT_VIS_NORMAL;
        return true;
    }
    if (!onGround)
        return false; /* airborne: already admitted in pass 0 */

    /* Classification first (same resolver as the radar), then policy. */
    CraftResolution r = ResolveAircraftWithHint(a->callsign, a->icao24, a->providerTypeHint, a->hasProviderTypeHint);
    const bool retained = RetentionEligible(&r, s_poll.retainMask);

    PollAirport *at = NULL;
    if (s_poll.airportMode != VIS_AIRPORT_OFF && s_airportCount) {
        const float radiusKm = (float)s_poll.airportRadiusM / 1000.0f;
        float best = radiusKm;
        for (size_t i = 0; i < s_airportCount; i++) {
            const float d = DistanceKm(a->latitude, a->longitude, s_airports[i].latitude, s_airports[i].longitude);
            if (d <= best) {
                best = d;
                at = &s_airports[i];
            }
        }
    }
    /* A full list (pass 1 keeps parsing for this) still counts the aircraft as
     * associated with its airport (Y), but it is not shown (X) or admitted. */
    const bool canAdmit = gAircraftCount < MAX_AIRCRAFT;
    const bool show = canAdmit && (retained || (at && s_poll.airportMode == VIS_AIRPORT_SHOW));
    if (at) {
        if (at->associated < UINT16_MAX)
            at->associated++;
        if (show && at->shown < UINT16_MAX)
            at->shown++;
    }
    if (!show)
        return false;
    if (retained)
        s_pollRetained++;
    a->visState = AIRCRAFT_VIS_GROUND;
    return true;
}

static int FindInList(const char *icao24)
{
    for (int i = 0; i < gAircraftCount; i++)
        if (gAircraft[i].valid && !strcmp(gAircraft[i].icao24, icao24))
            return i;
    return -1;
}

static void UpdateRetention(uint32_t nowMs, uint32_t *staleOut)
{
    *staleOut = 0;
    if (s_poll.retainMask == 0) {
        if (s_retained)
            memset(s_retained, 0, sizeof(RetainedEntry) * VIS_RETAIN_TABLE_MAX);
        return; /* no retention policy: nothing is carried (0.0.25 behaviour) */
    }
    if (!s_retained) {
        s_retained = heap_caps_malloc(sizeof(RetainedEntry) * VIS_RETAIN_TABLE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_retained) {
            ESP_LOGE(TAG, "No memory for the retention table; stale retention disabled");
            return;
        }
        memset(s_retained, 0, sizeof(RetainedEntry) * VIS_RETAIN_TABLE_MAX);
    }
    const uint32_t timeoutMs = (uint32_t)s_poll.staleMinutes * 60000u;

    /* 1. Observed this poll: remember (or forget, if no longer eligible). */
    const int observed = gAircraftCount;
    for (int i = 0; i < observed; i++) {
        const Aircraft *a = &gAircraft[i];
        if (!a->valid || a->visState == AIRCRAFT_VIS_STALE)
            continue;
        int slot = -1, freeSlot = -1, oldest = 0;
        for (int k = 0; k < (int)VIS_RETAIN_TABLE_MAX; k++) {
            if (s_retained[k].used && !strcmp(s_retained[k].last.icao24, a->icao24))
                slot = k;
            else if (!s_retained[k].used && freeSlot < 0)
                freeSlot = k;
            if (s_retained[k].used &&
                (uint32_t)(nowMs - s_retained[k].lastObservedMs) > (uint32_t)(nowMs - s_retained[oldest].lastObservedMs))
                oldest = k;
        }
        CraftResolution r = ResolveAircraftWithHint(a->callsign, a->icao24, a->providerTypeHint, a->hasProviderTypeHint);
        if (!RetentionEligible(&r, s_poll.retainMask)) {
            if (slot >= 0)
                s_retained[slot].used = false;
            continue;
        }
        if (slot < 0)
            slot = freeSlot >= 0 ? freeSlot : oldest; /* full: replace the least recently observed */
        s_retained[slot].used = true;
        s_retained[slot].reportedPoll = s_pollSeq;
        s_retained[slot].lastObservedMs = nowMs;
        s_retained[slot].last = *a;
    }

    /* 2. Not reported any more: keep at the last reported position until the
     *    timeout (measured from the last observation), then drop. Lowest
     *    priority for list slots; never duplicates an aircraft in the list. */
    for (int k = 0; k < (int)VIS_RETAIN_TABLE_MAX; k++) {
        RetainedEntry *e = &s_retained[k];
        if (!e->used || FindInList(e->last.icao24) >= 0)
            continue;
        if (e->reportedPoll == s_pollSeq) {
            /* Still reported, but no longer shown (e.g. the policy changed or it
             * is on the ground without a matching rule): not stale, forget it. */
            e->used = false;
            continue;
        }
        CraftResolution r = ResolveAircraftWithHint(e->last.callsign, e->last.icao24, e->last.providerTypeHint,
                                                    e->last.hasProviderTypeHint);
        if (!RetentionEligible(&r, s_poll.retainMask)) {
            e->used = false; /* the policy no longer keeps this class */
            continue;
        }
        if ((uint32_t)(nowMs - e->lastObservedMs) >= timeoutMs) {
            e->used = false;
            continue;
        }
        if (gAircraftCount >= MAX_AIRCRAFT)
            continue;
        Aircraft *slot = &gAircraft[gAircraftCount++];
        *slot = e->last;
        slot->valid = true;
        slot->visState = AIRCRAFT_VIS_STALE;
        slot->predictedLat = e->last.latitude;
        slot->predictedLon = e->last.longitude;
        slot->lastUpdateMs = nowMs;
        (*staleOut)++;
    }
}

void VisPolicy_EndPoll(uint32_t nowMs)
{
    VisPolicy_EndPollFrom(nowMs, -1, false);
}

static bool SameAirport(const VisAirportCount *a, const VisAirportCount *b)
{
    return !strcmp(a->name, b->name) && a->latitude == b->latitude && a->longitude == b->longitude;
}

void VisPolicy_EndPollFrom(uint32_t nowMs, int provider, bool combineOther)
{
    uint32_t stale = 0;
    UpdateRetention(nowMs, &stale);

    EnsureLock();
    if (!s_lock)
        return;
    if (!s_published && s_airportCount)
        s_published = heap_caps_malloc(sizeof(VisAirportCount) * VIS_AIRPORT_COUNTS_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (provider >= 0 && provider < VIS_PROVIDERS && !s_provCounts[provider])
        s_provCounts[provider] = heap_caps_malloc(sizeof(VisAirportCount) * VIS_AIRPORT_COUNTS_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    VisAirportCount *mine = (provider >= 0 && provider < VIS_PROVIDERS) ? s_provCounts[provider] : NULL;
    const int other = (provider == 0) ? 1 : 0;
    const VisAirportCount *theirs = (combineOther && provider >= 0 && provider < VIS_PROVIDERS) ? s_provCounts[other] : NULL;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_publishedCount = 0;
    s_totAssociated = s_totShown = 0;
    size_t mineN = 0;
    for (size_t i = 0; i < s_airportCount; i++) {
        const PollAirport *p = &s_airports[i];
        if (!p->associated)
            continue;
        VisAirportCount c;
        memcpy(c.name, p->name, sizeof(c.name));
        c.latitude = p->latitude;
        c.longitude = p->longitude;
        c.associated = p->associated;
        c.shown = p->shown;
        if (mine && mineN < VIS_AIRPORT_COUNTS_MAX)
            mine[mineN++] = c;
        if (s_published && s_publishedCount < VIS_AIRPORT_COUNTS_MAX)
            s_published[s_publishedCount++] = c;
    }
    if (mine && provider >= 0 && provider < VIS_PROVIDERS) {
        s_provCountN[provider] = mineN;
        s_provRetained[provider] = s_pollRetained;
    }
    uint32_t retained = s_pollRetained;
    if (theirs && s_published) {
        /* Same airport in both providers' latest polls: the higher count (the
         * aircraft at an airport are the same aircraft; adding would double
         * count). An airport only the other provider sees is added. */
        for (size_t j = 0; j < s_provCountN[other]; j++) {
            const VisAirportCount *t = &theirs[j];
            size_t k = 0;
            while (k < s_publishedCount && !SameAirport(&s_published[k], t))
                k++;
            if (k < s_publishedCount) {
                if (t->associated > s_published[k].associated)
                    s_published[k].associated = t->associated;
                if (t->shown > s_published[k].shown)
                    s_published[k].shown = t->shown;
            } else if (s_publishedCount < VIS_AIRPORT_COUNTS_MAX) {
                s_published[s_publishedCount++] = *t;
            }
        }
        if (s_provRetained[other] > retained)
            retained = s_provRetained[other];
    }
    if (theirs && s_published) {
        for (size_t k = 0; k < s_publishedCount; k++) {
            s_totAssociated += s_published[k].associated;
            s_totShown += s_published[k].shown;
        }
    } else { /* one provider: totals over every airport of this poll, as before */
        for (size_t i = 0; i < s_airportCount; i++) {
            s_totAssociated += s_airports[i].associated;
            s_totShown += s_airports[i].shown;
        }
    }
    s_totRetained = retained;
    s_totStale = stale;
    xSemaphoreGive(s_lock);
}

size_t VisPolicy_AirportCountTotal(void)
{
    EnsureLock();
    if (!s_lock)
        return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t n = s_publishedCount;
    xSemaphoreGive(s_lock);
    return n;
}

bool VisPolicy_GetAirportCount(size_t index, VisAirportCount *out)
{
    EnsureLock();
    if (!s_lock || !out)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = s_published && index < s_publishedCount;
    if (ok)
        *out = s_published[index];
    xSemaphoreGive(s_lock);
    return ok;
}

void VisPolicy_GetTotals(uint32_t *associated, uint32_t *shown, uint32_t *retained, uint32_t *stale)
{
    EnsureLock();
    if (!s_lock)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (associated)
        *associated = s_totAssociated;
    if (shown)
        *shown = s_totShown;
    if (retained)
        *retained = s_totRetained;
    if (stale)
        *stale = s_totStale;
    xSemaphoreGive(s_lock);
}

void VisPolicy_ResetForTest(void)
{
    s_loaded = false;
    VisPolicy_DefaultSettings(&s_settings);
    VisPolicy_DefaultSettings(&s_poll);
    if (s_retained)
        memset(s_retained, 0, sizeof(RetainedEntry) * VIS_RETAIN_TABLE_MAX);
    s_airportCount = 0;
    s_publishedCount = 0;
    for (int p = 0; p < VIS_PROVIDERS; p++) {
        s_provCountN[p] = 0;
        s_provRetained[p] = 0;
    }
    s_totAssociated = s_totShown = s_totRetained = s_totStale = 0;
}
