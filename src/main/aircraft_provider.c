#include "aircraft_provider.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "diag_telemetry.h"
#include "opensky_client.h"
#include "adsblol_client.h"
#include "visibility_policy.h"
#include "provider_merge.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RADAR_NAMESPACE "radar"
#define PROVIDER_KEY "aircraft_prov"
#define DEBUG_LEVEL_KEY "prov_debug"
/* 0.0.28: independent providers. Enabled set and one interval per provider,
 * same NVS namespace as every other radar setting. */
#define ENABLED_KEY "prov_mask"
static const char *const kIntervalKey[AIRCRAFT_PROVIDER_COUNT] = { "int_osky", "int_adsb" };

static const char *TAG = "AircraftProvider";

Aircraft gAircraft[MAX_AIRCRAFT];
int gAircraftCount = 0;

static AircraftProviderType activeProvider = AIRCRAFT_PROVIDER_OPENSKY; /* legacy single selection; = first enabled */
static ProviderDebugLevel debugLevel = PROVIDER_DEBUG_OFF;
static uint8_t enabledMask;      /* bit per AircraftProviderType; 0 = not loaded yet (derived from activeProvider) */
static bool haveStoredMask;
static uint32_t intervalSec[AIRCRAFT_PROVIDER_COUNT] = { PROVIDER_INTERVAL_DEFAULT_SEC, PROVIDER_INTERVAL_DEFAULT_SEC };
static bool haveStoredInterval[AIRCRAFT_PROVIDER_COUNT];
static uint32_t mergeMaxAgeMs[AIRCRAFT_PROVIDER_COUNT];

/* ---- naming / parsing ---- */

const char *AircraftProviderType_Name(AircraftProviderType type)
{
    switch (type) {
    case AIRCRAFT_PROVIDER_OPENSKY: return "OpenSky";
    case AIRCRAFT_PROVIDER_ADSBLOL: return "adsb.lol";
    default: return "Unknown";
    }
}

const char *AircraftProviderType_CsvName(AircraftProviderType type)
{
    switch (type) {
    case AIRCRAFT_PROVIDER_OPENSKY: return "OPENSKY";
    case AIRCRAFT_PROVIDER_ADSBLOL: return "ADSBLOL";
    default: return "OPENSKY";
    }
}

bool AircraftProviderType_Parse(const char *token, AircraftProviderType *out)
{
    if (!token || !out)
        return false;
    if (strcasecmp(token, "OPENSKY") == 0) { *out = AIRCRAFT_PROVIDER_OPENSKY; return true; }
    if (strcasecmp(token, "ADSBLOL") == 0 || strcasecmp(token, "ADSB.LOL") == 0) { *out = AIRCRAFT_PROVIDER_ADSBLOL; return true; }
    return false;
}

const char *ProviderDebugLevel_Name(ProviderDebugLevel level)
{
    switch (level) {
    case PROVIDER_DEBUG_OFF: return "Off";
    case PROVIDER_DEBUG_NORMAL: return "Normal";
    case PROVIDER_DEBUG_VERBOSE: return "Verbose";
    case PROVIDER_DEBUG_RAW: return "Raw";
    default: return "Off";
    }
}

bool ProviderDebugLevel_Parse(const char *token, ProviderDebugLevel *out)
{
    if (!token || !out)
        return false;
    if (strcasecmp(token, "OFF") == 0) { *out = PROVIDER_DEBUG_OFF; return true; }
    if (strcasecmp(token, "NORMAL") == 0) { *out = PROVIDER_DEBUG_NORMAL; return true; }
    if (strcasecmp(token, "VERBOSE") == 0) { *out = PROVIDER_DEBUG_VERBOSE; return true; }
    if (strcasecmp(token, "RAW") == 0) { *out = PROVIDER_DEBUG_RAW; return true; }
    return false;
}

/* ---- NVS-backed selection (reuses the "radar" namespace/infrastructure) ---- */

