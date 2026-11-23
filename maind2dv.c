/*
 * maind2dv.c - Visualizzatore DOCX con litedocx.dll e renderer Direct2D
 * (Win32 + Direct2D/DirectWrite, C puro, MinGW GCC).
 *
 * Punto di ingresso del viewer. Contiene lo stato condiviso, i tipi, il
 * message loop e il wnd_proc; le funzioni sono organizzate per argomento
 * nei file viewer_*.c dentro viewer_includes/ (build unity, un solo TU).
 *
 * Variante di docxview_new.c che usa SOLO il ramo Direct2D: nessun
 * RENDERER_GDI, nessun selettore di renderer (variabile d'ambiente o
 * argomento --renderer=), nessun fallback GDI.
 * Lo zoom e' puro zoom del renderer: il layout resta a zoom 1.0 e non
 * viene mai rifatto al cambio di zoom.
 */
#include "litedocx_c.h"

#include <windows.h>
#include <commctrl.h>
#undef WINAPI_FAMILY_PARTITION
/* COBJMACROS rende pubblici i metodi delle interfacce COM (ID2D1*, IDWrite*, …) */
#define COBJMACROS
#include <dwrite.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "layout.h"
#include "renderer.h"
#include "text_utf8.h"
#include "document.h"

/* Accesso allo stato di layout (vedi func_gdi/layout.h). */
#define LAY layout_get()
/* Alias storico: l'implementazione vive in func_gdi/text_utf8.c. */
#define u8_to_w utf8_to_wide



/* PAGE_MAX_W, PAGE_DESK_MARGIN, PAGE_TOP_GAP, PAGE_SHADOW_OFF
 * sono definiti in func_gdi/layout.h. */


/* ------------------------------------------------------------------
 * Moduli del viewer, inclusi nello stesso translation unit (build unity).
 * Ordine di dipendenza: stato -> log -> font -> callback -> job -> render
 * -> lifecycle -> paint -> input -> comandi.
 * ------------------------------------------------------------------ */
#include "viewer_includes/viewer_state.c"
#include "viewer_includes/viewer_log.c"
#include "viewer_includes/viewer_fonts.c"
#include "viewer_includes/viewer_callbacks.c"
#include "viewer_includes/viewer_jobs.c"
#include "viewer_includes/viewer_render.c"
#include "viewer_includes/viewer_lifecycle.c"
#include "viewer_includes/viewer_paint.c"
#include "viewer_includes/viewer_input.c"
#include "viewer_includes/viewer_commands.c"
#include "viewer_includes/viewer_editor_ui.c"

/* Handlers per messaggi asincroni (Fase 4) */
static void handle_hit_result(HitResult *r)
{
    fprintf(stderr, "[HIT_HANDLER] handle_hit_result called r=%p g_doc=%p g_load_gen=%u g_layout_epoch=%d g_doc_state=%d\n",
            r, g_doc, g_load_gen, g_layout_epoch, g_doc_state);
    if (!r) {
        fprintf(stderr, "[HIT_HANDLER] r is NULL\n");
        return;
    }
    fprintf(stderr, "[HIT_HANDLER] r->doc=%p r->gen=%u r->epoch=%d r->hit=%d r->index=%d r->text=%p\n",
            r->doc, r->gen, r->epoch, r->hit, r->index, r->text);
    if (r->doc != g_doc || r->gen != g_load_gen || r->epoch != g_layout_epoch)
    {
        fprintf(stderr, "[HIT_HANDLER] STALE: doc mismatch (r->doc=%p vs g_doc=%p) gen mismatch (%u vs %u) epoch mismatch (%d vs %d)\n",
                r->doc, g_doc, r->gen, g_load_gen, r->epoch, g_layout_epoch);
        if (r->text) ld_free_string(r->text);
        free(r);
        if (g_doc_state == DOC_STATE_HITTEST)
            g_doc_state = DOC_STATE_IDLE;
        return;
    }
    if (g_doc_state == DOC_STATE_HITTEST)
        g_doc_state = DOC_STATE_IDLE;
    
    if (r->hit && r->index >= 0)
    {
        fprintf(stderr, "[HIT] hit para=%d, calling begin_paragraph_edit_at_index\n", r->index);
        if (r->text) ld_free_string(r->text);
        /* Per ora apriamo l'editor direttamente */
        begin_paragraph_edit_at_index(r->index);
    }
    else
    {
        fprintf(stderr, "[HIT] NO HIT (hit=%d index=%d)\n", r->hit, r->index);
        if (r->text) ld_free_string(r->text);
        g_doc_state = DOC_STATE_IDLE;
    }
    free(r);
}

