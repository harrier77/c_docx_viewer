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
    return 0;
}

static void viewer_on_destroy(void)
{
    // log_msg("[DESTROY] Cleaning up\n");
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