static void LoadSelection(void)
{
    nvs_handle_t handle;
    if (nvs_open(RADAR_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return;

    uint32_t stored = 0;
    if (nvs_get_u32(handle, PROVIDER_KEY, &stored) == ESP_OK &&
        stored < AIRCRAFT_PROVIDER_COUNT)
    {
        activeProvider = (AircraftProviderType)stored;
    }

    stored = 0;
    if (nvs_get_u32(handle, ENABLED_KEY, &stored) == ESP_OK &&
        (stored & AIRCRAFT_PROVIDER_ALL_MASK) != 0)
    {
        enabledMask = (uint8_t)(stored & AIRCRAFT_PROVIDER_ALL_MASK);
        haveStoredMask = true;
    }

    for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++) {
        stored = 0;
        if (nvs_get_u32(handle, kIntervalKey[p], &stored) == ESP_OK &&
            stored >= AircraftProvider_MinIntervalSecondsFor((AircraftProviderType)p) &&
            stored <= PROVIDER_INTERVAL_MAX_SEC)
        {
            intervalSec[p] = stored;
            haveStoredInterval[p] = true;
        }
    }

    stored = 0;
    if (nvs_get_u32(handle, DEBUG_LEVEL_KEY, &stored) == ESP_OK &&
        stored < PROVIDER_DEBUG_COUNT)
    {
        debugLevel = (ProviderDebugLevel)stored;
    }

    nvs_close(handle);
}

static void SaveU32(const char *key, uint32_t value)
{
    nvs_handle_t handle;
    if (nvs_open(RADAR_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return;
    nvs_set_u32(handle, key, value);
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Could not persist %s: %s", key, esp_err_to_name(err));
}

AircraftProviderType AircraftProvider_GetActive(void) { return activeProvider; }

static void SyncActiveFromMask(void)
{
    for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++)
        if (enabledMask & (1u << p)) {
            activeProvider = (AircraftProviderType)p;
            return;
        }
}

void AircraftProvider_SetActive(AircraftProviderType type)
{
    if (type >= AIRCRAFT_PROVIDER_COUNT)
        return;
    (void)AircraftProvider_SetEnabledMask((uint8_t)(1u << type));
}

/* ---- 0.0.28: independent providers ---- */

uint8_t AircraftProvider_EnabledMask(void) { return enabledMask; }

bool AircraftProvider_IsEnabled(AircraftProviderType type)
{
    return type < AIRCRAFT_PROVIDER_COUNT && (enabledMask & (1u << type)) != 0;
}

bool AircraftProvider_SetEnabledMask(uint8_t mask)
{
    mask &= AIRCRAFT_PROVIDER_ALL_MASK;
    if (!mask)
        return false; /* at least one provider */
    if (mask == enabledMask && haveStoredMask)
        return true;
    for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++)
        if (!(mask & (1u << p)))
            ProviderMerge_Forget((AircraftProviderType)p); /* a disabled provider's aircraft leave at once */
    enabledMask = mask;
    haveStoredMask = true;
    SyncActiveFromMask();
    SaveU32(ENABLED_KEY, mask);
    SaveU32(PROVIDER_KEY, (uint32_t)activeProvider); /* older firmware reads this one */
    ESP_LOGI(TAG, "Aircraft data providers: OpenSky %s, adsb.lol %s",
             (mask & 1u) ? "on" : "off", (mask & 2u) ? "on" : "off");
    return true;
}

uint32_t AircraftProvider_MinIntervalSecondsFor(AircraftProviderType type)
{
    /* OpenSky: unchanged 10 s floor (documented ~4000 requests/day budget).
     * adsb.lol: no published fixed minimum ("rate limits are dynamic based on
     * load", github.com/adsblol/api); 5 s on request, still one request per poll. */
    return type == AIRCRAFT_PROVIDER_ADSBLOL ? 5u : 10u;
}

uint32_t AircraftProvider_GetIntervalSeconds(AircraftProviderType type)
{
    return type < AIRCRAFT_PROVIDER_COUNT ? intervalSec[type] : PROVIDER_INTERVAL_DEFAULT_SEC;
}

bool AircraftProvider_SetIntervalSeconds(AircraftProviderType type, uint32_t seconds)
{
    if (type >= AIRCRAFT_PROVIDER_COUNT || seconds < AircraftProvider_MinIntervalSecondsFor(type) ||
        seconds > PROVIDER_INTERVAL_MAX_SEC)
        return false;
    if (intervalSec[type] == seconds && haveStoredInterval[type])
        return true; /* nothing to write */
    intervalSec[type] = seconds;
    haveStoredInterval[type] = true;
    SaveU32(kIntervalKey[type], seconds); /* only this provider's key */
    return true;
}

void AircraftProvider_MigrateLegacyInterval(uint32_t legacySeconds)
{
    /* Before 0.0.28 there was one provider selection ("aircraft_prov") and one
     * refresh interval ("refresh"). First boot of 0.0.28: the provider in use
     * stays the only enabled one and keeps that interval; the other provider
     * starts disabled with the default. Written once, then independent. */
    if (!haveStoredMask)
        (void)AircraftProvider_SetEnabledMask((uint8_t)(1u << activeProvider));
    for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++) {
        if (haveStoredInterval[p])
            continue;
        uint32_t v = (p == (int)activeProvider) ? legacySeconds : PROVIDER_INTERVAL_DEFAULT_SEC;
        const uint32_t lo = AircraftProvider_MinIntervalSecondsFor((AircraftProviderType)p);
        if (v < lo)
            v = lo;
        if (v > PROVIDER_INTERVAL_MAX_SEC)
            v = PROVIDER_INTERVAL_MAX_SEC;
        (void)AircraftProvider_SetIntervalSeconds((AircraftProviderType)p, v);
    }
}

