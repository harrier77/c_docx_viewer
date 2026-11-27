# Ottimizzazione delle prestazioni di `d2dview` (cache e rebuild)

## Il problema
Quando si modifica un paragrafo in `d2dview`, `ld_paragraph_set_ooxml` ricostruisce l'intero documento (`DocxBuilder::build()` + `finalize` in `litehtml`). Su un documento di **200 pagine**, questa operazione richiede circa **400–460 ms**.

Per mitigare l'impatto visivo, il viewer mantiene a schermo l'ultimo frame valido durante il ricalcolo (`g_render_busy` in `viewer_paint.c`), evitando il flash della pagina bianca.

---

## Dove va davvero il tempo (documento da 200 pagine)

Tempi misurati con `std::chrono` (al netto dei log `TLOG`, vedi nota in fondo):

| Fase | Tempo | Peso | Note |
|---|---:|---:|---|
| **`finalize compute_styles`** | **297.90 ms** | **~70%** | **Collo di bottiglia principale** |
| `DocxBuilder build_body` | 50.82 ms | ~12% | Costruzione albero DOM |
| `finalize create_render_item` | 30.81 ms | ~7% | Creazione render tree |
| `finalize init` | 16.03 ms | ~4% | Inizializzazione render tree |
| `finalize parse_attributes` | 13.25 ms | ~3% | Parsing attributi |
| `finalize apply_stylesheets` | 10.39 ms | ~2% | Applicazione CSS |
| `DocxBuilder xml_parse` | 4.31 ms | ~1% | Parsing XML |
| Altre fasi (`css`, `fix_tables`, ecc.) | ~0 ms | — | Trascurabili su questo corpus (CSS fino a ~3.5 ms su doc molto stilati) |

*(Il rendering a schermo successivo, `ld_render`, impiega **~50 ms** e non è il collo di bottiglia.)*

---

## La scoperta chiave su `compute_styles` (~298 ms)

A prima vista sembrava che il tempo fosse perso sui **~3.000 elementi** HTML (`span=1687`, `p=1001`, `a=171`, `li=101`, `ul=30`, `br=8`, `html/body=2`). In realtà:

- Il lavoro proprio di quei 3.000 nodi `html_tag` (attributi, stili inline, variabili, `css_properties::compute`, loop figli) somma appena **~24 ms** (`~23.1 ms` + `~1 ms` di loop).
- I restanti **~274 ms (92%)** stanno nei **~63.392 nodi foglia di testo e spazio** (`el_text` e `el_space`, creati parola per parola da `litehtml`), che portano il totale dei figli visitati a **66.391**.

`g_compute_styles_calls` contava solo `html_tag::compute_styles` e non `el_text::compute_styles`. Per ciascuna di quelle ~63.392 parole/spazi, `el_text::compute_styles()` esegue ogni volta:
1. Un lock di `std::weak_ptr` (`parent()`) per copiare font, metriche, `line_height`, `white_space`, `text_transform`.
2. Un secondo lock di `std::weak_ptr` (`parent()`) seguito da un ciclo `while` lungo gli antenati `display_inline` per cercare `position: relative`.

---

## Cosa è già stato fatto (branch `page_prova`)

- **UX "last frame"** (`499defe`): durante il rebuild il viewer continua a mostrare l'ultimo frame valido invece di una pagina vuota.
- **Strumentazione timing** (`build_timing.bat` -> `d2dview_timing.exe`): build console dedicata al profiling.
- **Span merging** (`3b1d549`, committato): in `DocxBuilder::process_run`, i run di testo semplice adiacenti con lo stesso stile vengono fusi in un unico `<span>`. Corretto e sicuro, ma su 200 pagine riduce gli span solo da 1.703 a 1.687 (-0.9%), senza impatto misurabile sui tempi.
- **Pulizia timer interni in `html_tag`** (`17e206b`, committato): rimossi i `std::chrono::now()` per-statement dentro `html_tag::compute_styles`.
- **Logging Release esplicito** (`be350e5`, committato): aggiunto `-DENABLE_FILE_LOGGING=0` in `build_d2dview.bat`.

