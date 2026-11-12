/*
 * text_utf8.h - conversione UTF-8 -> UTF-16 per le API GDI wide.
 */
#ifndef TEXT_UTF8_H
#define TEXT_UTF8_H

#include <windows.h>

/* Restituisce una stringa allocata con malloc (da liberare con free). */
WCHAR *utf8_to_wide(const char *s);

#endif /* TEXT_UTF8_H */
