/*
 * renderer_d2d.c - implementazione Direct2D del renderer (build MinGW/GCC).
 * Per TCC restituisce NULL (fallback a GDI).
 *
 * Nota: COBJMACROS e INITGUID DEVONO essere definiti PRIMA di qualsiasi
 * header Windows, altrimenti DEFINE_GUID si espande come dichiarazion
 * esterne (EXTERN_C const GUID) e il linker riporta undefined reference.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include "renderer.h"
#include "text_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef __TINYC__

Renderer *renderer_d2d_create(void)
{
    return NULL;
}

#else

#include <d2d1.h>
#include <dwrite.h>
#include <stdlib.h>
#include <math.h>

/* Renderer-internal font cache entry */
typedef struct FontCacheEntryR {
    char        *family;
    int          size;
    int          bold;
    int          italic;
    int          underline;
    int          strike;
    IDWriteTextFormat *format;
    struct FontCacheEntryR *next;
} FontCacheEntryR;

/* Font handle structure (matches d2dview.c's DwFontHandle) */
typedef struct DwFontHandle {
    IDWriteTextFormat *format;
    DWRITE_FONT_METRICS metrics;
    float        x_height;
    int          underline;
    int          strike;
    int          size_px;
    float        ascent;
    float        descent;
    float        line_gap;
} DwFontHandle;

typedef struct D2dRenderer
{
    Renderer               base;
    ID2D1Factory          *factory;
    ID2D1DCRenderTarget   *renderTarget;
    ID2D1SolidColorBrush  *brush;
    HDC                    memdc;
    HBITMAP                membmp;
    HGDIOBJ                memold;
    int                    w, h;
    int                    in_draw;

    /* DirectWrite */
    IDWriteFactory        *dw_factory;

    /* Font cache for this renderer instance */
    FontCacheEntryR       *font_cache;

    /* Current transform matrix */
    D2D1_MATRIX_3X2_F    current_transform;
    int                  current_transform_valid;
} D2dRenderer;

static D2dRenderer *d2d_of(Renderer *r)
{
    return (D2dRenderer *)r;
}

static int d2d_begin_draw(Renderer *r, HWND hwnd, int w, int h)
{
    D2dRenderer *d = d2d_of(r);
    HDC screen;
    HRESULT hr;

    if (w < 1) w = 1;
    if (h < 1) h = 1;

    /* Se un frame precedente e' rimasto aperto (es. Present non raggiunto),
     * chiudi il batch: lasciare il target in stato "drawing" rende invalide
     * le chiamate successive e fa fallire EndDraw (schermo nero).
     * Niente early-return "stesso size": BindDC + BeginDraw ad ogni frame,
     * come nell'esempio di riferimento funzionante. */
    if (d->in_draw) {
        if (d->renderTarget)
            ID2D1DCRenderTarget_EndDraw(d->renderTarget, NULL, NULL);
        d->in_draw = 0;
    }

    if (d->w != w || d->h != h) {
        if (d->renderTarget) {
            ID2D1DCRenderTarget_Release(d->renderTarget);
            d->renderTarget = NULL;
        }
        if (d->memold && d->memdc) {
            SelectObject(d->memdc, d->memold);
            d->memold = NULL;
        }
        if (d->membmp) {
            DeleteObject(d->membmp);
            d->membmp = NULL;
        }
        if (d->memdc) {
            DeleteDC(d->memdc);
            d->memdc = NULL;
        }
    }

    if (!d->memdc) {
        screen = GetDC(hwnd);
        if (!screen)
            return 0;
        d->memdc = CreateCompatibleDC(screen);
        d->membmp = CreateCompatibleBitmap(screen, w, h);
        ReleaseDC(hwnd, screen);

        if (!d->memdc || !d->membmp)
            return 0;

        d->memold = SelectObject(d->memdc, d->membmp);
        d->w = w;
        d->h = h;
    }

    if (!d->renderTarget) {
        D2D1_RENDER_TARGET_PROPERTIES props = {
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE },
            96.0f, 96.0f,
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT
        };
        hr = ID2D1Factory_CreateDCRenderTarget(d->factory, &props, &d->renderTarget);
        if (FAILED(hr) || !d->renderTarget)
            return 0;

        {
            D2D1_COLOR_F init_col = {0, 0, 0, 1};
            hr = ID2D1DCRenderTarget_CreateSolidColorBrush(d->renderTarget,
                &init_col, NULL, &d->brush);
        }
        if (FAILED(hr) || !d->brush)
            return 0;
    }

    {
        RECT rc = {0, 0, w, h};
        hr = ID2D1DCRenderTarget_BindDC(d->renderTarget, d->memdc, &rc);
        if (FAILED(hr))
            return 0;
    }

    ID2D1DCRenderTarget_BeginDraw(d->renderTarget);
    ID2D1DCRenderTarget_SetAntialiasMode(d->renderTarget, D2D1_ANTIALIAS_MODE_ALIASED);

    /* Clear esplicito (come nell'esempio di riferimento funzionante).
     * Senza Clear, con D2D1_ALPHA_MODE_IGNORE, EndDraw riversa sul DC anche
     * i pixel mai disegnati (zero = nero): ecco perche' lo schermo appariva
     * completamente nero. Il colore e' quello dello sfondo scrivania del
     * viewer: cosi' anche con zoom < 1 (dove il FillRect scalato dello
     * sfondo non copre tutto il client) non restano zone bianche.
     * Il Clear ignora la trasformata: va bene anche in zoom. */
    {
        D2D1_COLOR_F clear_col = {240.0f / 255.0f, 240.0f / 255.0f, 242.0f / 255.0f, 1.0f};
        ID2D1DCRenderTarget_Clear(d->renderTarget, &clear_col);
    }

    /* Reapply transform if one was set */
    if (d->current_transform_valid) {
        ID2D1DCRenderTarget_SetTransform(d->renderTarget, &d->current_transform);
    }

    d->in_draw = 1;
    return 1;
}

