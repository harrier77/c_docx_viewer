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

    case WM_APP_DOC_READY:
        apply_loaded_doc((unsigned)wp, (void *)lp);
        return 0;

    case WM_APP_RENDER_DONE:
        on_render_done((void *)lp, (int)wp);
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
    wc.style = CS_HREDRAW | CS_VREDRAW;
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