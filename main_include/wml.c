#include "wml.h"
#include "xmlbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------- relationships ------------------------- */

int wml_parse_rels(const char *xml, size_t len, wml_rels *rels)
{
    xscan s;
    xtok t;

    rels->items = NULL;
    rels->count = 0;

    xs_init(&s, xml, len);
    while (xs_next(&s, &t)) {
        char id[48], target[768];

        if (t.kind != XT_TAG || t.closing || !xname_is(&t, "Relationship"))
            continue;
        if (!xattr(t.attrs, t.attrs_len, "Id", id, sizeof(id)))
            continue;
        if (!xattr(t.attrs, t.attrs_len, "Target", target, sizeof(target)))
            continue;

        {
            wml_rel *np = (wml_rel *)realloc(rels->items,
                                             (size_t)(rels->count + 1) * sizeof(wml_rel));
            if (!np)
                return 0;
            rels->items = np;
            strcpy(np[rels->count].id, id);
            strcpy(np[rels->count].target, target);
            rels->count++;
        }
    }
    return 1;
}

void wml_free_rels(wml_rels *rels)
{
    free(rels->items);
    rels->items = NULL;
    rels->count = 0;
}

static const char *rel_lookup(const wml_rels *rels, const char *id)
{
    int i;

    if (!rels)
        return NULL;
    for (i = 0; i < rels->count; i++)
        if (strcmp(rels->items[i].id, id) == 0)
            return rels->items[i].target;
    return NULL;
}

/* --------------------------- stili (w:jc) --------------------------- */

void wml_free_styles(wml_styles *st)
{
    if (!st)
        return;
    free(st->items);
    st->items = NULL;
    st->count = 0;
    st->def_idx = -1;
    st->doc_jc[0] = 0;
}

/* Legge word/styles.xml e tiene solo cio' che serve per l'allineamento:
 * w:jc di ogni stile paragrafo, la catena w:basedOn, lo stile predefinito
 * e il w:jc di w:docDefaults. Gli altri aspetti degli stili sono ignorati. */
int wml_parse_styles(const char *xml, size_t len, wml_styles *st)
{
    xscan s;
    xtok t;
    int in_doc = 0, in_pprd = 0, in_ppr = 0;
    int in_style = 0, style_ppr = 0, idx = -1;

    if (!st)
        return 0;
    st->items = NULL;
    st->count = 0;
    st->def_idx = -1;
    st->doc_jc[0] = 0;

    xs_init(&s, xml, len);
    while (xs_next(&s, &t)) {
        if (t.kind != XT_TAG)
            continue;

        if (!t.closing) {
            if (xname_is(&t, "docDefaults")) {
                if (!t.self_closing) in_doc = 1;
                continue;
            }
            if (in_doc && xname_is(&t, "pPrDefault")) {
                if (!t.self_closing) in_pprd = 1;
                continue;
            }
            if (in_pprd && xname_is_w(&t, "pPr")) {
                if (!t.self_closing) in_ppr = 1;
                continue;
            }
            if (in_ppr && xname_is_w(&t, "jc")) {
                char v[16];
                if (!xattr(t.attrs, t.attrs_len, "w:val", v, sizeof(v)))
                    strcpy(v, "left"); /* val predefinito di ST_Jc */
                snprintf(st->doc_jc, sizeof(st->doc_jc), "%s", v);
                continue;
            }
            if (xname_is_w(&t, "style")) {
                char id[64], dflt[8];
                idx = -1;
                if (xattr(t.attrs, t.attrs_len, "w:styleId", id, sizeof(id))) {
                    wml_style *np = (wml_style *)realloc(
                        st->items, (size_t)(st->count + 1) * sizeof(wml_style));
                    if (!np) {
                        wml_free_styles(st);
                        return 0;
                    }
                    st->items = np;
                    idx = st->count++;
                    memset(&st->items[idx], 0, sizeof(wml_style));
                    snprintf(st->items[idx].id, sizeof(st->items[idx].id), "%s", id);
                    if (xattr(t.attrs, t.attrs_len, "w:default", dflt, sizeof(dflt)) &&
                        strcmp(dflt, "1") == 0)
                        st->def_idx = idx;
                }
                in_style = (idx >= 0 && !t.self_closing);
                style_ppr = 0;
                continue;
            }
            if (in_style && xname_is_w(&t, "basedOn")) {
                char v[64];
                if (idx >= 0 && xattr(t.attrs, t.attrs_len, "w:val", v, sizeof(v)))
                    snprintf(st->items[idx].based_on, sizeof(st->items[idx].based_on),
                             "%s", v);
                continue;
            }
            if (in_style && xname_is_w(&t, "pPr")) {
                if (!t.self_closing) style_ppr = 1;
                continue;
            }
            if (in_style && style_ppr && xname_is_w(&t, "jc")) {
                char v[16];
                if (idx >= 0) {
                    if (!xattr(t.attrs, t.attrs_len, "w:val", v, sizeof(v)))
                        strcpy(v, "left");
                    snprintf(st->items[idx].jc, sizeof(st->items[idx].jc), "%s", v);
                }
                continue;
            }
            continue;
        }

        /* tag di chiusura */
        if (xname_is(&t, "docDefaults")) {
            in_doc = 0; in_pprd = 0; in_ppr = 0;
        } else if (xname_is(&t, "pPrDefault")) {
            in_pprd = 0; in_ppr = 0;
        } else if (xname_is_w(&t, "pPr")) {
            if (in_ppr) in_ppr = 0;
            if (style_ppr) style_ppr = 0;
        } else if (xname_is_w(&t, "style")) {
            in_style = 0;
            style_ppr = 0;
            idx = -1;
        }
    }
    return 1;
}

