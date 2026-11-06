/*
 * dev/dump_html.c - strumento di collaudo (non fa parte della libreria).
 *
 * Scrive l'HTML prodotto su stdout o su file, cosi' si puo' ispezionare senza
 * aprire una finestra. Uso:
 *     dev\dump_html.exe test.docx [out.html] [--fragment] [--css extra.css]
 */
#include "../docx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    b = (char *)malloc((size_t)n + 1);
    if (!b) {
        fclose(f);
        return NULL;
    }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return NULL;
    }
    b[n] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    docx_html_opts opts;
    docx_html h;
    const char *out = NULL;
    char *css = NULL;
    int i;

    if (argc < 2) {
        fprintf(stderr, "uso: dump_html.exe file.docx [out.html] [--fragment] [--css extra.css]\n");
        return 2;
    }

    opts.title = NULL;
    opts.extra_css = NULL;
    opts.font_px = 0;
    opts.full_document = 1;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--fragment") == 0) {
            opts.full_document = 0;
        } else if (strcmp(argv[i], "--css") == 0 && i + 1 < argc) {
            css = slurp(argv[++i]);
            if (!css) {
                fprintf(stderr, "impossibile leggere il CSS: %s\n", argv[i]);
                return 2;
            }
            opts.extra_css = css;
        } else if (!out) {
            out = argv[i];
        }
    }

    if (!docx_file_to_html(argv[1], &opts, &h)) {
        fprintf(stderr, "errore: %s\n", h.error);
        free(css);
        return 1;
    }

    if (out) {
        FILE *f = fopen(out, "wb");
        if (!f) {
            fprintf(stderr, "impossibile creare %s\n", out);
            docx_html_free(&h);
            free(css);
            return 1;
        }
        fwrite(h.data, 1, h.len, f);
        fclose(f);
        fprintf(stderr, "scritto %s (%lu byte, %d paragrafi)\n",
                out, (unsigned long)h.len, h.paragraphs);
    } else {
        fwrite(h.data, 1, h.len, stdout);
        fprintf(stderr, "\n%lu byte, %d paragrafi\n", (unsigned long)h.len, h.paragraphs);
    }

    docx_html_free(&h);
    free(css);
    return 0;
}
