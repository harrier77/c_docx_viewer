Files to Modify

 ### 1. litedocx_c.h (C ABI header) — Add new API declarations

 ```c
   // New paginated layout API
   int ld_render_range(void* doc, int page_idx, int viewport_h, int max_width);  // layout page + 1-2 next
   int ld_get_page_count(void* doc);        // total pages (may compute lazily)
   int ld_get_page_height(void* doc, int page_idx);  // height of specific page
   void ld_clear_page_cache(void* doc);     // LRU eviction
 ```

 ### 2. litedocx_c.cpp (C ABI implementation) — Core incremental logic

 Changes needed:
 - ld_doc struct: Add pagination state
   ```cpp
     struct ld_doc {
         // ...existing...
         int            m_page_height_px = 0;      // CSS page height
         std::vector<int> m_page_heights;          // computed page heights (lazy)
         std::vector<int> m_page_y_offsets;        // y-offset of each page
         int            m_last_rendered_page = -1; // highest page laid out
         // LRU cache for rendered pages (optional: could just re-render from cache)
     };
   ```
 - ld_open_file: Already does DOM-only (good), but ensure ld_render is not called internally.
 - ld_render_range: Implement incremental layout:
     1. Compute page_height_px from section margins (already in DocxBuilder::m_page_h_px)
     2. Call doc->render(max_width) once to build full render tree (litehtml needs this for measurements)
     3. But: instead of full layout, compute page breaks via position of page-break divs (add_page_break creates height:Npx divs)
     4. Store page heights/offsets lazily; ld_render_range(page_idx, ...) returns immediately if page already computed
 - ld_get_page_count/height: Return cached or compute on-demand
 - ld_draw: Already takes clip rect — works with incremental if you only draw visible pages

 ### 3. docx_builder.h / docx_builder.cpp — Expose page metrics

 Minimal changes:
 - DocxBuilder::page_height() already returns CSS page height (m_page_h_px)
 - Ensure add_page_break divs have unique IDs or data-attributes so ld_render_range can find page boundaries
 - Consider adding get_page_break_positions() helper if needed

 ### 4. docxview_new.c (Viewer) — Consume new API

 Changes needed:
 - load_docx_file: Call ld_open_file only (no ld_render)
 - WM_PAINT / scroll handler: Compute visible page range from g_scroll / g_ch
   ```c
     int first_page = g_scroll / page_height;
     int last_page  = (g_scroll + g_ch) / page_height + 1; // +1 for lookahead
     for (int p = first_page; p <= last_page; ++p)
         ld_render_range(g_doc, p, g_ch, content_width());
   ```
 - Background worker: On scroll, enqueue JOB_RENDER_RANGE for next N pages (LRU: evict pages far from viewport)
 - request_render: Replace with request_render_range(first, last)
 - on_render_done: Update g_doc_h incrementally (sum of page heights); InvalidateRect only changed region

 ────────────────────────────────────────────────────────────────────────────────

 Key Design Points

 ┌──────────────┬───────────────────────────────────┬──────────────────────────────────────────────┐
 │ Aspect       │ Current                           │ Target                                       │
 ├──────────────┼───────────────────────────────────┼──────────────────────────────────────────────┤
 │ ld_open_file │ DOM + full ld_render              │ DOM only (already true)                      │
 ├──────────────┼───────────────────────────────────┼──────────────────────────────────────────────┤
 │ Layout       │ ld_render(doc, w) → full document │ ld_render_range(doc, page, h, w) → 1-3 pages │
 ├──────────────┼───────────────────────────────────┼──────────────────────────────────────────────┤
 │ First paint  │ Blocks on full layout             │ < 1s (DOM parse + page 0 only)               │
 ├──────────────┼───────────────────────────────────┼──────────────────────────────────────────────┤
 │ Scroll       │ Full re-render on resize          │ On-demand page render + LRU cache            │
 ├──────────────┼───────────────────────────────────┼──────────────────────────────────────────────┤
 │ ld_draw      │ Draws all                         │ Draws clipped — already supports incremental │
 └──────────────┴───────────────────────────────────┴──────────────────────────────────────────────┘

 ────────────────────────────────────────────────────────────────────────────────

 Implementation Order

 1. litedocx_c.h — Add declarations
 2. litedocx_c.cpp — Implement ld_render_range, page tracking, cache
 3. docx_builder — Minor: ensure page-break divs are identifiable
 4. docxview_new.c — Wire up scroll → ld_render_range, background pre-fetch

 The heavy lifting is in litedocx_c.cpp (incremental layout state machine) and docxview_new.c (viewport-driven scheduling). No changes to litehtml core needed — it already renders to a clip rect.