static void handle_run_hit_result(RunHitResult *r)
{
    if (!r) return;
    if (r->doc != g_doc || r->gen != g_load_gen || r->epoch != g_layout_epoch)
    {
        free(r);
        if (g_doc_state == DOC_STATE_HITTEST)
            g_doc_state = DOC_STATE_IDLE;
        return;
    }
    if (g_doc_state == DOC_STATE_HITTEST)
        g_doc_state = DOC_STATE_IDLE;

    log_msg("[RUN-HIT] hit=%d para=%d run=%d off=%d [%d..%d) inRun=%d box=%d,%d %dx%d\n",
            r->hit, r->info.para_index, r->info.run_index, r->info.char_offset,
            r->info.run_char_start, r->info.run_char_end, r->info.run_char_offset,
            r->info.x, r->info.y, r->info.w, r->info.h);

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
    }
    free(r);
}

static void handle_para_xml_ready(ParaXmlResult *r)
{
    if (!r) return;
    if (r->doc != g_doc || r->gen != g_load_gen || r->epoch != g_layout_epoch)
    {
        if (r->xml) ld_free_string(r->xml);
        free(r);
        return;
    }
    if (r->ok && r->xml)
    {
        fprintf(stderr, "[XML] para_xml ready idx=%d\n", r->index);
        ld_free_string(r->xml);
    }
    free(r);
}

