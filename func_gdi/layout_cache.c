/*
 * layout_cache.c - Implementazione cache layout in memoria.
 */
#include "layout_cache.h"
#include "debug_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* Helper allocazione                                                        */
/* ------------------------------------------------------------------------- */

static LayoutParagraph* para_alloc(void)
{
    LayoutParagraph *p = (LayoutParagraph*)calloc(1, sizeof(LayoutParagraph));
    if (p) {
        p->type = LAYOUT_EL_PARAGRAPH;
    }
    return p;
}

static TextRun* run_alloc(const char *text, int x, int y, int w, int h,
                          void *font_handle, COLORREF color, int underline, int strike)
{
    TextRun *r = (TextRun*)calloc(1, sizeof(TextRun));
    if (!r) return NULL;

    if (text && text[0]) {
        size_t len = strlen(text);
        r->text = (char*)malloc(len + 1);
        if (!r->text) {
            free(r);
            return NULL;
        }
        memcpy(r->text, text, len + 1);
    }
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
    r->font_handle = font_handle;
    r->color = color;
    r->underline = underline;
    r->strike = strike;
    r->char_start = -1;
    r->char_end = -1;
    return r;
}

static void run_free(TextRun *r)
{
    if (!r) return;
    free(r->text);
    free(r);
}

static void runs_free_list(TextRun *head)
{
    TextRun *cur = head;
    while (cur) {
        TextRun *next = cur->next;
        run_free(cur);
        cur = next;
    }
}

static void para_free_recursive(LayoutParagraph *p)
{
    if (!p) return;
    para_free_recursive(p->first_child);
    para_free_recursive(p->next_sibling);
    runs_free_list(p->runs);
    free(p);
}

/* ------------------------------------------------------------------------- */
/* LayoutCache API                                                           */
/* ------------------------------------------------------------------------- */

LayoutCache* layout_cache_create(void)
{
    LayoutCache *cache = (LayoutCache*)calloc(1, sizeof(LayoutCache));
    if (!cache) return NULL;

    cache->root = para_alloc();
    if (!cache->root) {
        free(cache);
        return NULL;
    }
    cache->root->id = 0; /* Document root */
    cache->valid = 0;
    cache->generation = 0;
    return cache;
}

void layout_cache_destroy(LayoutCache *cache)
{
    if (!cache) return;
    para_free_recursive(cache->root);
    free(cache->pages);
    free(cache);
}

void layout_cache_invalidate(LayoutCache *cache)
{
    if (!cache) return;
    if (cache->root) {
        /* Libera tutto tranne la radice */
        para_free_recursive(cache->root->first_child);
        cache->root->first_child = NULL;
        runs_free_list(cache->root->runs);
        cache->root->runs = NULL;
        cache->root->runs_tail = NULL;
    }
    free(cache->pages);
    cache->pages = NULL;
    cache->paragraph_count = 0;
    cache->total_width = 0;
    cache->total_height = 0;
    cache->page_count = 0;
    cache->valid = 0;
    cache->generation++;
}

void layout_cache_set_layout_params(LayoutCache *cache,
                                    int layout_width,
                                    int justification_width,
                                    int visual_padding_x,
                                    int visual_padding_y)
{
    if (!cache) return;
    cache->layout_width = layout_width;
    cache->justification_width = justification_width > 0 ? justification_width : layout_width;
    cache->visual_padding_x = visual_padding_x;
    cache->visual_padding_y = visual_padding_y;
}

/* ------------------------------------------------------------------------- */
/* Costruzione cache (chiamato dai callback litedocx)                        */
/* ------------------------------------------------------------------------- */

/* Aggiunge un paragrafo come figlio di parent (o root se parent=NULL).
 * Restituisce il nuovo paragrafo o NULL se OOM. */
LayoutParagraph* layout_cache_add_paragraph(LayoutCache *cache,
                                            LayoutParagraph *parent,
                                            int id, int x, int y, int w, int h,
                                            LayoutAlignment alignment,
                                            int line_spacing,
                                            int space_before,
                                            int space_after,
                                            int indent_left,
                                            int indent_right,
                                            int indent_first)
{
    if (!cache) return NULL;

    LayoutParagraph *p = para_alloc();
    if (!p) return NULL;

    p->id = id;
    p->x = x;
    p->y = y;
    p->w = w;
    p->h = h;
    p->content_x = x;
    p->content_y = y;
    p->content_w = w;
    p->content_h = h;
    p->alignment = alignment;
    p->line_spacing = line_spacing;
    p->space_before = space_before;
    p->space_after = space_after;
    p->indent_left = indent_left;
    p->indent_right = indent_right;
    p->indent_first = indent_first;

    if (!parent) parent = cache->root;
    p->parent = parent;

    /* Inserisci in coda ai figli di parent */
    if (!parent->first_child) {
        parent->first_child = p;
    } else {
        LayoutParagraph *sib = parent->first_child;
        while (sib->next_sibling) sib = sib->next_sibling;
        sib->next_sibling = p;
        p->prev_sibling = sib;
    }

    cache->paragraph_count++;
    return p;
}

