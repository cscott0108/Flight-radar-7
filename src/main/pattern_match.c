#include "pattern_match.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

bool Pattern_HasWildcards(const char *query)
{
    return query && (strchr(query, '?') || strchr(query, '*'));
}

static size_t TrimmedLength(const char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ')
        n--;
    return n;
}

static bool ContainsNoCase(const char *hay, size_t hayLen, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0)
        return true;
    for (size_t start = 0; start + n <= hayLen; start++) {
        size_t i = 0;
        while (i < n && toupper((unsigned char)hay[start + i]) == toupper((unsigned char)needle[i]))
            i++;
        if (i == n)
            return true;
    }
    return false;
}

/* One query token against one value character. */
static bool TokenMatches(char token, char c, PatternMode mode)
{
    const unsigned char u = (unsigned char)c;
    if (token == '?')
        return true;
    if (mode == PATTERN_MODE_PATTERN) {
        if (token == 'L' || token == 'l')
            return isalpha(u) != 0;
        if (token == 'N' || token == 'n')
            return isdigit(u) != 0;
    }
    return toupper(u) == toupper((unsigned char)token);
}

/* Whole-value match with '*' backtracking to the last star (linear for
 * typical queries, O(n*m) worst case; both lengths are tiny here). */
static bool WholeMatch(const char *v, size_t vLen, const char *q, PatternMode mode)
{
    size_t vi = 0, qi = 0, qLen = strlen(q);
    size_t starQ = (size_t)-1, starV = 0;
    while (vi < vLen) {
        if (qi < qLen && q[qi] == '*') {
            starQ = qi++;
            starV = vi;
        } else if (qi < qLen && TokenMatches(q[qi], v[vi], mode)) {
            qi++;
            vi++;
        } else if (starQ != (size_t)-1) {
            qi = starQ + 1;
            vi = ++starV;
        } else {
            return false;
        }
    }
    while (qi < qLen && q[qi] == '*')
        qi++;
    return qi == qLen;
}

bool Pattern_Match(const char *value, const char *query, PatternMode mode)
{
    if (!query || !query[0])
        return true;
    if (!value)
        return false;
    const size_t vLen = TrimmedLength(value);
    if (mode == PATTERN_MODE_NORMAL && !Pattern_HasWildcards(query))
        return ContainsNoCase(value, vLen, query);
    return WholeMatch(value, vLen, query, mode);
}

SearchField SearchField_FromToken(const char *token)
{
    if (token && !strcmp(token, "c"))
        return SEARCH_FIELD_CALLSIGN;
    if (token && !strcmp(token, "r"))
        return SEARCH_FIELD_REGISTRATION;
    return SEARCH_FIELD_EITHER;
}

const char *SearchField_Token(SearchField field)
{
    switch (field) {
    case SEARCH_FIELD_CALLSIGN: return "c";
    case SEARCH_FIELD_REGISTRATION: return "r";
    default: return "e";
    }
}
