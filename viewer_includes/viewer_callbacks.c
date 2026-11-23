/*
 * viewer_callbacks.c - callback litedocx verso il renderer Direct2D.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

#include "layout_cache.h"

static void cb_draw_text(void *hdc, const char *text, void *font, unsigned rgb, int x, int y, int w, int h)
{
    // log_msg("[DRAW_TEXT] text='%s' at (%d,%d) w=%d h=%d\n", text ? text : "NULL", x, y, w, h);
    (void)hdc;
    if (!text || !text[0])
        return;
    static int s_text_count = 0;
    if (s_text_count++ < 200)
        dbg_log("[TABLE] cb_draw_text text='%.60s' at (%d,%d) size=%dx%d\n", text, x, y, w, h);
    DLOG("[FONT_DRAW] text='%.30s' font=%p at (%d,%d) zoom=%.2f\n", text ? text : "NULL", font, x, y, g_zoom);
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

/* ------------------------------------------------------------------------- */
/* Layout Cache Callbacks                                                    */
/* ------------------------------------------------------------------------- */

static void cache_on_paragraph_start(void *user,
                                     int id, int x, int y, int w, int h,
                                     int alignment, int line_spacing,
                                     int space_before, int space_after,
                                     int indent_left, int indent_right, int indent_first)
{
    (void)user;
    DLOG("[CACHE_CB] para_start id=%d pos=(%d,%d) size=(%d,%d) align=%d\n",
            id, x, y, w, h, alignment);
    fflush(stderr);
    if (!g_layout_cache)
        return;
    layout_cache_add_paragraph(g_layout_cache, NULL, id, x, y, w, h,
                               (LayoutAlignment)alignment, line_spacing,
                               space_before, space_after,
                               indent_left, indent_right, indent_first);
}

static void cache_on_text_run(void *user,
                              int para_id, const char *text, int x, int y, int w, int h,
                              void *font_handle, unsigned color, int underline, int strike)
{
    (void)user;
    DLOG("[CACHE_CB] text_run para_id=%d text='%.20s' pos=(%d,%d) size=(%d,%d)\n",
            para_id, text ? text : "NULL", x, y, w, h);
    fflush(stderr);
    if (!g_layout_cache)
        return;
    /* Trova il paragrafo per ID (semplificato: usa l'ultimo aggiunto) */
    LayoutParagraph *para = g_layout_cache->root ? g_layout_cache->root->first_child : NULL;
    while (para && para->id != para_id) {
        para = para->next_sibling;
    }
    if (para) {
        if (layout_cache_add_run(para, text, x, y, w, h,
                                 font_handle, (COLORREF)color, underline, strike)) {
            /* Copia l'identita' del font nel run: il puntatore puo' essere
             * liberato dalla DLL, i campi restano validi per il caret. */
            TextRun *r = para->runs_tail;
            if (r && font_handle) {
                DwFontHandle *fh = (DwFontHandle *)font_handle;
                r->font_size = fh->size_px;
                r->bold = fh->bold;
                r->italic = fh->italic;
                lstrcpynA(r->family, fh->family, sizeof(r->family));
            }
        }
    }
}

static void cache_on_paragraph_end(void *user, int id)
{
    (void)user; (void)id;
    DLOG("[CACHE_CB] para_end id=%d\n", id);
    fflush(stderr);
    /* Nulla da fare per ora */
}

static void cache_on_background(void *user, int x, int y, int w, int h, unsigned color)
{
    (void)user; (void)x; (void)y; (void)w; (void)h; (void)color;
    /* I background sono gestiti dal renderer, non serve memorizzarli in cache */
}

static void cache_on_border(void *user, int x, int y, int w, int h,
                            int side, int width, unsigned color)
{
    (void)user; (void)x; (void)y; (void)w; (void)h; (void)side; (void)width; (void)color;
}

static void cache_on_image(void *user, int x, int y, int w, int h, const char *src)
{
    (void)user; (void)x; (void)y; (void)w; (void)h; (void)src;
}

static void cache_on_list_marker(void *user, int x, int y, int w, int h,
                                 int type, unsigned color, const char *image)
{
    (void)user; (void)x; (void)y; (void)w; (void)h; (void)type; (void)color; (void)image;
}

static void cache_on_table_cell(void *user,
                                int table_idx, int row, int col,
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

/* Struttura callback per litedocx */
static const ld_layout_cache_ops g_cache_ops = {
    cache_on_paragraph_start,
    cache_on_text_run,
    cache_on_paragraph_end,
    cache_on_background,
    cache_on_border,
    cache_on_image,
    cache_on_list_marker,
    cache_on_table_cell
};

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