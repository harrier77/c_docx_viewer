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

/* List marker shapes passed to draw_list_marker. */
typedef enum ld_list_marker_type
{
    LD_MARKER_NONE   = 0,  /* nothing to draw */
    LD_MARKER_CIRCLE = 1,  /* hollow circle */
    LD_MARKER_DISC   = 2,  /* filled disc (default bullet) */
    LD_MARKER_SQUARE = 3,  /* filled square */
    LD_MARKER_IMAGE  = 4,  /* image marker; 'image' is the media name */
    LD_MARKER_OTHER  = 5   /* numbered / alpha / roman: text is drawn separately */
} ld_list_marker_type;

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

    /* Opaque host renderer (added at the end to preserve the layout of the
     * previous fields).  When non-NULL, litedocx runs in "renderer mode":
     * text measurement/drawing and list markers are delegated to the host
     * callbacks instead of being done with GDI inside the DLL.
     * Pass NULL to keep the legacy GDI behaviour. */
    void* renderer;

    /* Optional list marker hook (renderer mode).  Coordinates are in
     * document space.  'image' is the marker image name, or NULL/empty.
     * Return 1 if the marker was drawn, 0 to let litedocx fall back to the
     * legacy GDI drawing for this marker. */
    int (*draw_list_marker)(void* target, int x, int y, int w, int h,
                            int marker_type, unsigned color, const char* image);

    /* Optional bitmap hook (renderer mode).  When load_image_data is NULL,
     * litedocx decodes the image itself and hands the native GDI bitmap to
     * this callback; the host draws it through its renderer.  When both this
     * and draw_image_data are NULL, images are skipped in renderer mode
     * (no AlphaBlend fallback). */
    void (*draw_image_bitmap)(void* target, void* native_bitmap,
                              int src_w, int src_h, int x, int y, int w, int h);
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
/* Page-range / on-demand layout helpers */
int ld_get_page_count(void* doc);
int ld_get_page_height(void* doc, int page_idx);
int ld_render_range(void* doc, int page_idx, int viewport_h, int max_width);

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

/* ------------------------------------------------------------------------- */
/* Layout Cache Extraction                                                   */
/* ------------------------------------------------------------------------- */
/*
 * The layout cache allows the host to extract the computed layout
 * (paragraphs, text runs, positions, styles) after ld_render/ld_draw,
 * so that relayout is not needed for zoom/pan/transform operations.
 */

/* Layout element types */
typedef enum ld_layout_element_type
{
    LD_LAYOUT_EL_NONE        = 0,
    LD_LAYOUT_EL_PARAGRAPH   = 1,
    LD_LAYOUT_EL_TEXT_RUN    = 2,
    LD_LAYOUT_EL_TABLE       = 3,
    LD_LAYOUT_EL_TABLE_ROW   = 4,
    LD_LAYOUT_EL_TABLE_CELL  = 5,
    LD_LAYOUT_EL_IMAGE       = 6,
    LD_LAYOUT_EL_LIST_MARKER = 7,
    LD_LAYOUT_EL_BORDER      = 8,
    LD_LAYOUT_EL_BACKGROUND  = 9,
} ld_layout_element_type;

/* Paragraph alignment */
typedef enum ld_layout_alignment
{
    LD_LAYOUT_ALIGN_LEFT   = 0,
    LD_LAYOUT_ALIGN_CENTER = 1,
    LD_LAYOUT_ALIGN_RIGHT  = 2,
    LD_LAYOUT_ALIGN_JUSTIFY = 3,
} ld_layout_alignment;

/* Callback structure for layout extraction.
 * Register with ld_set_layout_cache_callbacks(), then call ld_extract_layout().
 * All coordinates are in document space (CSS pixels, unscaled).
 */
typedef struct ld_layout_cache_ops
{
    /* Called when a new paragraph/block starts.
     * id: unique identifier for this paragraph (element pointer)
     * x,y,w,h: bounding box in document coordinates
     * alignment: text alignment
     * line_spacing: inter-line spacing in px
     * space_before/after: paragraph spacing
     * indent_left/right/first: indentation
     */
    void (*on_paragraph_start)(void* user,
                               int id, int x, int y, int w, int h,
                               int alignment, int line_spacing,
                               int space_before, int space_after,
                               int indent_left, int indent_right, int indent_first);

    /* Called for each text run within a paragraph.
     * para_id: id from on_paragraph_start
     * text: UTF-8 text (valid only during callback)
     * x,y,w,h: run position/size in document coordinates
     * font_handle: opaque font handle from litedocx
     * color: text color (RGB)
     * underline/strike: text decoration flags
     */
    void (*on_text_run)(void* user,
                        int para_id, const char* text, int x, int y, int w, int h,
                        void* font_handle, unsigned color, int underline, int strike);

    /* Called when paragraph ends (optional, for cleanup). */
    void (*on_paragraph_end)(void* user, int id);

    /* Called for background fills (paragraph, table cell, etc.)
     * x,y,w,h: rectangle in document coordinates
     * color: fill color (RGB)
     */
    void (*on_background)(void* user, int x, int y, int w, int h, unsigned color);

    /* Called for borders.
     * side: 0=left, 1=top, 2=right, 3=bottom
     * width: border width in px
     * color: border color (RGB)
     */
    void (*on_border)(void* user, int x, int y, int w, int h,
                      int side, int width, unsigned color);

    /* Called for images.
     * src: media path (e.g., "media/image1.png")
     */
    void (*on_image)(void* user, int x, int y, int w, int h, const char* src);

    /* Called for list markers.
     * type: ld_list_marker_type
     * color: marker color
     * image: image name if LD_MARKER_IMAGE, else NULL
     */
    void (*on_list_marker)(void* user, int x, int y, int w, int h,
                           int type, unsigned color, const char* image);

    /* Called for table cells.
     * Provides cell geometry and style for potential custom rendering.
     */
    void (*on_table_cell)(void* user,
                          int table_idx, int row, int col,
                          int x, int y, int w, int h,
                          int is_header, unsigned bg_color,
                          int border_left_w, int border_top_w,
                          int border_right_w, int border_bottom_w,
                          unsigned border_left_c, unsigned border_top_c,
                          unsigned border_right_c, unsigned border_bottom_c,
                          int padding_left, int padding_top,
                          int padding_right, int padding_bottom,
                          const char* valign);
} ld_layout_cache_ops;

/* Register callbacks for layout extraction.
 * Call before ld_extract_layout(). Pass NULL to disable.
 */
void ld_set_layout_cache_callbacks(void* doc, const ld_layout_cache_ops* ops, void* user);

/* Extract layout by performing a draw pass with the registered callbacks.
 * Must be called after ld_render() has completed layout.
 * Does not produce visible output; only invokes the callbacks.
 */
void ld_extract_layout(void* doc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LITEDOCX_C_H */