void AircraftProvider_SetMergeMaxAgeMs(AircraftProviderType type, uint32_t ms)
{
    if (type < AIRCRAFT_PROVIDER_COUNT)
        mergeMaxAgeMs[type] = ms;
}

ProviderDebugLevel AircraftProvider_GetDebugLevel(void) { return debugLevel; }

void AircraftProvider_SetDebugLevel(ProviderDebugLevel level)
{
    if (level >= PROVIDER_DEBUG_COUNT)
        return;
    debugLevel = level;
    SaveU32(DEBUG_LEVEL_KEY, (uint32_t)level);
    ESP_LOGI(TAG, "Provider debug level set to %s", ProviderDebugLevel_Name(level));
}

/* ---- init / dispatch ---- */

void AircraftProvider_Init(void)
{
    LoadSelection();
    if (haveStoredMask)
        SyncActiveFromMask();
    else
        enabledMask = (uint8_t)(1u << activeProvider); /* until MigrateLegacyInterval persists it */
    OpenSky_Init();
    AdsbLol_Init();
    ESP_LOGI(TAG, "Providers initialized; active=%s debug=%s",
             AircraftProviderType_Name(activeProvider),
             ProviderDebugLevel_Name(debugLevel));
}

/* True when at least one enabled provider can poll (OpenSky with credentials,
 * or adsb.lol): the "configuration required" prompt is about that. */
bool AircraftProvider_HasCredentials(void)
{
    for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++)
        if (AircraftProvider_IsEnabled((AircraftProviderType)p) && AircraftProvider_HasCredentialsFor((AircraftProviderType)p))
            return true;
    return false;
}

bool AircraftProvider_HasCredentialsFor(AircraftProviderType type)
{
    switch (type) {
    case AIRCRAFT_PROVIDER_OPENSKY: return OpenSky_HasCredentials();
    case AIRCRAFT_PROVIDER_ADSBLOL: return AdsbLol_HasCredentials();
    default: return false;
    }
}

