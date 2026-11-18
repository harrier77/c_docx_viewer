/*
 * viewer_editor_ui.c - finestra modale di modifica OOXML del paragrafo.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

static char *control_text_utf8(HWND ctl)
{
    int   len;
    WCHAR *w;
    int   bytes;
    char *out;

    if (!ctl)
        return NULL;
    len = GetWindowTextLengthW(ctl);
    w = (WCHAR *)malloc((size_t)(len + 1) * sizeof(WCHAR));
    if (!w)
        return NULL;
    GetWindowTextW(ctl, w, len + 1);
    bytes = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    out = NULL;
    if (bytes > 0)
    {
        out = (char *)malloc((size_t)bytes);
        if (out)
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out, bytes, NULL, NULL);
    }
    free(w);
    return out;
}

/* Confronto che ignora i CR: il controllo EDIT converte i newline in CRLF,
 * quindi il testo riletto non e' byte-identico a quello impostato. */
static int strings_equal_ignore_cr(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    for (;;)
    {
        while (*a == '\r') a++;
        while (*b == '\r') b++;
        if (*a != *b)
            return 0;
        if (!*a)
            return 1;
        a++;
        b++;
    }
}

/* Commit nella modalita' "modifica run": nessun confronto con l'OOXML
 * originale, il testo letto dal controllo va dritto in g_edit_result_utf8
 * (poi request_run_apply lo manda a ld_run_set_text). */
static void commit_run_editor(HWND hwnd)
{
    char *text = control_text_utf8(g_edit_ctl_xml);
    if (!text)
    {
        g_edit_apply = 0;
        DestroyWindow(hwnd);
        return;
    }
    free(g_edit_result_utf8);
    g_edit_result_utf8 = text;
    g_edit_apply = 1;
    DestroyWindow(hwnd);
}

