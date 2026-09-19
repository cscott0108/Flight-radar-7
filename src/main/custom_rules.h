#pragma once

#include <stdbool.h>
#include <stddef.h>

#define MAX_CUSTOM_RULES 100
#define MAX_RULE_PREFIX 15
#define MAX_COMMERCIAL_RULES 50
#define MAX_COMMERCIAL_CODE 4

typedef enum {
    TYPE_PRIVATE = 0,
    TYPE_COMMERCIAL = 1,
    TYPE_POLICE = 2,
    TYPE_MILITARY = 3,
    TYPE_EMERGENCY = 4
} CustomType;

typedef struct {
    char prefix[MAX_RULE_PREFIX + 1];
    CustomType type;
} CustomRule;

/* Call after nvs_flash_init(), before the web server or OpenSky polling. */
bool CustomRules_Init(void);
size_t CustomRules_Count(void);
bool CustomRules_Get(size_t index, CustomRule *out);
bool CustomRules_Add(const char *prefix, CustomType type);
bool CustomRules_Delete(const char *prefix);
bool CustomRules_NormalizePrefix(const char *input, char out[MAX_RULE_PREFIX + 1]);
size_t CommercialRules_Count(void);
bool CommercialRules_Get(size_t index, char out[MAX_COMMERCIAL_CODE + 1]);
bool CommercialRules_Add(const char *code);
bool CommercialRules_Delete(const char *code);
bool CommercialRules_NormalizeCode(const char *input, char out[MAX_COMMERCIAL_CODE + 1]);
size_t CommercialBuiltIn_Count(void);
const char *CommercialBuiltIn_Get(size_t index);
CustomType evaluateAircraftType(const char *callsign, const char *hex);
const char *CustomType_Name(CustomType type);
