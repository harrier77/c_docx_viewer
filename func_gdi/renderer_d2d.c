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

#ifdef __TINYC__

Renderer *renderer_d2d_create(void)
{
    return NULL;
}

#else

#include <d2d1.h>
#include <dwrite.h>
#include <stdlib.h>

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

    if (d->in_draw && d->w == w && d->h == h)
        return 1;

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
    {
        /* Identita': union { struct{m11..dy}; struct{_11.._32}; float m[3][2]; }
         * -> servono 3 livelli di graffe per evitare -Wmissing-braces. */
        D2D1_MATRIX_3X2_F identity = {{ {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f} }};
        ID2D1DCRenderTarget_SetTransform(d->renderTarget, &identity);
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
        ID2D1DCRenderTarget_EndDraw(d->renderTarget, NULL, NULL);
        d->in_draw = 0;
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
    WCHAR *ws;

    if (!d->renderTarget || !utf8 || !utf8[0])
        return;

    ws = utf8_to_wide(utf8);
    if (!ws)
        return;

    /* --- DirectWrite path (requires dw_factory + valid HFONT) --- */
    if (d->dw_factory && font) {
        LOGFONTW lf;
        DWRITE_FONT_WEIGHT weight;
        DWRITE_FONT_STYLE style;
        IDWriteTextFormat *fmt = NULL;
        IDWriteTextLayout *layout = NULL;
        HRESULT hr;

        ZeroMemory(&lf, sizeof(lf));
        if (GetObjectW((HGDIOBJ)font, sizeof(LOGFONTW), &lf) == sizeof(LOGFONTW)) {
            float px_size = (float)(-lf.lfHeight); /* positive pixel-size */
            switch (lf.lfWeight) {
            case FW_BOLD:
            case FW_EXTRABOLD:
                weight = DWRITE_FONT_WEIGHT_BOLD;
                break;
            default:
                weight = DWRITE_FONT_WEIGHT_NORMAL;
                break;
            }
            style = (lf.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL);

            hr = IDWriteFactory_CreateTextFormat(d->dw_factory, lf.lfFaceName,
                NULL, weight, style, DWRITE_FONT_STRETCH_NORMAL,
                px_size, L"en-US", &fmt);
            if (SUCCEEDED(hr) && fmt) {
                UINT32 len = lstrlenW(ws);
                RECT rc = { x, y, x + w, y + h };

                hr = IDWriteFactory_CreateTextLayout(
                    d->dw_factory, ws, len, fmt,
                    (FLOAT)w, (FLOAT)h, &layout);
                if (SUCCEEDED(hr) && layout) {
                    IDWriteTextLayout_SetWordWrapping(layout, DWRITE_WORD_WRAPPING_NO_WRAP);
                    /* Draw text via GDI-interop DC so the output lands on the
                       same D2D-rendered back buffer used by the rest of the renderer. */
                    ID2D1GdiInteropRenderTarget *gdiInterop = NULL;
                    HDC dc = NULL;
                    HGDIOBJ old_font = NULL;

                    if (SUCCEEDED(ID2D1DCRenderTarget_QueryInterface(
                            d->renderTarget, &IID_ID2D1GdiInteropRenderTarget,
                            (void **)&gdiInterop))) {
                        if (SUCCEEDED(ID2D1GdiInteropRenderTarget_GetDC(
                                gdiInterop, D2D1_DC_INITIALIZE_MODE_COPY, &dc))) {
                            SetMapMode(dc, MM_TEXT);
                            SetTextColor(dc, color);
                            SetBkMode(dc, TRANSPARENT);
                            old_font = SelectObject(dc, (HFONT)font);
                            DrawTextW(dc, ws, (int)len, &rc,
                                      DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
                            if (old_font)
                                SelectObject(dc, old_font);
                            ID2D1GdiInteropRenderTarget_ReleaseDC(gdiInterop, NULL);
                        }
                        ID2D1GdiInteropRenderTarget_Release(gdiInterop);
                    }
                    IDWriteTextLayout_Release(layout);
                }
                IDWriteTextFormat_Release(fmt);
            }
        }
        goto done_free_ws;
    }

    /* --- Fallback: pure GDI interop (no DirectWrite) --- */
    {
        ID2D1GdiInteropRenderTarget *gdiInterop = NULL;
        HDC dc = NULL;
        HGDIOBJ old = NULL;
        RECT rc;

        if (SUCCEEDED(ID2D1DCRenderTarget_QueryInterface(d->renderTarget,
                        &IID_ID2D1GdiInteropRenderTarget, (void **)&gdiInterop))) {
            if (SUCCEEDED(ID2D1GdiInteropRenderTarget_GetDC(gdiInterop,
                            D2D1_DC_INITIALIZE_MODE_COPY, &dc))) {
                if (font)
                    old = SelectObject(dc, (HFONT)font);
                SetMapMode(dc, MM_TEXT);
                SetTextColor(dc, color);
                SetBkMode(dc, TRANSPARENT);

                rc.left = x;
                rc.top = y;
                rc.right = x + w;
                rc.bottom = y + h;
                DrawTextW(dc, ws, lstrlenW(ws), &rc,
                          DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);

                if (font)
                    SelectObject(dc, old);
                ID2D1GdiInteropRenderTarget_ReleaseDC(gdiInterop, NULL);
            }
            ID2D1GdiInteropRenderTarget_Release(gdiInterop);
        }
    }

done_free_ws:
    free(ws);
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
    SetStretchBltMode(d->memdc, HALFTONE);
    SetBrushOrgEx(d->memdc, 0, 0, NULL);
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    AlphaBlend(d->memdc, x, y, w, h, mem, 0, 0, src_w, src_h, bf);
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
    d->base.vtbl = &g_d2d_vtbl;
    return &d->base;
}

#endif /* __TINYC__ */
