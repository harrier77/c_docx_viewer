/*
 * renderer.h - interfaccia astratta di rendering (C puro, senza Direct2D).
 *
 * Operazioni in coordinate intere: la resa GDI attuale deve restare
 * pixel-identica. Il renderer possiede il proprio back buffer; Present()
 * lo copia sul DC della finestra.
 */
#ifndef RENDERER_H
#define RENDERER_H

#include <windows.h>

/* Direttive forward per DirectWrite — il renderer le usa solo internamente */
#ifndef __TINYC__
typedef struct IDWriteFactory    IDWriteFactory;
typedef struct IDWriteTextFormat IDWriteTextFormat;
#endif

typedef struct Renderer Renderer;

typedef struct RendererVtbl
{
    int  (*BeginDraw)(Renderer *r, HWND hwnd, int w, int h);
    void (*EndDraw)(Renderer *r);
    void (*Present)(Renderer *r, HDC target, int w, int h);

    void (*FillRect)(Renderer *r, int x, int y, int w, int h, COLORREF color);
    void (*FrameRect)(Renderer *r, int x, int y, int w, int h, COLORREF color);

    /* Primitive per i marcatori degli elenchi (puntini/quadrati). */
    void (*DrawEllipse)(Renderer *r, int x, int y, int w, int h, COLORREF color);
    void (*DrawRect)(Renderer *r, int x, int y, int w, int h, COLORREF color);

    /* font: handle opaco restituito da cb_create_font (oggi HFONT). */
    void (*DrawText)(Renderer *r, const char *utf8, void *font, COLORREF color,
                     int x, int y, int w, int h);

    /* Immagine nativa del renderer (GDI: HBITMAP), scalata nel rettangolo.
     * native_bitmap e' opaco: solo l'implementazione del renderer lo interpreta. */
    void (*DrawImage)(Renderer *r, void *native_bitmap, int src_w, int src_h,
                      int x, int y, int w, int h);

    void (*DrawBorders)(Renderer *r, int x, int y, int w, int h,
                        int left_w, int top_w, int right_w, int bottom_w,
                        COLORREF left_c, COLORREF top_c,
                        COLORREF right_c, COLORREF bottom_c);

    /* Target nativo per ld_draw (GDI: HDC del back buffer). */
    void *(*GetNativeTarget)(Renderer *r);

    /* Transform: imposta la matrice di trasformazione sul render target D2D.
     * (m11, m12, m21, m22, dx, dy) — formato row-major come D2D1_MATRIX_3X2_F.
     * Le chiamate successive si sovrappongono (non sono cumulative).
     * Chiama ResetTransform prima del prossimo frame se serve tornare all'identità.
     * Funzione opzionale: NULL = comportamento pre-definito (GDI: no-op). */
    void (*SetTransform)(Renderer *r, float m11, float m12, float m21, float m22, float dx, float dy);
    void (*ResetTransform)(Renderer *r);

    void (*Destroy)(Renderer *r);
} RendererVtbl;

struct Renderer
{
    const RendererVtbl *vtbl;
};

Renderer *renderer_gdi_create(void);

/* Per D2D: factory opzionale (NULL = GDI interop per il testo, fallback).
   Il font-resolver converte un HFONT (restaurato da cb_create_font) in
   informazioni che DirectWrite puo' usare per creare IDWriteTextFormat.
   Ritorna 0 in caso di successo, !=0 altrimenti.
   Se il resolver e' NULL, il renderer non attivava DirectWrite per il testo.
*/
typedef int (*D2dFontResolver)(void *ctx, void *hfont, wchar_t *family, int *size_px,
                               int *bold, int *italic, int *underline);
#ifndef __TINYC__
Renderer *renderer_d2d_create(IDWriteFactory *dw_factory,
                              D2dFontResolver resolver, void *resolver_ctx);
#else
/* TCC non ha header D2D/DWrite — stub che restituisce NULL (fallback GDI). */
Renderer *renderer_d2d_create(void);
#endif

#endif /* RENDERER_H */
