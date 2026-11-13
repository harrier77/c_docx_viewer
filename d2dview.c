/*
 * d2dview.c - Visualizzatore DOCX con litedocx.dll e renderer Direct2D
 * (Win32 + Direct2D/DirectWrite, C puro, MinGW GCC).
 *
 * Variante di docxview_new.c che usa SOLO il ramo Direct2D: nessun
 * RENDERER_GDI, nessun selettore di renderer (variabile d'ambiente o
 * argomento --renderer=), nessun fallback GDI.
 * Lo zoom e' puro zoom del renderer: il layout resta a zoom 1.0 e non
 * viene mai rifatto al cambio di zoom.
 */
#include "litedocx_c.h"

#include <windows.h>
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
 * Master flag: set to 0 to disable ALL console (stderr) logging from
 * the viewer. Set to 1 to re-enable it. When 1, the amount of output
 * is further controlled by TABLE_DEBUG below (0 = only the essential
 * render-lifecycle line, 1 = full table-layout trace set used to
 * diagnose the "first column text in second column" bug).
 * NOTE: this flag covers the viewer only; the DLL has its own switch
 * (LITEDOCX_CONSOLE_LOG in litedocx_c.cpp).
 * ------------------------------------------------------------------ */
#define CONSOLE_LOGGING 0

/* Logging su file (logs.txt accanto all'eseguibile). 0 = disabilitato. */
#define ENABLE_FILE_LOGGING 0

/* Padding: vedi LAYOUT_PADDING_X / LAYOUT_VISUAL_PADDING_X /
 * LAYOUT_VISUAL_MARGIN_Y in func_gdi/layout.h. */

static HWND   g_hwnd;
static void  *g_doc;
static Renderer *g_renderer;
static IDWriteFactory *g_dwrite_factory = NULL;

/* Font cache key for deduplication */
typedef struct FontCacheEntry {
    char        *family;
    int          size;
    int          bold;
    int          italic;
    int          underline;
    int          strike;
    IDWriteTextFormat *format;
    DWRITE_FONT_METRICS metrics;
    float        x_height;
    struct FontCacheEntry *next;
} FontCacheEntry;

static FontCacheEntry *g_font_cache = NULL;

/* Font handle returned to layout/DLL - wraps IDWriteTextFormat + metrics */
typedef struct DwFontHandle {
    IDWriteTextFormat *format;
    DWRITE_FONT_METRICS metrics;
    float        x_height;
    int          underline;
    int          strike;
    int          size_px;
    float        ascent;
    float        descent;
    float        line_gap;
} DwFontHandle;

/* Zoom puro del renderer (nessun re-layout). 1.0f = "nessuno zoom".
 * Il layout resta SEMPRE a LAY->zoom = 1.0: lo zoom e' applicato dal renderer
 * scalando coordinate e font al momento del disegno (vedi d2d_fill_rect /
 * d2d_draw_text). Cosi' il root del blocco di testo non viene mai riposizionato
 * (niente duplicazione/tosatura) e il range puo' essere ampio. */
static float          g_zoom     = 1.0f;
static const float    G_ZOOM_MIN = 0.9f;
static const float    G_ZOOM_MAX = 3.0f;

static int    g_doc_h;               /* altezza layout, cache lato UI */

/* Allinea il layout al fattore di scala effettivo di scroll/scroll_x.
 * Il layout resta a zoom 1.0, scroll/scroll_x sono pixel di layout e il
 * renderer li moltiplica per g_zoom -> view_scale = g_zoom.
 * Da chiamare ad ogni cambio di zoom/dimensioni, prima di layout_pan() e
 * layout_update_scrollbars(). */
static void sync_view_scale(void)
{
    layout_set_view_scale(g_zoom > 0.0f ? (double)g_zoom : 1.0);
}

static char      g_path[MAX_PATH];
static char      g_title[300];
/* g_url rimosso: serviva al vecchio docxview.c per aprire i link via
 * ShellExecute; ora cb_on_anchor_click riceve l'URL come parametro. */
static char      g_cur[32] = "arrow";
/* Pan con tasto centrale: ancora del drag (px schermo). */
static int       g_pan_active;
static int       g_pan_x, g_pan_y;
static FILE     *g_log_file = NULL;
#if ENABLE_FILE_LOGGING
static char      g_log_path[MAX_PATH];
#endif

