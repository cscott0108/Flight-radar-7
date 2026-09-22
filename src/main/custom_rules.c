#include "custom_rules.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_spiffs.h"
#include "esp_log.h"

static const char *TAG = "CustomRules";

/* ---------------------------------------------------------------------------
 * Built-in operator defaults (flash, read-only). ICAO code, operator name,
 * default craft type. Sorted by code. User edits never modify this table;
 * they are stored as override rows in operators.csv and removed again by
 * "Restore default".
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *code;
    const char *name;
    CraftType type;
} BuiltinOperator;

static const BuiltinOperator builtinOperators[] = {
    {"AAL", "American Airlines", CRAFT_COMMERCIAL},
    {"AAR", "Asiana Airlines", CRAFT_COMMERCIAL},
    {"AAY", "Allegiant Air", CRAFT_COMMERCIAL},
    {"ABX", "ABX Air", CRAFT_CARGO},
    {"ACA", "Air Canada", CRAFT_COMMERCIAL},
    {"AEA", "Air Europa", CRAFT_COMMERCIAL},
    {"AEE", "Aegean Airlines", CRAFT_COMMERCIAL},
    {"AFL", "Aeroflot", CRAFT_COMMERCIAL},
    {"AFR", "Air France", CRAFT_COMMERCIAL},
    {"ANA", "All Nippon Airways", CRAFT_COMMERCIAL},
    {"ANZ", "Air New Zealand", CRAFT_COMMERCIAL},
    {"ASA", "Alaska Airlines", CRAFT_COMMERCIAL},
    {"ATI", "Air Transport International", CRAFT_CARGO},
    {"AWC", "Titan Airways", CRAFT_COMMERCIAL},
    {"BAW", "British Airways", CRAFT_COMMERCIAL},
    {"CAL", "China Airlines", CRAFT_COMMERCIAL},
    {"CAO", "Air China Cargo", CRAFT_CARGO},
    {"CCA", "Air China", CRAFT_COMMERCIAL},
    {"CES", "China Eastern Airlines", CRAFT_COMMERCIAL},
    {"CMP", "Copa Airlines", CRAFT_COMMERCIAL},
    {"CPA", "Cathay Pacific", CRAFT_COMMERCIAL},
    {"DAL", "Delta Air Lines", CRAFT_COMMERCIAL},
    {"DLH", "Lufthansa", CRAFT_COMMERCIAL},
    {"EIN", "Aer Lingus", CRAFT_COMMERCIAL},
    {"EJA", "NetJets", CRAFT_COMMERCIAL},
    {"ELY", "El Al", CRAFT_COMMERCIAL},
    {"ENY", "Envoy Air", CRAFT_COMMERCIAL},
    {"EVA", "EVA Air", CRAFT_COMMERCIAL},
    {"EZY", "easyJet", CRAFT_COMMERCIAL},
    {"FDX", "FedEx", CRAFT_CARGO},
    {"FFT", "Frontier Airlines", CRAFT_COMMERCIAL},
    {"FIN", "Finnair", CRAFT_COMMERCIAL},
    {"GLO", "Gol Linhas Aereas", CRAFT_COMMERCIAL},
    {"GTI", "Atlas Air", CRAFT_CARGO},
    {"HAL", "Hawaiian Airlines", CRAFT_COMMERCIAL},
    {"IBE", "Iberia", CRAFT_COMMERCIAL},
    {"JAL", "Japan Airlines", CRAFT_COMMERCIAL},
    {"JBU", "JetBlue Airways", CRAFT_COMMERCIAL},
    {"JZA", "Jazz Aviation", CRAFT_COMMERCIAL},
    {"KLM", "KLM Royal Dutch Airlines", CRAFT_COMMERCIAL},
    {"LOT", "LOT Polish Airlines", CRAFT_COMMERCIAL},
    {"QFA", "Qantas", CRAFT_COMMERCIAL},
    {"QTR", "Qatar Airways", CRAFT_COMMERCIAL},
    {"RPA", "Republic Airways", CRAFT_COMMERCIAL},
    {"RYR", "Ryanair", CRAFT_COMMERCIAL},
    {"SAS", "Scandinavian Airlines", CRAFT_COMMERCIAL},
    {"SIA", "Singapore Airlines", CRAFT_COMMERCIAL},
    {"SKW", "SkyWest Airlines", CRAFT_COMMERCIAL},
    {"SWA", "Southwest Airlines", CRAFT_COMMERCIAL},
    {"TAP", "TAP Air Portugal", CRAFT_COMMERCIAL},
    {"THY", "Turkish Airlines", CRAFT_COMMERCIAL},
    {"TSC", "Air Transat", CRAFT_COMMERCIAL},
    {"TWY", "TWY (name not set)", CRAFT_COMMERCIAL},
    {"UAE", "Emirates", CRAFT_COMMERCIAL},
    {"UAL", "United Airlines", CRAFT_COMMERCIAL},
    {"UPS", "UPS Airlines", CRAFT_CARGO},
    {"VIR", "Virgin Atlantic", CRAFT_COMMERCIAL},
    {"VOI", "Volaris", CRAFT_COMMERCIAL},
    {"WJA", "WestJet", CRAFT_COMMERCIAL},
    {"WZZ", "Wizz Air", CRAFT_COMMERCIAL},
};
#define BUILTIN_COUNT (sizeof(builtinOperators) / sizeof(builtinOperators[0]))

/* Extra operator codes the project already carried in its commercial-codes
 * list (all Commercial, no authoritative names). Used only to seed a
 * device that has neither operators.csv nor commercial_codes.csv. CAO and ABX
 * are built-ins now, so they are not repeated here. */