/* Aggiunge un text run a un paragrafo. Restituisce 1 se OK, 0 se OOM. */
int layout_cache_add_run(LayoutParagraph *para,
                         const char *text, int x, int y, int w, int h,
                         void *font_handle, COLORREF color,
                         int underline, int strike)
{
    if (!para) return 0;

    TextRun *r = run_alloc(text, x, y, w, h, font_handle, color, underline, strike);
    if (!r) return 0;

    if (!para->runs) {
        para->runs = r;
        para->runs_tail = r;
    } else {
        para->runs_tail->next = r;
        para->runs_tail = r;
    }
    para->run_count++;
    return 1;
}

/* Aggiorna bounding box totale cache. */
void layout_cache_update_bounds(LayoutCache *cache)
{
    if (!cache || !cache->root) return;
    int max_x = 0, max_y = 0;

    /* DFS per trovare estremi */
    struct Stack { LayoutParagraph *p; struct Stack *next; } *stack = NULL;
    LayoutParagraph *p = cache->root->first_child;
    while (p) {
        struct Stack *s = (struct Stack*)malloc(sizeof(struct Stack));
        s->p = p;
        s->next = stack;
        stack = s;
        p = p->next_sibling;
    }

    while (stack) {
        struct Stack *s = stack;
        stack = s->next;
        LayoutParagraph *cur = s->p;
        free(s);

        int right = cur->x + cur->w;
        int bottom = cur->y + cur->h;
        if (right > max_x) max_x = right;
        if (bottom > max_y) max_y = bottom;

        /* Push children */
        LayoutParagraph *child = cur->first_child;
        while (child) {
            struct Stack *ns = (struct Stack*)malloc(sizeof(struct Stack));
            ns->p = child;
            ns->next = stack;
            stack = ns;
            child = child->next_sibling;
        }
        /* Push siblings */
        if (cur->next_sibling) {
            struct Stack *ns = (struct Stack*)malloc(sizeof(struct Stack));
            ns->p = cur->next_sibling;
            ns->next = stack;
            stack = ns;
        }
    }

    cache->total_width = max_x;
    cache->total_height = max_y;
}

/* ------------------------------------------------------------------------- */
/* Query                                                                     */
/* ------------------------------------------------------------------------- */

const LayoutParagraph* layout_cache_find_paragraph_at(LayoutCache *cache, int x, int y)
{
    if (!cache || !cache->valid || !cache->root) return NULL;

    /* DFS - restituisce il paragrafo PIU' PROFONDO che contiene il punto
     * (quello foglia, non il contenitore) */
    const LayoutParagraph *found = NULL;
    struct Stack { const LayoutParagraph *p; struct Stack *next; } *stack = NULL;
    const LayoutParagraph *p = cache->root->first_child;
    while (p) {
        struct Stack *s = (struct Stack*)malloc(sizeof(struct Stack));
        s->p = p;
        s->next = stack;
        stack = s;
        p = p->next_sibling;
    }

    while (stack) {
        struct Stack *s = stack;
        stack = s->next;
        const LayoutParagraph *cur = s->p;
        free(s);

        if (x >= cur->x && x < cur->x + cur->w &&
            y >= cur->y && y < cur->y + cur->h) {
            found = cur; /* Più profondo vince (DFS pre-order) */
        }

        /* Push children first (per depth-first) */
        const LayoutParagraph *child = cur->first_child;
        while (child) {
            struct Stack *ns = (struct Stack*)malloc(sizeof(struct Stack));
            ns->p = child;
            ns->next = stack;
            stack = ns;
            child = child->next_sibling;
        }
        /* Push siblings */
        if (cur->next_sibling) {
            struct Stack *ns = (struct Stack*)malloc(sizeof(struct Stack));
            ns->p = cur->next_sibling;
            ns->next = stack;
            stack = ns;
        }
    }

    return found;
}

