/*
 * viewer_render.c - richieste di render, caricamento e stato di paginazione.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

/* Richiesta di render generica.  job_kind distingue il render completo
 * (JOB_RENDER) da quello successivo a un edit (JOB_RELAYOUT_PARAGRAPH):
 * in FASE 1 entrambi eseguono ld_render(), ma il secondo porta para_index
 * fino al worker come base per il rilayout locale delle fasi successive. */
static void request_render_ex(int w, JobKind job_kind, int para_index)
{
    Job *j;

    if (!g_doc || g_load_active)
        return;

    DLOG("[RENDER_REQ] kind=%d para=%d w=%d g_render_busy=%d zoom=%.2f\n",
            job_kind, para_index, w, g_render_busy, g_zoom);

    LOG_RENDER("request_render called");
    log_msg("[RENDER] request_render: w=%d g_render_busy=%d para=%d\n",
            w, g_render_busy, para_index);

    /* Invalida layout cache se la larghezza di layout cambia (resize) */
    if (g_layout_cache && g_layout_cache->layout_width != w) {
        layout_cache_invalidate(g_layout_cache);
    }

    if (!g_worker_started)
    {
        /* Fallback sincrono */
        ld_render(g_doc, w);
        g_doc_content_w = w;
        g_doc_h = document_height(g_doc);
        layout_update_scrollbars(g_hwnd, g_doc_h);
        InvalidateRect(g_hwnd, NULL, FALSE);
        /* Popola layout cache (Fase 3) */
        if (g_layout_cache == NULL) {
            g_layout_cache = layout_cache_create();
        }
        if (g_layout_cache) {
            layout_cache_set_layout_params(g_layout_cache,
                layout_content_width(),
                LAY->page_w - 2 * LAYOUT_VISUAL_PADDING_X,
                LAYOUT_VISUAL_PADDING_X,
                LAYOUT_VISUAL_MARGIN_Y);
            ld_set_layout_cache_callbacks(g_doc, &g_cache_ops, g_layout_cache);
            ld_extract_layout(g_doc);
            g_layout_cache->valid = 1;
            layout_cache_dump_stats(g_layout_cache);
        }
        return;
    }

    if (g_render_busy)
    {
        LOG_RENDER("render busy, queuing pending width");
        g_render_pending_width = w;
        return;
    }

    j = alloc_job(job_kind);
    if (!j)
        return;
    j->doc = g_doc;
    j->content_w = w;
    j->para_index = para_index;
    g_render_busy = 1;
    g_render_doc = g_doc;
    g_render_used_w = w;
    g_render_pending_width = -1;
    dbg_log("[TABLE] request_render content_w=%d para=%d\n", w, para_index);
    enqueue_job(j);
    LOG_RENDER("render job enqueued");
}

static void request_render(int w)
{
    request_render_ex(w, JOB_RENDER, -1);
}

/* FASE 1: dopo un edit si passa dal job dedicato.  Il comportamento e'
 * ancora identico a request_render(); cambia solo il tipo di job. */
static void request_paragraph_relayout(int para_index)
{
    request_render_ex(layout_content_width(), JOB_RELAYOUT_PARAGRAPH, para_index);
}

/* FASE 5/6: Renderizza un range di pagine (es. pagina 0, o 1-2 per lookahead).
 * Se il range è già coperto, non tocca il worker e non alza g_render_busy:
 * così lo scroll su pagine già layoutate non produce pagina bianca. */
