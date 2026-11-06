# Piano di Implementazione Incrementale — Layout Paginato

**Obiettivo**: First paint < 1s, scroll fluido, nessun regressione.
**Metodo**: 6 fasi, ognuna **compilabile, testabile, reversibile**. Si parte dal branch `litedocx` (codice attuale funzionante).

---

## ⚠️ Regole d'oro
- **Una fase = un commit atomico** (o al max 2-3 commit correlati)
- **Test manuale obbligatorio** prima di passare alla fase successiva
- **Niente refactoring "di contorno"**: solo ciò che serve alla fase
- Se una fase rompe qualcosa → `git checkout -- .` e si riparte

---

## FASE 0 — Baseline & Safety Net (15 min)
**Obiettivo**: Avere un punto di ritorno garantito e script di test rapido.

| Azione | Verifica |
|--------|----------|
| `git checkout -b feat/incremental-layout litedocx` | Branch creato |
| `build_viewer_new.bat` → `docxview_new.exe` parte | ✅ Finestra apre, menu File→Apri funziona |
| Apri `test.docx` (qualunque file ≥ 3 pagine) | ✅ Render completo, scroll funziona |
| Crea `test_smoke.bat`: `docxview_new.exe test.docx && echo OK` | ✅ Exit code 0 |

**Deliverable**: Tag `baseline-working` + script `test_smoke.bat`.

---

## FASE 1 — API Header Only (solo `litedocx_c.h`) (30 min)
**Obiettivo**: Dichiarare le nuove funzioni **senza implementarle** (stub che ritornano 0/NULL). Il codice esistente **non deve cambiare**.

### Modifiche
```c
// litedocx_c.h — AGGIUNTA in fondo, prima di #endif
int ld_render_range(void* doc, int page_idx, int viewport_h, int max_width);
int ld_get_page_count(void* doc);
int ld_get_page_height(void* doc, int page_idx);
void ld_clear_page_cache(void* doc);
```

### Test
- `build_viewer_new.bat` → **compila senza warning/errori**
- `test_smoke.bat` → **ancora OK** (nessuna chiamata nuova)

**Commit**: `feat: add paginated API declarations (stubs)`

---

## FASE 2 — Stato Paginazione in `ld_doc` (solo `litedocx_c.cpp`) (45 min)
**Obiettivo**: Aggiungere i campi di paginazione alla struct interna `ld_doc` e inizializzarli in `open_impl`. **Nessuna logica di layout ancora**.

### Modifiche
```cpp
// In namespace anonimo, struct ld_doc
struct ld_doc {
    // ... campi esistenti ...
    int            m_page_height_px = 0;       // Altezza pagina CSS (da DocxBuilder)
    std::vector<int> m_page_heights;           // Altezze pagine calcolate (lazy)
    std::vector<int> m_page_y_offsets;         // Offset Y di ogni pagina
    int            m_last_rendered_page = -1;  // Ultima pagina per cui abbiamo fatto layout
};
```

In `open_impl` (dopo `builder.build()`):
```cpp
h->m_page_height_px = builder.page_height();  // Già disponibile!
```

### Test
- Compila → **zero warning**
- `test_smoke.bat` → **OK** (struttura dati passiva, nessun effetto collaterale)

**Commit**: `feat: add pagination state to ld_doc`

---

## FASE 3 — Page-Break Div Identificabili (solo `docx_builder.cpp`) (30 min)
**Obiettivo**: Far sì che i `<div>` inseriti da `add_page_break` abbiano un attributo `data-page-break="N"` per trovarli dopo il render.

### Modifica minima in `DocxBuilder::add_page_break`
```cpp
void DocxBuilder::add_page_break(const litehtml::element::ptr& parent)
{
    int content_h = m_page_h_px - twip_to_px(m_model.section.margin_t_tw) -
                    twip_to_px(m_model.section.margin_b_tw);
    if(content_h < 100) content_h = 600;
    char buf[128];
    // AGGIUNTA: attributo data-page-break
    std::snprintf(buf, sizeof(buf), "height:%dpx;", content_h);
    auto div = make_element("div", buf, "data-page-break", std::to_string(m_page_break_count++));
    parent->appendChild(div);
}
```
*Aggiungi `int m_page_break_count = 0;` in `DocxBuilder`.*

### Test
- Compila
- Apri un DOCX multipagina → **Ispeziona DOM** (aggiungi `printf` temporaneo in `ld_render` per stampare `data-page-break` degli elementi) → **Vedi pagine 0,1,2...**

**Commit**: `feat: tag page-break divs with data-page-break`

---

## FASE 4 — `ld_get_page_count` + `ld_get_page_height` (solo `litedocx_c.cpp`) (60 min)
**Obiettivo**: Implementare le funzioni "read-only" che **non fanno layout**, calcolano le pagine analizzando il render tree **già esistente** (dopo una `ld_render` completa).

