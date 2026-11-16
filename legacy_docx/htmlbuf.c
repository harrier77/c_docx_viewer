#include "htmlbuf.h"
#include "xmlbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void hb_init(htmlbuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->err = 0;
}

void hb_free(htmlbuf *b)
{
    free(b->data);
    hb_init(b);
}

int hb_reserve(htmlbuf *b, size_t extra)
{
    size_t need, cap;
    char *p;

    if (b->err)
        return 0;
    need = b->len + extra + 1;
    if (need <= b->cap)
        return 1;

    cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            b->err = 1;
            return 0;
        }
        cap *= 2;
    }
    p = (char *)realloc(b->data, cap);
    if (!p) {
        b->err = 1;
        return 0;
    }
    b->data = p;
    b->cap = cap;
    return 1;
}

int hb_putc(htmlbuf *b, char c)
{
    if (!hb_reserve(b, 1))
        return 0;
    b->data[b->len++] = c;
    b->data[b->len] = 0;
    return 1;
}

int hb_putn(htmlbuf *b, const char *s, size_t n)
{
    if (!n)
        return 1;
    if (!hb_reserve(b, n))
        return 0;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
    return 1;
}

int hb_puts(htmlbuf *b, const char *s)
{
    return hb_putn(b, s, strlen(s));
}

int hb_printf(htmlbuf *b, const char *fmt, ...)
{
    char tmp[512];
    int n;
    va_list ap;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n < 0) {
        b->err = 1;
        return 0;
    }
    if ((size_t)n < sizeof(tmp))
        return hb_putn(b, tmp, (size_t)n);

    /* Rare: formatted output longer than the temporary buffer. */
    {
        char *big = (char *)malloc((size_t)n + 1);
        int ok;
        if (!big) {
            b->err = 1;
            return 0;
        }
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        ok = hb_putn(b, big, (size_t)n);
        free(big);
        return ok;
    }
}

char *hb_detach(htmlbuf *b, size_t *len)
{
    char *p = b->data;

    if (p && b->cap > b->len)
        p[b->len] = 0;
    if (len)
        *len = b->len;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return p;
}

void hb_escape(htmlbuf *b, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '&')
            hb_puts(b, "&amp;");
        else if (c == '<')
            hb_puts(b, "&lt;");
        else if (c == '>')
            hb_puts(b, "&gt;");
        else if (c == '"')
            hb_puts(b, "&quot;");
        else if (c == '\'')
            hb_puts(b, "&#39;");
        else
            hb_putc(b, (char)c);
    }
}

static void emit_escaped_char(htmlbuf *b, unsigned char c)
{
    if (c == '&')
        hb_puts(b, "&amp;");
    else if (c == '<')
        hb_puts(b, "&lt;");
    else if (c == '>')
        hb_puts(b, "&gt;");
    else
        hb_putc(b, (char)c);
}

void hb_wml_text(htmlbuf *b, const char *s, size_t n, int preserve)
{
    char *plain;
    size_t i, m;

    if (!n)
        return;

    (void)preserve; /* handled per-space below: only runs/edges become &#160; */

    plain = (char *)malloc(n + 1);
    if (!plain) {
        b->err = 1;
        return;
    }
    m = xml_decode(s, n, plain, n + 1);

    for (i = 0; i < m; i++) {
        unsigned char c = (unsigned char)plain[i];

        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';

        if (c == ' ') {
            int lead = (i == 0);
            int trail = (i + 1 == m);
            int prev_space = (i > 0 && plain[i - 1] == ' ');
            int next_space = (i + 1 < m && plain[i + 1] == ' ');
            if (lead || trail || prev_space || next_space)
                hb_puts(b, "&#160;");
            else
                hb_putc(b, ' ');
        } else {
            emit_escaped_char(b, c);
        }
    }
    free(plain);
}
