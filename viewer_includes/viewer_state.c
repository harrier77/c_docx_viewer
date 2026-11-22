/*
 * viewer_state.c - macro di configurazione, tipi e stato condiviso del viewer.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * viene incluso per primo, prima degli altri moduli che usano questo stato.
 */

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
static int      g_render_busy;      /* un render e' in corso/in coda */
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

/* Layout Cache (Fase 3) */
#include "layout_cache.h"
static LayoutCache *g_layout_cache = NULL;


/* REGRESSION WARNING: layout_content_width() = page_w - LAYOUT_PADDING_X is the
 * LAYOUT width passed to ld_render, while paint uses the full page width
 * (LAY->page_w - 2*LAYOUT_VISUAL_PADDING_X). The root-level line boxes get the
 * layout box centered inside the paint box (offset ~177px). This only works
 * because litehtml clamps line-box layout bounds to the containing block
 * width (see REGRESSION WARNING in litehtml render_inline_context.cpp).
 * If that clamp is removed or broken again, table-cell text of the first
 * column will render inside the second column. */