static ld_callbacks g_cb;
static int g_cb_init;

/* Find or create a cached font entry */
static FontCacheEntry *font_cache_find(const char *family, int size, int bold,
                                       int italic, int underline, int strike)
{
    FontCacheEntry *e = g_font_cache;
    while (e) {
        if (e->size == size && e->bold == bold && e->italic == italic &&
            e->underline == underline && e->strike == strike &&
            strcmp(e->family, family) == 0) {
            return e;
        }
        e = e->next;
    }
    return NULL;
}

static FontCacheEntry *font_cache_add(const char *family, int size, int bold,
                                      int italic, int underline, int strike,
                                      IDWriteTextFormat *format,
                                      const DWRITE_FONT_METRICS *metrics,
                                      float x_height)
{
    FontCacheEntry *e = (FontCacheEntry *)calloc(1, sizeof(FontCacheEntry));
    if (!e) return NULL;
    
    e->family = _strdup(family);
    if (!e->family) {
        free(e);
        return NULL;
    }
    e->size = size;
    e->bold = bold;
    e->italic = italic;
    e->underline = underline;
    e->strike = strike;
    e->format = format;
    e->metrics = *metrics;
    e->x_height = x_height;
    e->next = g_font_cache;
    g_font_cache = e;
    return e;
}

static void *cb_create_font(const char *fam, int size, int bold, int italic,
                            int underline, int strike, lh_font_metrics *fm)
{
    WCHAR *wfam = NULL;
    const char *family = fam && fam[0] ? fam : "Aptos";
    IDWriteTextFormat *format = NULL;
    DWRITE_FONT_METRICS metrics;
    HRESULT hr;
    FontCacheEntry *cached;
    DwFontHandle *handle;
    float x_height = 0.0f;
    DWRITE_FONT_WEIGHT weight = bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style = italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
    float px_size = (float)size;

    if (size < 6) size = 6;
    px_size = (float)size;

    fprintf(stderr, "[FONT_CREATE] fam='%s' size=%d bold=%d italic=%d underline=%d strike=%d\n",
            family, size, bold, italic, underline, strike);

    /* Check cache first */
    cached = font_cache_find(family, size, bold, italic, underline, strike);
    if (cached) {
        format = cached->format;
        IDWriteTextFormat_AddRef(format);
        metrics = cached->metrics;
        x_height = cached->x_height;
        fprintf(stderr, "[FONT_CREATE] cache hit for '%s' %dpx\n", family, size);
    } else {
        wfam = u8_to_w(family);
        if (!wfam) {
            fprintf(stderr, "[FONT_CREATE] utf8_to_wide failed\n");
            return NULL;
        }

        if (!g_dwrite_factory) {
            fprintf(stderr, "[FONT_CREATE] no DWrite factory\n");
            free(wfam);
            return NULL;
        }

        hr = IDWriteFactory_CreateTextFormat(g_dwrite_factory, wfam, NULL,
            weight, style, stretch, px_size, L"en-US", &format);
        free(wfam);
        wfam = NULL;

        if (FAILED(hr) || !format) {
            fprintf(stderr, "[FONT_CREATE] CreateTextFormat failed hr=0x%08lX\n", (unsigned long)hr);
            return NULL;
        }

        /* Get font metrics: IDWriteTextFormat doesn't expose GetFontFace in this MinGW version.
         * Use fallback metrics based on font size. */
        {
            /* Fallback metrics based on typical font proportions */
            ZeroMemory(&metrics, sizeof(metrics));
            metrics.designUnitsPerEm = 2048;
            metrics.ascent = (UINT16)(2048 * 0.8f);
            metrics.descent = (UINT16)(2048 * 0.2f);
            metrics.lineGap = (UINT16)(2048 * 0.15f);
            x_height = (float)metrics.ascent * 0.5f;
        }

        /* Cache the font */
        cached = font_cache_add(family, size, bold, italic, underline, strike,
                                 format, &metrics, x_height);
        if (!cached) {
            IDWriteTextFormat_Release(format);
            return NULL;
        }
    }

    /* Create the opaque handle returned to the layout/DLL */
    handle = (DwFontHandle *)calloc(1, sizeof(DwFontHandle));
    if (!handle) {
        IDWriteTextFormat_Release(format);
        return NULL;
    }
    
    handle->format = format;
    handle->metrics = metrics;
    handle->x_height = x_height;
    handle->underline = underline;
    handle->strike = strike;
    handle->size_px = size;
    
    /* Convert design units to pixels */
    if (metrics.designUnitsPerEm > 0) {
        float scale = px_size / (float)metrics.designUnitsPerEm;
        handle->ascent = metrics.ascent * scale;
        handle->descent = metrics.descent * scale;
        handle->line_gap = metrics.lineGap * scale;
    } else {
        handle->ascent = px_size * 0.8f;
        handle->descent = px_size * 0.2f;
        handle->line_gap = px_size * 0.15f;
    }

    if (fm) {
        /* height = ascent + descent + line_gap (similar to tmHeight) */
        float height_f = handle->ascent + handle->descent + handle->line_gap;
        fm->font_size   = size;
        fm->height = (int)(height_f * 1.15f);  /* Word-like 1.15 line spacing */
        fm->ascent = (int)(handle->ascent + 0.5f);
        fm->descent = (int)(handle->descent + 0.5f);
        fm->x_height = (int)(x_height + 0.5f);
        fm->ch_width = (int)(px_size * 0.6f);  /* approximate average char width */
        fm->draw_spaces = (italic || underline || strike) ? 1 : 0;
        fm->sub_shift   = size / 5;
        fm->super_shift = size / 3;
    }

    return handle;
}

