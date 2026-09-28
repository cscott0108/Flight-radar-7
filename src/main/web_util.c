#include "web_util.h"

#include <stdbool.h>
#include <string.h>

size_t WebUtil_EscapeHtml(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0)
        return 0;
    size_t used = 0;
    for (const char *p = src ? src : ""; *p; p++) {
        const char *escape = NULL;
        switch (*p) {
        case '&': escape = "&amp;"; break;
        case '<': escape = "&lt;"; break;
        case '>': escape = "&gt;"; break;
        case '"': escape = "&quot;"; break;
        case '\'': escape = "&#39;"; break;
        default: break;
        }
        size_t length = escape ? strlen(escape) : 1;
        if (used + length + 1 > cap)
            break;
        if (escape)
            memcpy(dst + used, escape, length);
        else
            dst[used] = *p;
        used += length;
    }
    dst[used] = '\0';
    return used;
}

static int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void WebUtil_UrlDecodeInPlace(char *s)
{
    if (!s)
        return;
    char *out = s;
    for (const char *in = s; *in; in++) {
        if (*in == '+') {
            *out++ = ' ';
        } else if (in[0] == '%' && HexValue(in[1]) >= 0 && HexValue(in[2]) >= 0) {
            *out++ = (char)(HexValue(in[1]) * 16 + HexValue(in[2]));
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

size_t WebUtil_UrlEncode(char *dst, size_t cap, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!dst || cap == 0)
        return 0;
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                     *p == '-' || *p == '_' || *p == '.' || *p == '~';
        size_t length = plain ? 1 : 3;
        if (used + length + 1 > cap)
            break;
        if (plain) {
            dst[used++] = (char)*p;
        } else {
            dst[used++] = '%';
            dst[used++] = hex[*p >> 4];
            dst[used++] = hex[*p & 0xF];
        }
    }
    dst[used] = '\0';
    return used;
}
