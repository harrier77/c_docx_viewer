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

/* ------------------------------------------------------------------
 * Paragraph editing ABI (in-memory only)
 *
 * Every paragraph created from a source w:p gets a stable index for the
 * lifetime of the open document.  Indices are assigned in document order
 * and are valid for normal paragraphs, list paragraphs and paragraphs
 * inside table cells.  Builder-only placeholder paragraphs (empty cells or
 * an empty body) are intentionally not indexed and are not editable.
 *
 * Geometry is available only after a completed layout: before the first
 * render, and after a text change until the next render, hit-testing fails
 * cleanly instead of returning stale placements.
 * ------------------------------------------------------------------ */

typedef struct ld_paragraph_hit
{
    int index;      /* stable paragraph index (>= 0) */
    int x, y, w, h; /* paragraph box in document coordinates */
} ld_paragraph_hit;

/* Hit-test a point in document coordinates.
 * Returns 1 when a paragraph with valid post-layout geometry contains the
 * point and fills out_hit.  If out_text is not NULL it receives a
 * null-terminated UTF-8 copy of the paragraph text; the caller must free it
 * with ld_free_string() (it is set to NULL when nothing is hit).
 * Returns 0 when no editable paragraph is hit. */
int ld_paragraph_hit_test(void* doc, int x, int y,
                          ld_paragraph_hit* out_hit, char** out_text);

/* Diagnostic, read-only run hit-test.  Given a point in document coordinates
 * (the same space as ld_paragraph_hit_test), it reports which direct child
 * ("run slot") of the hit paragraph contains the point and where the click
 * falls in the paragraph's plain text (the normalized text returned by
 * ld_paragraph_get_text: NBSP -> space, byte offsets in UTF-8).
 * litehtml splits text into word-level elements, so x/y/w/h is the box of
 * the hit text part (a word); a click in a gap between words falls back to
 * the nearest word on the same line.  Nothing is modified: no text, no
 * layout, no caches are touched.  Returns 1 when a text part is resolved. */
typedef struct ld_run_hit
{
    int para_index;      /* hit paragraph index, -1 when nothing is hit */
    int run_index;       /* direct-child slot in DOM order, -1 when none */
    int char_offset;     /* click offset in the paragraph plain text, -1 */
    int run_char_start;  /* run start offset in the paragraph text, -1 */
    int run_char_end;    /* run end offset (exclusive) in that text, -1 */
    int run_char_offset; /* click offset inside the run text, -1 */
    int x, y, w, h;      /* hit text-part box in document coordinates */
    /* Font metrics of the hit text part (px), 0 when not resolved. */
    int font_size;
    int font_height;
    int font_ascent;
    int font_descent;
    int font_ch_width;   /* character advance measure useful for caret */
} ld_run_hit;

int ld_run_hit_test(void* doc, int x, int y, ld_run_hit* out);

/* Precision caret hit-test (read-only, first step of the inline caret work).
 * Same read-only contract as ld_run_hit_test (nothing is modified: no text,
 * no DOM, no caches), but the intra-word offset is NOT estimated
 * proportionally: the widths of the prefixes of the hit text part are
 * measured with the same font metrics used by the renderer/layout
 * (the container's text_width), so the reported character offset and the
 * caret position are exact at character level.  Clicks in a gap between
 * words snap the caret to the nearest edge on the same line.
 * caret_x/caret_y is the left edge / top of the caret bar in document
 * coordinates, caret_h its height (the hit text part box height) and
 * caret_w the suggested bar thickness (1 px). */
typedef struct ld_caret_hit
{
    int para_index;      /* hit paragraph index, -1 when nothing is hit */
    int run_index;       /* direct-child slot in DOM order, -1 when none */
    int char_offset;     /* caret offset in the paragraph plain text, -1 */
    int run_char_start;  /* run start offset in the paragraph text, -1 */
    int run_char_end;    /* run end offset (exclusive) in that text, -1 */
    int run_char_offset; /* caret offset inside the run text, -1 */
    int caret_x, caret_y; /* caret bar position in document coordinates */
    int caret_w;         /* suggested bar thickness (1 px) */
    int caret_h;         /* bar height (hit text-part box height) */
} ld_caret_hit;

int ld_run_caret_hit(void* doc, int x, int y, ld_caret_hit* out);

