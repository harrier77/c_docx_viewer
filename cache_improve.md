# Ottimizzazione delle prestazioni di `d2dview` (cache e rebuild)

## Il problema
Quando si modifica un paragrafo in `d2dview`, `ld_paragraph_set_ooxml` ricostruisce l'intero documento (`DocxBuilder::build()` + `finalize` in `litehtml`). Su un documento di **200 pagine**, la versione iniziale richiedeva circa **~500 ms**.

Per mitigare l'impatto visivo, il viewer mantiene a schermo l'ultimo frame valido durante il ricalcolo (`g_render_busy` in `viewer_paint.c`), evitando il flash della pagina bianca.

---

## Stato attuale (branch `page_prova`, HEAD `5f7db3a`)

Le misure seguenti provengono da `build_timing.bat` → `d2dview_timing.exe`: `litedocx` compilato in configurazione `Timing` (`-O2`, `TIMING_LOGS=1`, log di debug disattivati — `litedocx/build_litedocx.ps1:58-62`) e viewer `-O2`. Il corpus è sempre il documento da 200 pagine: **3.000 tag** (`p=1001`, `span=1687`, `a=171`, `li=101`, `ul=30`, `br=8`, `body=1`, `html=1`) più ~63.000 nodi testo/spazio, che i timer non conteggiano (vedi sotto).

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
3. **Il `CSS font` raddoppia in edit** (4,52 → 8,39 ms) pur con la `TextWidthCache` quasi perfetta: il tempo non è più nella misura delle stringhe, ma nella risoluzione delle proprietà font.
4. **Il `3000 elements` del log è fuorviante**: `g_compute_styles_calls` viene incrementato solo in `html_tag::compute_styles` (`litehtml/src/html_tag.cpp:381-382`). I nodi `el_text`/`el_space` — ~63.392 (53.048 hit + 10.344 miss) — non vengono conteggiati ma sono visitati dalla ricorsione. I 55,83 ms di `compute_styles` in apertura sono quindi ~8,74 ms di `css_properties::compute` (che è tutto ciò che misura il `CSS timing`) e ~47 ms di percorso testo/`text_width`.
5. **Il blocco mancante di ~90 ms è la distruzione del vecchio documento**: a `litedocx_c.cpp:2574` (`h->container->doc = new_doc`) viene rilasciato l'ultimo riferimento al documento precedente; ~66k nodi DOM/render, liste e stringhe vengono deallocati dentro il timer `total` ma fuori da ogni sotto-timer. Durante il rebuild i due alberi coesistono (`h->doc` e `container->doc`): questo spiega il rallentamento ~1,6x delle fasi dominate dalle allocazioni (`build_body`, `create_render_item`, `init`).
6. **I ~20 ms di scarto tra timer `build()`** (294,87 esterno vs 275,09 interno) sono costruttore `DocxBuilder`, `QueryPerformanceFrequency` e i dump `dump_css_timing`/`dump_html_tag_timing` + `fprintf(stderr)`, esclusi dal timer interno: per quantità di lavoro dovrebbero essere ≪1 ms, quindi la causa più plausibile è I/O su console/scheduler. Restano da spiegare i 11,87/33,55 ms di `finalize` e i 6,89/14,62 ms di `build()` non coperti dai sotto-timer.
7. **Il `CSS font` in edit non è una regressione della cache testo**: ogni documento nuovo ha `m_fonts` vuoto (`document.cpp:527-537`) e la cache font del viewer è una lista lineare (`viewer_fonts.c:8-21`): i +3,9 ms sono risoluzione delle proprietà font, non `text_width`.
8. **Dopo ogni edit la cache testo viene svuotata**: `handle_edit_applied` (`maind2dv.c:208`) chiama `ld_invalidate_font_cache`; il `ld_render` successivo (`litedocx_c.cpp:1617-1620`) esegue `text_width_cache.clear()` anche se salta `clear_font_cache`/`rebuild_render_tree` perché `m_full_layout_done=false`. Il log con 63.361 hit è quindi solo il **primo edit dopo l'apertura**; gli edit successivi ripartono con cache fredda.
9. `ld_render` resta stabile a **~52 ms** in entrambi i percorsi: non è il collo di bottiglia del rebuild, ma diventa rilevante se l'obiettivo è scendere sotto i ~100 ms totali per edit.

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