/* The longest remaining backoff among the enabled providers (the radar's
 * "API call limit exceeded" banner); AircraftProvider_RateLimitedProvider()
 * names that provider. */
uint32_t AircraftProvider_GetRateLimitSeconds(void)
{
    return AircraftProvider_GetRateLimitSecondsFor(AircraftProvider_RateLimitedProvider());
}

AircraftProviderType AircraftProvider_RateLimitedProvider(void)
{
    AircraftProviderType who = activeProvider;
    uint32_t longest = 0;
    for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++) {
        if (!AircraftProvider_IsEnabled((AircraftProviderType)p))
            continue;
        const uint32_t s = AircraftProvider_GetRateLimitSecondsFor((AircraftProviderType)p);
        if (s > longest) {
            longest = s;
            who = (AircraftProviderType)p;
        }
    }
    return who;
}

uint32_t AircraftProvider_GetRateLimitSecondsFor(AircraftProviderType type)
{
    switch (type) {
    case AIRCRAFT_PROVIDER_OPENSKY: return OpenSky_GetRateLimitSeconds();
    case AIRCRAFT_PROVIDER_ADSBLOL: return AdsbLol_GetRateLimitSeconds();
    default: return 0;
    }
}

static bool GetAircraftJsonRaw(AircraftProviderType type, float centerLat, float centerLon, float radiusKm, const char **json)
{
    switch (type) {
    case AIRCRAFT_PROVIDER_OPENSKY: return OpenSky_GetAircraftJson(centerLat, centerLon, radiusKm, json);
    case AIRCRAFT_PROVIDER_ADSBLOL: return AdsbLol_GetAircraftJson(centerLat, centerLon, radiusKm, json);
    default: return false;
    }
}

/* Same behaviour as before, plus recurring-operation telemetry (duration, failures, skips). A call made
 * while a 429 backoff is active is counted as skipped, not as a failure. */
bool AircraftProvider_GetAircraftJson(float centerLat, float centerLon, float radiusKm, const char **json)
{
    return AircraftProvider_GetAircraftJsonFor(activeProvider, centerLat, centerLon, radiusKm, json);
}

bool AircraftProvider_GetAircraftJsonFor(AircraftProviderType type, float centerLat, float centerLon, float radiusKm,
                                         const char **json)
{
    bool backoff = AircraftProvider_GetRateLimitSecondsFor(type) > 0;
    uint32_t t0 = DiagTelemetry_NowMs();
    bool ok = GetAircraftJsonRaw(type, centerLat, centerLon, radiusKm, json);
    if (!ok && backoff)
        DiagTelemetry_OpDone(DT_OP_PROVIDER_REFRESH, DT_RES_SKIPPED, 0, NULL);
    else
        DiagTelemetry_OpEnd(DT_OP_PROVIDER_REFRESH, t0, ok,
                            AircraftProvider_GetRateLimitSecondsFor(type) > 0
                                ? (type == AIRCRAFT_PROVIDER_ADSBLOL ? "adsb.lol rate limited (429)" : "OpenSky rate limited (429)")
                                : (type == AIRCRAFT_PROVIDER_ADSBLOL ? "adsb.lol request or response failed"
                                                                     : "OpenSky request or response failed"));
    return ok;
}

/* HOSTTEST:BEGIN dispatch (extracted verbatim by host_tests/visibility_test.c) */
bool AircraftProvider_ParseAircraft(const char *json)
{
    return AircraftProvider_ParseAircraftFor(activeProvider, json);
}