/* Deterministic caret placement at an exact normalized UTF-8 byte offset
 * within a run slot (used to re-anchor the caret after an inline edit
 * without pixel-coordinate hit-test drift across mixed fonts). */
int ld_run_caret_pos(void* doc, int para_index, int run_index,
                     int run_char_offset, ld_caret_hit* out);

/* Font character-advance metric (px) of a run slot, read-only.  Returns 0 when
 * the paragraph/run index is invalid or no font metric is available.  Unlike
 * ld_run_hit_test this does not need a pixel point, so it also works on mixed
 * font-size lines where a point hit-test may miss the smaller text box. */
int ld_run_font_ch_width(void* doc, int para_index, int run_index);

/* ------------------------------------------------------------------
 * Read-only data for a temporary shadow-text preview (inline typing).
 *
 * The host must call this ONLY from the UI thread, after a completed
 * layout, while the document is not busy (no render/hittest/apply/load).
 * Nothing is modified: no text, no DOM, no layout, no caches.
 *
 * Returns 1 only when EVERY condition can be proven:
 *   - the paragraph exists, has post-layout geometry and is a normal
 *     paragraph (not inside a table);
 *   - run_index selects a direct child slot whose content is pure text
 *     (litehtml may split it into several word-level leaves, each with a
 *     valid box, contiguous within its line; no <br> and no mixed/inline
 *     elements), and no other rendered content to its right;
 *   - that text has an opaque colour and sits on a known white (or fully
 *     transparent) background, with no background image/gradient, no
 *     visible borders and no text decoration that a white patch would
 *     erase;
 *   - a valid opaque font handle and the text colour are available.
 *
 * caret_y is the caret's document-space Y.  It selects WHICH line of a
 * wrapped run to preview; the returned box and byte range describe only
 * that line.  char_start/char_end are byte offsets into the run's plain
 * (normalized UTF-8) text, so the host can take the matching slice of its
 * shadow string and draw just that line.
 *
 * Returns 0 without side effects for any ambiguous case.  The returned
 * font handle is BORROWED: it is valid only until the document layout /
 * font cache is rebuilt (next render or edit).  Never free it and never
 * keep it across renders.
 * ------------------------------------------------------------------ */
typedef struct ld_run_preview
{
    int      x, y, w, h;   /* selected line's text box, document coordinates */
    int      char_start;   /* byte offset of this line in the run text */
    int      char_end;     /* byte offset just past this line */
    void    *font;         /* borrowed opaque renderer font handle */
    unsigned color;        /* COLORREF-compatible 0x00BBGGRR */
    int      safe_w;       /* max width available from x on this line */
} ld_run_preview;

int ld_run_preview_info(void* doc, int para_index, int run_index, int caret_y, ld_run_preview* out);

/* Replace the plain text of a single run ("slot") of a paragraph, keeping the
 * formatting of every other run.  run_index is the same direct-child slot
 * index reported by ld_run_hit_test (DOM order, <br> excluded).  When the
 * paragraph has stored OOXML and it maps cleanly onto the render tree, the
 * change is applied to the underlying word/document.xml (the w:t of the
 * matching w:r) and the render tree is rebuilt, so the edit is INCLUDED in
 * ld_get_document_xml and therefore in the saved file.  Otherwise the change
 * falls back to an in-memory-only edit of the render tree (the existing span
 * element is reused, so its class and matched styles are preserved).  text
 * is UTF-8; '\n' and '\r' are rejected (a run is inline).  A full ld_render()
 * is needed afterwards.  Returns 1 on success, 0 on invalid index/failure. */
int ld_run_set_text(void* doc, int index, int run_index, const char* text);

/* Inline editing: insert or delete at a character offset within a run.
 * This is a convenience wrapper that reads the current run text, applies
 * the edit in the normalized character space (same space as ld_run_hit_test
 * and ld_paragraph_get_text: NBSP->space, byte offsets in UTF-8), and calls
 * ld_run_set_text with the new run text.  The edit reuses the existing span
 * element so formatting is preserved.
 * 
 * Parameters:
 *   doc          - document handle
 *   index        - paragraph index
 *   run_index    - run slot index (from ld_run_hit_test / ld_run_caret_hit)
 *   char_offset  - offset in the PARAGRAPH normalized text where edit occurs
 *   op           - 0 = insert, 1 = delete (backspace), 2 = delete (forward)
 *   text         - text to insert (UTF-8, only used when op == 0)
 * 
 * For insert: inserts 'text' at char_offset.
 * For backspace: deletes the character before char_offset (if char_offset > run_start).
 * For forward delete: deletes the character at char_offset (if char_offset < run_end).
 * 
 * Returns 1 on success, 0 on invalid index/failure.
 * This is an in-memory edit only; OOXML persistence is disabled during typing.
 * Call ld_get_document_xml after typing session to get updated OOXML for save.
 */
