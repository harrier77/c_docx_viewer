/*
 * viewer_input.c - eventi finestra: resize, pan, scroll, zoom, doppio clic.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

/* Forward declarations for editor functions */
extern void begin_paragraph_edit_at_index(int index);
extern void show_run_viewer(int para_index, const ld_run_hit *hit, const char *run_text);

static void viewer_on_size(HWND hwnd, WPARAM wp, LPARAM lp)
{
    (void)wp;
    // log_msg("[SIZE] WM_SIZE cw=%d ch=%d\n", LOWORD(lp), HIWORD(lp));
    layout_resize(LOWORD(lp), HIWORD(lp));
    sync_view_scale();
    /* NON chiamare BeginDraw qui: in WM_SIZE non si disegna nulla.
     * Il back buffer viene (ri)allocato da BeginDraw in WM_PAINT, che e'
     * l'unico punto in cui viene bilanciato da Present/EndPaint.
     * Per il renderer D2D una BeginDraw orfana lascia il render target
     * con in_draw=1 e senza EndDraw (stato sporco). */
    if (g_doc && !g_load_active) {
        /* Update justification width on page size change */
        int justification_w = LAY->page_w - 2 * LAYOUT_VISUAL_PADDING_X;
        if (justification_w > 0)
            ld_set_justification_width(g_doc, justification_w);
        int content_w = layout_content_width();
        /* Il layout viene rifatto dal worker: la UI non si blocca.
         * Durante il render il paint mostra solo lo sfondo. */
        /* Soglia 2px per evitare render spuri su resize minimi (es. WM_SIZE iniziale) */
        if (abs(content_w - g_doc_content_w) > 2)
            request_render(content_w);
    }
    layout_update_scrollbars(hwnd, g_doc_h);
    InvalidateRect(hwnd, NULL, FALSE);
}

static LRESULT viewer_on_setcursor(HWND hwnd, WPARAM wp, LPARAM lp)
{
    /* Cursore "sposta" durante il pan col tasto centrale. */
    if (LOWORD(lp) == HTCLIENT && g_pan_active) {
        SetCursor(LoadCursor(NULL, IDC_SIZEALL));
        return TRUE;
    }
    return DefWindowProcW(hwnd, WM_SETCURSOR, wp, lp);
}

static LRESULT viewer_on_mbuttondown(HWND hwnd, LPARAM lp)
{
    if (g_doc) {
        g_pan_active = 1;
        g_pan_x = (short)LOWORD(lp);
        g_pan_y = (short)HIWORD(lp);
        SetCapture(hwnd);
        SetCursor(LoadCursor(NULL, IDC_SIZEALL));
    }
    return 0;
}

