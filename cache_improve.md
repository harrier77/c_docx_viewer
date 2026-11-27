# Ottimizzazione delle prestazioni di `d2dview` (cache e rebuild)

## Il problema
Quando si modifica un paragrafo in `d2dview`, `ld_paragraph_set_ooxml` ricostruisce l'intero documento (`DocxBuilder::build()` + `finalize` in `litehtml`). Su un documento di **200 pagine**, questa operazione richiedeva circa **~500 ms** (~443 ms il solo `build()`).

Per mitigare l'impatto visivo, il viewer mantiene a schermo l'ultimo frame valido durante il ricalcolo (`g_render_busy` in `viewer_paint.c`), evitando il flash della pagina bianca.

---

## La scoperta e l'ottimizzazione fatta (`TextWidthCache`)

Il profiling dettagliato aveva mostrato che **~303 ms** di `build()` andavano in `finalize compute_styles`, ma solo ~24 ms erano sui 3.000 nodi `html_tag`: **269,52 ms** erano nelle **63.392 chiamate** a `el_text::compute_styles`, di cui **259,10 ms** nel solo `text_width` (`IDWriteFactory_CreateTextLayout` per ogni parola nuova).

La `TextWidthCache` originale era una LRU da **1.024 voci** (in *thrashing* continuo su 63k parole), allocava `std::string` + `std::list` a ogni lookup ed era indicizzata sul puntatore `DwFontHandle*` (invalidato a ogni rebuild).

### Intervento applicato (`litedocx_c.cpp`)
- Sostituita con una tabella hash **4-way set-associative da 65.536 slot**, senza allocazioni su cache hit.
- Chiave font stabile calcolata da `(family, size, bold, italic)`, così la cache sopravvive ai rebuild dopo un edit.

### Risultati misurati (200 pagine)

| Metrica | Prima | Apertura (dopo) | **Edit (dopo)** |
|---|---:|---:|---:|
| `TextWidthCache` | 1.024 LRU | 53.048 hit / 10.344 miss (**83,7%**) | **63.361 hit / 33 miss (99,95%)** |
| `el_text::compute_styles` | 269,52 ms | 198,03 ms | **13,13 ms (-95%)** |
| — di cui `text_width` | 259,10 ms | 193,06 ms | **9,61 ms (-96%)** |
| `finalize compute_styles` | 302,91 ms | 237,81 ms | **32,15 ms (-89%)** |
| `DocxBuilder finalize` | 384,96 ms | 320,28 ms | **114,05 ms (-70%)** |
| `DocxBuilder build() TOTAL` | 442,88 ms | 379,25 ms | **177,04 ms (-60%)** |
| `ld_paragraph_set_ooxml total` | ~500 ms | — | **259,30 ms** |

*(Il rendering a schermo `ld_render` impiega **~46–52 ms** e non è il collo di bottiglia.)*

---

## Cosa è già stato fatto (branch `page_prova`)

- **UX "last frame"** (`499defe`): durante il rebuild il viewer mostra l'ultimo frame valido invece della pagina vuota.
- **Strumentazione timing** (`build_timing.bat` -> `d2dview_timing.exe`): build console `-O2` dedicata al profiling; rimosso `fflush(stderr)` da `TLOG` (`debug_log.h`) per eliminare il gonfiaggio dei timer esterni.
- **Span merging** (`3b1d549`): fusione di run adiacenti identici in `DocxBuilder::process_run` (-0.9% span su questo corpus, sicuro ma impatto trascurabile).
- **Pulizia timer interni** (`17e206b` + rimozione timer `el_text`): rimossi i `std::chrono::now()` per-nodo da `html_tag::compute_styles` ed `el_text::compute_styles`.
- **Logging Release esplicito** (`be350e5`): aggiunto `-DENABLE_FILE_LOGGING=0` in `build_d2dview.bat`.
- **`TextWidthCache` 65k + chiave font stabile** (`litedocx_c.cpp`): abbattuto `compute_styles` in edit da 303 ms a 32 ms.

---

## Prossime priorità (in ordine di impatto)

1. **Apertura: velocizzare `cb_text_width` sui *miss*** *(target: ~193 ms in apertura)*
   - I 10.344 *miss* dell'apertura chiamano `IDWriteFactory_CreateTextLayout` + `Release` per ogni singola parola (~18,6 µs/parola).
   - Usare una cache delle larghezze dei glifi/caratteri tramite `IDWriteFontFace` per testo semplice, evitando l'oggetto COM `IDWriteTextLayout` per parola.
   - File: `viewer_includes/viewer_fonts.c`.

2. **Edit: profilare `serialize_document_xml` + `extract_paragraph_ooxml`** *(target: ~73 ms)*
   - Oggi `ld_paragraph_set_ooxml` impiega **259,30 ms** totali contro **185,80 ms** di `build()`: quei ~73 ms sono serializzazione XML e ri-estrazione degli indici dei paragrafi.
   - File: `litedocx/src/litedocx_c.cpp`, `litedocx/src/docx_model.cpp`.

3. **Voci secondarie del rebuild**
   - `DocxBuilder build_body`: **~47 ms**
   - `finalize create_render_item` + `init`: **~40 ms** (`27,6 + 12,5 ms`)

4. **Rebuild incrementale per singolo paragrafo** *(strategico, alto rischio)*
   - Da valutare **solo** se, dopo i punti 1–3, la latenza fosse ancora percepibile.
   - Il tentativo precedente (branch `lazy-cache`, eliminato) fallì per cache vuota su hit-test/caret e incoerenze DOM/render tree. Se ripreso, mantenere **sempre** il fallback al full-rebuild.

---

## Piste scartate (già verificate)

- **Lazy layout cache senza cablare i consumer**: crash su hit-test e caret.
- **Memoization di `get_property`**: misurata a 249.522 chiamate = **3.90 ms totali**.
- **Ottimizzazione risoluzione proprietà CSS**: `css_properties::compute` pesa ~4–20 ms.
- **Span merging più aggressivo**: pochi run contigui con stile identico nel corpus reale (-0.9% span).
- **Batching del draw su testo giustificato**: `< 20 ms` su `ld_render`, alto rischio di regressioni visive.
