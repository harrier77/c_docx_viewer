/*
 * docxview.c - visualizzatore DOCX con litehtml (Win32 + GDI, C puro, TCC).
 *
 * Legge un file .docx con la libreria docx2html (docx.h), converte il
 * contenuto in HTML con CSS incorporato e lo disegna in una finestra usando
 * litehtml_c.dll, esattamente come fa sunburst.c per il pannello Inserimento.
 *
 * Compilazione:  build_viewer.bat
 * Uso:           docxview.exe [file.docx]
 *
 * Funzioni: apertura file (menu/argomento), doppio buffer GDI, scroll con
 * rotella, scrollbar, tastiera, link cliccabili, ridimensionamento con
 * re-layout di litehtml.
 */
#include "docx.h"
#include "litehtml_c.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LPOFNHOOKPROC
typedef void *LPOFNHOOKPROC;
#endif

/* tcc non distribuisce commdlg.h: struttura e prototype dichiarati qui.
 * Layout identico a OPENFILENAMEA su Win64. */
typedef struct docx_openfilenamea {
    DWORD        lStructSize;
    HWND         hwndOwner;
    HINSTANCE    hInstance;
    LPCSTR       lpstrFilter;
    LPSTR        lpstrCustomFilter;
    DWORD        nMaxCustFilter;
    DWORD        nFilterIndex;
    LPSTR        lpstrFile;
    DWORD        nMaxFile;
    LPSTR        lpstrFileTitle;
    DWORD        nMaxFileTitle;
    LPCSTR       lpstrInitialDir;
    LPCSTR       lpstrTitle;
    DWORD        Flags;
    WORD         nFileOffset;
    WORD         nFileExtension;
    LPCSTR       lpstrDefExt;
    LPARAM       lCustData;
    LPOFNHOOKPROC lpfnHook;
    LPCSTR       lpTemplateName;
    void        *pvReserved;
    DWORD        dwReserved;
    DWORD        FlagsEx;
} docx_ofna;

__declspec(dllimport) BOOL WINAPI GetOpenFileNameA(docx_ofna *ofn);

#define OFN_READONLY       0x00000001
#define OFN_HIDEREADONLY   0x00000004
#define OFN_PATHMUSTEXIST  0x00000800
#define OFN_FILEMUSTEXIST  0x00001000
#define OFN_EXPLORER       0x00080000

/* ------------------------- litehtml a runtime ------------------------- */

typedef void *(*P_create)(const char *, const lh_callbacks *, int, int);
typedef int   (*P_render)(void *, int);
typedef void  (*P_draw)(void *, void *, int, int, int, int, int, int);
typedef int   (*P_mouse)(void *, int, int, int, int);
typedef int   (*P_mleave)(void *);
typedef int   (*P_height)(void *);
typedef int   (*P_width)(void *);
typedef void  (*P_destroy)(void *);
typedef int   (*P_findbox)(void *, const char *, int *, int *, int *, int *);

static HMODULE g_dll;
static P_create  p_create;
static P_render  p_render;
static P_draw    p_draw;
static P_mouse   p_over, p_down, p_up;
static P_mleave  p_leave;
static P_height  p_height;
static P_width   p_width;
static P_destroy p_destroy;
static P_findbox p_findbox;

/* ---------------------------- stato app ---------------------------- */

#define WM_APP_OPENURL  (WM_APP + 1)
#define WM_APP_SCROLLTO (WM_APP + 2)

/* Aspetto "foglio": pagina larga centrata su scrivania neutra. */
#define PAGE_MAX_W       960
#define PAGE_DESK_MARGIN 24
#define PAGE_TOP_GAP     20

static HWND   g_hwnd;
static HDC    g_tmpdc;          /* DC schermo per metriche font */
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

static docx_html g_html;        /* HTML corrente (posseduto) */
static char      g_path[MAX_PATH];
static char      g_title[300];
static char      g_url[2048];
static char      g_cur[32] = "arrow";

static lh_callbacks g_cb;
static int g_cb_init;

/* Margini del foglio (un po' più larghi del testo, come un documento Word). */
static const char g_page_css[] = "body{padding:60px 64px;}\n";

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
        w = g_cw; /* finestra molto stretta: niente margini */
    if (w < 1)
        w = 1;
    g_page_w = w;
    g_page_x = (g_cw - g_page_w) / 2;
    if (g_page_x < 0)
        g_page_x = 0;
}

