#pragma once
/*
 * Persistent feature / subsystem switches and the WebUI Dark Mode flag.
 *
 * Stored in the existing NVS namespace "radar" (same store the other radar
 * settings use) as one u8 per key. A missing key means the default, so a device
 * upgraded from older firmware keeps every feature ON and the light theme.
 *
 * These are subsystem controls, NOT a radar shutdown: provider polling,
 * normalization, gAircraft tracking and display never consult them.
 */
#include <stdbool.h>

typedef enum {
    FEATURE_SEEN = 0,        /* Seen logging (new records / persistence work) */
    FEATURE_CURRENT,         /* Current Aircraft WebUI page processing        */
    FEATURE_REGISTERED,      /* Registered Aircraft (registry rule matching)  */
    FEATURE_OPERATORS,       /* Registered Operators (operator matching)      */
    FEATURE_DARK_MODE,       /* WebUI dark theme (presentation only)          */
    FEATURE_COUNT
} FeatureId;

/* Loads all flags from NVS once at boot (after nvs_flash_init). Safe to skip:
 * the compiled-in defaults are ON/ON/ON/ON/dark-OFF. */
void Features_Init(void);
bool Features_Get(FeatureId id);
/* Persists (only when the value changes) and applies immediately. */
bool Features_Set(FeatureId id, bool on);
const char *Features_Name(FeatureId id);

#define Features_SeenEnabled()       Features_Get(FEATURE_SEEN)
#define Features_CurrentEnabled()    Features_Get(FEATURE_CURRENT)
#define Features_RegisteredEnabled() Features_Get(FEATURE_REGISTERED)
#define Features_OperatorsEnabled()  Features_Get(FEATURE_OPERATORS)
#define Features_DarkMode()          Features_Get(FEATURE_DARK_MODE)
