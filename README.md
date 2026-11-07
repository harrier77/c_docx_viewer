# c_docx_viewer — Visualizzatore DOCX Win32/GDI diretto con litedocx

Visualizzatore nativo `.docx` per Windows scritto in C puro (TCC/GCC/MSVC compatibile).
Niente HTML intermedio: il documento viene convertito direttamente in albero `litehtml`
e renderizzato via GDI.

## Architettura

```
docxview_new.c      → UI Win32, worker thread, scroll, paint, double-buffering
litedocx/           → DLL C++ (C ABI): OOXML → litehtml document tree
  ├─ litedocx_c.cpp  → ABI: ld_open_file, ld_render, ld_draw, ld_get_page_count, ld_render_range
  ├─ docx_builder.cpp→ Conversione WordprocessingML → litehtml elements
  ├─ docx_package.cpp→ Lettura ZIP + parsing XML (document.xml, styles.xml, numbering.xml)
  └─ docx_model.cpp  → Modello dati intermedi (paragraphs, runs, tables, images, media)
litehtml/           → Motore layout HTML/CSS (submodule / header-only build)
```

**Flusso dati:**
```
.docx  →  ld_open_file()  →  ld_render() [worker]  →  ld_draw() [UI thread, clipped to viewport]
```

## Build

```bat
build_viewer.bat
```
Genera `docxview_new.exe` e `litedocx.dll` nella cartella `bin/`.

Richiede:
- `litehtml` (header-only, incluso come submodule in `litehtml/`)
- Windows SDK (GDI, COM, WIC per decode immagini)

## Uso

```bat
docxview_new.exe [percorso/file.docx]
```

Senza argomenti apre `File → Apri` dialog.

### Comandi

| Tasto / Azione | Effetto |
|---|---|
| Mouse wheel / ↑ ↓ / PgUp PgDn / Home End | Scroll |
| `Ctrl+O` | Apri altro `.docx` |
| `F5` | Ricarica file corrente |
| `Esc` | Chiudi |
| Click link | Apre browser / salto anchor |
| Resize finestra | Re-layout immediato (background) |

## Fase D — Background full layout + UI non bloccante (✅ Completata)

| Step | Descrizione | Stato |
|---|---|---|
| **D1** | `JOB_LOAD` chiama solo `ld_open_file`, niente `ld_render` | ✅ |
| **D2** | `apply_loaded_doc` lancia **un solo** job layout background; stima altezza senza triggerare render su UI thread | ✅ |
| **D3** | `request_render_range` ritorna subito se range già renderizzato; pending width (resize) e pending range (scroll) separati | ✅ |
| **D4** | `on_render_done`: `g_layout_complete=1` dopo primo full render; aggiorna `g_last_rendered_page` e `g_page_h_est` con valori reali | ✅ |
| **D5** | Scroll (`WM_VSCROLL`/`WM_MOUSEWHEEL`) non chiama worker se layout completo | ✅ |
| **D6** | `WM_PAINT`: clip `ld_draw` = intersezione area contenuto ∩ viewport (non `doc_h` intero) | ✅ |
| **D7** | `extract_page_geometry`: restituisce stima se full layout non fatto (`m_full_layout_done` flag); geometria reale solo dopo `ld_render` | ✅ |

**Risultati verificati (log):**
- Apertura: UI risponde subito (scheletro/pagina bianca)
- Un solo full layout in background all'apertura
- Salto pagina lontana: **niente pagina bianca**, **nessun job worker**
- Scroll fluido post-layout: solo paint, zero `JOB_RENDER_RANGE`
- Paint documenti lunghi (200k+ px): clip al viewport → frame time costante
- Zero `ld_render`/`document::render` su thread UI prima del worker

## Fase B — Early-exit layout incrementale (opzionale)

Da valutare **solo se** first-content-time > 1-2s su documenti grandi (>100 pagine).
Aggiungerebbe `y_limit` a `document::render()` per layoutare solo fino alla pagina visibile + lookahead.

## Logging diagnostico

Abilitato di default in Fase D:
- `logs.txt` (cartella Desktop): eventi UI (JOB, RENDER, PAINT, SCROLL)
- `stderr` (console/debug): eventi ABI litedocx (`LDLOG`)

```c
// In docxview_new.c
#define LOG_JOB(kind, msg)    log_msg("[JOB] %s: %s\n", #kind, msg)
#define LOG_RENDER(msg)       log_msg("[RENDER] %s\n", msg)
#define LOG_PAINT(msg)        log_msg("[PAINT] %s\n", msg)
#define LOG_SCROLL(msg)       log_msg("[SCROLL] %s\n", msg)

// In litedocx_c.cpp
#define LDLOG(fmt, ...) fprintf(stderr, "[LITEDOCX] " fmt "\n", ##__VA_ARGS__)
```

## File principali

| File | Ruolo |
|---|---|
| `docxview_new.c` | UI, worker thread, message loop, paint double-buffered |
| `litedocx/src/litedocx_c.cpp` | C ABI: `ld_open_file`, `ld_render`, `ld_render_range`, `ld_draw`, `ld_get_page_count`, `ld_get_page_height`, `ld_destroy` |
| `litedocx/src/docx_builder.cpp` | Costruzione albero `litehtml` da `DocxModel` |
| `litedocx/src/docx_package.cpp` | Parsing OOXML/ZIP → `DocxModel` |
| `litedocx/include/litedocx_c.h` | Header pubblico ABI C |

## Limitazioni note

- Liste (`numbering.xml`): supporto base (bullet/numbered), nesting limitato
- Tabelle complesse: `colspan`/`rowspan` OK, bordi custom via callback opzionale
- Immagini: decode WIC (PNG/JPEG/BMP/GIF/TIFF), cache LRU per-thread
- Float/positioned CSS: supporto `litehtml` standard, early-exit Fase B non li gestisce
- Paginazione: basata su `<div data-page-break>` iniettati dal builder; non Word-perfect

## Licenza

Codice sorgente: MIT (vedi `LICENSE`).
`litehtml`: BSD-2-Clause.
`puff.c` (inflate): zlib license.