#include "provider_merge.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "ProviderMerge";

typedef struct {
    Aircraft *list; /* PSRAM, MAX_AIRCRAFT entries, allocated on first Store */
    int count;
    uint32_t storedMs;
    bool have;
} Snapshot;

static Snapshot s_snap[AIRCRAFT_PROVIDER_COUNT];

bool ProviderMerge_Store(AircraftProviderType provider, const Aircraft *list, int count, uint32_t nowMs)
{
    if (provider >= AIRCRAFT_PROVIDER_COUNT || !list || count < 0)
        return false;
    Snapshot *s = &s_snap[provider];
    if (!s->list) {
        s->list = heap_caps_malloc(sizeof(Aircraft) * MAX_AIRCRAFT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s->list) {
            ESP_LOGE(TAG, "No memory for the %s snapshot; showing that provider alone",
                     AircraftProviderType_Name(provider));
            return false;
        }
    }
    if (count > MAX_AIRCRAFT)
        count = MAX_AIRCRAFT;
    int n = 0;
    for (int i = 0; i < count; i++)
        if (list[i].valid && list[i].visState != AIRCRAFT_VIS_STALE)
            s->list[n++] = list[i];
    s->count = n;
    s->storedMs = nowMs;
    s->have = true;
    return true;
}

void ProviderMerge_Forget(AircraftProviderType provider)
{
    if (provider < AIRCRAFT_PROVIDER_COUNT) {
        s_snap[provider].have = false;
        s_snap[provider].count = 0;
    }
}

bool ProviderMerge_IsFresh(AircraftProviderType provider, uint32_t nowMs, uint32_t maxAgeMs)
{
    if (provider >= AIRCRAFT_PROVIDER_COUNT || maxAgeMs == 0)
        return false;
    const Snapshot *s = &s_snap[provider];
    return s->have && (uint32_t)(nowMs - s->storedMs) <= maxAgeMs;
}

static bool Blank(const char *s)
{
    for (; *s; s++)
        if (*s != ' ')
            return false;
    return true;
}

#define FILL_TEXT(field)                                                     \
    do {                                                                     \
        if (Blank(newer->field) && !Blank(older->field))                     \
            memcpy(newer->field, older->field, sizeof(newer->field));        \
    } while (0)

void ProviderMerge_Fill(Aircraft *newer, const Aircraft *older)
{
    FILL_TEXT(callsign);
    FILL_TEXT(originCountry);
    FILL_TEXT(registration);
    FILL_TEXT(operatorName);
    if (!newer->hasProviderTypeHint && older->hasProviderTypeHint) {
        newer->hasProviderTypeHint = true;
        newer->providerTypeHint = older->providerTypeHint;
    }
    /* 0.0.30: optional values a report did not carry are filled the same way
     * (a missing speed / vertical rate never erases the other provider's). */
    if (!(newer->dataFlags & AIRCRAFT_DATA_VELOCITY) && (older->dataFlags & AIRCRAFT_DATA_VELOCITY)) {
        newer->velocity = older->velocity;
        newer->dataFlags |= AIRCRAFT_DATA_VELOCITY;
    }
    if (!(newer->dataFlags & AIRCRAFT_DATA_VRATE) && (older->dataFlags & AIRCRAFT_DATA_VRATE)) {
        newer->verticalRateFpm = older->verticalRateFpm;
        newer->dataFlags |= AIRCRAFT_DATA_VRATE;
    }
}

static int FindIcao(const Aircraft *list, int count, const char *icao24)
{
    for (int i = 0; i < count; i++)
        if (!strcasecmp(list[i].icao24, icao24))
            return i;
    return -1;
}

/* The merged view of one ICAO24: providers are visited in `order`; a later
 * provider's report replaces the current one only when strictly newer (so a
 * tie keeps the earlier provider), and every step fills the missing fields
 * from the other report. Deterministic for a given order. */
static void BestFor(const char *icao24, const AircraftProviderType *order, int nOrder, uint32_t nowMs,
                    const uint32_t *maxAgeMs, Aircraft *best)
{
    bool have = false;
    for (int k = 0; k < nOrder; k++) {
        const AircraftProviderType q = order[k];
        if (!ProviderMerge_IsFresh(q, nowMs, maxAgeMs[q]))
            continue;
        const int j = FindIcao(s_snap[q].list, s_snap[q].count, icao24);
        if (j < 0)
            continue;
        const Aircraft *b = &s_snap[q].list[j];
        if (!have) {
            *best = *b;
            have = true;
        } else if ((int32_t)(b->lastUpdateMs - best->lastUpdateMs) > 0) {
            Aircraft older = *best;
            *best = *b;
            ProviderMerge_Fill(best, &older);
        } else {
            ProviderMerge_Fill(best, b);
        }
    }
}

int ProviderMerge_Build(Aircraft *out, int max, uint32_t nowMs,
                        const uint32_t maxAgeMs[AIRCRAFT_PROVIDER_COUNT], AircraftProviderType first)
{
    if (!out || max <= 0 || first >= AIRCRAFT_PROVIDER_COUNT)
        return 0;
    AircraftProviderType order[AIRCRAFT_PROVIDER_COUNT];
    int nOrder = 0;
    order[nOrder++] = first;
    for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++)
        if (p != (int)first)
            order[nOrder++] = (AircraftProviderType)p;

    int n = 0;
    /* Pass 0: airborne (normal) entries; pass 1: on-ground entries. An
     * ICAO24 lands in the pass of its most recent report. */
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < nOrder; k++) {
            const AircraftProviderType p = order[k];
            if (!ProviderMerge_IsFresh(p, nowMs, maxAgeMs[p]))
                continue;
            const Snapshot *s = &s_snap[p];
            for (int i = 0; i < s->count; i++) {
                if (FindIcao(out, n, s->list[i].icao24) >= 0)
                    continue; /* already merged */
                Aircraft best;
                BestFor(s->list[i].icao24, order, nOrder, nowMs, maxAgeMs, &best);
                const bool ground = best.visState == AIRCRAFT_VIS_GROUND;
                if ((pass == 0) == ground)
                    continue; /* listed in the other pass */
                if (n >= max)
                    return n;
                out[n++] = best;
            }
        }
    }
    return n;
}

void ProviderMerge_ResetForTest(void)
{
    for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++) {
        free(s_snap[p].list);
        memset(&s_snap[p], 0, sizeof(s_snap[p]));
    }
}