### Logica (pseudocodice)
```cpp
int ld_get_page_count(void* doc) {
    auto* h = static_cast<ld_doc*>(doc);
    if (!h->doc) return 0;
    // Se non abbiamo mai fatto render completo, forza uno (compatibilità)
    if (h->m_page_heights.empty()) {
        ensure_full_layout(h);  // Chiama doc->render() + scandisci page-break divs
    }
    return (int)h->m_page_heights.size();
}

int ld_get_page_height(void* doc, int page_idx) {
    auto* h = static_cast<ld_doc*>(doc);
    if (page_idx < 0 || page_idx >= (int)h->m_page_heights.size()) return 0;
    return h->m_page_heights[page_idx];
}
```

`ensure_full_layout(h)`:
1. `h->doc->render(max_width)` (usa `h->container->viewport_w`)
2. Scansiona `h->doc->root()->select_all("[data-page-break]")`
3. Per ogni div: `pos = el->get_placement();` → `m_page_y_offsets.push_back(pos.y); m_page_heights.push_back(pos.height);`
4. Ultima pagina: altezza = `doc->height() - last_offset`

### Test
- Aggiungi in `docxview_new.c` temporaneo (dopo `apply_loaded_doc`):
  ```c
  int pages = ld_get_page_count(g_doc);
  int h0 = ld_get_page_height(g_doc, 0);
  log_msg("[TEST] pages=%d page0_h=%d\n", pages, h0);
  ```
- Compila + apri `test.docx` → **Log mostra conteggio corretto**
- Rimuovi il test temporaneo

**Commit**: `feat: implement ld_get_page_count / ld_get_page_height`

---

## FASE 5 — `ld_render_range` Core (solo `litedocx_c.cpp`) (90 min)
**Obiettivo**: Layout **solo della pagina richiesta + 1 lookahead**. Litehtml non supporta layout parziale nativo → strategia: **render completo una volta, poi cache delle geometrie**. La prima chiamata a `ld_render_range(0, ...)` fa il render completo ma **memorizza solo le pagine 0 e 1**.

### Algoritmo
```cpp
int ld_render_range(void* doc, int page_idx, int viewport_h, int max_width) {
    auto* h = static_cast<ld_doc*>(doc);
    if (!h->doc) return 0;

    // 1. Se non abbiamo MAI fatto render → render completo + estrai TUTTE le pagine
    if (h->m_last_rendered_page == -1) {
        int w = h->doc->render(max_width);
        extract_all_pages(h);  // Popola m_page_heights, m_page_y_offsets
        h->m_last_rendered_page = (int)h->m_page_heights.size() - 1;
        return w;
    }

    // 2. Pagina già calcolata → no-op veloce
    if (page_idx <= h->m_last_rendered_page) {
        return ld_width(doc);  // Già noto
    }

    // 3. Pagina nuova → in questa architettura non succede (extract_all_pages fa tutto)
    //    Ma lasciamo l'hook per futuro lazy reale
    return ld_width(doc);
}
```

**Nota**: Per ora `ld_render_range` è **compatibilità** — fa render completo alla prima chiamata (pagina 0) e poi risponde O(1). Il vero "incrementale" verrà in Fase 7 lato viewer.

### Test
- In `docxview_new.c`: sostituisci `ld_render(g_doc, cw)` con `ld_render_range(g_doc, 0, g_ch, cw)` in `start_async_load` (worker)
- Compila + apri `test.docx` → **First paint visivo identico a prima**
- Scroll → **Funziona** (perché `ld_draw` usa clip rect)

**Commit**: `feat: implement ld_render_range (full-render-on-first-call)`

---

## FASE 6 — Viewer: First Paint = Solo Pagina 0 (solo `docxview_new.c`) (60 min)
**Obiettivo**: Il viewer **non chiama più `ld_render` full** all'avvio. Chiama `ld_render_range(doc, 0, viewport_h, content_w)` → **solo pagina 0 layoutata**. L'altezza totale `g_doc_h` viene calcolata da `ld_get_page_count * page_height` (stima) o somma pagine note.

### Modifiche chiave
1. **`start_async_load`**: Dopo `ld_open_file` → **NON** chiamare `ld_render`. Invece:
   ```c
   // Prima chiamata: layout pagina 0
   ld_render_range(doc, 0, g_ch, cw);
   int page_h = ld_get_page_height(doc, 0);
   int est_pages = ld_get_page_count(doc);  // Potrebbe essere 0 se non c'è page-break
   g_doc_h = est_pages > 0 ? page_h * est_pages : 1000;  // Stima iniziale
   ```
