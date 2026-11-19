/*
 * viewer_inline.c - gestione edit inline (prototipo in-memory).
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 *
 * Prototipo per documento piccolo: solo insert/delete a confine carattere,
 * caratteri stampabili + Backspace, niente frecce/selection/undo.
 * Persistenza OOXML disattivata durante il typing, attivata solo su salita.
 */

#include "litedocx_c.h"

#define LD_INLINE_OP_INSERT     0
#define LD_INLINE_OP_BACKSPACE  1
#define LD_INLINE_OP_DELETE     2

/* Avvia la modalita' inline sul run indicato dall'hit-test del caret.
 * Viene chiamato dopo aver ottenuto un ld_run_caret_hit valido. */
static void inline_begin(const ld_caret_hit* hit)
{
    if (!hit || hit->para_index < 0 || hit->run_index < 0)
        return;

    g_inline_active  = 1;
    g_inline_para    = hit->para_index;
    g_inline_run     = hit->run_index;
    g_inline_offset     = hit->char_offset; /* offset nel paragrafo (spazio normalizzato) */
    g_inline_run_offset = hit->run_char_offset; /* offset dentro il run */
    if (g_inline_run_offset < 0) g_inline_run_offset = 0; /* FIX: hit-test potrebbe restituire -1 */
    g_inline_coalesce = 0;
    free(g_inline_buffer);
    g_inline_buffer = NULL;
    free(g_inline_last_char);
    g_inline_last_char = NULL;
    free(g_inline_run_text);
    g_inline_run_text = NULL;

    /* Ottieni il testo del run dal paragrafo usando i boundary dal caret hit. */
    char* para_text = ld_paragraph_get_text(g_doc, hit->para_index);
    if (para_text && hit->run_char_start >= 0 && hit->run_char_end >= hit->run_char_start)
    {
        int run_len = hit->run_char_end - hit->run_char_start;
        if (run_len > 0 && hit->run_char_start + run_len <= (int)strlen(para_text))
        {
            g_inline_run_text = (char*)malloc(run_len + 1);
            if (g_inline_run_text)
            {
                memcpy(g_inline_run_text, para_text + hit->run_char_start, run_len);
                g_inline_run_text[run_len] = '\0';
            }
        }
        else if (run_len == 0)
        {
            /* Run vuoto: allocare un buffer vuoto perche' inline_apply_edit
             * fa strlen(g_inline_run_text) e non gestisce NULL. */
            g_inline_run_text = (char*)malloc(1);
            if (g_inline_run_text) g_inline_run_text[0] = '\0';
            g_inline_run_offset = 0;
        }
    }
    if (para_text) ld_free_string(para_text);

    // fprintf(stderr, "[INLINE] begin para=%d run=%d para_off=%d run_off=%d run_text='%s'\n",
    //         g_inline_para, g_inline_run, g_inline_offset, g_inline_run_offset, g_inline_run_text ? g_inline_run_text : "");
}

/* Termina la modalita' inline. */
static void inline_end(const char* why)
{
    if (!g_inline_active)
        return;

    // fprintf(stderr, "[INLINE] end (%s)\n", why ? why : "?");
    g_inline_active  = 0;
    g_inline_para    = -1;
    g_inline_run     = -1;
    g_inline_offset     = -1;
    g_inline_run_offset = -1;
    g_inline_coalesce = 0;
    g_inline_pending_op = -1;
    free(g_inline_buffer);
    g_inline_buffer = NULL;
    free(g_inline_run_text);
    free(g_inline_last_char);
    g_inline_run_text = NULL;
    g_inline_last_char = NULL;
}

/* Applica un singolo edit inline (insert/backspace) sul run corrente.
 * Chiama ld_run_edit sul worker thread (tramite job asincrono). */
