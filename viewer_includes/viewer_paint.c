/*
 * viewer_paint.c - disegno del frame (WM_PAINT).
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

static void viewer_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    DLOG("[PAINT] zoom=%.2f g_render_busy=%d g_doc=%p scroll=%d cw=%d ch=%d\n",
            g_zoom, g_render_busy, g_doc, LAY->scroll, LAY->cw, LAY->ch);

    if (!g_renderer) {
        DLOG("[PAINT] ERROR: g_renderer is NULL!\n");
        RECT rc_all = {0, 0, LAY->cw, LAY->ch};
        FillRect(hdc, &rc_all, (HBRUSH)(COLOR_WINDOW + 1));
        EndPaint(hwnd, &ps);
        return;
    }
    DLOG("[PAINT] g_renderer OK, calling BeginDraw...\n");
    if (!g_renderer->vtbl->BeginDraw(g_renderer, hwnd, LAY->cw, LAY->ch)) {
        DLOG("[PAINT] ERROR: BeginDraw failed!\n");
        RECT rc_all = {0, 0, LAY->cw, LAY->ch};
        FillRect(hdc, &rc_all, (HBRUSH)(COLOR_WINDOW + 1));
        EndPaint(hwnd, &ps);
        return;
    }
    DLOG("[PAINT] BeginDraw OK\n");

    /* D2D: zoom == sola trasformazione di scala applicata dal renderer.
     * Stesso meccanismo dell'esempio di riferimento funzionante
     * (esempio_zoom_d2d.c), SENZA lo spostamento (dx/dy) del root del
     * blocco di testo: matrice puramente scalare, ancorata all'origine.
     *   - il layout NON viene toccato (LAY->zoom resta 1.0): niente
     *     re-render, niente riposizionamento del blocco, niente
     *     duplicazione del testo;
     *   - il renderer scala coordinate e font al momento del disegno
     *     (d_zoom, vedi renderer_d2d.c: FillRect/FrameRect/DrawText/...).
     * ATTENZIONE: questo ramo NON deve uscire da WM_PAINT: deve
     * proseguire con document_draw -> Present (EndDraw + BitBlt) ->
     * EndPaint, altrimenti la pagina resta vuota e si genera un loop
     * di repaint (EndPaint mai chiamato). */
    {
        /* Riallinea il pan orizzontale ai limiti della nuova scala:
         * la larghezza del viewport in px di layout dipende dallo zoom. */
        sync_view_scale();
        layout_clamp_scroll_x();
        DLOG("[PAINT] SetTransform zoom=%.2f\n", g_zoom);
        g_renderer->vtbl->SetTransform(g_renderer,
                                       g_zoom, 0.0f,
                                       0.0f, g_zoom,
                                       0.0f, 0.0f);
    }

    /* Sfondo scrivania */
    DLOG("[PAINT] FillRect desktop bg\n");
    g_renderer->vtbl->FillRect(g_renderer, 0, 0, LAY->cw, LAY->ch,
                               RGB(240, 240, 242));

    /* Fase 3: la geometria di paint vive in func_gdi/document.c.
     * Se un render e' in corso il documento viene modificato dal worker:
     * document_draw non chiama ld_draw e mostra la pagina bianca.
     * clip_scale = 1/zoom: il renderer scala il disegno, quindi l'area
     * logica che copre il viewport e' piu' grande quando zoom < 1. */
    DLOG("[PAINT] calling document_draw g_doc=%p g_doc_h=%d g_render_busy=%d clip_scale=%.2f\n",
            g_doc, g_doc_h, g_render_busy, g_zoom > 0.0f ? 1.0f / g_zoom : 1.0f);
    /* Durante un apply (editor o caret) il worker sta mutando il DOM:
     * chiamare ld_draw ora leggerebbe un albero a meta' (garbage/crash).
     * Si mostra la pagina bianca finche' il render successivo non e' pronto. */
    document_draw(g_doc, g_renderer, g_doc_h,
                  g_render_busy || g_doc_state == DOC_STATE_APPLYING,
                  g_zoom > 0.0f ? 1.0f / g_zoom : 1.0f);
    DLOG("[PAINT] document_draw returned\n");

    /* Overlay del caret: disegnato con lo stesso renderer D2D della pagina,
     * prima di Present, cosi' l'anteprima del run modificato usa la stessa
     * baseline DirectWrite del documento (il vecchio overlay GDI la
     * spostava di pochi pixel verso l'alto). */
    caret_draw(g_renderer);

    DLOG("[PAINT] calling Present...\n");
    g_renderer->vtbl->Present(g_renderer, hdc, LAY->cw, LAY->ch);
    DLOG("[PAINT] Present returned\n");
    /* Riporta il renderer a scala 1.0 (la trasformata viene reimpostata
     * a ogni WM_PAINT prima dei draw). */
    g_renderer->vtbl->ResetTransform(g_renderer);
    EndPaint(hwnd, &ps);
}