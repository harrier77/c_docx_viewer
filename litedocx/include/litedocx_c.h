/*
 * litedocx_c.h - C ABI for litedocx.dll
 *
 * litedocx renders a .docx directly into the litehtml element tree.  No HTML
 * string is produced and the caller only sees opaque document handles.
 *
 * The callbacks are the same ones used by litehtml_c.dll (lh_callbacks).
 */
#ifndef LITEDOCX_C_H
#define LITEDOCX_C_H

#include <stddef.h>

#include "litehtml_c.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Optional image hooks.  litedocx decodes PNG/JPEG itself through WIC, so the
 * host does not need to implement these.  They are declared for future hosts
 * that want to supply their own image cache/decoder.  When load_image_data is
 * NULL litedocx uses its internal decoder.
 */

/*
 * Optional table cell hooks.  When a table cell (td/th) is about to be drawn,
 * these callbacks are invoked before the standard draw_solid_fill/draw_borders.
 * Return 1 to suppress the default drawing for that aspect, 0 to proceed normally.
 * The cell_style parameter is an opaque pointer to internal style data (may be NULL).
 */
typedef struct ld_table_cell_style
{
    int         row_index;        /* 0-based row index in the table */
    int         col_index;        /* 0-based column index in the table */
    int         row_span;         /* rowspan attribute (>=1) */
    int         col_span;         /* colspan attribute (>=1) */
    int         is_header;        /* 1 if <th>, 0 if <td> */
    unsigned    bg_color;         /* resolved background color (0 if transparent) */
    int         has_borders;      /* 1 if any border is non-zero */
    int         border_left_w;    /* border widths in px */
    int         border_top_w;
    int         border_right_w;
    int         border_bottom_w;
    unsigned    border_left_c;    /* border colors */
    unsigned    border_top_c;
    unsigned    border_right_c;
    unsigned    border_bottom_c;
    int         padding_left;     /* cell padding in px */
    int         padding_top;
    int         padding_right;
    int         padding_bottom;
    const char* valign;           /* "top", "middle", "bottom", or NULL */
} ld_table_cell_style;

typedef struct ld_callbacks
{
    lh_callbacks lh;

    /* name is the media path ("media/image1.png"). data/size is the raw file.
       Return an opaque host image, or NULL to let litedocx decode it. */
    void* (*load_image_data)(const char* name, const void* data, int size);
    void  (*draw_image_data)(void* hdc, void* image, int x, int y, int w, int h);
    void  (*delete_image_data)(void* image);

    /* Table cell rendering hooks (optional, NULL = default behavior).
     * Called for each td/th during drawing. Coordinates are in document space.
     * Return 1 to skip default drawing for that aspect, 0 to allow it. */
    int (*draw_table_cell_bg)(void* hdc, int x, int y, int w, int h,
                              const ld_table_cell_style* style);
    int (*draw_table_cell_borders)(void* hdc, int x, int y, int w, int h,
                                   const ld_table_cell_style* style);
} ld_callbacks;

/* Open a .docx from disk or from a complete ZIP image in memory. */
void* ld_open_file(const char* path, const ld_callbacks* cb, int viewport_width, int viewport_height);
void* ld_open_memory(const void* data, size_t size, const ld_callbacks* cb, int viewport_width,
                     int viewport_height);

/* Layout for max_width; returns the natural width. */
int ld_render(void* doc, int max_width);

/* Set visual padding (margins) for paint. The layout box (width = max_width) will be centered
 * inside the visual box (width = max_width + 2 * visual_padding_x). */
void ld_set_visual_padding(void* doc, int visual_padding_x, int visual_padding_y);

/* Set justification width for root block. This fixes the justification layout width
 * independent of the layout width (max_width passed to ld_render).
 * If 0 (default), falls back to layout_width.
 * Use this to keep justified text stable when changing page margins. */
void ld_set_justification_width(void* doc, int justification_width);

/* Paint into hdc, offset by (x,y), clipped to the given rectangle. */
void ld_draw(void* doc, void* hdc, int x, int y, int clip_x, int clip_y, int clip_w, int clip_h);

int ld_width(void* doc);
int ld_height(void* doc);

/* Page size of the first/active section, in CSS pixels. 0 when unknown. */
int ld_page_width(void* doc);
int ld_page_height(void* doc);

/* Mouse handling.  x/y are document coordinates, client_x/client_y are the
 * draw-area coordinates (for position:fixed elements). Return 1 if a redraw
 * is needed. */
int ld_mouse_over(void* doc, int x, int y, int client_x, int client_y);
int ld_lbutton_down(void* doc, int x, int y, int client_x, int client_y);
int ld_lbutton_up(void* doc, int x, int y, int client_x, int client_y);
int ld_mouse_leave(void* doc);

/* Find an element by id / data-field.  Returns 1 and fills the box. */
int ld_find_box(void* doc, const char* id, int* x, int* y, int* w, int* h);

/* Table info: returns number of tables in document. */
int ld_table_count(void* doc);

/* Get table cell geometry for a specific table. Returns number of cells.
 * If cells array is NULL, only returns count. Each cell receives its bounds and style.
 * The caller owns the returned array and must free it with ld_free_table_cells(). */
typedef struct ld_table_cell_info
{
    int         table_index;      /* which table (0-based) */
    int         row_index;
    int         col_index;
    int         row_span;
    int         col_span;
    int         is_header;
    int         x, y, w, h;       /* cell bounds in document coordinates */
    unsigned    bg_color;
    int         border_left_w, border_top_w, border_right_w, border_bottom_w;
    unsigned    border_left_c, border_top_c, border_right_c, border_bottom_c;
    int         padding_left, padding_top, padding_right, padding_bottom;
    const char* valign;
} ld_table_cell_info;

int ld_get_table_cells(void* doc, int table_index, ld_table_cell_info** out_cells, int* out_count);
void ld_free_table_cells(ld_table_cell_info* cells);

const char* ld_last_error(void);
const char* ld_version(void);

/* Invalidate font cache (call when zoom factor changes).
 * Forces font recreation at next layout/render. */
void ld_invalidate_font_cache(void* doc);

void ld_destroy(void* doc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LITEDOCX_C_H */
