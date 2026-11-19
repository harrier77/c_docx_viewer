/*
 * viewer_paragraph.c - hit-test, applicazione modifiche e coordinate paragrafo.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

/* Converte un punto client in coordinate documento.
 * La trasformazione D2D e' puramente scalare e ancorata all'origine, e
 * ld_draw riceve rc_content.left/top come origine logica:
 *     screen = (rc_content.<x|y> + doc_<x|y>) * g_zoom
 * quindi l'inversa e' doc = client / zoom - rc_content. */
static void client_to_doc(int cx, int cy, int *doc_x, int *doc_y)
{
    RECT rc;
    double z = (g_zoom > 0.0f) ? (double)g_zoom : 1.0;
    layout_content_rect(g_doc_h, &rc);
    if (doc_x)
        *doc_x = (int)floor((double)cx / z - (double)rc.left + 0.5);
    if (doc_y)
        *doc_y = (int)floor((double)cy / z - (double)rc.top + 0.5);
}

/* Esito di JOB_APPLY_EDIT.  Non libera r: lo fa il chiamante. */
static void on_edit_applied(EditResult *r)
{
    if (!r)
        return;

    // fprintf(stderr, "[XML][APPLY-DONE] doc=%p gen=%u token=%u pending=%u ok=%d err='%s'\n",
    //         r->doc, r->gen, r->token, g_pending_apply_token, r->ok,
    //         r->error[0] ? r->error : "");

    /* Solo il risultato del job corrente puo' chiudere la fase APPLYING.
     * Oltre a doc/gen/token viene verificata anche l'epoch del layout
     * catturata quando il job e' stato accodato: un risultato di una
     * generazione di layout diversa non deve mai aggiornare lo stato UI. */
    if (r->doc != g_doc || r->gen != g_load_gen ||
        r->token != g_pending_apply_token || r->epoch != g_pending_apply_epoch)
    {
        // fprintf(stderr, "[XML][APPLY-DONE] scartato (stale): doc=%d gen=%d token=%d epoch=%d\n",
        //         r->doc == g_doc, r->gen == g_load_gen,
        //         r->token == g_pending_apply_token, r->epoch == g_pending_apply_epoch);
        return;
    }

    /* Se l'edit inline e' attivo, gestisci il risultato qui e ritorna. */
    if (g_inline_active)
    {
        inline_handle_result(r);
        return;
    }

    g_pending_apply_token = 0;
    g_render_busy = 0;
    g_render_doc = NULL;

    if (r->ok)
    {
        /* Il testo e' cambiato: la cache dei paragrafi e la paginazione
         * lato DLL sono gia' state invalidate.  Qui si invalida lo stato
         * lato viewer e si forza un layout COMPLETO (ld_render), perche'
         * ld_render_range potrebbe non rilanciare il layout. */
        g_layout_complete = 0;
        g_first_rendered_page = -1;
        g_last_rendered_page = -1;
        g_render_pending_width = -1;
        g_render_pending_first_page = -1;
        g_render_pending_last_page = -1;
        g_doc_state = DOC_STATE_IDLE;
        g_layout_epoch++;
        viewer_caret_clear();
        apply_pending_justification();
        request_render(layout_content_width());
        InvalidateRect(g_hwnd, NULL, TRUE);
    }
    else
    {
        g_doc_state = DOC_STATE_IDLE;
        MessageBoxA(g_hwnd,
                    r->error[0] ? r->error : "Modifica non applicata.",
                    "d2dview", MB_OK | MB_ICONERROR);
        resume_pending_render();
    }
}

/* Accoda la sostituzione del testo del paragrafo in editing.  g_render_busy
 * viene impostato QUI, sul thread UI, prima di accodare il job. */
static void request_paragraph_apply(const char *text)
{
    if (!g_doc || !text || g_edit_index < 0)
    {
        g_doc_state = DOC_STATE_IDLE;
        return;
    }

    g_apply_token++;
    g_doc_state = DOC_STATE_APPLYING;
    g_pending_apply_token = g_apply_token;
    g_pending_apply_epoch = g_layout_epoch;

    if (!g_worker_started)
    {
        EditResult r;
        ZeroMemory(&r, sizeof(r));
        r.doc = g_doc;
        r.gen = g_load_gen;
        r.epoch = g_pending_apply_epoch;
        r.token = g_apply_token;
        r.ok = ld_paragraph_set_text(g_doc, g_edit_index, text);
        if (!r.ok)
        {
            const char *err = ld_last_error();
            lstrcpynA(r.error, err ? err : "", sizeof(r.error));
        }
        on_edit_applied(&r);
        return;
    }

    {
        Job *j = alloc_job(JOB_APPLY_EDIT);
        if (!j)
        {
            g_pending_apply_token = 0;
            g_doc_state = DOC_STATE_IDLE;
            return;
        }
        j->doc = g_doc;
        j->gen = g_load_gen;
        j->epoch = g_layout_epoch;
        j->token = g_apply_token;
        j->para_index = g_edit_index;
        j->text = _strdup(text);
        if (!j->text)
        {
            free(j);
            g_pending_apply_token = 0;
            g_doc_state = DOC_STATE_IDLE;
            return;
        }
        g_render_busy = 1;
        g_render_doc = g_doc;
        enqueue_job(j);
    }
}