bool AircraftProvider_ParseAircraftFor(AircraftProviderType type, const char *json)
{
    /* The provider parsers admit aircraft through VisPolicy_Admit (the one
     * visibility decision); retention and per-airport counts follow a
     * successful parse only, so a failed poll keeps the previous list as before. */
    VisPolicy_BeginPoll();
    bool ok;
    switch (type) {
    case AIRCRAFT_PROVIDER_OPENSKY: ok = OpenSky_ParseAircraft(json); break;
    case AIRCRAFT_PROVIDER_ADSBLOL: ok = AdsbLol_ParseAircraft(json); break;
    default: ok = false; break;
    }
    if (!ok)
        return false;
    const uint32_t nowMs = xTaskGetTickCount() * portTICK_PERIOD_MS;
    /* 0.0.28: with more than one provider enabled, the list just parsed is
     * this provider's snapshot and gAircraft becomes the ICAO24-merged view of
     * every fresh snapshot (provider_merge.h). One provider: unchanged. */
    /* The list is kept as this provider's snapshot even while it is the only
     * provider, so switching the other one on later merges with it at once
     * instead of showing only the newcomer's aircraft until this provider's
     * next poll. A provider switched off while its request was in flight
     * contributes nothing: the list is rebuilt from the enabled providers. */
    const uint8_t mask = AircraftProvider_EnabledMask();
    const bool stored = ProviderMerge_Store(type, gAircraft, gAircraftCount, nowMs);
    bool combined = false, multi = false;
    if (stored && ((mask & (mask - 1u)) != 0 || !AircraftProvider_IsEnabled(type))) {
        multi = true;
        uint32_t maxAge[AIRCRAFT_PROVIDER_COUNT];
        for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++)
            maxAge[p] = AircraftProvider_IsEnabled((AircraftProviderType)p) ? AircraftProvider_MergeMaxAgeMs((AircraftProviderType)p) : 0;
        gAircraftCount = ProviderMerge_Build(gAircraft, MAX_AIRCRAFT, nowMs, maxAge, type);
        for (int p = 0; p < AIRCRAFT_PROVIDER_COUNT; p++)
            if (p != (int)type && ProviderMerge_IsFresh((AircraftProviderType)p, nowMs, maxAge[p]))
                combined = true;
    }
    VisPolicy_EndPollFrom(nowMs, multi ? (int)type : -1, combined);
    return true;
}
/* HOSTTEST:END dispatch */

uint32_t AircraftProvider_MinPollIntervalSeconds(void)
{
    return AircraftProvider_MinIntervalSecondsFor(activeProvider);
}

uint32_t AircraftProvider_MergeMaxAgeMs(AircraftProviderType type)
{
    /* Set by the poll loop (2 x the provider's effective interval); a
     * conservative fallback before the first schedule pass. */
    if (type >= AIRCRAFT_PROVIDER_COUNT)
        return 0;
    return mergeMaxAgeMs[type] ? mergeMaxAgeMs[type] : 2u * 1000u * intervalSec[type];
}

/* ---- diagnostics ---- */

void ProviderDiag_RequestStart(const char *provider, const char *what, const char *urlRedacted)
{
    if (debugLevel < PROVIDER_DEBUG_NORMAL) return;
    ESP_LOGI(TAG, "[%s] %s starting: %s", provider, what, urlRedacted ? urlRedacted : "");
}

void ProviderDiag_RequestDone(const char *provider, const char *what, int httpStatus, size_t bytes, bool ok)
{
    if (debugLevel < PROVIDER_DEBUG_NORMAL) return;
    ESP_LOGI(TAG, "[%s] %s done: status=%d bytes=%u result=%s",
             provider, what, httpStatus, (unsigned)bytes, ok ? "ok" : "failed");
}

void ProviderDiag_ParseResult(const char *provider, bool parseOk, int rawCount, int normalizedCount, int rejectedCount)
{
    if (debugLevel < PROVIDER_DEBUG_NORMAL) return;
    ESP_LOGI(TAG, "[%s] parse=%s raw=%d normalized=%d rejected=%d",
             provider, parseOk ? "ok" : "FAILED", rawCount, normalizedCount, rejectedCount);
}

