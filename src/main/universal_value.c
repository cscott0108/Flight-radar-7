#include "universal_value.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#else
#include <stdlib.h>
#endif

/* The table (MAX_UNIVERSAL_VALUES * sizeof(UniversalValue), ~104 KB) is
 * allocated from PSRAM at Init() time rather than declared `static`: a
 * plain static array of this size lands in internal DRAM, which this
 * project has already hit the limits of once before (see
 * PROJECT_STATE.md's airports.c stack/`.bss` fix) - and did again with
 * this table during the first real `idf.py build` of this feature
 * (`.dram0.bss` overflow). PSRAM is unused RAM this device already has
 * 8 MB of; internal DRAM is the genuinely scarce resource. A NULL
 * `s_table` (PSRAM absent/exhausted) degrades to "no Universal Values"
 * rather than crashing - `UniversalValue_Sync()` guards on it and every
 * iteration is bounded by `s_usedSlots`, which stays 0 in that case. */
static UniversalValue *s_table = NULL;
static size_t s_usedSlots = 0; /* high-water mark of everUsed slots, for iteration */
static uint16_t s_lastAssignedCode = UV_CODE_UNASSIGNED;
static UniversalValueStats s_stats;
static bool s_initDone = false;

/* ---- FNV-1a 32-bit ----
 *
 * Deterministic, tiny, fast on ESP32; NOT a security hash (PROMPT.md
 * section 9-13). Kept local rather than a shared fnv1a.h so this file has
 * exactly one thing to get right and host-tests trivially. tf_history.c
 * uses the identical constants for its own hashing (index slot placement),
 * duplicated rather than shared across an ESP-IDF/host boundary header for
 * the same reason. */
static uint32_t Fnv1a32(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        h ^= (uint32_t)(*p);
        h *= 0x01000193u;
    }
    return h;
}

uint32_t UniversalValue_Fingerprint(const char *canonicalIdentity)
{
    return Fnv1a32(canonicalIdentity);
}

void UniversalValue_NormalizeIdentity(const char *operatorCode, char out[UV_IDENTITY_MAX])
{
    if (!out)
        return;
    char code[MAX_OPERATOR_CODE + 1] = {0};
    size_t n = 0;
    for (const char *p = operatorCode ? operatorCode : ""; *p && n < MAX_OPERATOR_CODE; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        code[n++] = (char)toupper(c);
    }
    code[n] = '\0';
    snprintf(out, UV_IDENTITY_MAX, "OPERATOR:%s", code);
}

/* Finds an existing slot (active or inactive) whose canonical identity
 * matches, or NULL. Fingerprint is checked first (cheap int compare); the
 * string compare is what actually decides identity - a fingerprint match
 * with a differing string is a collision, resolved by the caller. */
static UniversalValue *FindByIdentityExact(const char *identity, uint32_t fp)
{
    for (size_t i = 0; i < s_usedSlots; i++) {
        UniversalValue *e = &s_table[i];
        if (e->everUsed && e->fingerprint == fp && strcmp(e->canonicalIdentity, identity) == 0)
            return e;
    }
    return NULL;
}

/* True if some OTHER slot already holds this fingerprint with a different
 * identity - a genuine collision (PROMPT.md section 9F, 13). */
static UniversalValue *FindFingerprintCollision(uint32_t fp, const char *identity)
{
    for (size_t i = 0; i < s_usedSlots; i++) {
        UniversalValue *e = &s_table[i];
        if (e->everUsed && e->fingerprint == fp && strcmp(e->canonicalIdentity, identity) != 0)
            return e;
    }
    return NULL;
}

static bool CodeInUseByActive(uint16_t code)
{
    for (size_t i = 0; i < s_usedSlots; i++) {
        if (s_table[i].active && s_table[i].code == code)
            return true;
    }
    return false;
}

/* Reusable numeric-code allocator (PROMPT.md section 11/30). Active entries
 * are always few (<= MAX_OPERATORS <= 1000) against a 65,533-value code
 * space, so a linear scan starting just after the last handed-out code
 * converges immediately in every realistic case; this is intentionally not
 * a bitmap (would cost 64 KB of RAM for no measured benefit - section 42). */
