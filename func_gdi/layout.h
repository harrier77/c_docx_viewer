/*
 * layout.h - stato e calcoli di pagina, zoom e scroll.
 *
 * Estratto da docxview_new.c. Nessuna dipendenza da GDI/renderer:
 * il layout produce solo coordinate intere in pixel schermo.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#include <windows.h>

/* Geometria della pagina (ex docxview_new.c) */
#define PAGE_MAX_W        800
#define PAGE_DESK_MARGIN   24
#define PAGE_TOP_GAP       20
#define PAGE_SHADOW_OFF     4

/* Padding: valori legati tra loro, non cambiare uno senza gli altri. */
#define LAYOUT_PADDING_X         202
#define LAYOUT_VISUAL_PADDING_X   18
#define LAYOUT_VISUAL_MARGIN_Y     6

/* Limiti zoom (ex docxview_new.c) */
#define LAYOUT_ZOOM_STEP 0.1
#define LAYOUT_ZOOM_MIN  0.90
#define LAYOUT_ZOOM_MAX  1.5

typedef struct Layout
{
    int    cw, ch;          /* area client */
    int    page_w, page_x;  /* pagina non scalata */
    double zoom;
    int    scroll;          /* scroll verticale, pixel schermo */
    int    scroll_x;
    double page_left_doc;   /* bordo sinistro pagina in coordinate documento */
} Layout;

Layout *layout_get(void);

void layout_init(int cw, int ch);
void layout_resize(int cw, int ch);
void layout_set_zoom(double zoom);

/* Ancora il punto documento sotto il cursore e ricalcola scroll/posizione. */
void layout_zoom_at(double new_zoom, int cursor_x, int cursor_y, int doc_h);

int  layout_content_width(void);
int  layout_page_top(void);
int  layout_max_scroll(int doc_h);

void layout_update_scrollbars(HWND hwnd, int doc_h);

void layout_page_rect(int doc_h, RECT *page, RECT *shadow);
void layout_content_rect(int doc_h, RECT *content);
void layout_paint_rects(int doc_h, RECT *page, RECT *shadow, RECT *content, RECT *clip);

#endif /* LAYOUT_H */