static const char *const seedCommercialExtras[] = {
    "BYF", "WUP", "XE", "JSX", "ROU", "LND", "WCC", "VJA", "CNS", "SCX", "FTD",
    "QXE", "MXY", "CKS", "PXT", "FBU", "AMX", "KAL", "TAI", "XAFL", "AIH",
    "APZ", "MNL", "DLX"
};

/* Registry entries seeded on a device with no custom_rules.csv at all, and
 * re-checked (added if still missing) on every migration to a newer format -
 * see SeedMissingRegistryDefaults(). The VIP/special-mission prefixes are
 * ordinary registry rows like any other: the existing longest-prefix-match
 * precedence already means a more specific entry (e.g. "SAM123") overrides
 * a shorter generic one ("SAM") with no separate rule tier needed. These are
 * defaults only - fully editable/removable through the webserver. */
static const struct { const char *prefix; CraftType type; const char *notes; } seedRegistry[] = {
    {"NGF", CRAFT_EMERGENCY, ""}, {"REH", CRAFT_EMERGENCY, ""}, {"N248PH", CRAFT_EMERGENCY, ""},
    {"N145TN", CRAFT_EMERGENCY, ""}, {"STNFORD1", CRAFT_EMERGENCY, ""}, {"N743AM", CRAFT_EMERGENCY, ""},
    {"N408SD", CRAFT_POLICE, ""}, {"YEL7", CRAFT_COMMERCIAL, ""}, {"ASY", CRAFT_PERSONAL, ""},
    {"C6559", CRAFT_MILITARY, ""}, {"PFT144", CRAFT_IMPORTANT, ""},
    {"SAM", CRAFT_IMPORTANT, "Special Air Mission / VIP transport"},
    {"SPAR", CRAFT_IMPORTANT, "Special Air Mission / VIP transport"},
    {"EXEC", CRAFT_IMPORTANT, "Executive/government transport"},
    {"PAT", CRAFT_IMPORTANT, "Patriot / VIP transport"},
};

/* ---- in-memory state ---- */

typedef struct {
    char code[MAX_OPERATOR_CODE + 1];
    char name[MAX_OPERATOR_NAME + 1];
    CraftType type;
} OperatorRow;

static CustomRule *rules = NULL;
static size_t rulesCount = 0;
static size_t rulesCapacity = 0;

/* User rows, sorted by code: overrides of built-ins plus custom operators. */
static OperatorRow *opRows = NULL;
static size_t opCount = 0;
static size_t opCapacity = 0;

static SemaphoreHandle_t rulesLock;
static bool spiffsMounted = false;

/* ---- helpers ---- */

static char UpperChar(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

bool CustomRules_NormalizePrefix(const char *input, char out[MAX_RULE_PREFIX + 1])
{
    if (!input || !out)
        return false;
    size_t length = strlen(input);
    if (length == 0 || length > MAX_RULE_PREFIX)
        return false;
    for (size_t i = 0; i < length; i++) {
        char c = UpperChar(input[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '?'))
            return false;
        out[i] = c;
    }
    out[length] = '\0';
    return true;
}

bool Operators_NormalizeCode(const char *input, char out[MAX_OPERATOR_CODE + 1])
{
    if (!input || !out)
        return false;
    size_t length = strlen(input);
    if (length < 2 || length > MAX_OPERATOR_CODE)
        return false;
    for (size_t i = 0; i < length; i++) {
        char c = UpperChar(input[i]);
        if (c < 'A' || c > 'Z')
            return false;
        out[i] = c;
    }
    out[length] = '\0';
    return true;
}

/* Trims, then accepts 1..MAX_OPERATOR_NAME printable ASCII characters. Commas
 * and double quotes are rejected so the CSV never needs quoting. */
bool Operators_NormalizeName(const char *input, char out[MAX_OPERATOR_NAME + 1])
{
    if (!input || !out)
        return false;
    while (*input == ' ' || *input == '\t')
        input++;
    size_t length = strlen(input);
    while (length > 0 && (input[length - 1] == ' ' || input[length - 1] == '\t'))
        length--;
    if (length == 0 || length > MAX_OPERATOR_NAME)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c < 0x20 || c > 0x7E || c == ',' || c == '"')
            return false;
        out[i] = (char)c;
    }
    out[length] = '\0';
    return true;
}