/* --------------------------- utility UTF-8 --------------------------- */

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

/* ----------------------- callback litehtml ----------------------- */

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
    HGDIOBJ old;
    WCHAR *w = u8_to_w(text);

    ZeroMemory(&sz, sizeof(sz));
    if (g_tmpdc && font) {
        old = SelectObject(g_tmpdc, (HFONT)font);
        if (w) {
            GetTextExtentPoint32W(g_tmpdc, w, lstrlenW(w), &sz);
            free(w);
        }
        SelectObject(g_tmpdc, old);
    }
    return sz.cx;
}

static void cb_draw_text(void *hdc, const char *text, void *font, unsigned rgb,
                         int x, int y, int w, int h)
{
    HDC dc = (HDC)hdc;
    RECT rc;
    HGDIOBJ old;
    WCHAR *wt = u8_to_w(text);

    rc.left = x; rc.top = y; rc.right = x + w; rc.bottom = y + h;
    old = SelectObject(dc, (HFONT)font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, (COLORREF)rgb);
    if (wt) {
        DrawTextW(dc, wt, -1, &rc, DT_SINGLELINE | DT_NOPREFIX | DT_BOTTOM | DT_NOCLIP);
        free(wt);
    }
    SelectObject(dc, old);
}

static void cb_draw_solid_fill(void *hdc, int x, int y, int w, int h, unsigned rgb)
{
    RECT rc;
    HBRUSH br;
    HGDIOBJ old;

    if (w <= 0 || h <= 0)
        return;
    rc.left = x; rc.top = y; rc.right = x + w; rc.bottom = y + h;
    br = CreateSolidBrush((COLORREF)rgb);
    old = SelectObject((HDC)hdc, br);
    FillRect((HDC)hdc, &rc, br);
    SelectObject((HDC)hdc, old);
    DeleteObject(br);
}

static void cb_draw_borders(void *hdc, int x, int y, int w, int h,
                            int lw, int tw, int rw, int bw,
                            unsigned lc, unsigned tc, unsigned rc_, unsigned bc)
{
    HDC dc = (HDC)hdc;
    HBRUSH br;
    HGDIOBJ old;
    RECT r;

    if (tw > 0) {
        br = CreateSolidBrush((COLORREF)tc); old = SelectObject(dc, br);
        r.left = x; r.top = y; r.right = x + w; r.bottom = y + tw;
        FillRect(dc, &r, br);
        SelectObject(dc, old); DeleteObject(br);
    }
    if (bw > 0) {
        br = CreateSolidBrush((COLORREF)bc); old = SelectObject(dc, br);
        r.left = x; r.top = y + h - bw; r.right = x + w; r.bottom = y + h;
        FillRect(dc, &r, br);
        SelectObject(dc, old); DeleteObject(br);
    }
    if (lw > 0) {
        br = CreateSolidBrush((COLORREF)lc); old = SelectObject(dc, br);
        r.left = x; r.top = y; r.right = x + lw; r.bottom = y + h;
        FillRect(dc, &r, br);
        SelectObject(dc, old); DeleteObject(br);
    }
    if (rw > 0) {
        br = CreateSolidBrush((COLORREF)rc_); old = SelectObject(dc, br);
        r.left = x + w - rw; r.top = y; r.right = x + w; r.bottom = y + h;
        FillRect(dc, &r, br);
        SelectObject(dc, old); DeleteObject(br);
    }
}

static void cb_set_caption(const char *caption)
{
    (void)caption; /* il titolo e' gestito da load_docx() */
}

static void cb_on_anchor_click(const char *url)
{
    if (!url || !url[0])
        return;

    if (url[0] == '#') {
        int x = 0, y = 0, w = 0, h = 0;
        if (p_findbox && g_doc && p_findbox(g_doc, url + 1, &x, &y, &w, &h)) {
            PostMessageA(g_hwnd, WM_APP_SCROLLTO, 0, (LPARAM)(y + PAGE_TOP_GAP - 24));
        }
        return;
    }
    lstrcpynA(g_url, url, sizeof(g_url));
    if (g_hwnd)
        PostMessageA(g_hwnd, WM_APP_OPENURL, 0, 0);
}

