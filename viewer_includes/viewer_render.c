/*
 * viewer_render.c - richieste di render, caricamento e stato di paginazione.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

static void request_render(int w)
{
    Job *j;

    if (!g_doc || g_load_active)
        return;

    /* No new render jobs while the DOM is being read (hit-test) or edited.
     * Remember the width and let the state transition flush it later. */
    if (g_doc_state == DOC_STATE_HITTEST ||
        g_doc_state == DOC_STATE_EDITOR ||
        g_doc_state == DOC_STATE_APPLYING)
    {
        g_render_pending_width = w;
        return;
    }

    //fprintf(stderr, "[RENDER_REQ] w=%d g_render_busy=%d zoom=%.2f\n", w, g_render_busy, g_zoom);

    LOG_RENDER("request_render called");
    log_msg("[RENDER] request_render: w=%d g_render_busy=%d\n", w, g_render_busy);

    if (!g_worker_started)
    {
        /* Fallback sincrono */
        g_doc_state = DOC_STATE_RENDERING;
        ld_render(g_doc, w);
        g_doc_content_w = w;
        g_doc_h = document_height(g_doc);
        g_layout_epoch++;
        g_doc_state = DOC_STATE_IDLE;
        layout_update_scrollbars(g_hwnd, g_doc_h);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }

    if (g_render_busy)
    {
        LOG_RENDER("render busy, queuing pending width");
        g_render_pending_width = w;
        return;
    }

    j = alloc_job(JOB_RENDER);
    if (!j)
        return;
    j->doc = g_doc;
    j->content_w = w;
    g_render_busy = 1;
    g_render_doc = g_doc;
    g_render_used_w = w;
    g_render_pending_width = -1;
    g_doc_state = DOC_STATE_RENDERING;
    dbg_log("[TABLE] request_render content_w=%d\n", w);
    enqueue_job(j);
    LOG_RENDER("JOB_RENDER enqueued");
}

/* FASE 5/6: Renderizza un range di pagine (es. pagina 0, o 1-2 per lookahead).
 * Se il range è già coperto, non tocca il worker e non alza g_render_busy:
 * così lo scroll su pagine già layoutate non produce pagina bianca. */
static void request_render_range(int first_page, int last_page)
{
    Job *j;

    if (!g_doc || g_load_active)
        return;

    /* No range jobs while the DOM is being read/edited. */
    if (g_doc_state == DOC_STATE_HITTEST ||
        g_doc_state == DOC_STATE_EDITOR ||
        g_doc_state == DOC_STATE_APPLYING)
    {
        return;
    }

    if (first_page < 0)
        first_page = 0;
    if (last_page < first_page)
        last_page = first_page;

    LOG_RENDER("request_render_range called");
    log_msg("[RENDER] request_render_range: first=%d last=%d g_last_rendered_page=%d g_render_busy=%d g_layout_complete=%d\n",
            first_page, last_page, g_last_rendered_page, g_render_busy, g_layout_complete);

    /* Già layoutato fino a last_page: niente job, niente busy. */
    if (g_last_rendered_page >= 0 && last_page <= g_last_rendered_page)
    {
        LOG_RENDER("range already rendered, skipping");
        return;
    }

    if (!g_worker_started)
    {
        /* Fallback sincrono */
        g_doc_state = DOC_STATE_RENDERING;
        ld_render_range(g_doc, first_page, LAY->ch, layout_content_width());
        {
            int page_count = ld_get_page_count(g_doc);
            if (page_count > 0)
                g_last_rendered_page = page_count - 1;
            else if (last_page > g_last_rendered_page)
                g_last_rendered_page = last_page;
        }
        g_doc_h = document_height(g_doc);
        g_layout_epoch++;
        g_doc_state = DOC_STATE_IDLE;
        layout_update_scrollbars(g_hwnd, g_doc_h);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }

    if (g_render_busy)
    {
        /* Accoda range pending separato da width pending */
        if (g_render_pending_first_page < 0 || last_page > g_render_pending_last_page)
        {
            g_render_pending_first_page = first_page;
            g_render_pending_last_page = last_page;
        }
        LOG_RENDER("render busy, queued pending range");
        return;
    }

    j = alloc_job(JOB_RENDER_RANGE);
    if (!j)
        return;
    j->doc = g_doc;
    j->content_w = layout_content_width();
    j->viewport_h = LAY->ch;
    j->first_page = first_page;
    j->last_page = last_page;
    g_render_busy = 1;
    g_render_doc = g_doc;
    g_render_used_w = last_page; /* reused as last_page for range jobs */
    g_render_pending_first_page = first_page;
    g_render_pending_last_page = last_page;
    g_doc_state = DOC_STATE_RENDERING;
    enqueue_job(j);
    LOG_RENDER("JOB_RENDER_RANGE enqueued");
}