/* Accoda la sostituzione del testo del run in editing (finestra del run,
 * doppio click destro).  g_edit_index = paragrafo, g_edit_run_slot = slot
 * del run (esito dell'hit-test, non ricalcolato).  g_render_busy viene
 * impostato QUI, sul thread UI, prima di accodare il job, come in
 * request_paragraph_apply. */
static void request_run_apply(const char *text)
{
    if (!g_doc || !text || g_edit_index < 0 || g_edit_run_slot < 0)
    {
        g_doc_state = DOC_STATE_IDLE;
        return;
    }

    g_apply_token++;
    g_doc_state = DOC_STATE_APPLYING;
    g_pending_apply_token = g_apply_token;
    g_pending_apply_epoch = g_layout_epoch;

    if (!g_worker_started)
    {
        EditResult r;
        ZeroMemory(&r, sizeof(r));
        r.doc = g_doc;
        r.gen = g_load_gen;
        r.epoch = g_pending_apply_epoch;
        r.token = g_apply_token;
        r.ok = ld_run_set_text(g_doc, g_edit_index, g_edit_run_slot, text);
        if (!r.ok)
        {
            const char *err = ld_last_error();
            lstrcpynA(r.error, err ? err : "", sizeof(r.error));
        }
        on_edit_applied(&r);
        return;
    }

    {
        Job *j = alloc_job(JOB_RUN_APPLY_EDIT);
        if (!j)
        {
            g_pending_apply_token = 0;
            g_doc_state = DOC_STATE_IDLE;
            return;
        }
        j->doc = g_doc;
        j->gen = g_load_gen;
        j->epoch = g_layout_epoch;
        j->token = g_apply_token;
        j->para_index = g_edit_index;
        j->run_index = g_edit_run_slot;
        j->text = _strdup(text);
        if (!j->text)
        {
            free(j);
            g_pending_apply_token = 0;
            g_doc_state = DOC_STATE_IDLE;
            return;
        }
        g_render_busy = 1;
        g_render_doc = g_doc;
        enqueue_job(j);
    }
}

/* Avvia l'hit-test sul worker (o in modo sincrono senza worker). */
static void begin_paragraph_edit_at(int doc_x, int doc_y)
{
    if (!g_doc)
        return;

    if (!g_worker_started)
    {
        ld_paragraph_hit hit;
        char            *text = NULL;
        if (ld_paragraph_hit_test(g_doc, doc_x, doc_y, &hit, &text) && text)
        {
            /* Usa direttamente l'editor di testo plain (XML editor disabilitato) */
            show_paragraph_editor(hit.index, text);
            ld_free_string(text);
        }
        return;
    }

    {
        Job *j = alloc_job(JOB_HIT_TEST);
        if (!j)
            return;
        j->doc   = g_doc;
        j->gen   = g_load_gen;
        j->epoch = g_layout_epoch;
        j->doc_x = doc_x;
        j->doc_y = doc_y;
        /* Flag: para_index = -1 significa "dopo hit-test, richiedi XML" (disabilitato: usa testo plain) */
        j->para_index = -1;
        g_doc_state = DOC_STATE_HITTEST;
        enqueue_job(j);
    }
}

/* Esito di JOB_HIT_TEST.  Libera sempre r. */
static void handle_hit_result(HitResult *r)
{
    if (!r)
    {
        if (g_doc_state == DOC_STATE_HITTEST)
            g_doc_state = DOC_STATE_IDLE;
        resume_pending_render();
        return;
    }

    /* Un editor gia' aperto ha la precedenza: il risultato non deve
     * sovrascrivere lo stato EDITOR ne' aprire una seconda finestra. */
    if (g_edit_hwnd)
    {
        ld_free_string(r->text);
        free(r);
        return;
    }

    /* Scarta hit-test obsoleti: documento, generazione e versione layout
     * devono coincidere.  Nessun editor viene aperto. */
    if (r->doc != g_doc || r->gen != g_load_gen || r->epoch != g_layout_epoch)
    {
        ld_free_string(r->text);
        free(r);
        if (g_doc_state == DOC_STATE_HITTEST)
            g_doc_state = DOC_STATE_IDLE;
        resume_pending_render();
        return;
    }

    if (!r->hit || !r->text)
    {
        ld_free_string(r->text);
        free(r);
        g_doc_state = DOC_STATE_IDLE;
        resume_pending_render();
        return;
    }

    /* XML editor disabilitato: usa direttamente l'editor di testo plain */
    g_doc_state = DOC_STATE_IDLE;
    g_edit_is_xml = 0;
    show_paragraph_editor(r->index, r->text);
    ld_free_string(r->text);
    free(r);
}