#if ENABLE_FILE_LOGGING
static void init_log_path(void)
{
    if (g_log_path[0]) return;
    GetModuleFileNameA(NULL, g_log_path, MAX_PATH);
    char *p = strrchr(g_log_path, '\\');
    if (p) p[1] = '\0';
    else g_log_path[0] = '\0';
    strcat(g_log_path, "logs.txt");
}
#endif

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
    if (font) {
        DwFontHandle *handle = (DwFontHandle *)font;
        if (handle->format) {
            IDWriteTextFormat_Release(handle->format);
        }
        free(handle);
    }
}

static int cb_text_width(const char *text, void *font)
{
    DwFontHandle *handle = (DwFontHandle *)font;
    WCHAR *ws;
    IDWriteTextLayout *layout = NULL;
    DWRITE_TEXT_METRICS metrics;
    int width = 0;
    HRESULT hr;

    if (!text || !text[0] || !handle || !handle->format)
        return 0;

    ws = u8_to_w(text);
    if (!ws)
        return 0;

    hr = IDWriteFactory_CreateTextLayout(g_dwrite_factory, ws, lstrlenW(ws),
        handle->format, 10000.0f, 10000.0f, &layout);
    free(ws);
    
    if (SUCCEEDED(hr) && layout) {
        hr = IDWriteTextLayout_GetMetrics(layout, &metrics);
        if (SUCCEEDED(hr)) {
            width = (int)(metrics.widthIncludingTrailingWhitespace + 0.5f);
        }
        IDWriteTextLayout_Release(layout);
    }

    return width;
}

static void cb_draw_text(void *hdc, const char *text, void *font, unsigned rgb, int x, int y, int w, int h)
{
    // log_msg("[DRAW_TEXT] text='%s' at (%d,%d) w=%d h=%d\n", text ? text : "NULL", x, y, w, h);
    (void)hdc;
    if (!text || !text[0])
        return;
    static int s_text_count = 0;
    if (s_text_count++ < 200)
        dbg_log("[TABLE] cb_draw_text text='%.60s' at (%d,%d) size=%dx%d\n", text, x, y, w, h);
    fprintf(stderr, "[FONT_DRAW] text='%.30s' font=%p at (%d,%d) zoom=%.2f\n", text ? text : "NULL", font, x, y, g_zoom);
    if (!g_renderer)
        return;
    g_renderer->vtbl->DrawText(g_renderer, text, font,
                               RGB(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)),
                               x, y, w, h);
}

static void cb_draw_solid_fill(void *hdc, int x, int y, int w, int h, unsigned rgb)
{
    (void)hdc;
    if (!g_renderer)
        return;
    g_renderer->vtbl->FillRect(g_renderer, x, y, w, h,
                               RGB(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)));
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

