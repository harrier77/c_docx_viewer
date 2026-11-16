#ifndef XMLBUF_H
#define XMLBUF_H

#include <stddef.h>

/* Decode one XML entity at *pp (which points at '&'). Returns 1 and advances
 * *pp past the entity, writing the UTF-8 encoded code point to out/cap. */
int xml_entity(const char **pp, const char *end, char *out, size_t cap);

/* Decode entities in [p,p+n) into out (UTF-8, NUL terminated when cap > 0).
 * Returns the number of bytes written (excluding the terminator). */
size_t xml_decode(const char *p, size_t n, char *out, size_t cap);

/* ---- minimal sequential XML scanner (tags + text nodes) ---- */

typedef struct xscan {
    const char *p;
    const char *end;
} xscan;

enum {
    XT_TEXT = 0,
    XT_TAG  = 1
};

typedef struct xtok {
    int         kind;
    const char *name;      /* tag name, e.g. "w:p" */
    size_t      name_len;
    const char *attrs;     /* raw attribute text (between name and '>' ) */
    size_t      attrs_len;
    int         closing;   /* </tag> */
    int         self_closing;
    const char *text;      /* valid when kind == XT_TEXT */
    size_t      text_len;
} xtok;

void xs_init(xscan *s, const char *p, size_t n);
int  xs_next(xscan *s, xtok *t);

/* 1 when the tag local name (after the last ':') equals local. */
int  xname_is(const xtok *t, const char *local);

/* 1 when the tag prefix is "w" (or the name is unprefixed) and the local
 * name equals local. Used to ignore a:t, m:t, etc. from drawings. */
int  xname_is_w(const xtok *t, const char *local);

/* Look up an attribute by full name (e.g. "w:val", "r:id", "xml:space").
 * Copies the decoded value into val (NUL terminated). Returns 1 if found. */
int  xattr(const char *attrs, size_t alen, const char *name,
           char *val, size_t cap);

#endif /* XMLBUF_H */