/* ---------------------------------------------------------------
 * NUOVO: Gestione edit XML paragrafo
 * --------------------------------------------------------------- */

/* Esito di JOB_GET_PARA_XML. Apre l'editor con l'XML. */
static void handle_para_xml_ready(ParaXmlResult *r)
{
    if (!r)
    {
        if (g_doc_state == DOC_STATE_HITTEST)
            g_doc_state = DOC_STATE_IDLE;
        resume_pending_render();
        return;
    }

    if (g_edit_hwnd)
    {
        ld_free_string(r->xml);
        free(r);
        return;
    }

    if (r->doc != g_doc || r->gen != g_load_gen || r->epoch != g_layout_epoch)
    {
        ld_free_string(r->xml);
        free(r);
        if (g_doc_state == DOC_STATE_HITTEST)
            g_doc_state = DOC_STATE_IDLE;
        resume_pending_render();
        return;
    }

    if (!r->ok || !r->xml)
    {
        MessageBoxA(g_hwnd,
                    r->error[0] ? r->error : "Errore lettura XML paragrafo",
                    "Errore", MB_OK | MB_ICONERROR);
        g_doc_state = DOC_STATE_IDLE;
        resume_pending_render();
        ld_free_string(r->xml);
        free(r);
        return;
    }

    g_doc_state = DOC_STATE_IDLE;
    g_edit_is_xml = 1;
    show_paragraph_editor_xml(r->index, r->xml);
    ld_free_string(r->xml);
    free(r);
}

/* Accoda applicazione XML paragrafo. */
static void request_paragraph_apply_xml(const char *xml)
{
    // fprintf(stderr, "[XML][APPLY-REQ] idx=%d worker=%d\n", g_edit_index, g_worker_started);
    log_xml_block("APPLY-REQ", g_edit_index, xml);
    if (!g_doc || !xml || g_edit_index < 0)
    {
        // fprintf(stderr, "[XML][APPLY-REQ] parametri non validi: doc=%p xml=%p idx=%d\n",
        //         g_doc, (const void *)xml, g_edit_index);
        g_doc_state = DOC_STATE_IDLE;
        return;
    }

    g_apply_token++;
    g_doc_state = DOC_STATE_APPLYING;
    g_pending_apply_token = g_apply_token;
    g_pending_apply_epoch = g_layout_epoch;

    if (!g_worker_started)
    {
        EditResult r;
        ZeroMemory(&r, sizeof(r));
        r.doc = g_doc;
        r.gen = g_load_gen;
        r.epoch = g_pending_apply_epoch;
        r.token = g_apply_token;
        r.ok = ld_paragraph_set_ooxml(g_doc, g_edit_index, xml);
        // fprintf(stderr, "[XML][APPLY-SYNC-OOXML] ok=%d err='%s'\n",
        //         r.ok, r.ok ? "" : ld_last_error());
        if (!r.ok)
        {
            const char *err = ld_last_error();
            lstrcpynA(r.error, err ? err : "", sizeof(r.error));
        }
        on_edit_applied(&r);
        return;
    }

    {
        Job *j = alloc_job(JOB_APPLY_EDIT);
        if (!j)
        {
            g_pending_apply_token = 0;
            g_doc_state = DOC_STATE_IDLE;
            return;
        }
        j->doc = g_doc;
        j->gen = g_load_gen;
        j->epoch = g_layout_epoch;
        j->token = g_apply_token;
        j->para_index = g_edit_index;
        j->xml_text = _strdup(xml);
        j->ooxml = 1;
        if (!j->xml_text)
        {
            free(j);
            g_pending_apply_token = 0;
            g_doc_state = DOC_STATE_IDLE;
            return;
        }
        // fprintf(stderr, "[XML][APPLY-ASYNC] enqueued idx=%d bytes=%d token=%u\n",
        //         g_edit_index, (int)strlen(xml), g_apply_token);
        g_render_busy = 1;
        g_render_doc = g_doc;
        enqueue_job(j);
    }
}
