# docx2html - DOCX -> HTML in C puro per litehtml (TCC)

C library without `main()` and without CLI: reads a `.docx` file and returns a
UTF-8 HTML string with **embedded CSS in a `<style>` block**, ready for
`lh_create()` of `litehtml_c.dll`.

```
docx.h          Public API (docx_file_to_html / docx_mem_to_html / free)
docx.c          Orchestration: file -> ZIP -> XML -> full HTML
zip.c/.h        ZIP reader (EOCD, central directory, stored + deflate)
inflate.c       puff.c (Mark Adler, zlib license) - raw inflate, C89
puff.h          puff header
xmlbuf.c/.h     Sequential XML scanner + entity decoding
wml.c/.h        WordprocessingML -> HTML (paragraphs, headings, run, link, w:jc)
css.c/.h        Embedded CSS theme (without var(), not supported by litehtml)
build.bat       Compiles the library and test tools in dev\
```

docxview.c      Win32 viewer (window + GDI + litehtml)
litehtml_c.h    C bridge header of the DLL
build_viewer.bat Compiles docxview.exe

## Usage

```c
#include "docx.h"

docx_html h;
if (docx_file_to_html("test.docx", NULL, &h)) {
    void *doc = lh_create(h.data, &callbacks, width, height);
    int w = lh_render(doc, width);
    lh_draw(doc, hdc, 0, 0, 0, 0, width, height);
    lh_destroy(doc);
    docx_html_free(&h);
} else {
    /* h.error contains the reason (in Italian) */
}
```

Options (all optional; `NULL` = default):

```c
docx_html_opts o = DOCX_HTML_OPTS_INIT; /* {NULL,NULL,0,1} */
o.title = "My document";         /* <title>; default = filename */
o.extra_css = "p{color:#333;}"; /* appended to <style> block */
o.font_px = 15;                /* base font in px */
o.full_document = 0;           /* fragment only, without <html>/<style> */
```

`docx_mem_to_html(buf, size, ...)` does the same from an in-memory `.docx`
(useful if the file comes from a resource or network).

## `docxview.exe` Viewer

`docxview.c` is the viewer app: converts `.docx` and renders it in a Win32 window
using `litehtml_c.dll` (same scheme as `sunburst.c` panel).

```
bat
build_viewer.bat
docxview.exe test.docx
docxview.exe dev\fixture.docx
```

Without arguments opens `test.docx` next to the exe; if not found it shows the
"Open" dialog.

Commands:

| Command | Action |
|---|---|
| Mouse wheel / ↑ ↓ / PgUp PgDn / Home End | Scrolling |
| `Ctrl+O` | Open another `.docx` |
| `F5` | Reload current file |
| `Esc` | Close |
| Click on a link | Opens browser (or jumps to `#anchor`) |
| Window resizing | Immediate litehtml re-layout |

Technical notes:

- **Sheet layout**: document is laid out at page width (max 960 px) and drawn
  centered on a gray desk, with light border and shadow; text wraps as on paper
  and inner margins are `60px 64px` (override CSS passed by the viewer). Reducing
  the window the page shrinks keeping 24 px margin on the side;
- GDI double buffering (`CreateCompatibleDC` + `BitBlt`), no flicker;
- Fonts, metrics and wrapping go through `lh_callbacks`: UTF-8 texts are
  converted to UTF-16 for `DrawTextW`/`GetTextExtentPoint32W`;
- Links are opened with `ShellExecuteA` loaded at runtime from `shell32.dll`
  (tcc does not have the import library);
- `docxview` looks for `litehtml_c.dll` in its own execution folder (same repo
  as `docxview_new.c` and `litedocx/`), otherwise in the absolute path used
  during development.

Requires `litehtml_c.dll` and, to compile, the library sources in the same folder.

## Compilation

```bat
build.bat
```

Verifies the library with `-Wall` and produces:

- `dev\dump_html.exe` — converts and writes HTML to file/stdout (validation)
- `dev\check_litehtml.exe` — converts, loads `litehtml_c.dll`, runs
  `lh_create`/`lh_render` and prints `width`/`height` (integration test)
- `dev\check_justify.exe` — renders a justified paragraph and prints
  the position of each text piece (validation of `text-align:justify`)

To add the host app, just append the `.c` files to the existing tcc command:

```bat
tcc app.c docx.c zip.c inflate.c xmlbuf.c htmlbuf.c wml.c css.c ^
    -o app.exe -lgdi32 -luser32 -lkernel32 -Wall
```

## What is converted

| DOCX | HTML |
|---|---|
| `w:p` | `<p>` |
| `w:pStyle` = `Heading1..9`, `Title`, `Subtitle` | `<h1>`..`<h6>` |
| `w:b` / `w:i` / `w:u` | `<strong>` / `<em>` / `<u>` |
| `w:color`, `w:sz` | `<span style="color:#…;font-size:…px">` |
| `w:t` | text with escaping and spaces preserved |
| `w:br`, `w:cr` | `<br>` |
| `w:tab` | four `&#160;` |
| `w:hyperlink` (r:id / anchor) | `<a href="…">` via `document.xml.rels` |
| `w:jc` (direct, from paragraph style or from `docDefaults`) | `style="text-align:left\|center\|right\|justify"` |
| `w:instrText`, `w:delText` | ignored |

For `w:jc` the Word hierarchy applies: `w:pPr/w:jc` of the paragraph, then the
style indicated by `w:pStyle` following the `w:basedOn` chain, then the default
style (`w:default="1"`), finally `w:docDefaults` (read only from
`word/styles.xml`, which is read for this purpose). Values `both`/`distribute`
become `justify`: litehtml extends intermediate lines to the right edge and
leaves the last line unchanged.

Out of scope (for now): lists (`numbering.xml`), tables, images, aspects of
styles other than alignment. Paragraphs inside tables/lists are nevertheless
emitted as linear `<p>`, so the text is not lost.

## Known limits

- ZIP64, ZIP encrypted and compression methods other than store/deflate are
  rejected with an explicit error.
- Maximum decompressed entry size: 64 MB; maximum `.docx` file size: 128 MB.
- The document must contain `word/document.xml` (standard OOXML).
- A trailing space of a run becomes `&#160;`, so there is no line break at that
  point: it is the compromise to not lose Word spacing.

## About litehtml

- CSS uses only supported properties: **no `var(--…)`, no grid**.
- Theme does not set `max-width`: wrapping is decided by `lh_render(doc, width)`.
- Units are px (base font configurable with `font_px`).
- Leading/trailing spaces and multiple spaces become `&#160;` (makes them visible
  to litehtml); internal single spaces remain normal spaces to allow line break.
- Images are not supported: `litehtml_c.dll` has no image callback.

## Testing

```bat
dev\dump_html.exe test.docx dev\out.html
dev\check_litehtml.exe test.docx
dev\check_litehtml.exe dev\fixture.docx
dev\check_justify.exe
```

Expected results:

- `test.docx` -> 3 paragraphs, `layout: width=800 height=138`
- `fixture.docx` -> h1/h2/h3 titles, bold, italic, links, accents and
  typographic quotes preserved (UTF-8), `height ~ 310`
- `check_justify.exe` -> intermediate lines end at `right=400`
  (words spaced out), the last line remains unextended

## Known limits

- ZIP64, ZIP encrypted and compression methods other than store/deflate are
  rejected with an explicit error.
- Maximum decompressed entry size: 64 MB; maximum `.docx` file size: 128 MB.
- The document must contain `word/document.xml` (standard OOXML).
- A trailing space of a run becomes `&#160;`, so there is no line break at that
  point: it is the compromise to not lose Word spacing.