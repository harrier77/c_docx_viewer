# Legacy DOCX-to-HTML pipeline

This folder contains the old, self-contained DOCX-to-HTML pipeline and its
legacy viewer:

- `xmlbuf.c`, `xmlbuf.h` - small XML buffer helper
- `wml.c`, `wml.h` - WordprocessingML parsing
- `htmlbuf.c`, `htmlbuf.h` - HTML output buffer
- `css.c`, `css.h` - minimal CSS handling
- `docx.c`, `docx.h` - DOCX to HTML conversion
- `docxview.c` - old standalone viewer built on this pipeline
- `litehtml_c.h` - old API header used by `docxview.c`

These files are **legacy only**. They are used by the old build scripts under
`bak/` (for example `bak/build.bat` and `bak/build_viewer.bat`) and are not
part of the current viewer.

The current viewer (`maind2dv.c` / `d2dview.exe`) uses `litedocx.dll` for DOCX
rendering and the Miniz library in `../miniz_zip/`.

`docx.c` expects the ZIP implementation in `../zip_lib/`. If you build the
legacy viewer, update the source and include paths in the old scripts under
`bak/` accordingly, because the files no longer live in the project root.
