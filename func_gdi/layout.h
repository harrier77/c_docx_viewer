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
    int    scroll_x;        /* pan orizzontale (px di layout, >0 = pagina a sinistra) */
    double page_left_doc;   /* bordo sinistro pagina in coordinate documento */
    double view_scale;      /* px schermo per px di layout (GDI: 1.0; D2D: zoom) */
    double pan_rem_x;       /* resto frazionario del pan (px di layout) */
    double pan_rem_y;
} Layout;

Layout *layout_get(void);

void layout_init(int cw, int ch);
void layout_resize(int cw, int ch);
void layout_set_zoom(double zoom);

/* Ancora il punto documento sotto il cursore e ricalcola scroll/posizione.
 * Tiene conto di scroll_x (pan orizzontale). */
void layout_zoom_at(double new_zoom, int cursor_x, int cursor_y, int doc_h);

/* Fattore px schermo / px di layout usato per pan e limiti orizzontali.
 *   GDI : 1.0            (lo zoom e' cotto nel layout e nei font, quindi
 *                         scroll/scroll_x sono gia' in pixel schermo)
 *   D2D : g_zoom_d2d     (il layout resta a 1.0, scroll/scroll_x sono in
 *                         pixel di layout e il renderer li scala)
 * Va impostato dal chiamante ad ogni cambio zoom/dimensioni. */
void layout_set_view_scale(double scale);

/* Pan di (dx_screen, dy_screen) pixel SCHERMO (delta del mouse).
 * Sposta il contenuto aggiornando scroll_x (orizzontale) e scroll
 * (verticale) in coordinate di layout: nessun re-layout, clip, scrollbar
 * e ancoraggio dello zoom restano coerenti.
 * Convenzione: trascinando verso destra (dx>0) il contenuto segue il mouse.
 * I delta sono accumulati con il resto frazionario (pan_rem_*) perche' la
 * conversione schermo->layout divide per view_scale: senza accumulatore,
 * con zoom D2D frazionario (es. 1.5) l'arrotondamento per evento farebbe
 * muovere il contenuto piu' velocemente del cursore. */
void layout_pan(int dx_screen, int dy_screen, int doc_h);

/* Riallinea scroll_x ai limiti di pagina (px di layout). */
void layout_clamp_scroll_x(void);

/* Compensa il cambio di zoom del renderer: quando la scala passa da z_old a
 * z_new il disegno e' ancorato all'origine (screen = layout * z), quindi
 * senza compensazione la pagina (centrata) trasla verso destra ad ogni
 * zoom. Qui si spostano scroll/scroll_x, cosi' che:
 *   - orizzontalmente resta fermo il CENTRO della pagina;
 *   - verticalmente resta fermo il contenuto al centro della vista.
 * Nessun re-layout: si toccano solo le coordinate di scorrimento, e la
 * mappatura resta quella di layout (clip, scrollbar e culling coerenti).
 * Va chiamata DOPO layout_set_view_scale(zoom nuovo). */
void layout_zoom_view(double z_old, double z_new, int doc_h);

int  layout_content_width(void);
int  layout_page_top(void);
int  layout_max_scroll(int doc_h);

void layout_update_scrollbars(HWND hwnd, int doc_h);

void layout_page_rect(int doc_h, RECT *page, RECT *shadow);
void layout_content_rect(int doc_h, RECT *content);
void layout_paint_rects(int doc_h, RECT *page, RECT *shadow, RECT *content, RECT *clip);

#endif /* LAYOUT_H */
