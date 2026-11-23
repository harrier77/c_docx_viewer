/*
 * viewer_jobs.c - coda job e worker thread.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

static Job *alloc_job(JobKind kind)
{
    Job *j = (Job *)calloc(1, sizeof(Job));
    if (!j)
        return NULL;
    j->kind = kind;
    j->hwnd = g_hwnd;
    return j;
}

static void enqueue_job(Job *j)
{
    if (!j)
        return;
    EnterCriticalSection(&g_job_lock);
    j->next = NULL;
    if (g_job_tail)
        g_job_tail->next = j;
    else
        g_job_head = j;
    g_job_tail = j;
    LeaveCriticalSection(&g_job_lock);
    SetEvent(g_job_event);
}

static Job *dequeue_job(void)
{
    Job *j;
    EnterCriticalSection(&g_job_lock);
    j = g_job_head;
    if (j)
    {
        g_job_head = j->next;
        if (!g_job_head)
            g_job_tail = NULL;
    }
    LeaveCriticalSection(&g_job_lock);
    return j;
}

static void enqueue_destroy(void *doc)
{
    Job *j;
    if (!doc)
        return;
    j = alloc_job(JOB_DESTROY);
    if (!j)
    {
        ld_destroy(doc); /* fallback: non dovrebbe mai accadere */
        return;
    }
    j->doc = doc;
    enqueue_job(j);
}

/* Distrugge un documento sul thread che lo ha creato: il worker.
 * Se il worker non esiste, siamo in modalità sincrona e va bene il thread UI. */
static void dispose_doc(void *doc)
{
    if (!doc)
        return;
    if (g_worker_started)
        enqueue_destroy(doc);
    else
        ld_destroy(doc);
}

