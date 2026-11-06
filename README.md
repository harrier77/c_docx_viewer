# docx2html - DOCX -> HTML in C puro per litehtml (TCC)

Libreria C senza `main()` e senza CLI: legge un file `.docx` e restituisce una
stringa HTML UTF-8 con il **CSS incorporato in un blocco `<style>`**, pronta
per `lh_create()` di `litehtml_c.dll`.

```
docx.h          API pubblica (docx_file_to_html / docx_mem_to_html / free)
docx.c          orchestrazione: file -> ZIP -> XML -> HTML completo
zip.c/.h        lettore ZIP (EOCD, central directory, stored + deflate)
inflate.c       puff.c (Mark Adler, zlib license) - inflate raw, C89
puff.h          header di puff
xmlbuf.c/.h     scanner XML sequenziale + decodifica entità
wml.c/.h        WordprocessingML -> HTML (paragrafi, titoli, run, link, w:jc)
htmlbuf.c/.h    buffer dinamico, escaping HTML, gestione spazi Word
css.c/.h        tema CSS incorporato (senza var(), non supportate da litehtml)
build.bat       compila la libreria e gli strumenti di collaudo in dev\

docxview.c      visualizzatore Win32 (finestra + GDI + litehtml)
litehtml_c.h    header del bridge C della DLL
build_viewer.bat compila docxview.exe
```

## Uso

```c
#include "docx.h"

docx_html h;
if (docx_file_to_html("test.docx", NULL, &h)) {
    void *doc = lh_create(h.data, &callbacks, width, height);
    int w = lh_render(doc, width);
    lh_draw(doc, hdc, 0, 0, 0, 0, width, height);
    lh_destroy(doc);
    docx_html_free(&h);
} else {
    /* h.error contiene il motivo (in italiano) */
}
```

Opzioni (tutte facoltative; `NULL` = default):

```c
docx_html_opts o = DOCX_HTML_OPTS_INIT; /* {NULL,NULL,0,1} */
o.title = "Il mio documento";  /* <title>; default = nome del file */
o.extra_css = "p{color:#333;}";/* accodato al blocco <style> */
o.font_px = 15;                /* font base in px */
o.full_document = 0;           /* solo frammento, senza <html>/<style> */
```

`docx_mem_to_html(buf, size, ...)` fa lo stesso da un `.docx` già in memoria
(utile se il file arriva da una risorsa o da rete).

## Visualizzatore `docxview.exe`

`docxview.c` è l'app viewer: converte il `.docx` e lo disegna in una finestra
Win32 tramite `litehtml_c.dll` (stesso schema del pannello di `sunburst.c`).

```bat
build_viewer.bat
docxview.exe test.docx
docxview.exe dev\fixture.docx
```

Senza argomento apre `test.docx` accanto all'exe; se non lo trova mostra il
dialogo "Apri".

Comandi:

