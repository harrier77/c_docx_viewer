#include "xmlbuf.h"

#include <string.h>

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t put_utf8(char *out, size_t cap, unsigned cp)
{
    size_t n = 0;

    if (cp < 0x80) {
        if (n < cap) out[n] = (char)cp;
        n++;
    } else if (cp < 0x800) {
        if (n < cap) out[n] = (char)(0xC0 | (cp >> 6));
        n++;
        if (n < cap) out[n] = (char)(0x80 | (cp & 0x3F));
        n++;
    } else if (cp < 0x10000) {
        if (n < cap) out[n] = (char)(0xE0 | (cp >> 12));
        n++;
        if (n < cap) out[n] = (char)(0x80 | ((cp >> 6) & 0x3F));
        n++;
        if (n < cap) out[n] = (char)(0x80 | (cp & 0x3F));
        n++;
    } else {
        if (n < cap) out[n] = (char)(0xF0 | (cp >> 18));
        n++;
        if (n < cap) out[n] = (char)(0x80 | ((cp >> 12) & 0x3F));
        n++;
        if (n < cap) out[n] = (char)(0x80 | ((cp >> 6) & 0x3F));
        n++;
        if (n < cap) out[n] = (char)(0x80 | (cp & 0x3F));
        n++;
    }
    return n;
}

int xml_entity(const char **pp, const char *end, char *out, size_t cap)
{
    const char *p = *pp;
    unsigned cp = 0;
    int i;

    if (p >= end || *p != '&')
        return 0;
    p++;

    if (p < end && *p == '#') {
        int base = 10;
        int digits = 0;
        p++;
        if (p < end && (*p == 'x' || *p == 'X')) {
            base = 16;
            p++;
        }
        while (p < end && *p != ';') {
            int v = base == 16 ? hexval((unsigned char)*p)
                               : (*p >= '0' && *p <= '9') ? *p - '0' : -1;
            if (v < 0)
                return 0;
            cp = cp * (unsigned)base + (unsigned)v;
            if (cp > 0x10FFFF)
                return 0;
            digits++;
            p++;
        }
        if (!digits || p >= end || *p != ';')
            return 0;
        p++;
        put_utf8(out, cap, cp);
        *pp = p;
        return 1;
    }

    {
        static const struct { const char *name; unsigned cp; } ent[] = {
            { "amp;",  '&' }, { "lt;", '<' }, { "gt;", '>' },
            { "quot;", '"' }, { "apos;", '\'' }, { "nbsp;", 0xA0 }
        };
        for (i = 0; i < (int)(sizeof(ent) / sizeof(ent[0])); i++) {
            size_t l = strlen(ent[i].name);
            if ((size_t)(end - p) >= l && memcmp(p, ent[i].name, l) == 0) {
                put_utf8(out, cap, ent[i].cp);
                *pp = p + l;
                return 1;
            }
        }
    }
    return 0;
}

size_t xml_decode(const char *p, size_t n, char *out, size_t cap)
{
    const char *end = p + n;
    size_t w = 0;

    while (p < end) {
        if (*p == '&') {
            char tmp[8] = {0};
            const char *save = p;
            if (xml_entity(&p, end, tmp, sizeof(tmp))) {
                size_t l = strlen(tmp);
                if (w + l + 1 <= cap) {
                    memcpy(out + w, tmp, l);
                    w += l;
                } else {
                    if (w < cap) out[w] = 0;
                    return w;
                }
            } else {
                p = save;
                if (w + 1 < cap) out[w++] = *p;
                p++;
            }
        } else {
            if (w + 1 < cap) out[w++] = *p;
            p++;
        }
    }
    if (cap)
        out[w < cap ? w : cap - 1] = 0;
    return w;
}

/* ------------------------------------------------------------------ */

void xs_init(xscan *s, const char *p, size_t n)
{
    s->p = p;
    s->end = p + n;
}

static void skip_until(xscan *s, const char *needle)
{
    size_t l = strlen(needle);
    while (s->p + l <= s->end) {
        if (memcmp(s->p, needle, l) == 0) {
            s->p += l;
            return;
        }
        s->p++;
    }
    s->p = s->end;
}

