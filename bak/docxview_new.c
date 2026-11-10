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
#include <stdarg.h>





#define PAGE_MAX_W       960
#define PAGE_DESK_MARGIN 24
#define PAGE_TOP_GAP     20

/* ------------------------------------------------------------------
 * Master flag: set to 0 to disable ALL console (stderr) logging from
 * the viewer. Set to 1 to re-enable it. When 1, the amount of output
 * is further controlled by TABLE_DEBUG below (0 = only the essential
 * render-lifecycle line, 1 = full table-layout trace set used to
 * diagnose the "first column text in second column" bug).
 * NOTE: this flag covers the viewer only; the DLL has its own switch
 * (LITEDOCX_CONSOLE_LOG in litedocx_c.cpp).
 * ------------------------------------------------------------------ */
#define CONSOLE_LOGGING 0

/* NEVER CHANGE THIS VALUE! Last valid value was 366 but it depends from other layout parameters*/
static int      g_layout_padding_x = 366;     /* padding TOTALE orizzontale per layout (giustificazione) */
/*groq equation does not work*/
//static int      g_layout_padding_x = g_page_w - word_text;

static HWND   g_hwnd;
static void  *g_doc;
static int    g_doc_h;               /* altezza layout, cache lato UI */
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
static FILE     *g_log_file = NULL;
static char      g_log_path[MAX_PATH];

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

    if (fm && f) {
        /* DC locale: create_font viene chiamata dal worker di layout, non usare
         * un HDC globale creato dal thread UI. */
        HDC dc = GetDC(NULL);
        if (dc) {
            HGDIOBJ old = SelectObject(dc, f);
            ZeroMemory(&tm, sizeof(tm));
            GetTextMetricsW(dc, &tm);
            SelectObject(dc, old);
            ReleaseDC(NULL, dc);
            fm->font_size = size;
            /* Word usa interlinea 1.15 di default (single spacing).
             * tm.tmHeight include ascent+descent+internal_leading.
             * Moltiplichiamo per 1.xx per approssimare lo spacing di Word. */
            fm->height = (int)(tm.tmHeight * 1.15f);
            fm->ascent = tm.tmAscent;
            fm->descent = tm.tmDescent;
            fm->x_height = tm.tmHeight / 2;
            fm->ch_width = tm.tmAveCharWidth;
            fm->draw_spaces = (italic || underline || strike) ? 1 : 0;
            fm->sub_shift = size / 5;
            fm->super_shift = size / 3;
        }
    }
    return f;
}

#define ENABLE_FILE_LOGGING 0  // set to 0 to disable all file logging

static void init_log_path(void)
{
    if (g_log_path[0]) return;
    GetModuleFileNameA(NULL, g_log_path, MAX_PATH);
    char *p = strrchr(g_log_path, '\\');
    if (p) p[1] = '\0';
    else g_log_path[0] = '\0';
    strcat(g_log_path, "logs.txt");
}

static void log_msg(const char *fmt, ...)
{
#if !ENABLE_FILE_LOGGING
    return;
#else
    if (!g_log_file) {
        init_log_path();
        g_log_file = fopen(g_log_path, "a");
        if (!g_log_file) return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log_file, fmt, args);
    va_end(args);
    fflush(g_log_file);
#endif
}

/* Fine-grained switch, active only when CONSOLE_LOGGING is 1.
 * 0 = only the essential render-lifecycle message stays visible.
 * 1 = full table-layout trace set (diagnosis of the "first column text
 * in second column" bug; see REGRESSION WARNING below). */
#define TABLE_DEBUG 0

static void dbg_log(const char *fmt, ...)
{
#if CONSOLE_LOGGING && TABLE_DEBUG
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[DBG] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
#else
    (void)fmt;
#endif
}

#define LOG_JOB(kind, msg) log_msg("[JOB] %s: %s\n", #kind, msg)
#define LOG_RENDER(msg) log_msg("[RENDER] %s\n", msg)
#define LOG_PAINT(msg) log_msg("[PAINT] %s\n", msg)
#define LOG_SCROLL(msg) log_msg("[SCROLL] %s\n", msg)

static void cb_delete_font(void *font)
{
    if (font)
        DeleteObject((HFONT)font);
}

