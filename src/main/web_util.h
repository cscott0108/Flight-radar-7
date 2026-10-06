#pragma once

/* Small pure helpers for the C-generated web pages (no ESP-IDF dependencies,
 * so they are host-tested). web_seen.c uses them; the older pages still carry
 * their own private copies of the escape/decode helpers. */

#include <stdbool.h>
#include <stddef.h>

/* HTML-escapes src (& < > " ') into dst. Always NUL-terminates; truncates at
 * an entity boundary rather than overflowing or cutting an entity in half.
 * Returns the length written. */
size_t WebUtil_EscapeHtml(char *dst, size_t cap, const char *src);

/* application/x-www-form-urlencoded / query-string decoding, in place. */
void WebUtil_UrlDecodeInPlace(char *s);

/* Percent-encodes everything except unreserved characters (A-Z a-z 0-9 - _ . ~).
 * Always NUL-terminates; truncates on a whole-character boundary. Returns the
 * length written. */
size_t WebUtil_UrlEncode(char *dst, size_t cap, const char *src);

/* Web search text for the Lookup button (0.0.28): always names the subject as
 * an aircraft so a call sign that resembles a part number or product code
 * still finds aircraft. Both known: "aircraft ICAO24 a1b2c3 callsign N12345";
 * one known: "aircraft ICAO24 a1b2c3" / "aircraft callsign N12345"; neither:
 * "" (the button is disabled). Leading/trailing spaces of the inputs are
 * ignored. Returns the length written (always NUL-terminated). */
size_t WebUtil_BuildLookupQuery(char *dst, size_t cap, const char *icao24, const char *callsign);
