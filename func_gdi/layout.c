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
    0.0,                /* page_left_doc */
    1.0,                /* view_scale */
    0.0, 0.0            /* pan_rem_x, pan_rem_y */
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

void layout_set_view_scale(double scale)
{
    if (scale <= 0.0)
        scale = 1.0;
    g_layout.view_scale = scale;
}

/* Limiti di scroll_x nelle STESSE unita' di scroll_x, che sono:
 *   GDI  : pixel schermo  (px = page_left_doc*zoom   - scroll_x)
 *   D2D  : pixel di layout (px = page_left_doc*1.0    - scroll_x, poi il
 *          renderer moltiplica per g_zoom_d2d)
 * view_scale = px schermo per unita' di scroll (GDI: 1.0, D2D: zoom).
 *
 * Intervallo VOLUTAMENTE permissivo: la pagina puo' essere trascinata fino
 * a lasciare 1 px visibile ai due bordi. Un intervallo "stretto" (bordi
 * pagina a filo dei bordi viewport) sarebbe piu' elegante ma, con lo zoom
 * D2D ancorato all'origine, il clamp farebbe SALTARE la pagina al primo
 * pan dopo uno zoom. In piu' 0 (vista di default) e' sempre valido, cosi'
 * resize/zoom non spostano mai il contenuto da soli. */
static void layout_scroll_x_limits(int *lo, int *hi)
{
    double sc = g_layout.view_scale > 0.0 ? g_layout.view_scale : 1.0;
    double view_w = (double)g_layout.cw / sc;       /* viewport in unita' di scroll */
    double pl = g_layout.page_left_doc * g_layout.zoom;
    double pw = (double)g_layout.page_w * g_layout.zoom;
    double a = pl - view_w + 1.0;   /* pagina col bordo sinistro al bordo destro */
    double b = pl + pw - 1.0;       /* pagina col bordo destro al bordo sinistro */
    double t;

    /* La vista di default (scroll_x = 0) non deve mai essere fuori range. */
    if (a > 0.0)
        a = 0.0;
    if (b < 0.0)
        b = 0.0;
    if (b < a) {
        t = a;
        a = b;
        b = t;
    }
    if (lo)
        *lo = (int)(a < 0.0 ? a - 0.5 : a + 0.5);
    if (hi)
        *hi = (int)(b < 0.0 ? b - 0.5 : b + 0.5);
}

void layout_clamp_scroll_x(void)
{
    int lo = 0, hi = 0;

    layout_scroll_x_limits(&lo, &hi);
    if (g_layout.scroll_x < lo)
        g_layout.scroll_x = lo;
    if (g_layout.scroll_x > hi)
        g_layout.scroll_x = hi;
}

/* Arrotondamento a intero con .5 lontano da zero (simmetrico). */
static int round_i(double v)
{
    return (int)(v < 0.0 ? v - 0.5 : v + 0.5);
}

/* Applica un delta frazionario (in unita' di scroll) a *val, accumulando il
 * resto in *rem: senza accumulatore, l'arrotondamento per evento farebbe
 * muovere il contenuto piu' veloce del cursore quando view_scale != 1
 * (zoom D2D). Il resto si scarta se si tocca un limite, altrimenti si
 * accumulerebbe contro il bordo e farebbe saltare il contenuto al ritorno. */
static int apply_scroll_delta(int *val, double *rem, double delta,
                              int lo, int hi)
{
    int iv, want;

    *rem += delta;
    iv = round_i(*rem);
    if (!iv)
        return 0;
    *rem -= (double)iv;
    want = *val + iv;
    if (want < lo)
        want = lo;
    if (want > hi)
        want = hi;
    if (want != *val + iv)
        *rem = 0.0;
    *val = want;
    return 1;
}

void layout_pan(int dx_screen, int dy_screen, int doc_h)
{
    double sc = g_layout.view_scale > 0.0 ? g_layout.view_scale : 1.0;
    int lo, hi;

    if (dx_screen) {
        /* Trascinando a destra il contenuto segue il mouse -> scroll_x cala. */
        layout_scroll_x_limits(&lo, &hi);
        apply_scroll_delta(&g_layout.scroll_x, &g_layout.pan_rem_x,
                           -(double)dx_screen / sc, lo, hi);
    }
    if (dy_screen) {
        apply_scroll_delta(&g_layout.scroll, &g_layout.pan_rem_y,
                           -(double)dy_screen / sc, 0, layout_max_scroll(doc_h));
    }
}

void layout_zoom_view(double z_old, double z_new, int doc_h)
{
    double r, pc, cy;
    int lo, hi;

    if (z_old <= 0.0)
        z_old = 1.0;
    if (z_new <= 0.0)
        z_new = 1.0;
    if (z_old == z_new)
        return;

    r = z_old / z_new;   /* fattore di posizione residuo della vecchia scala */

    /* X: centro pagina invariato. page_left_screen = (pc - scroll_x) * z,
     * quindi scroll_x_new = pc - (pc - scroll_x_old) * r. */
    layout_scroll_x_limits(&lo, &hi);
    pc = g_layout.page_left_doc * g_layout.zoom
         + 0.5 * (double)g_layout.page_w * g_layout.zoom;
    apply_scroll_delta(&g_layout.scroll_x, &g_layout.pan_rem_x,
                       (pc - (double)g_layout.scroll_x) * (1.0 - r), lo, hi);

    /* Y: resta fermo il contenuto al centro della vista.
     * scroll_new = scroll_old + (ch/2) * (1/z_old - 1/z_new). */
    cy = 0.5 * (double)g_layout.ch;
    apply_scroll_delta(&g_layout.scroll, &g_layout.pan_rem_y,
                       cy * (1.0 / z_old - 1.0 / z_new),
                       0, layout_max_scroll(doc_h));
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

    /* Il pan orizzontale segue le dimensioni della pagina/zoom: riallineato
     * qui perche' questo e' il punto chiamato dopo resize e zoom. */
    layout_clamp_scroll_x();

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
    /* Posizione VISIBILE del bordo pagina: include il pan orizzontale.
     * (Prima scroll_x veniva ignorato: con pan attivo l'ancora sbagliava.) */
    double page_scr_left = g_layout.page_left_doc * old_zoom - g_layout.scroll_x;
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
    /* Si toglie di nuovo scroll_x: page_left_doc e' riferito alla pagina
     * non traslata dal pan (px = page_left_doc*zoom - scroll_x). */
    g_layout.page_left_doc = (new_page_scr_left + g_layout.scroll_x) / new_zoom;
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