static void cb_set_cursor(const char *cursor)
{
    const char *c = cursor ? cursor : "auto";
    lstrcpynA(g_cur, c, sizeof(g_cur));
    if (strstr(c, "pointer") || strstr(c, "hand"))
        SetCursor(LoadCursor(NULL, IDC_HAND));
    else if (strstr(c, "text"))
        SetCursor(LoadCursor(NULL, IDC_IBEAM));
    else
        SetCursor(LoadCursor(NULL, IDC_ARROW));
}

static void cb_get_viewport(int *x, int *y, int *w, int *h)
{
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = g_page_w;
    if (h) *h = g_ch;
}

static void cb_get_media_features(lh_media_features *m)
{
    if (!m)
        return;
    m->type = 3; /* screen */
    m->width = g_page_w;
    m->height = g_ch;
    m->device_width = g_page_w;
    m->device_height = g_ch;
    m->color = 8;
    m->color_index = 256;
    m->monochrome = 0;
    m->resolution = g_tmpdc ? GetDeviceCaps(g_tmpdc, LOGPIXELSY) : 96;
}

static void cb_redraw_box(int x, int y, int w, int h)
{
    RECT r;
    if (!g_hwnd)
        return;
    r.left = g_page_x + x;
    r.top = page_top() + y;
    r.right = r.left + w;
    r.bottom = r.top + h;
    InvalidateRect(g_hwnd, &r, FALSE);
}

/* --------------------------- caricamento --------------------------- */

static int dll_load(void)
{
    g_dll = LoadLibraryA("litehtml_c.dll");
    if (!g_dll) g_dll = LoadLibraryA("..\\sunburst\\litehtml_c.dll");
    if (!g_dll) g_dll = LoadLibraryA("..\\litehtml_host\\litehtml_c.dll");
    if (!g_dll) g_dll = LoadLibraryA("C:\\Users\\pr30565\\Desktop\\Pure_C\\sunburst\\litehtml_c.dll");
    if (!g_dll) return 0;

    p_create  = (P_create)  GetProcAddress(g_dll, "lh_create");
    p_render  = (P_render)  GetProcAddress(g_dll, "lh_render");
    p_draw    = (P_draw)    GetProcAddress(g_dll, "lh_draw");
    p_over    = (P_mouse)   GetProcAddress(g_dll, "lh_mouse_over");
    p_down    = (P_mouse)   GetProcAddress(g_dll, "lh_lbutton_down");
    p_up      = (P_mouse)   GetProcAddress(g_dll, "lh_lbutton_up");
    p_leave   = (P_mleave)  GetProcAddress(g_dll, "lh_mouse_leave");
    p_height  = (P_height)  GetProcAddress(g_dll, "lh_height");
    p_width   = (P_width)   GetProcAddress(g_dll, "lh_width");
    p_destroy = (P_destroy) GetProcAddress(g_dll, "lh_destroy");
    p_findbox = (P_findbox) GetProcAddress(g_dll, "lh_find_box");

    return p_create && p_render && p_draw && p_height && p_destroy;
}

static void free_memdc(void)
{
    if (g_memdc) {
        if (g_memold) SelectObject(g_memdc, g_memold);
        if (g_membmp) DeleteObject(g_membmp);
        DeleteDC(g_memdc);
    }
    g_memdc = NULL;
    g_membmp = NULL;
    g_memold = NULL;
    g_memw = g_memh = 0;
}

static void clamp_scroll(void)
{
    int h = (g_doc && p_height) ? p_height(g_doc) : 0;
    int maxo = h + 2 * PAGE_TOP_GAP - g_ch;
    if (maxo < 0) maxo = 0;
    if (g_scroll > maxo) g_scroll = maxo;
    if (g_scroll < 0) g_scroll = 0;
}

static void update_scrollbar(void)
{
    SCROLLINFO si;
    int h = (g_doc && p_height) ? p_height(g_doc) : 0;
    int total = h + 2 * PAGE_TOP_GAP;

    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = total > 0 ? total - 1 : 0;
    si.nPage = (UINT)g_ch;
    si.nPos = g_scroll;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
}

