/*
 * docx.h - DOCX -> HTML converter, pure C, TCC friendly.
 *
 * The converter has no main() and no CLI: it returns an UTF-8 HTML string
 * that the host application hands directly to litehtml's lh_create().
 *
 * Usage:
 *
 *     docx_html h;
 *     if (docx_file_to_html("test.docx", NULL, &h)) {
 *         void *doc = lh_create(h.data, &callbacks, w, h_client);
 *         ...
 *         docx_html_free(&h);
 *     } else {
 *         MessageBoxA(0, h.error, "docx", MB_ICONERROR);
 *     }
 */
#ifndef DOCX_H
#define DOCX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct docx_html_opts {
    const char *title;        /* <title>; NULL = document/file name */
    const char *extra_css;    /* appended after the embedded <style>, may be NULL */
    int         font_px;      /* base body font size in px; 0 = 16 */
    int         full_document;/* 1 = full HTML5 document with <style>; 0 = fragment */
} docx_html_opts;

/* Convenience initializer: full document, default font, no extra CSS. */
#define DOCX_HTML_OPTS_INIT { NULL, NULL, 0, 1 }

typedef struct docx_html {
    char  *data;              /* malloc'ed, NUL terminated; NULL on failure */
    size_t len;
    int    paragraphs;        /* number of paragraphs emitted */
    char   error[192];        /* empty on success */
} docx_html;

/*
 * Convert a .docx file. Returns 1 on success, 0 on failure (out->error set).
 * On success the caller owns out->data and must release it with
 * docx_html_free().
 */
int docx_file_to_html(const char *path, const docx_html_opts *opts, docx_html *out);

/*
 * Same, from a memory buffer holding the whole .docx (ZIP) image.
 */
int docx_mem_to_html(const void *buf, size_t size, const docx_html_opts *opts, docx_html *out);

/* Release the HTML string. Safe to call twice. */
void docx_html_free(docx_html *out);

/*
 * The built-in stylesheet, without CSS custom properties (litehtml does not
 * support var(--...)). Returns a pointer to a static buffer valid until the
 * next call with a different font_px.
 */
const char *docx_default_css(int font_px);

#ifdef __cplusplus
}
#endif

#endif /* DOCX_H */