static LRESULT viewer_on_mousemove(HWND hwnd, WPARAM wp, LPARAM lp)
{
    if (g_pan_active) {
        int x = (short)LOWORD(lp);
        int y = (short)HIWORD(lp);

        sync_view_scale();
        layout_pan(x - g_pan_x, y - g_pan_y, g_doc_h);
        g_pan_x = x;
        g_pan_y = y;
        SetScrollPos(hwnd, SB_VERT, LAY->scroll, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, WM_MOUSEMOVE, wp, lp);
}

static LRESULT viewer_on_mbuttonup(HWND hwnd)
{
    (void)hwnd;
    if (g_pan_active) {
        g_pan_active = 0;
        ReleaseCapture();
    }
    return 0;
}

static void viewer_on_capturechanged(void)
{
    g_pan_active = 0;
}

static void viewer_on_vscroll(HWND hwnd, WPARAM wp)
{
    SCROLLINFO si;
    int old_s = LAY->scroll;
    int doc_h = g_doc_h;
    int max_s = (doc_h + 120) - LAY->ch;
    if (max_s < 0) max_s = 0;

    switch (LOWORD(wp)) {
    case SB_LINEUP:   LAY->scroll -= 30; break;
    case SB_LINEDOWN: LAY->scroll += 30; break;
    case SB_PAGEUP:   LAY->scroll -= LAY->ch / 2; break;
    case SB_PAGEDOWN: LAY->scroll += LAY->ch / 2; break;
    case SB_THUMBTRACK:
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_TRACKPOS;
        GetScrollInfo(hwnd, SB_VERT, &si);
        LAY->scroll = si.nTrackPos;
        break;
    }
    if (LAY->scroll < 0) LAY->scroll = 0;
    if (LAY->scroll > max_s) LAY->scroll = max_s;
    if (LAY->scroll != old_s) {
        SetScrollPos(hwnd, SB_VERT, LAY->scroll, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        LOG_SCROLL("WM_VSCROLL scroll changed");
        log_msg("[SCROLL] WM_VSCROLL: scroll=%d g_layout_complete=%d g_last_rendered_page=%d\n",
                LAY->scroll, g_layout_complete, g_last_rendered_page);
        /* FASE 5/6: Trigger on-demand page rendering SOLO se layout non completo */
        if (g_doc && g_page_h_est > 0 && !g_layout_complete) {
            int visible_first = LAY->scroll / g_page_h_est;
            int visible_last = (LAY->scroll + LAY->ch) / g_page_h_est + 1; /* +1 lookahead */
            int page_count = ld_get_page_count(g_doc);
            if (visible_last >= page_count) visible_last = page_count - 1;
            if (visible_first < 0) visible_first = 0;
            if (visible_last > g_last_rendered_page) {
                request_render_range(g_last_rendered_page + 1, visible_last);
            }
        }
    }
}

static void viewer_on_mousewheel(HWND hwnd, WPARAM wp, LPARAM lp)
{
    short cx_ = (short)LOWORD(lp);
    short cy_ = (short)HIWORD(lp);
    int delta = GET_WHEEL_DELTA_WPARAM(wp);

    if (wp & MK_CONTROL) {
        /* Zoom puro del renderer (sola scala, nessun pan, nessun
         * re-layout). Il layout e i font restano a scala logica: il
         * fattore viene applicato in fase di disegno da renderer_d2d.c
         * (d_zoom), quindi basta ridisegnare. */
        float old_zoom = g_zoom;
        float new_zoom;
        const float step = 0.1f;
        if (delta > 0)
            new_zoom = (old_zoom + step < G_ZOOM_MAX)
                           ? old_zoom + step : G_ZOOM_MAX;
        else
            new_zoom = (old_zoom - step > G_ZOOM_MIN)
                           ? old_zoom - step : G_ZOOM_MIN;

        fprintf(stderr, "[ZOOM] delta=%d old=%.2f new=%.2f cursor=(%d,%d)\n",
                delta, old_zoom, new_zoom, cx_, cy_);
        g_zoom = new_zoom;
        /* La scala del renderer e' ancorata all'origine: senza questa
         * compensazione la pagina (centrata) trasla verso destra ad
         * ogni zoom. layout_zoom_view sposta solo scroll/scroll_x
         * (coordinate di layout): nessun re-render. */
        sync_view_scale();
        layout_zoom_view((double)old_zoom, (double)new_zoom, g_doc_h);
        SetScrollPos(hwnd, SB_VERT, LAY->scroll, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    /* Shift+rotella: pan orizzontale (stessa autorita' del pan: scroll_x).
     * Non tocca il layout del documento: nessun re-render. */
    if (wp & MK_SHIFT) {
        sync_view_scale();
        layout_pan(delta / 3, 0, g_doc_h);
        SetScrollPos(hwnd, SB_VERT, LAY->scroll, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    /* Scroll verticale normale */
    int doc_h = g_doc_h;
    int max_s = (doc_h + 120) - LAY->ch;
    if (max_s < 0) max_s = 0;
    LAY->scroll -= delta / 3;
    if (LAY->scroll < 0) LAY->scroll = 0;
    if (LAY->scroll > max_s) LAY->scroll = max_s;
    SetScrollPos(hwnd, SB_VERT, LAY->scroll, TRUE);
    InvalidateRect(hwnd, NULL, FALSE);
    LOG_SCROLL("WM_MOUSEWHEEL scroll changed");
    log_msg("[SCROLL] WM_MOUSEWHEEL: scroll=%d g_layout_complete=%d g_last_rendered_page=%d\n",
            LAY->scroll, g_layout_complete, g_last_rendered_page);
    /* FASE 5/6: Trigger on-demand page rendering SOLO se layout non completo */
    if (g_doc && g_page_h_est > 0 && !g_layout_complete) {
        int visible_first = LAY->scroll / g_page_h_est;
        int visible_last = (LAY->scroll + LAY->ch) / g_page_h_est + 1; /* +1 lookahead */
        int page_count = ld_get_page_count(g_doc);
        if (visible_last >= page_count) visible_last = page_count - 1;
        if (visible_first < 0) visible_first = 0;
        if (visible_last > g_last_rendered_page) {
            request_render_range(g_last_rendered_page + 1, visible_last);
        }
    }
}