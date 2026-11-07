# c_docx_viewer — Native Win32/GDI DOCX Viewer with litedocx

A native `.docx` viewer for Windows written in pure C (TCC/GCC/MSVC compatible).
No intermediate HTML: the document is converted directly to a `litehtml` tree
and rendered via GDI.

## Architecture

```
docxview_new.c      → Win32 UI, worker thread, scroll, paint, double-buffering
litedocx/           → C++ DLL (C ABI): OOXML → litehtml document tree
  ├─ litedocx_c.cpp  → ABI: ld_open_file, ld_render, ld_draw, ld_get_page_count, ld_render_range
  ├─ docx_builder.cpp→ WordprocessingML → litehtml elements conversion
  ├─ docx_package.cpp→ ZIP reading + XML parsing (document.xml, styles.xml, numbering.xml)
  └─ docx_model.cpp  → Intermediate data model (paragraphs, runs, tables, images, media)
litehtml/           → HTML/CSS layout engine (submodule / header-only build)
```

**Data flow:**
```
.docx  →  ld_open_file()  →  ld_render() [worker]  →  ld_draw() [UI thread, clipped to viewport]
```

## Build

```bat
build_viewer.bat
```
Generates `docxview_new.exe` and `litedocx.dll` in `bin/`.

Requires:
- `litehtml` (header-only, included as submodule in `litehtml/`)
- Windows SDK (GDI, COM, WIC for image decoding)

## Usage

```bat
docxview_new.exe [path/to/file.docx]
```

Without arguments opens `File → Open` dialog.

### Commands

| Key / Action | Effect |
|---|---|
| Mouse wheel / ↑ ↓ / PgUp PgDn / Home End | Scroll |
| `Ctrl+O` | Open another `.docx` |
| `F5` | Reload current file |
| `Esc` | Close |
| Click link | Open browser / jump anchor |
| Window resize | Immediate re-layout (background) |

## Phase D — Background Full Layout + Non-blocking UI (✅ Complete)

| Step | Description | Status |
|---|---|---|
| **D1** | `JOB_LOAD` calls only `ld_open_file`, no `ld_render` | ✅ |
| **D2** | `apply_loaded_doc` launches **single** background layout job; estimates height without triggering render on UI thread | ✅ |
| **D3** | `request_render_range` returns immediately if range already rendered; pending width (resize) and pending range (scroll) separated | ✅ |
| **D4** | `on_render_done`: `g_layout_complete=1` after first full render; updates `g_last_rendered_page` and `g_page_h_est` with real values | ✅ |
| **D5** | Scroll (`WM_VSCROLL`/`WM_MOUSEWHEEL`) doesn't call worker if layout complete | ✅ |
| **D6** | `WM_PAINT`: `ld_draw` clip = intersection of content area ∩ viewport (not entire `doc_h`) | ✅ |
| **D7** | `extract_page_geometry`: returns estimate if full layout not done (`m_full_layout_done` flag); real geometry only after `ld_render` | ✅ |

**Verified results (logs):**
- Open: UI responds immediately (skeleton/blank page)
- Single full layout in background on open
- Jump to distant page: **no blank page**, **no worker job**
- Smooth scroll post-layout: paint only, zero `JOB_RENDER_RANGE`
- Paint long documents (200k+ px): viewport clip → constant frame time
- Zero `ld_render`/`document::render` on UI thread before worker

## Phase B — Incremental Early-exit Layout (Optional)

Evaluate **only if** first-content-time > 1-2s on large documents (>100 pages).
Would add `y_limit` to `document::render()` to lay out only up to visible page + lookahead.

## Diagnostic Logging

Enabled by default in Phase D:
- `logs.txt` (Desktop folder): UI events (JOB, RENDER, PAINT, SCROLL)
- `stderr` (console/debug): litedocx ABI events (`LDLOG`)

```c
// In docxview_new.c
#define LOG_JOB(kind, msg)    log_msg("[JOB] %s: %s\n", #kind, msg)
#define LOG_RENDER(msg)       log_msg("[RENDER] %s\n", msg)
#define LOG_PAINT(msg)        log_msg("[PAINT] %s\n", msg)
#define LOG_SCROLL(msg)       log_msg("[SCROLL] %s\n", msg)

// In litedocx_c.cpp
#define LDLOG(fmt, ...) fprintf(stderr, "[LITEDOCX] " fmt "\n", ##__VA_ARGS__)
```

## Key Files

| File | Role |
|---|---|
| `docxview_new.c` | UI, worker thread, message loop, double-buffered paint |
| `litedocx/src/litedocx_c.cpp` | C ABI: `ld_open_file`, `ld_render`, `ld_render_range`, `ld_draw`, `ld_get_page_count`, `ld_get_page_height`, `ld_destroy` |
| `litedocx/src/docx_builder.cpp` | `litehtml` tree construction from `DocxModel` |
| `litedocx/src/docx_package.cpp` | OOXML/ZIP parsing → `DocxModel` |
| `litedocx/include/litedocx_c.h` | Public C ABI header |

## Known Limitations

- Lists (`numbering.xml`): basic support (bullet/numbered), limited nesting
- Complex tables: `colspan`/`rowspan` OK, custom borders via optional callback
- Images: WIC decode (PNG/JPEG/BMP/GIF/TIFF), per-thread LRU cache
- Float/positioned CSS: standard `litehtml` support, Phase B early-exit doesn't handle them
- Pagination: based on `<div data-page-break>` injected by builder; not Word-perfect

## License

Source code: MIT (see `LICENSE`).
`litehtml`: BSD-2-Clause.
`puff.c` (inflate): zlib license.