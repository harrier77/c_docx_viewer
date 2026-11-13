/*
 * renderer_gdi.c - implementazione GDI del renderer.
 *
 * Possiede il back buffer (memdc + bitmap) e tutte le primitive GDI usate
 * dal viewer. La resa e' quella storica di docxview_new.c.
 */
#include "renderer.h"
#include "text_utf8.h"

#include <stdlib.h>

typedef struct GdiRenderer
{
    Renderer base;      /* deve restare il primo campo */
    HDC      memdc;
    HBITMAP  membmp;
    HGDIOBJ  memold;
    int      w, h;
} GdiRenderer;

static GdiRenderer *gdi_of(Renderer *r)
{
    return (GdiRenderer *)r;
}

static int gdi_begin_draw(Renderer *r, HWND hwnd, int w, int h)
{
    GdiRenderer *g = gdi_of(r);
    HDC screen;

    if (w < 1) w = 1;
    if (h < 1) h = 1;

    /* Stesse dimensioni: riusa il back buffer. */
    if (g->memdc && g->w == w && g->h == h)
        return 1;

    if (g->memold) {
        SelectObject(g->memdc, g->memold);
        g->memold = NULL;
    }
    if (g->membmp) {
        DeleteObject(g->membmp);
        g->membmp = NULL;
    }
    if (g->memdc) {
        DeleteDC(g->memdc);
        g->memdc = NULL;
    }

    screen = GetDC(hwnd);
    if (!screen)
        return 0;
    g->memdc = CreateCompatibleDC(screen);
    g->membmp = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(hwnd, screen);

    if (!g->memdc || !g->membmp)
        return 0;

    g->memold = SelectObject(g->memdc, g->membmp);
    g->w = w;
    g->h = h;
    return 1;
}

static void gdi_end_draw(Renderer *r)
{
    (void)r;
}

static void gdi_present(Renderer *r, HDC target, int w, int h)
{
    GdiRenderer *g = gdi_of(r);
    if (g->memdc && target && w > 0 && h > 0)
        BitBlt(target, 0, 0, w, h, g->memdc, 0, 0, SRCCOPY);
}

static void gdi_fill_rect(Renderer *r, int x, int y, int w, int h, COLORREF color)
{
    GdiRenderer *g = gdi_of(r);
    RECT rc;
    HBRUSH br;

    if (!g->memdc || w <= 0 || h <= 0)
        return;

    rc.left = x;
    rc.top = y;
    rc.right = x + w;
    rc.bottom = y + h;
    br = CreateSolidBrush(color);
    FillRect(g->memdc, &rc, br);
    DeleteObject(br);
}

static void gdi_frame_rect(Renderer *r, int x, int y, int w, int h, COLORREF color)
{
    GdiRenderer *g = gdi_of(r);
    RECT rc;
    HBRUSH br;

    if (!g->memdc || w <= 0 || h <= 0)
        return;

    rc.left = x;
    rc.top = y;
    rc.right = x + w;
    rc.bottom = y + h;
    br = CreateSolidBrush(color);
    FrameRect(g->memdc, &rc, br);
    DeleteObject(br);
}

static void gdi_draw_text(Renderer *r, const char *utf8, void *font, COLORREF color,
                          int x, int y, int w, int h)
{
    GdiRenderer *g = gdi_of(r);
    HGDIOBJ old = NULL;
    WCHAR *ws;
    RECT rc;

    if (!g->memdc || !utf8 || !utf8[0])
        return;

    ws = utf8_to_wide(utf8);
    if (!ws)
        return;

    if (font)
        old = SelectObject(g->memdc, (HFONT)font);
    SetMapMode(g->memdc, MM_TEXT);
    SetTextColor(g->memdc, color);
    SetBkMode(g->memdc, TRANSPARENT);

    /* Stessa geometria e stesse flag del vecchio percorso GDI in litedocx
     * (DrawTextW su un rettangolo): la resa deve restare identica. */
    rc.left = x;
    rc.top = y;
    rc.right = x + w;
    rc.bottom = y + h;
    DrawTextW(g->memdc, ws, lstrlenW(ws), &rc,
              DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);

    if (font)
        SelectObject(g->memdc, old);
    free(ws);
}

