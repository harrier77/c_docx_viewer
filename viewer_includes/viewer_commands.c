/*
 * viewer_commands.c - comandi del menu File (apri, ricarica, salva, esci, ?).
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

/* Il documento puo' essere salvato solo quando nessun indice di
 * paragrafo e' in uso e nessuna modifica e' in volo. */
static int can_switch_document(void)
{
    return g_doc_state != DOC_STATE_HITTEST &&
           g_doc_state != DOC_STATE_EDITOR &&
           g_doc_state != DOC_STATE_APPLYING;
}

static void viewer_on_command(HWND hwnd, WPARAM wp)
{
    switch (LOWORD(wp)) {
    case 100: {
        OPENFILENAMEA ofn;
        char szFile[MAX_PATH] = "";
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hInstance = GetModuleHandle(NULL);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = "Documenti DOCX (*.docx)\0*.docx\0Tutti i file (*.*)\0*.*\0";
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        if (GetOpenFileNameA(&ofn)) {
            load_docx_file(szFile);
        }
        return;
    }
    case 101:
        if (g_path[0]) load_docx_file(g_path);
        return;
    case 102:
        PostMessageA(hwnd, WM_CLOSE, 0, 0);
        return;
    case 103:
        MessageBoxA(hwnd, "d2dview - Visualizzatore DOCX con renderer Direct2D (litedocx.dll).", "Informazioni", MB_OK);
        return;
    case 104:
        if (!can_switch_document())
        {
            MessageBoxA(hwnd,
                        "Attendere il completamento dell'operazione sul paragrafo.",
                        "d2dview", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (g_path[0]) save_docx_file(g_path);
        return;
    }
}