/* Trims, then accepts 0..MAX_RULE_NOTES printable ASCII characters. Commas
 * and double quotes are rejected, same convention as Operators_NormalizeName,
 * so the CSV never needs quoting. Empty is valid - Notes is optional. */
bool CustomRules_NormalizeNotes(const char *input, char out[MAX_RULE_NOTES + 1])
{
    if (!out)
        return false;
    if (!input)
        input = "";
    while (*input == ' ' || *input == '\t')
        input++;
    size_t length = strlen(input);
    while (length > 0 && (input[length - 1] == ' ' || input[length - 1] == '\t'))
        length--;
    if (length > MAX_RULE_NOTES)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c < 0x20 || c > 0x7E || c == ',' || c == '"')
            return false;
        out[i] = (char)c;
    }
    out[length] = '\0';
    return true;
}

static char *TrimInPlace(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t length = strlen(s);
    while (length > 0 && (s[length - 1] == ' ' || s[length - 1] == '\t')) {
        s[length - 1] = '\0';
        length--;
    }
    return s;
}

static bool MountSpiffsIfNeeded(void)
{
    if (spiffsMounted)
        return true;

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL, // use the one and only "spiffs" partition from the partition table
        .max_files = 6,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // ESP_ERR_INVALID_STATE = already registered
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s) - custom rules will not persist", esp_err_to_name(err));
        return false;
    }

    spiffsMounted = true;
    return true;
}

static bool FileExists(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    fclose(f);
    return true;
}

/* ---- registry storage ---- */

static bool EnsureRulesCapacity(size_t needed)
{
    if (needed <= rulesCapacity)
        return true;
    if (needed > MAX_CUSTOM_RULES)
        return false;
    size_t newCapacity = rulesCapacity == 0 ? 16 : rulesCapacity * 2;
    if (newCapacity < needed)
        newCapacity = needed;
    if (newCapacity > MAX_CUSTOM_RULES)
        newCapacity = MAX_CUSTOM_RULES;
    CustomRule *grown = realloc(rules, newCapacity * sizeof(*grown));
    if (!grown)
        return false;
    rules = grown;
    rulesCapacity = newCapacity;
    return true;
}

static int FindRule(const char *prefix)
{
    for (size_t i = 0; i < rulesCount; i++)
        if (!strcmp(rules[i].prefix, prefix))
            return (int)i;
    return -1;
}

/* In-memory upsert without saving. notes may be NULL (treated as empty). */
static bool UpsertRule(const char *prefix, CraftType type, AircraftType aircraftType, const char *notes)
{
    int i = FindRule(prefix);
    if (i < 0) {
        if (!EnsureRulesCapacity(rulesCount + 1))
            return false;
        i = (int)rulesCount++;
        strcpy(rules[i].prefix, prefix);
    }
    rules[i].type = type;
    rules[i].aircraftType = aircraftType;
    strncpy(rules[i].notes, notes ? notes : "", MAX_RULE_NOTES);
    rules[i].notes[MAX_RULE_NOTES] = '\0';
    return true;
}

/* Adds any of seedRegistry[] not already present, without touching existing
 * rows (including ones the user has since edited or deleted). Used both to
 * populate a brand-new device and to backfill the VIP defaults into an
 * existing installation migrating to format 3. */
static void SeedMissingRegistryDefaults(void)
{
    for (size_t i = 0; i < sizeof(seedRegistry) / sizeof(seedRegistry[0]); i++) {
        if (FindRule(seedRegistry[i].prefix) < 0)
            UpsertRule(seedRegistry[i].prefix, seedRegistry[i].type, AIRCRAFT_FIXED_WING,
                       seedRegistry[i].notes);
    }
}