static void d2d_end_draw(Renderer *r)
{
    (void)r;
}

static void d2d_present(Renderer *r, HDC target, int w, int h)
{
    D2dRenderer *d = d2d_of(r);
    if (d->renderTarget && d->in_draw) {
        HRESULT hr = ID2D1DCRenderTarget_EndDraw(d->renderTarget, NULL, NULL);
        d->in_draw = 0;
        /* Se EndDraw fallisce il batch non viene riversato sul DC:
         * lo schermo resta nero. Il log e' essenziale per diagnosi. */
        if (FAILED(hr))
            fprintf(stderr, "[D2D] EndDraw FAILED hr=0x%08lX\n", (unsigned long)hr);
    }
    if (d->memdc && target && w > 0 && h > 0) {
        BitBlt(target, 0, 0, w, h, d->memdc, 0, 0, SRCCOPY);
    }
}

static D2D1_COLOR_F color_to_d2d(COLORREF c)
{
    D2D1_COLOR_F col = {
        GetRValue(c) / 255.0f,
        GetGValue(c) / 255.0f,
        GetBValue(c) / 255.0f,
        1.0f
    };
    return col;
}

static void d2d_fill_rect(Renderer *r, int x, int y, int w, int h, COLORREF color)
{
    D2dRenderer *d = d2d_of(r);
    D2D1_RECT_F rc;
    D2D1_COLOR_F col;

    if (!d->renderTarget || w <= 0 || h <= 0)
        return;

    rc.left = (float)x;
    rc.top = (float)y;
    rc.right = (float)(x + w);
    rc.bottom = (float)(y + h);

    col = color_to_d2d(color);
    ID2D1SolidColorBrush_SetColor(d->brush, &col);
    ID2D1DCRenderTarget_FillRectangle(d->renderTarget, &rc, (ID2D1Brush *)d->brush);
}

static void d2d_frame_rect(Renderer *r, int x, int y, int w, int h, COLORREF color)
{
    D2dRenderer *d = d2d_of(r);
    D2D1_RECT_F rc;
    D2D1_COLOR_F col;

    if (!d->renderTarget || w <= 0 || h <= 0)
        return;

    rc.left = (float)x + 0.5f;
    rc.top = (float)y + 0.5f;
    rc.right = (float)(x + w) - 0.5f;
    rc.bottom = (float)(y + h) - 0.5f;

    col = color_to_d2d(color);
    ID2D1SolidColorBrush_SetColor(d->brush, &col);
    ID2D1DCRenderTarget_DrawRectangle(d->renderTarget, &rc, (ID2D1Brush *)d->brush, 1.0f, NULL);
}

static void d2d_draw_ellipse(Renderer *r, int x, int y, int w, int h, COLORREF color)
{
    D2dRenderer *d = d2d_of(r);
    D2D1_ELLIPSE el;
    D2D1_COLOR_F col;

    if (!d->renderTarget || w <= 0 || h <= 0)
        return;

    el.point.x = (float)x + (float)w / 2.0f;
    el.point.y = (float)y + (float)h / 2.0f;
    el.radiusX = (float)w / 2.0f;
    el.radiusY = (float)h / 2.0f;

    col = color_to_d2d(color);
    ID2D1SolidColorBrush_SetColor(d->brush, &col);
    ID2D1DCRenderTarget_FillEllipse(d->renderTarget, &el, (ID2D1Brush *)d->brush);
}

