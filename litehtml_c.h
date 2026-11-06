/*
 * litehtml_c.h - C ABI bridge for the litehtml C++ rendering engine.
 *
 * litehtml is C++17, so it cannot be compiled by Tiny C Compiler.
 * The C++ side lives in litehtml_c.dll (built with MinGW-w64 g++), which
 * implements litehtml::document_container and forwards every drawing
 * operation to the C callbacks below. A C application (compiled with tcc)
 * only needs this header plus litehtml_c.def / liblitehtml_c.a.
 *
 * Everything is 64-bit; the DLL and the tcc application must match.
 * All HTML input is UTF-8.
 */

#ifndef LITEHTML_C_H
#define LITEHTML_C_H

#ifdef __cplusplus
extern "C"
{
#endif

/* Filled by create_font. Pixel values. */
typedef struct lh_font_metrics
{
    int font_size;   /* requested size in px */
    int height;      /* line height */
    int ascent;      /* baseline -> top */
    int descent;     /* baseline -> bottom */
    int x_height;
    int ch_width;    /* '0' advance width */
    int draw_spaces; /* 1 = draw space glyphs, 0 = skip */
    int sub_shift;
    int super_shift;
} lh_font_metrics;

/* Filled by get_media_features (CSS @media). */
typedef struct lh_media_features
{
    int type; /* 0 unknown, 1 all, 2 print, 3 screen -> see litehtml media_type */
    int width;
    int height;
    int device_width;
    int device_height;
    int color;
    int color_index;
    int monochrome;
    int resolution; /* DPI */
} lh_media_features;

/*
 * Callbacks implemented by the C application. Every pointer is optional:
 * a NULL callback becomes a no-op (create_font returning nothing will make
 * text invisible, so you normally want at least the font/text/fill ones).
 *
 * Colors are passed as Win32 COLORREF: 0x00BBGGRR  (r | g<<8 | b<<16),
 * so they can be handed straight to SetTextColor / CreateSolidBrush.
 *
 * hdc is the opaque pointer given to lh_draw (normally an HDC).
 * font is the opaque pointer returned by create_font.
 * All coordinates/sizes are integer pixels; boxes are document-relative.
 */
typedef struct lh_callbacks
{
    /* Return a font handle; fill *fm with metrics (may be NULL). */
    void* (*create_font)(const char* family, int size, int bold, int italic, int underline, int strike,
                         lh_font_metrics* fm);
    void (*delete_font)(void* font);
    int (*text_width)(const char* text, void* font);

    /* Draw one line of text inside the box; align it yourself (e.g. DrawText). */
    void (*draw_text)(void* hdc, const char* text, void* font, unsigned rgb, int x, int y, int w, int h);

    /* Background of a box (flat color). */
    void (*draw_solid_fill)(void* hdc, int x, int y, int w, int h, unsigned rgb);

    /* Borders of a box: per-side widths and colors. */
    void (*draw_borders)(void* hdc, int x, int y, int w, int h, int left_w, int top_w, int right_w, int bottom_w,
                         unsigned left_rgb, unsigned top_rgb, unsigned right_rgb, unsigned bottom_rgb);

    /* Notification / misc. */
    void (*set_caption)(const char* caption);
    void (*on_anchor_click)(const char* url);
    void (*set_cursor)(const char* cursor);
    void (*get_viewport)(int* x, int* y, int* w, int* h);
    void (*get_media_features)(lh_media_features* media);

    /* Called when a rectangle must be repainted (hover / active state). */
    void (*redraw_box)(int x, int y, int w, int h);
} lh_callbacks;

/* Create a document from an UTF-8 HTML string. Returns NULL on failure. */
void* lh_create(const char* html, const lh_callbacks* cb, int viewport_width, int viewport_height);

/* Layout for the given max width; returns the best (natural) width. */
int lh_render(void* doc, int max_width);

/* Paint into hdc, offset by (x,y), clipped to the given rectangle. */
void lh_draw(void* doc, void* hdc, int x, int y, int clip_x, int clip_y, int clip_w, int clip_h);

int lh_width(void* doc);
int lh_height(void* doc);

/* Mouse handling. x/y are document coordinates, client_x/client_y are the
 * draw-area coordinates (for position:fixed elements). Return 1 if a redraw
 * is needed. */
int lh_mouse_over(void* doc, int x, int y, int client_x, int client_y);
int lh_lbutton_down(void* doc, int x, int y, int client_x, int client_y);
int lh_lbutton_up(void* doc, int x, int y, int client_x, int client_y);
int lh_mouse_leave(void* doc);

int lh_find_box(void* doc, const char* field_id, int* x, int* y, int* w, int* h);

void lh_destroy(void* doc);

const char* lh_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LITEHTML_C_H */
