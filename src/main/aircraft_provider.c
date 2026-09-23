#include "aircraft_provider.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "opensky_client.h"
#include "adsblol_client.h"

#define RADAR_NAMESPACE "radar"
#define PROVIDER_KEY "aircraft_prov"
#define DEBUG_LEVEL_KEY "prov_debug"

static const char *TAG = "AircraftProvider";

Aircraft gAircraft[MAX_AIRCRAFT];
int gAircraftCount = 0;

static AircraftProviderType activeProvider = AIRCRAFT_PROVIDER_OPENSKY;
static ProviderDebugLevel debugLevel = PROVIDER_DEBUG_OFF;

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

void AircraftProvider_SetActive(AircraftProviderType type)
{
    if (type >= AIRCRAFT_PROVIDER_COUNT)
        return;
    activeProvider = type;
    SaveU32(PROVIDER_KEY, (uint32_t)type);
    ESP_LOGI(TAG, "Active aircraft data provider set to %s", AircraftProviderType_Name(type));
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
    OpenSky_Init();
    AdsbLol_Init();
    ESP_LOGI(TAG, "Providers initialized; active=%s debug=%s",
             AircraftProviderType_Name(activeProvider),
             ProviderDebugLevel_Name(debugLevel));
}

bool AircraftProvider_HasCredentials(void)
{
    switch (activeProvider) {
    case AIRCRAFT_PROVIDER_OPENSKY: return OpenSky_HasCredentials();
    case AIRCRAFT_PROVIDER_ADSBLOL: return AdsbLol_HasCredentials();
    default: return false;
    }
}

uint32_t AircraftProvider_GetRateLimitSeconds(void)
{
    switch (activeProvider) {
    case AIRCRAFT_PROVIDER_OPENSKY: return OpenSky_GetRateLimitSeconds();
    case AIRCRAFT_PROVIDER_ADSBLOL: return AdsbLol_GetRateLimitSeconds();
    default: return 0;
    }
}

bool AircraftProvider_GetAircraftJson(float centerLat, float centerLon, float radiusKm, const char **json)
{
    switch (activeProvider) {
    case AIRCRAFT_PROVIDER_OPENSKY: return OpenSky_GetAircraftJson(centerLat, centerLon, radiusKm, json);
    case AIRCRAFT_PROVIDER_ADSBLOL: return AdsbLol_GetAircraftJson(centerLat, centerLon, radiusKm, json);
    default: return false;
    }
}

bool AircraftProvider_ParseAircraft(const char *json)
{
    switch (activeProvider) {
    case AIRCRAFT_PROVIDER_OPENSKY: return OpenSky_ParseAircraft(json);
    case AIRCRAFT_PROVIDER_ADSBLOL: return AdsbLol_ParseAircraft(json);
    default: return false;
    }
}

uint32_t AircraftProvider_MinPollIntervalSeconds(void)
{
    switch (activeProvider) {
    /* OpenSky: documented ~4000 req/day budget: the web UI already floors
     * the refresh interval at 10s, so this just backstops that. */
    case AIRCRAFT_PROVIDER_OPENSKY: return 10;
    /* adsb.lol: no published fixed minimum ("rate limits are dynamic based
     * on load... if you get 4xx errors, you are doing something wrong",
     * per github.com/adsblol/api). 10s matches OpenSky's floor and is a
     * conservative, community-friendly default for a free shared service. */
    case AIRCRAFT_PROVIDER_ADSBLOL: return 10;
    default: return 10;
    }
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
static void RedactSensitive(char *buf)
{
    static const char *sensitiveKeys[] = {
        "access_token", "client_secret", "authorization", "Authorization"
    };
    for (size_t k = 0; k < sizeof(sensitiveKeys) / sizeof(sensitiveKeys[0]); k++) {
        char *p = buf;
        size_t keyLen = strlen(sensitiveKeys[k]);
        while ((p = strstr(p, sensitiveKeys[k])) != NULL) {
            char *valueStart = p + keyLen;
            while (*valueStart == ':' || *valueStart == '=' || *valueStart == '"' || *valueStart == ' ')
                valueStart++;
            char *v = valueStart;
            while (*v && *v != '"' && *v != '&' && *v != ',' && *v != '}' && *v != ' ')
            {
                *v = '*';
                v++;
            }
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
