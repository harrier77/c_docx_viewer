/*
 * text_utf8.c - conversione UTF-8 -> UTF-16.
 *
 * Funzione gia' presente in docxview_new.c; spostata qui perche' serve
 * sia al viewer sia al renderer GDI.
 */
#include "text_utf8.h"

#include <stdlib.h>
#include <string.h>

WCHAR *utf8_to_wide(const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    size_t len = strlen((const char *)p), i = 0, o = 0;
    WCHAR *out = (WCHAR *)malloc((len + 1) * sizeof(WCHAR));
    if (!out)
        return NULL;
    while (i < len) {
        unsigned c = p[i++], cp;
        if (c < 0x80)
            cp = c;
        else if ((c & 0xE0) == 0xC0 && i < len)
            cp = ((c & 0x1F) << 6) | (p[i++] & 0x3F);
        else if ((c & 0xF0) == 0xE0 && i + 1 < len) {
            cp = ((c & 0x0F) << 12) | ((p[i] & 0x3F) << 6) | (p[i + 1] & 0x3F);
            i += 2;
        } else if ((c & 0xF8) == 0xF0 && i + 2 < len) {
            cp = ((c & 0x07) << 18) | ((p[i] & 0x3F) << 12) |
                 ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
            i += 3;
        } else
            cp = '?';
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[o++] = (WCHAR)(0xD800 + (cp >> 10));
            out[o++] = (WCHAR)(0xDC00 + (cp & 0x3FF));
        } else {
            out[o++] = (WCHAR)cp;
        }
    }
    out[o] = 0;
    return out;
}