static void request_render_range(int first_page, int last_page)
{
    Job *j;

    if (!g_doc || g_load_active)
        return;

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
        ld_render_range(g_doc, first_page, LAY->ch, layout_content_width());
        {
            int page_count = ld_get_page_count(g_doc);
            if (page_count > 0)
                g_last_rendered_page = page_count - 1;
            else if (last_page > g_last_rendered_page)
                g_last_rendered_page = last_page;
        }
        g_doc_h = document_height(g_doc);
        layout_update_scrollbars(g_hwnd, g_doc_h);
        InvalidateRect(g_hwnd, NULL, FALSE);
        /* Popola layout cache (Fase 3) */
        if (g_layout_cache == NULL) {
            g_layout_cache = layout_cache_create();
        }
        if (g_layout_cache) {
            layout_cache_set_layout_params(g_layout_cache,
                layout_content_width(),
                LAY->page_w - 2 * LAYOUT_VISUAL_PADDING_X,
                LAYOUT_VISUAL_PADDING_X,
                LAYOUT_VISUAL_MARGIN_Y);
            ld_set_layout_cache_callbacks(g_doc, &g_cache_ops, g_layout_cache);
            ld_extract_layout(g_doc);
            g_layout_cache->valid = 1;
            layout_cache_dump_stats(g_layout_cache);
        }
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

    /* Un nuovo documento rende invalido il caret del precedente. */
    caret_close();

    /* Risultato di una richiesta superata: va distrutto sul worker. */
    if (id != g_load_gen)
    {
        if (doc)
            dispose_doc(doc);
        return;
    }

    g_load_active = 0;

    if (doc)
    {
        if (g_doc)
            dispose_doc(g_doc);
        g_doc = doc;
        g_doc_content_w = g_load_content_w;

        /* Invalida layout cache per nuovo documento */
        if (g_layout_cache) {
            layout_cache_invalidate(g_layout_cache);
        }

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

    DLOG("[RENDER_DONE] used_w=%d g_render_busy=%d zoom=%.2f\n", used_w, g_render_busy, g_zoom);

    LOG_RENDER("on_render_done called");
    log_msg("[RENDER] on_render_done: used_w=%d g_render_busy=%d g_render_pending_width=%d g_render_pending_first=%d g_render_pending_last=%d\n",
            used_w, g_render_busy, g_render_pending_width, g_render_pending_first_page, g_render_pending_last_page);

    g_render_busy = 0;
    g_render_doc = NULL;

    if (doc != g_doc)
        return;

    g_doc_content_w = used_w;
    g_doc_h = document_height(doc);

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
    LOG_RENDER("layout complete flag set");
    DLOG("[RENDER_DONE] g_layout_complete=1, creating/checking layout cache\n");

    /* La cache e' gia' stata popolata dal worker (worker_extract_layout in
     * viewer_jobs.c): qui sul thread UI si fa solo il ri-ancora del caret.
     * Estrarre qui correva con il job successivo del worker (crash). */
    if (g_layout_cache) {
        layout_cache_dump_stats(g_layout_cache);
        DLOG("[RENDER_DONE] layout cache ready (extracted by worker)\n");
        /* Dopo un commit il caret e' stato nascosto (reanchor=1): qui la
         * cache e' di nuovo valida e si può ricalcolare la posizione. */
        caret_reanchor_after_render();
    } else {
        DLOG("[RENDER_DONE] ERROR: g_layout_cache is NULL!\n");
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
}

static void start_async_load(const char *path)
{
    Job *j;
    int cw;

    init_callbacks();
    cw = layout_content_width();

    lstrcpynA(g_load_path, path, sizeof(g_load_path));
    g_load_gen++;
    g_load_active = 1;
    g_load_content_w = cw;
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

/* Log XML block for debugging */
void log_xml_block(const char *tag, int index, const char *xml)
{
    if (!xml) return;
    DLOG("[XML][%s] idx=%d:\n%s\n", tag, index, xml);
}

/* Riprende render in sospeso dopo editor/hit-test */
void resume_pending_render(void)
{
    if (g_render_pending_width >= 0 && g_render_pending_width != g_doc_content_w)
    {
        int w = g_render_pending_width;
        g_render_pending_width = -1;
        request_render(w);
    }
    else if (g_render_pending_first_page >= 0)
    {
        int first = g_render_pending_first_page;
        int last = g_render_pending_last_page;
        g_render_pending_first_page = -1;
        g_render_pending_last_page = -1;
        request_render_range(first, last);
    }
}

/* Accoda job per applicare modifica XML */
void request_paragraph_apply_xml(const char *xml)
{
    DLOG("[RENDER][APPLY_XML] START g_doc=%p g_edit_index=%d xml=%p len=%d g_worker_started=%d\n",
            g_doc, g_edit_index, xml, xml ? (int)strlen(xml) : 0, g_worker_started);
    if (!g_doc || !xml) {
        DLOG("[RENDER][APPLY_XML] IGNORED: g_doc=%p xml=%p\n", g_doc, xml);
        return;
    }
    if (!g_worker_started) {
        DLOG("[RENDER][APPLY_XML] IGNORED: worker not started\n");
        return;
    }

    Job *j = alloc_job(JOB_APPLY_EDIT);
    if (!j) {
        DLOG("[RENDER][APPLY_XML] alloc_job failed\n");
        return;
    }
    j->doc = g_doc;
    j->gen = g_load_gen;
    j->para_index = g_edit_index;
    j->xml_text = _strdup(xml);
    j->ooxml = 1;
    /* REGRESSION: DOC_STATE_APPLYING is released by handle_edit_applied() in
     * maind2dv.c on every outcome (success, error, stale). Any new early return
     * after this point must still let WM_APP_EDIT_APPLIED reach the UI thread,
     * otherwise double-click editing stays blocked forever. */
    g_doc_state = DOC_STATE_APPLYING;
    DLOG("[RENDER][APPLY_XML] enqueue JOB_APPLY_EDIT gen=%u para_index=%d\n", j->gen, j->para_index);
    enqueue_job(j);
}

/* Accoda job per applicare modifica testo semplice */
void request_paragraph_apply(const char *text)
{
    if (!g_doc || !text) return;
    if (!g_worker_started) return;

    Job *j = alloc_job(JOB_APPLY_EDIT);
    if (!j) return;
    j->doc = g_doc;
    j->gen = g_load_gen;
    j->para_index = g_edit_index;
    j->text = _strdup(text);
    j->ooxml = 0;
    /* REGRESSION: paired with handle_edit_applied() in maind2dv.c; see the
     * matching note in request_paragraph_apply_xml() above. */
    g_doc_state = DOC_STATE_APPLYING;
    enqueue_job(j);
}

/* Commit del caret inline: come request_paragraph_apply ma con indice
 * esplicito e marcato come caret, così handle_edit_applied ri-ancora invece
 * di trattarlo come una modifica dell'editor OOXML. */
void request_caret_apply(int para_index, const char *text)
{
    if (!g_doc || !text) return;
    if (!g_worker_started) return;

    Job *j = alloc_job(JOB_APPLY_EDIT);
    if (!j) return;
    j->doc = g_doc;
    j->gen = g_load_gen;
    j->para_index = para_index;
    j->text = _strdup(text);
    j->ooxml = 0;
    j->caret = 1;
    g_doc_state = DOC_STATE_APPLYING;
    DLOG("[CARET] enqueue JOB_APPLY_EDIT para=%d gen=%u\n", para_index, j->gen);
    enqueue_job(j);
}

/* Accoda hit-test asincrono per doppio click su paragrafo.
 * doc_x, doc_y: coordinate in spazio pagina litedocx (layout a zoom 1.0),
 *               INCLUSO visual padding (18,6) come impostato via ld_set_visual_padding.
 * Imposta g_doc_state = DOC_STATE_HITTEST per bloccare ulteriori input.
 * Risultato arriva via WM_APP_HIT_RESULT -> handle_hit_result -> begin_paragraph_edit_at_index.
 * REGRESSION FIX: senza CS_DBLCLKS in window class + questa funzione, doppio click non funzionava. */
void request_hit_test(int doc_x, int doc_y)
{
    DLOG("[HIT_REQ] request_hit_test doc=(%d,%d) g_doc=%p g_doc_state=%d g_worker_started=%d\n",
            doc_x, doc_y, g_doc, g_doc_state, g_worker_started);
    if (!g_doc || g_doc_state != DOC_STATE_IDLE) {
        DLOG("[HIT_REQ] IGNORED: g_doc=%p g_doc_state=%d\n", g_doc, g_doc_state);
        return;
    }
    if (!g_worker_started) {
        DLOG("[HIT_REQ] IGNORED: worker not started\n");
        return;
    }

    Job *j = alloc_job(JOB_HIT_TEST);
    if (!j) {
        DLOG("[HIT_REQ] alloc_job failed\n");
        return;
    }
    j->doc = g_doc;
    j->gen = g_load_gen;
    j->epoch = g_layout_epoch;
    j->doc_x = doc_x;
    j->doc_y = doc_y;
    g_doc_state = DOC_STATE_HITTEST;
    DLOG("[HIT_REQ] enqueue JOB_HIT_TEST gen=%u epoch=%d\n", j->gen, j->epoch);
    enqueue_job(j);
}