static int cb_draw_list_marker(void *target, int x, int y, int w, int h,
                               int marker_type, unsigned rgb, const char *image)
{
    COLORREF col;

    (void)target;
    (void)image;

    if (!g_renderer)
        return 0; /* fallback GDI dentro litedocx */

    col = RGB(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
    switch (marker_type) {
    case LD_MARKER_SQUARE:
        g_renderer->vtbl->DrawRect(g_renderer, x, y, w, h, col);
        return 1;
    case LD_MARKER_DISC:
    case LD_MARKER_CIRCLE:
        g_renderer->vtbl->DrawEllipse(g_renderer, x, y, w, h, col);
        return 1;
    default:
        /* none / numbered / alpha: nessuna forma da disegnare qui. */
        return 0;
    }
}

static void cb_draw_image_bitmap(void *target, void *native_bitmap,
                                 int src_w, int src_h, int x, int y, int w, int h)
{
    (void)target;
    if (!g_renderer || !native_bitmap)
        return;
    g_renderer->vtbl->DrawImage(g_renderer, native_bitmap, src_w, src_h, x, y, w, h);
}

static void cb_set_caption(const char *caption) { /* log_msg("[CB] set_caption: %s\n", caption ? caption : "NULL"); */ }
/* set_base_url e link rimosse: la DLL le implementa come no-op
 * (litedocx/src/litedocx_c.cpp:1077) e lh_callbacks non espone i campi:
 * nessuna callback puo' essere invocata dal viewer. */
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
    /* Fase 4: modalita' renderer.  Con renderer NULL la DLL torna al
     * comportamento GDI storico. */
    g_cb.renderer              = g_renderer;
    g_cb.draw_list_marker      = cb_draw_list_marker;
    g_cb.draw_image_bitmap     = cb_draw_image_bitmap;
    g_cb_init = 1;
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



/* REGRESSION WARNING: layout_content_width() = page_w - LAYOUT_PADDING_X is the
 * LAYOUT width passed to ld_render, while paint uses the full page width
 * (LAY->page_w - 2*LAYOUT_VISUAL_PADDING_X). The root-level line boxes get the
 * layout box centered inside the paint box (offset ~177px). This only works
 * because litehtml clamps line-box layout bounds to the containing block
 * width (see REGRESSION WARNING in litehtml render_inline_context.cpp).
 * If that clamp is removed or broken again, table-cell text of the first
 * column will render inside the second column. */

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

    fprintf(stderr, "[RENDER_REQ] w=%d g_render_busy=%d zoom=%.2f\n", w, g_render_busy, g_zoom);

    LOG_RENDER("request_render called");
    log_msg("[RENDER] request_render: w=%d g_render_busy=%d\n", w, g_render_busy);

    if (!g_worker_started)
    {
        /* Fallback sincrono */
        ld_render(g_doc, w);
        g_doc_content_w = w;
        g_doc_h = document_height(g_doc);
        layout_update_scrollbars(g_hwnd, g_doc_h);
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
        ld_render_range(g_doc, first_page, LAY->ch, layout_content_width());
        {
            int page_count = ld_get_page_count(g_doc);
            if (page_count > 0)
                g_last_rendered_page = page_count - 1;
            else if (last_page > g_last_rendered_page)
                g_last_rendered_page = last_page;
        }
        g_doc_h = document_height(g_doc);
        layout_update_scrollbars(g_hwnd, g_doc_h);
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
    j->content_w = layout_content_width();
    j->viewport_h = LAY->ch;
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
        ld_set_visual_padding(g_doc, LAYOUT_VISUAL_PADDING_X, LAYOUT_VISUAL_MARGIN_Y);
        /* Set fixed justification width = paintable width (viewport - 2*visual_padding)
         * This keeps justified text stable when LAYOUT_PADDING_X changes. */
        int justification_w = LAY->page_w - 2 * LAYOUT_VISUAL_PADDING_X;
        if (justification_w > 0)
            ld_set_justification_width(g_doc, justification_w);

        lstrcpynA(g_path, g_load_path, sizeof(g_path));
        {
            const char *p = strrchr(g_load_path, '\\');
            if (!p)
                p = strrchr(g_load_path, '/');
            sprintf(g_title, "d2dview - %s", p ? p + 1 : g_load_path);
        }
        SetWindowTextA(g_hwnd, g_title);
        LAY->scroll = 0;
        LAY->scroll_x = 0;
        LAY->pan_rem_x = 0.0;
        LAY->pan_rem_y = 0.0;

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
        request_render(layout_content_width());

        layout_update_scrollbars(g_hwnd, g_doc_h);
        InvalidateRect(g_hwnd, NULL, TRUE);
    }
    else
    {
        sprintf(g_title, "d2dview - [Errore apertura file]");
        SetWindowTextA(g_hwnd, g_title);
        if (g_doc && layout_content_width() != g_doc_content_w)
            request_render(layout_content_width());
        layout_update_scrollbars(g_hwnd, g_doc_h);
        InvalidateRect(g_hwnd, NULL, TRUE);
    }
}

