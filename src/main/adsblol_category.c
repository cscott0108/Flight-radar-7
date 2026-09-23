#include "adsblol_category.h"

#include <string.h>
#include <strings.h>

bool AdsbLol_CategoryToAircraftType(const char *category, AircraftType *out)
{
    if (!category || !out)
        return false;

    if (strcasecmp(category, "A7") == 0)
    {
        *out = AIRCRAFT_HELICOPTER;
        return true;
    }

    if (strcasecmp(category, "B1") == 0 || strcasecmp(category, "B2") == 0 ||
        strcasecmp(category, "B4") == 0 || strcasecmp(category, "B6") == 0)
    {
        *out = AIRCRAFT_OTHER;
        return true;
    }

    /* C0-C7: ground vehicles / obstacles. No dedicated shape exists for
     * these in the project, so they're surfaced as Other rather than
     * inventing a mapping the provider doesn't document (see header). */
    if (category[0] == 'C' || category[0] == 'c')
    {
        size_t len = strlen(category);
        if (len == 2 && category[1] >= '0' && category[1] <= '7')
        {
            *out = AIRCRAFT_OTHER;
            return true;
        }
    }

    return false; /* A0/unknown/A1-A6/A8+/B0/B3/B5/B7/missing: no hint */
}
