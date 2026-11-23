#include "litedocx/include/litedocx_c.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void *cb_create_font(const char *family, int size, int bold, int italic,
                            int underline, int strike, lh_font_metrics *fm)
{
    (void)family; (void)bold; (void)italic; (void)underline; (void)strike;
    if(fm) {
        fm->font_size   = size;
        fm->height      = (int)(((size * 0.8f) + (size * 0.2f) + (size * 0.15f)) * 1.15f);
        fm->ascent      = (int)(size * 0.8f + 0.5f);
        fm->descent     = (int)(size * 0.2f + 0.5f);
        fm->x_height    = (int)(size * 0.8f * 0.5f + 0.5f);
        fm->ch_width    = (int)(size * 0.55f);
        fm->draw_spaces = 0;
        fm->sub_shift   = size / 3;
        fm->super_shift = size / 3;
    }
    return (void *)(size_t)(size ? size : 1);
}
static void cb_delete_font(void *font) { (void)font; }
static int cb_text_width(const char *text, void *font)
{
    int size = (int)(size_t)font;
    return (int)strlen(text) * size / 2;
}
static void cb_draw_text(void *hdc, const char *text, void *font, unsigned rgb,
                         int x, int y, int w, int h)
{ (void)hdc;(void)text;(void)font;(void)rgb;(void)x;(void)y;(void)w;(void)h; }
static void cb_solid(void *hdc, int x, int y, int w, int h, unsigned rgb)
{ (void)hdc;(void)x;(void)y;(void)w;(void)h;(void)rgb; }
static void cb_borders(void *hdc, int x, int y, int w, int h,
                       int lw, int tw, int rw, int bw,
                       unsigned lc, unsigned tc, unsigned rc, unsigned bc)
{ (void)hdc;(void)x;(void)y;(void)w;(void)h;(void)lw;(void)tw;(void)rw;(void)bw;
  (void)lc;(void)tc;(void)rc;(void)bc; }

int main()
{
    ld_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.lh.create_font = cb_create_font;
    cb.lh.delete_font = cb_delete_font;
    cb.lh.text_width = cb_text_width;
    cb.lh.draw_text = cb_draw_text;
    cb.lh.draw_solid_fill = cb_solid;
    cb.lh.draw_borders = cb_borders;

    void *doc = ld_open_file("dev/fixture.docx", &cb, 800, 600);
    if (!doc) { printf("Failed to open doc\n"); return 1; }
    ld_render(doc, 610);

    for (int p = 0; p < 10; p++) {
        char *t = ld_paragraph_get_text(doc, p);
        if (t) {
            printf("P%d: '%s'\n", p, t);
            ld_free_string(t);
        }
    }

    ld_run_preview q;
    memset(&q, 0, sizeof(q));
    // Let's test preview for para 4, run 0
    // First, caret position at anchor 12 ("Testo finale")
    ld_caret_hit ch;
    if (ld_run_caret_pos(doc, 4, 0, 12, &ch)) {
        printf("Caret at 12: x=%d, y=%d, h=%d\n", ch.caret_x, ch.caret_y, ch.caret_h);
    }
    if (ld_run_preview_info_ex(doc, 4, 0, ch.caret_y, 12, LD_PREVIEW_APPEND, &q)) {
        printf("Preview append: x=%d, y=%d, w=%d, h=%d, cs=%d, ce=%d, safe_w=%d, caret_x=%d\n",
               q.x, q.y, q.w, q.h, q.char_start, q.char_end, q.safe_w, q.caret_x);
    } else {
        printf("Preview append returned 0!\n");
    }

    ld_destroy(doc);
    return 0;
}
