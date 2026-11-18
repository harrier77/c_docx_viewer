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

    fprintf(stderr, "[PAINT] zoom=%.2f g_render_busy=%d g_doc=%p scroll=%d\n",
            g_zoom, g_render_busy, g_doc, LAY->scroll);

    if (!g_renderer ||
        !g_renderer->vtbl->BeginDraw(g_renderer, hwnd, LAY->cw, LAY->ch)) {
        RECT rc_all = {0, 0, LAY->cw, LAY->ch};
        FillRect(hdc, &rc_all, (HBRUSH)(COLOR_WINDOW + 1));
        EndPaint(hwnd, &ps);
        return;
    }

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
        g_renderer->vtbl->SetTransform(g_renderer,
                                       g_zoom, 0.0f,
                                       0.0f, g_zoom,
                                       0.0f, 0.0f);
    }

    /* Sfondo scrivania */
    g_renderer->vtbl->FillRect(g_renderer, 0, 0, LAY->cw, LAY->ch,
                               RGB(240, 240, 242));

    /* Fase 3: la geometria di paint vive in func_gdi/document.c.
     * Se un render e' in corso il documento viene modificato dal worker:
     * document_draw non chiama ld_draw e mostra la pagina bianca.
     * Lo stesso vale mentre il worker legge il DOM per un hit-test o lo
     * modifica per un apply: nessun ld_draw concorrente.
     * clip_scale = 1/zoom: il renderer scala il disegno, quindi l'area
     * logica che copre il viewport e' piu' grande quando zoom < 1. */
    {
        int dom_busy = g_render_busy ||
                       g_doc_state == DOC_STATE_HITTEST ||
                       g_doc_state == DOC_STATE_APPLYING;
        document_draw(g_doc, g_renderer, g_doc_h, dom_busy,
                      g_zoom > 0.0f ? 1.0f / g_zoom : 1.0f);

        /* Caret inline (Fase 2, prova di posizionamento): barretta in
         * coordinate DOCUMENTO.  La trasformata di scala del renderer e'
         * ancora attiva e ld_draw usa rc_content come origine logica
         * (screen = (rc_content + doc) * g_zoom, inversa di client_to_doc),
         * quindi basta sommare il contenuto: la barretta resta ancorata al
         * punto del documento anche con scroll/pan/zoom. */
        if (!dom_busy && g_caret_valid && g_doc && g_caret_h > 0)
        {
            RECT rc_content;
            layout_content_rect(g_doc_h, &rc_content);
            g_renderer->vtbl->FillRect(g_renderer,
                                       rc_content.left + g_caret_x,
                                       rc_content.top + g_caret_y,
                                       g_caret_w > 0 ? g_caret_w : 1,
                                       g_caret_h,
                                       RGB(0, 0, 0));
            fprintf(stderr, "[CARET] paint: doc(%d,%d) w=%d h=%d\n", g_caret_x, g_caret_y, g_caret_w, g_caret_h);
        }
    }

    g_renderer->vtbl->Present(g_renderer, hdc, LAY->cw, LAY->ch);
    /* Riporta il renderer a scala 1.0 (la trasformata viene reimpostata
     * a ogni WM_PAINT prima dei draw). */
    g_renderer->vtbl->ResetTransform(g_renderer);
    EndPaint(hwnd, &ps);
}
