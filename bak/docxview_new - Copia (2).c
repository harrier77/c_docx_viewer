/*
 * docxview_new.c - Visualizzatore DOCX diretto con litedocx.dll (Win32 + GDI, C puro, TCC).
 *
 * Utilizza direttamente litedocx.dll per aprire e renderizzare file .docx senza
 * generare stringhe HTML intermedie.
 */
#include "litedocx_c.h"

#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>





#define PAGE_MAX_W       960
#define PAGE_DESK_MARGIN 24
#define PAGE_TOP_GAP     20

static HWND   g_hwnd;
static HDC    g_tmpdc;
static void  *g_doc;
static int    g_cw = 900, g_ch = 700;
static int    g_scroll;
static int    g_page_w = PAGE_MAX_W;
static int    g_page_x;

static HBRUSH g_br_desk;
static HBRUSH g_br_shadow;
static HBRUSH g_br_border;

static HDC     g_memdc;
static HBITMAP g_membmp;
static HGDIOBJ g_memold;
static int     g_memw, g_memh;

static char      g_path[MAX_PATH];
static char      g_title[300];
static char      g_url[2048];
static char      g_cur[32] = "arrow";

static ld_callbacks g_cb;
static int g_cb_init;

static int page_top(void)
{
    return PAGE_TOP_GAP - g_scroll;
}

static void compute_page(void)
{
    int w = g_cw - 2 * PAGE_DESK_MARGIN;
    if (w > PAGE_MAX_W)
        w = PAGE_MAX_W;
    if (w < 240)
        w = g_cw;
    if (w < 1)
        w = 1;
    g_page_w = w;
    g_page_x = (g_cw - g_page_w) / 2;
    if (g_page_x < 0)
        g_page_x = 0;
}

static WCHAR *u8_to_w(const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    size_t len = strlen((const char *)p), i = 0, o = 0;
    WCHAR *out = (WCHAR *)malloc((len + 1) * sizeof(WCHAR));
    if (!out)
        return NULL;
    while (i < len) {
        unsigned c = p[i++], cp;
        if (c < 0x80) cp = c;
        else if ((c & 0xE0) == 0xC0 && i < len) cp = ((c & 0x1F) << 6) | (p[i++] & 0x3F);
        else if ((c & 0xF0) == 0xE0 && i + 1 < len) {
            cp = ((c & 0x0F) << 12) | ((p[i] & 0x3F) << 6) | (p[i + 1] & 0x3F);
            i += 2;
        } else if ((c & 0xF8) == 0xF0 && i + 2 < len) {
            cp = ((c & 0x07) << 18) | ((p[i] & 0x3F) << 12) |
                 ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
            i += 3;
        } else cp = '?';
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[o++] = (WCHAR)(0xD800 + (cp >> 10));
            out[o++] = (WCHAR)(0xDC00 + (cp & 0x3FF));
        } else {
            out[o++] = (WCHAR)cp;
        }
    }
    out[o] = 0;
    return out;
}

static void *cb_create_font(const char *fam, int size, int bold, int italic,
                            int underline, int strike, lh_font_metrics *fm)
{
    LOGFONTW lf;
    HFONT f;
    TEXTMETRICW tm;
    WCHAR *w;

    ZeroMemory(&lf, sizeof(lf));
    w = u8_to_w(fam && fam[0] ? fam : "Aptos");
    if (w) {
        lstrcpynW(lf.lfFaceName, w, LF_FACESIZE);
        free(w);
    }
    if (size < 6) size = 6;
    lf.lfHeight = -size;
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = (BYTE)(italic ? 1 : 0);
    lf.lfUnderline = (BYTE)(underline ? 1 : 0);
    lf.lfStrikeOut = (BYTE)(strike ? 1 : 0);
    lf.lfCharSet = DEFAULT_CHARSET;
    f = CreateFontIndirectW(&lf);

    if (fm && f && g_tmpdc) {
        HGDIOBJ old = SelectObject(g_tmpdc, f);
        ZeroMemory(&tm, sizeof(tm));
        GetTextMetricsW(g_tmpdc, &tm);
        SelectObject(g_tmpdc, old);
        fm->font_size = size;
        fm->height = tm.tmHeight;
        fm->ascent = tm.tmAscent;
        fm->descent = tm.tmDescent;
        fm->x_height = tm.tmHeight / 2;
        fm->ch_width = tm.tmAveCharWidth;
        fm->draw_spaces = (italic || underline || strike) ? 1 : 0;
        fm->sub_shift = size / 5;
        fm->super_shift = size / 3;
    }
    return f;
}