static int style_find(const wml_styles *st, const char *id)
{
    int i;

    if (!st || !id || !id[0])
        return -1;
    for (i = 0; i < st->count; i++)
        if (strcmp(st->items[i].id, id) == 0)
            return i;
    return -1;
}

/* Valore OOXML di w:jc -> text-align CSS. NULL se non riconosciuto. */
static const char *jc_to_css(const char *jc)
{
    if (!jc || !jc[0])
        return NULL;
    if (strcmp(jc, "left") == 0 || strcmp(jc, "start") == 0)
        return "left";
    if (strcmp(jc, "right") == 0 || strcmp(jc, "end") == 0)
        return "right";
    if (strcmp(jc, "center") == 0)
        return "center";
    if (strcmp(jc, "both") == 0 || strcmp(jc, "bothStr") == 0 ||
        strcmp(jc, "distribute") == 0 || strcmp(jc, "thaiDistribute") == 0)
        return "justify";
    return NULL;
}

/* --------------------------- converter --------------------------- */

typedef struct wml_ctx {
    htmlbuf *out;
    const wml_rels *rels;

    const wml_styles *styles;   /* stili di styles.xml, puo' essere NULL */

    int heading;        /* 0 = <p>, 1..6 = <hN> */
    char jc[16];        /* w:jc diretto del paragrafo, "" se assente */
    char style_id[64];  /* w:pStyle del paragrafo, "" se assente */
    int in_tbl;           /* inside a table */
    int in_tr;            /* inside a table row */
    int in_tc;            /* inside a table cell */
    int tc_open;          /* <td> emitted for current cell */
    int tcpr, trpr;       /* inside w:tcPr / w:trPr */
    int tc_bg_on, tr_bg_on; /* w:shd fill di cella / riga */
    char tc_bg[7], tr_bg[7];
    int tc_span;            /* w:gridSpan della cella corrente */
    int in_para;
    int para_tag_open;
    int para_content;
    int paragraphs;

    int in_run;
    int run_open;
    int bold, italic, underline;
    int color_on;
    char color[7];
    int size_half;      /* w:sz: half points */

    int ppr;            /* inside w:pPr */
    int rpr;            /* inside w:rPr (1 = run props, 2 = ignored pPr rPr) */
    int capture;        /* inside w:t */
    int preserve;
    int skip;           /* inside w:instrText / w:delText */
    int link_open;

    int err;
} wml_ctx;