static void inline_apply_edit(int op, const char* text)
{
    if (!g_inline_active || !g_doc || g_inline_para < 0 || g_inline_run < 0)
        return;

    /* Se il worker e' occupato, accumula il tasto per coalescing. */
    if (g_render_busy || g_doc_state != DOC_STATE_IDLE)
    {
        fprintf(stderr, "[CARET line 100] busy %d\n", op);
        if (op == LD_INLINE_OP_INSERT && text && *text)
        {
            size_t old_len = g_inline_buffer ? strlen(g_inline_buffer) : 0;
            size_t add_len = strlen(text);
            char* new_buf = (char*)realloc(g_inline_buffer, old_len + add_len + 1);
            if (new_buf)
            {
                g_inline_buffer = new_buf;
                memcpy(g_inline_buffer + old_len, text, add_len + 1);
                g_inline_coalesce = 1;
            }
        }
        else if (op == LD_INLINE_OP_BACKSPACE)
        {
            /* Per backspace non accumuliamo nel buffer: accodiamo un job
             * separato con op=1. Il coalescing per backspace e' piu' delicato
             * perche' l'offset cambia. Per il prototipo lo mandiamo subito. */
        }
        return;
    }

    /* Manda il job al worker. Usiamo lo stesso meccanismo di JOB_RUN_APPLY_EDIT
     * ma con ld_run_edit invece di ld_run_set_text. Per ora usiamo un job
     * sincrono (fallback) per semplicita', poi lo renderemo asincrono. */

    if (!g_worker_started)
    {
        fprintf(stderr, "[CARET line 128] sync %d\n", op);
        /* Fallback sincrono: usa ld_run_set_text con testo completo del run */
        int ok = 0;
        if (g_inline_run_text)
        {
            if (op == LD_INLINE_OP_INSERT && text) //inizio blocco per offset in run
            {
                int run_len = (int)strlen(g_inline_run_text);
                int txt_len = (int)strlen(text);
                int ins_pos = (g_inline_run_offset >= 0 && g_inline_run_offset <= run_len) ? g_inline_run_offset : run_len;
                int new_len = run_len + txt_len + 1;
                char* new_text = (char*)malloc(new_len);
                if (new_text)
                {
                    memcpy(new_text, g_inline_run_text, ins_pos);
                    memcpy(new_text + ins_pos, text, txt_len);
                    memcpy(new_text + ins_pos + txt_len, g_inline_run_text + ins_pos, run_len - ins_pos); //Sync insert Grok corretto
                    new_text[new_len - 1] = '\0';
                    ok = ld_run_set_text(g_doc, g_inline_para, g_inline_run, new_text);
                    if (ok)
                    {
                        free(g_inline_run_text);
                        free(g_inline_last_char);
                        g_inline_run_text = new_text;
                        g_inline_run_offset += txt_len;
                    }
                    else
                    {
                        free(new_text);
                    }
                }
            }
            else if (op == LD_INLINE_OP_BACKSPACE && strlen(g_inline_run_text) > 0)
            {
                int len = strlen(g_inline_run_text);
                int del_pos = g_inline_run_offset - 1;  // cancella il carattere PRIMA del cursore fix di Pi
                if (len > 0)
                  if (del_pos >= 0 && del_pos < len)
                   {
                       memmove(g_inline_run_text + del_pos, g_inline_run_text + del_pos + 1, len - del_pos);
                       ok = ld_run_set_text(g_doc, g_inline_para, g_inline_run, g_inline_run_text);
                   } //fine fix 
            }
        }
        
        if (ok)
        {
            fprintf(stderr, "[CARET sinc line 175] sync ok x=%d\n", g_caret_x);
            /* Aggiorna offset caret locale */
            if (op == LD_INLINE_OP_INSERT && text)
            {
                g_inline_offset += (int)strlen(text);
                /* g_inline_run_offset e' gia' stato aggiornato sopra nel ramo ok */
            }
            else if (op == LD_INLINE_OP_BACKSPACE && g_inline_offset > 0)
            {
                g_inline_offset -= 1;
                if (g_inline_run_offset > 0) g_inline_run_offset -= 1;
            }

            /* Avanza offset caret globale prima di salvare */
            if (op == LD_INLINE_OP_INSERT && text)
            {
                int add_len = (int)strlen(text);
                g_caret_offset += add_len;
                g_caret_run_end += add_len;
            }
            else if (op == LD_INLINE_OP_BACKSPACE)
            {
                if (g_caret_offset > 0) g_caret_offset -= 1;
                if (g_caret_run_end > g_caret_run_start) g_caret_run_end -= 1;
            }

            /* Richiedi re-layout e riancora caret */
            g_layout_complete      = 0;
            g_first_rendered_page  = -1;
            g_last_rendered_page   = -1;
            g_layout_epoch++;

            /* Salva e ripristina il caret per inline_reanchor_caret() */
            {
                int _sp = g_caret_para, _sr = g_caret_run, _so = g_caret_offset;
                int _rs = g_caret_run_start, _re = g_caret_run_end;
                int _sx = g_caret_x, _sy = g_caret_y, _sw = g_caret_w, _sh = g_caret_h;
                viewer_caret_clear();
                g_caret_valid  = 1;
                g_caret_para   = _sp; g_caret_run    = _sr;
                g_caret_offset = _so;
                g_caret_run_start = _rs; g_caret_run_end = _re;
                g_caret_x      = _sx;   g_caret_y      = _sy;
                g_caret_w      = _sw > 0 ? _sw : 1;
                g_caret_h      = _sh;
            }
            request_render(layout_content_width());
            InvalidateRect(g_hwnd, NULL, TRUE);
        }
        return;
    }

    /* Job asincrono: usa ld_run_set_text con testo completo del run. */
    {
        fprintf(stderr, "[CARET line 229] job op=%d x=%d\n", op, g_caret_x);
        Job* j = alloc_job(JOB_RUN_APPLY_EDIT); /* riutilizziamo lo stesso tipo */
        if (!j)
            return;
        j->doc = g_doc;
        j->gen = g_load_gen;
        j->epoch = g_layout_epoch;
        j->token = ++g_apply_token;
        j->para_index = g_inline_para;
        j->run_index = g_inline_run;
        
        /* Calcola il nuovo testo completo del run */
        char* new_run_text = NULL;
        if (g_inline_run_text)
        {
            if (op == LD_INLINE_OP_INSERT && text)
            {
                int run_len = (int)strlen(g_inline_run_text);
                int txt_len = (int)strlen(text);
                int ins_pos = (g_inline_run_offset >= 0 && g_inline_run_offset <= run_len) ? g_inline_run_offset : run_len;
                int new_len = run_len + txt_len + 1;
                new_run_text = (char*)malloc(new_len);
                if (new_run_text)
                {
                    memcpy(new_run_text, g_inline_run_text, ins_pos);
                    memcpy(new_run_text + ins_pos, text, txt_len);
                    //memcpy(new_run_text+ins_pos, g_inline_run_text + ins_pos, run_len - ins_pos); //aggiunto da Grok ma non va bene
                    memcpy(new_run_text + ins_pos + txt_len, g_inline_run_text + ins_pos, run_len - ins_pos); //fix di Pi a codice Grok
                    new_run_text[new_len - 1] = '\0';
                }
            }
            else if (op == LD_INLINE_OP_BACKSPACE && strlen(g_inline_run_text) > 0)
            {
                int len = strlen(g_inline_run_text);
                int del_pos = g_inline_run_offset - 1;  // cancella il carattere PRIMA del cursore fix di Pi
                if (del_pos >= 0 && del_pos < len)
                   {
                       new_run_text = (char*)malloc(len + 1);
                       if (new_run_text)
                       {
                           memcpy(new_run_text, g_inline_run_text, del_pos);                // parte prima del char da cancellare
                           memcpy(new_run_text + del_pos, g_inline_run_text + del_pos + 1, len - del_pos); // salta il char da cancellare
                           new_run_text[len - 1] = '\0';
                       }
                   } //fine fix
            }
        }
        
        j->doc = g_doc;
        j->gen = g_load_gen;
        j->epoch = g_layout_epoch;
        j->token = ++g_apply_token;
        j->para_index = g_inline_para;
        j->run_index = g_inline_run;
        /* Aggiungi marker 0xFF 0xFF per identificare edit inline */
        char* marked_text = NULL;
        if (new_run_text)
        {
            int marked_len = strlen(new_run_text) + 2 + 1;
            marked_text = (char*)malloc(marked_len);
            if (marked_text)
            {
                marked_text[0] = (char)0xFF;
                marked_text[1] = (char)0xFF;
                memcpy(marked_text + 2, new_run_text, strlen(new_run_text) + 1);
            }
            free(new_run_text);
        }
        
        j->text = marked_text;  /* Testo completo del run con marker */

        // fprintf(stderr, "[INLINE] enqueue job para=%d run=%d op=%d new_text_len=%d\n",
        //         g_inline_para, g_inline_run, op, new_run_text ? (int)strlen(new_run_text) : 0);

        /* Memorizza l'operazione per aggiornare l'offset al completamento */
        g_inline_pending_op = op;
        /* Memorizza l'ultimo carattere inserito per aggiornare g_inline_run_text */
        free(g_inline_last_char);
        g_inline_last_char = text ? strdup(text) : NULL;

        g_pending_apply_token = j->token;
        g_doc_state = DOC_STATE_APPLYING;
        g_render_busy = 1;
        g_render_doc = g_doc;
        enqueue_job(j);
    }
}

