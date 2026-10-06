#pragma once
/* "Automatically select closest aircraft", as a pure, host-testable policy (no LVGL, no globals
 * beyond the state struct the caller owns). Nothing here depends on the order of gAircraft[]:
 * ties are resolved by ICAO24, so the same traffic always gives the same answer.
 *
 * Ranking: classification first (IMPORTANT > INTERESTING > everything else), then which third of
 * the radar the aircraft is in (inner <= R/3, middle <= 2R/3, outer), then distance from the
 * radar center. Aircraft that rank the same (same class and ring; in the inner ring also within
 * max(0.25 km, 2% of range) of the best) take turns: the selection moves to the next ICAO24 after
 * AUTOSEL_DWELL_MS. A manual Prev/Next holds the chosen aircraft for AUTOSEL_MANUAL_HOLD_MS. */
#include <stdbool.h>
#include <stdint.h>

#define AUTOSEL_DWELL_MS 10000u
#define AUTOSEL_MANUAL_HOLD_MS 30000u

#define AUTOSEL_CLASS_OTHER 0
#define AUTOSEL_CLASS_INTERESTING 1
#define AUTOSEL_CLASS_IMPORTANT 2

typedef struct {
    const char *icao24;
    int cls;       /* AUTOSEL_CLASS_* */
    float distKm;  /* from the radar center (dead-reckoned position) */
} AutoSelectCand;

typedef struct {
    char icao24[12];     /* current selection ("" = none) */
    uint32_t sinceMs;    /* when it was selected */
    char manual[12];     /* aircraft chosen with Prev/Next, held until manualUntilMs */
    uint32_t manualUntilMs;
    bool haveManual;
} AutoSelectState;

void AutoSelect_Init(AutoSelectState *s);
/* A manual Prev/Next chose `icao24` at nowMs: hold it. */
void AutoSelect_NoteManual(AutoSelectState *s, const char *icao24, uint32_t nowMs);
/* Index into c[] of the aircraft to select, or -1 when n == 0. Updates the state. */
int AutoSelect_Choose(AutoSelectState *s, const AutoSelectCand *c, int n, float rangeKm, uint32_t nowMs);
/* Ring of a distance: 0 inner, 1 middle, 2 outer. */
int AutoSelect_Ring(float distKm, float rangeKm);