## Serbatoio rimasto: percorso testo (`compute_styles`)

I ~63k nodi testo/spazio nascono in `litedocx/src/docx_builder.cpp:1273-1324` (`append_text` crea un `el_text` per parola e un `el_space` per ogni spazio singolo). Ogni nodo passa da `el_text::compute_styles` (`litehtml/src/el_text.cpp:28-104`) che arriva a `text_width` (riga 100-101), il quale consulta la `TextWidthCache` (`litedocx/src/litedocx_c.cpp:320-414,732-784`) e il callback `cb_text_width` (`viewer_includes/viewer_fonts.c:221-305`).

In apertura `open_impl` crea un **nuovo container** e quindi una cache vuota: i 10.344 miss sono il first-touch delle coppie `(parola, font)` uniche. Nel primo edit dopo l'apertura il container e la cache vengono riutilizzati (`reset_stats()` ma non `clear()`), per questo i miss crollano a 33 — ma il render successivo svuota la cache (punto 8 qui sopra), quindi gli edit seguenti ripartono freddi. I miss in apertura non possono arrivare a zero senza preriscaldare la cache; si possono però rendere più economici.

| Possibile intervento | Dove | Guadagno stimato | Rischio |
|---|---|---:|---|
| Non chiamare `text_width` per i puri spazi: larghezza U+0020/U+00A0 calcolata una volta per font | `el_text.cpp:100-101`, `el_space.cpp:10-14`, `docx_builder.cpp:1311`, `viewer_state.c:53-64` | 4–10 ms | Medio-basso |
| Inserimento in cache più economico: hash calcolato una volta, niente `e.text = text` su miss (chiave inline/SSO o offset) | `litedocx_c.cpp:331-372,397-409` | 3–7 ms | Medio |
| Evitare il cammino degli antenati per parola passando il CSS/font già calcolato ai figli testo | `html_tag.cpp:397-403`, `el_text.cpp:30-38,52-66,88-92` | 2–6 ms | Medio |
| Default CSS non allocanti (`static const` per i vettori) e hash font numerico invece di `std::string` | `css_properties.cpp:264-296,431,552-581`, `font_description.h:27-43`, `document.cpp:527-568` | 2–4 ms | Basso/medio |
| Togliere il `font_key_map.find` per chiamata (chiave stabile dentro l'handle) | `litedocx_c.cpp:675-679,732-736`, `viewer_state.c:53-64` | 1–3 ms | Basso/medio |
| Mettere sotto `#if TIMING_LOGS` i `chrono` interni e `_tag_counts` | `css_properties.cpp:14-18,23,115-119,302-316`, `html_tag.cpp:23,382,1677-1686` | < 1 ms | Basso |
| Preriscaldare la cache in apertura / condividerla a livello viewer | `litedocx_c.cpp:1434,1487`, `viewer_lifecycle.c` | 0 ms prima apertura, 10–25 ms su riaperture | Medio |
| Ridurre i nodi parola (blocchi di testo in `append_text`) | `docx_builder.cpp:1273-1324`, `line_box.cpp:200-225` | fino a 20–30 ms | **Alto** (`line_box` tratta ogni elemento come atomico) |

Nota: azzerare completamente il `CSS timing` darebbe al massimo 8,74 ms (16% dei 55,83 ms), quindi le azioni sul percorso testo contano più quelle sulle proprietà CSS.

---

## Serbatoio lato rebuild/edit

`create_render_item` (~0,74 µs/nodo in edit) e `init` (~0,41 µs/nodo) sono lineari sul numero di nodi (~66k, non 3k): il costo è fatto di `make_shared`, `std::list` e copie di stringhe per nodo, non di un algoritmo nascosto. In `init` i render item di blocco vengono però creati **due volte**: `render_item_block::init()` (`litehtml/src/render_block.cpp:124,177-186`) crea un nuovo contesto e copia la lista dei figli, sostituendo quello costruito da `create_render_item` — ~1.200 `make_shared` aggiuntivi e ~14.400 chiamate a `to_pixels()`.

| Possibile intervento | Dove | Guadagno stimato | Rischio |
|---|---|---:|---|
| Differire la distruzione del vecchio documento fuori dal critical path dell'edit (retire-list su `ld_doc`, liberata in un job sul worker) | `litedocx_c.cpp:2573-2574`, `viewer_jobs.c:65-73,286-288` | **80–90 ms/edit (~23% del totale)** | Medio (mantenere affinità worker per COM/D2D) |
| Evitare la doppia creazione dei render item di blocco in `init` scegliendo il contesto già in `create_render_item` | `render_block.cpp:124,177-186`, `render_item.cpp:8-28` | 5–8 ms | Medio |
| Indicizzare i selettori CSS per tag/classe invece di scandirli tutti per ogni elemento | `html_tag.cpp:211-311`, `master_css.h` (~67 selettori) | 5–8 ms | Medio (cascade/specificità) |
| `m_renders` da `std::list<weak_ptr>` a vettore/singolo slot inline | `element.h:35`, `element.cpp:192-215` | 3–6 ms | Medio |
| `XmlNode::attr_view()` (`string_view`) nei parse che usano il valore solo per `atoi`/confronti | `xml_dom.h:26-30`, `docx_style.cpp:217-374` | 3–6 ms su `build_body` | Basso/medio |
| Sostituire `std::ostringstream` con `std::string` + `to_chars` nelle chiavi di `StyleCache` | `docx_builder.h:142-167` | 2–4 ms | Basso |
| Bufferizzare i `TLOG` in modalità timing (una sola flush a fine build) | `debug_log.h:34-38`, `docx_builder.cpp:277-332` | 1–5 ms | Basso |
| Evitare il `vector` in `apply_style_chain` (buffer stack) | `docx_style.cpp:89-116` | 1–2 ms | Basso |
| Passare le mappe di attributi per `move` in `make_element` | `docx_builder.cpp:1348` | 1–2 ms | Basso |
| `reserve()` su `blocks`/`lists`/`StyleCache`; riutilizzo `StyleCache` tra edit | `docx_builder.cpp:393,443`, `docx_builder.h:233-238`, `docx_builder.cpp:275` | < 1 ms | Basso (ultimo non conveniente) |

---

## Prossime priorità (aggiornate, in ordine di impatto)

1. **Distruzione del vecchio documento**: misurare con un TLOG dedicato attorno a `litedocx_c.cpp:2573-2574`, poi differirla fuori dal critical path dell'edit. È il singolo intervento più redditizio (~80–90 ms, 23% del totale) e spiega perché l'edit è ~1,6x più lento nelle fasi di allocazione.
2. **Invalidazione della cache testo dopo ogni edit**: verificare con un secondo edit consecutivo (il log attuale copre solo il primo dopo l'apertura); se la cache riparte fredda a causa di `ld_invalidate_font_cache` (`maind2dv.c:208` → `litedocx_c.cpp:1619`), evitare l'invalidazione negli edit che cambiano solo testo.
3. **Ridurre i nodi `el_text` per parola**: 15–25 ms su `build_body`, 10–15 su `create_render_item`, 5–8 su `init`, oltre al layout. Rischio alto: il word-wrapping si appoggia al nodo-parola (`render_inline_context.cpp:345-351`).
4. **Eliminare la doppia creazione dei render item di blocco in `init`**: 5–8 ms, rischio medio.
5. **Indicizzare i selettori CSS**: 5–8 ms su `apply_master_css` + `apply_stylesheets`, rischio medio per l'ordine di cascata.
6. **Interventi a basso rischio**: bufferizzare i TLOG (1–5 ms, spiega anche parte del delta dei 20 ms), `attr_view` (3–6 ms), `to_chars` (2–4 ms), `move` delle mappe (1–2 ms); strumentare i ~33 ms ancora non coperti in `finalize`.
7. **Rebuild incrementale per singolo paragrafo** *(strategico, alto rischio)*: da valutare **solo** dopo aver chiuso i punti 1–6 se la latenza fosse ancora percepibile. Il tentativo precedente (branch `lazy-cache`, eliminato) fallì per cache vuota su hit-test/caret e incoerenze DOM/render tree. Se ripreso, mantenere **sempre** il fallback al full-rebuild.

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