static void d2d_draw_rect(Renderer *r, int x, int y, int w, int h, COLORREF color)
{
    d2d_fill_rect(r, x, y, w, h, color);
    d2d_frame_rect(r, x, y, w, h, RGB(0, 0, 0));
}

static void d2d_draw_text(Renderer *r, const char *utf8, void *font, COLORREF color,
                          int x, int y, int w, int h)
{
    D2dRenderer *d = d2d_of(r);
    DwFontHandle *handle = (DwFontHandle *)font;
    WCHAR *ws;
    IDWriteTextLayout *layout = NULL;
    IDWriteTextFormat *format = NULL;
    D2D1_COLOR_F col;
    HRESULT hr;

    if (!d->renderTarget || !utf8 || !utf8[0] || !handle || !handle->format)
        return;

    ws = utf8_to_wide(utf8);
    if (!ws)
        return;

    format = handle->format;
    IDWriteTextFormat_AddRef(format);

    /* Create text layout for measuring and drawing */
    hr = IDWriteFactory_CreateTextLayout(d->dw_factory, ws, lstrlenW(ws),
        format, (FLOAT)w, (FLOAT)h, &layout);
    free(ws);
    
    if (FAILED(hr) || !layout) {
        if (format) IDWriteTextFormat_Release(format);
        return;
    }

    /* Configure layout - no word wrapping for single line */
    IDWriteTextLayout_SetWordWrapping(layout, DWRITE_WORD_WRAPPING_NO_WRAP);

    /* Set up color brush */
    col.r = GetRValue(color) / 255.0f;
    col.g = GetGValue(color) / 255.0f;
    col.b = GetBValue(color) / 255.0f;
    col.a = 1.0f;
    ID2D1SolidColorBrush_SetColor(d->brush, &col);

    /* Draw using DirectWrite text layout - this uses the render target's transform */
    D2D1_POINT_2F origin = { (FLOAT)x, (FLOAT)y };
    ID2D1DCRenderTarget_DrawTextLayout(d->renderTarget,
        origin, layout, (ID2D1Brush *)d->brush, D2D1_DRAW_TEXT_OPTIONS_NONE);

    IDWriteTextLayout_Release(layout);
    IDWriteTextFormat_Release(format);
}

static void d2d_draw_image(Renderer *r, void *native_bitmap, int src_w, int src_h,
                           int x, int y, int w, int h)
{
    D2dRenderer *d = d2d_of(r);
    HDC mem;
    HGDIOBJ old;
    BLENDFUNCTION bf;

    if (!d->memdc || !native_bitmap || src_w <= 0 || src_h <= 0 || w <= 0 || h <= 0)
        return;

    mem = CreateCompatibleDC(d->memdc);
    if (!mem)
        return;
    old = SelectObject(mem, (HBITMAP)native_bitmap);
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    /* Il DC del back buffer e' bindato al render target D2D: scriverci
     * direttamente mentre il target e' in stato "drawing" fa perdere
     * l'immagine a EndDraw. Si usa quindi il DC di interop, come per il testo. */
    if (d->renderTarget && d->in_draw) {
        ID2D1GdiInteropRenderTarget *interop = NULL;
        if (SUCCEEDED(ID2D1DCRenderTarget_QueryInterface(d->renderTarget,
                        &IID_ID2D1GdiInteropRenderTarget, (void **)&interop))) {
            HDC dc = NULL;
            if (SUCCEEDED(ID2D1GdiInteropRenderTarget_GetDC(interop,
                            D2D1_DC_INITIALIZE_MODE_COPY, &dc))) {
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, NULL);
                AlphaBlend(dc, x, y, w, h, mem, 0, 0, src_w, src_h, bf);
                ID2D1GdiInteropRenderTarget_ReleaseDC(interop, NULL);
            }
            ID2D1GdiInteropRenderTarget_Release(interop);
        }
    } else {
        SetStretchBltMode(d->memdc, HALFTONE);
        SetBrushOrgEx(d->memdc, 0, 0, NULL);
        AlphaBlend(d->memdc, x, y, w, h, mem, 0, 0, src_w, src_h, bf);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
}

