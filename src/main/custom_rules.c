#include "custom_rules.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define RULE_NAMESPACE "craft_rules"
#define RULE_KEY "rules"
#define RULE_VERSION 1
#define COMMERCIAL_NAMESPACE "commercial_cs"
#define COMMERCIAL_KEY "codes"
#define COMMERCIAL_VERSION 1

typedef struct {
    char prefix[MAX_RULE_PREFIX + 1];
    uint8_t type;
} StoredRule;

typedef struct {
    uint16_t version;
    uint16_t count;
    StoredRule rules[MAX_CUSTOM_RULES];
} StoredRules;

typedef struct {
    uint16_t version;
    uint16_t count;
    char codes[MAX_COMMERCIAL_RULES][MAX_COMMERCIAL_CODE + 1];
} StoredCommercialRules;

static StoredRules stored;
static StoredCommercialRules commercialStored;
static SemaphoreHandle_t rulesLock;

/* ICAO operator designators, curated from the FAA three-letter decode table.
 * The editable list below is separate from these built-in entries. */
static const char *const commercialBuiltIn[] = {
    "AAL", "AAR", "ACA", "AAY", "AEA", "AEE", "AFL", "AFR", "ANA", "ANZ",
    "ASA", "AWC", "BAW", "CAL", "CCA", "CES", "CMP", "CPA", "DAL", "DLH",
    "EIN", "EJA", "ELY", "ENY", "EVA", "EZY", "FDX", "FIN", "FFT", "GLO",
    "HAL", "IBE", "JAL", "JBU", "JZA", "KLM", "LOT", "QFA", "QTR", "RPA",
    "RYR", "SAS", "SIA", "SKW", "SWA", "TAP", "THY", "TSC", "TWY", "UAL",
    "UAE", "UPS", "VIR", "VOI", "WJA", "WZZ"
};

size_t CommercialBuiltIn_Count(void)
{
    return sizeof(commercialBuiltIn) / sizeof(commercialBuiltIn[0]);
}

const char *CommercialBuiltIn_Get(size_t index)
{
    return index < CommercialBuiltIn_Count() ? commercialBuiltIn[index] : NULL;
}

