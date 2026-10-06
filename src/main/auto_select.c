#include "auto_select.h"

#include <math.h>
#include <string.h>

static void CopyIcao(char *dst, const char *src)
{
    strncpy(dst, src ? src : "", 11);
    dst[11] = '\0';
}

void AutoSelect_Init(AutoSelectState *s)
{
    memset(s, 0, sizeof(*s));
}

void AutoSelect_NoteManual(AutoSelectState *s, const char *icao24, uint32_t nowMs)
{
    CopyIcao(s->manual, icao24);
    s->haveManual = s->manual[0] != '\0';
    s->manualUntilMs = nowMs + AUTOSEL_MANUAL_HOLD_MS;
    CopyIcao(s->icao24, icao24);
    s->sinceMs = nowMs;
}

int AutoSelect_Ring(float distKm, float rangeKm)
{
    if (!(rangeKm > 0.0f))
        return 2;
    if (distKm <= rangeKm / 3.0f)
        return 0;
    if (distKm <= rangeKm * 2.0f / 3.0f)
        return 1;
    return 2;
}

/* true when a ranks strictly better than b (class desc, ring asc, distance asc, ICAO24 asc) */
static bool Better(const AutoSelectCand *a, const AutoSelectCand *b, float rangeKm)
{
    if (a->cls != b->cls)
        return a->cls > b->cls;
    int ra = AutoSelect_Ring(a->distKm, rangeKm), rb = AutoSelect_Ring(b->distKm, rangeKm);
    if (ra != rb)
        return ra < rb;
    if (a->distKm != b->distKm)
        return a->distKm < b->distKm;
    return strcmp(a->icao24, b->icao24) < 0;
}

static int Find(const AutoSelectCand *c, int n, const char *icao)
{
    if (!icao || !icao[0])
        return -1;
    for (int i = 0; i < n; i++)
        if (strcmp(c[i].icao24, icao) == 0)
            return i;
    return -1;
}

int AutoSelect_Choose(AutoSelectState *s, const AutoSelectCand *c, int n, float rangeKm, uint32_t nowMs)
{
    if (n <= 0) {
        s->icao24[0] = '\0';
        s->haveManual = false;
        return -1;
    }

    /* Manual hold: keep the aircraft the user picked while it is still around. */
    if (s->haveManual) {
        int m = ((int32_t)(s->manualUntilMs - nowMs) > 0) ? Find(c, n, s->manual) : -1;
        if (m >= 0) {
            if (strcmp(s->icao24, c[m].icao24) != 0) {
                CopyIcao(s->icao24, c[m].icao24);
                s->sinceMs = nowMs;
            }
            return m;
        }
        s->haveManual = false;
    }

    int best = 0;
    for (int i = 1; i < n; i++)
        if (Better(&c[i], &c[best], rangeKm))
            best = i;

    /* Equivalent set: same class and ring as the best; in the inner ring also within eps of it. */
    float eps = fmaxf(0.25f, 0.02f * rangeKm);
    int bestRing = AutoSelect_Ring(c[best].distKm, rangeKm);
    int cur = Find(c, n, s->icao24);
    bool curEquivalent = false;
    int lowest = -1;     /* lowest equivalent ICAO24 */
    int nextGreater = -1;
    for (int i = 0; i < n; i++) {
        if (c[i].cls != c[best].cls || AutoSelect_Ring(c[i].distKm, rangeKm) != bestRing)
            continue;
        if (bestRing == 0 && c[i].distKm > c[best].distKm + eps)
            continue;
        if (i == cur)
            curEquivalent = true;
        if (lowest < 0 || strcmp(c[i].icao24, c[lowest].icao24) < 0)
            lowest = i;
        if (cur >= 0 && strcmp(c[i].icao24, c[cur].icao24) > 0 &&
            (nextGreater < 0 || strcmp(c[i].icao24, c[nextGreater].icao24) < 0))
            nextGreater = i;
    }
    int next = (nextGreater >= 0) ? nextGreater : lowest; /* next equivalent ICAO24 after the current one, wrapping */

    int pick;
    if (curEquivalent) {
        pick = ((uint32_t)(nowMs - s->sinceMs) >= AUTOSEL_DWELL_MS) ? next : cur;
    } else {
        pick = best; /* a better-ranked aircraft appeared, or the current one left */
    }
    if (strcmp(s->icao24, c[pick].icao24) != 0) {
        CopyIcao(s->icao24, c[pick].icao24);
        s->sinceMs = nowMs;
    }
    return pick;
}