static void cb_delete_font(void *font)
{
    if (font)
        DeleteObject((HFONT)font);
}

static int cb_text_width(const char *text, void *font)
{
    SIZE sz;
    HDC dc = g_tmpdc;
    if (!dc || !text || !text[0])
        return 0;
    if (font)
        SelectObject(dc, (HFONT)font);
    {
        WCHAR *w = u8_to_w(text);
        if (w) {
            GetTextExtentPoint32W(dc, w, lstrlenW(w), &sz);
            free(w);
        } else {
            sz.cx = 0;
        }
    }
    return sz.cx;
}

static void cb_draw_text(void *hdc, const char *text, void *font, unsigned rgb, int x, int y, int w, int h)
{
    HDC dc = (HDC)hdc;
    if (!dc || !text || !text[0])
        return;
    if (font)
        SelectObject(dc, (HFONT)font);
    SetTextColor(dc, RGB(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)));
    SetBkMode(dc, TRANSPARENT);
    {
        WCHAR *w_str = u8_to_w(text);
        if (w_str) {
            TextOutW(dc, x, y, w_str, lstrlenW(w_str));
            free(w_str);
        }
    }
}

static void cb_draw_solid_fill(void *hdc, int x, int y, int w, int h, unsigned rgb)
{
    HDC dc = (HDC)hdc;
    RECT rc;
    HBRUSH br;
    rc.left = x;
    rc.top = y;
    rc.right = x + w;
    rc.bottom = y + h;
    br = CreateSolidBrush(RGB(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)));
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void cb_draw_borders(void *hdc, int x, int y, int w, int h, int left_w, int top_w, int right_w, int bottom_w,
                         unsigned left_rgb, unsigned top_rgb, unsigned right_rgb, unsigned bottom_rgb)
{
    if (left_w > 0) {
        cb_draw_solid_fill(hdc, x, y, left_w, h, left_rgb);
    }
    if (top_w > 0) {
        cb_draw_solid_fill(hdc, x, y, w, top_w, top_rgb);
    }
    if (right_w > 0) {
        cb_draw_solid_fill(hdc, x + w - right_w, y, right_w, h, right_rgb);
    }
    if (bottom_w > 0) {
        cb_draw_solid_fill(hdc, x, y + h - bottom_w, w, bottom_w, bottom_rgb);
    }
}

static void cb_set_caption(const char *caption) {}
static void cb_set_base_url(const char *url) {}
static void cb_link(const char *url, const char *title) {}
static void cb_on_anchor_click(const char *url)
{
    if (url && url[0])
        MessageBoxA(g_hwnd, url, "Link Clicked", MB_OK);
}
static void cb_set_cursor(const char *cursor)
{
    lstrcpynA(g_cur, cursor ? cursor : "arrow", sizeof(g_cur));
}

static void init_callbacks(void)
{
    if (g_cb_init) return;
    ZeroMemory(&g_cb, sizeof(g_cb));
    g_cb.lh.create_font        = cb_create_font;
    g_cb.lh.delete_font        = cb_delete_font;
    g_cb.lh.text_width         = cb_text_width;
    g_cb.lh.draw_text          = cb_draw_text;
    g_cb.lh.draw_solid_fill    = cb_draw_solid_fill;
    g_cb.lh.draw_borders       = cb_draw_borders;
    g_cb.lh.set_caption        = cb_set_caption;
    g_cb.lh.on_anchor_click    = cb_on_anchor_click;
    g_cb.lh.set_cursor         = cb_set_cursor;
    g_cb_init = 1;
}

static void update_scrollbars(void)
{
    SCROLLINFO si;
    int doc_h = g_doc ? ld_height(g_doc) : 0;
    int page_content_h = doc_h + 120;
    int max_s = page_content_h - g_ch;
    if (max_s < 0) max_s = 0;
    if (g_scroll > max_s) g_scroll = max_s;

    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = page_content_h > 0 ? page_content_h - 1 : 0;
    si.nPage = g_ch > 0 ? g_ch : 1;
    si.nPos = g_scroll;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
}