---

## Piano d'azione (in ordine di priorità)

1. **Pulire `TLOG` prima delle prossime misure** (`debug_log.h`)
   - Rimuovere `fflush(stderr)` da `TLOG` (o disattivare `TIMING_LOGS` quando si misurano i totali): oggi ogni `TLOG` forza un flush sincrono che gonfia i timer esterni di decine di ms (es. `finalize` segnato a 390 ms invece di ~370 ms reali, e su documenti piccoli 30 ms invece di ~9 ms).
   - Verificare anche i flag di ottimizzazione con cui `build_litedocx.bat Timing` compila `litehtml` (eventuali build Debug amplificano molto `shared_ptr` e chiamate virtuali).

2. **Ottimizzare `el_text::compute_styles`** *(impatto atteso: ALTO — target dei ~274 ms)*
   - Aggiungere prima un singolo contatore + timer cumulativo (non per singolo nodo) su `el_text::compute_styles` per confermare la cifra.
   - Eliminare il doppio `parent()` (`weak_ptr::lock()`) e la risalita degli antenati `inline` su ogni parola quando non c'è `position: relative` (es. usando puntatore raw dove sicuro o propagando il flag dall'elemento padre).
   - File: `litehtml/src/el_text.cpp`, `litehtml/include/litehtml/el_text.h`, `el_space.h`.

3. **Profilare e snellire `create_render_item` (30.8 ms) e `init` (16.0 ms)** *(impatto atteso: MEDIO-BASSO — ~47 ms totali)*
   - File: `litehtml/src/render_item.cpp`, `litehtml/src/element.cpp`.

4. **Profilare `parse_attributes` (13.25 ms)** *(impatto atteso: BASSO-MEDIO)*
   - File: `litehtml/src/element.cpp`.

5. **Cache dei CSS parsati (`master`/`user`)** *(impatto atteso: BASSO — 0 ms qui, fino a ~3.5 ms su doc molto stilati)*
   - Solo dopo i punti 1–4. File: `litehtml/src/document.cpp`, `litedocx/src/docx_builder.cpp`.

6. **Rebuild incrementale per singolo paragrafo** *(strategico, alto rischio)*
   - Da valutare **solo** se, dopo i punti 1–5, la latenza fosse ancora percepibile.
   - Il tentativo precedente (branch `lazy-cache`, eliminato) è fallito per cache vuota/non popolata su hit-test/caret e incoerenze tra DOM e render tree.
   - Se ripreso, va fatto a piccoli passi verificabili mantenendo **sempre** il fallback al full-rebuild per i casi complessi (elenchi, tabelle, numerazioni).

---

## Piste scartate (già verificate)

- **Lazy layout cache senza cablare i consumer**: ha causato crash perché hit-test e caret leggevano `g_layout_cache` vuota (e risparmiava comunque solo ~140 ms secondari).
- **Memoization di `get_property`**: misurata a 249.522 chiamate = **3.90 ms totali** (~0.000016 ms/chiamata). Scartata.
- **Ottimizzazione risoluzione proprietà CSS**: `css_properties::compute` pesa solo ~24 ms totali.
- **Span merging più aggressivo**: pochi run contigui con stile identico nel corpus reale (-0.9% span).
- **Batching del draw su testo giustificato**: `< 20 ms` su `ld_render`, alto rischio di regressioni visive.

---

## Nota metodologica sui profili precedenti
- **Effetto `TLOG`/`fflush`**: i totali esterni (`build()` TOTAL 460.8 ms, `finalize` 390.3 ms) includono i syscall di `fflush(stderr)` delle stampe interne; per confrontare i singoli passi fanno fede i timer interni `std::chrono`.
- **Base stabile di riferimento**: `master` (`20d4bda`), su cui si innesta il branch corrente `page_prova`.