static bool SaveRulesToCsv(void)
{
    FILE *f = fopen(CUSTOM_RULES_CSV_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Could not open %s for writing", CUSTOM_RULES_CSV_PATH);
        return false;
    }

    fprintf(f, "#FORMAT=%d\n", CUSTOM_RULES_FORMAT_VERSION);
    fprintf(f, "# Flight Radar registry rules (aircraft-specific craft types).\n");
    fprintf(f, "# One rule per line: PREFIX,TYPE,AIRCRAFT,NOTES\n");
    fprintf(f, "# TYPE is one of PERSONAL, PRIVATE, BUSINESS, COMMERCIAL, CARGO, MILITARY (MIL),\n");
    fprintf(f, "# POLICE (LEO), EMERGENCY (ES), INTERESTING (INT), IMPORTANT.\n");
    fprintf(f, "# AIRCRAFT is FIXED or HELI (manual designation; optional, a missing value means FIXED).\n");
    fprintf(f, "# NOTES is free text, optional, up to %d characters, no commas or quotes.\n", MAX_RULE_NOTES);
    fprintf(f, "# Prefix matches ignore case and include any following flight number.\n");
    fprintf(f, "# Use ? for one unknown letter or digit (e.g. S?NFRD). The longest matching rule wins.\n");
    fprintf(f, "# Keep the #FORMAT=%d line: a file without any #FORMAT marker is read as an old file\n", CUSTOM_RULES_FORMAT_VERSION);
    fprintf(f, "# where PRIVATE meant PERSONAL; a #FORMAT=2 file loads with every rule's notes empty.\n");

    for (size_t i = 0; i < rulesCount; i++)
        fprintf(f, "%s,%s,%s,%s\n", rules[i].prefix, CraftType_CsvName(rules[i].type),
                AircraftType_CsvName(rules[i].aircraftType), rules[i].notes);

    fclose(f);
    return true;
}

/* PRIVATE meant the small-aircraft category (now Personal) only in files
 * predating format 2. This is a fixed historical cutoff, deliberately NOT
 * tied to CUSTOM_RULES_FORMAT_VERSION - bumping the format version further
 * (e.g. for Notes, format 3) must never re-trigger this remap for files that
 * already use post-format-2 semantics. */
#define CUSTOM_RULES_LEGACY_PRIVATE_BEFORE_VERSION 2

/* Loads rules[] fresh. Malformed lines are skipped (not fatal). Returns true
 * if the file needs rewriting to the current format (older than it, in any
 * respect - not just the legacy-PRIVATE cutoff above). */
static bool LoadRulesFromCsv(void)
{
    rulesCount = 0;

    FILE *f = fopen(CUSTOM_RULES_CSV_PATH, "r");
    if (!f)
        return false;

    int version = 1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *trimmed = TrimInPlace(line);
        if (!strncmp(trimmed, "#FORMAT=", 8)) {
            version = atoi(trimmed + 8);
            continue;
        }
        if (*trimmed == '\0' || *trimmed == '#')
            continue;

        char *comma = strchr(trimmed, ',');
        if (!comma)
            continue;
        *comma = '\0';

        /* Optional third column: the manual aircraft type. A missing column is
         * Fixed-Wing (older files); an unrecognized value also falls back to
         * Fixed-Wing so a typo cannot delete the classification.
         * Optional fourth column (format 3+): free-text notes. A missing or
         * invalid value falls back to empty rather than dropping the rule. */
        char *typeField = comma + 1;
        AircraftType aircraftType = AIRCRAFT_FIXED_WING;
        char notesField[MAX_RULE_NOTES + 1] = "";
        char *second = strchr(typeField, ',');
        if (second) {
            *second = '\0';
            char *aircraftField = second + 1;
            char *third = strchr(aircraftField, ',');
            if (third) {
                *third = '\0';
                if (!CustomRules_NormalizeNotes(TrimInPlace(third + 1), notesField)) {
                    notesField[0] = '\0';
                    ESP_LOGW(TAG, "Invalid notes on rule '%s'; leaving blank", trimmed);
                }
            }
            if (!AircraftType_Parse(TrimInPlace(aircraftField), &aircraftType)) {
                aircraftType = AIRCRAFT_FIXED_WING;
                ESP_LOGW(TAG, "Unknown aircraft type on rule '%s'; using Fixed-Wing", trimmed);
            }
        }

        char normalizedPrefix[MAX_RULE_PREFIX + 1];
        CraftType type;
        if (!CustomRules_NormalizePrefix(TrimInPlace(trimmed), normalizedPrefix) ||
            !CraftType_Parse(TrimInPlace(typeField), version < CUSTOM_RULES_LEGACY_PRIVATE_BEFORE_VERSION, &type))
            continue;

        if (!UpsertRule(normalizedPrefix, type, aircraftType, notesField))
            break;
    }
    fclose(f);
    return version < CUSTOM_RULES_FORMAT_VERSION;
}

/* ---- operator storage ---- */

static int BuiltinFind(const char *code)
{
    for (size_t i = 0; i < BUILTIN_COUNT; i++)
        if (!strcmp(builtinOperators[i].code, code))
            return (int)i;
    return -1;
}

static int FindRow(const char *code)
{
    for (size_t i = 0; i < opCount; i++)
        if (!strcmp(opRows[i].code, code))
            return (int)i;
    return -1;
}

static bool EnsureOpCapacity(size_t needed)
{
    if (needed <= opCapacity)
        return true;
    if (needed > MAX_OPERATORS)
        return false;
    size_t newCapacity = opCapacity == 0 ? 16 : opCapacity * 2;
    if (newCapacity < needed)
        newCapacity = needed;
    if (newCapacity > MAX_OPERATORS)
        newCapacity = MAX_OPERATORS;
    OperatorRow *grown = realloc(opRows, newCapacity * sizeof(*grown));
    if (!grown)
        return false;
    opRows = grown;
    opCapacity = newCapacity;
    return true;
}