static int truthy(const xtok *t)
{
    char val[16];

    if (xattr(t->attrs, t->attrs_len, "w:val", val, sizeof(val))) {
        if (strcmp(val, "0") == 0 || strcmp(val, "false") == 0 ||
            strcmp(val, "none") == 0 || strcmp(val, "off") == 0)
            return 0;
        if (strcmp(val, "1") == 0 || strcmp(val, "true") == 0 ||
            strcmp(val, "on") == 0 || strcmp(val, "single") == 0 ||
            strcmp(val, "double") == 0)
            return 1;
        if (strcmp(val, "auto") == 0)
            return 0;
        /* any other value (e.g. "single") counts as on */
        return 1;
    }
    return 1;
}

/* Risolve l'allineamento del paragrafo corrente con le regole di Word:
 * w:jc diretto > stile del paragrafo (catena w:basedOn) > stile
 * predefinito > w:docDefaults. Restituisce il valore CSS o NULL. */
static const char *resolve_jc(const wml_ctx *c)
{
    char jc[16];
    const wml_styles *st = c->styles;
    int idx = -1, hops = 0;

    jc[0] = 0;
    if (c->jc[0]) {
        snprintf(jc, sizeof(jc), "%s", c->jc);
    } else if (st) {
        idx = style_find(st, c->style_id);
        if (idx < 0)
            idx = st->def_idx;
        while (idx >= 0 && hops++ < 16) {
            if (st->items[idx].jc[0]) {
                snprintf(jc, sizeof(jc), "%s", st->items[idx].jc);
                break;
            }
            idx = style_find(st, st->items[idx].based_on);
        }
        if (!jc[0] && st->doc_jc[0])
            snprintf(jc, sizeof(jc), "%s", st->doc_jc);
    }
    return jc_to_css(jc);
}

/* Accoda l'attributo style="text-align:..." se il paragrafo ha un
 * allineamento esplicito; se non c'e' nessun w:jc, nulla viene emesso
 * e l'output resta identico ai documenti senza giustificazione. */
static void emit_align(wml_ctx *c)
{
    const char *css = resolve_jc(c);

    if (css)
        hb_printf(c->out, " style=\"text-align:%s\"", css);
}

/* Lo sfondo della cella e' indispensabile per le righe di intestazione:
 * Word usa spesso testo bianco su w:shd; senza il fill il testo sparisce
 * sul fondo bianco della pagina. */
static void open_td(wml_ctx *c)
{
    const char *bg = NULL;

    if (c->tc_bg_on)
        bg = c->tc_bg;
    else if (c->tr_bg_on)
        bg = c->tr_bg;

    hb_puts(c->out, "<td");
    if (c->tc_span > 1)
        hb_printf(c->out, " colspan=\"%d\"", c->tc_span);
    if (bg)
        hb_printf(c->out, " style=\"background-color:#%s\"", bg);
    hb_puts(c->out, ">");
    c->tc_open = 1;
}

static void open_para(wml_ctx *c)
{
    if (!c->in_para || c->para_tag_open)
        return;
    if (c->in_tc && !c->tc_open)
        open_td(c);
    if (c->heading >= 1 && c->heading <= 6)
        hb_printf(c->out, "<h%d", c->heading);
    else
        hb_puts(c->out, "<p");
    emit_align(c);
    hb_puts(c->out, ">");
    c->para_tag_open = 1;
}

static void close_para(wml_ctx *c)
{
    if (!c->in_para)
        return;

    if (!c->para_tag_open) {
        /* empty paragraph: keep the visual gap */
        if (c->in_tc && !c->tc_open)
            open_td(c);
        if (c->heading >= 1 && c->heading <= 6)
            hb_printf(c->out, "<h%d", c->heading);
        else
            hb_puts(c->out, "<p");
        emit_align(c);
        if (c->heading >= 1 && c->heading <= 6)
            hb_printf(c->out, ">&#160;</h%d>", c->heading);
        else
            hb_puts(c->out, ">&#160;</p>");
        c->paragraphs++;
    } else {
        if (!c->para_content)
            hb_puts(c->out, "&#160;");
        if (c->heading >= 1 && c->heading <= 6)
            hb_printf(c->out, "</h%d>", c->heading);
        else
            hb_puts(c->out, "</p>");
        c->paragraphs++;
    }
    c->in_para = 0;
    c->para_tag_open = 0;
    c->para_content = 0;
    c->heading = 0;
}