static int cb_text_width(const char *text, void *font)
{
    // log_msg("[TEXT_WIDTH] text='%s'\n", text ? text : "NULL");
    SIZE sz;
    HDC dc;
    HGDIOBJ old = NULL;

    sz.cx = sz.cy = 0;
    if (!text || !text[0])
        return 0;
    /* La misura può avvenire sul worker di layout: niente HDC globale. */
    dc = GetDC(NULL);
    if (!dc)
        return 0;
    if (font)
        old = SelectObject(dc, (HFONT)font);
    {
        WCHAR *w = u8_to_w(text);
        if (w) {
            GetTextExtentPoint32W(dc, w, lstrlenW(w), &sz);
            free(w);
        }
    }
    if (font)
        SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    return sz.cx;
}

static void cb_draw_text(void *hdc, const char *text, void *font, unsigned rgb, int x, int y, int w, int h)
{
    // log_msg("[DRAW_TEXT] text='%s' at (%d,%d) w=%d h=%d\n", text ? text : "NULL", x, y, w, h);
    HDC dc = (HDC)hdc;
    if (!dc || !text || !text[0])
        return;
    static int s_text_count = 0;
    if (s_text_count++ < 200)
        dbg_log("[TABLE] cb_draw_text text='%.60s' at (%d,%d) size=%dx%d\n", text, x, y, w, h);
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

static void cb_set_caption(const char *caption) { /* log_msg("[CB] set_caption: %s\n", caption ? caption : "NULL"); */ }
static void cb_set_base_url(const char *url) { /* log_msg("[CB] set_base_url: %s\n", url ? url : "NULL"); */ }
static void cb_link(const char *url, const char *title) { /* log_msg("[CB] link: url=%s title=%s\n", url ? url : "NULL", title ? title : "NULL"); */ }
static void cb_on_anchor_click(const char *url)
{
    /* log_msg("[CB] on_anchor_click: %s\n", url ? url : "NULL"); */
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
    int doc_h = g_doc_h;
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

/* --------------------------- worker thread / jobs --------------------------- */

#define WM_APP_DOC_READY    (WM_APP + 1)
#define WM_APP_RENDER_DONE  (WM_APP + 3)

typedef enum JobKind
{
    JOB_LOAD = 1,
    JOB_RENDER,
    JOB_RENDER_RANGE,
    JOB_DESTROY,
    JOB_QUIT
} JobKind;

typedef struct Job
{
    struct Job *next;
    JobKind     kind;
    HWND        hwnd;
    unsigned    id;
    char        path[MAX_PATH];
    int         content_w;
    int         viewport_w;
    int         viewport_h;
    void       *doc;
    int         first_page;
    int         last_page;
} Job;

static CRITICAL_SECTION g_job_lock;
static Job             *g_job_head;
static Job             *g_job_tail;
static HANDLE           g_job_event;
static HANDLE           g_worker_thread;
static int              g_worker_started;
static volatile LONG    g_shutdown;

/* Stato usato solo dal thread UI */
static char     g_load_path[MAX_PATH];
static unsigned g_load_gen;         /* generazione dell'ultima richiesta */
static int      g_load_active;      /* load in corso o in coda */
static int      g_load_content_w;   /* larghezza usata da quella load */
static int      g_doc_content_w;    /* larghezza del layout corrente */
static int      g_render_busy;      /* un render è in corso/in coda */
static int      g_render_pending_width;   /* larghezza richiesta per resize, -1 = nessuna */
static int      g_render_pending_first_page; /* first page per range pending, -1 = none */
static int      g_render_pending_last_page;  /* last page per range pending, -1 = none */
static void    *g_render_doc;
static int      g_render_used_w;

/* FASE 5/6: Pagination state */
static int      g_first_rendered_page = -1;  /* prima pagina renderizzata */
static int      g_last_rendered_page = -1;   /* ultima pagina renderizzata */
static int      g_page_h_est = 0;            /* altezza stimata singola pagina */
static int      g_layout_complete = 0;       /* layout completo dopo primo full render */



/* REGRESSION WARNING: content_width() = page_w - g_layout_padding_x is the
 * LAYOUT width passed to ld_render, while paint uses the full page width
 * (g_page_w - 2*g_visual_padding_x). The root-level line boxes get the
 * layout box centered inside the paint box (offset ~177px). This only works
 * because litehtml clamps line-box layout bounds to the containing block
 * width (see REGRESSION WARNING in litehtml render_inline_context.cpp).
 * If that clamp is removed or broken again, table-cell text of the first
 * column will render inside the second column. */

static int      g_visual_padding_x = 6;      /* <-- IMPOSTA QUI il padding visivo orizzontale (paint), es. 6, 12, 20... */
static int      g_visual_margin_y = 6;      /* margine visivo alto/basso (paint) */

static int content_width(void)
{
    int w = g_page_w - g_layout_padding_x;
    dbg_log("[TABLE] content_width page_w=%d layout_padding_x=%d result=%d\n", g_page_w, g_layout_padding_x, w);
    return w > 0 ? w : 1;
}

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
             * contract between viewer and litedocx (see content_width()). */
#if CONSOLE_LOGGING
            fprintf(stderr, "[RENDER] ld_render content_w=%d\n", j->content_w);
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

static void request_render(int w)
{
    Job *j;

    if (!g_doc || g_load_active)
        return;

    LOG_RENDER("request_render called");
    log_msg("[RENDER] request_render: w=%d g_render_busy=%d\n", w, g_render_busy);

    if (!g_worker_started)
    {
        /* Fallback sincrono */
        ld_render(g_doc, w);
        g_doc_content_w = w;
        g_doc_h = ld_height(g_doc);
        update_scrollbars();
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }

    if (g_render_busy)
    {
        LOG_RENDER("render busy, queuing pending width");
        g_render_pending_width = w;
        return;
    }

    j = alloc_job(JOB_RENDER);
    if (!j)
        return;
    j->doc = g_doc;
    j->content_w = w;
    g_render_busy = 1;
    g_render_doc = g_doc;
    g_render_used_w = w;
    g_render_pending_width = -1;
    dbg_log("[TABLE] request_render content_w=%d\n", w);
    enqueue_job(j);
    LOG_RENDER("JOB_RENDER enqueued");
}

/* FASE 5/6: Renderizza un range di pagine (es. pagina 0, o 1-2 per lookahead).
 * Se il range è già coperto, non tocca il worker e non alza g_render_busy:
 * così lo scroll su pagine già layoutate non produce pagina bianca. */
static void request_render_range(int first_page, int last_page)
{
    Job *j;

    if (!g_doc || g_load_active)
        return;

    if (first_page < 0)
        first_page = 0;
    if (last_page < first_page)
        last_page = first_page;

    LOG_RENDER("request_render_range called");
    log_msg("[RENDER] request_render_range: first=%d last=%d g_last_rendered_page=%d g_render_busy=%d g_layout_complete=%d\n",
            first_page, last_page, g_last_rendered_page, g_render_busy, g_layout_complete);

    /* Già layoutato fino a last_page: niente job, niente busy. */
    if (g_last_rendered_page >= 0 && last_page <= g_last_rendered_page)
    {
        LOG_RENDER("range already rendered, skipping");
        return;
    }

    if (!g_worker_started)
    {
        /* Fallback sincrono */
        ld_render_range(g_doc, first_page, g_ch, content_width());
        {
            int page_count = ld_get_page_count(g_doc);
            if (page_count > 0)
                g_last_rendered_page = page_count - 1;
            else if (last_page > g_last_rendered_page)
                g_last_rendered_page = last_page;
        }
        g_doc_h = ld_height(g_doc);
        update_scrollbars();
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }

    if (g_render_busy)
    {
        /* Accoda range pending separato da width pending */
        if (g_render_pending_first_page < 0 || last_page > g_render_pending_last_page)
        {
            g_render_pending_first_page = first_page;
            g_render_pending_last_page = last_page;
        }
        LOG_RENDER("render busy, queued pending range");
        return;
    }

    j = alloc_job(JOB_RENDER_RANGE);
    if (!j)
        return;
    j->doc = g_doc;
    j->content_w = content_width();
    j->viewport_h = g_ch;
    j->first_page = first_page;
    j->last_page = last_page;
    g_render_busy = 1;
    g_render_doc = g_doc;
    g_render_used_w = last_page; /* reused as last_page for range jobs */
    g_render_pending_first_page = first_page;
    g_render_pending_last_page = last_page;
    enqueue_job(j);
    LOG_RENDER("JOB_RENDER_RANGE enqueued");
}

static void apply_loaded_doc(unsigned id, void *doc)
{
    if (g_shutdown)
    {
        if (doc)
            dispose_doc(doc);
        return;
    }

    LOG_RENDER("apply_loaded_doc called");
    log_msg("[RENDER] apply_loaded_doc: id=%u g_load_gen=%u doc=%p\n", id, g_load_gen, doc);

    /* Risultato di una richiesta superata: va distrutto sul worker. */
    if (id != g_load_gen)
    {
        if (doc)
            dispose_doc(doc);
        return;
    }

    g_load_active = 0;

    if (doc)
    {
        if (g_doc)
            dispose_doc(g_doc);
        g_doc = doc;
        g_doc_content_w = g_load_content_w;

        /* Set visual padding so layout box is centered inside visual box */
        ld_set_visual_padding(g_doc, g_visual_padding_x, g_visual_margin_y);

        lstrcpynA(g_path, g_load_path, sizeof(g_path));
        {
            const char *p = strrchr(g_load_path, '\\');
            if (!p)
                p = strrchr(g_load_path, '/');
            sprintf(g_title, "docxview_new - %s", p ? p + 1 : g_load_path);
        }
        SetWindowTextA(g_hwnd, g_title);
        g_scroll = 0;

        /* Stima altezza documento SENZA chiamare ld_get_page_height/count
         * (possono triggerare full render sul thread UI - vedi D7). */
        g_doc_h = 2000; /* fallback iniziale, sarà aggiornato dopo render */
        g_page_h_est = 1000;
        g_first_rendered_page = -1;
        g_last_rendered_page = -1;
        g_layout_complete = 0;

        /* Lancia UN SOLO job di layout completo in background */
        g_render_pending_width = -1;
        g_render_pending_first_page = -1;
        g_render_pending_last_page = -1;
        request_render(content_width());

        update_scrollbars();
        InvalidateRect(g_hwnd, NULL, TRUE);
    }
    else
    {
        sprintf(g_title, "docxview_new - [Errore apertura file]");
        SetWindowTextA(g_hwnd, g_title);
        if (g_doc && content_width() != g_doc_content_w)
            request_render(content_width());
        update_scrollbars();
        InvalidateRect(g_hwnd, NULL, TRUE);
    }
}

static void on_render_done(void *doc, int used_w)
{
    if (g_shutdown)
        return;

    LOG_RENDER("on_render_done called");
    log_msg("[RENDER] on_render_done: used_w=%d g_render_busy=%d g_render_pending_width=%d g_render_pending_first=%d g_render_pending_last=%d\n",
            used_w, g_render_busy, g_render_pending_width, g_render_pending_first_page, g_render_pending_last_page);

    g_render_busy = 0;
    g_render_doc = NULL;

    if (doc != g_doc)
        return;

    g_doc_content_w = used_w;
    g_doc_h = ld_height(doc);

    /* ld_render_range oggi, al primo richiamo, fa full layout di tutto il
     * documento. Quindi dopo il primo RENDER_DONE possiamo considerare
     * TUTTE le pagine già layoutate: niente altri job a ogni salto. */
    {
        int page_count = ld_get_page_count(g_doc);
        if (page_count > 0)
            g_last_rendered_page = page_count - 1;
        else if (g_render_pending_last_page >= 0 && g_render_pending_last_page > g_last_rendered_page)
            g_last_rendered_page = g_render_pending_last_page;

        if (g_first_rendered_page < 0)
            g_first_rendered_page = 0;

        /* Aggiorna stima altezza pagina con valore REALE dopo full layout */
        int page_h = ld_get_page_height(g_doc, 0);
        if (page_h > 0)
            g_page_h_est = page_h;
        log_msg("[RENDER] on_render_done: page_count=%d g_last_rendered_page=%d g_page_h_est=%d\n",
                page_count, g_last_rendered_page, g_page_h_est);
    }

    /* Layout completo dopo il primo full render */
    g_layout_complete = 1;
    LOG_RENDER("layout complete flag set");

    /* Pending width (resize) ha priorità su pending range */
    if (!g_load_active && g_render_pending_width >= 0 && g_render_pending_width != used_w)
    {
        int w = g_render_pending_width;
        g_render_pending_width = -1;
        request_render(w);
        /* Non processare pending range se c'era pending width */
        g_render_pending_first_page = -1;
        g_render_pending_last_page = -1;
        update_scrollbars();
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }

    /* Pending range (scroll) */
    if (!g_load_active && g_render_pending_first_page >= 0)
    {
        int first = g_render_pending_first_page;
        int last = g_render_pending_last_page;
        g_render_pending_first_page = -1;
        g_render_pending_last_page = -1;
        request_render_range(first, last);
        return;
    }

    g_render_pending_width = -1;
    g_render_pending_first_page = -1;
    g_render_pending_last_page = -1;
    g_render_used_w = -1;
    update_scrollbars();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void start_async_load(const char *path)
{
    Job *j;
    int cw;

    init_callbacks();
    compute_page();
    cw = content_width();

    lstrcpynA(g_load_path, path, sizeof(g_load_path));
    g_load_gen++;
    g_load_active = 1;
    g_load_content_w = cw;
    g_render_pending_width = -1;
    g_render_pending_first_page = -1;
    g_render_pending_last_page = -1;

    /* UI mostra subito lo stato */
    SetWindowTextA(g_hwnd, "docxview_new - Caricamento...");

    if (!g_worker_started)
    {
        /* Fallback sincrono: il worker non è disponibile. */
        void *doc = ld_open_file(path, &g_cb, g_page_w, g_ch > 0 ? g_ch : 600);
        /* D1: NON chiamare ld_render qui - apply_loaded_doc lancerà request_render */
        g_load_active = 0;
        apply_loaded_doc(g_load_gen, doc);
        return;
    }

    j = alloc_job(JOB_LOAD);
    if (!j)
    {
        g_load_active = 0;
        MessageBoxA(g_hwnd, "Memoria insufficiente", "Errore", MB_OK | MB_ICONERROR);
        return;
    }
    j->id = g_load_gen;
    lstrcpynA(j->path, path, sizeof(j->path));
    j->content_w = cw;
    j->viewport_w = g_page_w;
    j->viewport_h = g_ch > 0 ? g_ch : 600;
    enqueue_job(j);
}

static void load_docx_file(const char *path)
{
    if (!path || !path[0]) return;
    start_async_load(path);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        // log_msg("[CREATE] WM_CREATE\n");
        g_hwnd = hwnd;
        g_br_desk = CreateSolidBrush(RGB(240, 240, 242));
        g_br_shadow = CreateSolidBrush(RGB(190, 190, 195));
        g_br_border = CreateSolidBrush(RGB(210, 210, 215));
        break;

    case WM_SIZE:
        // log_msg("[SIZE] WM_SIZE cw=%d ch=%d\n", LOWORD(lp), HIWORD(lp));
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
        if (g_doc && !g_load_active) {
            int content_w = content_width();
            /* Il layout viene rifatto dal worker: la UI non si blocca.
             * Durante il render il paint mostra solo lo sfondo. */
            /* Soglia 2px per evitare render spuri su resize minimi (es. WM_SIZE iniziale) */
            if (abs(content_w - g_doc_content_w) > 2)
                request_render(content_w);
        }
        update_scrollbars();
        InvalidateRect(hwnd, NULL, FALSE);
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc_all = {0, 0, g_cw, g_ch};

        if (!g_memdc) {
            FillRect(hdc, &rc_all, g_br_desk);
            EndPaint(hwnd, &ps);
            break;
        }

        FillRect(g_memdc, &rc_all, g_br_desk);

        /* Se un render è in corso il documento viene modificato dal worker:
         * non chiamare ld_draw per evitare accessi concorrenti. */
        if (g_doc && !g_render_busy) {
            int pt = page_top();
            int doc_h = g_doc_h;
            int page_h = doc_h + 120;
            RECT rc_shd = {g_page_x + 4, pt + 4, g_page_x + g_page_w + 4, pt + page_h + 4};
            RECT rc_page = {g_page_x, pt, g_page_x + g_page_w, pt + page_h};

            FillRect(g_memdc, &rc_shd, g_br_shadow);
            FillRect(g_memdc, &rc_page, (HBRUSH)GetStockObject(WHITE_BRUSH));
            FrameRect(g_memdc, &rc_page, g_br_border);

            /* Margini interni pagina: visual padding orizzontale configurabile */
            const int page_margin_x = g_visual_padding_x;
            const int page_margin_y = g_visual_margin_y;
            int content_w = g_page_w - 2 * page_margin_x;
            int content_x = g_page_x + page_margin_x;
            int content_y = pt + page_margin_y;

            /* Clip rect = intersezione area contenuto + viewport visibile */
            RECT rc_content = {content_x, content_y, content_x + content_w, content_y + doc_h};
            RECT rc_viewport = {0, 0, g_cw, g_ch};
            RECT rc_clip;
            IntersectRect(&rc_clip, &rc_content, &rc_viewport);
            
            dbg_log("[TABLE] WM_PAINT page_w=%d page_x=%d layout_padding_x=%d content_w=%d content_x=%d content_y=%d clip=(%d,%d,%d,%d)\n",
                    g_page_w, g_page_x, g_layout_padding_x, content_w, content_x, content_y,
                    rc_clip.left, rc_clip.top, rc_clip.right, rc_clip.bottom);

            ld_draw(g_doc, g_memdc, content_x, content_y,
                    rc_clip.left, rc_clip.top, rc_clip.right - rc_clip.left, rc_clip.bottom - rc_clip.top);
        } else if (g_doc && g_render_busy) {
            /* Layout in corso: mostra la pagina bianca senza contenuto. */
            int pt = page_top();
            int page_h = g_doc_h + 120;
            RECT rc_page = {g_page_x, pt, g_page_x + g_page_w, pt + page_h};
            FillRect(g_memdc, &rc_page, (HBRUSH)GetStockObject(WHITE_BRUSH));
            FrameRect(g_memdc, &rc_page, g_br_border);
        }

        BitBlt(hdc, 0, 0, g_cw, g_ch, g_memdc, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_VSCROLL: {
        SCROLLINFO si;
        int old_s = g_scroll;
        int doc_h = g_doc_h;
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
            LOG_SCROLL("WM_VSCROLL scroll changed");
            log_msg("[SCROLL] WM_VSCROLL: scroll=%d g_layout_complete=%d g_last_rendered_page=%d\n",
                    g_scroll, g_layout_complete, g_last_rendered_page);
            /* FASE 5/6: Trigger on-demand page rendering SOLO se layout non completo */
            if (g_doc && g_page_h_est > 0 && !g_layout_complete) {
                int visible_first = g_scroll / g_page_h_est;
                int visible_last = (g_scroll + g_ch) / g_page_h_est + 1; /* +1 lookahead */
                int page_count = ld_get_page_count(g_doc);
                if (visible_last >= page_count) visible_last = page_count - 1;
                if (visible_first < 0) visible_first = 0;
                if (visible_last > g_last_rendered_page) {
                    request_render_range(g_last_rendered_page + 1, visible_last);
                }
            }
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int doc_h = g_doc_h;
        int max_s = (doc_h + 120) - g_ch;
        if (max_s < 0) max_s = 0;
        g_scroll -= delta / 3;
        if (g_scroll < 0) g_scroll = 0;
        if (g_scroll > max_s) g_scroll = max_s;
        SetScrollPos(hwnd, SB_VERT, g_scroll, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        LOG_SCROLL("WM_MOUSEWHEEL scroll changed");
        log_msg("[SCROLL] WM_MOUSEWHEEL: scroll=%d g_layout_complete=%d g_last_rendered_page=%d\n",
                g_scroll, g_layout_complete, g_last_rendered_page);
        /* FASE 5/6: Trigger on-demand page rendering SOLO se layout non completo */
        if (g_doc && g_page_h_est > 0 && !g_layout_complete) {
            int visible_first = g_scroll / g_page_h_est;
            int visible_last = (g_scroll + g_ch) / g_page_h_est + 1; /* +1 lookahead */
            int page_count = ld_get_page_count(g_doc);
            if (visible_last >= page_count) visible_last = page_count - 1;
            if (visible_first < 0) visible_first = 0;
            if (visible_last > g_last_rendered_page) {
                request_render_range(g_last_rendered_page + 1, visible_last);
            }
        }
        break;
    }

    case WM_APP_DOC_READY:
        apply_loaded_doc((unsigned)wp, (void *)lp);
        return 0;

    case WM_APP_RENDER_DONE:
        on_render_done((void *)lp, (int)wp);
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
        if (g_memold) SelectObject(g_memdc, g_memold);
        if (g_membmp) DeleteObject(g_membmp);
        if (g_memdc) DeleteDC(g_memdc);
        if (g_br_desk) DeleteObject(g_br_desk);
        if (g_br_shadow) DeleteObject(g_br_shadow);
        if (g_br_border) DeleteObject(g_br_border);
        if (g_log_file) {
            fclose(g_log_file);
            g_log_file = NULL;
        }
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

    hwnd = CreateWindowExW(0, L"DocxViewNewClass", L"docxview_new",
                           WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                           CW_USEDEFAULT, CW_USEDEFAULT, g_cw, g_ch,
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
