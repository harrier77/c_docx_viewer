/*
 * layout.c - implementazione dello stato di layout.
 *
 * Le formule sono identiche a quelle di docxview_new.c: il refactoring
 * non deve cambiare ne' la geometria ne' la resa.
 */
#include "layout.h"

static Layout g_layout = {
    900, 700,           /* cw, ch */
    PAGE_MAX_W, 0,      /* page_w, page_x */
    1.0,                /* zoom */
    0, 0,               /* scroll, scroll_x */
    0.0                 /* page_left_doc */
};

Layout *layout_get(void)
{
    return &g_layout;
}

static void layout_compute_page(void)
{
    int w = g_layout.cw - 2 * PAGE_DESK_MARGIN;
    if (w > PAGE_MAX_W)
        w = PAGE_MAX_W;
    if (w < 240)
        w = g_layout.cw;
    if (w < 1)
        w = 1;
    g_layout.page_w = w;
    g_layout.page_x = (g_layout.cw - g_layout.page_w) / 2;
    if (g_layout.page_x < 0)
        g_layout.page_x = 0;
}

void layout_init(int cw, int ch)
{
    g_layout.cw = cw;
    g_layout.ch = ch;
    layout_compute_page();
    g_layout.page_left_doc = (double)g_layout.page_x;
}

void layout_resize(int cw, int ch)
{
    g_layout.cw = cw;
    g_layout.ch = ch;
    layout_compute_page();
    g_layout.page_left_doc = (double)g_layout.page_x;
}

void layout_set_zoom(double zoom)
{
    if (zoom < LAYOUT_ZOOM_MIN)
        zoom = LAYOUT_ZOOM_MIN;
    if (zoom > LAYOUT_ZOOM_MAX)
        zoom = LAYOUT_ZOOM_MAX;
    g_layout.zoom = zoom;
}

int layout_content_width(void)
{
    int w = (int)((g_layout.page_w - LAYOUT_PADDING_X) * g_layout.zoom + 1.5);
    if (w < 1)
        w = 1;
    return w;
}

int layout_page_top(void)
{
    return PAGE_TOP_GAP - g_layout.scroll;
}

int layout_max_scroll(int doc_h)
{
    int max_s = (doc_h + 120) - g_layout.ch;
    return max_s < 0 ? 0 : max_s;
}

void layout_update_scrollbars(HWND hwnd, int doc_h)
{
    SCROLLINFO si;
    int page_content_h = doc_h + 120;
    int max_s = page_content_h - g_layout.ch;

    if (max_s < 0)
        max_s = 0;
    if (g_layout.scroll > max_s)
        g_layout.scroll = max_s;

    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = page_content_h > 0 ? page_content_h - 1 : 0;
    si.nPage = g_layout.ch > 0 ? g_layout.ch : 1;
    si.nPos = g_layout.scroll;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

void layout_zoom_at(double new_zoom, int cursor_x, int cursor_y, int doc_h)
{
    double old_zoom = g_layout.zoom;
    double page_scr_left = g_layout.page_left_doc * old_zoom;
    double doc_x = ((double)cursor_x - page_scr_left) / old_zoom;
    double doc_y = ((double)g_layout.scroll + (double)cursor_y) / old_zoom;
    int max_s = layout_max_scroll(doc_h);
    int new_scroll;
    double new_page_scr_left;

    new_scroll = (int)(doc_y * new_zoom - (double)cursor_y);
    if (new_scroll < 0)
        new_scroll = 0;
    if (new_scroll > max_s)
        new_scroll = max_s;
    g_layout.scroll = new_scroll;

    g_layout.zoom = new_zoom;

    new_page_scr_left = (double)cursor_x - doc_x * new_zoom;
    g_layout.page_left_doc = new_page_scr_left / new_zoom;
}

void layout_page_rect(int doc_h, RECT *page, RECT *shadow)
{
    int pt = PAGE_TOP_GAP - g_layout.scroll;
    int pw = (int)(g_layout.page_w * g_layout.zoom);
    int px = (int)(g_layout.page_left_doc * g_layout.zoom) - g_layout.scroll_x;
    int py = pt;
    int ph = (int)((doc_h + 120) * g_layout.zoom);

    if (page) {
        page->left = px;
        page->top = py;
        page->right = px + pw;
        page->bottom = py + ph;
    }
    if (shadow) {
        shadow->left = px + PAGE_SHADOW_OFF;
        shadow->top = py + PAGE_SHADOW_OFF;
        shadow->right = px + pw + PAGE_SHADOW_OFF;
        shadow->bottom = py + ph + PAGE_SHADOW_OFF;
    }
}

void layout_content_rect(int doc_h, RECT *content)
{
    int pt, pw, px, py, ph;
    double k, dz;
    int page_margin_x, page_margin_y, content_w;

    if (!content)
        return;

    pt = PAGE_TOP_GAP - g_layout.scroll;
    pw = (int)(g_layout.page_w * g_layout.zoom);
    px = (int)(g_layout.page_left_doc * g_layout.zoom) - g_layout.scroll_x;
    py = pt;
    ph = (int)((doc_h + 120) * g_layout.zoom);

    /* Margini interni pagina: dipendenza non lineare dallo zoom (come prima). */
    dz = g_layout.zoom - 1.0;
    k = 1.0 + 1.18 * dz + 0.09 * dz * dz;
    page_margin_x = (int)(LAYOUT_VISUAL_PADDING_X * (g_layout.zoom * k) + 1.1);
    page_margin_y = (int)(LAYOUT_VISUAL_MARGIN_Y * (g_layout.zoom * 0.2));
    content_w = pw - 2 * page_margin_x;

    content->left = px + page_margin_x;
    content->top = py + page_margin_y;
    content->right = content->left + content_w;
    content->bottom = content->top + ph;
}

void layout_paint_rects(int doc_h, RECT *page, RECT *shadow, RECT *content, RECT *clip)
{
    RECT viewport;
    viewport.left = 0;
    viewport.top = 0;
    viewport.right = g_layout.cw;
    viewport.bottom = g_layout.ch;

    layout_page_rect(doc_h, page, shadow);
    layout_content_rect(doc_h, content);

    if (clip) {
        if (!IntersectRect(clip, content, &viewport)) {
            clip->left = 0;
            clip->top = 0;
            clip->right = 0;
            clip->bottom = 0;
        }
    }
}