/* In-memory upsert keeping opRows sorted by code. No saving. */
static bool UpsertRow(const char *code, const char *name, CraftType type)
{
    int i = FindRow(code);
    if (i < 0) {
        if (!EnsureOpCapacity(opCount + 1))
            return false;
        size_t pos = 0;
        while (pos < opCount && strcmp(opRows[pos].code, code) < 0)
            pos++;
        memmove(&opRows[pos + 1], &opRows[pos], (opCount - pos) * sizeof(*opRows));
        opCount++;
        i = (int)pos;
        strcpy(opRows[i].code, code);
    }
    strcpy(opRows[i].name, name);
    opRows[i].type = type;
    return true;
}

static void RemoveRowAt(size_t i)
{
    memmove(&opRows[i], &opRows[i + 1], (opCount - i - 1) * sizeof(*opRows));
    opCount--;
}

static bool SaveOperatorsToCsv(void)
{
    FILE *f = fopen(OPERATORS_CSV_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Could not open %s for writing", OPERATORS_CSV_PATH);
        return false;
    }

    fprintf(f, "#FORMAT=1\n");
    fprintf(f, "# Flight Radar operator changes: ICAO,NAME,TYPE\n");
    fprintf(f, "# Only edits to the built-in operator list and your own added operators are stored\n");
    fprintf(f, "# here. A row whose ICAO matches a built-in operator overrides its name/type;\n");
    fprintf(f, "# delete the row (or use Restore Default) to go back to the built-in value.\n");
    fprintf(f, "# TYPE is one of PERSONAL, PRIVATE, BUSINESS, COMMERCIAL, CARGO, MILITARY,\n");
    fprintf(f, "# POLICE, EMERGENCY, INTERESTING, IMPORTANT. NAME may not contain commas or quotes.\n");

    for (size_t i = 0; i < opCount; i++)
        fprintf(f, "%s,%s,%s\n", opRows[i].code, opRows[i].name, CraftType_CsvName(opRows[i].type));

    fclose(f);
    return true;
}

static void LoadOperatorsFromCsv(void)
{
    opCount = 0;

    FILE *f = fopen(OPERATORS_CSV_PATH, "r");
    if (!f)
        return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *trimmed = TrimInPlace(line);
        if (*trimmed == '\0' || *trimmed == '#')
            continue;

        char *first = strchr(trimmed, ',');
        char *last = strrchr(trimmed, ',');
        if (!first || first == last)
            continue;
        *first = '\0';
        *last = '\0';

        char code[MAX_OPERATOR_CODE + 1];
        char name[MAX_OPERATOR_NAME + 1];
        CraftType type;
        if (!Operators_NormalizeCode(TrimInPlace(trimmed), code) ||
            !Operators_NormalizeName(first + 1, name) ||
            !CraftType_Parse(TrimInPlace(last + 1), false, &type))
            continue;

        if (!UpsertRow(code, name, type))
            break;
    }
    fclose(f);
}

/* One-time migration of the old commercial-codes list into operator rows.
 * Built-in codes are skipped (their built-in classification stands). */
static void ImportLegacyCommercialCodes(void)
{
    FILE *f = fopen(COMMERCIAL_CODES_CSV_PATH, "r");
    if (!f)
        return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *trimmed = TrimInPlace(line);
        if (*trimmed == '\0' || *trimmed == '#')
            continue;
        char *comma = strchr(trimmed, ',');
        if (comma)
            *comma = '\0';
        char code[MAX_OPERATOR_CODE + 1];
        if (!Operators_NormalizeCode(TrimInPlace(trimmed), code) || BuiltinFind(code) >= 0 ||
            FindRow(code) >= 0)
            continue;
        if (!UpsertRow(code, code, CRAFT_COMMERCIAL))
            break;
    }
    fclose(f);
}

/* ---- load everything (Init and Reload) ---- */

static void LoadAllLocked(bool isInit)
{
    /* Registry */
    if (!FileExists(CUSTOM_RULES_CSV_PATH)) {
        rulesCount = 0;
        SeedMissingRegistryDefaults();
        SaveRulesToCsv();
        ESP_LOGI(TAG, "No registry file; seeded %u default registry entries", (unsigned)rulesCount);
    } else if (LoadRulesFromCsv()) {
        /* Legacy/older-format file. On the device's first boot with this
         * firmware, backfill any seed defaults still missing (this is the
         * same mechanism that has always (re-)added PFT144; it now also
         * covers the new VIP/special-mission prefixes) without touching
         * anything the user already has. */
        if (isInit)
            SeedMissingRegistryDefaults();
        SaveRulesToCsv();
        ESP_LOGI(TAG, "Migrated registry file to format %d", CUSTOM_RULES_FORMAT_VERSION);
    }

    /* Operators */
    if (FileExists(OPERATORS_CSV_PATH)) {
        LoadOperatorsFromCsv();
    } else {
        opCount = 0;
        if (FileExists(COMMERCIAL_CODES_CSV_PATH)) {
            ImportLegacyCommercialCodes();
            ESP_LOGI(TAG, "Migrated %u operator code(s) from commercial_codes.csv", (unsigned)opCount);
        } else {
            for (size_t i = 0; i < sizeof(seedCommercialExtras) / sizeof(seedCommercialExtras[0]); i++)
                UpsertRow(seedCommercialExtras[i], seedCommercialExtras[i], CRAFT_COMMERCIAL);
        }
        SaveOperatorsToCsv();
    }
}