static void open_run(wml_ctx *c)
{
    if (!c->in_run || c->run_open || c->skip)
        return;

    if (c->color_on || c->size_half > 0) {
        hb_puts(c->out, "<span style=\"");
        if (c->color_on)
            hb_printf(c->out, "color:#%s;", c->color);
        if (c->size_half > 0) {
            /* half points -> px: pt = sz/2; px = pt * 96 / 72 = sz*2/3 */
            int px = (c->size_half * 2 + 1) / 3;
            if (px > 0)
                hb_printf(c->out, "font-size:%dpx;", px);
        }
        hb_puts(c->out, "\">");
    }
    if (c->bold)
        hb_puts(c->out, "<strong>");
    if (c->italic)
        hb_puts(c->out, "<em>");
    if (c->underline)
        hb_puts(c->out, "<u>");
    c->run_open = 1;
}

static void close_run(wml_ctx *c)
{
    if (!c->run_open)
        return;
    if (c->underline)
        hb_puts(c->out, "</u>");
    if (c->italic)
        hb_puts(c->out, "</em>");
    if (c->bold)
        hb_puts(c->out, "</strong>");
    if (c->color_on || c->size_half > 0)
        hb_puts(c->out, "</span>");
    c->run_open = 0;
}

static void reset_run(wml_ctx *c)
{
    c->bold = c->italic = c->underline = 0;
    c->color_on = 0;
    c->color[0] = 0;
    c->size_half = 0;
    c->run_open = 0;
}

static void start_hyperlink(wml_ctx *c, const xtok *t)
{
    char id[48], anchor[256];
    const char *target = NULL;

    if (c->link_open)
        return;

    if (xattr(t->attrs, t->attrs_len, "r:id", id, sizeof(id)))
        target = rel_lookup(c->rels, id);
    if (!target && xattr(t->attrs, t->attrs_len, "w:anchor", anchor, sizeof(anchor))) {
        open_para(c);
        hb_puts(c->out, "<a href=\"#");
        hb_escape(c->out, anchor, strlen(anchor));
        hb_puts(c->out, "\">");
        c->link_open = 1;
        c->para_content = 1;
        return;
    }
    if (!target)
        return;

    open_para(c);
    hb_puts(c->out, "<a href=\"");
    hb_escape(c->out, target, strlen(target));
    hb_puts(c->out, "\">");
    c->link_open = 1;
    c->para_content = 1;
}

static void end_hyperlink(wml_ctx *c)
{
    if (!c->link_open)
        return;
    hb_puts(c->out, "</a>");
    c->link_open = 0;
}