static void destroy_doc(void)
{
    if (g_doc) {
        p_destroy(g_doc);
        g_doc = NULL;
    }
}

static void render_doc(void)
{
    destroy_doc();
    if (!g_html.data)
        return;
    if (!g_cb_init) {
        ZeroMemory(&g_cb, sizeof(g_cb));
        g_cb.create_font = cb_create_font;
        g_cb.delete_font = cb_delete_font;
        g_cb.text_width = cb_text_width;
        g_cb.draw_text = cb_draw_text;
        g_cb.draw_solid_fill = cb_draw_solid_fill;
        g_cb.draw_borders = cb_draw_borders;
        g_cb.set_caption = cb_set_caption;
        g_cb.on_anchor_click = cb_on_anchor_click;
        g_cb.set_cursor = cb_set_cursor;
        g_cb.get_viewport = cb_get_viewport;
        g_cb.get_media_features = cb_get_media_features;
        g_cb.redraw_box = cb_redraw_box;
        g_cb_init = 1;
    }
    compute_page();
    g_doc = p_create(g_html.data, &g_cb, g_page_w, g_ch);
    if (g_doc)
        p_render(g_doc, g_page_w);
    clamp_scroll();
    update_scrollbar();
    if (g_hwnd)
        InvalidateRect(g_hwnd, NULL, FALSE);
}

static void set_window_title(void)
{
    if (!g_hwnd)
        return;
    lstrcpynA(g_title, "docxview - ", sizeof(g_title));
    lstrcatA(g_title, g_path[0] ? g_path : "(nessun file)");
    SetWindowTextA(g_hwnd, g_title);
}

static int load_docx(const char *path)
{
    docx_html h;
    docx_html_opts opts;
    char err[256];

    if (!path || !path[0])
        return 0;

    opts.title = NULL;
    opts.extra_css = g_page_css;
    opts.font_px = 0;
    opts.full_document = 1;

    if (!docx_file_to_html(path, &opts, &h)) {
        lstrcpynA(err, h.error, sizeof(err));
        MessageBoxA(g_hwnd, err, "Errore apertura DOCX", MB_OK | MB_ICONERROR);
        return 0;
    }

    destroy_doc();
    docx_html_free(&g_html);
    g_html = h; /* trasferisce la proprieta' della stringa */
    lstrcpynA(g_path, path, sizeof(g_path));

    render_doc();
    set_window_title();
    return 1;
}

static int pick_file(char *out, int cap)
{
    docx_ofna ofn;
    char file[MAX_PATH];
    char filter[] = "Documenti Word (*.docx)\0*.docx\0Tutti i file (*.*)\0*.*\0\0";

    ZeroMemory(&ofn, sizeof(ofn));
    ZeroMemory(file, sizeof(file));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrTitle = "Apri un documento Word";
    ofn.lpstrDefExt = "docx";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;

    if (!GetOpenFileNameA(&ofn))
        return 0;
    lstrcpynA(out, file, cap);
    return 1;
}

/* --------------------------- finestra --------------------------- */

static HBRUSH brush_cache(HBRUSH *slot, COLORREF c)
{
    if (!*slot)
        *slot = CreateSolidBrush(c);
    return *slot;
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc, page;
    int doc_h;
    HBRUSH desk, shadow, border;

    GetClientRect(h, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) {
        EndPaint(h, &ps);
        return;
    }

    if (!g_memdc || g_memw != rc.right || g_memh != rc.bottom) {
        free_memdc();
        g_memdc = CreateCompatibleDC(hdc);
        g_membmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        g_memold = SelectObject(g_memdc, g_membmp);
        g_memw = rc.right;
        g_memh = rc.bottom;
    }

    /* scrivania neutra */
    desk = brush_cache(&g_br_desk, RGB(226, 230, 235));
    FillRect(g_memdc, &rc, desk);

    doc_h = (g_doc && p_height) ? p_height(g_doc) : 0;
    if (doc_h < 48)
        doc_h = 48;

    page.left = g_page_x;
    page.top = page_top();
    page.right = page.left + g_page_w;
    page.bottom = page.top + doc_h;

    /* ombra morbida sotto/destra del foglio (senza alpha) */
    shadow = brush_cache(&g_br_shadow, RGB(201, 207, 214));
    {
        RECT s = page;
        OffsetRect(&s, 3, 3);
        FillRect(g_memdc, &s, shadow);
    }

    /* foglio bianco + contenuto litehtml */
    FillRect(g_memdc, &page, (HBRUSH)(COLOR_WINDOW + 1));
    if (g_doc)
        p_draw(g_doc, g_memdc, g_page_x, page.top, 0, 0, g_cw, g_ch);

    /* bordo pagina */
    border = brush_cache(&g_br_border, RGB(190, 197, 205));
    FrameRect(g_memdc, &page, border);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, g_memdc, 0, 0, SRCCOPY);
    EndPaint(h, &ps);
}