int xs_next(xscan *s, xtok *t)
{
    memset(t, 0, sizeof(*t));

    while (s->p < s->end) {
        if (*s->p != '<') {
            const char *start = s->p;
            while (s->p < s->end && *s->p != '<')
                s->p++;
            t->kind = XT_TEXT;
            t->text = start;
            t->text_len = (size_t)(s->p - start);
            return 1;
        }

        /* markup declarations, processing instructions, comments */
        if (s->p + 1 < s->end && s->p[1] == '?') {
            skip_until(s, "?>");
            continue;
        }
        if (s->p + 3 < s->end && memcmp(s->p, "<!--", 4) == 0) {
            skip_until(s, "-->");
            continue;
        }
        if (s->p + 8 < s->end && memcmp(s->p, "<![CDATA[", 9) == 0) {
            const char *start = s->p + 9;
            s->p = start;
            skip_until(s, "]]>");
            t->kind = XT_TEXT;
            t->text = start;
            t->text_len = (size_t)(s->p - 3 - start);
            return 1;
        }
        if (s->p + 1 < s->end && s->p[1] == '!') {
            while (s->p < s->end && *s->p != '>')
                s->p++;
            if (s->p < s->end)
                s->p++;
            continue;
        }

        /* regular tag */
        {
            const char *name;
            const char *attrs;
            int quote = 0;

            s->p++; /* '<' */
            t->kind = XT_TAG;
            if (s->p < s->end && *s->p == '/') {
                t->closing = 1;
                s->p++;
            }
            name = s->p;
            while (s->p < s->end && *s->p != '>' && *s->p != '/' &&
                   *s->p != ' ' && *s->p != '\t' && *s->p != '\r' && *s->p != '\n')
                s->p++;
            t->name = name;
            t->name_len = (size_t)(s->p - name);

            attrs = s->p;
            while (s->p < s->end) {
                char c = *s->p;
                if (quote) {
                    if (c == quote)
                        quote = 0;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '>') {
                    break;
                }
                s->p++;
            }
            t->attrs = attrs;
            t->attrs_len = (size_t)(s->p - attrs);
            /* self closing: last non-space char before '>' is '/' */
            {
                const char *q = s->p;
                while (q > attrs && (q[-1] == ' ' || q[-1] == '\t' ||
                                     q[-1] == '\r' || q[-1] == '\n'))
                    q--;
                if (q > attrs && q[-1] == '/')
                    t->self_closing = 1;
            }
            if (s->p < s->end && *s->p == '>')
                s->p++;
            return 1;
        }
    }
    return 0;
}

static const char *local_name(const xtok *t, size_t *len)
{
    const char *p = t->name;
    size_t i;
    const char *colon = NULL;

    for (i = 0; i < t->name_len; i++)
        if (p[i] == ':')
            colon = p + i;
    if (colon) {
        *len = (size_t)(t->name_len - (size_t)(colon - p) - 1);
        return colon + 1;
    }
    *len = t->name_len;
    return p;
}

int xname_is(const xtok *t, const char *local)
{
    size_t len;
    const char *p = local_name(t, &len);

    return len == strlen(local) && memcmp(p, local, len) == 0;
}

int xname_is_w(const xtok *t, const char *local)
{
    const char *p = t->name;
    size_t len = t->name_len;

    if (len >= 2 && p[1] == ':') {
        if (p[0] != 'w' && p[0] != 'W')
            return 0;
        p += 2;
        len -= 2;
    }
    return len == strlen(local) && memcmp(p, local, len) == 0;
}

int xattr(const char *attrs, size_t alen, const char *name,
          char *val, size_t cap)
{
    const char *p = attrs;
    const char *end = attrs + alen;
    size_t nl = strlen(name);

    while (p < end) {
        const char *n, *v;
        size_t nlen;

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '/'))
            p++;
        if (p >= end)
            break;
        n = p;
        while (p < end && *p != '=' && *p != ' ' && *p != '\t' &&
               *p != '\r' && *p != '\n' && *p != '>')
            p++;
        nlen = (size_t)(p - n);
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            p++;
        if (p < end && *p == '=')
            p++;
        else
            v = NULL;

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            p++;

        if (p < end && (*p == '"' || *p == '\'')) {
            char q = *p++;
            v = p;
            while (p < end && *p != q)
                p++;
            if (nlen == nl && memcmp(n, name, nl) == 0) {
                xml_decode(v, (size_t)(p - v), val, cap);
                return 1;
            }
            if (p < end)
                p++;
        } else {
            v = p;
            while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>')
                p++;
            if (v && nlen == nl && memcmp(n, name, nl) == 0) {
                xml_decode(v, (size_t)(p - v), val, cap);
                return 1;
            }
        }
    }
    return 0;
}
