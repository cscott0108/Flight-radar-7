#pragma once

/* One search-matching convention for every Flight-radar-7 search box
 * (/seen, /history). Case-insensitive; the caller trims the query.
 *
 * Normal mode (default):
 *   - text without '?' or '*'  -> literal substring ("contains"), exactly the
 *     behaviour the search boxes had before 0.0.27;
 *   - text with '?' or '*'     -> wildcard match against the WHOLE value:
 *       ?  = exactly one character
 *       *  = zero or more characters
 *     e.g. "ABC?" matches ABC1 / ABCD but not ABC or ABC12; "*ABC*" = contains.
 *   L and N are ordinary letters in normal mode ("LLL123" means LLL123).
 *
 * Pattern mode (only when the user ticks it): like a wildcard match, plus
 *       L  = exactly one letter A-Z
 *       N  = exactly one digit 0-9
 *   every other character is literal. e.g. "LLLNNNNL", "LLL*", "NNN*".
 *
 * /registered rules are classification, not search, and keep their own
 * documented rule syntax (custom_rules.c MatchesRulePattern): '?' = one letter
 * or digit and a rule always matches as a prefix (as if it ended in '*'). */

#include <stdbool.h>

typedef enum {
    PATTERN_MODE_NORMAL = 0,
    PATTERN_MODE_PATTERN = 1
} PatternMode;

/* Which identifier a search looks at (pages decide what "registration" means
 * for their data; see web_seen.c / web_history.c). */
typedef enum {
    SEARCH_FIELD_EITHER = 0,
    SEARCH_FIELD_CALLSIGN = 1,
    SEARCH_FIELD_REGISTRATION = 2
} SearchField;

/* True when the query contains '?' or '*'. */
bool Pattern_HasWildcards(const char *query);

/* Does `value` match `query` under `mode`? NULL/empty value never matches a
 * non-empty query; an empty query matches everything. Trailing spaces of the
 * value are ignored (padded call signs). Bounded: O(len(value) * len(query)). */
bool Pattern_Match(const char *value, const char *query, PatternMode mode);

/* Query-string helpers shared by the pages: "e"/"c"/"r" <-> SearchField. */
SearchField SearchField_FromToken(const char *token);
const char *SearchField_Token(SearchField field);
