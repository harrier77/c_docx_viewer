/*
 * dev/check_litehtml.c - collaudo di integrazione (non fa parte della libreria).
 *
 * Converte un .docx e lo passa a litehtml_c.dll, poi stampa le metriche di
 * layout. Serve a verificare che l'HTML/CSS generato sia digerito da litehtml
 * senza aprire una finestra.
 *
 *     dev\check_litehtml.exe test.docx
 */
#include "../docx.h"
#include "litehtml_c.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_bold, g_italic;

static void *cb_create_font(const char *family, int size, int bold, int italic,
                            int underline, int strike, lh_font_metrics *fm)
{
    (void)family; (void)underline; (void)strike;
    g_bold = bold;
    g_italic = italic;
    if (fm) {
        fm->font_size = size;
        fm->height = size * 3 / 2;
        fm->ascent = size;
        fm->descent = size / 3;
        fm->x_height = size / 2;
        fm->ch_width = size / 2;
        fm->draw_spaces = 0;
        fm->sub_shift = size / 3;
        fm->super_shift = size / 3;
    }
    return (void *)(size_t)(size ? size : 1);
}

static void cb_delete_font(void *font) { (void)font; }

static int cb_text_width(const char *text, void *font)
{
    int size = (int)(size_t)font;
    return (int)strlen(text) * size / 2;
}

static void cb_draw_text(void *hdc, const char *text, void *font, unsigned rgb,
                         int x, int y, int w, int h)
{
    (void)hdc; (void)text; (void)font; (void)rgb; (void)x; (void)y; (void)w; (void)h;
}

static void cb_solid(void *hdc, int x, int y, int w, int h, unsigned rgb)
{
    (void)hdc; (void)x; (void)y; (void)w; (void)h; (void)rgb;
}

static void cb_borders(void *hdc, int x, int y, int w, int h,
                       int lw, int tw, int rw, int bw,
                       unsigned lc, unsigned tc, unsigned rc, unsigned bc)
{
    (void)hdc; (void)x; (void)y; (void)w; (void)h;
    (void)lw; (void)tw; (void)rw; (void)bw;
    (void)lc; (void)tc; (void)rc; (void)bc;
}

int main(int argc, char **argv)
{
    HMODULE dll;
    void *(*p_create)(const char *, const lh_callbacks *, int, int);
    int (*p_render)(void *, int);
    int (*p_height)(void *);
    int (*p_width)(void *);
    void (*p_destroy)(void *);
    const char *(*p_version)(void);
    lh_callbacks cb;
    docx_html h;
    void *doc;
    int w, height;

    if (argc < 2) {
        fprintf(stderr, "uso: check_litehtml.exe file.docx\n");
        return 2;
    }

    if (!docx_file_to_html(argv[1], NULL, &h)) {
        fprintf(stderr, "conversione fallita: %s\n", h.error);
        return 1;
    }
    printf("html: %lu byte, %d paragrafi\n", (unsigned long)h.len, h.paragraphs);

    dll = LoadLibraryA("litehtml_c.dll");
    if (!dll)
        dll = LoadLibraryA("..\\sunburst\\litehtml_c.dll");
    if (!dll)
        dll = LoadLibraryA("C:\\Users\\pr30565\\Desktop\\Pure_C\\sunburst\\litehtml_c.dll");
    if (!dll) {
        fprintf(stderr, "litehtml_c.dll non trovata\n");
        docx_html_free(&h);
        return 1;
    }

    p_create  = (void *(*)(const char *, const lh_callbacks *, int, int))
                GetProcAddress(dll, "lh_create");
    p_render  = (int (*)(void *, int))GetProcAddress(dll, "lh_render");
    p_height  = (int (*)(void *))GetProcAddress(dll, "lh_height");
    p_width   = (int (*)(void *))GetProcAddress(dll, "lh_width");
    p_destroy = (void (*)(void *))GetProcAddress(dll, "lh_destroy");
    p_version = (const char *(*)(void))GetProcAddress(dll, "lh_version");

    if (!p_create || !p_render || !p_height || !p_destroy) {
        fprintf(stderr, "export lh_* mancanti nella DLL\n");
        docx_html_free(&h);
        return 1;
    }

    memset(&cb, 0, sizeof(cb));
    cb.create_font = cb_create_font;
    cb.delete_font = cb_delete_font;
    cb.text_width = cb_text_width;
    cb.draw_text = cb_draw_text;
    cb.draw_solid_fill = cb_solid;
    cb.draw_borders = cb_borders;

    printf("litehtml: %s\n", p_version ? p_version() : "(versione sconosciuta)");

    doc = p_create(h.data, &cb, 800, 600);
    if (!doc) {
        fprintf(stderr, "lh_create ha restituito NULL\n");
        docx_html_free(&h);
        return 1;
    }
    w = p_render(doc, 800);
    height = p_height(doc);
    printf("layout: width=%d height=%d\n", p_width ? p_width(doc) : w, height);
    p_destroy(doc);
    docx_html_free(&h);

    if (height <= 0) {
        fprintf(stderr, "ERRORE: altezza nulla\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}