const TextRun* layout_cache_find_run_at(LayoutCache *cache, int x, int y, const LayoutParagraph **para_out)
{
    if (!cache || !cache->valid) return NULL;

    const LayoutParagraph *para = layout_cache_find_paragraph_at(cache, x, y);
    if (!para) return NULL;

    if (para_out) *para_out = para;

    const TextRun *r = para->runs;
    while (r) {
        if (x >= r->x && x < r->x + r->w &&
            y >= r->y && y < r->y + r->h) {
            return r;
        }
        r = r->next;
    }

    /* Gli spazi tra le parole non vengono emessi come run da litehtml:
     * se il punto e' nel gap, ancorati al run piu' vicino della stessa
     * riga. A parita' di distanza scegli quello a sinistra, cosi' il
     * caret finisce in fondo alla parola precedente. */
    const TextRun *best = NULL;
    int best_dx = 0;
    for (r = para->runs; r; r = r->next) {
        if (r->w <= 0 || r->h <= 0) continue;
        if (y < r->y || y >= r->y + r->h) continue;
        int dx = (x < r->x) ? (r->x - x) : (x - (r->x + r->w));
        if (dx < 0) dx = 0;
        if (!best || dx < best_dx ||
            (dx == best_dx && r->x < best->x)) {
            best = r;
            best_dx = dx;
        }
    }
    if (best) return best;

    /* Punto in un'interlinea: nessun run copre la riga. Prendi il piu'
     * vicino in verticale e, a parita', in orizzontale. Se il click e'
     * oltre il bordo destro del run scelto, ancoralo alla fine del run. */
    int best_dy = 0;
    best_dx = 0;
    for (r = para->runs; r; r = r->next) {
        if (r->w <= 0 || r->h <= 0) continue;
        int dy = (y < r->y) ? (r->y - y) : (y - (r->y + r->h));
        int dx = (x < r->x) ? (r->x - x) : (x - (r->x + r->w));
        if (dy < 0) dy = 0;
        if (dx < 0) dx = 0;
        if (!best || dy < best_dy ||
            (dy == best_dy && dx < best_dx) ||
            (dy == best_dy && dx == best_dx && r->x < best->x)) {
            best = r;
            best_dy = dy;
            best_dx = dx;
        }
    }
    /* layout_cache_find_run_at returns a const run from cache; callers use it
     * read-only for hit testing. We do not mutate cache here. The caret uses
     * the click x against run bounds, so allow selecting right edge by
     * returning the run as-is; caret_offset_in_run already clamps. */
    return best;
}

void layout_cache_enum_paragraphs(LayoutCache *cache, LayoutCacheEnumCallback cb, void *ctx)
{
    if (!cache || !cache->valid || !cb) return;

    struct Stack { LayoutParagraph *p; struct Stack *next; } *stack = NULL;
    LayoutParagraph *p = cache->root->first_child;
    while (p) {
        struct Stack *s = (struct Stack*)malloc(sizeof(struct Stack));
        s->p = p;
        s->next = stack;
        stack = s;
        p = p->next_sibling;
    }

    while (stack) {
        struct Stack *s = stack;
        stack = s->next;
        LayoutParagraph *cur = s->p;
        free(s);

        cb(cur, ctx);

        /* Push children first */
        LayoutParagraph *child = cur->first_child;
        while (child) {
            struct Stack *ns = (struct Stack*)malloc(sizeof(struct Stack));
            ns->p = child;
            ns->next = stack;
            stack = ns;
            child = child->next_sibling;
        }
        /* Push siblings */
        if (cur->next_sibling) {
            struct Stack *ns = (struct Stack*)malloc(sizeof(struct Stack));
            ns->p = cur->next_sibling;
            ns->next = stack;
            stack = ns;
        }
    }
}

void layout_cache_enum_runs(const LayoutParagraph *para, void (*cb)(const TextRun*, void*), void *ctx)
{
    if (!para || !cb) return;
    const TextRun *r = para->runs;
    while (r) {
        cb(r, ctx);
        r = r->next;
    }
}

/* ------------------------------------------------------------------------- */
/* Paginazione                                                               */
/* ------------------------------------------------------------------------- */

void layout_cache_set_pages(LayoutCache *cache, int page_count, const int *page_heights, const int *page_y_offsets)
{
    if (!cache) return;
    free(cache->pages);
    cache->pages = NULL;
    cache->page_count = 0;

    if (page_count <= 0) return;

    cache->pages = (LayoutPageInfo*)calloc(page_count, sizeof(LayoutPageInfo));
    if (!cache->pages) return;

    cache->page_count = page_count;
    for (int i = 0; i < page_count; i++) {
        cache->pages[i].page_index = i;
        cache->pages[i].height = page_heights ? page_heights[i] : 0;
        cache->pages[i].y_offset = page_y_offsets ? page_y_offsets[i] : 0;
        /* content_top/bottom verranno calcolati dal chiamante se necessario */
    }
}

