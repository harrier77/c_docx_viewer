/*
 * document.c - geometria di paint del documento.
 *
 * Il codice e' lo stesso che stava in WM_PAINT: stessi colori, stessi
 * rettangoli, stesso ordine di disegno.  L'unica differenza e' che ora passa
 * dal renderer astratto invece che da variabili GDI globali.
 */
#include "document.h"

#include "litedocx_c.h"

/* Colori storici del viewer (non ancora configurabili). */
#define DOC_DESK_COLOR    RGB(240, 240, 242)
#define DOC_SHADOW_COLOR  RGB(190, 190, 195)
#define DOC_PAGE_COLOR    RGB(255, 255, 255)
#define DOC_BORDER_COLOR  RGB(210, 210, 215)

int document_height(void *doc)
{
    return doc ? ld_height(doc) : 0;
}

int document_content_width(void)
{
    return layout_content_width();
}

static void document_draw_blank_page(Renderer *r, int doc_h)
{
    RECT rc_page, rc_shd;

    layout_page_rect(doc_h, &rc_page, &rc_shd);
    (void)rc_shd; /* l'ombra non viene disegnata durante un render */

    r->vtbl->FillRect(r,
                      rc_page.left, rc_page.top,
                      rc_page.right - rc_page.left,
                      rc_page.bottom - rc_page.top,
                      DOC_PAGE_COLOR);
    r->vtbl->FrameRect(r,
                       rc_page.left, rc_page.top,
                       rc_page.right - rc_page.left,
                       rc_page.bottom - rc_page.top,
                       DOC_BORDER_COLOR);
}

void document_draw(void *doc, Renderer *r, int doc_h, int render_busy, float clip_scale)
{
    RECT rc_page, rc_shd, rc_content, rc_clip;

    if (!r || !doc)
        return;

    if (render_busy) {
        /* Un render e' in corso: il documento viene modificato dal worker,
         * quindi non si chiama ld_draw.  Si mostra la pagina bianca. */
        document_draw_blank_page(r, doc_h);
        return;
    }

    layout_paint_rects(doc_h, &rc_page, &rc_shd, &rc_content, &rc_clip);

    /* D2D: il renderer scala il disegno di zoom, quindi l'area logica che
     * finisce a schermo e' piu' grande quando si zooma out (clip_scale > 1).
     * Con clip_scale == 1.0f (GDI) il rettangolo resta identico. */
    if (clip_scale > 1.0f) {
        LONG w = (LONG)((rc_clip.right - rc_clip.left) * clip_scale);
        LONG h = (LONG)((rc_clip.bottom - rc_clip.top) * clip_scale);
        rc_clip.right = rc_clip.left + w;
        rc_clip.bottom = rc_clip.top + h;
        if (rc_clip.right > rc_content.right)
            rc_clip.right = rc_content.right;
        if (rc_clip.bottom > rc_content.bottom)
            rc_clip.bottom = rc_content.bottom;
    }

    r->vtbl->FillRect(r,
                      rc_shd.left, rc_shd.top,
                      rc_shd.right - rc_shd.left,
                      rc_shd.bottom - rc_shd.top,
                      DOC_SHADOW_COLOR);
    r->vtbl->FillRect(r,
                      rc_page.left, rc_page.top,
                      rc_page.right - rc_page.left,
                      rc_page.bottom - rc_page.top,
                      DOC_PAGE_COLOR);
    r->vtbl->FrameRect(r,
                       rc_page.left, rc_page.top,
                       rc_page.right - rc_page.left,
                       rc_page.bottom - rc_page.top,
                       DOC_BORDER_COLOR);

    ld_draw(doc, r->vtbl->GetNativeTarget(r),
            rc_content.left, rc_content.top,
            rc_clip.left, rc_clip.top,
            rc_clip.right - rc_clip.left,
            rc_clip.bottom - rc_clip.top);
}