static DWORD WINAPI WorkerProc(LPVOID arg)
{
    (void)arg;
    for (;;)
    {
        Job *j = dequeue_job();
        if (!j)
        {
            WaitForSingleObject(g_job_event, INFINITE);
            continue;
        }

        if (j->kind == JOB_QUIT)
        {
            free(j);
            break;
        }

        switch (j->kind)
        {
        case JOB_LOAD:
        {
            LOG_JOB(JOB_LOAD, "start ld_open_file (NO ld_render)");
            void *doc = ld_open_file(j->path, &g_cb,
                                     j->viewport_w > 0 ? j->viewport_w : 800,
                                     j->viewport_h > 0 ? j->viewport_h : 600);
            /* D1: NON chiamare ld_render qui - layout avverrà in background dopo apply_loaded_doc */

            if (g_shutdown)
            {
                if (doc) ld_destroy(doc);
            }
            else if (!doc)
            {
                PostMessageA(j->hwnd, WM_APP_DOC_READY, (WPARAM)j->id, 0);
            }
            else
            {
                PostMessageA(j->hwnd, WM_APP_DOC_READY, (WPARAM)j->id, (LPARAM)doc);
            }
            LOG_JOB(JOB_LOAD, "ld_open_file done, posted WM_APP_DOC_READY");
            break;
        }

        case JOB_RENDER:
            LOG_JOB(JOB_RENDER, "start ld_render");
            LOG_RENDER("ld_render called from JOB_RENDER");
            /* Essential trace: one line per full layout, shows the width
             * contract between viewer and litedocx (see layout_content_width()). */
#if CONSOLE_LOGGING
            DLOG("[RENDER] ld_render content_w=%d\n", j->content_w);
#endif
            if (j->doc && !g_shutdown)
                ld_render(j->doc, j->content_w);
            LOG_JOB(JOB_RENDER, "ld_render done, posting WM_APP_RENDER_DONE");
            if (!g_shutdown)
                PostMessageA(j->hwnd, WM_APP_RENDER_DONE,
                             (WPARAM)j->content_w, (LPARAM)j->doc);
            break;

        case JOB_RENDER_RANGE:
            LOG_JOB(JOB_RENDER_RANGE, "start ld_render_range");
            LOG_RENDER("ld_render_range called from JOB_RENDER_RANGE");
            if (j->doc && !g_shutdown) {
                for (int p = j->first_page; p <= j->last_page; ++p) {
                    ld_render_range(j->doc, p, j->viewport_h, j->content_w);
                }
            }
            LOG_JOB(JOB_RENDER_RANGE, "ld_render_range done, posting WM_APP_RENDER_DONE");
            if (!g_shutdown)
                PostMessageA(j->hwnd, WM_APP_RENDER_DONE,
                             (WPARAM)j->content_w, (LPARAM)j->doc);
            break;

       case JOB_HIT_TEST:
           {
               DLOG("[HIT_JOB] JOB_HIT_TEST start doc_x=%d doc_y=%d gen=%u epoch=%d\n",
                       j->doc_x, j->doc_y, j->gen, j->epoch);
               if (!g_shutdown && j->doc)
               {
                   ld_paragraph_hit hit;
                   char *text = NULL;
                   int got = ld_paragraph_hit_test(j->doc, j->doc_x, j->doc_y, &hit, &text);
                   DLOG("[HIT_JOB] ld_paragraph_hit_test got=%d hit.index=%d text=%p\n",
                           got, got ? hit.index : -1, text);
                   HitResult *r = (HitResult *)calloc(1, sizeof(HitResult));
                   if (r)
                   {
                       r->doc = j->doc;
                       r->gen = j->gen;
                       r->epoch = j->epoch;
                       r->hit = got;
                       r->index = got ? hit.index : -1;
                       r->text = text;
                       DLOG("[HIT_JOB] PostMessage WM_APP_HIT_RESULT\n");
                       PostMessageA(j->hwnd, WM_APP_HIT_RESULT, 0, (LPARAM)r);
                   }
                   else
                   {
                       DLOG("[HIT_JOB] calloc failed for HitResult\n");
                       ld_free_string(text);
                       PostMessageA(j->hwnd, WM_APP_HIT_RESULT, 0, 0);
                   }
               } else {
                   DLOG("[HIT_JOB] IGNORED: g_shutdown=%ld j->doc=%p\n", g_shutdown, j->doc);
               }
               break;
           }

       case JOB_RUN_HIT_TEST:
           {
               /* Diagnostica read-only: ld_run_hit_test non tocca testo,
                * DOM ne' cache di layout. Esito via WM_APP_RUN_HIT_RESULT. */
               if (!g_shutdown && j->doc)
               {
                   RunHitResult *r = (RunHitResult *)calloc(1, sizeof(RunHitResult));
                   if (r)
                   {
                       r->doc   = j->doc;
                       r->gen   = j->gen;
                       r->epoch = j->epoch;
                       r->doc_x = j->doc_x;
                       r->doc_y = j->doc_y;
                       r->hit   = ld_run_hit_test(j->doc, j->doc_x, j->doc_y, &r->info);
                       PostMessageA(j->hwnd, WM_APP_RUN_HIT_RESULT, 0, (LPARAM)r);
                   }
               }
               break;
           }

           case JOB_GET_PARA_XML:
           {
               if (!g_shutdown && j->doc && j->para_index >= 0)
               {
                   ParaXmlResult *r = (ParaXmlResult *)calloc(1, sizeof(ParaXmlResult));
                   if (r)
                   {
                       r->doc = j->doc;
                       r->gen = j->gen;
                       r->epoch = j->epoch;
                       r->index = j->para_index;
                       r->ok = ld_paragraph_get_xml(j->doc, j->para_index, &r->xml);
                       if (!r->ok)
                       {
                           const char *err = ld_last_error();
                           lstrcpynA(r->error, err ? err : "", sizeof(r->error));
                       }
                       PostMessageA(j->hwnd, WM_APP_PARA_XML_READY, 0, (LPARAM)r);
                   }
               }
               break;
           }

           case JOB_APPLY_EDIT:
        {
            DLOG("[JOB_APPLY_EDIT] START doc=%p gen=%u para_index=%d xml_text=%p ooxml=%d g_shutdown=%ld\n",
                    j->doc, j->gen, j->para_index, j->xml_text, j->ooxml, g_shutdown);
            EditResult *r = (EditResult *)calloc(1, sizeof(EditResult));
            int ok = 0;
            if (!g_shutdown && j->doc)
            {
                if (j->xml_text)
                {
                    DLOG("[XML][WORKER-APPLY] idx=%d bytes=%d ooxml=%d\n",
                            j->para_index, (int)strlen(j->xml_text), j->ooxml);
                    log_xml_block("WORKER-APPLY", j->para_index, j->xml_text);
                    if (j->ooxml)
                    {
                        DLOG("[XML][WORKER-APPLY] calling ld_paragraph_set_ooxml\n");
                        ok = ld_paragraph_set_ooxml(j->doc, j->para_index, j->xml_text);
                        DLOG("[XML][WORKER-APPLY] ld_paragraph_set_ooxml returned ok=%d\n", ok);
                    }
                    else
                    {
                        DLOG("[XML][WORKER-APPLY] calling ld_paragraph_set_xml\n");
                        ok = ld_paragraph_set_xml(j->doc, j->para_index, j->xml_text);
                        DLOG("[XML][WORKER-APPLY] ld_paragraph_set_xml returned ok=%d\n", ok);
                    }
                    DLOG("[XML][WORKER-APPLY] ok=%d err='%s'\n",
                            ok, ok ? "" : ld_last_error());
                }
                else if (j->text)
                {
                    DLOG("[TEXT][WORKER-APPLY] calling ld_paragraph_set_text\n");
                    ok = ld_paragraph_set_text(j->doc, j->para_index, j->text);
                    DLOG("[TEXT][WORKER-APPLY] ld_paragraph_set_text returned ok=%d\n", ok);
                }
            }
            else
            {
                DLOG("[JOB_APPLY_EDIT] IGNORED: g_shutdown=%ld j->doc=%p\n", g_shutdown, j->doc);
            }
            if (r)
            {
                r->doc = j->doc;
                r->gen = j->gen;
                r->token = j->token;
                r->ok = ok;
                if (!ok)
                {
                    const char *err = ld_last_error();
                    lstrcpynA(r->error, err ? err : "", sizeof(r->error));
                }
                DLOG("[JOB_APPLY_EDIT] Posting WM_APP_EDIT_APPLIED ok=%d\n", ok);
                if (!g_shutdown)
                    PostMessageA(j->hwnd, WM_APP_EDIT_APPLIED, 0, (LPARAM)r);
                else
                    free(r);
            }
            else
            {
                DLOG("[JOB_APPLY_EDIT] calloc failed for EditResult\n");
            }
            free(j->text);
            j->text = NULL;
            free(j->xml_text);
            j->xml_text = NULL;
            DLOG("[JOB_APPLY_EDIT] END\n");
            break;
        }

        case JOB_DESTROY:
            if (j->doc)
                ld_destroy(j->doc);
            break;

        default:
            break;
        }
        free(j);
    }
    return 0;
}

static int start_worker(void)
{
    if (g_worker_started)
        return 1;

    InitializeCriticalSection(&g_job_lock);
    g_job_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_job_event)
        return 0;

    g_worker_thread = CreateThread(NULL, 0, WorkerProc, NULL, 0, NULL);
    if (!g_worker_thread)
        return 0;

    g_worker_started = 1;
    return 1;
}