# Ottimizzazione delle prestazioni di `d2dview` (cache e rebuild)

## Il problema
Quando si modifica un paragrafo in `d2dview`, `ld_paragraph_set_ooxml` ricostruisce l'intero documento (`DocxBuilder::build()` + `finalize` in `litehtml`). Su un documento di **200 pagine**, la versione iniziale richiedeva circa **~500 ms**.

Per mitigare l'impatto visivo, il viewer mantiene a schermo l'ultimo frame valido durante il ricalcolo (`g_render_busy` in `viewer_paint.c`), evitando il flash della pagina bianca.

---

## Stato attuale (branch `page_prova`, HEAD `5f7db3a`)

Le misure seguenti provengono da `build_timing.bat` → `d2dview_timing.exe`: `litedocx` compilato in configurazione `Timing` (`-O2`, `TIMING_LOGS=1`, log di debug disattivati — `litedocx/build_litedocx.ps1:58-62`) e viewer `-O2`. Il corpus è sempre il documento da 200 pagine: **3.000 elementi** (`p=1001`, `span=1687`, `a=171`, `li=101`, `ul=30`, `br=8`, `body=1`, `html=1`).

### Confronto apertura vs edit (edit semplice su un paragrafo)

| Fase | Apertura | Edit | Δ |
|---|---:|---:|---:|
| `DocxBuilder xml_parse` | 4,01 | 7,89 | +3,88 |
| `DocxBuilder parse_section` | 0,05 | 0,10 | +0,05 |
| `DocxBuilder build_body` | 45,88 | 74,69 | **+28,81** |
| `DocxBuilder css/gen` | 0,02 | 0,04 | +0,02 |
| `finalize master/user css` | 1,02 | 2,56 | +1,54 |
| `finalize apply_master_css` | 6,69 | 7,18 | +0,49 |
| `finalize parse_attributes` | 1,01 | 2,58 | +1,57 |
| `finalize parse_linked_css` | 0,00 | 0,00 | 0 |
| `finalize update_media_lists` | 0,00 | 0,00 | 0 |
| `finalize apply_stylesheets` | 9,79 | 12,37 | +2,58 |
| `finalize compute_styles` | 55,83 | 43,27 | **−12,56** |
| `finalize create_render_item` | 29,65 | 49,13 | **+19,48** |
| `finalize fix_tables` | 0,00 | 0,00 | 0 |
| `finalize init` | 16,93 | 27,11 | **+10,18** |
| `DocxBuilder finalize` | 132,79 | 177,75 | +44,96 |
| `DocxBuilder build() TOTAL` | 189,64 | 275,09 | **+85,45** |
| `ld_paragraph_set_ooxml build()` (timer esterno) | — | 294,87 | — |
| `ld_paragraph_set_ooxml serialize` | — | **0,80** | — |
| `ld_paragraph_set_ooxml total` | — | **386,42** | — |

### Cache, CSS e rendering

| Metrica | Apertura | Edit |
|---|---:|---:|
| `TextWidthCache` | 53.048 hit / 10.344 miss (83,7%) | **63.361 hit / 33 miss (99,95%)** |
| `cb_text_width` | fast=9.767, fallback=551, no_face=26 | fast=33, fallback=0, no_face=0 |
| `CSS timing` (3.000 call) | 8,74 ms = font 4,52 + bg 4,22 | 11,94 ms = font 8,39 + bg 2,54 + other 1,01 |
| `JOB_RENDER ld_render` | 52,86 ms | 52,02 ms |

### Cosa dicono questi numeri

1. **La vecchia priorità 1 (serializzazione XML) è risolta**: `serialize_document_xml` ora costa **0,80 ms** (contro i ~71 ms stimati in questa pagina prima di `5f7db3a`), e la ri-estrazione degli OOXML viene saltata quando il paragrafo modificato contiene un solo `</w:p>`.
2. **L'edit è ancora ~2x più lento dell'apertura** (386,42 ms totali, 275–295 ms di `build()`) *nonostante* la cache di testo sia calda al 99,95%. Il costo si è spostato da `compute_styles` (che anzi migliora: −12,56 ms) a `build_body` (+28,81), `create_render_item` (+19,48) e `init` (+10,18).
3. **Il `CSS font` raddoppia in edit** (4,52 → 8,39 ms) pur con la `TextWidthCache` quasi perfetta: il tempo non è più nella misura delle stringhe, ma altrove nella risoluzione delle proprietà font.
4. **Ci sono buchi di strumentazione** (differenze tra totali e somma dei sotto-timer):
   - `finalize`: 11,87 ms in apertura, **33,55 ms in edit** non coperti dai sotto-timer;
   - `build()`: 6,89 ms / 14,62 ms non coperti;
   - `ld_paragraph_set_ooxml total` − (serialize + build esterno) = **~90,75 ms** da attribuire;
   - timer `build()` esterno 294,87 ms vs interno 275,09 ms: **~19,78 ms** di differenza.