static bool SaveRules(const StoredRules *next)
{
    nvs_handle_t handle;
    if (nvs_open(RULE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    esp_err_t err = nvs_set_blob(handle, RULE_KEY, next, sizeof(*next));
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool SaveCommercialRules(const StoredCommercialRules *next)
{
    nvs_handle_t handle;
    if (nvs_open(COMMERCIAL_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    esp_err_t err = nvs_set_blob(handle, COMMERCIAL_KEY, next, sizeof(*next));
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool CommercialRules_NormalizeCode(const char *input, char out[MAX_COMMERCIAL_CODE + 1])
{
    if (!input || !out)
        return false;
    size_t length = strlen(input);
    if (length < 2 || length > MAX_COMMERCIAL_CODE)
        return false;
    for (size_t i = 0; i < length; i++) {
        char c = input[i];
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        if (c < 'A' || c > 'Z')
            return false;
        out[i] = c;
    }
    out[length] = '\0';
    return true;
}

bool CustomRules_NormalizePrefix(const char *input, char out[MAX_RULE_PREFIX + 1])
{
    if (!input || !out)
        return false;
    size_t length = strlen(input);
    if (length == 0 || length > MAX_RULE_PREFIX)
        return false;
    for (size_t i = 0; i < length; i++) {
        char c = input[i];
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '?'))
            return false;
        out[i] = c;
    }
    out[length] = '\0';
    return true;
}

bool CustomRules_Init(void)
{
    if (!rulesLock)
        rulesLock = xSemaphoreCreateMutex();
    if (!rulesLock)
        return false;
    memset(&stored, 0, sizeof(stored));
    stored.version = RULE_VERSION;
    memset(&commercialStored, 0, sizeof(commercialStored));
    commercialStored.version = COMMERCIAL_VERSION;

    /* Load both independent NVS blobs even when one has never been saved. */
    bool commercialValid = true;
    nvs_handle_t commercialHandle;
    esp_err_t commercialErr = nvs_open(COMMERCIAL_NAMESPACE, NVS_READONLY, &commercialHandle);
    if (commercialErr == ESP_OK) {
        StoredCommercialRules loaded;
        size_t commercialSize = sizeof(loaded);
        commercialErr = nvs_get_blob(commercialHandle, COMMERCIAL_KEY, &loaded, &commercialSize);
        nvs_close(commercialHandle);
        if (commercialErr == ESP_OK) {
            commercialValid = commercialSize == sizeof(loaded) &&
                loaded.version == COMMERCIAL_VERSION && loaded.count <= MAX_COMMERCIAL_RULES;
            for (size_t i = 0; commercialValid && i < loaded.count; i++) {
                char normalized[MAX_COMMERCIAL_CODE + 1];
                commercialValid = memchr(loaded.codes[i], '\0', sizeof(loaded.codes[i])) &&
                    CommercialRules_NormalizeCode(loaded.codes[i], normalized) &&
                    strcmp(loaded.codes[i], normalized) == 0;
            }
            if (commercialValid)
                commercialStored = loaded;
        } else if (commercialErr != ESP_ERR_NVS_NOT_FOUND) {
            commercialValid = false;
        }
    } else if (commercialErr != ESP_ERR_NVS_NOT_FOUND) {
        commercialValid = false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(RULE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return commercialValid;
    if (err != ESP_OK)
        return false;
    StoredRules *loaded = malloc(sizeof(*loaded));
    if (!loaded) {
        nvs_close(handle);
        return false;
    }
    size_t size = sizeof(*loaded);
    err = nvs_get_blob(handle, RULE_KEY, loaded, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        free(loaded);
        return commercialValid;
    }
    bool valid = err == ESP_OK && size == sizeof(*loaded) &&
                 loaded->version == RULE_VERSION && loaded->count <= MAX_CUSTOM_RULES;
    if (valid) {
        for (size_t i = 0; i < loaded->count; i++) {
            char normalized[MAX_RULE_PREFIX + 1];
            if (!memchr(loaded->rules[i].prefix, '\0', sizeof(loaded->rules[i].prefix)) ||
                !CustomRules_NormalizePrefix(loaded->rules[i].prefix, normalized) ||
                strcmp(loaded->rules[i].prefix, normalized) != 0 ||
                loaded->rules[i].type > TYPE_EMERGENCY) {
                valid = false;
                break;
            }
        }
    }
    if (valid)
        stored = *loaded;
    free(loaded);
    return valid && commercialValid;
}

size_t CustomRules_Count(void)
{
    if (!rulesLock)
        return 0;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    size_t count = stored.count;
    xSemaphoreGive(rulesLock);
    return count;
}

bool CustomRules_Get(size_t index, CustomRule *out)
{
    if (!rulesLock || !out)
        return false;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    bool found = index < stored.count;
    if (found) {
        strcpy(out->prefix, stored.rules[index].prefix);
        out->type = (CustomType)stored.rules[index].type;
    }
    xSemaphoreGive(rulesLock);
    return found;
}

bool CustomRules_Add(const char *prefix, CustomType type)
{
    char normalized[MAX_RULE_PREFIX + 1];
    if (!rulesLock || !CustomRules_NormalizePrefix(prefix, normalized) ||
        type > TYPE_EMERGENCY)
        return false;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    StoredRules *next = malloc(sizeof(*next));
    if (!next) {
        xSemaphoreGive(rulesLock);
        return false;
    }
    *next = stored;
    size_t i = 0;
    for (; i < next->count; i++)
        if (strcmp(next->rules[i].prefix, normalized) == 0)
            break;
    if (i == next->count) {
        if (next->count == MAX_CUSTOM_RULES) {
            free(next);
            xSemaphoreGive(rulesLock);
            return false;
        }
        next->count++;
    }
    strcpy(next->rules[i].prefix, normalized);
    next->rules[i].type = (uint8_t)type;
    bool saved = SaveRules(next);
    if (saved)
        stored = *next;
    free(next);
    xSemaphoreGive(rulesLock);
    return saved;
}

bool CustomRules_Delete(const char *prefix)
{
    char normalized[MAX_RULE_PREFIX + 1];
    if (!rulesLock || !CustomRules_NormalizePrefix(prefix, normalized))
        return false;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    size_t i = 0;
    for (; i < stored.count; i++)
        if (strcmp(stored.rules[i].prefix, normalized) == 0)
            break;
    if (i == stored.count) {
        xSemaphoreGive(rulesLock);
        return false;
    }
    StoredRules *next = malloc(sizeof(*next));
    if (!next) {
        xSemaphoreGive(rulesLock);
        return false;
    }
    *next = stored;
    for (size_t j = i + 1; j < next->count; j++)
        next->rules[j - 1] = next->rules[j];
    memset(&next->rules[--next->count], 0, sizeof(next->rules[0]));
    bool saved = SaveRules(next);
    if (saved)
        stored = *next;
    free(next);
    xSemaphoreGive(rulesLock);
    return saved;
}

size_t CommercialRules_Count(void)
{
    if (!rulesLock)
        return 0;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    size_t count = commercialStored.count;
    xSemaphoreGive(rulesLock);
    return count;
}

bool CommercialRules_Get(size_t index, char out[MAX_COMMERCIAL_CODE + 1])
{
    if (!rulesLock || !out)
        return false;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    bool found = index < commercialStored.count;
    if (found)
        strcpy(out, commercialStored.codes[index]);
    xSemaphoreGive(rulesLock);
    return found;
}

bool CommercialRules_Add(const char *code)
{
    char normalized[MAX_COMMERCIAL_CODE + 1];
    if (!rulesLock || !CommercialRules_NormalizeCode(code, normalized))
        return false;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    StoredCommercialRules next = commercialStored;
    for (size_t i = 0; i < next.count; i++) {
        if (strcmp(next.codes[i], normalized) == 0) {
            xSemaphoreGive(rulesLock);
            return true;
        }
    }
    if (next.count >= MAX_COMMERCIAL_RULES) {
        xSemaphoreGive(rulesLock);
        return false;
    }
    strcpy(next.codes[next.count++], normalized);
    bool saved = SaveCommercialRules(&next);
    if (saved)
        commercialStored = next;
    xSemaphoreGive(rulesLock);
    return saved;
}

bool CommercialRules_Delete(const char *code)
{
    char normalized[MAX_COMMERCIAL_CODE + 1];
    if (!rulesLock || !CommercialRules_NormalizeCode(code, normalized))
        return false;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    StoredCommercialRules next = commercialStored;
    size_t i = 0;
    while (i < next.count && strcmp(next.codes[i], normalized) != 0)
        i++;
    if (i == next.count) {
        xSemaphoreGive(rulesLock);
        return false;
    }
    for (size_t j = i + 1; j < next.count; j++)
        strcpy(next.codes[j - 1], next.codes[j]);
    memset(next.codes[--next.count], 0, sizeof(next.codes[0]));
    bool saved = SaveCommercialRules(&next);
    if (saved)
        commercialStored = next;
    xSemaphoreGive(rulesLock);
    return saved;
}

static bool StartsWith(const char *value, const char *prefix)
{
    for (; *prefix; value++, prefix++) {
        char c = *value;
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        if (c != *prefix)
            return false;
    }
    return true;
}

static bool MatchesRulePattern(const char *value, const char *pattern)
{
    for (; *pattern; value++, pattern++) {
        char c = *value;
        if (!c)
            return false;
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        if (*pattern == '?' && !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
        if (*pattern != '?' && c != *pattern)
            return false;
    }
    return true;
}

CustomType evaluateAircraftType(const char *callsign, const char *hex)
{
    (void)hex; /* Reserved for future ICAO address rules. */
    if (!callsign)
        callsign = "";
    while (*callsign == ' ')
        callsign++;

    CustomType result = TYPE_PRIVATE;
    size_t longest = 0;
    if (rulesLock) {
        xSemaphoreTake(rulesLock, portMAX_DELAY);
        for (size_t i = 0; i < stored.count; i++) {
            size_t length = strlen(stored.rules[i].prefix);
            if (length > longest && MatchesRulePattern(callsign, stored.rules[i].prefix)) {
                result = (CustomType)stored.rules[i].type;
                longest = length;
            }
        }
        xSemaphoreGive(rulesLock);
    }
    if (longest)
        return result;

    static const struct { const char *prefix; CustomType type; } defaults[] = {
        {"V1LLAN", TYPE_MILITARY}, {"S1NFRD", TYPE_EMERGENCY},
        {"RESCUE", TYPE_MILITARY}, {"NAVY", TYPE_MILITARY},
        {"ADF", TYPE_MILITARY}, {"ARMY", TYPE_MILITARY},
        {"RCH", TYPE_MILITARY},
        {"CHP", TYPE_POLICE}, {"PD", TYPE_POLICE},
        {"POLICE", TYPE_POLICE},
        {"MED", TYPE_EMERGENCY}, {"LIFE", TYPE_EMERGENCY},
        {"EMS", TYPE_EMERGENCY}, {"AIRAMB", TYPE_EMERGENCY}
    };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
        if (StartsWith(callsign, defaults[i].prefix))
            return defaults[i].type;
    if (rulesLock) {
        xSemaphoreTake(rulesLock, portMAX_DELAY);
        for (size_t i = 0; i < commercialStored.count; i++) {
            if (StartsWith(callsign, commercialStored.codes[i])) {
                xSemaphoreGive(rulesLock);
                return TYPE_COMMERCIAL;
            }
        }
        xSemaphoreGive(rulesLock);
    }
    for (size_t i = 0; i < CommercialBuiltIn_Count(); i++)
        if (StartsWith(callsign, commercialBuiltIn[i]))
            return TYPE_COMMERCIAL;
    return TYPE_PRIVATE;
}

const char *CustomType_Name(CustomType type)
{
    switch (type) {
    case TYPE_PRIVATE: return "Private";
    case TYPE_COMMERCIAL: return "Commercial";
    case TYPE_POLICE: return "Police";
    case TYPE_MILITARY: return "Military";
    case TYPE_EMERGENCY: return "Emergency Services";
    default: return "Private";
    }
}