| Comando | Azione |
|---|---|
| rotella / ↑ ↓ / PgUp PgDn / Home End | scorrimento |
| `Ctrl+O` | apri un altro `.docx` |
| `F5` | ricarica il file corrente |
| `Esc` | chiudi |
| click su un link | apre il browser (o salta all'ancora `#id`) |
| ridimensionamento finestra | re-layout immediato di litehtml |

Note tecniche:

- **layout a foglio**: il documento viene impaginato alla larghezza della
  pagina (max 960 px) e disegnato centrato su una scrivania grigia, con bordo
  e ombra leggera; il testo va a capo come su carta e i margini interni sono
  `60px 64px` (override CSS passato dal viewer). Riducendo la finestra la
  pagina si restringe mantenendo 24 px di margine per lato;
- doppio buffer GDI (`CreateCompatibleDC` + `BitBlt`), niente sfarfallio;
- font, metriche e wrapping passano dai callback `lh_callbacks`: i testi UTF-8
  sono convertiti in UTF-16 per `DrawTextW`/`GetTextExtentPoint32W`;
- i link sono aperti con `ShellExecuteA` caricata a runtime da `shell32.dll`
  (tcc non ha l'import library);
- `docxview` cerca `litehtml_c.dll` accanto all'exe, poi in `..\sunburst\` e
  `..\litehtml_host\`, infine nel percorso assoluto usato in sviluppo.

Richiede `litehtml_c.dll` e, per compilare, i sorgenti della libreria nella
stessa cartella.

## Compilazione

```bat
build.bat
```

Verifica la libreria con `-Wall` e produce:

- `dev\dump_html.exe` — converte e scrive l'HTML su file/stdout (collaudo)
- `dev\check_litehtml.exe` — converte, carica `litehtml_c.dll`, esegue
  `lh_create`/`lh_render` e stampa `width`/`height` (collaudo d'integrazione)
- `dev\check_justify.exe` — renderizza un paragrafo giustificato e stampa
  la posizione di ogni pezzo di testo (collaudo di `text-align:justify`)

Per l'app host basta aggiungere i `.c` alla riga tcc esistente:

```bat
tcc app.c docx.c zip.c inflate.c xmlbuf.c htmlbuf.c wml.c css.c ^
    -o app.exe -lgdi32 -luser32 -lkernel32 -Wall
```

## Cosa viene convertito

| DOCX | HTML |
|---|---|
| `w:p` | `<p>` |
| `w:pStyle` = `Heading1..9`, `Title`, `Subtitle` | `<h1>`..`<h6>` |
| `w:b` / `w:i` / `w:u` | `<strong>` / `<em>` / `<u>` |
| `w:color`, `w:sz` | `<span style="color:#…;font-size:…px">` |
| `w:t` | testo con escaping e spazi preservati |
| `w:br`, `w:cr` | `<br>` |
| `w:tab` | quattro `&#160;` |
| `w:hyperlink` (r:id / anchor) | `<a href="…">` via `document.xml.rels` |
| `w:jc` (diretto, dallo stile del paragrafo o da `docDefaults`) | `style="text-align:left\|center\|right\|justify"` |
| `w:instrText`, `w:delText` | ignorati |

Per `w:jc` vale la gerarchia di Word: `w:pPr/w:jc` del paragrafo, poi lo
stile indicato da `w:pStyle` seguendo la catena `w:basedOn`, poi lo stile
predefinito (`w:default="1"`), infine `w:docDefaults` (da `word/styles.xml`,
che viene letto solo per questo). I valori `both`/`distribute` diventano
`justify`: litehtml estende le righe intermedie fino al bordo destro e
lascia invariata l'ultima riga.

Fuori scope (per ora): liste (`numbering.xml`), tabelle, immagini, aspetti
degli stili diversi dall'allineamento. I paragrafi dentro tabelle/liste
vengono comunque emessi come `<p>` lineari, quindi il testo non va perso.

## Note su litehtml

- Il CSS usa solo proprietà supportate: **niente `var(--…)`, niente grid**.
- Il tema non imposta `max-width`: il wrapping lo decide `lh_render(doc, width)`.
- Le unità sono px (il font base è configurabile con `font_px`).
- Gli spazi iniziali/finali e multipli diventano `&#160;` (li rende visibili
  litehtml); i singoli spazi interni restano spazi normali per permettere il
  ritorno a capo.
- Le immagini non sono supportate: `litehtml_c.dll` non ha un callback immagini.

## Collaudo

```bat
dev\dump_html.exe test.docx dev\out.html
dev\check_litehtml.exe test.docx
dev\check_litehtml.exe dev\fixture.docx
dev\check_justify.exe
```

Risultati attesi:

- `test.docx` -> 3 paragrafi, `layout: width=800 height=138`
- `fixture.docx` -> titoli h1/h2/h3, grassetto, corsivo, link, accenti e
  virgolette tipografiche preservati (UTF-8), `height ~ 310`
- `check_justify.exe` -> le righe intermedie finiscono a `right=400`
  (parole distanziate), l'ultima riga resta non estesa

## Limiti noti

- ZIP64, ZIP cifrati e metodi di compressione diversi da store/deflate vengono
  rifiutati con un errore esplicito.
- Dimensione massima di una entry decompressa: 64 MB; file `.docx` massimo
  128 MB.
- Il documento deve contenere `word/document.xml` (standard OOXML).
- Un singolo spazio finale di un run diventa `&#160;`, quindi in quel punto non
  c'è ritorno a capo: è il compromesso per non perdere la spaziatura di Word.