static void handle_edit_applied(EditResult *r)
{
    if (!r) return;
    if (r->doc != g_doc || r->gen != g_load_gen)
    {
        free(r);
        return;
    }
    
    if (r->ok)
    {
        fprintf(stderr, "[EDIT] Applicato con successo, richiedo re-render\n");
        ld_invalidate_font_cache(g_doc);
        ld_clear_page_cache(g_doc);
        g_layout_complete = 0;
        g_last_rendered_page = -1;
        g_first_rendered_page = -1;
        if (g_layout_cache)
            layout_cache_invalidate(g_layout_cache);
        request_render(layout_content_width());
    }
    else
    {
        fprintf(stderr, "[EDIT] Errore applicazione: %s\n", r->error);
        g_doc_state = DOC_STATE_IDLE;
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
    free(r);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        return viewer_on_create(hwnd);

    case WM_SIZE:
        viewer_on_size(hwnd, wp, lp);
        break;

    case WM_PAINT:
        viewer_paint(hwnd);
        break;

    /* ---- Pan con tasto centrale (o trascinamento col tasto destro non
     * gestito altrove): sposta il contenuto via scroll_x/scroll, cioe' in
     * coordinate di LAYOUT. Nessun re-render: clip, scrollbar e ancoraggio
     * dello zoom restano coerenti, e vale identico per GDI e D2D. ---- */
    case WM_SETCURSOR:
        return viewer_on_setcursor(hwnd, wp, lp);

    case WM_MBUTTONDOWN:
        return viewer_on_mbuttondown(hwnd, lp);

    case WM_MOUSEMOVE:
        return viewer_on_mousemove(hwnd, wp, lp);

    case WM_MBUTTONUP:
        return viewer_on_mbuttonup(hwnd);

    case WM_CAPTURECHANGED:
        viewer_on_capturechanged();
        return 0;

    case WM_VSCROLL:
        viewer_on_vscroll(hwnd, wp);
        break;

    case WM_MOUSEWHEEL:
        viewer_on_mousewheel(hwnd, wp, lp);
        break;

    /* WM_LBUTTONDBLCLK: doppio click sinistro su paragrafo -> apre editor OOXML.
     * Converte coordinate schermo (pixel) -> coordinate pagina litedocx (layout a zoom 1.0).
     * litedocx riceve visual padding via ld_set_visual_padding(18,6) e il suo hit-test
     * usa coordinate relative alla pagina INCLUSO il visual padding.
     * FIX: senza CS_DBLCLKS nella window class, WM_LBUTTONDBLCLK non veniva mai generato. */
    case WM_LBUTTONDBLCLK: {
        fprintf(stderr, "[DBLCLK] WM_LBUTTONDBLCLK received, g_doc=%p g_doc_state=%d\n", g_doc, g_doc_state);
        if (g_doc && g_doc_state == DOC_STATE_IDLE) {
            int screen_x = (short)LOWORD(lp);
            int screen_y = (short)HIWORD(lp);
            
            /* Converti coordinate schermo -> coordinate pagina (layout a zoom 1.0).
             * litedocx hit-test si aspetta coordinate pagina INCLUSO visual padding (18,6). */
            sync_view_scale();
            
            RECT rc_page, rc_shadow;
            layout_page_rect(g_doc_h, &rc_page, &rc_shadow);
            
            double inv_zoom = (g_zoom > 0.0f) ? (1.0 / g_zoom) : 1.0;
            int page_x = (int)(screen_x * inv_zoom) - rc_page.left;
            int page_y = (int)(screen_y * inv_zoom) - rc_page.top;
            
            fprintf(stderr, "[DBLCLK] screen=(%d,%d) page=(%d,%d) zoom=%.2f page_rect=(%ld,%ld)\n",
                    screen_x, screen_y, page_x, page_y, g_zoom,
                    rc_page.left, rc_page.top);
            
            /* Prova 1: coordinate pagina (include visual padding per litedocx) */
            ld_paragraph_hit hit_sync;
            char *text_sync = NULL;
            int got_sync = ld_paragraph_hit_test(g_doc, page_x, page_y, &hit_sync, &text_sync);
            fprintf(stderr, "[DBLCLK][SYNC page] got=%d hit.index=%d text=%p\n",
                    got_sync, got_sync ? hit_sync.index : -1, text_sync);
            if (text_sync) ld_free_string(text_sync);
            
            /* Prova 2: coordinate pagina - visual padding */
            int doc_x = page_x - LAYOUT_VISUAL_PADDING_X;
            int doc_y = page_y - LAYOUT_VISUAL_MARGIN_Y;
            int got_sync2 = ld_paragraph_hit_test(g_doc, doc_x, doc_y, &hit_sync, &text_sync);
            fprintf(stderr, "[DBLCLK][SYNC page-vpad] got=%d hit.index=%d text=%p\n",
                    got_sync2, got_sync2 ? hit_sync.index : -1, text_sync);
            if (text_sync) ld_free_string(text_sync);
            
            /* Usa coordinate pagina per l'async (litedocx si aspetta questo) */
            request_hit_test(page_x, page_y);
        } else {
            fprintf(stderr, "[DBLCLK] IGNORED: g_doc=%p g_doc_state=%d (expected 0=IDLE)\n", g_doc, g_doc_state);
        }
        return 0;
    }

    case WM_APP_DOC_READY:
        apply_loaded_doc((unsigned)wp, (void *)lp);
        return 0;

    case WM_APP_RENDER_DONE:
        on_render_done((void *)lp, (int)wp);
        return 0;

    case WM_APP_HIT_RESULT:
        handle_hit_result((HitResult *)lp);
        return 0;

    case WM_APP_RUN_HIT_RESULT:
        handle_run_hit_result((RunHitResult *)lp);
        return 0;

    case WM_APP_PARA_XML_READY:
        handle_para_xml_ready((ParaXmlResult *)lp);
        return 0;

    case WM_APP_EDIT_APPLIED:
        handle_edit_applied((EditResult *)lp);
        return 0;

    case WM_COMMAND:
        viewer_on_command(hwnd, wp);
        return 0;

    case WM_DESTROY:
        viewer_on_destroy();
        break;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    HMENU menu, filemenu;

    fprintf(stderr, "[RENDERER] kind=d2d\n");

    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_TAB_CLASSES;
        InitCommonControlsEx(&icc);
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    /* CS_DBLCLKS: abilita WM_LBUTTONDBLCLK per doppio click su paragrafo -> editor OOXML. */
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"D2DViewClass";
    RegisterClassExW(&wc);

    if (!start_worker())
        MessageBoxA(NULL,
                    "Impossibile avviare il thread di caricamento.\n"
                    "L'apertura dei file avverra' in modo sincrono.",
                    "Avviso", MB_OK | MB_ICONWARNING);

    menu = CreateMenu();
    filemenu = CreatePopupMenu();
    AppendMenuA(filemenu, MF_STRING, 100, "&Apri...\tCtrl+O");
    AppendMenuA(filemenu, MF_STRING, 101, "&Ricarica\tF5");
    AppendMenuA(filemenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(filemenu, MF_STRING, 102, "&Esci");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)filemenu, "&File");
    AppendMenuA(menu, MF_STRING, 103, "&?");

    layout_init(900, 700);

    hwnd = CreateWindowExW(0, L"D2DViewClass", L"d2dview",
                           WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                           CW_USEDEFAULT, CW_USEDEFAULT, LAY->cw, LAY->ch,
                           NULL, menu, inst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    if (cmd && cmd[0]) {
        char path[MAX_PATH];
        char *p = cmd;
        if (*p == '"') {
            p++;
            lstrcpynA(path, p, sizeof(path));
            char *e = strchr(path, '"');
            if (e) *e = 0;
        } else {
            lstrcpynA(path, p, sizeof(path));
        }
        load_docx_file(path);
    }

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}