2. **`WM_PAINT`**: Disegna solo se `!g_render_busy` (già c'è)
3. **Scroll**: Quando `g_scroll` entra in zona non ancora renderizzata → `request_render_range(first, last)`

### Nuovo Job Type
```c
typedef enum JobKind { ..., JOB_RENDER_RANGE } JobKind;
struct Job { ..., int first_page, last_page; };
```

### Test
- Apri `test.docx` (10+ pagine) → **Finestra appare in < 1s** (solo DOM + pagina 0)
- Scroll giù → **Pagine successive renderizzate on-demand** (vedi log `JOB_RENDER_RANGE`)
- Scroll su/giù veloce → **Nessun crash, niente artefatti**

**Commit**: `feat: viewer first-paint page-0 only + on-demand scroll`

---

## FASE 7 — True Incremental Layout in `ld_render_range` (solo `litedocx_c.cpp`) (120 min)
**Obiettivo**: `ld_render_range` **non fa più render completo**. Usa `litehtml::document::render` con `max_width` ma **limita l'altezza** via viewport fittizio o clip. *Richiede indagine su litehtml: `render(max_width)` calcola tutto; non c'è API parziale nativa.*

### Strategia Realistica (v1)
- **Mantieni render completo alla prima chiamata** (Fase 5) — è già "abbastanza veloce" per first paint
- **Ottimizzazione**: `ld_render_range` per pagine > 0 diventa **no-op** (geometrie già in cache)
- **Guadagno reale**: First paint = DOM parse + 1 render (non 2), scroll = zero layout

### Strategia Avanzata (v2 — opzionale, post-MVP)
- Fork `litehtml::document::render` → `render_range(max_width, y_start, y_end)`
- O usa `position` fittizio per "spingere" il layout solo fino a `y_end`
- **Non fare ora** — YAGNI, complica troppo

### Decisione: **Fase 7 = Skip / Merge in Fase 5**. La cache geometrie + no-op per pagine successive **È** il layout incrementale per i nostri scopi.

---

## FASE 8 — LRU Cache Eviction (opzionale, `litedocx_c.cpp` + `docxview_new.c`) (45 min)
**Obiettivo**: Se documento ha 100+ pagine, non tenere tutte le geometrie in RAM.

### Policy semplice
- `m_page_heights` / `m_page_y_offsets`: `vector<int>` — **costo trascurabile** (400 byte/pagina → 40 KB per 100 pagine)
- **Non serve LRU** per le geometrie
- LRU serve per **immagini decode** → già c'è in `ld_container::images` (map, non LRU)
- **Rimanda**: Se `images` cresce troppo, aggiungi LRU lì (separato)

**Conclusione**: **Fase 8 non necessaria per MVP**.

---

## FASE 9 — Polish & Edge Cases (60 min)
- Resize finestra → `request_render_range` per pagine visibili
- `ld_clear_page_cache` → `m_page_heights.clear(); m_last_rendered_page = -1;`
- Gestione documento **senza page-break** (1 pagina logica)
- Test con: tabelle grandi, immagini, liste annidate, header/footer

---

## 📋 Checklist Riepilogativa

| Fase | File toccati | Rischio | Tempo stimato | Test chiave |
|------|--------------|---------|---------------|-------------|
| 0 | — | 0 | 15m | `test_smoke.bat` passa |
| 1 | `litedocx_c.h` | 0 | 30m | Compila, smoke OK |
| 2 | `litedocx_c.cpp` (struct) | Basso | 45m | Compila, smoke OK |
| 3 | `docx_builder.cpp` | Basso | 30m | `data-page-break` visibile |
| 4 | `litedocx_c.cpp` (getters) | Medio | 60m | Log pagine corretto |
| 5 | `litedocx_c.cpp` (render_range) | Medio | 90m | First paint identico |
| 6 | `docxview_new.c` (viewer) | **Alto** | 60m | **<1s first paint, scroll on-demand** |
| 9 | Tutti (polish) | Basso | 60m | Nessuna regressione |

**Totale stimato**: ~6 ore **lavoro netto** (esclusi test manuali).

---

## 🚀 Prossimo Passo Immediato
```bash
cd C:/Users/pr30565/Desktop/Pure_c/c_docx_viewer
git checkout -b feat/incremental-layout litedocx
# FASE 0: verifica baseline
./build_viewer_new.bat
./docxview_new.exe test.docx  # Apri un file reale
# Se tutto OK → inizia FASE 1
```

---

## 🛑 Rollback Points (Tag Git)
Dopo ogni fase che passa i test:
```bash
git tag -a phase-N-done -m "Phase N complete & tested"
```
Se Fase N+1 rompe → `git reset --hard phase-N-done`.

---

## 📝 Note per il Developer
- **Non toccare `litehtml`** — il clip rect in `ld_draw` basta
- **Worker thread**: Riutilizza `JOB_RENDER` → rinomina in `JOB_RENDER_RANGE`, passa `first_page, last_page`
- **Log**: Usa `log_msg` esistente per tracciare `ld_render_range` calls
- **Debug**: Aggiungi `LITEDOCX_DEBUG=1` in `litedocx_c.cpp` se serve tracciare layout

---

*Documento vivo — aggiorna man mano che le fasi procedono.*