static void d2d_draw_borders(Renderer *r, int x, int y, int w, int h,
                             int left_w, int top_w, int right_w, int bottom_w,
                             COLORREF left_c, COLORREF top_c,
                             COLORREF right_c, COLORREF bottom_c)
{
    if (left_w > 0)
        d2d_fill_rect(r, x, y, left_w, h, left_c);
    if (top_w > 0)
        d2d_fill_rect(r, x, y, w, top_w, top_c);
    if (right_w > 0)
        d2d_fill_rect(r, x + w - right_w, y, right_w, h, right_c);
    if (bottom_w > 0)
        d2d_fill_rect(r, x, y + h - bottom_w, w, bottom_w, bottom_c);
}

static void *d2d_get_native_target(Renderer *r)
{
    return (void *)d2d_of(r)->memdc;
}

/* --- Zoom / pan via matrice di trasformazione (D2D puro). ---
 * Il layout e i font restano alla scala logica (zoom = 1.0):
 * la trasformazione viene applicata al render target prima di ogni frame.
 */
static void d2d_set_transform(Renderer *r, float m11, float m12, float m21, float m22, float dx, float dy)
{
    D2dRenderer *d = d2d_of(r);
    d->current_transform._11 = m11;
    d->current_transform._12 = m12;
    d->current_transform._21 = m21;
    d->current_transform._22 = m22;
    d->current_transform._31 = dx;
    d->current_transform._32 = dy;
    d->current_transform_valid = 1;
    fprintf(stderr, "[D2D_TRANSFORM] set m11=%f m12=%f m21=%f m22=%f dx=%f dy=%f\n",
            m11, m12, m21, m22, dx, dy);

    /* Apply transform immediately if we're in a draw frame */
    if (d->renderTarget && d->in_draw) {
        ID2D1DCRenderTarget_SetTransform(d->renderTarget, &d->current_transform);
    }
}

static void d2d_reset_transform(Renderer *r)
{
    D2dRenderer *d = d2d_of(r);
    d->current_transform_valid = 0;

    /* Reset to identity */
    D2D1_MATRIX_3X2_F identity = { { { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f } } };

    if (d->renderTarget && d->in_draw) {
        ID2D1DCRenderTarget_SetTransform(d->renderTarget, &identity);
    }
}

static void d2d_destroy_font_cache(FontCacheEntryR *cache)
{
    FontCacheEntryR *e = cache;
    while (e) {
        FontCacheEntryR *next = e->next;
        if (e->format) IDWriteTextFormat_Release(e->format);
        free(e->family);
        free(e);
        e = next;
    }
}

static void d2d_destroy(Renderer *r)
{
    D2dRenderer *d = d2d_of(r);

    if (d->brush) {
        ID2D1SolidColorBrush_Release(d->brush);
        d->brush = NULL;
    }
    if (d->renderTarget) {
        ID2D1DCRenderTarget_Release(d->renderTarget);
        d->renderTarget = NULL;
    }
    if (d->factory) {
        ID2D1Factory_Release(d->factory);
        d->factory = NULL;
    }
    if (d->memold && d->memdc) {
        SelectObject(d->memdc, d->memold);
        d->memold = NULL;
    }
    if (d->membmp) {
        DeleteObject(d->membmp);
        d->membmp = NULL;
    }
    if (d->memdc) {
        DeleteDC(d->memdc);
        d->memdc = NULL;
    }
    /* Free font cache */
    d2d_destroy_font_cache(d->font_cache);
    d->font_cache = NULL;
    free(d);
}

static const RendererVtbl g_d2d_vtbl = {
    d2d_begin_draw,
    d2d_end_draw,
    d2d_present,
    d2d_fill_rect,
    d2d_frame_rect,
    d2d_draw_ellipse,
    d2d_draw_rect,
    d2d_draw_text,
    d2d_draw_image,
    d2d_draw_borders,
    d2d_get_native_target,
    d2d_set_transform,
    d2d_reset_transform,
    d2d_destroy
};

Renderer *renderer_d2d_create(IDWriteFactory *dw_factory,
                              D2dFontResolver resolver, void *resolver_ctx)
{
    D2dRenderer *d;
    HRESULT hr;
    (void)resolver; (void)resolver_ctx;

    d = (D2dRenderer *)calloc(1, sizeof(D2dRenderer));
    if (!d)
        return NULL;

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                           &IID_ID2D1Factory, NULL, (void **)&d->factory);
    if (FAILED(hr) || !d->factory) {
        free(d);
        return NULL;
    }

    d->dw_factory = dw_factory;
    d->font_cache = NULL;
    d->current_transform_valid = 0;
    d->base.vtbl = &g_d2d_vtbl;
    return &d->base;
}

#endif /* __TINYC__ */
