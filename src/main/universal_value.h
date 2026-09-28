#pragma once

/* Universal Value table: stable, reusable metadata (operator identity,
 * display name, craft category) referenced compactly by aircraft history
 * records - see PROJECT_STATE.md "Universal Values".
 *
 * THREE SEPARATE IDENTIFIERS, never combined:
 *
 *   - 16-bit numeric code   : compact, REUSABLE application/table reference.
 *   - Canonical identity    : stable logical identity, e.g. "OPERATOR:UAL".
 *   - 32-bit FNV-1a fingerprint : deterministic hash of the canonical
 *                                 identity; names the persistent TF file
 *                                 (tf_history.h) and never changes for a
 *                                 given canonical identity.
 *
 * The table is DERIVED from the existing operator configuration
 * (custom_rules.h: Operators_Count/Operators_Get - "operators.csv"), which
 * remains the single place operator ICAO/name/craft-type is configured; see
 * PROJECT_STATE.md/PROMPT.md section 7-8. This module adds nothing to that
 * file format. It instead layers three things on top of it that
 * operators.csv itself has no room for and does not need:
 *   - a compact, reusable 16-bit code aircraft-history records can store
 *     instead of a full operator row,
 *   - a stable canonical identity + FNV-1a fingerprint so a Universal
 *     Value's persistent TF history survives the operator row being
 *     edited, deleted and later re-added,
 *   - active/inactive bookkeeping so a numeric code can be reused without
 *     disturbing another Universal Value's persistent history.
 *
 * Registry Rules (custom_rules.csv) are NOT part of this table - see
 * PROMPT.md section 6: registry-classified aircraft use the separate
 * REGISTRY_DEFINED bucket in tf_history.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "craft_types.h"
#include "custom_rules.h" /* MAX_OPERATORS, MAX_OPERATOR_CODE, MAX_OPERATOR_NAME */

/* ---- A. numeric code (16-bit, reusable) ---- */
#define UV_CODE_UNASSIGNED       0x0000u /* not a real Universal Value */
#define UV_CODE_REGISTRY_DEFINED 0xFFFEu /* reserved token; never assigned to a real entry */
#define UV_CODE_RESERVED         0xFFFFu /* reserved */
#define UV_CODE_MIN              0x0001u
#define UV_CODE_MAX              0xFFFDu

/* ---- B. canonical identity ---- */
#define UV_IDENTITY_MAX 40 /* "OPERATOR:" (9) + MAX_OPERATOR_CODE (4) + slack */

/* One slot per operator row the table has ever seen this boot, active or
 * inactive (inactive slots are what let a removed-then-re-added operator
 * reclaim its persistent history without needing its old numeric code
 * back - PROMPT.md section 30). Sized to the existing Operators cap: in the
 * worst case every operator slot churns once and none are ever reused,
 * which this bounds the same way MAX_OPERATORS already bounds the source
 * list. Not a database - see PROMPT.md section 42. */
#define MAX_UNIVERSAL_VALUES MAX_OPERATORS

typedef struct {
    bool active;
    bool everUsed; /* false only for a slot that has never held an entry */
    uint16_t code; /* valid only when active */
    char canonicalIdentity[UV_IDENTITY_MAX];
    uint32_t fingerprint; /* FNV-1a 32-bit of canonicalIdentity; stable for life of the slot */
    char operatorCode[MAX_OPERATOR_CODE + 1];
    char displayName[MAX_OPERATOR_NAME + 1];
    CraftType craftCategory;
    AircraftType aircraftType; /* operators.csv carries no aircraft-type field; Fixed-Wing unless proven otherwise */
} UniversalValue;

/* Call once after CustomRules_Init(). Builds the initial table from the
 * current operator list. */
bool UniversalValue_Init(void);

/* Re-derives the table from the CURRENT operator list: reactivates a
 * matching inactive entry, creates a new one, refreshes metadata on an
 * existing active one, or deactivates an entry whose operator row is gone.
 * Cheap (O(operators x table size), both <= 1000), no persistent I/O. Safe
 * to call often; call at least whenever the operator list may have changed
 * and is about to be relied on (history_manager.c calls this from its own
 * periodic sync so web_rules.c needs no changes). */
void UniversalValue_Sync(void);

size_t UniversalValue_Count(void);       /* active + inactive slots ever used */
size_t UniversalValue_ActiveCount(void);

bool UniversalValue_GetByIndex(size_t index, UniversalValue *out);
bool UniversalValue_FindActiveByCode(uint16_t code, UniversalValue *out);
/* Active only - this is the normal classification-time lookup. */
bool UniversalValue_FindByOperatorCode(const char *operatorCode, UniversalValue *out);
/* Active or inactive - used by tf_history.c collision resolution. */
bool UniversalValue_FindByFingerprint(uint32_t fingerprint, UniversalValue *out);

/* Uppercases, trims, and prefixes "OPERATOR:". Deterministic: "ual", "UAL"
 * and " UAL " all normalize identically. */
void UniversalValue_NormalizeIdentity(const char *operatorCode, char out[UV_IDENTITY_MAX]);

/* FNV-1a 32-bit of a NUL-terminated string. Not a security hash - see
 * PROMPT.md section 9-13 and fnv1a.h. */
uint32_t UniversalValue_Fingerprint(const char *canonicalIdentity);

typedef struct {
    uint32_t lookups;
    uint32_t lookupMisses;
    uint32_t created;
    uint32_t reactivated;
    uint32_t codeReuseCount;       /* a code handed to an entry that isn't the one it was last assigned to */
    uint32_t fingerprintCollisions;/* two different canonical identities that hashed the same before resolution */
} UniversalValueStats;

void UniversalValue_GetStats(UniversalValueStats *out);
