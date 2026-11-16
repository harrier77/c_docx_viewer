/*
 * viewer_callbacks.c - callback litedocx verso il renderer Direct2D.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

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
