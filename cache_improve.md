# Layout Cache Improvement Notes

## Context
The d2dview document viewer renders .docx files via litedocx -> litehtml. Editing a
paragraph triggers a full document rebuild (`DocxBuilder::build()` + `finalize`), which
dominates edit latency (~500 ms on a 200-page document). The layout cache
(`ensure_layout_cache_populated` / `ld_extract_layout`) was a secondary cost (~140 ms)
that we tried to make lazy.

## What was tried (branch `lazy-cache`, now deleted)
1. **Lazy layout cache**: removed eager `ld_extract_layout` from render/edit paths;added `ensure_layout_cache_populated()` for on-demand population.
2. **Timing instrumentation**: `TIMING_LOGS`/`TLOG` in viewer, litedocx, litehtml;`build_timing.bat` producing `d2dview_timing.exe`.
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
1. **Do not make the cache lazy without wiring on-demand population** at every query site that reads `g_layout_cache` (hit-test, caret, layout). Either keep eager population or add `ensure_layout_cache_populated()` calls at all consumers.
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
1. Keep `compute_styles` profiling (timing build) available; it is cheap and invaluable for validating any optimization.
2. Prefer a micro-optimization pass on `compute_styles` (memoize inherited properties, skip unchanged subtrees) before attempting full incremental DOM patching.
3. If incremental work is pursued, do it in small, testable phases with a working full-rebuild fallback at every step (as in `cache_improve.md`).
4. Keep the "last frame" UX fallback; optionally add an "Updating..." overlay on top of the frozen frame for clarity.
5. `build_timing.bat` now builds a timing-only binary (no icon, no DLOG spam) for clean console profiling.## Micro-profiling update (compute_styles breakdown)
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

## Micro-profiling update 2: single-paragraph document (8 lines)

Measured on a document with a single paragraph of ~8 lines (18 elements), editing/opening:

- `DocxBuilder xml_parse`: ~0.03 ms
- `DocxBuilder parse_section`: ~0.02 ms
- `DocxBuilder build_body`: ~0.35 ms
- `DocxBuilder css/gen`: ~0.02 ms
- `finalize master/user css`: ~3.47 ms
- `finalize apply_stylesheets`: ~0.00 ms
- `finalize compute_styles`: ~7.20 ms (18 elements, 0.400 ms/element)
- `finalize create_render_item`: ~0.00 ms
- `finalize fix_tables`: ~0.00 ms
- `finalize init`: ~1.01 ms
- `finalize` total: ~27.68 ms
- `build()` total: ~43.23 ms
- `ld_render`: ~0.32 ms
- `HTML_TAG timing`: attr=0.00 doc=0.00 styleadd=0.00 subst=0.00 css=0.00 rec=26.70 ms
- `HTML_TAG tags`: body=1 html=1 p=2 span=14

### Key observations

1. **Per-element cost is ~4.3x higher on small docs** (0.400 ms/el vs 0.092 ms/el on
   200 pp). This points to fixed overhead + function-call/recursion overhead dominating
   at small scale, not just element volume.
2. **`rec=26.70 ms` is a double-counted aggregate, not an additive cost.** `_t_rec` is
   accumulated inside every node's child loop and includes the whole `compute_styles` of
   each child (which itself includes its own `_t_rec`). Summing across all nodes gives
   roughly (depth) x the real traversal cost. The real signal is
   `compute_styles = 7.20 ms` for 18 elements, with non-recursive work (attr/doc/
   styleadd/subst/css) at ~0 ms — i.e. ~100% of the time is traversal/call overhead,
   0% CSS resolution.