void ProviderDiag_Warning(const char *provider, const char *msg)
{
    /* Warnings are always worth seeing once diagnostics are on at all. */
    if (debugLevel < PROVIDER_DEBUG_NORMAL) return;
    ESP_LOGW(TAG, "[%s] %s", provider, msg);
}

void ProviderDiag_TypeResolution(
    const char *icao24,
    const char *providerTypeRaw,
    bool hasHint,
    AircraftType hint,
    const char *registryOverrideName,
    AircraftType finalType)
{
    if (debugLevel < PROVIDER_DEBUG_VERBOSE) return;
    ESP_LOGI(TAG,
             "[type] icao=%s providerType='%s' hint=%s registryOverride=%s final=%s",
             icao24 ? icao24 : "",
             (providerTypeRaw && providerTypeRaw[0]) ? providerTypeRaw : "-",
             hasHint ? AircraftType_Name(hint) : "none",
             registryOverrideName ? registryOverrideName : "none",
             AircraftType_Name(finalType));
}

/* Masks the value following any of a small set of sensitive key names, so a
 * Raw-level dump can never leak OAuth tokens or client secrets even if a
 * provider's response body happened to echo request parameters back. */
/* Case-insensitive strstr (strcasestr is not standard C). */
static char *FindNoCase(char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, n) == 0)
            return hay;
    }
    return NULL;
}

/* Masks the value that follows any credential-looking key, in JSON
 * ("key":"value"), query-string (key=value) and header (Key: value) forms.
 * A quoted value is masked up to its closing quote, so multi-word values such
 * as "Bearer <token>" are fully hidden; an unquoted one ends at a delimiter.
 * The keys are matched case-insensitively. */
static void RedactSensitive(char *buf)
{
    static const char *sensitiveKeys[] = {
        "access_token", "refresh_token", "id_token", "token", "client_secret", "client_id",
        "password", "api_key", "apikey", "secret", "authorization"
    };
    for (size_t k = 0; k < sizeof(sensitiveKeys) / sizeof(sensitiveKeys[0]); k++) {
        char *p = buf;
        size_t keyLen = strlen(sensitiveKeys[k]);
        while ((p = FindNoCase(p, sensitiveKeys[k])) != NULL) {
            char *v = p + keyLen;
            if (*v == '"')
                v++; /* closing quote of a JSON key */
            while (*v == ' ')
                v++;
            if (*v != ':' && *v != '=') { /* just a word that contains the text, not key: value */
                p += keyLen;
                continue;
            }
            v++;
            while (*v == ' ')
                v++;
            bool quoted = (*v == '"');
            if (quoted)
                v++;
            /* "Authorization: Bearer <token>" has a space inside its value, so for
             * that key an unquoted value runs to the end of the line. */
            bool spaceEnds = (strcmp(sensitiveKeys[k], "authorization") != 0);
            while (*v && (quoted ? *v != '"' : (*v != '&' && *v != ',' && *v != '}' && (*v != ' ' || !spaceEnds) && *v != '\n' && *v != '\r' && *v != '"')))
                *v++ = '*';
            p += keyLen;
        }
    }
}

#define RAW_PREVIEW_MAX_BYTES 512

void ProviderDiag_RawPreview(const char *provider, const char *body, size_t bodyLen)
{
    if (debugLevel < PROVIDER_DEBUG_RAW) return;
    if (!body) return;

    size_t previewLen = bodyLen < RAW_PREVIEW_MAX_BYTES ? bodyLen : RAW_PREVIEW_MAX_BYTES;
    char preview[RAW_PREVIEW_MAX_BYTES + 1];
    memcpy(preview, body, previewLen);
    preview[previewLen] = '\0';

    RedactSensitive(preview);

    ESP_LOGI(TAG, "[%s] raw preview (%u of %u bytes)%s:\n%s",
             provider, (unsigned)previewLen, (unsigned)bodyLen,
             bodyLen > previewLen ? " [truncated]" : "", preview);
}