bool CustomRules_Init(void)
{
    if (!rulesLock)
        rulesLock = xSemaphoreCreateMutex();
    if (!rulesLock)
        return false;

    if (!MountSpiffsIfNeeded())
        return false;

    xSemaphoreTake(rulesLock, portMAX_DELAY);
    LoadAllLocked(true);
    xSemaphoreGive(rulesLock);

    ESP_LOGI(TAG, "Loaded %u registry rule(s), %u operator override/custom row(s), %u built-in operators",
             (unsigned)rulesCount, (unsigned)opCount, (unsigned)BUILTIN_COUNT);
    return true;
}

bool CustomRules_Reload(void)
{
    if (!rulesLock)
        return false;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    LoadAllLocked(false);
    xSemaphoreGive(rulesLock);
    return true;
}

/* ---- registry API ---- */

size_t CustomRules_Count(void)
{
    if (!rulesLock)
        return 0;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    size_t count = rulesCount;
    xSemaphoreGive(rulesLock);
    return count;
}

bool CustomRules_Get(size_t index, CustomRule *out)
{
    if (!rulesLock || !out)
        return false;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    bool found = index < rulesCount;
    if (found)
        *out = rules[index];
    xSemaphoreGive(rulesLock);
    return found;
}

bool CustomRules_Find(const char *prefix, CustomRule *out)
{
    char normalized[MAX_RULE_PREFIX + 1];
    if (!rulesLock || !out || !CustomRules_NormalizePrefix(prefix, normalized))
        return false;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    int i = FindRule(normalized);
    if (i >= 0)
        *out = rules[i];
    xSemaphoreGive(rulesLock);
    return i >= 0;
}

bool CustomRules_Add(const char *prefix, CraftType type, AircraftType aircraftType, const char *notes)
{
    char normalized[MAX_RULE_PREFIX + 1];
    char normalizedNotes[MAX_RULE_NOTES + 1];
    if (!rulesLock || !CustomRules_NormalizePrefix(prefix, normalized) || !CraftType_IsValid((int)type) ||
        !AircraftType_IsValid((int)aircraftType) || !CustomRules_NormalizeNotes(notes, normalizedNotes))
        return false;

    xSemaphoreTake(rulesLock, portMAX_DELAY);

    int i = FindRule(normalized);
    bool isNew = (i < 0);
    CustomRule previous = {0};
    if (!isNew)
        previous = rules[i];

    if (!UpsertRule(normalized, type, aircraftType, normalizedNotes)) {
        xSemaphoreGive(rulesLock);
        return false;
    }

    bool saved = SaveRulesToCsv();
    if (!saved) {
        // Roll back the in-memory change so it doesn't drift from what's
        // actually on disk.
        if (isNew)
            rulesCount--;
        else
            rules[i] = previous;
    }

    xSemaphoreGive(rulesLock);
    return saved;
}

bool CustomRules_Delete(const char *prefix)
{
    char normalized[MAX_RULE_PREFIX + 1];
    if (!rulesLock || !CustomRules_NormalizePrefix(prefix, normalized))
        return false;

    xSemaphoreTake(rulesLock, portMAX_DELAY);

    int found = FindRule(normalized);
    if (found < 0) {
        xSemaphoreGive(rulesLock);
        return false;
    }
    size_t i = (size_t)found;

    CustomRule removed = rules[i];
    for (size_t j = i + 1; j < rulesCount; j++)
        rules[j - 1] = rules[j];
    rulesCount--;

    bool saved = SaveRulesToCsv();
    if (!saved) {
        for (size_t j = rulesCount; j > i; j--)
            rules[j] = rules[j - 1];
        rules[i] = removed;
        rulesCount++;
    }

    xSemaphoreGive(rulesLock);
    return saved;
}

/* ---- operator API ---- */

size_t Operators_BuiltinCount(void)
{
    return BUILTIN_COUNT;
}