/* Gestisce il risultato di un edit inline (chiamato da on_edit_applied). */
static void inline_handle_result(EditResult* r)
{
    if (!r || !g_inline_active)
        return;

    if (r->doc != g_doc || r->gen != g_load_gen || r->token != g_pending_apply_token)
        return; /* stale */

    g_pending_apply_token = 0;
    g_render_busy = 0;
    g_render_doc = NULL;

    if (r->ok)
    {
        /* Ripristina lo stato del documento per permettere il disegno */
        g_doc_state = DOC_STATE_IDLE;

        /* L'edit e' riuscito: riancora il caret facendo un nuovo caret hit-test
         * alla posizione aggiornata. Per ora ricomputiamo l'offset locale e
         * richiediamo un nuovo caret hit-test asincrono. */
        // fprintf(stderr, "[INLINE] edit ok, re-anchoring caret\n");

        /* Reset cache paginazione per forzare il re-layout. */
        g_layout_complete      = 0;
        g_first_rendered_page  = -1;
        g_last_rendered_page   = -1;
        g_layout_epoch++;

        /* Salva lo stato del caret prima di cancellarlo, poi lo ripristina
         * subito: inline_reanchor_caret() (chiamata da on_render_done)
         * richiede g_caret_valid == 1 per lavorare. */
        /* Salva lo stato del caret prima di cancellarlo.
         *
         * FIX: NON ripristiniamo g_caret_x/saved_caret_x qui: dopo
         * l'aggiornamento qui sotto (che applica la STIMA di avanzata
         * del caret), g_caret_x e' gia' alla nuova posizione stimata.
         * inline_reanchor_caret() usera' quel valore per il JOB_CARET_HIT;
         * se la DLL restituisce una x piu' grande, la adottiamo; se
         * e' uguale o minore (cache stalla o risposta pre-edit),
         * non regredisce la posizione.
         *
         * Gli altri campi (para, run, offset, y, w, h) si ripristinano
         * dallo stato salvato: il run non cambia, l'offset si aggiorna
         * qui sotto. */
        int saved_caret_para     = g_caret_para;
        int saved_caret_run      = g_caret_run;
        int saved_caret_offset   = g_caret_offset;
        int saved_caret_rs       = g_caret_run_start;
        int saved_caret_re       = g_caret_run_end;
        /* caret_x e caret_y NON vengono salvati/ripristinati qui:
         * caret_x viene aggiornato in-place dalla stima qui sotto;
         * caret_y non cambia durante la modalita' inline. */
        int saved_caret_y        = g_caret_y;
        int saved_caret_w        = g_caret_w;
        int saved_caret_h        = g_caret_h;
        viewer_caret_clear();

        fprintf(stderr, "[CARET line 375] res x=%d\n", g_caret_x);
        g_caret_valid  = 1;
        g_caret_para   = saved_caret_para;
        g_caret_run    = saved_caret_run;
        g_caret_offset = saved_caret_offset;
        g_caret_run_start = saved_caret_rs;
        g_caret_run_end   = saved_caret_re;
        /* g_caret_x is NOT restored from saved state: it stays at the
         * current value (the estimate applied below is the correct
         * post-edit position). The re-anchor (JOB_CARET_HIT) will
         * refine it to the exact position via the DLL. */
        g_caret_y      = saved_caret_y;
        g_caret_w      = saved_caret_w > 0 ? saved_caret_w : 1;
        g_caret_h      = saved_caret_h;

        /* Update the local run text for the next edit.
         * g_doc_state is already DOC_STATE_IDLE, so request_render
         * actually starts (not queued as pending). on_render_done
         * calls inline_reanchor_caret which issues JOB_CARET_HIT;
         * the DLL (with text_width_cache cleared by ld_render after
         * a content change) returns the exact post-edit position. */
        if (g_inline_run_text && g_inline_pending_op >= 0)
        {
            if (g_inline_pending_op == LD_INLINE_OP_INSERT && g_inline_last_char)
            {
                /* 
                 * CRITICAL REGRESSION FIX:
                 * When inserting characters (especially mid-run or sequentially), we must insert 
                 * the new text at the exact cursor position (`ins_pos`) rather than blindly 
                 * appending it to the end of the run buffer. 
                 * Advancing offsets first and then slicing the buffer via memcpy ensures 
                 * correct character ordering and prevents text inversion/overwriting issues.
                 */
                int old_len = (int)strlen(g_inline_run_text);
                int add_len = (int)strlen(g_inline_last_char);
                
                g_inline_run_offset += add_len;
                g_caret_offset += add_len;
                g_caret_run_end += add_len;

                int ins_pos = g_inline_run_offset - add_len;
                if (ins_pos < 0) ins_pos = 0;
                if (ins_pos > old_len) ins_pos = old_len;

                char* new_text = (char*)malloc(old_len + add_len + 1);
                if (new_text)
                {
                    memcpy(new_text, g_inline_run_text, ins_pos);
                    memcpy(new_text + ins_pos, g_inline_last_char, add_len);
                    memcpy(new_text + ins_pos + add_len, g_inline_run_text + ins_pos, old_len - ins_pos);
                    new_text[old_len + add_len] = '\0';

                    free(g_inline_run_text);
                    g_inline_run_text = new_text;
                }
                /* FIX char_w CARET: use the font-derived width instead of g_caret_x / pre_edit_len. */
                {
                    int step = g_inline_char_w > 0 ? g_inline_char_w : 8; /* FIX char_w CARET: fall back to the measured 8px step when no font metric is available. */
                    g_caret_x += step * add_len; /* FIX char_w CARET: advance by the same constant font metric on every keystroke. */
                    /*Attention: the value to monitor is step, if decreases something is wrong */
                    fprintf(stderr, "[CARET inline] est+ step=%d x=%d\n", step, g_caret_x); /* FIX char_w CARET: log the stable step for verification. */
                }
                free(g_inline_last_char);
                g_inline_last_char = NULL;
            }
            else if (g_inline_pending_op == LD_INLINE_OP_BACKSPACE)
            {
                int len = (int)strlen(g_inline_run_text);
                /* Decrement the caret offset BEFORE the estimate.
                 * The estimate uses g_inline_run_offset + 1 (the pre-edit
                 * character count) to compute the average width. */
                if (g_inline_run_offset > 0) g_inline_run_offset -= 1;
                if (g_caret_offset > 0) g_caret_offset -= 1;
                if (g_caret_run_end > g_caret_run_start) g_caret_run_end -= 1;
                if (len > 0)
                {
                    /* FIX char_w CARET: retreat by the same font-derived step (symmetric with insert). */
                    int step = g_inline_char_w > 0 ? g_inline_char_w : 8; /* FIX char_w CARET: fall back to 8px when the font metric is missing. */
                    g_caret_x -= step; /* FIX char_w CARET: no decaying average, so the retreat stays constant. */
                    g_inline_run_text[len - 1] = '\0'; /* FIX char_w CARET: drop the deleted character from the local run buffer. */
                    fprintf(stderr, "[CARET inline] est- step=%d x=%d\n", step, g_caret_x); /* FIX char_w CARET: log the stable step for verification. */
                }
            }
        }

        request_render(layout_content_width());
        InvalidateRect(g_hwnd, NULL, TRUE);
        fprintf(stderr, "[CARET line 504] done x=%d\n", g_caret_x);
    }
    else
    {
        // fprintf(stderr, "[INLINE] edit failed: %s\n", r->error);
        MessageBoxA(g_hwnd, r->error[0] ? r->error : "Edit inline fallito",
                    "d2dview", MB_OK | MB_ICONERROR);
    }
}