static void apply_loaded_doc(unsigned id, void *doc)
{
    if (g_shutdown)
    {
        if (doc)
            dispose_doc(doc);
        return;
    }

    LOG_RENDER("apply_loaded_doc called");
    log_msg("[RENDER] apply_loaded_doc: id=%u g_load_gen=%u doc=%p\n", id, g_load_gen, doc);

    /* Risultato di una richiesta superata: va distrutto sul worker. */
    if (id != g_load_gen)
    {
        if (doc)
            dispose_doc(doc);
        return;
    }

    /* Il cambio documento non deve mai avvenire mentre un indice di
     * paragrafo e' in uso (editor aperto) o una modifica e' in volo. */
    if (g_doc_state == DOC_STATE_EDITOR || g_doc_state == DOC_STATE_APPLYING)
    {
        g_load_active = 0;
        if (doc)
            dispose_doc(doc);
        return;
    }

    g_load_active = 0;
    g_doc_state = DOC_STATE_IDLE;
    /* Nuovo documento: ogni hit-test in volo appartiene alla generazione
     * precedente e va scartato. */
    g_layout_epoch++;
    g_pending_apply_token = 0;
    g_pending_apply_epoch = 0;
    viewer_caret_clear();

    if (doc)
    {
        if (g_doc)
            dispose_doc(g_doc);
        g_doc = doc;
        g_doc_content_w = g_load_content_w;

        /* Set visual padding so layout box is centered inside visual box */
        ld_set_visual_padding(g_doc, LAYOUT_VISUAL_PADDING_X, LAYOUT_VISUAL_MARGIN_Y);
        /* Set fixed justification width = paintable width (viewport - 2*visual_padding)
         * This keeps justified text stable when LAYOUT_PADDING_X changes. */
        int justification_w = LAY->page_w - 2 * LAYOUT_VISUAL_PADDING_X;
        if (justification_w > 0)
            ld_set_justification_width(g_doc, justification_w);

        lstrcpynA(g_path, g_load_path, sizeof(g_path));
        {
            const char *p = strrchr(g_load_path, '\\');
            if (!p)
                p = strrchr(g_load_path, '/');
            sprintf(g_title, "d2dview - %s", p ? p + 1 : g_load_path);
        }
        SetWindowTextA(g_hwnd, g_title);
        LAY->scroll = 0;
        LAY->scroll_x = 0;
        LAY->pan_rem_x = 0.0;
        LAY->pan_rem_y = 0.0;

        /* Stima altezza documento SENZA chiamare ld_get_page_height/count
         * (possono triggerare full render sul thread UI - vedi D7). */
        g_doc_h = 2000; /* fallback iniziale, sarà aggiornato dopo render */
        g_page_h_est = 1000;
        g_first_rendered_page = -1;
        g_last_rendered_page = -1;
        g_layout_complete = 0;

        /* Lancia UN SOLO job di layout completo in background */
        g_render_pending_width = -1;
        g_render_pending_first_page = -1;
        g_render_pending_last_page = -1;
        request_render(layout_content_width());

        layout_update_scrollbars(g_hwnd, g_doc_h);
        InvalidateRect(g_hwnd, NULL, TRUE);
    }
    else
    {
        sprintf(g_title, "d2dview - [Errore apertura file]");
        SetWindowTextA(g_hwnd, g_title);
        if (g_doc && layout_content_width() != g_doc_content_w)
            request_render(layout_content_width());
        layout_update_scrollbars(g_hwnd, g_doc_h);
        InvalidateRect(g_hwnd, NULL, TRUE);
    }
}