static void handle_tag(wml_ctx *c, const xtok *t)
{
    /* text-bearing leaf tags handled before the generic dispatch */
    if (!t->closing && xname_is_w(t, "t")) {
        char space[24];

        if (t->self_closing)
            return;
        open_para(c);
        open_run(c);
        c->capture = 1;
        c->preserve = xattr(t->attrs, t->attrs_len, "xml:space", space, sizeof(space)) &&
                      strcmp(space, "preserve") == 0;
        return;
    }
    if (t->closing && xname_is_w(t, "t")) {
        c->capture = 0;
        c->preserve = 0;
        return;
    }
    if (c->capture)
        return; /* no nested tags inside w:t */

    if (xname_is_w(t, "tbl")) {
        if (t->self_closing) {
            hb_puts(c->out, "<table></table>");
            return;
        }
        if (!t->closing) {
            if (c->in_para)
                close_para(c);
            c->in_tbl = 1;
            c->in_tr = 0;
            c->in_tc = 0;
            c->tc_open = 0;
            hb_puts(c->out, "<table>");
            return;
        } else {
            if (c->in_para)
                close_para(c);
            c->in_tbl = 0;
            c->in_tr = 0;
            c->in_tc = 0;
            c->tc_open = 0;
            hb_puts(c->out, "</table>");
            return;
        }
    }

    if (xname_is_w(t, "tr")) {
        if (t->self_closing) {
            hb_puts(c->out, "<tr></tr>");
            return;
        }
        if (!t->closing) {
            if (c->in_para)
                close_para(c);
            c->in_tr = 1;
            c->trpr = 0;
            c->tr_bg_on = 0;
            c->tr_bg[0] = 0;
            hb_puts(c->out, "<tr>");
            return;
        } else {
            c->in_tr = 0;
            c->trpr = 0;
            c->tr_bg_on = 0;
            c->tr_bg[0] = 0;
            hb_puts(c->out, "</tr>");
            return;
        }
    }

    if (xname_is_w(t, "tc")) {
        if (t->self_closing) {
            hb_puts(c->out, "<td></td>");
            return;
        }
        if (!t->closing) {
            if (c->in_para)
                close_para(c);
            c->in_tc = 1;
            c->tc_open = 0;
            c->tcpr = 0;
            c->tc_bg_on = 0;
            c->tc_bg[0] = 0;
            c->tc_span = 0;
            return;
        } else {
            if (c->in_para)
                close_para(c);
            if (!c->tc_open)
                open_td(c);
            hb_puts(c->out, "</td>");
            c->in_tc = 0;
            c->tc_open = 0;
            c->tcpr = 0;
            c->tc_bg_on = 0;
            c->tc_bg[0] = 0;
            c->tc_span = 0;
            return;
        }
    }

    if (xname_is_w(t, "p")) {
        if (t->self_closing) {
            if (c->in_para)
                close_para(c);
            hb_puts(c->out, "<p>&#160;</p>");
            c->paragraphs++;
            return;
        }
        if (!t->closing) {
            if (c->in_para)
                close_para(c);
            c->in_para = 1;
            c->para_tag_open = 0;
            c->para_content = 0;
            c->heading = 0;
            c->jc[0] = 0;
            c->style_id[0] = 0;
            c->ppr = 0;
            c->rpr = 0;
            c->in_run = 0;
            reset_run(c);
        } else {
            close_para(c);
        }
        return;
    }

    if (xname_is_w(t, "tcPr")) {
        c->tcpr = t->closing ? 0 : 1;
        return;
    }
    if (xname_is_w(t, "trPr")) {
        c->trpr = t->closing ? 0 : 1;
        return;
    }
    if (xname_is_w(t, "gridSpan") && !t->closing && c->tcpr) {
        char val[16];
        int n;
        if (xattr(t->attrs, t->attrs_len, "w:val", val, sizeof(val))) {
            n = atoi(val);
            if (n < 1) n = 1;
            if (n > 64) n = 64;
            c->tc_span = n;
        }
        return;
    }
    if (xname_is_w(t, "shd") && !t->closing && !c->rpr && (c->tcpr || c->trpr)) {
        char val[16];
        int ok = 1, k;
        if (xattr(t->attrs, t->attrs_len, "w:fill", val, sizeof(val)) &&
            strlen(val) == 6 && strcmp(val, "auto") != 0) {
            for (k = 0; k < 6; k++) {
                char ch = val[k];
                if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                      (ch >= 'A' && ch <= 'F')))
                    ok = 0;
            }
            if (ok) {
                if (c->tcpr) {
                    strcpy(c->tc_bg, val);
                    c->tc_bg_on = 1;
                } else {
                    strcpy(c->tr_bg, val);
                    c->tr_bg_on = 1;
                }
            }
        }
        return;
    }
    if (xname_is_w(t, "pPr")) {
        c->ppr = t->closing ? 0 : 1;
        return;
    }
    if (xname_is_w(t, "rPr")) {
        if (t->closing)
            c->rpr = 0;
        else
            c->rpr = c->ppr ? 2 : 1;
        return;
    }

    if (xname_is_w(t, "pStyle") && !t->closing) {
        char val[64];
        if (xattr(t->attrs, t->attrs_len, "w:val", val, sizeof(val))) {
            snprintf(c->style_id, sizeof(c->style_id), "%s", val);
            if (strncmp(val, "Heading", 7) == 0 && val[7] >= '1' && val[7] <= '9')
                c->heading = val[7] - '0';
            else if (strcmp(val, "Title") == 0)
                c->heading = 1;
            else if (strcmp(val, "Subtitle") == 0)
                c->heading = 2;
        }
        return;
    }

    if (xname_is_w(t, "jc") && c->ppr && !c->rpr && !t->closing) {
        /* allineamento del paragrafo: w:pPr/w:jc */
        char val[16];
        if (!xattr(t->attrs, t->attrs_len, "w:val", val, sizeof(val)))
            strcpy(val, "left");
        snprintf(c->jc, sizeof(c->jc), "%s", val);
        return;
    }

    if (xname_is_w(t, "r")) {
        if (t->closing) {
            close_run(c);
            c->in_run = 0;
        } else {
            c->in_run = 1;
            reset_run(c);
        }
        return;
    }

    if (c->rpr == 1) {
        if (xname_is_w(t, "b")) {
            if (!t->closing) c->bold = truthy(t);
            return;
        }
        if (xname_is_w(t, "i")) {
            if (!t->closing) c->italic = truthy(t);
            return;
        }
        if (xname_is_w(t, "u")) {
            if (!t->closing) c->underline = truthy(t);
            return;
        }
        if (xname_is_w(t, "color") && !t->closing) {
            char val[16];
            if (xattr(t->attrs, t->attrs_len, "w:val", val, sizeof(val)) &&
                strlen(val) == 6) {
                int ok = 1, k;
                for (k = 0; k < 6; k++) {
                    char ch = val[k];
                    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                          (ch >= 'A' && ch <= 'F')))
                        ok = 0;
                }
                if (ok) {
                    strcpy(c->color, val);
                    c->color_on = 1;
                }
            }
            return;
        }
        if (xname_is_w(t, "sz") && !t->closing) {
            char val[16];
            if (xattr(t->attrs, t->attrs_len, "w:val", val, sizeof(val)))
                c->size_half = atoi(val);
            return;
        }
    }

    if (xname_is_w(t, "br") || xname_is_w(t, "cr")) {
        if (!t->closing) {
            open_para(c);
            open_run(c);
            hb_puts(c->out, "<br>");
            c->para_content = 1;
        }
        return;
    }
    if (xname_is_w(t, "tab") && !t->closing) {
        open_para(c);
        open_run(c);
        hb_puts(c->out, "&#160;&#160;&#160;&#160;");
        c->para_content = 1;
        return;
    }
    if (xname_is_w(t, "noBreakHyphen") && !t->closing) {
        open_para(c);
        open_run(c);
        hb_putc(c->out, '-');
        c->para_content = 1;
        return;
    }

    if (xname_is_w(t, "hyperlink")) {
        if (t->closing)
            end_hyperlink(c);
        else
            start_hyperlink(c, t);
        return;
    }

    if (xname_is_w(t, "instrText") || xname_is_w(t, "delText")) {
        c->skip = t->closing ? 0 : 1;
        return;
    }
}

int wml_to_html(const char *xml, size_t len, const wml_rels *rels,
                const wml_styles *styles, htmlbuf *out, int *paragraphs)
{
    wml_ctx c;
    xscan s;
    xtok t;

    memset(&c, 0, sizeof(c));
    c.out = out;
    c.rels = rels;
    c.styles = styles;

    xs_init(&s, xml, len);
    while (xs_next(&s, &t)) {
        if (t.kind == XT_TEXT) {
            if (c.capture && !c.skip && t.text_len) {
                hb_wml_text(out, t.text, t.text_len, c.preserve);
                c.para_content = 1;
            }
            continue;
        }
        handle_tag(&c, &t);
        if (out->err)
            return 0;
    }

    if (c.link_open)
        end_hyperlink(&c);
    close_run(&c);
    close_para(&c);

    if (paragraphs)
        *paragraphs += c.paragraphs;
    return out->err ? 0 : 1;
}
