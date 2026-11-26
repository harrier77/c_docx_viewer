# Layout Cache Improvement Notes

## Context
The d2dview document viewer renders .docx files via litedocx -> litehtml. Editing a
paragraph triggers a full document rebuild (`DocxBuilder::build()` + `finalize`), which
dominates edit latency (~500 ms on a 200-page document). The layout cache
(`ensure_layout_cache_populated` / `ld_extract_layout`) was a secondary cost (~140 ms)
that we tried to make lazy.

## What was tried (branch `lazy-cache`, now deleted)
1. **Lazy layout cache**: removed eager `ld_extract_layout` from render/edit paths;
   added `ensure_layout_cache_populated()` for on-demand population.
2. **Timing instrumentation**: `TIMING_LOGS`/`TLOG` in viewer, litedocx, litehtml;
   `build_timing.bat` producing `d2dview_timing.exe`.
3. **Incremental paragraph patching** (Phases 1-3): `insertBefore` in litehtml DOM,
   `DocxBuilder::build_paragraph_element`, incremental path in `ld_paragraph_set_ooxml`.

## Why it failed
The lazy-cache change removed the eager population in `request_render` /
`on_render_done` but `ensure_layout_cache_populated()` was never called from any
consumer (hit-test, caret, layout queries). As a result the cache stayed empty/invalid
and any code relying on it crashed. The incremental patching inherited the same
problem and added DOM/render-tree inconsistencies.

Profiling confirmed the real bottleneck is not cache extraction but
`finalize`/`compute_styles` (~292 ms on 200 pp), which only an incremental rebuild
can address — but that refactor is large and was not stabilized.

## Baseline measurements (200-page doc, edit first paragraph)
| Metric | Before | After lazy cache |
|---|---|---|
| `set_ooxml` total | 696 ms | 544 ms |
| `ld_render` | 51 ms | 45 ms |
| `ld_extract_layout` | 141 ms | 0 ms (lazy) |
| Improvement | — | -152 ms (-22%) |

Remaining cost: `DocxBuilder::build()` / `finalize` / `compute_styles` (~365 ms).

## Recommendations for a future attempt
1. **Do not make the cache lazy without wiring on-demand population** at every
   query site that reads `g_layout_cache` (hit-test, caret, layout). Either keep
   eager population or add `ensure_layout_cache_populated()` calls at all consumers.
2. **Keep timing instrumentation** (`TIMING_LOGS`/`TLOG`) available behind a build
   switch (`build_timing.bat`) for profiling; it is cheap and invaluable.
3. **If pursuing incremental patching again**, do it in small, testable phases with
   a working baseline at each step:
   - Phase 1: DOM primitives (`insertBefore`, `apply_stylesheets_to`) — compile only.
   - Phase 2: single-paragraph builder in `DocxBuilder` — unused, compile only.
   - Phase 3: incremental path with full-rebuild fallback for unsupported cases
     (numbered/list/table paragraphs).
   - Always keep the full-rebuild fallback; never remove it.
4. **Alternative cheaper wins**: optimize `compute_styles` (memoize inherited
   properties, skip unchanged subtrees) before attempting full incremental DOM
   patching.

## Current state
Branch `lazy-cache` has been abandoned and deleted. The working branch is `master`
(commit `20d4bda`, "feat: add native Win32 toolbar to DOCX viewer"), which is the
last known-good state. All lazy-cache commits are gone from history.

## Latest profiling results (branch `page_prova`)
Fine-grained timing added via `build_timing.bat` (timing-only console build).
Measured on a 200-page document, editing one paragraph:

- `DocxBuilder xml_parse`: ~4-7 ms
- `DocxBuilder parse_section`: ~0.1 ms
- `DocxBuilder build_body`: ~46-49 ms
- `DocxBuilder css/gen`: ~0.02-0.03 ms
- `finalize master/user css`: ~1-3 ms
- `finalize apply_stylesheets`: ~8 ms
- `finalize compute_styles`: ~276-300 ms  (dominant bottleneck)
- `finalize create_render_item`: ~30 ms
- `finalize fix_tables`: ~0 ms
- `finalize init`: ~14-17 ms
- `finalize` total: ~347-372 ms
- `build()` total: ~413-431 ms
- `ld_paragraph_set_ooxml` total: ~489-505 ms
- viewer `ld_render`: ~43-56 ms

Conclusion: the real bottleneck is `compute_styles` (recursive CSS resolution),
not cache extraction, XML parsing, body building, or on-screen rendering.

## UX improvement implemented
During a render (`g_render_busy`), the viewer now keeps and blits the last valid
frame instead of showing a blank page (`viewer_paint.c`). This makes the
~490 ms rebuild wait much less noticeable without changing correctness.

## Updated recommendations
1. Keep `compute_styles` profiling (timing build) available; it is cheap and
   invaluable for validating any optimization.
2. Prefer a micro-optimization pass on `compute_styles` (memoize inherited
   properties, skip unchanged subtrees) before attempting full incremental
   DOM patching.
3. If incremental work is pursued, do it in small, testable phases with a
   working full-rebuild fallback at every step (as in `cache_improve.md`).
4. Keep the "last frame" UX fallback; optionally add an "Updating..." overlay
   on top of the frozen frame for clarity.
5. `build_timing.bat` now builds a timing-only binary (no icon, no DLOG spam)
   for clean console profiling.## Micro-profiling update (compute_styles breakdown)
- 3016 elements total (~15/page)
- css_properties::compute total: ~13-16 ms
  - font: ~7-14 ms
  - background: ~1-2 ms
  - flex: ~0 ms
  - other: ~1-3 ms
- html_tag::compute_styles non-CSS work: ~4-5 ms total
- recursion/traversal overhead: ~270-290 ms (dominant)

Element distribution:
- span: 1703 (56%)
- p: 1001 (33%)
- a: 171
- li: 101
- ul: 30
- br: 8
- html/body: 2

Conclusion: bottleneck is NOT CSS property resolution (~13-16 ms),
but traversal/recursion overhead on ~3016 elements (mostly spans and paragraphs).
Most promising optimization: reduce element count (merge contiguous spans
with identical style) or reduce virtual-dispatch overhead.
