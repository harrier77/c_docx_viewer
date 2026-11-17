/*
 * viewer_lifecycle.c - creazione e distruzione delle risorse della finestra.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

static LRESULT viewer_on_create(HWND hwnd)
{
    // log_msg("[CREATE] WM_CREATE\n");
    g_hwnd = hwnd;
    /* Crea IDWriteFactory per DirectWrite (text rendering migliorato).
       DWriteCreateFactory non richiede CoInitialize quando si usa il
       tipo SHARED; creiamo qui l'factory sul thread UI prima del renderer. */
    {
        static const GUID IID_IDWriteFactory =
            {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
        if (SUCCEEDED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                &IID_IDWriteFactory,
                (IUnknown **)&g_dwrite_factory))) {
            fprintf(stderr, "[DWRITE] factory created ok\n");
        } else {
            fprintf(stderr, "[DWRITE] factory creation failed, testo via GDI interop\n");
            g_dwrite_factory = NULL;
        }
    }
    /* Unico renderer disponibile: Direct2D. Nessun fallback GDI. */
    g_renderer = renderer_d2d_create(g_dwrite_factory,
            NULL, NULL);  /* resolver opzionale: NULL = usa GDI interop interno */
    if (!g_renderer) {
        fprintf(stderr, "[RENDERER] Fallito D2D\n");
        MessageBoxA(hwnd,
                    "Impossibile creare il renderer Direct2D.",
                    "Errore", MB_OK | MB_ICONERROR);
        return -1;
    }
    /* Tooltip nativo per il click destro: finestra TOOLTIPS "tracking"
     * attivata solo on-demand da viewer_on_rbuttonup (nessun tool
     * associato a un'area: solo TTF_TRACK | TTF_ABSOLUTE). */
    {
        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        TOOLINFOW ti;
        g_tip_wnd = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                                    WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    hwnd, NULL, hi, NULL);
        log_msg("[TOOLTIP] create: g_tip_wnd=%p (err=%lu)\n",
                (void *)g_tip_wnd, GetLastError());
        if (g_tip_wnd) {
            LRESULT addres;
            /* TopMost e no-activate: il tooltip non ruba il focus. */
            SetWindowPos(g_tip_wnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            /* Consente il testo su piu' righe (le due righe del
             * tooltip: coordinate client e coordinate documento). */
            SendMessageW(g_tip_wnd, TTM_SETMAXTIPWIDTH, 0, 300);
            ZeroMemory(&ti, sizeof(ti));
            ti.cbSize = TTTOOLINFOW_V1_SIZE;
            ti.hwnd = hwnd;            /* finestra "proprietaria" del tool */
            ti.uId = 1;                /* id fittizio, tool senza hit-test */
            /* TTF_TRANSPARENT: i messaggi mouse attraversano il tooltip. */
            ti.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_TRANSPARENT;
            addres = SendMessageW(g_tip_wnd, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            log_msg("[TOOLTIP] TTM_ADDTOOLW -> %ld\n", (long)addres);
        } else {
            fprintf(stderr, "[TOOLTIP] creazione TOOLTIPS fallita\n");
        }
    }
    return 0;
}

static void viewer_on_destroy(void)
{
    // log_msg("[DESTROY] Cleaning up\n");
    /* Un editor aperto viene annullato: nessuna modifica al documento. */
    g_doc_state = DOC_STATE_IDLE;
    g_pending_apply_token = 0;
    g_edit_apply = 0;
    if (g_tip_wnd)
    {
        DestroyWindow(g_tip_wnd);
        g_tip_wnd = NULL;
        g_tip_shown = 0;
    }
    if (g_edit_hwnd)
    {
        DestroyWindow(g_edit_hwnd);
        g_edit_hwnd = NULL;
    }
    free(g_edit_result_utf8);
    g_edit_result_utf8 = NULL;
    /* Tutte le distruzioni avvengono sul worker, lo stesso thread che ha
     * creato i documenti (COM/GDI hanno affinità di thread). Non si attende:
     * un layout in corso non deve bloccare la chiusura. */
    g_shutdown = 1;
    if (g_doc) {
        dispose_doc(g_doc);
        g_doc = NULL;
    }
    if (g_worker_started) {
        Job *q = alloc_job(JOB_QUIT);
        if (q) enqueue_job(q);
    }
    if (g_renderer) {
        g_renderer->vtbl->Destroy(g_renderer);
        g_renderer = NULL;
    }
    /* Clean up global font cache */
    {
        FontCacheEntry *e = g_font_cache;
        while (e) {
            FontCacheEntry *next = e->next;
            if (e->format) IDWriteTextFormat_Release(e->format);
            free(e->family);
            free(e);
            e = next;
        }
        g_font_cache = NULL;
    }
    if (g_dwrite_factory) {
        IDWriteFactory_Release(g_dwrite_factory);
        g_dwrite_factory = NULL;
    }
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    PostQuitMessage(0);
}
