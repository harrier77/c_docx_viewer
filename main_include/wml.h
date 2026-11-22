#ifndef WML_H
#define WML_H

#include <stddef.h>
#include "htmlbuf.h"

typedef struct wml_rel {
    char id[48];
    char target[768];
} wml_rel;

typedef struct wml_rels {
    wml_rel *items;
    int      count;
} wml_rels;

/* Parse word/_rels/document.xml.rels. Returns 1 on success (count may be 0). */
int  wml_parse_rels(const char *xml, size_t len, wml_rels *rels);
void wml_free_rels(wml_rels *rels);

/* ---- stili paragrafo (solo w:jc, per la giustificazione) ---- */

typedef struct wml_style {
    char id[64];        /* w:styleId */
    char based_on[64];  /* w:basedOn w:val (catena di ereditarieta') */
    char jc[16];        /* w:jc w:val dentro w:pPr, "" se assente */
} wml_style;

typedef struct wml_styles {
    wml_style *items;
    int        count;
    int        def_idx;    /* stile predefinito (w:default="1"), -1 se nessuno */
    char       doc_jc[16]; /* w:jc di w:docDefaults/w:pPrDefault/w:pPr, "" se assente */
} wml_styles;

/* Parse word/styles.xml (opzionale). Returns 1 on success. */
int  wml_parse_styles(const char *xml, size_t len, wml_styles *st);
void wml_free_styles(wml_styles *st);

/*
 * Convert word/document.xml into HTML and append it to out.
 * rels/styles may be NULL. styles supplies w:jc inheritance for paragraphs
 * that do not set w:jc directly. *paragraphs is incremented for every
 * paragraph emitted. Returns 1 on success, 0 on allocation failure.
 */
int wml_to_html(const char *xml, size_t len, const wml_rels *rels,
                const wml_styles *styles, htmlbuf *out, int *paragraphs);

#endif /* WML_H */
