/*
 * viewer_state.c - macro di configurazione, tipi e stato condiviso del viewer.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * viene incluso per primo, prima degli altri moduli che usano questo stato.
 */

#include "debug_log.h"

/* ------------------------------------------------------------------
 * Viewer logging follows the single build switch in debug_log.h.
 * Builds may still override these two flags on the command line.
 * NOTE: litedocx.dll and litehtml use the same D2D_DEBUG_LOGS switch.
 * ------------------------------------------------------------------ */
#ifndef CONSOLE_LOGGING
#define CONSOLE_LOGGING D2D_DEBUG_LOGS
#endif

/* Logging su file (logs.txt accanto all'eseguibile). */
#ifndef ENABLE_FILE_LOGGING
#define ENABLE_FILE_LOGGING D2D_DEBUG_LOGS
#endif

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
    /* Copia leggera dell'identita' del font: il caret la usa per ricostruire
     * un layout DirectWrite senza conservare il puntatore al font della DLL. */
    char         family[64];
    int          bold;
    int          italic;
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
/* Toolbar */
static HWND      g_toolbar;
#define TOOLBAR_HEIGHT 36
#define IDM_TB_OPEN   4001
#define IDM_TB_SAVE   4002
#define IDM_TB_RELOAD 4003
#define IDM_TB_EXIT   4004
static FILE     *g_log_file = NULL;
#if ENABLE_FILE_LOGGING
static char      g_log_path[MAX_PATH];
#endif

static ld_callbacks g_cb;
static int g_cb_init;

/* --------------------------- worker thread / jobs --------------------------- */

#define WM_APP_DOC_READY        (WM_APP + 1)
#define WM_APP_RENDER_DONE      (WM_APP + 3)
#define WM_APP_HIT_RESULT       (WM_APP + 4)
#define WM_APP_RUN_HIT_RESULT   (WM_APP + 5)
#define WM_APP_PARA_XML_READY   (WM_APP + 6)
#define WM_APP_EDIT_APPLIED     (WM_APP + 7)

typedef enum JobKind
{
    JOB_LOAD = 1,
    JOB_RENDER,
    JOB_RENDER_RANGE,
    JOB_HIT_TEST,
    JOB_RUN_HIT_TEST,
    JOB_GET_PARA_XML,
    JOB_APPLY_EDIT,
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
    int         doc_x;
    int         doc_y;
    int         para_index;
    int         gen;
    int         epoch;
    int         ooxml;
    int         caret;      /* 1 = commit del caret inline (non dell'editor) */
    char       *text;
    char       *xml_text;
    unsigned    token;
} Job;

/* Result structures for async jobs */
typedef struct HitResult
{
    void       *doc;
    unsigned    gen;
    int         epoch;
    int         hit;
    int         index;
    char       *text;
} HitResult;

typedef struct RunHitResult
{
    void       *doc;
    unsigned    gen;
    int         epoch;
    int         doc_x;
    int         doc_y;
    int         hit;
    ld_run_hit  info;
} RunHitResult;

typedef struct ParaXmlResult
{
    void       *doc;
    unsigned    gen;
    int         epoch;
    int         index;
    int         ok;
    char       *xml;
    char        error[256];
} ParaXmlResult;

typedef struct EditResult
{
    void       *doc;
    unsigned    gen;
    unsigned    token;
    int         ok;
    int         caret;      /* 1 = proviene dal caret inline */
    char        error[256];
} EditResult;

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

/* Editor constants */
#define LD_PARA_EDIT_CLASS   L"D2DParaEditClass"
#define IDC_PARA_EDIT_XML    1001
#define IDC_PARA_CANCEL      1002
#define IDC_PARA_APPLY       1003
#define IDC_PARA_HINT        1004

/* Editor state (Fase 4) */
typedef enum DocState
{
    DOC_STATE_IDLE       = 0,
    DOC_STATE_HITTEST    = 1,
    DOC_STATE_EDITOR     = 2,
    DOC_STATE_APPLYING   = 3
} DocState;

static DocState  g_doc_state = DOC_STATE_IDLE;
static int       g_edit_index = -1;
static int       g_edit_apply = 0;
static int       g_edit_is_xml = 0;
static int       g_edit_readonly = 0;
static HWND      g_edit_hwnd = NULL;
static HWND      g_edit_ctl = NULL;
static HWND      g_edit_ctl_xml = NULL;
static HWND      g_edit_tab = NULL;
static int       g_edit_tab_index = 0;
static char     *g_edit_xml_original = NULL;
static char     *g_edit_result_utf8 = NULL;
static HACCEL    g_edit_accel = NULL;
static int       g_run_view_pending = 0;
static int       g_layout_epoch = 0;
static int       g_editor_class_registered = 0;

/* ------------------------------------------------------------------
 * Inline caret editing (basato sulla layout cache).
 * Il testo di lavoro e' una copia del testo plain del paragrafo: il commit
 * usa ld_paragraph_set_text, che appiattisce la formattazione dei run.
 * ------------------------------------------------------------------ */
#define LD_CARET_DEBOUNCE_MS  300
#define LD_CARET_TIMER_ID     31

static int       g_caret_active = 0;
static int       g_caret_para = -1;
static int       g_caret_offset = 0;      /* offset byte in g_caret_text */
static int       g_caret_gen = -1;        /* generation cache all'apertura */
static int       g_caret_timer = 0;       /* 1 = commit pendente */
static int       g_caret_reanchor = 0;    /* ri-ancora dopo il render */
static char     *g_caret_text = NULL;     /* testoplain modificabile (UTF-8) */
static char     *g_caret_original = NULL; /* testo originale, per confronto */
static RECT      g_caret_rc = {0};        /* box caret in coordinate documento */

/* Shared function declarations */
void log_xml_block(const char *tag, int index, const char *xml);

/* Inline caret (viewer_caret.c) */
int  caret_begin_from_point(int doc_x, int doc_y);
void caret_close(void);
void caret_draw(HWND hwnd, HDC hdc);
void caret_on_char(const char *utf8);
void caret_on_wchar(WCHAR ch);
void caret_on_key(int vk);
void caret_on_timer(void);
void caret_on_zoom_or_scroll(void);
void caret_reanchor_after_render(void);


/* REGRESSION WARNING: layout_content_width() = page_w - LAYOUT_PADDING_X is the
 * LAYOUT width passed to ld_render, while paint uses the full page width
 * (LAY->page_w - 2*LAYOUT_VISUAL_PADDING_X). The root-level line boxes get the
 * layout box centered inside the paint box (offset ~177px). This only works
 * because litehtml clamps line-box layout bounds to the containing block
 * width (see REGRESSION WARNING in litehtml render_inline_context.cpp).
 * If that clamp is removed or broken again, table-cell text of the first
 * column will render inside the second column. */