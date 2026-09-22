#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "craft_types.h"

/* Two user-editable lists, both plain CSV on the SPIFFS partition:
 *
 *   Registry (custom_rules.csv):   PREFIX,TYPE[,AIRCRAFT[,NOTES]] - explicit
 *                                  per-aircraft (registration / call sign)
 *                                  classification, plus the manual aircraft
 *                                  type (FIXED or HELI; optional, a missing
 *                                  one means Fixed-Wing) and optional
 *                                  free-text Notes (MAX_RULE_NOTES chars,
 *                                  no commas/quotes). VIP/special-mission
 *                                  prefixes (SAM, SPAR, EXEC, PAT -> Important)
 *                                  are seeded here as ordinary rows, not a
 *                                  separate mechanism.
 *   Operators (operators.csv):     ICAO,NAME,TYPE         - per-operator
 *                                  classification. Only *changes* to the
 *                                  built-in defaults and user-added operators
 *                                  are stored; the built-in table itself lives
 *                                  in flash (custom_rules.c) and is never
 *                                  regenerated over user edits.
 *
 * Lookup order (ResolveAircraft): registry -> built-in prefix defaults ->
 * operator -> fallback (Personal). The result is a CraftType (classification,
 * decides color) and an AircraftType (manual, decides shape); how they are
 * drawn comes from craft_types.c. Only registry rules carry an aircraft type;
 * everything else resolves to Fixed-Wing. */

#define MAX_CUSTOM_RULES 5000
#define MAX_RULE_PREFIX 15
#define MAX_RULE_NOTES 96 /* optional free-text context, e.g. "South Korea, LG Electronics" */
#define MAX_OPERATORS 1000
#define MAX_OPERATOR_CODE 4
#define MAX_OPERATOR_NAME 39

/* Registry CSV files carry "#FORMAT=2" (or higher) on their first line. A
 * file without any #FORMAT marker is a pre-Personal/Business/Cargo/Important
 * file, in which PRIVATE meant the small-aircraft category, so it is read as
 * Personal and rewritten. Version 3 adds the optional trailing Notes column;
 * a version-2 file loads with every rule's notes empty. */
#define CUSTOM_RULES_FORMAT_VERSION 3

#define CUSTOM_RULES_CSV_PATH "/spiffs/custom_rules.csv"
#define OPERATORS_CSV_PATH "/spiffs/operators.csv"
/* Old operator-extras file. Read once (only if operators.csv does not exist
 * yet) and migrated into operators.csv; never written again. */
#define COMMERCIAL_CODES_CSV_PATH "/spiffs/commercial_codes.csv"

typedef struct {
    char prefix[MAX_RULE_PREFIX + 1];
    CraftType type;
    AircraftType aircraftType;
    char notes[MAX_RULE_NOTES + 1]; /* optional, may be empty */
} CustomRule;

typedef struct {
    char code[MAX_OPERATOR_CODE + 1];
    char name[MAX_OPERATOR_NAME + 1];
    CraftType type;
    bool builtin;                        /* has a built-in default */
    bool modified;                       /* builtin whose name/type was changed */
    char defaultName[MAX_OPERATOR_NAME + 1]; /* valid when builtin */
    CraftType defaultType;                   /* valid when builtin */
} OperatorInfo;

typedef enum {
    CRAFT_SRC_REGISTRY = 0,
    CRAFT_SRC_PREFIX_DEFAULT,
    CRAFT_SRC_OPERATOR,
    CRAFT_SRC_FALLBACK
} CraftSource;

typedef struct {
    CraftType type;
    AircraftType aircraftType; /* manual designation; Fixed-Wing unless a registry rule says otherwise */
    CraftSource source;
    char operatorCode[MAX_OPERATOR_CODE + 1]; /* set when source == CRAFT_SRC_OPERATOR */
    char registryPrefix[MAX_RULE_PREFIX + 1]; /* matching rule, set when source == CRAFT_SRC_REGISTRY */
} CraftResolution;

/* Call after nvs_flash_init(), before the web server or OpenSky polling. */
bool CustomRules_Init(void);
/* Re-reads both CSV files from scratch (used after a bulk upload). */
bool CustomRules_Reload(void);

/* ---- registry ---- */
size_t CustomRules_Count(void);
bool CustomRules_Get(size_t index, CustomRule *out);
/* Exact (case-insensitive) lookup of one rule; used for duplicate detection. */
bool CustomRules_Find(const char *prefix, CustomRule *out);
/* notes may be NULL or empty (optional). add or update. */
bool CustomRules_Add(const char *prefix, CraftType type, AircraftType aircraftType, const char *notes);
bool CustomRules_Delete(const char *prefix);
bool CustomRules_NormalizePrefix(const char *input, char out[MAX_RULE_PREFIX + 1]);
/* Trims, then accepts 0..MAX_RULE_NOTES printable ASCII characters. Commas
 * and double quotes are rejected (same convention as Operators_NormalizeName)
 * so the CSV never needs quoting. NULL/empty input is valid - Notes is optional. */
bool CustomRules_NormalizeNotes(const char *input, char out[MAX_RULE_NOTES + 1]);

/* ---- operators: built-ins first (alphabetical), then custom (alphabetical) ---- */
size_t Operators_Count(void);
size_t Operators_BuiltinCount(void);
bool Operators_Get(size_t index, OperatorInfo *out);
bool Operators_Find(const char *code, OperatorInfo *out);
bool Operators_NormalizeCode(const char *input, char out[MAX_OPERATOR_CODE + 1]);
bool Operators_NormalizeName(const char *input, char out[MAX_OPERATOR_NAME + 1]);
/* Edits a built-in operator or adds/updates a custom one. */
bool Operators_Set(const char *code, const char *name, CraftType type);
/* Built-in only: discards the user's changes and returns to the default. */
bool Operators_RestoreDefault(const char *code);
/* Custom only: built-ins cannot be deleted (restore them instead). */
bool Operators_DeleteCustom(const char *code);

/* ---- lookup ---- */
CraftResolution ResolveAircraft(const char *callsign, const char *hex);
CraftType evaluateAircraftType(const char *callsign, const char *hex);
/* The single resolve -> classification -> aircraft type -> appearance path
 * used by every renderer (radar and web preview). */
CraftAppearance ResolveAircraftAppearance(const char *callsign, const char *hex);
const char *CraftSource_Name(CraftSource source);