/* Fills an OperatorInfo for a built-in, applying any override row. */
static void FillBuiltin(size_t b, OperatorInfo *out)
{
    memset(out, 0, sizeof(*out));
    strcpy(out->code, builtinOperators[b].code);
    strncpy(out->defaultName, builtinOperators[b].name, MAX_OPERATOR_NAME);
    out->defaultType = builtinOperators[b].type;
    out->builtin = true;
    int r = FindRow(out->code);
    if (r >= 0) {
        strcpy(out->name, opRows[r].name);
        out->type = opRows[r].type;
        out->modified = true;
    } else {
        strcpy(out->name, out->defaultName);
        out->type = out->defaultType;
    }
}

static void FillCustom(size_t r, OperatorInfo *out)
{
    memset(out, 0, sizeof(*out));
    strcpy(out->code, opRows[r].code);
    strcpy(out->name, opRows[r].name);
    out->type = opRows[r].type;
}

static size_t CustomCountLocked(void)
{
    size_t n = 0;
    for (size_t i = 0; i < opCount; i++)
        if (BuiltinFind(opRows[i].code) < 0)
            n++;
    return n;
}

size_t Operators_Count(void)
{
    if (!rulesLock)
        return BUILTIN_COUNT;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    size_t n = BUILTIN_COUNT + CustomCountLocked();
    xSemaphoreGive(rulesLock);
    return n;
}