static void on_render_done(void *doc, int used_w)
{
    if (g_shutdown)
        return;

    //fprintf(stderr, "[RENDER_DONE] used_w=%d g_render_busy=%d zoom=%.2f\n", used_w, g_render_busy, g_zoom);

    LOG_RENDER("on_render_done called");
    log_msg("[RENDER] on_render_done: used_w=%d g_render_busy=%d g_render_pending_width=%d g_render_pending_first=%d g_render_pending_last=%d\n",
            used_w, g_render_busy, g_render_pending_width, g_render_pending_first_page, g_render_pending_last_page);

    /* Un risultato di un render appartenente a un altro documento non deve
     * azzerare il busy di un render corrente. */
    if (g_render_doc && doc != g_render_doc)
        return;

    g_render_busy = 0;
    g_render_doc = NULL;

    if (doc != g_doc)
        return;

    g_doc_content_w = used_w;
    g_doc_h = document_height(doc);
    /* Geometria aggiornata: gli hit-test in volo con epoch precedente non
     * sono piu' coerenti con il layout appena completato. */
    g_layout_epoch++;

    /* ld_render_range oggi, al primo richiamo, fa full layout di tutto il
     * documento. Quindi dopo il primo RENDER_DONE possiamo considerare
     * TUTTE le pagine già layoutate: niente altri job a ogni salto. */
    {
        int page_count = ld_get_page_count(g_doc);
        if (page_count > 0)
            g_last_rendered_page = page_count - 1;
        else if (g_render_pending_last_page >= 0 && g_render_pending_last_page > g_last_rendered_page)
            g_last_rendered_page = g_render_pending_last_page;

        if (g_first_rendered_page < 0)
            g_first_rendered_page = 0;

        /* Aggiorna stima altezza pagina con valore REALE dopo full layout */
        int page_h = ld_get_page_height(g_doc, 0);
        if (page_h > 0)
            g_page_h_est = page_h;
        log_msg("[RENDER] on_render_done: page_count=%d g_last_rendered_page=%d g_page_h_est=%d\n",
                page_count, g_last_rendered_page, g_page_h_est);
    }

    /* Layout completo dopo il primo full render */
    g_layout_complete = 1;
    if (!g_load_active)
        g_doc_state = DOC_STATE_IDLE;
    apply_pending_justification();
    LOG_RENDER("layout complete flag set");

    /* Re-anchor the caret if inline editing mode is active. After a
     * render following an edit, the layout has been rebuilt and the
     * text_width_cache has been cleared by ld_render (content_changed
     * branch), so the DLL's JOB_CARET_HIT now returns the exact
     * post-edit caret position. The re-anchor refines the estimate
     * that was applied in inline_handle_result. */
    if (g_inline_active)
    {
#if CONSOLE_LOGGING
        fprintf(stderr, "[INLINE] render done: reanchor caret\n");
#endif
        /* Un solo re-anchor per edit applicato: la decisione e' presa qui,
         * dopo il render che ha ricostruito il layout. I backspace non vengono
         * piu' ripartiti ad ogni render: sono coalesciati dal timer inline. */
        inline_reanchor_caret();
    }

    /* Pending width (resize) ha priorità su pending range */
    if (!g_load_active && g_render_pending_width >= 0 && g_render_pending_width != used_w)
    {
        int w = g_render_pending_width;
        g_render_pending_width = -1;
        request_render(w);
        /* Non processare pending range se c'era pending width */
        g_render_pending_first_page = -1;
        g_render_pending_last_page = -1;
        layout_update_scrollbars(g_hwnd, g_doc_h);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }

    /* Pending range (scroll) */
    if (!g_load_active && g_render_pending_first_page >= 0)
    {
        int first = g_render_pending_first_page;
        int last = g_render_pending_last_page;
        g_render_pending_first_page = -1;
        g_render_pending_last_page = -1;
        request_render_range(first, last);
        return;
    }

    g_render_pending_width = -1;
    g_render_pending_first_page = -1;
    g_render_pending_last_page = -1;
    g_render_used_w = -1;
    layout_update_scrollbars(g_hwnd, g_doc_h);
    InvalidateRect(g_hwnd, NULL, FALSE);

    /* Transizione a IDLE: recupera un flush rimasto deferred per occupazione
     * (SetTimer/allocazione falliti). Invia al massimo un job, senza riarmare
     * il debounce pieno perche' non e' arrivato un nuovo input. */
    inline_resume_deferred();
}