const LayoutPageInfo* layout_cache_get_page(LayoutCache *cache, int page_index)
{
    if (!cache || page_index < 0 || page_index >= cache->page_count) return NULL;
    return &cache->pages[page_index];
}

int layout_cache_find_page_at_y(LayoutCache *cache, int y)
{
    if (!cache || cache->page_count == 0) return -1;
    for (int i = 0; i < cache->page_count; i++) {
        int top = cache->pages[i].y_offset;
        int bottom = top + cache->pages[i].height;
        if (y >= top && y < bottom) return i;
    }
    /* Se oltre l'ultima pagina, restituisci ultima */
    if (y >= cache->pages[cache->page_count - 1].y_offset)
        return cache->page_count - 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Popolazione da litedocx (chiamato dopo ld_draw con callback)              */
/* ------------------------------------------------------------------------- */

/* Stato temporaneo durante populate */
typedef struct PopulateState {
    LayoutCache *cache;
    LayoutParagraph *current_para;  /* Paragrafo corrente per accumulare run */
    int last_para_id;               /* Per rilevare cambio paragrafo */
} PopulateState;

/* Callback da litedocx: inizio paragrafo */
static void populate_on_paragraph_start(void *user, int id, int x, int y, int w, int h,
                                        int alignment, int line_spacing,
                                        int space_before, int space_after,
                                        int indent_left, int indent_right, int indent_first)
{
    PopulateState *st = (PopulateState*)user;
    if (!st || !st->cache) return;

    LayoutAlignment align = (LayoutAlignment)alignment;
    st->current_para = layout_cache_add_paragraph(st->cache, NULL, id, x, y, w, h,
                                                  align, line_spacing,
                                                  space_before, space_after,
                                                  indent_left, indent_right, indent_first);
    st->last_para_id = id;
}

/* Callback da litedocx: text run */
static void populate_on_text_run(void *user, int para_id, const char *text, int x, int y, int w, int h,
                                 void *font_handle, unsigned color, int underline, int strike)
{
    PopulateState *st = (PopulateState*)user;
    if (!st || !st->cache || !st->current_para) return;

    /* Se para_id cambiato, il paragrafo è già stato switchato da on_paragraph_start */
    if (para_id != st->last_para_id) {
        /* Paragrafo senza on_paragraph_start? Crea implicitamente */
        st->current_para = layout_cache_add_paragraph(st->cache, NULL, para_id, x, y, w, h,
                                                      LAYOUT_ALIGN_LEFT, 0, 0, 0, 0, 0, 0);
        st->last_para_id = para_id;
    }

    layout_cache_add_run(st->current_para, text, x, y, w, h,
                         font_handle, (COLORREF)color, underline, strike);
}

/* Callback da litedocx: fine paragrafo */
static void populate_on_paragraph_end(void *user, int id)
{
    PopulateState *st = (PopulateState*)user;
    if (!st) return;
    st->current_para = NULL;
    st->last_para_id = -1;
}

/* Callback da litedocx: background */
static void populate_on_background(void *user, int x, int y, int w, int h, unsigned color)
{
    (void)user; (void)x; (void)y; (void)w; (void)h; (void)color;
    /* Per ora ignoriamo: i background sono disegnati dal renderer */
}

/* Callback da litedocx: border */
static void populate_on_border(void *user, int x, int y, int w, int h, int side, int width, unsigned color)
{
    (void)user; (void)x; (void)y; (void)w; (void)h; (void)side; (void)width; (void)color;
}

/* Callback da litedocx: image */
static void populate_on_image(void *user, int x, int y, int w, int h, const char *src)
{
    (void)user; (void)x; (void)y; (void)w; (void)h; (void)src;
}

/* Callback da litedocx: list marker */
static void populate_on_list_marker(void *user, int x, int y, int w, int h,
                                    int type, unsigned color, const char *image)
{
    (void)user; (void)x; (void)y; (void)w; (void)h; (void)type; (void)color; (void)image;
}

/* Callback da litedocx: table cell */
static void populate_on_table_cell(void *user, int table_idx, int row, int col,
                                   int x, int y, int w, int h,
                                   int is_header, unsigned bg_color,
                                   int border_left_w, int border_top_w,
                                   int border_right_w, int border_bottom_w,
                                   unsigned border_left_c, unsigned border_top_c,
                                   unsigned border_right_c, unsigned border_bottom_c,
                                   int padding_left, int padding_top,
                                   int padding_right, int padding_bottom,
                                   const char *valign)
{
    (void)user; (void)table_idx; (void)row; (void)col; (void)x; (void)y; (void)w; (void)h;
    (void)is_header; (void)bg_color; (void)border_left_w; (void)border_top_w;
    (void)border_right_w; (void)border_bottom_w; (void)border_left_c; (void)border_top_c;
    (void)border_right_c; (void)border_bottom_c; (void)padding_left; (void)padding_top;
    (void)padding_right; (void)padding_bottom; (void)valign;
}

/* Tabella callback per litedocx */
static const struct {
    void (*on_paragraph_start)(void*, int, int, int, int, int, int, int, int, int, int, int, int);
    void (*on_text_run)(void*, int, const char*, int, int, int, int, void*, unsigned, int, int);
    void (*on_paragraph_end)(void*, int);
    void (*on_background)(void*, int, int, int, int, unsigned);
    void (*on_border)(void*, int, int, int, int, int, int, unsigned);
    void (*on_image)(void*, int, int, int, int, const char*);
    void (*on_list_marker)(void*, int, int, int, int, int, unsigned, const char*);
    void (*on_table_cell)(void*, int, int, int, int, int, int, int, int, unsigned,
                          int, int, int, int, unsigned, unsigned, unsigned, unsigned,
                          int, int, int, int, const char*);
} g_populate_callbacks = {
    populate_on_paragraph_start,
    populate_on_text_run,
    populate_on_paragraph_end,
    populate_on_background,
    populate_on_border,
    populate_on_image,
    populate_on_list_marker,
    populate_on_table_cell
};

/* Espone i callback per litedocx */
const void* layout_cache_get_populate_callbacks(void)
{
    return &g_populate_callbacks;
}

int layout_cache_populate(LayoutCache *cache, void *doc_handle)
{
    if (!cache) return 0;

    /* Invalida prima (mantiene struttura) */
    layout_cache_invalidate(cache);

    /* Imposta parametri layout se non già fatti */
    if (cache->layout_width == 0) {
        cache->layout_width = 800; /* default, sarà sovrascritto da viewer */
    }

    /* Chiama litedocx per estrarre layout via callback */
    /* NOTA: l'implementazione reale sarà in litedocx_c.cpp che chiama
     * ld_extract_layout(doc, &callbacks, user_data).
     * Qui prepariamo solo lo stato. */

    (void)doc_handle;
    PopulateState state = {0};
    state.cache = cache;
    state.last_para_id = -1;
    (void)state; /* usato quando ld_extract_layout sarà implementato */

    /* TODO: Chiamata a ld_extract_layout(doc_handle, &g_populate_callbacks, &state);
     * Per ora simuliamo: la chiamata vera avverrà da viewer_render.c dopo aver
     * registrato i callback in litedocx. */

    /* Aggiorna bounds */
    layout_cache_update_bounds(cache);

    cache->valid = 1;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Debug                                                                     */
/* ------------------------------------------------------------------------- */

static void dump_para_cb(const LayoutParagraph *p, void *ctx)
{
    (void)ctx;
    DLOG("  Para id=%d (%d,%d) %dx%d runs=%d align=%d\n",
            p->id, p->x, p->y, p->w, p->h, p->run_count, p->alignment);
    const TextRun *r = p->runs;
    while (r) {
        DLOG("    Run '%.*s' (%d,%d) %dx%d font=%p col=0x%06lX\n",
                r->text ? (int)strnlen(r->text, 40) : 0, r->text ? r->text : "",
                r->x, r->y, r->w, r->h, r->font_handle, (unsigned long)r->color);
        r = r->next;
    }
}

void layout_cache_dump_stats(const LayoutCache *cache)
{
    if (!cache) {
        DLOG("[LAYOUT_CACHE] NULL\n");
        return;
    }
    DLOG("[LAYOUT_CACHE] valid=%d gen=%d paras=%d size=%dx%d pages=%d\n",
            cache->valid, cache->generation, cache->paragraph_count,
            cache->total_width, cache->total_height, cache->page_count);
    if (cache->valid && cache->root) {
        layout_cache_enum_paragraphs((LayoutCache*)cache, dump_para_cb, NULL);
    }
}