static uint16_t AllocateCode(void)
{
    uint16_t start = s_lastAssignedCode;
    for (uint32_t tries = 0; tries < (UV_CODE_MAX - UV_CODE_MIN + 1); tries++) {
        uint16_t candidate = (uint16_t)(UV_CODE_MIN + ((start - UV_CODE_MIN + 1 + tries) % (UV_CODE_MAX - UV_CODE_MIN + 1)));
        if (!CodeInUseByActive(candidate)) {
            s_lastAssignedCode = candidate;
            return candidate;
        }
    }
    return UV_CODE_UNASSIGNED; /* table pathologically full; caller must handle */
}

static UniversalValue *FindFreeSlot(void)
{
    if (s_usedSlots < MAX_UNIVERSAL_VALUES)
        return &s_table[s_usedSlots++];
    /* No never-used slot left; recycle the longest-inactive entry rather
     * than refusing new operators outright. This does discard that entry's
     * ability to reactivate into the SAME slot object, but its persistent
     * TF history file is keyed by fingerprint, not by slot, so nothing on
     * disk is lost - only the in-RAM bookkeeping for an operator that has
     * been both removed and never reused across MAX_OPERATORS other churn
     * events, an extreme case PROMPT.md section 42 says not to design
     * around further without measurement. */
    for (size_t i = 0; i < MAX_UNIVERSAL_VALUES; i++) {
        if (!s_table[i].active)
            return &s_table[i];
    }
    return NULL;
}

