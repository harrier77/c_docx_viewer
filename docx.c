#include "docx.h"
#include "htmlbuf.h"
#include "wml.h"
#include "zip.h"
#include "css.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(docx_html *out, const char *msg)
{
    if (!out)
        return;
    if (out->data) {
        free(out->data);
        out->data = NULL;
        out->len = 0;
    }
    snprintf(out->error, sizeof(out->error), "%s", msg ? msg : "errore sconosciuto");
}

static void base_name(const char *path, char *out, size_t cap)
{
    const char *p = path ? path : "";
    const char *slash = strrchr(p, '\\');
    const char *slash2 = strrchr(p, '/');
    const char *dot;
    size_t n;

    if (slash2 && (!slash || slash2 > slash))
        slash = slash2;
    if (slash)
        p = slash + 1;

    dot = strrchr(p, '.');
    n = dot && dot != p ? (size_t)(dot - p) : strlen(p);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static int read_file(const char *path, char **out, size_t *out_len)
{
    FILE *f;
    long sz;
    char *buf;
    size_t got;

    *out = NULL;
    *out_len = 0;

    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    sz = ftell(f);
    if (sz < 0 || sz > (long)(128u * 1024u * 1024u)) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return 0;
    }
    buf[sz] = 0;
    *out = buf;
    *out_len = (size_t)sz;
    return 1;
}

int docx_mem_to_html(const void *buf, size_t size, const docx_html_opts *opts,
                     docx_html *out)
{
    static const docx_html_opts defs = DOCX_HTML_OPTS_INIT;
    const docx_html_opts *o = opts ? opts : &defs;
    zip_arc z;
    char *xml = NULL, *rels_xml = NULL;
    size_t xml_len = 0, rels_len = 0;
    wml_rels rels;
    wml_styles styles;
    htmlbuf hb;
    int ok = 0, full, font_px, paragraphs = 0;
    const char *title;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));

    rels.items = NULL;
    rels.count = 0;
    styles.items = NULL;
    styles.count = 0;
    styles.def_idx = -1;
    styles.doc_jc[0] = 0;
    hb_init(&hb);

    full = o->full_document ? 1 : 0;
    font_px = (o->font_px > 0) ? o->font_px : 16;
    title = (o->title && o->title[0]) ? o->title : "Documento";

    if (!zip_open(&z, buf, size)) {
        set_error(out, "file .docx non valido (troppo piccolo)");
        goto done;
    }
    if (!zip_read(&z, "word/document.xml", &xml, &xml_len) || !xml) {
        set_error(out, "word/document.xml non trovato o illeggibile");
        goto done;
    }

    if (zip_read(&z, "word/_rels/document.xml.rels", &rels_xml, &rels_len) && rels_xml) {
        if (!wml_parse_rels(rels_xml, rels_len, &rels))
            rels.count = 0;
    }

    /* styles.xml e' opzionale: senza non ci sono w:jc ereditati dagli stili */
    {
        char *styles_xml = NULL;
        size_t styles_len = 0;
        if (zip_read(&z, "word/styles.xml", &styles_xml, &styles_len) && styles_xml)
            wml_parse_styles(styles_xml, styles_len, &styles); /* errore: stili vuoti */
        free(styles_xml);
    }

    if (full) {
        hb_puts(&hb, "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">");
        hb_puts(&hb, "<title>");
        hb_escape(&hb, title, strlen(title));
        hb_puts(&hb, "</title><style>\n");
        hb_puts(&hb, docx_default_css(font_px));
        if (o->extra_css && o->extra_css[0]) {
            hb_puts(&hb, "\n");
            hb_puts(&hb, o->extra_css);
            hb_puts(&hb, "\n");
        }
        hb_puts(&hb, "</style></head><body>\n");
    }

    if (!wml_to_html(xml, xml_len, &rels, &styles, &hb, &paragraphs)) {
        set_error(out, "memoria esaurita durante la conversione");
        goto done;
    }

    if (full)
        hb_puts(&hb, "\n</body></html>\n");

    if (hb.err) {
        set_error(out, "memoria esaurita durante la conversione");
        goto done;
    }

    out->data = hb_detach(&hb, &out->len);
    out->paragraphs = paragraphs;
    out->error[0] = 0;
    if (!out->data) {
        set_error(out, "memoria esaurita");
        goto done;
    }
    ok = 1;

done:
    free(xml);
    free(rels_xml);
    wml_free_rels(&rels);
    wml_free_styles(&styles);
    hb_free(&hb);
    return ok;
}

int docx_file_to_html(const char *path, const docx_html_opts *opts, docx_html *out)
{
    char *buf = NULL, name[260];
    size_t size = 0;
    docx_html_opts local;
    int ok;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));

    if (!path || !path[0]) {
        set_error(out, "percorso vuoto");
        return 0;
    }
    if (!read_file(path, &buf, &size)) {
        set_error(out, "impossibile leggere il file");
        return 0;
    }

    if (opts) {
        local = *opts;
    } else {
        local.title = NULL;
        local.extra_css = NULL;
        local.font_px = 0;
        local.full_document = 1;
    }
    if (!local.title || !local.title[0]) {
        base_name(path, name, sizeof(name));
        local.title = name[0] ? name : "Documento";
    }

    ok = docx_mem_to_html(buf, size, &local, out);
    free(buf);
    return ok;
}

void docx_html_free(docx_html *out)
{
    if (!out)
        return;
    free(out->data);
    out->data = NULL;
    out->len = 0;
    out->paragraphs = 0;
    out->error[0] = 0;
}