/* --------------------------- async load --------------------------- */

#define WM_APP_DOC_READY  (WM_APP + 1)
#define WM_APP_LOAD_PROGRESS (WM_APP + 2)

static char g_load_path[MAX_PATH];
static HANDLE g_load_thread = NULL;
static DWORD  g_load_thread_id = 0;
static volatile LONG g_load_cancel = 0;

static DWORD WINAPI LoadThreadProc(LPVOID arg)
{
    (void)arg;
    const char *path = g_load_path;
    
    void *doc = ld_open_file(path, &g_cb, g_page_w, g_ch);
    if (g_load_cancel) {
        if (doc) ld_destroy(doc);
        return 0;
    }
    if (doc) {
        ld_render(doc, g_page_w);
    }
    if (g_load_cancel) {
        if (doc) ld_destroy(doc);
        return 0;
    }
    PostMessageA(g_hwnd, WM_APP_DOC_READY, 0, (LPARAM)doc);
    return 0;
}

static void start_async_load(const char *path)
{
    if (g_load_thread) {
        g_load_cancel = 1;
        WaitForSingleObject(g_load_thread, 2000);
        CloseHandle(g_load_thread);
        g_load_thread = NULL;
    }
    g_load_cancel = 0;
    lstrcpynA(g_load_path, path, sizeof(g_load_path));
    
    init_callbacks();
    compute_page();
    
    g_load_thread = CreateThread(NULL, 0, LoadThreadProc, NULL, 0, &g_load_thread_id);
    if (!g_load_thread) {
        MessageBoxA(g_hwnd, "Impossibile creare thread di caricamento", "Errore", MB_OK | MB_ICONERROR);
    }
}

