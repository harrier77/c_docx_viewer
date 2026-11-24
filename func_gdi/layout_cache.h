/*
 * layout_cache.h - Cache in memoria del layout documento.
 *
 * Separazione calcolo layout (salvato qui) da rendering (legge da qui).
 * Così zoom/pan/resize non richiedono ricalcolo: basta trasformare le coordinate.
 */
#ifndef LAYOUT_CACHE_H
#define LAYOUT_CACHE_H

#include <windows.h>

/* Tipi di elemento layout */
typedef enum LayoutElementType {
    LAYOUT_EL_NONE         = 0,
    LAYOUT_EL_PARAGRAPH    = 1,  /* <p>, <div>, blocchi di testo */
    LAYOUT_EL_TEXT_RUN     = 2,  /* run di testo dentro paragrafo */
    LAYOUT_EL_TABLE        = 3,
    LAYOUT_EL_TABLE_ROW    = 4,
    LAYOUT_EL_TABLE_CELL   = 5,
    LAYOUT_EL_IMAGE        = 6,
    LAYOUT_EL_LIST_MARKER  = 7,
    LAYOUT_EL_BORDER       = 8,
    LAYOUT_EL_BACKGROUND   = 9,
} LayoutElementType;

/* Allineamento paragrafo */
typedef enum LayoutAlignment {
    LAYOUT_ALIGN_LEFT   = 0,
    LAYOUT_ALIGN_CENTER = 1,
    LAYOUT_ALIGN_RIGHT  = 2,
    LAYOUT_ALIGN_JUSTIFY = 3,
} LayoutAlignment;

/* Forward declaration */
struct LayoutParagraph;

/* Singolo run di testo (porzione omogenea: stesso font, colore, stile) */
typedef struct TextRun {
    char                *text;           /* UTF-8, owned dalla cache */
    int                  x, y;           /* Posizione in coordinate documento (px layout) */
    int                  w, h;           /* Dimensioni */
    void                *font_handle;    /* Handle font (opaco per il viewer, da litedocx) */
    COLORREF             color;          /* Colore testo */
    int                  underline;      /* 1 = sottolineato */
    int                  strike;         /* 1 = barrato */
    /* Identita' del font e offset nel testo del paragrafo: usati dal caret
     * inline per misurare i caratteri e collegare il run al testo modificabile. */
    int                  char_start;     /* offset byte inizio nel paragrafo, -1 = sconosciuto */
    int                  char_end;       /* offset byte fine (esclusivo), -1 = sconosciuto */
    int                  font_size;
    int                  bold;
    int                  italic;
    char                 family[64];
    struct TextRun      *next;           /* Linked list per paragrafo */
} TextRun;

/* Paragrafo / blocco di layout */
typedef struct LayoutParagraph {
    int                      id;              /* ID univoco (puntatore elemento litehtml) */
    LayoutElementType        type;            /* LAYOUT_EL_PARAGRAPH */
    int                      x, y;            /* Posizione top-left in coordinate documento */
    int                      w, h;            /* Dimensioni bounding box */
    int                      content_x, content_y;  /* Offset contenuto (padding) */
    int                      content_w, content_h;
    TextRun                 *runs;            /* Lista run di testo (head) */
    TextRun                 *runs_tail;       /* Tail per append O(1) */
    int                      run_count;
    /* Stile paragrafo */
    LayoutAlignment          alignment;       /* Allineamento */
    int                      line_spacing;    /* Interlinea in px */
    int                      space_before;    /* Spazio prima */
    int                      space_after;     /* Spazio dopo */
    int                      indent_left;     /* Rientro sinistro */
    int                      indent_right;    /* Rientro destro */
    int                      indent_first;    /* Rientro prima riga */
    /* Gerarchia */
    struct LayoutParagraph  *parent;          /* Contenitore (es. cella tabella, document) */
    struct LayoutParagraph  *first_child;     /* Primo figlio */
    struct LayoutParagraph  *next_sibling;    /* Fratello successivo */
    struct LayoutParagraph  *prev_sibling;    /* Fratello precedente (per rimozione facile) */
} LayoutParagraph;

/* Informazioni pagina per paginazione */
typedef struct LayoutPageInfo {
    int page_index;        /* 0-based */
    int y_offset;          /* Offset Y dall'inizio documento */
    int height;            /* Altezza pagina */
    int content_top;       /* Top area contenuto (escl. header/footer) */
    int content_bottom;    /* Bottom area contenuto */
} LayoutPageInfo;