5. `ld_render` resta stabile a **~52 ms** in entrambi i percorsi: non è il collo di bottiglia del rebuild, ma diventa rilevante se l'obiettivo è scendere sotto i ~100 ms totali per edit.

> I numeri della vecchia tabella in questa pagina (es. `build()` 235,22 ms in apertura, 249,90 ms per l'edit) erano di una misurazione precedente e non sono confrontabili direttamente con quelli qui sopra, perché il build diagnostico usa ora `Timing` = `-O2` su entrambe le DLL e il viewer.

---

## Interventi applicati (cronologici sul branch)

1. **UX "last frame"** (`499defe`): durante il rebuild il viewer mostra l'ultimo frame valido invece della pagina vuota.
2. **Strumentazione timing** (`dc854cf`, `build_timing.bat` → `d2dview_timing.exe`): timer dedicati in `document.cpp`/`el_text.cpp`, poi rimossi dai nodi per non gonfiare le misure. Rimosso `fflush(stderr)` da `TLOG` (`debug_log.h`). Nota: il commento di `build_timing.bat` dice "Debug", ma `build_litedocx.ps1` compila la configurazione `Timing` in `-O2`.
3. **Span merging** (`3b1d549`): fusione di run adiacenti identici in `DocxBuilder::process_run` (−0,9% span su questo corpus, sicuro ma impatto trascurabile).
4. **Pulizia timer interni** (`17e206b` + rimozione timer `el_text`): rimossi i `std::chrono::now()` per-nodo da `html_tag::compute_styles` ed `el_text::compute_styles`.
5. **Logging Release esplicito** (`be350e5`): aggiunto `-DENABLE_FILE_LOGGING=0` in `build_d2dview.bat`.
6. **`TextWidthCache` 65k + chiave font stabile** (`8945518`, `litedocx/src/litedocx_c.cpp`): tabella hash 4-way set-associative da 65.536 slot, zero allocazioni su hit, chiave derivata da `(family, size, bold, italic)` così la cache sopravvive ai rebuild. Hit rate in edit: 99,95%.
7. **Fast-path `IDWriteFontFace` in `cb_text_width`** (`5fcc9a0`, `viewer_fonts.c`, `viewer_state.c`, `viewer_lifecycle.c`, `viewer_jobs.c`): per stringhe ASCII/Latin-1 somma le larghezze per-glifo scalate e cachate in `FontCacheEntry::char_advances[256]`, evitando `u8_to_w` e `IDWriteTextLayout` sul 94,4% dei miss in apertura. Fallback automatico per caratteri > 255 o glifi mancanti.
8. **Serializzazione/ri-estrazione edit** (`5f7db3a`, `litedocx_c.cpp`, `docx_package.cpp`): `reserve`+`emplace_back` in `extract_paragraph_ooxml`, salvataggio della ri-estrazione per OOXML a paragrafo singolo, timer espliciti su serialize/extract. Serializzazione: 0,80 ms.

---

## Prossime priorità (aggiornate, in ordine di impatto)

1. **Spiegare i ~90 ms fuori timer in `ld_paragraph_set_ooxml`** e i ~20 ms di scarto tra timer `build()` esterno/intero; poi instrumentare i ~33 ms non coperti da `finalize` in edit. Senza questo non si sa quanto del ritardo sia rebuild e quanto gestione del documento/viewer.
2. **`DocxBuilder build_body`**: 74,69 ms in edit (45,88 in apertura). È la singola fase più costosa del percorso edit insieme a `finalize`.
3. **`finalize create_render_item` + `init`**: 49,13 + 27,11 ms in edit (29,65 + 16,93 in apertura). Verificare QUARTZ/allocazioni per nodo e eventuale duplicazione di strutture già presenti nel vecchio documento.
4. **`CSS font` in edit**: 8,39 ms vs 4,52 ms in apertura con cache calda; capire cosa viene ricalcolato per-parameter (font description, metrics, lookup).
5. **Rebuild incrementale per singolo paragrafo** *(strategico, alto rischio)*: da valutare **solo** dopo aver chiuso i punti 1–4 se la latenza fosse ancora percepibile. Il tentativo precedente (branch `lazy-cache`, eliminato) fallì per cache vuota su hit-test/caret e incoerenze DOM/render tree. Se ripreso, mantenere **sempre** il fallback al full-rebuild.

---

## Piste scartate (già verificate)

- **Lazy layout cache senza cablare i consumer**: crash su hit-test e caret.
- **Memoization di `get_property`**: misurata a 249.522 chiamate = **3,90 ms totali**.
- **Ottimizzazione risoluzione proprietà CSS**: `css_properties::compute` pesa ~4–20 ms.
- **Span merging più aggressivo**: pochi run contigui con stile identico nel corpus reale (−0,9% span).
- **Batching del draw su testo giustificato**: `< 20 ms` su `ld_render`, alto rischio di regressioni visive.
- **Ottimizzare `serialize_document_xml` / `extract_paragraph_ooxml`**: risolto da `5f7db3a` (serialize 0,80 ms, extract saltato sul paragrafo singolo); non è più un collo di bottiglia.

---

## Log grezzi (`d2dview_timing.exe`)

### Apertura
```text
[TIMING] DocxBuilder xml_parse took 4.01 ms
[TIMING] DocxBuilder parse_section took 0.05 ms
[TIMING] DocxBuilder build_body took 45.88 ms
[TIMING] DocxBuilder css/gen took 0.02 ms
[TIMING] finalize master/user css took 1.02 ms
[TIMING] finalize apply_master_css took 6.69 ms
[TIMING] finalize parse_attributes took 1.01 ms
[TIMING] finalize parse_linked_css took 0.00 ms
[TIMING] finalize update_media_lists took 0.00 ms
[TIMING] finalize apply_stylesheets took 9.79 ms
[TIMING] finalize compute_styles took 55.83 ms (3000 elements, 0.019 ms/element)
[TIMING] finalize create_render_item took 29.65 ms
[TIMING] finalize fix_tables took 0.00 ms
[TIMING] finalize init took 16.93 ms
[TIMING] DocxBuilder finalize took 132.79 ms
[TIMING] DocxBuilder build() TOTAL took 189.64 ms
[TIMING] CSS timing: calls=3000 total=8.74 ms font=4.52 ms bg=4.22 ms flex=0.00 ms other=0.00 ms
[TIMING] HTML_TAG tags: a=171 body=1 br=8 html=1 li=101 p=1001 span=1687 ul=30
[TIMING] TextWidthCache (open): hits=53048 misses=10344
[TIMING] JOB_RENDER ld_render took 52.86 ms
[TIMING] cb_text_width: fast=9767 fallback=551 no_face=26
```

### Edit semplice
```text
[TIMING] ld_paragraph_set_ooxml serialize took 0.80 ms
[TIMING] DocxBuilder xml_parse took 7.89 ms
[TIMING] DocxBuilder parse_section took 0.10 ms
[TIMING] DocxBuilder build_body took 74.69 ms
[TIMING] DocxBuilder css/gen took 0.04 ms
[TIMING] finalize master/user css took 2.56 ms
[TIMING] finalize apply_master_css took 7.18 ms
[TIMING] finalize parse_attributes took 2.58 ms
[TIMING] finalize parse_linked_css took 0.00 ms
[TIMING] finalize update_media_lists took 0.00 ms
[TIMING] finalize apply_stylesheets took 12.37 ms
[TIMING] finalize compute_styles took 43.27 ms (3000 elements, 0.014 ms/element)
[TIMING] finalize create_render_item took 49.13 ms
[TIMING] finalize fix_tables took 0.00 ms
[TIMING] finalize init took 27.11 ms
[TIMING] DocxBuilder finalize took 177.75 ms
[TIMING] DocxBuilder build() TOTAL took 275.09 ms
[TIMING] CSS timing: calls=3000 total=11.94 ms font=8.39 ms bg=2.54 ms flex=0.00 ms other=1.01 ms
[TIMING] HTML_TAG tags: a=171 body=1 br=8 html=1 li=101 p=1001 span=1687 ul=30
[TIMING] ld_paragraph_set_ooxml build() took 294.87 ms
[TIMING] TextWidthCache (edit): hits=63361 misses=33
[TIMING] ld_paragraph_set_ooxml total took 386.42 ms
[TIMING] JOB_APPLY ld_paragraph_set_ooxml done
[TIMING] JOB_RENDER ld_render took 52.02 ms
[TIMING] cb_text_width: fast=33 fallback=0 no_face=0
```