bool UniversalValue_Init(void)
{
    if (!s_table) {
#ifdef ESP_PLATFORM
        s_table = (UniversalValue *)heap_caps_calloc(MAX_UNIVERSAL_VALUES, sizeof(UniversalValue),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        s_table = (UniversalValue *)calloc(MAX_UNIVERSAL_VALUES, sizeof(UniversalValue));
#endif
        if (!s_table) {
            /* PSRAM absent or exhausted: degrade to "no Universal Values"
             * rather than crash. Every registry-classified and unassigned
             * aircraft still gets persistent TF history via their own
             * special buckets either way - only the operator-based
             * Universal Value layer is unavailable. */
            s_initDone = false;
            return false;
        }
    } else {
        memset(s_table, 0, sizeof(UniversalValue) * MAX_UNIVERSAL_VALUES);
    }

    memset(&s_stats, 0, sizeof(s_stats));
    s_usedSlots = 0;
    s_lastAssignedCode = UV_CODE_UNASSIGNED;
    s_initDone = true;
    UniversalValue_Sync();
    return true;
}

void UniversalValue_Sync(void)
{
    if (!s_initDone || !s_table)
        return;

    /* Mark every currently-active slot "unseen this pass"; anything still
     * unseen after the loop below no longer has a matching operator row and
     * gets deactivated (code freed, identity/fingerprint/metadata kept).
     * Allocated (PSRAM, transient, freed below) rather than a ~1000-byte
     * stack array - the same reasoning as s_table above applies to a
     * caller's stack, which on this project has previously been as tight
     * as ~3.5 KB (see PROJECT_STATE.md). */
#ifdef ESP_PLATFORM
    bool *seen = (bool *)heap_caps_calloc(MAX_UNIVERSAL_VALUES, sizeof(bool), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    bool *seen = (bool *)calloc(MAX_UNIVERSAL_VALUES, sizeof(bool));
#endif
    if (!seen)
        return; /* PSRAM momentarily exhausted; try again next sync rather than risking the caller's stack */

    size_t opCount = Operators_Count();
    for (size_t i = 0; i < opCount; i++) {
        OperatorInfo op;
        if (!Operators_Get(i, &op))
            continue;
        if (op.code[0] == '\0')
            continue;

        char identity[UV_IDENTITY_MAX];
        UniversalValue_NormalizeIdentity(op.code, identity);
        uint32_t fp = Fnv1a32(identity);

        UniversalValue *existing = FindByIdentityExact(identity, fp);
        if (existing) {
            if (!existing->active) {
                existing->active = true;
                existing->code = AllocateCode();
                s_stats.reactivated++;
                if (existing->code != UV_CODE_UNASSIGNED)
                    s_stats.codeReuseCount++;
            }
            /* Metadata (name/craft category) is allowed to change; identity
             * and fingerprint never do. */
            snprintf(existing->displayName, sizeof(existing->displayName), "%s", op.name);
            existing->craftCategory = op.type;
            for (size_t k = 0; k < MAX_UNIVERSAL_VALUES; k++) {
                if (&s_table[k] == existing) { seen[k] = true; break; }
            }
            continue;
        }

        /* Resolve a genuine fingerprint collision deterministically before
         * creating a new entry (PROMPT.md section 9F/13): perturb by +1 and
         * recheck against everything already in the table. Astronomically
         * unlikely at this scale (<=1000 entries in a 4-billion space) but
         * detected rather than silently overwritten either way. */
        UniversalValue *collidesWith;
        uint32_t tries = 0;
        while ((collidesWith = FindFingerprintCollision(fp, identity)) != NULL && tries < 8) {
            s_stats.fingerprintCollisions++;
            fp = fp + 1u;
            tries++;
        }
        (void)collidesWith;

        UniversalValue *slot = FindFreeSlot();
        if (!slot)
            continue; /* table exhausted; operator simply won't get a Universal Value this pass */

        memset(slot, 0, sizeof(*slot));
        slot->everUsed = true;
        slot->active = true;
        snprintf(slot->canonicalIdentity, sizeof(slot->canonicalIdentity), "%s", identity);
        slot->fingerprint = fp;
        snprintf(slot->operatorCode, sizeof(slot->operatorCode), "%s", op.code);
        snprintf(slot->displayName, sizeof(slot->displayName), "%s", op.name);
        slot->craftCategory = op.type;
        slot->aircraftType = AIRCRAFT_FIXED_WING;
        slot->code = AllocateCode();
        s_stats.created++;
        for (size_t k = 0; k < MAX_UNIVERSAL_VALUES; k++) {
            if (&s_table[k] == slot) { seen[k] = true; break; }
        }
    }

    for (size_t i = 0; i < s_usedSlots; i++) {
        if (s_table[i].active && !seen[i]) {
            s_table[i].active = false;
            s_table[i].code = UV_CODE_UNASSIGNED;
        }
    }

    free(seen);
}

size_t UniversalValue_Count(void) { return s_usedSlots; }

size_t UniversalValue_ActiveCount(void)
{
    size_t n = 0;
    for (size_t i = 0; i < s_usedSlots; i++)
        if (s_table[i].active)
            n++;
    return n;
}

bool UniversalValue_GetByIndex(size_t index, UniversalValue *out)
{
    if (index >= s_usedSlots || !out)
        return false;
    *out = s_table[index];
    return true;
}

bool UniversalValue_FindActiveByCode(uint16_t code, UniversalValue *out)
{
    s_stats.lookups++;
    for (size_t i = 0; i < s_usedSlots; i++) {
        if (s_table[i].active && s_table[i].code == code) {
            if (out) *out = s_table[i];
            return true;
        }
    }
    s_stats.lookupMisses++;
    return false;
}

bool UniversalValue_FindByOperatorCode(const char *operatorCode, UniversalValue *out)
{
    s_stats.lookups++;
    char identity[UV_IDENTITY_MAX];
    UniversalValue_NormalizeIdentity(operatorCode, identity);
    for (size_t i = 0; i < s_usedSlots; i++) {
        if (s_table[i].active && strcmp(s_table[i].canonicalIdentity, identity) == 0) {
            if (out) *out = s_table[i];
            return true;
        }
    }
    s_stats.lookupMisses++;
    return false;
}

bool UniversalValue_FindByFingerprint(uint32_t fingerprint, UniversalValue *out)
{
    for (size_t i = 0; i < s_usedSlots; i++) {
        if (s_table[i].everUsed && s_table[i].fingerprint == fingerprint) {
            if (out) *out = s_table[i];
            return true;
        }
    }
    return false;
}

void UniversalValue_GetStats(UniversalValueStats *out)
{
    if (out) *out = s_stats;
}