bool Operators_Get(size_t index, OperatorInfo *out)
{
    if (!rulesLock || !out)
        return false;
    bool found = false;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    if (index < BUILTIN_COUNT) {
        FillBuiltin(index, out);
        found = true;
    } else {
        size_t want = index - BUILTIN_COUNT;
        for (size_t r = 0; r < opCount; r++) {
            if (BuiltinFind(opRows[r].code) >= 0)
                continue;
            if (want-- == 0) {
                FillCustom(r, out);
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(rulesLock);
    return found;
}

bool Operators_Find(const char *code, OperatorInfo *out)
{
    char normalized[MAX_OPERATOR_CODE + 1];
    if (!rulesLock || !out || !Operators_NormalizeCode(code, normalized))
        return false;
    bool found = false;
    xSemaphoreTake(rulesLock, portMAX_DELAY);
    int b = BuiltinFind(normalized);
    if (b >= 0) {
        FillBuiltin((size_t)b, out);
        found = true;
    } else {
        int r = FindRow(normalized);
        if (r >= 0) {
            FillCustom((size_t)r, out);
            found = true;
        }
    }
    xSemaphoreGive(rulesLock);
    return found;
}

bool Operators_Set(const char *code, const char *name, CraftType type)
{
    char c[MAX_OPERATOR_CODE + 1];
    char n[MAX_OPERATOR_NAME + 1];
    if (!rulesLock || !CraftType_IsValid((int)type) || !Operators_NormalizeCode(code, c) ||
        !Operators_NormalizeName(name, n))
        return false;

    xSemaphoreTake(rulesLock, portMAX_DELAY);

    int b = BuiltinFind(c);
    int r = FindRow(c);
    bool hadRow = r >= 0;
    OperatorRow previous = {0};
    if (hadRow)
        previous = opRows[r];

    bool ok;
    bool identicalToDefault = b >= 0 && !strcmp(n, builtinOperators[b].name) &&
                              type == builtinOperators[b].type;
    if (identicalToDefault) {
        /* Editing a built-in back to its defaults just drops the override. */
        if (hadRow)
            RemoveRowAt((size_t)r);
        ok = true;
    } else {
        ok = UpsertRow(c, n, type);
    }

    bool saved = false;
    if (ok) {
        saved = SaveOperatorsToCsv();
        if (!saved) {
            /* Roll back so memory doesn't drift from disk. */
            int now = FindRow(c);
            if (hadRow)
                UpsertRow(c, previous.name, previous.type);
            else if (now >= 0)
                RemoveRowAt((size_t)now);
        }
    }

    xSemaphoreGive(rulesLock);
    return saved;
}

bool Operators_RestoreDefault(const char *code)
{
    char c[MAX_OPERATOR_CODE + 1];
    if (!rulesLock || !Operators_NormalizeCode(code, c) || BuiltinFind(c) < 0)
        return false;

    xSemaphoreTake(rulesLock, portMAX_DELAY);
    int r = FindRow(c);
    if (r < 0) {
        xSemaphoreGive(rulesLock);
        return true; /* already at its default */
    }
    OperatorRow previous = opRows[r];
    RemoveRowAt((size_t)r);
    bool saved = SaveOperatorsToCsv();
    if (!saved)
        UpsertRow(previous.code, previous.name, previous.type);
    xSemaphoreGive(rulesLock);
    return saved;
}

bool Operators_DeleteCustom(const char *code)
{
    char c[MAX_OPERATOR_CODE + 1];
    if (!rulesLock || !Operators_NormalizeCode(code, c) || BuiltinFind(c) >= 0)
        return false;

    xSemaphoreTake(rulesLock, portMAX_DELAY);
    int r = FindRow(c);
    if (r < 0) {
        xSemaphoreGive(rulesLock);
        return false;
    }
    OperatorRow previous = opRows[r];
    RemoveRowAt((size_t)r);
    bool saved = SaveOperatorsToCsv();
    if (!saved)
        UpsertRow(previous.code, previous.name, previous.type);
    xSemaphoreGive(rulesLock);
    return saved;
}

/* ---- lookup ---- */

static bool StartsWith(const char *value, const char *prefix)
{
    for (; *prefix; value++, prefix++) {
        if (UpperChar(*value) != *prefix)
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
        c = UpperChar(c);
        if (*pattern == '?' && !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
        if (*pattern != '?' && c != *pattern)
            return false;
    }
    return true;
}

const char *CraftSource_Name(CraftSource source)
{
    switch (source) {
    case CRAFT_SRC_REGISTRY: return "Registry";
    case CRAFT_SRC_PREFIX_DEFAULT: return "Built-in prefix";
    case CRAFT_SRC_OPERATOR: return "Operator";
    default: return "Default";
    }
}

CraftResolution ResolveAircraft(const char *callsign, const char *hex)
{
    (void)hex; /* Reserved for future ICAO address rules. */
    CraftResolution result = {.type = CRAFT_PERSONAL, .aircraftType = AIRCRAFT_FIXED_WING,
                              .source = CRAFT_SRC_FALLBACK, .operatorCode = "", .registryPrefix = ""};
    if (!callsign)
        callsign = "";
    while (*callsign == ' ')
        callsign++;

    /* 1. Registry: longest matching rule wins. */
    size_t longest = 0;
    if (rulesLock) {
        xSemaphoreTake(rulesLock, portMAX_DELAY);
        for (size_t i = 0; i < rulesCount; i++) {
            size_t length = strlen(rules[i].prefix);
            if (length > longest && MatchesRulePattern(callsign, rules[i].prefix)) {
                result.type = rules[i].type;
                result.aircraftType = rules[i].aircraftType;
                result.source = CRAFT_SRC_REGISTRY;
                strcpy(result.registryPrefix, rules[i].prefix);
                longest = length;
            }
        }
        xSemaphoreGive(rulesLock);
    }
    if (longest)
        return result;

    /* 2. Long-standing built-in military/police/EMS prefixes. */
    static const struct { const char *prefix; CraftType type; } defaults[] = {
        {"V1LLAN", CRAFT_MILITARY}, {"S1NFRD", CRAFT_EMERGENCY},
        {"RESCUE", CRAFT_MILITARY}, {"NAVY", CRAFT_MILITARY},
        {"ADF", CRAFT_MILITARY}, {"ARMY", CRAFT_MILITARY},
        {"RCH", CRAFT_MILITARY},
        {"CHP", CRAFT_POLICE}, {"PD", CRAFT_POLICE},
        {"POLICE", CRAFT_POLICE},
        {"MED", CRAFT_EMERGENCY}, {"LIFE", CRAFT_EMERGENCY},
        {"EMS", CRAFT_EMERGENCY}, {"AIRAMB", CRAFT_EMERGENCY}
    };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        if (StartsWith(callsign, defaults[i].prefix)) {
            result.type = defaults[i].type;
            result.source = CRAFT_SRC_PREFIX_DEFAULT;
            return result;
        }
    }

    /* 3. Operator: longest matching ICAO/operator code (user rows override
     *    the built-in of the same code). */
    if (rulesLock) {
        xSemaphoreTake(rulesLock, portMAX_DELAY);
        longest = 0;
        for (size_t i = 0; i < opCount; i++) {
            size_t length = strlen(opRows[i].code);
            if (length > longest && StartsWith(callsign, opRows[i].code)) {
                result.type = opRows[i].type;
                strcpy(result.operatorCode, opRows[i].code);
                longest = length;
            }
        }
        for (size_t i = 0; i < BUILTIN_COUNT; i++) {
            size_t length = strlen(builtinOperators[i].code);
            if (length > longest && StartsWith(callsign, builtinOperators[i].code) &&
                FindRow(builtinOperators[i].code) < 0) {
                result.type = builtinOperators[i].type;
                strcpy(result.operatorCode, builtinOperators[i].code);
                longest = length;
            }
        }
        xSemaphoreGive(rulesLock);
        if (longest) {
            result.source = CRAFT_SRC_OPERATOR;
            return result;
        }
    }

    /* 4. Fallback: small/unknown aircraft are Personal. */
    return result;
}

CraftType evaluateAircraftType(const char *callsign, const char *hex)
{
    return ResolveAircraft(callsign, hex).type;
}

CraftAppearance ResolveAircraftAppearance(const char *callsign, const char *hex)
{
    CraftResolution resolved = ResolveAircraft(callsign, hex);
    return CraftType_Appearance(resolved.type, resolved.aircraftType);
}
