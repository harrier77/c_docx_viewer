# litedocx — DOCX rendering via litehtml

Direct DOCX-to-GDI renderer. Loads `.docx` files natively, bypasses the HTML conversion layer used by the legacy viewer, and renders straight into a Win32 DC via the `litedocx_c.h` C ABI.

## Build

```bash
cd C:/Users/pr30565/Desktop/Pure_C/litehtml_host/litedocx
lancia.bat
```

Or manually:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  ".\build_litedocx.ps1" ^
    -Root "C:/Users/pr30565/Desktop/Pure_C/litehtml_host" ^
    -Litehtml "C:/Users/pr30565/Desktop/Pure_C/litehtml_host/litehtml" ^
    -Obj "obj" ^
    -Gcc  "C:/Users/pr30565/Nim_code/mingw64/bin/gcc.exe" ^
    -Gxx  "C:/Users/pr30565/Nim_code/mingw64/bin/g++.exe" ^
    -Here "C:/Users/pr30565/Desktop/Pure_C/litehtml_host/litedocx/"
```

Objects are cached by timestamp so only changed files recompile.

### Build files

| File | Role |
|------|------|
| `build_litedocx.ps1` | Main build orchestrator (PowerShell) |
| `psbuild.ps1` | Per-file compile helper (unused by main script since v2) |
| `build_litedocx.bat` | Legacy batch — delegates to PowerShell script |
| `lancia.bat` | One-click rebuild launcher for Windows |

---

## UTF-8 / Apostrophe Fix (2025-11-04)

### Problem

Italian text containing the right single quotation mark (`'`, U+2019, UTF-8 `E2 80 99`) and the XML entity `&apos;` was followed by 2–4 garbage characters (e.g. `l'8󌽿autonomia`). The apostrophe rendered, but 3–4 extra bytes of heap memory were appended after it.

### Root cause

In `litedocx/third_party/xmlbuf.c`, function `xml_decode()`, the on-stack buffer used to hold decoded entity values was **not zero-initialized**:

```c
// BUGGY (before fix)
char tmp[8];

// FIXED
char tmp[8] = {0};
```

`put_utf8()` writes only the bytes needed for the codepoint and leaves the rest of `tmp` untouched. `strlen(tmp)` then reads past the valid bytes until it encounters a stray `\0` somewhere on the heap — causing a wrong length, a `memcpy` that overwrites adjacent memory, and garbage appended to the decoded string.

For the `&apos;` entity (U+0027, 1-byte UTF-8), `put_utf8` writes `tmp[0] = 0x27` but `tmp[1]` remains uninitialized. `strlen(tmp)` returned 8 instead of 1, and `memcpy(out + w, tmp, 8)` copied 7 bytes of heap junk after the apostrophe.

### Additional fixes in this patch

| File | Change |
|------|--------|
| `third_party/xmlbuf.c` | `char tmp[8]` → `char tmp[8] = {0}` — prevents reading past decoded bytes |
| `third_party/xmlbuf.c` | `xml_decode()`: boundary check `w + l < cap` → `w + l + 1 <= cap` — ensures NUL terminator always fits |
| `src/litedocx_c.cpp` | Added `u8_to_w16()` — UTF-8→UTF-16LE converter with surrogate-pair support |
| `src/litedocx_c.cpp` | `ld_container` now holds `m_measure_dc` (HDC) for text measurement |
| `src/litedocx_c.cpp` | `text_width` callback uses `u8_to_w16` + `GetTextExtentPoint32W` |
| `src/litedocx_c.cpp` | `draw_text` callback uses `u8_to_w16` + `DrawTextW` |
| `src/litedocx_c.cpp` | `ld_destroy()` releases `m_measure_dc` via `ReleaseDC` |
| `build_litedocx.ps1` | Added `-O2 -c` to all compilation invocations |
| `build_litedocx.ps1` | Fixed `Link-Dll` to search subdirectories (`gumbo/`, `litehtml/`, …) for `.o` files |

### Encoding pipeline

```
.docx (ZIP)
  → word/document.xml  (UTF-8 text nodes, XML entities like &apos;)
  → xml_decode()       (converts &apos; → U+0027, decodes UTF-8 sequences)
  → docx_builder.cpp   (collects decoded text into std::string)
  → el_text::m_text    (UTF-8 string stored in litehtml DOM)
  → draw_text() callback
  → u8_to_w16()        (UTF-8 → UTF-16LE, surrogate pairs handled)
  → DrawTextW()        (GDI renders UTF-16 string)
```
