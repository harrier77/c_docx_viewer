/*
 * document.h - geometria di paint e accessori del documento.
 *
 * Fase 3a del refactoring: il viewer non calcola piu' a mano i rettangoli
 * pagina/ombra/contenuto ne' chiama direttamente ld_draw.  Il documento resta
 * un handle opaco (void*) e il threading non viene toccato: load/render/job
 * continuano a vivere in docxview_new.c.
 */
#ifndef DOCUMENT_H
#define DOCUMENT_H

#include <windows.h>

#include "layout.h"
#include "renderer.h"

/* Altezza del layout in pixel documento (0 se il documento non e' valido). */
int document_height(void *doc);

/* Larghezza di layout corrente (coerente con layout_content_width()). */
int document_content_width(void);

/* Disegna sfondo pagina + contenuto sul renderer.
 *   doc          handle opaco restituito da ld_open_file/ld_open_memory
 *   r            renderer di destinazione
 *   doc_h        altezza layout memorizzata lato UI
 *   render_busy  1 = layout in corso, mostra solo la pagina bianca
 * Lascia intatto lo sfondo scrivania: lo riempie il chiamante. */
void document_draw(void *doc, Renderer *r, int doc_h, int render_busy);

#endif /* DOCUMENT_H */