static void gdi_draw_ellipse(Renderer *r, int x, int y, int w, int h, COLORREF color)
{
    GdiRenderer *g = gdi_of(r);
    HBRUSH br;
    HGDIOBJ old_br, old_pen;

    if (!g->memdc || w <= 0 || h <= 0)
        return;

    br = CreateSolidBrush(color);
    old_br = SelectObject(g->memdc, br);
    old_pen = SelectObject(g->memdc, GetStockObject(NULL_PEN));
    Ellipse(g->memdc, x, y, x + w, y + h);
    SelectObject(g->memdc, old_pen);
    SelectObject(g->memdc, old_br);
    DeleteObject(br);
}

static void gdi_draw_rect(Renderer *r, int x, int y, int w, int h, COLORREF color)
{
    GdiRenderer *g = gdi_of(r);
    HBRUSH br;
    HPEN pen;
    HGDIOBJ old_br, old_pen;

    if (!g->memdc || w <= 0 || h <= 0)
        return;

    /* Rectangle() usa il pennello per il riempimento e la penna per il bordo.
     * Il vecchio codice lasciava la penna di default (nera, 1px): la replichiamo
     * per mantenere la resa pixel-identica dei marcatori quadrati. */
    br = CreateSolidBrush(color);
    pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    old_br = SelectObject(g->memdc, br);
    old_pen = SelectObject(g->memdc, pen);
    Rectangle(g->memdc, x, y, x + w, y + h);
    SelectObject(g->memdc, old_pen);
    SelectObject(g->memdc, old_br);
    DeleteObject(pen);
    DeleteObject(br);
}

static void gdi_draw_image(Renderer *r, void *native_bitmap, int src_w, int src_h,
                           int x, int y, int w, int h)
{
    GdiRenderer *g = gdi_of(r);
    HDC mem;
    HGDIOBJ old;
    BLENDFUNCTION bf;

    if (!g->memdc || !native_bitmap || src_w <= 0 || src_h <= 0 || w <= 0 || h <= 0)
        return;

    mem = CreateCompatibleDC(g->memdc);
    if (!mem)
        return;
    old = SelectObject(mem, (HBITMAP)native_bitmap);
    SetStretchBltMode(g->memdc, HALFTONE);
    SetBrushOrgEx(g->memdc, 0, 0, NULL);
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    AlphaBlend(g->memdc, x, y, w, h, mem, 0, 0, src_w, src_h, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
}

static void gdi_draw_borders(Renderer *r, int x, int y, int w, int h,
                             int left_w, int top_w, int right_w, int bottom_w,
                             COLORREF left_c, COLORREF top_c,
                             COLORREF right_c, COLORREF bottom_c)
{
    if (left_w > 0)
        gdi_fill_rect(r, x, y, left_w, h, left_c);
    if (top_w > 0)
        gdi_fill_rect(r, x, y, w, top_w, top_c);
    if (right_w > 0)
        gdi_fill_rect(r, x + w - right_w, y, right_w, h, right_c);
    if (bottom_w > 0)
        gdi_fill_rect(r, x, y + h - bottom_w, w, bottom_w, bottom_c);
}

static void *gdi_get_native_target(Renderer *r)
{
    return (void *)gdi_of(r)->memdc;
}

/* GDI: le trasformazioni vengono gestite nel font (cb_create_font) e nel
 * layout (layout_zoom_at), non qui. Questi sono no-op. */
static void gdi_set_transform(Renderer *r, float m11, float m12, float m21, float m22, float dx, float dy)
{
    (void)r; (void)m11; (void)m12; (void)m21; (void)m22; (void)dx; (void)dy;
}
static void gdi_reset_transform(Renderer *r)
{
    (void)r;
}

static void gdi_destroy(Renderer *r)
{
    GdiRenderer *g = gdi_of(r);

    if (g->memold) {
        SelectObject(g->memdc, g->memold);
        g->memold = NULL;
    }
    if (g->membmp) {
        DeleteObject(g->membmp);
        g->membmp = NULL;
    }
    if (g->memdc) {
        DeleteDC(g->memdc);
        g->memdc = NULL;
    }
    free(g);
}

static const RendererVtbl g_gdi_vtbl = {
    gdi_begin_draw,
    gdi_end_draw,
    gdi_present,
    gdi_fill_rect,
    gdi_frame_rect,
    gdi_draw_ellipse,
    gdi_draw_rect,
    gdi_draw_text,
    gdi_draw_image,
    gdi_draw_borders,
    gdi_get_native_target,
    gdi_set_transform,
    gdi_reset_transform,
    gdi_destroy
};

Renderer *renderer_gdi_create(void)
{
    GdiRenderer *g = (GdiRenderer *)calloc(1, sizeof(GdiRenderer));
    if (!g)
        return NULL;
    g->base.vtbl = &g_gdi_vtbl;
    return &g->base;
}