static void scroll_to(int y)
{
    g_scroll = y;
    clamp_scroll();
    update_scrollbar();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        g_cw = LOWORD(lp);
        g_ch = HIWORD(lp);
        g_memw = g_memh = 0; /* forza ricreazione del buffer */
        compute_page();
        if (g_doc) {
            p_render(g_doc, g_page_w);
            clamp_scroll();
            update_scrollbar();
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 420;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 300;
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint(h);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (strstr(g_cur, "pointer") || strstr(g_cur, "hand"))
                SetCursor(LoadCursor(NULL, IDC_HAND));
            else if (strstr(g_cur, "text"))
                SetCursor(LoadCursor(NULL, IDC_IBEAM));
            else
                SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE:
        if (g_doc)
            p_over(g_doc, (short)LOWORD(lp) - g_page_x, (short)HIWORD(lp) - page_top(),
                   (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_LBUTTONDOWN:
        if (g_doc)
            p_down(g_doc, (short)LOWORD(lp) - g_page_x, (short)HIWORD(lp) - page_top(),
                   (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_LBUTTONUP:
        if (g_doc)
            p_up(g_doc, (short)LOWORD(lp) - g_page_x, (short)HIWORD(lp) - page_top(),
                 (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_MOUSELEAVE:
        if (g_doc && p_leave)
            p_leave(g_doc);
        return 0;

    case WM_MOUSEWHEEL:
        scroll_to(g_scroll - (short)HIWORD(wp) / WHEEL_DELTA * 64);
        return 0;

    case WM_VSCROLL: {
        int code = LOWORD(wp);
        int h_doc = (g_doc && p_height) ? p_height(g_doc) : 0;
        int page = g_ch > 40 ? g_ch - 40 : g_ch;
        switch (code) {
        case SB_LINEUP:        scroll_to(g_scroll - 48); break;
        case SB_LINEDOWN:      scroll_to(g_scroll + 48); break;
        case SB_PAGEUP:        scroll_to(g_scroll - page); break;
        case SB_PAGEDOWN:      scroll_to(g_scroll + page); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: scroll_to(HIWORD(wp)); break;
        case SB_TOP:           scroll_to(0); break;
        case SB_BOTTOM:        scroll_to(h_doc); break;
        default: break;
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            if (wp == 'O') {
                char path[MAX_PATH];
                if (pick_file(path, sizeof(path)))
                    load_docx(path);
                return 0;
            }
        }
        switch (wp) {
        case VK_UP:       scroll_to(g_scroll - 48); return 0;
        case VK_DOWN:     scroll_to(g_scroll + 48); return 0;
        case VK_PRIOR:    scroll_to(g_scroll - g_ch + 40); return 0;
        case VK_NEXT:     scroll_to(g_scroll + g_ch - 40); return 0;
        case VK_HOME:     scroll_to(0); return 0;
        case VK_END:      scroll_to(1 << 28); return 0;
        case VK_F5:
            if (g_path[0]) load_docx(g_path);
            return 0;
        case VK_ESCAPE:   PostMessageA(h, WM_CLOSE, 0, 0); return 0;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 100: { /* Apri */
            char path[MAX_PATH];
            if (pick_file(path, sizeof(path)))
                load_docx(path);
            return 0;
        }
        case 101: /* Ricarica */
            if (g_path[0]) load_docx(g_path);
            return 0;
        case 102: /* Esci */
            PostMessageA(h, WM_CLOSE, 0, 0);
            return 0;
        case 103: /* Informazioni */
            MessageBoxA(h,
                "docxview\n\nDOCX -> HTML (libreria docx2html) -> litehtml.\n"
                "Rotella / frecce: scorri. Ctrl+O: apri. F5: ricarica.",
                "Informazioni", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;

    case WM_APP_SCROLLTO:
        scroll_to((int)lp);
        return 0;

    case WM_APP_OPENURL: {
        HMODULE sh = LoadLibraryA("shell32.dll");
        if (sh) {
            typedef HINSTANCE (WINAPI *P_ShellExecuteA)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, int);
            P_ShellExecuteA se = (P_ShellExecuteA)GetProcAddress(sh, "ShellExecuteA");
            if (se && se(h, "open", g_url, NULL, NULL, SW_SHOWNORMAL) > (HINSTANCE)32)
                return 0;
        }
        MessageBoxA(h, g_url, "Link", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    case WM_DESTROY:
        destroy_doc();
        free_memdc();
        if (g_br_desk) { DeleteObject(g_br_desk); g_br_desk = NULL; }
        if (g_br_shadow) { DeleteObject(g_br_shadow); g_br_shadow = NULL; }
        if (g_br_border) { DeleteObject(g_br_border); g_br_border = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* ---------------------------- entry ---------------------------- */

static void cmdline_path(LPSTR cmd, char *out, int cap)
{
    char *p = cmd;
    int n = 0;

    out[0] = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n < cap - 1)
            out[n++] = *p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && n < cap - 1)
            out[n++] = *p++;
    }
    out[n] = 0;
}

static int file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void enable_dpi_awareness(void)
{
    HMODULE u = GetModuleHandleA("user32.dll");
    if (u) {
        typedef BOOL (WINAPI *P_DPIAWARE)(void);
        P_DPIAWARE f = (P_DPIAWARE)GetProcAddress(u, "SetProcessDPIAware");
        if (f)
            f();
    }
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG msg;
    HMENU menu, filemenu;
    char path[MAX_PATH];

    (void)prev;
    (void)show;

    enable_dpi_awareness();
    g_tmpdc = GetDC(NULL);

    if (!dll_load()) {
        MessageBoxA(NULL,
            "litehtml_c.dll non trovata.\n\n"
            "Copiala accanto a docxview.exe (oppure lascia la cartella sunburst\n"
            "nella posizione attuale).",
            "docxview", MB_OK | MB_ICONERROR);
        return 1;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "DocxViewerWnd";
    if (!RegisterClassA(&wc)) {
        MessageBoxA(NULL, "RegisterClassA fallita", "docxview", MB_OK | MB_ICONERROR);
        return 1;
    }

    menu = CreateMenu();
    filemenu = CreatePopupMenu();
    AppendMenuA(filemenu, MF_STRING, 100, "&Apri...\tCtrl+O");
    AppendMenuA(filemenu, MF_STRING, 101, "&Ricarica\tF5");
    AppendMenuA(filemenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(filemenu, MF_STRING, 102, "&Esci");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)filemenu, "&File");
    AppendMenuA(menu, MF_STRING, 103, "&?");

    g_hwnd = CreateWindowExA(0, "DocxViewerWnd", "docxview",
                             WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1100, 800,
                             NULL, menu, hi, NULL);
    if (!g_hwnd) {
        MessageBoxA(NULL, "CreateWindowExA fallita", "docxview", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    cmdline_path(cmd, path, sizeof(path));
    if (!path[0] || !file_exists(path)) {
        char alt[MAX_PATH];
        GetModuleFileNameA(NULL, alt, sizeof(alt));
        {
            char *slash = strrchr(alt, '\\');
            if (slash) {
                lstrcpyA(slash + 1, "test.docx");
                if (file_exists(alt))
                    lstrcpynA(path, alt, sizeof(path));
                else
                    path[0] = 0;
            } else {
                path[0] = 0;
            }
        }
        if (!path[0] && !pick_file(path, sizeof(path)))
            path[0] = 0;
    }

    if (path[0])
        load_docx(path);
    else
        set_window_title();

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    docx_html_free(&g_html);
    if (g_tmpdc)
        ReleaseDC(NULL, g_tmpdc);
    if (g_dll)
        FreeLibrary(g_dll);
    return (int)msg.wParam;
}