static void on_render_done(void *doc, int used_w)
{
    if (g_shutdown)
        return;

    fprintf(stderr, "[RENDER_DONE] used_w=%d g_render_busy=%d zoom=%.2f\n", used_w, g_render_busy, g_zoom);

    LOG_RENDER("on_render_done called");
    log_msg("[RENDER] on_render_done: used_w=%d g_render_busy=%d g_render_pending_width=%d g_render_pending_first=%d g_render_pending_last=%d\n",
            used_w, g_render_busy, g_render_pending_width, g_render_pending_first_page, g_render_pending_last_page);

    g_render_busy = 0;
    g_render_doc = NULL;

    if (doc != g_doc)
        return;

    g_doc_content_w = used_w;
    g_doc_h = document_height(doc);

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
        layout_update_scrollbars(g_hwnd, g_doc_h);
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
    layout_update_scrollbars(g_hwnd, g_doc_h);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void start_async_load(const char *path)
{
    Job *j;
    int cw;

    init_callbacks();
    cw = layout_content_width();

    lstrcpynA(g_load_path, path, sizeof(g_load_path));
    g_load_gen++;
    g_load_active = 1;
    g_load_content_w = cw;
    g_render_pending_width = -1;
    g_render_pending_first_page = -1;
    g_render_pending_last_page = -1;

    /* UI mostra subito lo stato */
    SetWindowTextA(g_hwnd, "d2dview - Caricamento...");

    if (!g_worker_started)
    {
        /* Fallback sincrono: il worker non è disponibile. */
        void *doc = ld_open_file(path, &g_cb, LAY->page_w, LAY->ch > 0 ? LAY->ch : 600);
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
    j->viewport_w = LAY->page_w;
    j->viewport_h = LAY->ch > 0 ? LAY->ch : 600;
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
        break;

    case WM_SIZE:
        // log_msg("[SIZE] WM_SIZE cw=%d ch=%d\n", LOWORD(lp), HIWORD(lp));
        layout_resize(LOWORD(lp), HIWORD(lp));
        sync_view_scale();
        /* NON chiamare BeginDraw qui: in WM_SIZE non si disegna nulla.
         * Il back buffer viene (ri)allocato da BeginDraw in WM_PAINT, che e'
         * l'unico punto in cui viene bilanciato da Present/EndPaint.
         * Per il renderer D2D una BeginDraw orfana lascia il render target
         * con in_draw=1 e senza EndDraw (stato sporco). */
        if (g_doc && !g_load_active) {
            /* Update justification width on page size change */
            int justification_w = LAY->page_w - 2 * LAYOUT_VISUAL_PADDING_X;
            if (justification_w > 0)
                ld_set_justification_width(g_doc, justification_w);
            int content_w = layout_content_width();
            /* Il layout viene rifatto dal worker: la UI non si blocca.
             * Durante il render il paint mostra solo lo sfondo. */
            /* Soglia 2px per evitare render spuri su resize minimi (es. WM_SIZE iniziale) */
            if (abs(content_w - g_doc_content_w) > 2)
                request_render(content_w);
        }
        layout_update_scrollbars(hwnd, g_doc_h);
        InvalidateRect(hwnd, NULL, FALSE);
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        fprintf(stderr, "[PAINT] zoom=%.2f g_render_busy=%d g_doc=%p scroll=%d\n",
                g_zoom, g_render_busy, g_doc, LAY->scroll);

        if (!g_renderer ||
            !g_renderer->vtbl->BeginDraw(g_renderer, hwnd, LAY->cw, LAY->ch)) {
            RECT rc_all = {0, 0, LAY->cw, LAY->ch};
            FillRect(hdc, &rc_all, (HBRUSH)(COLOR_WINDOW + 1));
            EndPaint(hwnd, &ps);
            break;
        }

        /* D2D: zoom == sola trasformazione di scala applicata dal renderer.
         * Stesso meccanismo dell'esempio di riferimento funzionante
         * (esempio_zoom_d2d.c), SENZA lo spostamento (dx/dy) del root del
         * blocco di testo: matrice puramente scalare, ancorata all'origine.
         *   - il layout NON viene toccato (LAY->zoom resta 1.0): niente
         *     re-render, niente riposizionamento del blocco, niente
         *     duplicazione del testo;
         *   - il renderer scala coordinate e font al momento del disegno
         *     (d_zoom, vedi renderer_d2d.c: FillRect/FrameRect/DrawText/...).
         * ATTENZIONE: questo ramo NON deve uscire da WM_PAINT: deve
         * proseguire con document_draw -> Present (EndDraw + BitBlt) ->
         * EndPaint, altrimenti la pagina resta vuota e si genera un loop
         * di repaint (EndPaint mai chiamato). */
        {
            /* Riallinea il pan orizzontale ai limiti della nuova scala:
             * la larghezza del viewport in px di layout dipende dallo zoom. */
            sync_view_scale();
            layout_clamp_scroll_x();
            g_renderer->vtbl->SetTransform(g_renderer,
                                           g_zoom, 0.0f,
                                           0.0f, g_zoom,
                                           0.0f, 0.0f);
        }

        /* Sfondo scrivania */
        g_renderer->vtbl->FillRect(g_renderer, 0, 0, LAY->cw, LAY->ch,
                                   RGB(240, 240, 242));

        /* Fase 3: la geometria di paint vive in func_gdi/document.c.
         * Se un render e' in corso il documento viene modificato dal worker:
         * document_draw non chiama ld_draw e mostra la pagina bianca.
         * clip_scale = 1/zoom: il renderer scala il disegno, quindi l'area
         * logica che copre il viewport e' piu' grande quando zoom < 1. */
        document_draw(g_doc, g_renderer, g_doc_h, g_render_busy,
                      g_zoom > 0.0f ? 1.0f / g_zoom : 1.0f);

        g_renderer->vtbl->Present(g_renderer, hdc, LAY->cw, LAY->ch);
        /* Riporta il renderer a scala 1.0 (la trasformata viene reimpostata
         * a ogni WM_PAINT prima dei draw). */
        g_renderer->vtbl->ResetTransform(g_renderer);
        EndPaint(hwnd, &ps);
        break;
    }

    /* ---- Pan con tasto centrale (o trascinamento col tasto destro non
     * gestito altrove): sposta il contenuto via scroll_x/scroll, cioe' in
     * coordinate di LAYOUT. Nessun re-render: clip, scrollbar e ancoraggio
     * dello zoom restano coerenti, e vale identico per GDI e D2D. ---- */
    case WM_SETCURSOR:
        /* Cursore "sposta" durante il pan col tasto centrale. */
        if (LOWORD(lp) == HTCLIENT && g_pan_active) {
            SetCursor(LoadCursor(NULL, IDC_SIZEALL));
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_MBUTTONDOWN:
        if (g_doc) {
            g_pan_active = 1;
            g_pan_x = (short)LOWORD(lp);
            g_pan_y = (short)HIWORD(lp);
            SetCapture(hwnd);
            SetCursor(LoadCursor(NULL, IDC_SIZEALL));
        }
        return 0;

    case WM_MOUSEMOVE:
        if (g_pan_active) {
            int x = (short)LOWORD(lp);
            int y = (short)HIWORD(lp);

            sync_view_scale();
            layout_pan(x - g_pan_x, y - g_pan_y, g_doc_h);
            g_pan_x = x;
            g_pan_y = y;
            SetScrollPos(hwnd, SB_VERT, LAY->scroll, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_MBUTTONUP:
        if (g_pan_active) {
            g_pan_active = 0;
            ReleaseCapture();
        }
        return 0;

    case WM_CAPTURECHANGED:
        g_pan_active = 0;
        return 0;

    case WM_VSCROLL: {
        SCROLLINFO si;
        int old_s = LAY->scroll;
        int doc_h = g_doc_h;
        int max_s = (doc_h + 120) - LAY->ch;
        if (max_s < 0) max_s = 0;

        switch (LOWORD(wp)) {
        case SB_LINEUP:   LAY->scroll -= 30; break;
        case SB_LINEDOWN: LAY->scroll += 30; break;
        case SB_PAGEUP:   LAY->scroll -= LAY->ch / 2; break;
        case SB_PAGEDOWN: LAY->scroll += LAY->ch / 2; break;
        case SB_THUMBTRACK:
            ZeroMemory(&si, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(hwnd, SB_VERT, &si);
            LAY->scroll = si.nTrackPos;
            break;
        }
        if (LAY->scroll < 0) LAY->scroll = 0;
        if (LAY->scroll > max_s) LAY->scroll = max_s;
        if (LAY->scroll != old_s) {
            SetScrollPos(hwnd, SB_VERT, LAY->scroll, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
            LOG_SCROLL("WM_VSCROLL scroll changed");
            log_msg("[SCROLL] WM_VSCROLL: scroll=%d g_layout_complete=%d g_last_rendered_page=%d\n",
                    LAY->scroll, g_layout_complete, g_last_rendered_page);
            /* FASE 5/6: Trigger on-demand page rendering SOLO se layout non completo */
            if (g_doc && g_page_h_est > 0 && !g_layout_complete) {
                int visible_first = LAY->scroll / g_page_h_est;
                int visible_last = (LAY->scroll + LAY->ch) / g_page_h_est + 1; /* +1 lookahead */
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
        short cx_ = (short)LOWORD(lp);
        short cy_ = (short)HIWORD(lp);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);

        if (wp & MK_CONTROL) {
            /* Zoom puro del renderer (sola scala, nessun pan, nessun
             * re-layout). Il layout e i font restano a scala logica: il
             * fattore viene applicato in fase di disegno da renderer_d2d.c
             * (d_zoom), quindi basta ridisegnare. */
            float old_zoom = g_zoom;
            float new_zoom;
            const float step = 0.1f;
            if (delta > 0)
                new_zoom = (old_zoom + step < G_ZOOM_MAX)
                               ? old_zoom + step : G_ZOOM_MAX;
            else
                new_zoom = (old_zoom - step > G_ZOOM_MIN)
                               ? old_zoom - step : G_ZOOM_MIN;

            fprintf(stderr, "[ZOOM] delta=%d old=%.2f new=%.2f cursor=(%d,%d)\n",
                    delta, old_zoom, new_zoom, cx_, cy_);
            g_zoom = new_zoom;
            /* La scala del renderer e' ancorata all'origine: senza questa
             * compensazione la pagina (centrata) trasla verso destra ad
             * ogni zoom. layout_zoom_view sposta solo scroll/scroll_x
             * (coordinate di layout): nessun re-render. */
            sync_view_scale();
            layout_zoom_view((double)old_zoom, (double)new_zoom, g_doc_h);
            SetScrollPos(hwnd, SB_VERT, LAY->scroll, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* Shift+rotella: pan orizzontale (stessa autorita' del pan: scroll_x).
         * Non tocca il layout del documento: nessun re-render. */
        if (wp & MK_SHIFT) {
            sync_view_scale();
            layout_pan(delta / 3, 0, g_doc_h);
            SetScrollPos(hwnd, SB_VERT, LAY->scroll, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* Scroll verticale normale */
        int doc_h = g_doc_h;
        int max_s = (doc_h + 120) - LAY->ch;
        if (max_s < 0) max_s = 0;
        LAY->scroll -= delta / 3;
        if (LAY->scroll < 0) LAY->scroll = 0;
        if (LAY->scroll > max_s) LAY->scroll = max_s;
        SetScrollPos(hwnd, SB_VERT, LAY->scroll, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        LOG_SCROLL("WM_MOUSEWHEEL scroll changed");
        log_msg("[SCROLL] WM_MOUSEWHEEL: scroll=%d g_layout_complete=%d g_last_rendered_page=%d\n",
                LAY->scroll, g_layout_complete, g_last_rendered_page);
        /* FASE 5/6: Trigger on-demand page rendering SOLO se layout non completo */
        if (g_doc && g_page_h_est > 0 && !g_layout_complete) {
            int visible_first = LAY->scroll / g_page_h_est;
            int visible_last = (LAY->scroll + LAY->ch) / g_page_h_est + 1; /* +1 lookahead */
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
            MessageBoxA(hwnd, "d2dview - Visualizzatore DOCX con renderer Direct2D (litedocx.dll).", "Informazioni", MB_OK);
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
