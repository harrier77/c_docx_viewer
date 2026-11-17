/*
 * viewer_input.c - eventi finestra: resize, pan, scroll, zoom, doppio clic.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

static void viewer_on_size(HWND hwnd, WPARAM wp, LPARAM lp)
{
    (void)wp;
    // log_msg("[SIZE] WM_SIZE cw=%d ch=%d\n", LOWORD(lp), HIWORD(lp));
    /* Resize toolbar */
    if (g_toolbar) {
        SendMessage(g_toolbar, TB_AUTOSIZE, 0, 0);
        MoveWindow(g_toolbar, 0, 0, LOWORD(lp), TOOLBAR_HEIGHT, TRUE);
    }
    layout_resize(LOWORD(lp), HIWORD(lp));
    sync_view_scale();
    /* Ogni ridimensionamento invalida gli hit-test in volo. */
    if (g_doc)
        g_layout_epoch++;
    /* NON chiamare BeginDraw qui: in WM_SIZE non si disegna nulla.
     * Il back buffer viene (ri)allocato da BeginDraw in WM_PAINT, che e'
     * l'unico punto in cui viene bilanciato da Present/EndPaint.
     * Per il renderer D2D una BeginDraw orfana lascia il render target
     * con in_draw=1 e senza EndDraw (stato sporco). */
    if (g_doc && !g_load_active) {
        /* Update justification width on page size change. Non toccare il
         * documento se il worker lo sta leggendo/modificando: si rimanda
         * all'uscita da HITTEST/EDITOR/APPLYING. */
        if (g_doc_state == DOC_STATE_HITTEST ||
            g_doc_state == DOC_STATE_EDITOR ||
            g_doc_state == DOC_STATE_APPLYING)
        {
            g_justification_dirty = 1;
        }
        else
        {
            int justification_w = LAY->page_w - 2 * LAYOUT_VISUAL_PADDING_X;
            if (justification_w > 0)
                ld_set_justification_width(g_doc, justification_w);
            g_justification_dirty = 0;
        }
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

/* ---------------- Tooltip nativo sul click destro ----------------
 * Il tooltip (finestra TOOLTIPS creata in viewer_on_create) e' di tipo
 * "tracking": si attiva manualmente con TTM_TRACKACTIVATE e resta
 * visibile finche' non viene esplicitamente nascosto. Il testo mostra
 * le coordinate client del mouse al momento del click destro. */

static void viewer_hide_tooltip(const char *why)
{
    if (g_tip_wnd && g_tip_shown) {
        TOOLINFOW ti;
        ZeroMemory(&ti, sizeof(ti));
        ti.cbSize = TTTOOLINFOW_V1_SIZE;
        ti.hwnd = g_hwnd;
        ti.uId = 1;
        SendMessageW(g_tip_wnd, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
        g_tip_shown = 0;
        log_msg("[TIP] hide (%s)\n", why ? why : "?");
    }
}

/* Mostra/aggiorna il tooltip "tracking" al punto client dato. Il tool
 * viene ricreato a ogni aggiornamento: e' l'unico modo affidabile (anche
 * su comctl32 v5, dove TTM_SETTOOLINFOW non aggiorna il testo) per
 * impostare il testo corrente. */
static void viewer_show_tooltip_at(int client_x, int client_y,
                                   const wchar_t *text)
{
    POINT pt;
    TOOLINFOW ti;

    if (!g_tip_wnd)
        return;

    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = TTTOOLINFOW_V1_SIZE;
    ti.hwnd = g_hwnd;
    ti.uId = 1;
    /* TTF_TRANSPARENT: i messaggi mouse attraversano il tooltip. */
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_TRANSPARENT;
    ti.lpszText = (wchar_t *)text;
    /* Ordine sicuro: disattiva, elimina, ricrea, riposiziona, attiva. */
    SendMessageW(g_tip_wnd, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
    {
        LRESULT delres = SendMessageW(g_tip_wnd, TTM_DELTOOLW, 0, (LPARAM)&ti);
        LRESULT addres = SendMessageW(g_tip_wnd, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        LRESULT actres;
        pt.x = client_x;
        pt.y = client_y;
        ClientToScreen(g_hwnd, &pt);
        g_tip_screen_x = pt.x + 16;
        g_tip_screen_y = pt.y + 20;
        SendMessageW(g_tip_wnd, TTM_TRACKPOSITION, 0,
                     (LPARAM)MAKELONG(g_tip_screen_x, g_tip_screen_y));
        actres = SendMessageW(g_tip_wnd, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
        g_tip_shown = 1;
        log_msg("[TIP] show %d,%d del=%ld add=%ld act=%ld len=%d\n",
                client_x, client_y, (long)delres, (long)addres, (long)actres,
                lstrlenW(text));
    }
}

static LRESULT viewer_on_rbuttonup(HWND hwnd, LPARAM lp)
{
    wchar_t text[160];
    int cx = (short)LOWORD(lp);
    int cy = (short)HIWORD(lp);
    int doc_x = 0, doc_y = 0;

    (void)hwnd; /* la posizione usa lo stato globale g_hwnd */

    /* Coordinate documento: la stessa conversione usata dal doppio clic
     * per l'editor (viewer_paragraph.c: client_to_doc). */
    client_to_doc(cx, cy, &doc_x, &doc_y);
    wsprintfW(text, L"%d, %d\ndoc: %d, %d", cx, cy, doc_x, doc_y);
    log_msg("[TIP-RUP] hwnd=%p g_tip_wnd=%p pos=%d,%d\n",
            (void *)hwnd, (void *)g_tip_wnd, cx, cy);

    if (!g_tip_wnd)
        return 0;
    viewer_show_tooltip_at(cx, cy, text);

    /* Ancora per la soglia di movimento (viewer_on_mousemove). */
    g_tip_anchor_x = cx;
    g_tip_anchor_y = cy;

    /* Diagnostica run (ld_run_hit_test, sola lettura): job asincrono
     * sul worker, solo a layout fermo (paragrafi e geometria sono
     * validi solo dopo un render completato, come per l'editor). */
    if (g_doc && !g_load_active && !g_render_busy && g_layout_complete &&
        g_doc_state == DOC_STATE_IDLE && g_worker_started)
    {
        Job *j = alloc_job(JOB_RUN_HIT_TEST);
        if (j)
        {
            j->doc = g_doc;
            j->gen = g_load_gen;
            j->epoch = g_layout_epoch;
            j->doc_x = doc_x;
            j->doc_y = doc_y;
            g_doc_state = DOC_STATE_HITTEST;
            enqueue_job(j);
        }
    }
    return 0;
}

/* Doppio click destro: apre l'editor del paragrafo (testo/OOXML).  Il
 * primo click del doppio ha gia' accodato JOB_RUN_HIT_TEST (per il
 * tooltip): se l'esito non e' ancora arrivato si marca l'attesa e
 * l'editor viene aperto dall'esito (handle_run_hit_result), altrimenti
 * si avvia subito l'hit-test del paragrafo. */
static void viewer_on_rbuttondblclk(HWND hwnd, LPARAM lp)
{
    int cx = (short)LOWORD(lp);
    int cy = (short)HIWORD(lp);
    int doc_x = 0, doc_y = 0;

    (void)hwnd;
    viewer_hide_tooltip("rdblclk");

    if (!g_doc || g_load_active || g_render_busy || !g_layout_complete)
        return;

    client_to_doc(cx, cy, &doc_x, &doc_y);

    if (g_doc_state == DOC_STATE_HITTEST)
    {
        /* Esito in volo (job del primo click): l'editor si aprira' dopo. */
        g_para_edit_pending = 1;
        return;
    }
    if (g_doc_state != DOC_STATE_IDLE || !g_worker_started || g_edit_hwnd)
        return;

    begin_paragraph_edit_at(doc_x, doc_y);
}

/* Esito asincrono del run hit-test: aggiorna il testo del tooltip con
 * i valori visti dalla DLL (run, offset, box) se ancora visibile.
 * Lo stato della macchina a stati viene comunque rilasciato. */
static void handle_run_hit_result(RunHitResult *r)
{
    wchar_t text[256];

    if (!r)
    {
        g_para_edit_pending = 0;
        if (g_doc_state == DOC_STATE_HITTEST)
            g_doc_state = DOC_STATE_IDLE;
        resume_pending_render();
        return;
    }

    /* Risultato scaduto (documento/layout cambiati nel frattempo). */
    if (r->doc != g_doc || r->gen != g_load_gen || r->epoch != g_layout_epoch)
    {
        g_para_edit_pending = 0;
        if (g_doc_state == DOC_STATE_HITTEST)
            g_doc_state = DOC_STATE_IDLE;
        resume_pending_render();
        free(r);
        return;
    }
    if (g_doc_state == DOC_STATE_HITTEST)
        g_doc_state = DOC_STATE_IDLE;
    resume_pending_render();

    log_msg("[TIP-RUN] hit=%d para=%d run=%d off=%d [%d..%d) inRun=%d box=%d,%d %dx%d (doc %d,%d)\n",
            r->hit, r->info.para_index, r->info.run_index, r->info.char_offset,
            r->info.run_char_start, r->info.run_char_end, r->info.run_char_offset,
            r->info.x, r->info.y, r->info.w, r->info.h, r->doc_x, r->doc_y);

    /* Doppio click sinistro: apre l'editor del run cliccato invece di
     * aggiornare il tooltip. */
    if (g_run_view_pending)
    {
        g_run_view_pending = 0;
        if (r->hit && r->info.para_index >= 0 &&
            r->info.run_char_start >= 0 && r->info.run_char_end >= r->info.run_char_start &&
            !g_edit_hwnd)
        {
            char *para = ld_paragraph_get_text(g_doc, r->info.para_index);
            if (para)
            {
                int plen = (int)strlen(para);
                int s = r->info.run_char_start;
                int e = r->info.run_char_end;
                if (s > plen) s = plen;
                if (e > plen) e = plen;
                if (e > s)
                {
                    int n = e - s;
                    char *run_text = (char *)malloc((size_t)n + 1);
                    if (run_text)
                    {
                        memcpy(run_text, para + s, (size_t)n);
                        run_text[n] = '\0';
                        g_doc_state = DOC_STATE_EDITOR;
                        show_run_viewer(r->info.para_index, &r->info, run_text);
                        free(run_text);
                    }
                }
                free(para);
            }
        }
        free(r);
        return;
    }

    /* Doppio click destro: il primo click del doppio ha accodato il job
     * per il tooltip; arrivato l'esito si apre l'editor del paragrafo
     * (testo/OOXML) nello stesso punto. */
    if (g_para_edit_pending)
    {
        g_para_edit_pending = 0;
        begin_paragraph_edit_at(r->doc_x, r->doc_y);
        free(r);
        return;
    }

    if (!g_tip_shown || !g_tip_wnd)
    {
        free(r);
        return;
    }

    if (r->hit)
    {
        wsprintfW(text,
                  L"%d, %d\ndoc: %d, %d\nrun %d: off %d [%d..%d) inRun %d\nbox %d, %d %dx%d",
                  g_tip_anchor_x, g_tip_anchor_y,
                  r->doc_x, r->doc_y,
                  r->info.run_index,
                  r->info.char_offset,
                  r->info.run_char_start, r->info.run_char_end,
                  r->info.run_char_offset,
                  r->info.x, r->info.y, r->info.w, r->info.h);
    }
    else if (r->info.para_index >= 0)
    {
        wsprintfW(text, L"%d, %d\ndoc: %d, %d\nrun: - (para %d, nessuna parola)",
                  g_tip_anchor_x, g_tip_anchor_y, r->doc_x, r->doc_y,
                  r->info.para_index);
    }
    else
    {
        wsprintfW(text, L"%d, %d\ndoc: %d, %d\nrun: -",
                  g_tip_anchor_x, g_tip_anchor_y, r->doc_x, r->doc_y);
    }
    viewer_show_tooltip_at(g_tip_anchor_x, g_tip_anchor_y, text);
    free(r);
}

static LRESULT viewer_on_mbuttondown(HWND hwnd, LPARAM lp)
{
    viewer_hide_tooltip("pan");
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
    /* Il tooltip "trasparente" rigenera WM_MOUSEMOVE sulla finestra
     * sotto di se: senza una soglia il tooltip si nasconderebbe da
     * solo appena appare. Resta visibile finche' il cursore non si
     * allontana piu' di TIP_HIDE_DIST px dall'ancora del click. */
#define TIP_HIDE_DIST 3
    if (g_tip_shown) {
        int mx = (short)LOWORD(lp);
        int my = (short)HIWORD(lp);
        if (abs(mx - g_tip_anchor_x) > TIP_HIDE_DIST ||
            abs(my - g_tip_anchor_y) > TIP_HIDE_DIST)
        {
            viewer_hide_tooltip("move");
        }
    }

    if (g_pan_active) {
        int x = (short)LOWORD(lp);
        int y = (short)HIWORD(lp);

        sync_view_scale();
        layout_pan(x - g_pan_x, y - g_pan_y, g_doc_h);
        g_pan_x = x;
        g_pan_y = y;
        if (g_doc)
            g_layout_epoch++;
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

/* Doppio click sinistro: apre l'editor del run cliccato.  Il primo click
 * del doppio non accoda nulla (il sinistro singolo chiude solo il
 * tooltip): se c'e' un hit-test in volo (es. da un click destro appena
 * fatto) si marca l'attesa, altrimenti si accoda un nuovo job con la
 * richiesta di apertura. */
static void viewer_on_lbuttondblclk(HWND hwnd, LPARAM lp)
{
    int cx = (short)LOWORD(lp);
    int cy = (short)HIWORD(lp);
    int doc_x = 0, doc_y = 0;

    (void)hwnd;
    viewer_hide_tooltip("ldblclk");

    if (!g_doc || g_load_active || g_render_busy || !g_layout_complete)
        return;

    client_to_doc(cx, cy, &doc_x, &doc_y);

    if (g_doc_state == DOC_STATE_HITTEST)
    {
        /* Esito in volo (job di un click precedente): verra' aperto il
         * run. */
        g_run_view_pending = 1;
        return;
    }
    if (g_doc_state != DOC_STATE_IDLE || !g_worker_started || g_edit_hwnd)
        return;

    {
        Job *j = alloc_job(JOB_RUN_HIT_TEST);
        if (!j)
            return;
        j->doc = g_doc;
        j->gen = g_load_gen;
        j->epoch = g_layout_epoch;
        j->doc_x = doc_x;
        j->doc_y = doc_y;
        g_run_view_pending = 1;
        g_doc_state = DOC_STATE_HITTEST;
        enqueue_job(j);
    }
}

static void viewer_on_capturechanged(void)
{
    g_pan_active = 0;
}

static void viewer_on_vscroll(HWND hwnd, WPARAM wp)
{
    viewer_hide_tooltip("vscroll");
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
        /* La mappatura client->documento e' cambiata: gli hit-test in
         * volo non sono piu' validi. */
        if (g_doc)
            g_layout_epoch++;
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
    viewer_hide_tooltip("wheel");
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
        if (g_doc)
            g_layout_epoch++;
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    /* Shift+rotella: pan orizzontale (stessa autorita' del pan: scroll_x).
     * Non tocca il layout del documento: nessun re-render. */
    if (wp & MK_SHIFT) {
        sync_view_scale();
        layout_pan(delta / 3, 0, g_doc_h);
        SetScrollPos(hwnd, SB_VERT, LAY->scroll, TRUE);
        if (g_doc)
            g_layout_epoch++;
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
    if (g_doc)
        g_layout_epoch++;
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
