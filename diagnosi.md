Diagnosi freeze docxview_new su DOCX da 229 pagine — conclusioni provvisorie (2026-11-05)

1. Sintomo e misure
Table with wall/CPU/WS:
- docxview.exe (vecchio): 1.6 s al primo paint, CPU 1.14 s, WS 365 MB
- docxview_new.exe: 32.4 s, CPU 5.67 s, WS 353.6 MB
Note: il vecchio costruisce comunque tutto il documento (365 MB) → il problema non è "costruire tutto", ma quanto costa e quando.

2. Cosa NON è la causa (con prove)
- Tabelle: no_tables 12s+ non responsive; one_table; tables_short_text
- Immagini: images_only 2s responsive 24MB; no_images come originale
- Testo: no_text 27s; no_text_no_tables 18.2s/2.31s CPU
- GDI/misura testo: microbench (0.1µs extent, 0.5µs select+extent, 11µs create+realize)
- Allocazioni generiche: 2M malloc/free in 41ms (0.02µs/op)
- Deadlock: completa; a 45s main thread in GetMessageA
→ Non è un singolo contenuto "velenoso" né un deadlock.

3. Cosa sappiamo
3.1 Il lavoro è tutto sincrono sul thread UI
docxview_new.c load_docx_file() chiama ld_open_file + ld_render dal wnd_proc; nessun pump dei messaggi → 33s di freeze; 6 thread, nessuno lavora.
3.2 Costo CPU 5x il vecchio
5.67 vs 1.14 s.
3.3 Gap wall/CPU 5.7x (vecchio 1.4x)
87% del tempo il main thread è bloccato. Stack: ntdll ZwWaitForSingleObject ← KernelBase WaitForSingleObjectEx ← TmUmEvt64.dll ← tmmon64.dll (Trend Micro User Mode Hook). Non riproducibile nei microbenchmark → dipende da una specifica API usata dal path litedocx. Da identificare.
3.4 Il costo scala con paragrafi/elementi, non col testo
1.3 ms CPU/paragrafo both variants.
Elementi: >200k text nodes (648,877 chars, 5,241 w:t, split per parola in append_text:1074).
3.5 Il modello per-parola è richiesto da litehtml
line_box::can_hold / new_width: ogni text part è atomico; litehtml stesso (document.cpp:371-387 split_text) fa lo stesso. Quindi unire le parole in un solo el_text rompe il wrapping (w:t fino a 2,779 char).
3.6 Bug tabelle
el_table::appendChild accetta solo tbody/thead/tfoot/caption → tr rifiutato → intere tabelle costruite e buttate. Non causa del freeze (il costo di build c'è già), ma va corretto; correggerlo aggiunge layout/render.
3.7 Memoria
Nessun leak osservato: WS si stabilizza (353 MB) e resta; 267→353 MB tra run fredde/calde. Il consumo è churn di allocazioni, non leak. Ancora da verificare il ciclo ld_destroy/riapertura.

4. Ipotesi aperte (da misurare domani)
- quale API scatena il wait TM (MultiByteToWideChar? GDI? altro)
- split build vs layout (build ~30s of 33s from progress data)
- costo per fase in litehtml: apply_stylesheet/compute_styles/render
- quanto del 5x CPU è XML DOM + CSS inline vs litehtml

5. Proposte di fix (ordinate)
F0 — Misure (mezz'ora) 
F1 — Non bloccare la UI (obbligatoria, basso rischio)  ✅ COMPLETATO
F2 — Costruzione/layout incrementale a pagine (la vera soluzione)
F3 — Ridurre il costo per elemento (3-5x CPU)
- CSS inline → classi/cache; style object riusato  ✅ COMPLETATO (F3.1)
- XML DOM: string_view / prefix compare, no std::map per nodo  ✅ COMPLETATO (F3.3 parziale)
- append_text: evitare substr + nbsp temporanei, riservare  ✅ COMPLETATO (F3.4)
- u8↔w16 cache per misura  ✅ COMPLETATO (F3.5)
F4 — Tabelle (correttezza)  ⬜ DA FARE
F5 — Pulizie viewer (SelectObject restore, DPI, errori) — fuori scope freeze

For each: impatto atteso, sforzo, rischio.

6. Criteri di accettazione
- primo paint ≤ 1-2 s su quel file
- UI mai bloccata > 100 ms
- scroll
- WS ≤ 150 MB target (opzionale)
- nessuna regressione: testo, tabelle, immagini, link

7. File di riferimento (paths + lines)
8. Comandi/esperimenti riproducibili (varianti in %TEMP%\docxdiag, script)
9. Nota su Trend Micro (esclusione per conferma, ma non è una soluzione)

Piano:
1. Sbloccare la UI: spostare ld_open_file/ld_render fuori dal thread UI o spezzarli in step con pump messaggi.
2. Layout incrementale/paginato: evitare di costruire tutto il documento prima del primo paint.
3. Ridurre churn e allocazioni: cache UTF, reuse degli style object, meno stringhe temporanee e meno split.
4. Correggere le tabelle: sistemare appendChild e il flusso table/row/cell.
5. Pulizia viewer: restore GDI objects, gestione errori, DPI, scroll/render invalidation.


### AGGIORNAMENTO 2026-11-05 — STATO FIX APPLICATI IN QUESTA SESSIONE


✅ F1 — UI NON BLOCCANTE (docxview_new.c)
   - Aggiunto thread worker LoadThreadProc per ld_open_file + ld_render
   - WM_APP_DOC_READY per notificare completamento al thread UI
   - UI responsiva durante caricamento (titolo "Caricamento...")
   - Cleanup corretto in WM_DESTROY (g_load_cancel + WaitForSingleObject)

✅ F3.1 — CSS CLASSI + CACHE (litedocx: docx_builder.h/.cpp)
   - StyleCache: deduplica ParaProps/RunProps identici → classi CSS "pN"/"sN"
   - Elimina 200k+ style inline duplicati → litehtml parsea CSS una volta per classe
   - get_or_create_list_class per ul/ol/li/p liste
   - add_dynamic_rule per list-style-type per-ol
   - Fix bug: emplace restituisce iterator, non usare find(cls) su valore spostato

✅ F3.3 (parziale) — XML DOM string_view (litedocx: xml_dom.h/.cpp)
   - xml_local_name accetta string_view, evita copy per local-name
   - Metodi XmlNode::has/attr/attr_local/child/children_named/is usano string_view param
   - Parser usa string_view per name/attr parsing dove possibile
   - Nota: map<string,string> resta (C++11 non ha heterogeneous lookup)

✅ CRASH FIX — Bug in StyleCache::get_*_class
   - m_map.emplace(std::move(key), cls) sposta key → find(cls) cercava valore come chiave
   - Fix: usa iterator restituito da emplace (result.first->second)

✅ F3.4 — append_text reserve + evita substr/nbsp temporanei (docx_builder.cpp)
   - std::string nbsp_buf con reserve(text.size() * 3) riutilizzato nel loop
   - Parole: evitato substr() → appendChild(el_text(text.c_str() + start))
   - NBSP: costruito in-place nel buffer (no string temporanea per run spazi)
   - Impatto: riduce allocazioni per 200k+ text nodes

✅ F3.5 — Cache UTF-8↔UTF-16 per misura testo GDI (litedocx_c.cpp)
   - Utf8ToUtf16Cache LRU (256 entry) per u8_to_w16 → evita MultiByteToWideChar ripetuti
   - TextWidthCache LRU (1024 entry) in ld_container → key = testo + font_handle
   - text_width() controlla cache prima di GetTextExtentPoint32W
   - Pulizia automatica in ld_destroy()
   - Impatto: riduce chiamate GDI ripetute (sospetto trigger Trend Micro wait)

✅ FIX MARGINI PAGINA (docxview_new.c)
   - Margini simmetrici: 64px sinistra/destra, 60px alto/basso
   - ld_render() chiamato con content_w = g_page_w - 128 (non page width)
   - ld_draw() usa clip rect area contenuto (esclude margini)
   - Applicato in 3 punti: LoadThreadProc, WM_SIZE, WM_PAINT


### COSA MANCA ANCORA DA FARE (PRIORITÀ)


🟡 F2 — Layout incrementale/paginato (litedocx_c.cpp + docx_builder.h/.cpp)
   - ld_render_range(page_idx, viewport_h) → layout solo pagina visibile + 1-2 successive
   - ld_open_file: solo DOM build, niente layout
   - Scroll → render pagina successiva on-demand + cache LRU
   - Obiettivo: primo paint < 1s

🟡 F4 — Fix tabelle (docx_builder.cpp)
   - el_table::appendChild accetti tr (ora solo tbody/thead/tfoot/caption)
   - Flusso table/row/cell corretto

🟢 F3.2 — Style object reuse (docx_builder.cpp)
   - Condividi ParaProps/RunProps risolti identici (pointer invece di copy)
   - Riduce merge() ripetuti

🟢 F5 — Pulizie viewer (docxview_new.c)
   - SelectObject restore, DPI awareness, error handling


### STIMA TEMPI RESIDUI

- F2: ~1-2 giorni (richiede API nuove in litedocx, rischio medio)
- F4: ~4-6 ore (correttezza, non performance)
- F3.2 + F5: ~1 giorno (pulizia)

Piano immediato: testare scroll con F3.4+F3.5+Lazy Measurement → decidere se F2 serve ancora.

### A. Virtualizzazione DOM per scrolling (da valutare dopo test)
```cpp
// ld_open_file: builda SOLO DOM struttura (p, table, div) - niente el_text/el_space
// ld_render_page(page_idx): crea el_text/el_space solo per quella pagina + buffer
// Scroll: distrugge pagine lontane, crea nuove
```
Pro: DOM piccolo (solo pagine visibili) → walk veloce
Contro: Riscrittura significativa docx_builder