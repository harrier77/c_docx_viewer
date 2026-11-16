# Legacy ZIP / inflate implementation

This folder contains a small ZIP and inflate implementation:

- `zip.c`, `zip.h`
- `puff.h`
- `inflate.c`

These files belong to the **legacy DOCX code path only**. They are used by
`docx.c` and by the old build scripts under `bak/` (for example
`bak/build.bat` and `bak/build_viewer.bat`).

They are **not used by the current viewer** (`maind2dv.c` / `d2dview.exe`).
The current viewer uses the Miniz library in `../miniz_zip/` instead.

The files are kept for reference and for anyone who still wants to build the
legacy version. If you do, update the source and include paths in the old
build scripts accordingly, because the files no longer live in the project
root.
