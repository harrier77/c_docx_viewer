# c_docx_viewer — Native Win32/Direct2D DOCX Viewer with litedocx

A native `.docx` viewer for Windows written in pure C (GCC/MinGW compatible).
Uses **Direct2D/DirectWrite** for hardware-accelerated rendering.
The document is converted directly to a `litehtml` tree by `litedocx.dll`
and rendered via a modular Direct2D pipeline.

## Architecture (current `main` branch)

```
maind2dv.c              → Win32 entry point, message loop (unity build)
viewer_includes/        → Modular viewer components (included by maind2dv.c)
  ├─ viewer_state.c      → Configuration, types, global state (fonts, zoom, jobs, pagination)
  ├─ viewer_log.c        → File logging & console diagnostics
  ├─ viewer_fonts.c      → DirectWrite font cache & text callbacks
  ├─ viewer_callbacks.c  → litedocx → renderer callbacks (draw text, fill, borders, images)
  ├─ viewer_jobs.c       → Job queue & worker thread (load, render, range, destroy)
  ├─ viewer_render.c     → Render requests, async loading, pagination state
  ├─ viewer_lifecycle.c  → Window create/destroy (DWrite factory, D2D renderer)
  ├─ viewer_paint.c      → WM_PAINT (D2D zoom transform, document_draw, Present)
  ├─ viewer_input.c      → Resize, pan, scroll, zoom, mouse events
  └─ viewer_commands.c   → File menu commands (Open, Reload, Exit, About)

func_gdi/               → Shared layout & rendering primitives
  ├─ layout.c/h          → Layout state, scroll, pan, zoom, content rect
  ├─ renderer_d2d.c/h    → Direct2D renderer implementation
  ├─ renderer.h          → Renderer vtable (FillRect, DrawText, DrawImage, etc.)
  ├─ text_utf8.c/h       → UTF-8 ↔ UTF-16 conversion
  └─ document.c/h        → Document drawing, page geometry, scrollbars

litedocx/               → C++ DLL (C ABI): OOXML → litehtml document tree
  ├─ litedocx_c.cpp      → ABI: ld_open_file, ld_render, ld_render_range, ld_draw, ld_get_page_count...
  ├─ docx_builder.cpp    → WordprocessingML → litehtml elements conversion
  ├─ docx_package.cpp    → ZIP reading + XML parsing (document.xml, styles.xml, numbering.xml)
  └─ docx_model.cpp      → Intermediate data model (paragraphs, runs, tables, images, media)

main_include/           → Legacy/original source files (preserved)
  ├─ docxview_new.c      → Original GDI-based viewer (reference)
  ├─ docxview.c          → Earlier HTML-based viewer
  └─ ...                 → Other historical files (css, htmlbuf, wml, xmlbuf, zip, inflate)

bak/                    → Historical builds and notes
```

**Data flow:**
```
.docx → ld_open_file() [worker] → ld_render() [worker, full layout]
          ↓
       UI thread: apply_loaded_doc() → request_render() [async]
          ↓
       WM_PAINT: document_draw() → D2D renderer → Present → screen
```

## Build

```bat
build_d2dview.bat
```

Generates `d2dview.exe` (and copies `litedocx.dll` + `liblitedocx.a` to working dir).

Requires:
- `litehtml` (header-only, included in `litehtml/`)
- Windows SDK (Direct2D, DirectWrite, WIC for image decoding)
- MinGW-w64 GCC (tested with `C:\Users\pr30565\Nim_code\mingw64\bin\gcc.exe`)

## Usage

```bat
d2dview.exe [path/to/file.docx]
```

Without arguments opens `File → Open` dialog.

### Commands

| Key / Action | Effect |
|---|---|
| Mouse wheel / ↑ ↓ / PgUp PgDn / Home End | Vertical scroll |
| Middle-drag | Pan (horizontal + vertical, layout space) |
| `Shift` + wheel | Pan horizontal |
| `Ctrl` + wheel | **Pure renderer zoom** (0.9x–3.0x, no re-layout) |
| `Ctrl+O` | Open another `.docx` |
| `F5` | Reload current file |
| `Esc` / `File → Exit` | Close |
| Click link | Shows link URL in message box |
| Window resize | Background re-layout (non-blocking UI) |

## Key Features (Phase D+ Complete)

- **Pure Direct2D zoom**: Layout stays at 1.0; zoom is a D2D transform applied at draw time — no re-layout, no text duplication.
- **Background loading**: `ld_open_file` runs on worker thread; UI remains responsive.
- **On-demand pagination**: First render does full layout; subsequent scrolls only render missing page ranges.
- **Viewport-clipped painting**: Only visible content drawn; constant frame time on long documents.
- **Font caching**: DirectWrite `IDWriteTextFormat` deduplicated by family/size/style.
- **Worker-thread document lifecycle**: All `ld_*` calls on same thread (COM affinity).

## Dev Tools (in `dev/`)

| Tool | Purpose |
|---|---|
| `check_justify.exe` | Test justified text layout in litehtml |
| `check_litehtml.exe file.docx` | Convert DOCX → HTML → litehtml layout metrics |
| `dump_html.exe file.docx [out.html]` | Export generated HTML for inspection |
| `zip_test.exe file.docx` | Extract and print `word/document.xml` |

Build with:
```bat
cd dev
..\build_dev_tools.bat   # (or compile manually with -Imain_include)
```

## Diagnostic Logging

- **File** (`logs.txt` next to exe): `ENABLE_FILE_LOGGING=1` in `viewer_state.c`
- **Console** (`stderr`): `CONSOLE_LOGGING=1` + `TABLE_DEBUG=1` for table layout traces
- **litedocx DLL**: `LITEDOCX_CONSOLE_LOG` in `litedocx_c.cpp`

```c
// viewer_state.c
#define CONSOLE_LOGGING 0      // set 1 for stderr logs
#define ENABLE_FILE_LOGGING 0  // set 1 for logs.txt
#define TABLE_DEBUG 0          // 1 = full table-layout trace
```

## Known Limitations

- **Lists** (`numbering.xml`): basic bullet/numbered support, limited nesting
- **Complex tables**: `colspan`/`rowspan` OK, custom borders via optional callback
- **Images**: WIC decode (PNG/JPEG/BMP/GIF/TIFF), per-thread LRU cache
- **Float/positioned CSS**: standard `litehtml` support
- **Pagination**: based on `<div data-page-break>` injected by builder; not Word-perfect
- **No GDI fallback**: Direct2D only; requires Win7+ with Platform Update / Win8+

## License

Source code: MIT (see `LICENSE`).
`litehtml`: BSD-2-Clause.
`puff.c` (inflate): zlib license.