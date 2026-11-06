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
F1 — Non bloccare la UI (obbligatoria, basso rischio)  ✅ COMPLETATO (rivisto 2026-11-06)
F2 — Costruzione/layout incrementale a pagine (la vera soluzione)
F3 — Ridurre il costo per elemento (3-5x CPU)
- CSS inline → classi/cache; style object riusato  ✅ COMPLETATO (F3.1)
- XML DOM: string_view / prefix compare, no std::map per nodo  ✅ COMPLETATO (F3.3)
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


### AGGIORNAMENTO 2026-11-05 — STATO FIX APPLICATI IN QUESTA SESSIONE (verificato 2026-11-06)


✅ F1 — UI NON BLOCCANTE (docxview_new.c)
   - v1 (2026-11-05): thread worker LoadThreadProc per ld_open_file + ld_render,
     WM_APP_DOC_READY, titolo "Caricamento..."
   - v2 (2026-11-06): riscritto con worker persistente + coda job; render
     asincrono anche al resize, distruzione differita sul worker, ID di
     generazione. Vedi "AGGIORNAMENTO 2026-11-06" sotto.

✅ F3.1 — CSS CLASSI + CACHE (litedocx: docx_builder.h/.cpp)
   - StyleCache: deduplica ParaProps/RunProps identici → classi CSS "pN"/"sN"
   - Elimina 200k+ style inline duplicati → litehtml parsea CSS una volta per classe
   - get_or_create_list_class per ul/ol/li/p liste
   - add_dynamic_rule per list-style-type per-ol
   - Fix bug: emplace restituisce iterator, non usare find(cls) su valore spostato

✅ F3.3 — XML DOM string_view (COMPLETATO 2026-11-06)
   - xml_dom.h: name → string_view, attrs → map<string_view, string, less<>>
   - xml_dom.cpp: parser usa string_view per tag/attr keys (zero alloc), xml_local_name → string_view
   - xml_dom.h: metodi XmlNode (has, attr, attr_local, child, children_named, is) accettano string_view
   - Chiavi attributi: niente allocazioni (string_view nel buffer XML originale)
   - Valori attributi: owned string (decodifica entità XML)
   - Compatibilità: chiamanti invariati (string literal → string_view implicito)
   - Impatto stimato: −1.5÷2.5 s CPU su 229 pagine, −80% memoria parsing, abilitante per F2

✅ CRASH FIX — Bug in StyleCache::get_*_class
   - m_map.emplace(std::move(key), cls) sposta key → find(cls) cercava valore come chiave
   - Fix: usa iterator restituito da emplace (result.first->second)

✅ F3.4 — append_text reserve + nbsp + no substr (docx_builder.cpp)
   - m_nbsp_buf: buffer membro riutilizzato per NBSP (reserve condizionale) ✅
   - m_word_buf: buffer membro riutilizzato per parole (assign + reserve condizionale) ✅
   - NBSP: costruito in-place nel buffer (no string temporanea per run spazi) ✅
   - Parole: substr() ELIMINATO → m_word_buf.assign(text.data() + start, len) ✅
   - Zero allocazioni temporanee per parola (riuso capacity dopo prima parola lunga)
   - Impatto stimato: −0.1÷0.3 s CPU, −1–3 MB churn su 229 pagine

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
   - Applicato in 3 punti: worker (load/render), WM_SIZE→request_render, WM_PAINT


### AGGIORNAMENTO 2026-11-06 — F1 RISCRITTO E VALIDATO A RUNTIME

✅ F1 — UI realmente non bloccante (docxview_new.c)
   - Thread worker persistente (WorkerProc) + coda FIFO di job protetta da
     CRITICAL_SECTION: JOB_LOAD, JOB_RENDER, JOB_DESTROY, JOB_QUIT.
   - ld_open_file / ld_render / ld_destroy avvengono sempre sul worker:
     rispettata l'affinità di thread di COM, WIC e degli HDC di misura.
   - WM_SIZE non chiama più ld_render sul thread UI: richiede il render al
     worker (request_render). Le larghezze richieste durante un render in corso
     vengono coalescate in g_render_pending (ultima vittoria).
   - Durante il render (g_render_busy) WM_PAINT non chiama ld_draw: si evita
     l'accesso concorrente al DOM. Al termine WM_APP_RENDER_DONE aggiorna
     altezza/scrollbar e invalida la finestra.
   - Apertura di un secondo file senza attese: rimosso il
     WaitForSingleObject(..., 2000) sul thread UI; il path viaggia dentro il
     job, niente più buffer globale condiviso.
   - ID di generazione (g_load_gen) su WM_APP_DOC_READY: i risultati di
     richieste superate vengono distrutti sul worker e non sovrascrivono il
     documento corrente.
   - Chiusura senza attese: WM_DESTROY accoda JOB_DESTROY + JOB_QUIT; nessun
     join sul thread UI. L'uscita del processo recupera eventuali job in corso.
   - HDC di misura: cb_create_font / cb_text_width usano un GetDC(NULL) locale
     (rimosso g_tmpdc globale creato dal thread UI e usato dal worker).
   - Altezza layout cache-ata in g_doc_h (letta dal thread UI) al posto di
     ld_height(g_doc) in paint/scroll.

✅ F3.5 — Cache UTF-8↔UTF-16 resa thread-local (litedocx_c.cpp)
   - utf_cache() usa static thread_local: eliminata la data race tra il render
     sul worker e il draw sul thread UI sulla cache globale.
   - TextWidthCache resta per-documento (ld_container) e viene usata dal solo
     worker.

Validazione runtime (2026-11-06): build e programma verificati dall'utente,
UI responsiva durante il caricamento. Resta non coperto il criterio
"primo paint ≤ 1-2 s": il documento compare ancora solo dopo il layout
completo, che richiede F2 (layout incrementale/paginato).


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

Piano immediato: test runtime completato (2026-11-06, F1 ok). Prossimo passo:
valutare F2 se il primo paint (layout completo) non è accettabile.

### A. Virtualizzazione DOM per scrolling (da valutare dopo test)
```cpp
// ld_open_file: builda SOLO DOM struttura (p, table, div) - niente el_text/el_space
// ld_render_page(page_idx): crea el_text/el_space solo per quella pagina + buffer
// Scroll: distrugge pagine lontane, crea nuove
```
Pro: DOM piccolo (solo pagine visibili) → walk veloce
Contro: Riscrittura significativa docx_builder


### Tabella sommaria fix (aggiornata 2026-11-06)

| Fix marcato ✅ in diagnosi.md | In codice? | Note |
|---|---|---|
| F1 — UI non bloccante | ✅ Sì (rivisto 2026-11-06) | Worker persistente + coda job; niente render/attese sul thread UI; distruzione sul worker; ID di generazione. Primo paint ancora legato al layout completo (F2) |
| F3.1 — StyleCache CSS classi | ✅ Sì | Implementato e usato |
| Crash fix `StyleCache::get_*_class` | ✅ Sì | `emplace(...).first->second` in tutti e 3 i metodi |
| F3.3 — XML DOM string_view | ✅ Sì | name/attr keys = string_view (zero alloc), parser zero-copy, compatibile chiamanti |
| F3.4 — append_text reserve/no substr | ✅ Sì | m_nbsp_buf/m_word_buf membri riutilizzati, zero substr, zero alloc temporanee |
| F3.5 — Cache UTF-8↔UTF-16 e text width | ✅ Sì | TextWidthCache per documento; Utf8ToUtf16Cache ora `thread_local` (race risolta) |
| Fix margini pagina | ✅ Sì | Applicato nei 3 punti dichiarati |