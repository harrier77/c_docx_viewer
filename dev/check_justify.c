/*
 * dev/check_justify.c - collaudo (non fa parte della libreria).
 *
 * Renderizza un paragrafo con text-align:justify e stampa la posizione di
 * ogni pezzo di testo disegnato, per verificare che litehtml estenda le
 * righe intermedie fino al bordo destro e NON estenda l'ultima riga.
 *
 *     dev\check_justify.exe
 */
#include "litehtml_c.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define BOX_W 400

static void *cb_create_font(const char *family, int size, int bold, int italic,
                            int underline, int strike, lh_font_metrics *fm)
{
    (void)family; (void)bold; (void)italic; (void)underline; (void)strike;
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

/* larghezza fissa per carattere: rende i calcoli prevedibili */
static int cb_text_width(const char *text, void *font)
{
    int size = (int)(size_t)font;
    return (int)strlen(text) * size / 2;
}

static int g_draws;

static void cb_draw_text(void *hdc, const char *text, void *font, unsigned rgb,
                         int x, int y, int w, int h)
{
    (void)hdc; (void)font; (void)rgb; (void)h;
    g_draws++;
    printf("  y=%3d  x=%3d w=%3d right=%3d  '%s'\n", y, x, w, x + w, text);
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

int main(void)
{
    static const char *html =
        "<!DOCTYPE html><html><head><style>"
        "body{margin:0;padding:0;width:400px;font-size:16px;}"
        "p{margin:0;}"
        "</style></head><body>"
        "<p style=\"text-align:justify\">"
        "alpha bravo charlie delta echo foxtrot golf hotel india juliet "
        "kilo lima mike november oscar papa quebec romeo sierra tango "
        "uniform victor whiskey xray yankee zulu"
        "</p></body></html>";

    HMODULE dll;
    void *(*p_create)(const char *, const lh_callbacks *, int, int);
    int (*p_render)(void *, int);
    int (*p_height)(void *);
    void (*p_destroy)(void *);
    lh_callbacks cb;
    void *doc;
    int h;

    dll = LoadLibraryA("litehtml_c.dll");
    if (!dll)
        dll = LoadLibraryA("..\\sunburst\\litehtml_c.dll");
    if (!dll)
        dll = LoadLibraryA("C:\\Users\\pr30565\\Desktop\\Pure_C\\sunburst\\litehtml_c.dll");
    if (!dll) {
        fprintf(stderr, "litehtml_c.dll non trovata\n");
        return 1;
    }

    p_create  = (void *(*)(const char *, const lh_callbacks *, int, int))
                GetProcAddress(dll, "lh_create");
    p_render  = (int (*)(void *, int))GetProcAddress(dll, "lh_render");
    p_height  = (int (*)(void *))GetProcAddress(dll, "lh_height");
    p_destroy = (void (*)(void *))GetProcAddress(dll, "lh_destroy");

    if (!p_create || !p_render || !p_height || !p_destroy) {
        fprintf(stderr, "export lh_* mancanti nella DLL\n");
        return 1;
    }

    memset(&cb, 0, sizeof(cb));
    cb.create_font = cb_create_font;
    cb.delete_font = cb_delete_font;
    cb.text_width = cb_text_width;
    cb.draw_text = cb_draw_text;
    cb.draw_solid_fill = cb_solid;
    cb.draw_borders = cb_borders;

    doc = p_create(html, &cb, BOX_W, 600);
    if (!doc) {
        fprintf(stderr, "lh_create NULL\n");
        return 1;
    }
    p_render(doc, BOX_W);
    h = p_height(doc);
    printf("layout height=%d\n\npezzi disegnati durante lh_draw:\n", h);
    {
        HDC dc = CreateCompatibleDC(NULL);
        HBITMAP bmp = CreateCompatibleBitmap(GetDC(NULL), BOX_W, 600);
        HGDIOBJ old = SelectObject(dc, bmp);
        typedef void (*P_draw)(void *, void *, int, int, int, int, int, int);
        P_draw p_draw = (P_draw)GetProcAddress(dll, "lh_draw");
        if (p_draw)
            p_draw(doc, dc, 0, 0, 0, 0, BOX_W, 600);
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    p_destroy(doc);

    if (g_draws < 3) {
        fprintf(stderr, "ERRORE: troppe poche righe disegnate\n");
        return 1;
    }
    printf("\nOK (%d pezzi)\n", g_draws);
    return 0;
}