static void on_doc_ready(void *doc)
{
    if (g_load_thread) {
        CloseHandle(g_load_thread);
        g_load_thread = NULL;
    }
    
    if (g_doc) {
        ld_destroy(g_doc);
    }
    g_doc = doc;
    
    if (g_doc) {
        lstrcpynA(g_path, g_load_path, sizeof(g_path));
        const char *p = strrchr(g_load_path, '\\');
        if (!p) p = strrchr(g_load_path, '/');
        sprintf(g_title, "docxview_new - %s", p ? p + 1 : g_load_path);
    } else {
        sprintf(g_title, "docxview_new - [Errore apertura file]");
        g_path[0] = 0;
    }
    SetWindowTextA(g_hwnd, g_title);
    g_scroll = 0;
    update_scrollbars();
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static void load_docx_file(const char *path)
{
    if (!path || !path[0]) return;
    start_async_load(path);
    /* UI mostra titolo "Caricamento..." subito */
    SetWindowTextA(g_hwnd, "docxview_new - Caricamento...");
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_tmpdc = GetDC(NULL);
        g_br_desk = CreateSolidBrush(RGB(240, 240, 242));
        g_br_shadow = CreateSolidBrush(RGB(190, 190, 195));
        g_br_border = CreateSolidBrush(RGB(210, 210, 215));
        break;

    case WM_SIZE:
        g_cw = LOWORD(lp);
        g_ch = HIWORD(lp);
        {
            HDC hdc = GetDC(hwnd);
            if (g_memold) SelectObject(g_memdc, g_memold);
            if (g_membmp) DeleteObject(g_membmp);
            if (g_memdc) DeleteDC(g_memdc);

            g_memdc = CreateCompatibleDC(hdc);
            g_membmp = CreateCompatibleBitmap(hdc, g_cw > 0 ? g_cw : 1, g_ch > 0 ? g_ch : 1);
            g_memold = SelectObject(g_memdc, g_membmp);
            g_memw = g_cw;
            g_memh = g_ch;
            ReleaseDC(hwnd, hdc);
        }
        compute_page();
        if (g_doc) {
            ld_render(g_doc, g_page_w);
        }
        update_scrollbars();
        InvalidateRect(hwnd, NULL, FALSE);
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc_all = {0, 0, g_cw, g_ch};

        FillRect(g_memdc, &rc_all, g_br_desk);

        if (g_doc) {
            int pt = page_top();
            int doc_h = ld_height(g_doc);
            int page_h = doc_h + 120;
            RECT rc_shd = {g_page_x + 4, pt + 4, g_page_x + g_page_w + 4, pt + page_h + 4};
            RECT rc_page = {g_page_x, pt, g_page_x + g_page_w, pt + page_h};

            FillRect(g_memdc, &rc_shd, g_br_shadow);
            FillRect(g_memdc, &rc_page, (HBRUSH)GetStockObject(WHITE_BRUSH));
            FrameRect(g_memdc, &rc_page, g_br_border);

            ld_draw(g_doc, g_memdc, g_page_x + 64, pt + 60,
                    rc_page.left, rc_page.top, rc_page.right - rc_page.left, rc_page.bottom - rc_page.top);
        }

        BitBlt(hdc, 0, 0, g_cw, g_ch, g_memdc, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_VSCROLL: {
        SCROLLINFO si;
        int old_s = g_scroll;
        int doc_h = g_doc ? ld_height(g_doc) : 0;
        int max_s = (doc_h + 120) - g_ch;
        if (max_s < 0) max_s = 0;

        switch (LOWORD(wp)) {
        case SB_LINEUP:   g_scroll -= 30; break;
        case SB_LINEDOWN: g_scroll += 30; break;
        case SB_PAGEUP:   g_scroll -= g_ch / 2; break;
        case SB_PAGEDOWN: g_scroll += g_ch / 2; break;
        case SB_THUMBTRACK:
            ZeroMemory(&si, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(hwnd, SB_VERT, &si);
            g_scroll = si.nTrackPos;
            break;
        }
        if (g_scroll < 0) g_scroll = 0;
        if (g_scroll > max_s) g_scroll = max_s;
        if (g_scroll != old_s) {
            SetScrollPos(hwnd, SB_VERT, g_scroll, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int doc_h = g_doc ? ld_height(g_doc) : 0;
        int max_s = (doc_h + 120) - g_ch;
        if (max_s < 0) max_s = 0;
        g_scroll -= delta / 3;
        if (g_scroll < 0) g_scroll = 0;
        if (g_scroll > max_s) g_scroll = max_s;
        SetScrollPos(hwnd, SB_VERT, g_scroll, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }

    case WM_APP_DOC_READY:
        on_doc_ready((void *)lp);
        return 0;

    case WM_COMMAND:
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
            return 0;
        }
        case 101:
            if (g_path[0]) load_docx_file(g_path);
            return 0;
        case 102:
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case 103:
            MessageBoxA(hwnd, "docxview_new - Visualizzatore DOCX diretto con litedocx.dll.", "Informazioni", MB_OK);
            return 0;
        }
        break;

    case WM_DESTROY:
        if (g_load_thread) {
            g_load_cancel = 1;
            WaitForSingleObject(g_load_thread, 2000);
            CloseHandle(g_load_thread);
            g_load_thread = NULL;
        }
        if (g_doc) {
            ld_destroy(g_doc);
            g_doc = NULL;
        }
        if (g_memold) SelectObject(g_memdc, g_memold);
        if (g_membmp) DeleteObject(g_membmp);
        if (g_memdc) DeleteDC(g_memdc);
        if (g_br_desk) DeleteObject(g_br_desk);
        if (g_br_shadow) DeleteObject(g_br_shadow);
        if (g_br_border) DeleteObject(g_br_border);
        if (g_tmpdc) ReleaseDC(NULL, g_tmpdc);
        PostQuitMessage(0);
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

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DocxViewNewClass";
    RegisterClassExW(&wc);

    menu = CreateMenu();
    filemenu = CreatePopupMenu();
    AppendMenuA(filemenu, MF_STRING, 100, "&Apri...\tCtrl+O");
    AppendMenuA(filemenu, MF_STRING, 101, "&Ricarica\tF5");
    AppendMenuA(filemenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(filemenu, MF_STRING, 102, "&Esci");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)filemenu, "&File");
    AppendMenuA(menu, MF_STRING, 103, "&?");

    hwnd = CreateWindowExW(0, L"DocxViewNewClass", L"docxview_new",
                           WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                           CW_USEDEFAULT, CW_USEDEFAULT, g_cw, g_ch,
                           NULL, menu, inst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show);
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