int ld_run_edit(void* doc, int index, int run_index, int char_offset,
                int op, const char* text);

/* Return a newly allocated UTF-8 copy of a paragraph's plain text.
 * The caller must free it with ld_free_string().  Returns NULL if the index
 * is invalid or the paragraph cache is not valid yet (before the first
 * completed layout, or while waiting for the relayout after an edit). */
char* ld_paragraph_get_text(void* doc, int index);

/* Replace the plain text of a paragraph.  text is UTF-8; '\n' becomes a
 * line break.  Only the paragraph's children are rebuilt: the element, its
 * class and its data-ld-paragraph-index attribute are preserved, so
 * paragraph-level formatting is kept.  Character-level run formatting is
 * flattened.  The change is in memory only and is NOT written to the DOCX
 * file.  Returns 1 on success, 0 on invalid index or failure. */
int ld_paragraph_set_text(void* doc, int index, const char* text);

/* Return the paragraph's inner HTML (litehtml DOM) as UTF-8 string.
 * The caller must free it with ld_free_string().  Returns NULL if the index
 * is invalid or the paragraph cache is not valid yet.  The HTML contains
 * <span class="sN"> elements with run-level formatting and <br> for line breaks.
 * This is the in-memory representation, NOT the original OOXML w:p. */
int ld_paragraph_get_xml(void* doc, int index, char** out_xml);

/* Replace the paragraph's inner HTML with the given XML/HTML fragment.
 * xml is UTF-8; a subset of HTML is supported: <span class="sN">, <b>, <i>,
 * <u>, <strike>, <br>, and plain text.  The paragraph element, its class,
 * and data-ld-paragraph-index are preserved.  A representative run span is
 * kept to maintain dominant character formatting.  The change is in memory
 * only and is NOT written to the DOCX file.  Returns 1 on success, 0 on failure. */
int ld_paragraph_set_xml(void* doc, int index, const char* xml);

/* Return the original OOXML <w:p>...</w:p> fragment of the paragraph, exactly
 * as captured from word/document.xml while loading (re-serialized from the
 * parsed XML DOM).  This is the real DOCX markup (w:p, w:r, w:t, w:pPr...),
 * NOT the litehtml serialization returned by ld_paragraph_get_xml().
 * The caller must free the string with ld_free_string().
 * Returns 1 on success, 0 if the index has no stored OOXML. */
int ld_paragraph_get_ooxml(void* doc, int index, char** out_xml);

/* Replace the paragraph's raw OOXML with the given <w:p>...</w:p> fragment.
 * The fragment must contain exactly one well-formed w:p.  Its original byte
 * range in word/document.xml is replaced in place and the whole render tree
 * is rebuilt, so styling/lists/tables continue to be resolved by the normal
 * DOCX pipeline.  The paragraph keeps its index as long as the fragment still
 * contains a single w:p that does not change document structure.
 * Caller must run a full ld_render() afterwards to get the new layout.
 * In-memory only: the DOCX file is not written.  Returns 1 on success. */
int ld_paragraph_set_ooxml(void* doc, int index, const char* ooxml);

/* Free a string returned by litedocx (ld_paragraph_get_text / hit-test). */
void ld_free_string(char* str);

const char* ld_last_error(void);
const char* ld_version(void);

/* Invalidate font cache (call when zoom factor changes).
 * Forces font recreation at next layout/render. */
void ld_invalidate_font_cache(void* doc);

/* Return the complete, up-to-date word/document.xml as UTF-8 string.
 * The returned string includes the XML declaration, root <w:document> with
 * all namespaces, the updated <w:body> with all paragraphs (including any
 * edits applied via ld_paragraph_set_ooxml), and the closing tags.
 * The caller owns the returned string and must free it with ld_free_string().
 * Returns 1 on success, 0 on failure (e.g., invalid document handle). */
int ld_get_document_xml(void* doc, char** out_xml);

void ld_destroy(void* doc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LITEDOCX_C_H */