/* Cache completa del layout documento */
typedef struct LayoutCache {
    /* Albero paragrafi */
    LayoutParagraph         *root;                /* Radice (documento) */
    int                      paragraph_count;     /* Totale paragrafi (contatore) */
    int                      total_width;         /* Larghezza layout */
    int                      total_height;        /* Altezza layout */
    /* Paginazione */
    int                      page_count;          /* Numero pagine */
    LayoutPageInfo          *pages;               /* Array info pagine [page_count] */
    /* Metadati layout */
    int                      layout_width;        /* Larghezza usata per ld_render (max_width) */
    int                      justification_width; /* Larghezza giustificazione */
    int                      visual_padding_x;    /* Padding visuale orizzontale */
    int                      visual_padding_y;    /* Padding visuale verticale */
    /* Validità e versioning */
    int                      valid;               /* 1 = cache popolata e valida */
    int                      generation;          /* Incrementa a ogni invalidate/populate */
    /* Font cache reference (per evitare ricreazione font) */
    void                    *font_cache_ref;      /* Riferimento opaco a font cache interna */
} LayoutCache;

/* Callback per enumerazione paragrafi */
typedef void (*LayoutCacheEnumCallback)(const LayoutParagraph *para, void *ctx);

/* ------------------------------------------------------------------------- */
/* API Pubblica                                                              */
/* ------------------------------------------------------------------------- */

/* Crea una nuova cache vuota. Restituisce NULL se OOM. */
LayoutCache* layout_cache_create(void);

/* Distrugge la cache e libera tutta la memoria (paragrafi, run, testo, pagine). */
void layout_cache_destroy(LayoutCache *cache);

/* Invalida la cache: marca come non valida, mantiene struttura per riutilizzo.
 * Libera i run di testo e paragrafi ma non la struttura LayoutCache stessa. */
void layout_cache_invalidate(LayoutCache *cache);

/* Popola la cache estraendo il layout da litedocx.
 * Deve essere chiamata DOPO che ld_render ha completato e ld_draw ha visitato l'albero.
 * Restituisce 1 se successo, 0 se errore (cache non valida). */
int layout_cache_populate(LayoutCache *cache, void *doc_handle);

/* Trova il paragrafo che contiene il punto (x,y) in coordinate documento.
 * Restituisce NULL se nessun paragrafo sotto quel punto. */
const LayoutParagraph* layout_cache_find_paragraph_at(LayoutCache *cache, int x, int y);

/* Trova il run di testo che contiene il punto (x,y). Se il punto cade
 * nello spazio tra due parole (o tra due righe), restituisce il run piu'
 * vicino: il caret verra' ancorato al bordo di quel run.
 * Se para_out non NULL, imposta al paragrafo contenitore. */
const TextRun* layout_cache_find_run_at(LayoutCache *cache, int x, int y, const LayoutParagraph **para_out);

/* Enumera tutti i paragrafi in ordine (depth-first, top-down).
 * Utile per rendering da cache, hit-testing batch, export. */
void layout_cache_enum_paragraphs(LayoutCache *cache, LayoutCacheEnumCallback cb, void *ctx);

/* Enumera i run di un paragrafo in ordine. */
void layout_cache_enum_runs(const LayoutParagraph *para, void (*cb)(const TextRun*, void*), void *ctx);

/* Funzioni di costruzione cache (usate dai callback litedocx) */
LayoutParagraph* layout_cache_add_paragraph(LayoutCache *cache,
                                            LayoutParagraph *parent,
                                            int id, int x, int y, int w, int h,
                                            LayoutAlignment alignment,
                                            int line_spacing,
                                            int space_before, int space_after,
                                            int indent_left, int indent_right, int indent_first);

int layout_cache_add_run(LayoutParagraph *para,
                         const char *text, int x, int y, int w, int h,
                         void *font_handle, COLORREF color,
                         int underline, int strike);

/* Ottiene info pagina per indice. Restituisce NULL se indice non valido. */
const LayoutPageInfo* layout_cache_get_page(LayoutCache *cache, int page_index);

/* Trova l'indice della pagina che contiene la coordinata Y documento. */
int layout_cache_find_page_at_y(LayoutCache *cache, int y);

/* Imposta metadati layout (chiamato prima di populate o dopo invalidate). */
void layout_cache_set_layout_params(LayoutCache *cache,
                                    int layout_width,
                                    int justification_width,
                                    int visual_padding_x,
                                    int visual_padding_y);

/* Ricalcola total_width/total_height dai paragrafi presenti.
 * Va chiamato dopo l'estrazione dei run (worker_extract_layout). */
void layout_cache_update_bounds(LayoutCache *cache);

/* Debug: stampa statistiche cache su stderr. */
void layout_cache_dump_stats(const LayoutCache *cache);

#endif /* LAYOUT_CACHE_H */