3. **Gap now closed by instrumentation (see Updated recommendations #1).** The
   previously unexplained ~16 ms were `apply_master_css` (stylesheet application over
   the whole tree) plus `master/user css` parse/sort, now separately timed. No hidden
   cost remains; components sum to ~finalize total on the measured documents.

### Updated recommendations

1. **Measurement gap closed (instrumentation added).** Added timers in
   `finalize_from_external_root` (`document.cpp`) for `apply_master_css`,
   `parse_attributes`, `parse_linked_css`, `update_media_lists`, alongside the
   existing `apply_stylesheets`. Rebuilt via `build_timing.bat` and re-measured on
   `dev/Prova.docx`, `dev/Titoli.docx`, `dev/long.docx`:
   - On `long.docx` (242 elements): finalize 14.02 ms = apply_master_css 1.00 +
     master/user css 1.00 + parse_attributes 0.00 + parse_linked_css 0.00 +
     update_media_lists 0.00 + apply_stylesheets 0.00 + compute_styles 8.73 +
     create_render_item 2.00 + init 1.00 (sum 13.73, ~0.3 ms rounding/overhead).
   - Components now sum to ~finalize total: the ~16 ms gap from the earlier
     single-paragraph run is accounted for; it was `apply_master_css` (stylesheet
     application over the whole tree) plus `master/user css` parse/sort, which
     scale with document size and style complexity. On the earlier 8-line document
     those two alone were ~3.47 + (unmeasured apply_master) ms.
   - Conclusion: no hidden cost remains; the dominant finalize costs are
     `compute_styles` (traversal) and, on larger/styled docs, `apply_master_css`
     + CSS parse/sort. Both are addressable (span merging for the former; CSS
     caching for the latter).
2. **Cache parsed CSS (new high-ROI lever for all sizes)**: master CSS is static and
   user CSS only changes when the style cache changes. Re-parsing/re-sorting them on
   every build costs ~3.47 ms fixed + part of the uninstrumented ~16 ms. Caching the
   parsed stylesheets would help small and large documents alike.
3. **Span merging stays the #1 lever for large docs** (1703 spans = 56% of elements),
   but yields little on small docs where cost is fixed. Implement in
   `process_run`/`process_inline_children`, fusing only adjacent text runs with
   identical `run_cls` (skip on `br`/`tab`/`noBreakHyphen`/hyperlink/bookmark).
4. **Virtual-dispatch / recursion overhead** (0.400 ms/el on small docs) is worth
   quantifying with timers on `get_property` and the child loop; only then consider
   `final` on leaf classes or devirtualization.
5. **Keep the "last frame" UX fallback**; the ~43 ms small-doc / ~490 ms large-doc
   rebuild wait is masked without correctness changes. Optional "Updating..." overlay
   on top of the frozen frame.
6. **Incremental rebuild remains strategic, not immediate**: high theoretical gain
   (~400 ms) but high risk of DOM/render-tree inconsistency (as shown by the failed
   `lazy-cache` branch). Only after micro-optimizations are exhausted, and always with
   a full-rebuild fallback.

## Instrumentation overhead caveat (TLOG/fflush)

Re-measured the 8-line document after adding the new timers. Result:

- `finalize` outer = 30.81 ms, but timed components sum to ~8.83 ms
  (master/user css 1.12 + compute_styles 7.71 + the rest ~0).
- Gap = ~22 ms inside `finalize_from_external_root`, not covered by any timer.
- Same pattern in `build()` TOTAL (41.22 ms vs ~31.15 ms sum of steps, gap ~10 ms).

Root cause: `TLOG` is defined as
`fprintf(stderr, ...) + fflush(stderr)`. The outer timers (`QueryPerformanceCounter`
around `finalize_from_external_root` / `build()`) include the cost of every `TLOG`
call inside, while the inner `std::chrono` timers stop before each `TLOG`. With
~12 TLOGs in finalize and ~5 between build steps, the `fflush` syscalls accumulate
into the gap. The gap scales with the number of TLOGs, not with document size.

Implications:
- All "large" numbers reported so far (43 ms build, 30 ms finalize, 490 ms on
  200 pp) are inflated by instrumentation. The real finalize cost on the 8-line doc
  is ~9 ms, not 30 ms.
- The trustworthy numbers are the inner `std::chrono` ones (compute_styles 7.71 ms
  for 18 elements = 0.43 ms/el, consistent with 200 pp's 0.09 ms/el once fixed
  overhead is accounted for).
- Two untimed spots remain in `finalize_from_external_root`: `get_media_features`
  (line 131) and `set_pseudo_class(_root_, true)` (line 133), but they are
  expected to be negligible (null media callback + root-only pseudo-class).

Action (requires code change, not done in Plan mode):
- Either disable `TIMING_LOGS` for clean end-to-end numbers, or remove the
  `fflush(stderr)` from `TLOG` in `debug_log.h` (batch stderr) and rebuild.
- Re-measure after that before quoting absolute timings in any conclusion.

## Implemented: span merging in `process_run` (litedocx)

Status: **done and tested** (working tree, not yet committed).

Change: in DocxBuilder::process_run, adjacent runs that are "plain text" (only w:t, no br tab noBreakHyphen drawing/math) and resolve to the same run_cls are fused into the previous  <span>  instead of creating a new one. Hyperlink/bookmark boundaries are preserved (parent is  a p).

Verified on `dev/test.docx` (1 paragraph, 14 runs, all default style):
- Before: 15 spans. After: **2 spans** (14 → 1). Merge works.

Measured on 200-page document (post-merge):
- `span`: 1703 → **1687** (-16, -0.9%). Negligible: most paragraphs have a
  single run or runs with differing styles, so few merge opportunities.
- `compute_styles`: 297.90 ms / 3000 elements (0.099 ms/el) — unchanged.
- Conclusion: span merging is correct and low-risk but **did not move the
  needle** on this corpus. Keep it (no regression) but do not expect further
  gains from more aggressive merging alone.

## Current state (post span-merging, 200 pp)

Trustworthy inner timings (`std::chrono`, exclude TLOG overhead):
- `DocxBuilder xml_parse`: 4.31 ms
- `DocxBuilder build_body`: 50.82 ms
- `finalize master/user css`: 0.00 ms (this run; up to ~3 ms on styled docs)
- `finalize apply_master_css`: 0.00 ms
- `finalize parse_attributes`: 13.25 ms  (newly exposed)
- `finalize parse_linked_css`: 0.00 ms
- `finalize update_media_lists`: 0.00 ms
- `finalize apply_stylesheets`: 10.39 ms
- `finalize compute_styles`: 297.90 ms (3000 elements, 0.099 ms/el) — DOMINANT
- `finalize create_render_item`: 30.81 ms
- `finalize fix_tables`: 0.00 ms
- `finalize init`: 16.03 ms
- `DocxBuilder finalize` (outer, TLOG-inflated): 390.33 ms
- `DocxBuilder build()` TOTAL (outer, TLOG-inflated): 460.84 ms
- `ld_render`: 49.93 ms (layout + draw; per-word draw on justified text estimated
  < 20 ms, not a bottleneck)
- `CSS timing`: calls=3000, total=24.40 ms (font 16.02, bg 8.38, flex 0, other 0)

Element distribution (post-merge): a=171, body=1, br=8, html=1, li=101,
p=1001, span=1687, ul=30 (total ~3000).

## Ordered optimization plan (by expected gain)

1. **Memoize inherited properties in `get_property`** — expected gain: HIGH.
   The traversal spends ~270-290 ms on ~3000 elements with non-CSS work ~0 ms;
   `get_property<T>` walks the parent chain for every property. A per-subtree
   cache of inherited (true-inherited) properties could cut a large fraction.
   Prereq: add a timer around `get_property` (as done for `apply_stylesheet`)
   to quantify before implementing. Files: `litehtml/src/html_tag.cpp`,
   `css_properties.cpp`.

2. **Reduce virtual dispatch in `compute_styles`** — expected gain: MEDIUM.
   Mark leaf classes (`el_text`, `el_style`, `el_image`) `final`; avoid virtual
   calls in the hot child loop. Low risk. Files: `litehtml/include/litehtml/*.h`,
   `html_tag.cpp`.

3. **Optimize `create_render_item` (30.81 ms) and `init` (16.03 ms)** —
   expected gain: MEDIUM-LOW. Secondary but sum to ~47 ms. Profile first
   (`create_render_item` per element). Files: `litehtml/src/render_item.cpp`,
   `element.cpp`.

4. **Optimize `parse_attributes` (13.25 ms)** — expected gain: LOW-MEDIUM.
   Newly exposed; likely string parsing. Profile to confirm. Files: `element.cpp`.

5. **Cache parsed CSS (master/user)** — expected gain: LOW on this corpus
   (0.00 ms here, up to ~3-3.5 ms on styled docs). Low priority; do only after
   1-4. Files: `document.cpp`, `docx_builder.cpp`.

6. **TLOG/fflush cleanup** — not a product optimization but required for
   trustworthy numbers: remove `fflush(stderr)` from `TLOG` (`debug_log.h`) or
   disable `TIMING_LOGS`, then re-measure. Do this before quoting any absolute
   timing in conclusions.

7. **Incremental rebuild** — strategic, HIGH theoretical gain (~400 ms) but
   HIGH risk (failed `lazy-cache` branch showed DOM/render-tree inconsistency).
   Only after 1-6 are exhausted; always keep full-rebuild fallback. Large
   refactor of `finalize`/`compute_styles`.

## Not worth pursuing (measured)
- Lazy layout cache without wiring consumers (failed; crashes).
- Span merging beyond plain-text runs (only -0.9% elements; diminishing returns).
- Rendering/draw batching for justified text (< 4-5% of total; high risk).
- CSS property resolution tuning (only 24 ms / 5%; not the bottleneck).
