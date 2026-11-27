# Ottimizzazione delle prestazioni di `d2dview` (cache e rebuild)

## Il problema
Quando si modifica un paragrafo in `d2dview`, `ld_paragraph_set_ooxml` ricostruisce l'intero documento (`DocxBuilder::build()` + `finalize` in `litehtml`). Su un documento di **200 pagine**, questa operazione richiedeva circa **~500 ms** (~443 ms il solo `build()`).

Per mitigare l'impatto visivo, il viewer mantiene a schermo l'ultimo frame valido durante il ricalcolo (`g_render_busy` in `viewer_paint.c`), evitando il flash della pagina bianca.

---

## La scoperta e l'ottimizzazione fatta (`TextWidthCache`)

Il profiling dettagliato aveva mostrato che **~303 ms** di `build()` andavano in `finalize compute_styles`, ma solo ~24 ms erano sui 3.000 nodi `html_tag`: **269,52 ms** erano nelle **63.392 chiamate** a `el_text::compute_styles`, di cui **259,10 ms** nel solo `text_width` (`IDWriteFactory_CreateTextLayout` per ogni parola nuova).

La `TextWidthCache` originale era una LRU da **1.024 voci** (in *thrashing* continuo su 63k parole), allocava `std::string` + `std::list` a ogni lookup ed era indicizzata sul puntatore `DwFontHandle*` (invalidato a ogni rebuild).

### Interventi applicati
1. **`TextWidthCache` 65k + chiave font stabile** (`litedocx/src/litedocx_c.cpp`):
   - Sostituita la LRU da 1.024 voci con una tabella hash **4-way set-associative da 65.536 slot**, senza allocazioni su cache hit.
   - Chiave font stabile calcolata da `(family, size, bold, italic)`, così la cache sopravvive ai rebuild dopo un edit.
2. **Fast-path `IDWriteFontFace` in `cb_text_width`** (`viewer_includes/viewer_fonts.c`, `viewer_state.c`, `viewer_lifecycle.c`):
   - Per stringhe ASCII/Latin-1 (`0x20..0xFF`), somma le larghezze per-glifo scalate e cachate in `FontCacheEntry::char_advances[256]` (via `GetGlyphIndices` + `GetDesignGlyphMetrics`), evitando `u8_to_w` e `IDWriteTextLayout` sul **94,4%** dei *miss* (9.767 su 10.344). Fallback automatico su `IDWriteTextLayout` per caratteri > 255 o glifi mancanti.

### Risultati misurati (200 pagine)

| Metrica | Baseline | Apertura (+ cache + `FontFace`) | **Edit (+ cache + `FontFace`)** |
|---|---:|---:|---:|
| `TextWidthCache` | 1.024 LRU | 53.048 hit / 10.344 miss (**83,7%**) | **63.361 hit / 33 miss (99,95%)** |
| `finalize compute_styles` | 302,91 ms | **72,53 ms (-76%)** | **26,50 ms (-91%)** |
| `DocxBuilder finalize` | 384,96 ms | **170,48 ms (-56%)** | **104,31 ms (-73%)** |
| `DocxBuilder build() TOTAL` | 442,88 ms | **235,22 ms (-47%)** | **169,05 ms (-62%)** |
| `ld_paragraph_set_ooxml total` | ~500 ms | — | **249,90 ms (-50%)** |

*(Il rendering a schermo `ld_render` impiega **~45–65 ms** e non è il collo di bottiglia.)*

---

## Cosa è già stato fatto (branch `page_prova`)

- **UX "last frame"** (`499defe`): durante il rebuild il viewer mostra l'ultimo frame valido invece della pagina vuota.
- **Strumentazione timing** (`build_timing.bat` -> `d2dview_timing.exe`): build console `-O2` dedicata al profiling; rimosso `fflush(stderr)` da `TLOG` (`debug_log.h`) per eliminare il gonfiaggio dei timer esterni.
- **Span merging** (`3b1d549`): fusione di run adiacenti identici in `DocxBuilder::process_run` (-0.9% span su questo corpus, sicuro ma impatto trascurabile).
- **Pulizia timer interni** (`17e206b` + rimozione timer `el_text`): rimossi i `std::chrono::now()` per-nodo da `html_tag::compute_styles` ed `el_text::compute_styles`.
- **Logging Release esplicito** (`be350e5`): aggiunto `-DENABLE_FILE_LOGGING=0` in `build_d2dview.bat`.
- **`TextWidthCache` 65k + chiave font stabile** (`litedocx_c.cpp`): hit rate 99,95% in edit.
- **Fast-path `IDWriteFontFace` in `cb_text_width`** (`viewer_fonts.c`, `viewer_state.c`, `viewer_lifecycle.c`): gestito il 94,4% dei *miss* senza `IDWriteTextLayout`, portando `compute_styles` in apertura da 303 ms a 72,5 ms.

---

## Prossime priorità (in ordine di impatto)

1. **Edit: profilare `serialize_document_xml` + `extract_paragraph_ooxml`** *(target: ~71 ms)*
   - Oggi `ld_paragraph_set_ooxml` impiega **249,90 ms** totali contro **178,64 ms** di `build()`: quei ~71 ms sono serializzazione XML e ri-estrazione degli indici dei paragrafi.
   - File: `litedocx/src/litedocx_c.cpp`, `litedocx/src/docx_model.cpp`.

2. **Voci secondarie del rebuild**
   - `DocxBuilder build_body`: **~49 ms**
   - `finalize create_render_item` + `init`: **~46 ms** (`29,5 + 16,4 ms`)

3. **Rebuild incrementale per singolo paragrafo** *(strategico, alto rischio)*
   - Da valutare **solo** se, dopo i punti 1–3, la latenza fosse ancora percepibile.
   - Il tentativo precedente (branch `lazy-cache`, eliminato) fallì per cache vuota su hit-test/caret e incoerenze DOM/render tree. Se ripreso, mantenere **sempre** il fallback al full-rebuild.

---

## Piste scartate (già verificate)

- **Lazy layout cache senza cablare i consumer**: crash su hit-test e caret.
- **Memoization di `get_property`**: misurata a 249.522 chiamate = **3.90 ms totali**.
- **Ottimizzazione risoluzione proprietà CSS**: `css_properties::compute` pesa ~4–20 ms.
- **Span merging più aggressivo**: pochi run contigui con stile identico nel corpus reale (-0.9% span).
- **Batching del draw su testo giustificato**: `< 20 ms` su `ld_render`, alto rischio di regressioni visive.