/* Gestione tasti per edit inline: caratteri stampabili + Backspace.
 * Ritorna 1 se il tasto e' stato gestito (edit inline attivo), 0 altrimenti. */
static int inline_handle_key(WPARAM wp, LPARAM lp)
{
    if (!g_inline_active || !g_doc)
        return 0;

    /* Backspace */
    if (wp == VK_BACK)
    {
        inline_apply_edit(LD_INLINE_OP_BACKSPACE, NULL);
        return 1;
    }

    /* Escape: esce dalla modalita' inline */
    if (wp == VK_ESCAPE)
    {
        inline_end("escape");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return 1;
    }

    /* Caratteri stampabili: TranslateMessage ha gia' generato WM_CHAR.
     * Qui intercettiamo solo WM_KEYDOWN per Backspace. Gli altri tasti
     * (frecce, Delete, Enter, Tab) non sono supportati nel prototipo. */
    return 0;
}

/* Gestione WM_CHAR per caratteri stampabili. */
static int inline_handle_char(WPARAM wp)
{
    if (!g_inline_active || !g_doc)
        return 0;

    /* Ignora caratteri di controllo (già gestiti da WM_KEYDOWN) */
    if (wp < 32)
        return 0;

    /* Converti WPARAM (UTF-16 code unit) in UTF-8.
     * Per il prototipo assumiamo BMP (singolo WCHAR). */
    WCHAR wc = (WCHAR)wp;
    char utf8[5] = {0};
    int len = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, utf8, sizeof(utf8), NULL, NULL);
    if (len > 0 && len < (int)sizeof(utf8))
    {
        inline_apply_edit(LD_INLINE_OP_INSERT, utf8);
        return 1;
    }
    return 0;
}

/* Riancora il caret dopo un edit completato (layout finito).
 * Chiama ld_run_caret_hit alla posizione documento del caret precedente. */
static void inline_reanchor_caret(void)
{
    fprintf(stderr, "[CARET line 569] reanchor x=%d\n", g_caret_x);
    if (!g_inline_active || !g_doc || !g_caret_valid)
        return;

    if (!g_worker_started || g_doc_state != DOC_STATE_IDLE)
    {
        fprintf(stderr, "[CARET line 575] reanchor skip\n");
        return;
    }

    Job* j = alloc_job(JOB_CARET_HIT);
    if (!j)
        return;
    j->doc = g_doc;
    j->gen = g_load_gen;
    j->epoch = g_layout_epoch;
    j->doc_x = g_caret_x;
    j->doc_y = g_caret_y + (g_caret_h > 0 ? g_caret_h / 2 : 0);
    g_doc_state = DOC_STATE_HITTEST;
    enqueue_job(j);
}