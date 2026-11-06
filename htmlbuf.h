#ifndef HTMLBUF_H
#define HTMLBUF_H

#include <stddef.h>

typedef struct htmlbuf {
    char  *data;
    size_t len;
    size_t cap;
    int    err;          /* 1 after an allocation failure */
} htmlbuf;

void  hb_init(htmlbuf *b);
void  hb_free(htmlbuf *b);
int   hb_reserve(htmlbuf *b, size_t extra);
int   hb_putc(htmlbuf *b, char c);
int   hb_putn(htmlbuf *b, const char *s, size_t n);
int   hb_puts(htmlbuf *b, const char *s);
int   hb_printf(htmlbuf *b, const char *fmt, ...);

/* Detach the buffer: NUL terminated, caller owns it. */
char *hb_detach(htmlbuf *b, size_t *len);

/* Escape plain text for HTML (no entity decoding). */
void  hb_escape(htmlbuf *b, const char *s, size_t n);

/*
 * XML text node -> HTML text. Decodes XML entities first, then escapes.
 * When preserve is non-zero (xml:space="preserve") leading, trailing and
 * repeated spaces become &#160; so litehtml does not collapse them.
 */
void  hb_wml_text(htmlbuf *b, const char *s, size_t n, int preserve);

#endif /* HTMLBUF_H */