static void start_async_load(const char *path)
{
    Job *j;
    int cw;

    /* Non cambiare documento mentre un paragrafo e' in editing o una
     * modifica e' in applicazione. */
    if (g_doc_state == DOC_STATE_HITTEST ||
        g_doc_state == DOC_STATE_EDITOR ||
        g_doc_state == DOC_STATE_APPLYING)
    {
        MessageBoxA(g_hwnd,
                    "Attendere il completamento dell'operazione sul paragrafo.",
                    "d2dview", MB_OK | MB_ICONINFORMATION);
        return;
    }

    /* Un cambio documento non puo' ereditare il timer/coalescing inline:
     * si chiude la sessione (annulla i caratteri non ancora confermati). */
    inline_end("load");

    init_callbacks();
    cw = layout_content_width();

    lstrcpynA(g_load_path, path, sizeof(g_load_path));
    g_load_gen++;
    g_load_active = 1;
    g_load_content_w = cw;
    g_doc_state = DOC_STATE_LOADING;
    g_layout_epoch++;
    /* Se inline_end ha lasciato un edit in volo, il suo risultato verra'
     * scartato (token azzerato): rilasciamo subito anche il flag di busy,
     * altrimenti il render del nuovo documento resterebbe in coda per sempre. */
    if (g_pending_apply_token)
    {
        g_pending_apply_token = 0;
        g_pending_apply_epoch = 0;
        g_render_busy = 0;
        g_render_doc = NULL;
    }
    g_render_pending_width = -1;
    g_render_pending_first_page = -1;
    g_render_pending_last_page = -1;

    /* UI mostra subito lo stato */
    SetWindowTextA(g_hwnd, "d2dview - Caricamento...");

    if (!g_worker_started)
    {
        /* Fallback sincrono: il worker non è disponibile. */
        void *doc = ld_open_file(path, &g_cb, LAY->page_w, LAY->ch > 0 ? LAY->ch : 600);
        /* D1: NON chiamare ld_render qui - apply_loaded_doc lancerà request_render */
        g_load_active = 0;
        apply_loaded_doc(g_load_gen, doc);
        return;
    }

    j = alloc_job(JOB_LOAD);
    if (!j)
    {
        g_load_active = 0;
        MessageBoxA(g_hwnd, "Memoria insufficiente", "Errore", MB_OK | MB_ICONERROR);
        return;
    }
    j->id = g_load_gen;
    lstrcpynA(j->path, path, sizeof(j->path));
    j->content_w = cw;
    j->viewport_w = LAY->page_w;
    j->viewport_h = LAY->ch > 0 ? LAY->ch : 600;
    enqueue_job(j);
}

static void load_docx_file(const char *path)
{
    if (!path || !path[0]) return;
    start_async_load(path);
}

/* Riallinea la larghezza di giustificazione quando il resize e' avvenuto
 * mentre il worker usava il DOM.  Chiamata solo da stato IDLE. */
static void apply_pending_justification(void)
{
    if (g_doc && g_justification_dirty)
    {
        int jw = LAY->page_w - 2 * LAYOUT_VISUAL_PADDING_X;
        if (jw > 0)
            ld_set_justification_width(g_doc, jw);
        g_justification_dirty = 0;
    }
}

/* Rilancia un render rimasto in sospeso durante hit-test/editor. */
static void resume_pending_render(void)
{
    if (g_doc && !g_load_active && g_doc_state == DOC_STATE_IDLE && !g_render_busy)
    {
        apply_pending_justification();
        if (g_render_pending_width >= 0)
        {
            int w = g_render_pending_width;
            g_render_pending_width = -1;
            request_render(w);
        }
    }
    /* Ultimo punto di transizione a IDLE: stesso recovery dell'errore di
     * edit/render. Se il job non puo' ancora partire, il flag resta attivo. */
    inline_resume_deferred();
}