static void commit_paragraph_editor(HWND hwnd)
{
    // fprintf(stderr, "[XML][COMMIT] XML tab has_original=%d\n",
    //         g_edit_xml_original ? 1 : 0);

    char *xml = control_text_utf8(g_edit_ctl_xml);
    if (!xml)
    {
        g_edit_apply = 0;
        DestroyWindow(hwnd);
        return;
    }
    log_xml_block("COMMIT-ORIGINAL", g_edit_index, g_edit_xml_original);
    log_xml_block("COMMIT-EDITED", g_edit_index, xml);
    if (!strings_equal_ignore_cr(xml, g_edit_xml_original))
    {
        // fprintf(stderr, "[XML][COMMIT] OOXML modificato: applico via ld_paragraph_set_ooxml\n");
        free(g_edit_result_utf8);
        g_edit_result_utf8 = xml;
        g_edit_is_xml = 1;
        g_edit_apply = 1;
        DestroyWindow(hwnd);
        return;
    }
    // fprintf(stderr, "[XML][COMMIT] OOXML invariato: nessun cambiamento\n");
    free(xml);
    g_edit_apply = 0;
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK paragraph_editor_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        g_edit_hwnd = hwnd;
        g_edit_tab = NULL;  // No tab control - only XML editing
        g_edit_tab_index = 1;  // Force XML mode
        g_edit_ctl = NULL;  // No simple text editor
        g_edit_ctl_xml = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
            0, 0, 10, 10, hwnd, (HMENU)(UINT_PTR)IDC_PARA_EDIT_XML,
            GetModuleHandle(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Annulla",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 10, 10, hwnd, (HMENU)(UINT_PTR)IDC_PARA_CANCEL,
            GetModuleHandle(NULL), NULL);
        /* Modalita' di sola lettura (visualizzatore del run): niente
         * Applica e controllo EDIT non modificabile. */
        if (g_edit_readonly)
        {
            SendMessageW(g_edit_ctl_xml, EM_SETREADONLY, TRUE, 0);
        }
        else
        {
            CreateWindowExW(0, L"BUTTON", L"Applica",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 10, 10, hwnd, (HMENU)(UINT_PTR)IDC_PARA_APPLY,
                GetModuleHandle(NULL), NULL);
        }
        CreateWindowExW(0, L"STATIC",
            L"Modifica OOXML del paragrafo (in memoria, salva per applicare su file).",
            WS_CHILD | WS_VISIBLE,
            0, 0, 10, 10, hwnd, (HMENU)(UINT_PTR)IDC_PARA_HINT,
            GetModuleHandle(NULL), NULL);
        if (g_edit_ctl_xml){
           HFONT hFont = CreateFontW(
                -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
            SendMessageW(g_edit_ctl_xml, WM_SETFONT, (WPARAM)hFont, TRUE);}
        {
            HWND hint = GetDlgItem(hwnd, IDC_PARA_HINT);
            HWND ba = GetDlgItem(hwnd, IDC_PARA_APPLY);
            HWND bc = GetDlgItem(hwnd, IDC_PARA_CANCEL);
            if (hint) SendMessageW(hint, WM_SETFONT,
                                   (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            if (ba) SendMessageW(ba, WM_SETFONT,
                                 (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            if (bc) SendMessageW(bc, WM_SETFONT,
                                 (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        }
        return 0;

    case WM_SIZE:
    {
        RECT rc;
        int  cw, ch, margin = 12, button_w = 90, button_h = 28, hint_h = 48;
        int  edit_y, edit_h;
        GetClientRect(hwnd, &rc);
        cw = rc.right - rc.left;
        ch = rc.bottom - rc.top;
        edit_y = margin;
        edit_h = ch - edit_y - hint_h - button_h - 8;
        if (edit_h < 40)
            edit_h = 40;
        if (g_edit_ctl_xml)
            MoveWindow(g_edit_ctl_xml, margin, edit_y,
                       cw - margin * 2, edit_h, TRUE);
        {
            HWND hint = GetDlgItem(hwnd, IDC_PARA_HINT);
            HWND ba = GetDlgItem(hwnd, IDC_PARA_APPLY);
            HWND bc = GetDlgItem(hwnd, IDC_PARA_CANCEL);
            int  hint_y = edit_y + edit_h + 6;
            if (hint)
                MoveWindow(hint, margin, hint_y,
                           cw - margin * 2, hint_h, TRUE);
            if (bc)
                MoveWindow(bc, cw - margin - button_w, ch - margin - button_h,
                           button_w, button_h, TRUE);
            if (ba)
                MoveWindow(ba, cw - margin * 2 - button_w * 2 + 8,
                           ch - margin - button_h, button_w, button_h, TRUE);
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_PARA_APPLY:
            /* Modalita' run: commit dedicato (testo del run, niente
             * confronto OOXML). */
            if (g_edit_run_slot >= 0)
                commit_run_editor(hwnd);
            else
                commit_paragraph_editor(hwnd);
            return 0;
        case IDC_PARA_CANCEL:
            g_edit_apply = 0;
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        /* Chiusura con la X equivale ad Annulla. */
        g_edit_apply = 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_edit_hwnd = NULL;
        g_edit_ctl = NULL;
        g_edit_ctl_xml = NULL;
        g_edit_tab = NULL;
        free(g_edit_xml_original);
        g_edit_xml_original = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ensure_editor_class(void)
{
    WNDCLASSEXW wc;
    if (g_editor_class_registered)
        return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = paragraph_editor_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = LD_PARA_EDIT_CLASS;
    RegisterClassExW(&wc);
    g_editor_class_registered = 1;
}

/* Apre l'editor modale sul paragrafo index.  Ritorna dopo la chiusura; se
 * l'utente ha applicato, accoda JOB_APPLY_EDIT. */
static void show_paragraph_editor(int index, const char *utf8_text)
{
    static ACCEL accels[2];
    WCHAR       *wtext;
    RECT         rc_main;
    int          w = 660, h = 440, x, y;
    HWND         hwnd;
    MSG          msg;
    int          quit = 0;

    if (index < 0 || g_edit_hwnd)
        return;

    ensure_editor_class();

    g_edit_index = index;
    g_edit_apply = 0;
    free(g_edit_result_utf8);
    g_edit_result_utf8 = NULL;

    GetWindowRect(g_hwnd, &rc_main);
    x = rc_main.left + ((rc_main.right - rc_main.left) - w) / 2;
    y = rc_main.top + ((rc_main.bottom - rc_main.top) - h) / 2;

    g_doc_state = DOC_STATE_EDITOR;
    hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, LD_PARA_EDIT_CLASS,
                           L"Modifica paragrafo",
                           WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                           x, y, w, h, g_hwnd, NULL,
                           GetModuleHandle(NULL), NULL);
    if (!hwnd)
    {
        g_edit_index = -1;
        g_doc_state = DOC_STATE_IDLE;
        return;
    }

    wtext = u8_to_w(utf8_text ? utf8_text : "");
    if (wtext)
    {
        SetWindowTextW(g_edit_ctl_xml, wtext);
        free(wtext);
    }

    /* Carica nel tab XML il markup del paragrafo.  g_edit_xml_original viene
     * usato per capire se l'utente ha davvero modificato l'XML: se non lo ha
     * fatto, Applica si comporta come prima (testo semplice). */
    free(g_edit_xml_original);
    g_edit_xml_original = NULL;
    if (g_edit_ctl_xml)
    {
        static const char *XML_UNAVAILABLE = "<!-- OOXML del paragrafo non disponibile -->";
        char *xml = NULL;
        const char *load = XML_UNAVAILABLE;
        int got = ld_paragraph_get_ooxml(g_doc, index, &xml);
        // fprintf(stderr, "[XML][GET-OOXML] idx=%d ok=%d xml=%p\n", index, got, (void *)xml);
        if (!got)
        {
            // fprintf(stderr, "[XML][GET-OOXML] errore ld_last_error='%s'\n", ld_last_error());
        }
        if (got && xml && xml[0])
            load = xml;
        else
        {
            // fprintf(stderr, "[XML][GET-OOXML] nessun OOXML utile: uso placeholder\n");
        }
        log_xml_block("GET-OOXML", index, load);
        g_edit_xml_original = _strdup(load);
        {
            WCHAR *wx = u8_to_w(load);
            if (wx)
            {
                SetWindowTextW(g_edit_ctl_xml, wx);
                free(wx);
            }
        }
        if (xml)
            ld_free_string(xml);
    }

    accels[0].fVirt = FVIRTKEY;
    accels[0].key = VK_ESCAPE;
    accels[0].cmd = IDC_PARA_CANCEL;
    accels[1].fVirt = FVIRTKEY | FCONTROL;
    accels[1].key = VK_RETURN;
    accels[1].cmd = IDC_PARA_APPLY;
    g_edit_accel = CreateAcceleratorTableW(accels, 2);

    EnableWindow(g_hwnd, FALSE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    SetFocus(g_edit_ctl_xml);
    SendMessageW(g_edit_ctl_xml, EM_SETSEL, 0, -1);

    for (;;)
    {
        int r = GetMessageW(&msg, NULL, 0, 0);
        if (r == 0)
        {
            /* WM_QUIT: chiude l'editor e lascia uscire anche il loop
             * principale. */
            quit = 1;
            break;
        }
        if (r == -1)
            break;
        if (!g_edit_accel || !TranslateAcceleratorW(hwnd, g_edit_accel, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_edit_hwnd)
            break;
    }

    if (g_edit_hwnd)
    {
        g_edit_apply = 0;
        DestroyWindow(g_edit_hwnd);
        g_edit_hwnd = NULL;
    }
    if (g_edit_accel)
    {
        DestroyAcceleratorTable(g_edit_accel);
        g_edit_accel = NULL;
    }

    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);

    if (quit)
    {
        g_edit_index = -1;
        free(g_edit_result_utf8);
        g_edit_result_utf8 = NULL;
        g_doc_state = DOC_STATE_IDLE;
        PostQuitMessage(0);
        return;
    }

    if (g_edit_apply && g_edit_result_utf8)
    {
        if (g_edit_is_xml)
            request_paragraph_apply_xml(g_edit_result_utf8);
        else
            request_paragraph_apply(g_edit_result_utf8);
    }
    else
    {
        g_edit_is_xml = 0;
        g_doc_state = DOC_STATE_IDLE;
        InvalidateRect(g_hwnd, NULL, FALSE);
        resume_pending_render();
    }

    g_edit_index = -1;
    free(g_edit_result_utf8);
    g_edit_result_utf8 = NULL;
}

/* Editor per XML: simile a show_paragraph_editor ma con hint diverso. */
static void show_paragraph_editor_xml(int index, const char *xml_utf8)
{
    static ACCEL accels[2];
    WCHAR       *wtext;
    RECT         rc_main;
    int          w = 660, h = 440, x, y;
    HWND         hwnd;
    MSG          msg;
    int          quit = 0;

    if (index < 0 || g_edit_hwnd)
        return;

    ensure_editor_class();

    g_edit_index = index;
    g_edit_apply = 0;
    free(g_edit_result_utf8);
    g_edit_result_utf8 = NULL;

    GetWindowRect(g_hwnd, &rc_main);
    x = rc_main.left + ((rc_main.right - rc_main.left) - w) / 2;
    y = rc_main.top + ((rc_main.bottom - rc_main.top) - h) / 2;

    g_doc_state = DOC_STATE_EDITOR;
    hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, LD_PARA_EDIT_CLASS,
                           L"Modifica XML paragrafo",
                           WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                           x, y, w, h, g_hwnd, NULL,
                           GetModuleHandle(NULL), NULL);
    if (!hwnd)
    {
        g_edit_index = -1;
        g_edit_is_xml = 0;
        g_doc_state = DOC_STATE_IDLE;
        return;
    }

    wtext = u8_to_w(xml_utf8 ? xml_utf8 : "");
    if (wtext)
    {
        SetWindowTextW(g_edit_ctl_xml, wtext);
        free(wtext);
    }

    /* Aggiorna hint per XML */
    HWND hint = GetDlgItem(hwnd, IDC_PARA_HINT);
    if (hint)
        SetWindowTextW(hint,
            L"Modifica l'XML w:p del paragrafo. Attenzione: XML malformato corrompe il documento.\nIl file DOCX su disco NON viene modificato (solo in memoria).");

    accels[0].fVirt = FVIRTKEY;
    accels[0].key = VK_ESCAPE;
    accels[0].cmd = IDC_PARA_CANCEL;
    accels[1].fVirt = FVIRTKEY | FCONTROL;
    accels[1].key = VK_RETURN;
    accels[1].cmd = IDC_PARA_APPLY;
    g_edit_accel = CreateAcceleratorTableW(accels, 2);

    EnableWindow(g_hwnd, FALSE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    SetFocus(g_edit_ctl_xml);
    SendMessageW(g_edit_ctl_xml, EM_SETSEL, 0, -1);

    for (;;)
    {
        int r = GetMessageW(&msg, NULL, 0, 0);
        if (r == 0)
        {
            quit = 1;
            break;
        }
        if (r == -1)
            break;
        if (!g_edit_accel || !TranslateAcceleratorW(hwnd, g_edit_accel, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_edit_hwnd)
            break;
    }

    if (g_edit_hwnd)
    {
        g_edit_apply = 0;
        DestroyWindow(g_edit_hwnd);
        g_edit_hwnd = NULL;
    }
    if (g_edit_accel)
    {
        DestroyAcceleratorTable(g_edit_accel);
        g_edit_accel = NULL;
    }

    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);

    if (quit)
    {
        g_edit_index = -1;
        g_edit_is_xml = 0;
        free(g_edit_result_utf8);
        g_edit_result_utf8 = NULL;
        g_doc_state = DOC_STATE_IDLE;
        PostQuitMessage(0);
        return;
    }

    if (g_edit_apply && g_edit_result_utf8)
    {
        request_paragraph_apply_xml(g_edit_result_utf8);
    }
    else
    {
        g_edit_is_xml = 0;
        g_doc_state = DOC_STATE_IDLE;
        InvalidateRect(g_hwnd, NULL, FALSE);
        resume_pending_render();
    }

    g_edit_index = -1;
    g_edit_is_xml = 0;
    free(g_edit_result_utf8);
    g_edit_result_utf8 = NULL;
}

/* Editor del run cliccato (doppio click destro).  Mostra la slice di testo
 * del run [run_char_start..run_char_end) ed e' EDITABILE: Applica/Ctrl+Invio
 * accoda JOB_RUN_APPLY_EDIT che chiama ld_run_set_text sullo slot cliccato
 * (passato dall'hit-test, non ricalcolato).  Lo span esistente viene
 * riusato dalla DLL: la formattazione del run e degli altri run resta
 * intatta. */
static void show_run_viewer(int para_index, const ld_run_hit *hit,
                            const char *run_text)
{
    static ACCEL accels[2];
    WCHAR       *wtext;
    RECT         rc_main;
    int          w = 660, h = 300, x, y;
    HWND         hwnd;
    MSG          msg;
    int          quit = 0;

    if (!run_text || para_index < 0 || g_edit_hwnd)
        return;

    ensure_editor_class();

    /* Editabile: stesso template dell'editor paragrafo, ma in modalita'
     * run (commit dedicato, g_edit_run_slot >= 0). */
    g_edit_readonly = 0;
    g_edit_run_slot = hit ? hit->run_index : -1;
    g_edit_index = para_index;
    g_edit_apply = 0;
    free(g_edit_result_utf8);
    g_edit_result_utf8 = NULL;

    GetWindowRect(g_hwnd, &rc_main);
    x = rc_main.left + ((rc_main.right - rc_main.left) - w) / 2;
    y = rc_main.top + ((rc_main.bottom - rc_main.top) - h) / 2;

    g_doc_state = DOC_STATE_EDITOR;
    hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, LD_PARA_EDIT_CLASS,
                           L"Modifica run",
                           WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                           x, y, w, h, g_hwnd, NULL,
                           GetModuleHandle(NULL), NULL);
    if (!hwnd)
    {
        g_edit_readonly = 0;
        g_edit_run_slot = -1;
        g_edit_index = -1;
        g_doc_state = DOC_STATE_IDLE;
        return;
    }

    wtext = u8_to_w(run_text);
    if (wtext)
    {
        SetWindowTextW(g_edit_ctl_xml, wtext);
        free(wtext);
    }

    /* Hint con i valori diagnostici del run. */
    {
        HWND hint = GetDlgItem(hwnd, IDC_PARA_HINT);
        WCHAR hint_w[200];
        wsprintfW(hint_w,
                  L"Run %d del paragrafo %d | offset paragrafo %d, range [%d..%d) | box %d, %d %dx%d | Ctrl+Invio = Applica",
                  hit ? hit->run_index : -1,
                  para_index,
                  hit ? hit->char_offset : -1,
                  hit ? hit->run_char_start : -1,
                  hit ? hit->run_char_end : -1,
                  hit ? hit->x : 0, hit ? hit->y : 0,
                  hit ? hit->w : 0, hit ? hit->h : 0);
        if (hint)
            SetWindowTextW(hint, hint_w);
    }

    /* ESC = Annulla, Ctrl+Invio = Applica (come l'editor del paragrafo). */
    accels[0].fVirt = FVIRTKEY;
    accels[0].key = VK_ESCAPE;
    accels[0].cmd = IDC_PARA_CANCEL;
    accels[1].fVirt = FVIRTKEY | FCONTROL;
    accels[1].key = VK_RETURN;
    accels[1].cmd = IDC_PARA_APPLY;
    g_edit_accel = CreateAcceleratorTableW(accels, 2);

    EnableWindow(g_hwnd, FALSE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    SetFocus(g_edit_ctl_xml);

    for (;;)
    {
        int r = GetMessageW(&msg, NULL, 0, 0);
        if (r == 0)
        {
            quit = 1;
            break;
        }
        if (r == -1)
            break;
        if (!g_edit_accel || !TranslateAcceleratorW(hwnd, g_edit_accel, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_edit_hwnd)
            break;
    }

    if (g_edit_hwnd)
    {
        g_edit_apply = 0;
        DestroyWindow(g_edit_hwnd);
        g_edit_hwnd = NULL;
    }
    if (g_edit_accel)
    {
        DestroyAcceleratorTable(g_edit_accel);
        g_edit_accel = NULL;
    }

    g_edit_readonly = 0;
    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);

    if (quit)
    {
        g_edit_index = -1;
        g_edit_run_slot = -1;
        free(g_edit_result_utf8);
        g_edit_result_utf8 = NULL;
        g_doc_state = DOC_STATE_IDLE;
        PostQuitMessage(0);
        return;
    }

    if (g_edit_apply && g_edit_result_utf8)
    {
        /* Applica: sostituzione del testo del run cliccato (la richiesta
         * usa g_edit_index/g_edit_run_slot, che vengono azzerati sotto). */
        request_run_apply(g_edit_result_utf8);
    }
    else
    {
        g_edit_is_xml = 0;
        g_doc_state = DOC_STATE_IDLE;
        InvalidateRect(g_hwnd, NULL, FALSE);
        resume_pending_render();
    }

    g_edit_index = -1;
    g_edit_run_slot = -1;
    g_edit_is_xml = 0;
    free(g_edit_result_utf8);
    g_edit_result_utf8 = NULL;
}
