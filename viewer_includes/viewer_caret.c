/*
 * viewer_caret.c - caret inline basato sulla layout cache.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti in viewer_state.c.
 *
 * Il testo modificato viene tenuto in un buffer locale (g_caret_text) e
 * disegnato come overlay dopo Present().  Al termine del debounce viene
 * applicato con ld_paragraph_set_text: la formattazione dei run del
 * paragrafo viene appiattita, ma l'elemento paragrafo resta stabile.
 */

/* Copia del run sotto il caret: la cache puo' essere invalidata prima del
 * ri-ancora, quindi non conserviamo il puntatore al TextRun. */
static TextRun g_caret_run;

/* Formato DirectWrite privato del caret: NON passa dalla font cache globale
 * (che e' usata anche dal worker di render e non e' thread-safe). */
static IDWriteTextFormat *g_caret_format = NULL;
static char g_caret_fmt_family[64];
static int  g_caret_fmt_size = 0;
static int  g_caret_fmt_bold = 0;
static int  g_caret_fmt_italic = 0;

static void caret_release_format(void)
{
    if (g_caret_format) {
        IDWriteTextFormat_Release(g_caret_format);
        g_caret_format = NULL;
    }
    g_caret_fmt_family[0] = '\0';
    g_caret_fmt_size = 0;
}

/* Crea/aggiorna il formato privato in base all'identita' del run. */
static void caret_ensure_format(const TextRun *r)
{
    const char *fam;
    int size, bold, italic;
    WCHAR *wfam;
    HRESULT hr;

    if (!r || !g_dwrite_factory) return;
    fam = r->family[0] ? r->family : "Aptos";
    size = r->font_size > 0 ? r->font_size : 12;
    bold = r->bold;
    italic = r->italic;

    if (g_caret_format &&
        g_caret_fmt_size == size && g_caret_fmt_bold == bold &&
        g_caret_fmt_italic == italic &&
        strcmp(g_caret_fmt_family, fam) == 0)
        return;

    caret_release_format();
    wfam = u8_to_w(fam);
    if (!wfam) return;
    hr = IDWriteFactory_CreateTextFormat(g_dwrite_factory, wfam, NULL,
        bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, (FLOAT)size, L"en-US", &g_caret_format);
    free(wfam);
    if (FAILED(hr) || !g_caret_format) {
        g_caret_format = NULL;
        return;
    }
    lstrcpynA(g_caret_fmt_family, fam, sizeof(g_caret_fmt_family));
    g_caret_fmt_size = size;
    g_caret_fmt_bold = bold;
    g_caret_fmt_italic = italic;
}

/* ------------------------------------------------------------------ */
/* Utility testo / font                                                */
/* ------------------------------------------------------------------ */

static void caret_utf8_slice(const char *s, int from, int to, char *out, size_t cap)
{
    int n, len;
    if (!s || cap == 0) {
        if (cap) out[0] = '\0';
        return;
    }
    len = (int)strlen(s);
    if (from < 0) from = 0;
    if (from > len) from = len;
    if (to < from) to = from;
    if (to > len) to = len;
    n = to - from;
    if ((size_t)n >= cap) n = (int)cap - 1;
    memcpy(out, s + from, (size_t)n);
    out[n] = '\0';
}

/* Converte un indice UTF-16 (DWrite) in offset byte UTF-8. */
static int caret_utf16_to_utf8(const WCHAR *ws, int wlen, int index)
{
    int i, bytes = 0;
    for (i = 0; i < index && i < wlen; i++) {
        WCHAR c = ws[i];
        if (c >= 0xD800 && c <= 0xDBFF && (i + 1) < wlen &&
            ws[i + 1] >= 0xDC00 && ws[i + 1] <= 0xDFFF) {
            bytes += 4;
            i++;
        } else if (c < 0x80) {
            bytes += 1;
        } else if (c < 0x800) {
            bytes += 2;
        } else {
            bytes += 3;
        }
    }
    return bytes;
}

/* Offset byte dentro il testo del run corrispondente a doc_x. */
static int caret_offset_in_run(const TextRun *r, int doc_x)
{
    IDWriteTextLayout *layout = NULL;
    DWRITE_HIT_TEST_METRICS hm;
    WINBOOL trailing = 0, inside = 0;
    WCHAR *ws;
    int wlen, utf8_off = 0;
    HRESULT hr;

    caret_ensure_format(r);
    if (!g_caret_format) return 0;
    ws = u8_to_w(r->text ? r->text : "");
    if (!ws) return 0;
    wlen = (int)lstrlenW(ws);
    hr = IDWriteFactory_CreateTextLayout(g_dwrite_factory, ws, wlen, g_caret_format,
                                         (FLOAT)(r->w > 0 ? r->w : 10000),
                                         (FLOAT)(r->h > 0 ? r->h : 1000),
                                         &layout);
    if (FAILED(hr) || !layout) {
        free(ws);
        return 0;
    }
    IDWriteTextLayout_SetWordWrapping(layout, DWRITE_WORD_WRAPPING_NO_WRAP);
    ZeroMemory(&hm, sizeof(hm));
    IDWriteTextLayout_HitTestPoint(layout,
                                   (FLOAT)(doc_x - r->x),
                                   (FLOAT)(r->h > 0 ? r->h / 2 : 8),
                                   &trailing, &inside, &hm);
    /* Il layout DirectWrite referenzia ws: va liberato solo dopo Release. */
    utf8_off = caret_utf16_to_utf8(ws, wlen, (int)hm.textPosition);
    IDWriteTextLayout_Release(layout);
    free(ws);
    if (utf8_off < 0) utf8_off = 0;
    if (r->text && utf8_off > (int)strlen(r->text))
        utf8_off = (int)strlen(r->text);
    return utf8_off;
}

/* Larghezza in pixel del prefisso [0, byte_count) del testo del run. */
static int caret_prefix_width(const TextRun *r, int byte_count, const char *text)
{
    IDWriteTextLayout *layout = NULL;
    DWRITE_TEXT_METRICS metrics;
    char buf[512];
    WCHAR *ws;
    int w = 0;
    HRESULT hr;

    caret_ensure_format(r);
    if (!g_caret_format || byte_count <= 0 || !text) return 0;
    caret_utf8_slice(text, 0, byte_count, buf, sizeof(buf));
    if (!buf[0]) return 0;
    ws = u8_to_w(buf);
    if (!ws) return 0;
    hr = IDWriteFactory_CreateTextLayout(g_dwrite_factory, ws, lstrlenW(ws),
                                         g_caret_format, 10000.0f, 10000.0f, &layout);
    if (SUCCEEDED(hr) && layout) {
        if (SUCCEEDED(IDWriteTextLayout_GetMetrics(layout, &metrics)))
            w = (int)(metrics.widthIncludingTrailingWhitespace + 0.5f);
        IDWriteTextLayout_Release(layout);
    }
    free(ws);
    return w > 0 ? w : 0;
}

/* Associa i run del paragrafo agli offset nel testo plain. */
static void caret_map_runs(LayoutParagraph *p, const char *text)
{
    TextRun *r;
    int cursor = 0;
    int len = text ? (int)strlen(text) : 0;

    if (!p) return;
    for (r = p->runs; r; r = r->next) {
        r->char_start = -1;
        r->char_end = -1;
        if (!r->text || !r->text[0]) continue;
        {
            int n = (int)strlen(r->text);
            const char *found = NULL;
            if (cursor <= len) found = strstr(text + cursor, r->text);
            if (found) {
                r->char_start = (int)(found - text);
                r->char_end = r->char_start + n;
            } else {
                r->char_start = cursor;
                r->char_end = cursor + n;
                if (r->char_end > len) r->char_end = len;
            }
            cursor = r->char_end;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Posizione del caret                                                 */
/* ------------------------------------------------------------------ */

static void caret_set_from_run(const TextRun *r, int offset_in_para)
{
    int base = r->char_start >= 0 ? r->char_start : 0;
    int local = offset_in_para - base;
    if (local < 0) local = 0;

    g_caret_run = *r;
    g_caret_rc.left = r->x + caret_prefix_width(r, local, r->text ? r->text : "");
    g_caret_rc.top = r->y;
    g_caret_rc.right = g_caret_rc.left + 1;
    g_caret_rc.bottom = r->y + (r->h > 0 ? r->h : 20);
}

static void caret_update_pos_from_text(void)
{
    int base = g_caret_run.char_start >= 0 ? g_caret_run.char_start : 0;
    int local = g_caret_offset - base;
    if (local < 0) local = 0;
    g_caret_rc.left = g_caret_run.x +
        caret_prefix_width(&g_caret_run, local, g_caret_text ? g_caret_text : "");
    g_caret_rc.right = g_caret_rc.left + 1;
}

static void caret_schedule_commit(void)
{
    if (!g_hwnd) return;
    if (g_caret_timer) KillTimer(g_hwnd, LD_CARET_TIMER_ID);
    g_caret_timer = 1;
    SetTimer(g_hwnd, LD_CARET_TIMER_ID, LD_CARET_DEBOUNCE_MS, NULL);
}

int caret_begin_from_point(int doc_x, int doc_y)
{
    const LayoutParagraph *para = NULL;
    const TextRun *run;
    char *text;
    int run_off, offset;

    DLOG("[CARET] begin doc=(%d,%d) cache=%p valid=%d state=%d busy=%d load=%d\n",
            doc_x, doc_y, (void*)g_layout_cache,
            g_layout_cache ? g_layout_cache->valid : -1,
            g_doc_state, g_render_busy, g_load_active);
    if (!g_doc || !g_layout_cache || !g_layout_cache->valid)
        return 0;
    if (g_doc_state != DOC_STATE_IDLE || g_render_busy || g_load_active)
        return 0;

    run = layout_cache_find_run_at(g_layout_cache, doc_x, doc_y, &para);
    DLOG("[CARET] find_run_at -> run=%p para=%p\n", (void*)run, (void*)para);
    if (!run || !para || !run->text || !run->text[0])
        return 0;

    text = ld_paragraph_get_text(g_doc, para->id);
    if (!text) return 0;

    caret_map_runs((LayoutParagraph *)para, text);
    caret_close();

    g_caret_text = _strdup(text);
    g_caret_original = _strdup(text);
    if (!g_caret_text || !g_caret_original) {
        caret_close();
        ld_free_string(text);
        return 0;
    }

    g_caret_para = para->id;
    g_caret_gen = g_layout_cache->generation;
    run_off = caret_offset_in_run(run, doc_x);
    offset = run->char_start >= 0 ? run->char_start + run_off : 0;
    if (offset < 0) offset = 0;
    if (offset > (int)strlen(g_caret_text))
        offset = (int)strlen(g_caret_text);
    g_caret_offset = offset;
    caret_set_from_run(run, offset);
    g_caret_active = 1;
    g_caret_timer = 0;
    g_caret_reanchor = 0;

    ld_free_string(text);
    DLOG("[CARET] opened para=%d offset=%d run_box=(%d,%d %dx%d) caret_rc=(%ld,%ld)\n",
            g_caret_para, g_caret_offset,
            g_caret_run.x, g_caret_run.y, g_caret_run.w, g_caret_run.h,
            g_caret_rc.left, g_caret_rc.top);
    InvalidateRect(g_hwnd, NULL, FALSE);
    return 1;
}

void caret_close(void)
{
    int was_active = g_caret_active;

    if (g_caret_timer && g_hwnd) {
        KillTimer(g_hwnd, LD_CARET_TIMER_ID);
    }
    free(g_caret_text);
    free(g_caret_original);
    g_caret_text = NULL;
    g_caret_original = NULL;
    g_caret_active = 0;
    g_caret_para = -1;
    g_caret_offset = 0;
    g_caret_gen = -1;
    g_caret_timer = 0;
    g_caret_reanchor = 0;
    ZeroMemory(&g_caret_run, sizeof(g_caret_run));
    ZeroMemory(&g_caret_rc, sizeof(g_caret_rc));
    caret_release_format();
    if (was_active && g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

void caret_on_zoom_or_scroll(void)
{
    if (g_caret_active && g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Modifica del testo                                                  */
/* ------------------------------------------------------------------ */

static int caret_prev_char_start(const char *s, int off)
{
    int i = off - 1;
    if (!s || off <= 0) return 0;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static int caret_next_char_end(const char *s, int off, int len)
{
    int i = off + 1;
    if (!s || off >= len) return len;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i < len ? i : len;
}

static void caret_replace_range(int from, int to, const char *insert)
{
    int len, ins, newlen;
    char *buf;

    if (!g_caret_text) return;
    len = (int)strlen(g_caret_text);
    if (from < 0) from = 0;
    if (to < from) to = from;
    if (to > len) to = len;
    ins = insert ? (int)strlen(insert) : 0;
    newlen = len - (to - from) + ins;
    buf = (char *)malloc((size_t)newlen + 1);
    if (!buf) return;
    if (from > 0) memcpy(buf, g_caret_text, (size_t)from);
    if (ins > 0) memcpy(buf + from, insert, (size_t)ins);
    if (to < len) memcpy(buf + from + ins, g_caret_text + to, (size_t)(len - to));
    buf[newlen] = '\0';
    free(g_caret_text);
    g_caret_text = buf;

    g_caret_offset = from + ins;
    if (g_caret_offset < g_caret_run.char_start)
        g_caret_run.char_start = g_caret_offset;
    if (g_caret_offset > g_caret_run.char_end)
        g_caret_run.char_end = g_caret_offset;
}

void caret_on_char(const char *utf8)
{
    if (!g_caret_active || !g_caret_text || !utf8 || !utf8[0]) return;
    DLOG("[CARET] on_char '%s' offset=%d len=%d\n", utf8, g_caret_offset, (int)strlen(g_caret_text));
    caret_replace_range(g_caret_offset, g_caret_offset, utf8);
    caret_update_pos_from_text();
    caret_schedule_commit();
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* WM_CHAR: un carattere UTF-16 (con surrogate pair) -> UTF-8. */
void caret_on_wchar(WCHAR ch)
{
    static WCHAR high_surrogate = 0;
    char utf8[5];
    int n = 0;

    if (!g_caret_active) { high_surrogate = 0; return; }
    /* I caratteri di controllo (Backspace, Enter, Esc, ...) sono gestiti in
     * WM_KEYDOWN: qui si accettano solo caratteri stampabili e il tab. */
    if (ch < 0x20 && ch != L'\t') { high_surrogate = 0; return; }
    if (ch == 0x7F) { high_surrogate = 0; return; }
    if (ch >= 0xD800 && ch <= 0xDBFF) {
        high_surrogate = ch;
        return;
    }
    if (ch >= 0xDC00 && ch <= 0xDFFF) {
        unsigned long cp;
        if (!high_surrogate) return;
        cp = 0x10000UL +
             (((unsigned long)(high_surrogate - 0xD800)) << 10) +
             (unsigned long)(ch - 0xDC00);
        high_surrogate = 0;
        utf8[0] = (char)(0xF0 | (cp >> 18));
        utf8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        utf8[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    } else {
        high_surrogate = 0;
        if (ch < 0x80) {
            utf8[0] = (char)ch;
            n = 1;
        } else if (ch < 0x800) {
            utf8[0] = (char)(0xC0 | (ch >> 6));
            utf8[1] = (char)(0x80 | (ch & 0x3F));
            n = 2;
        } else {
            utf8[0] = (char)(0xE0 | (ch >> 12));
            utf8[1] = (char)(0x80 | ((ch >> 6) & 0x3F));
            utf8[2] = (char)(0x80 | (ch & 0x3F));
            n = 3;
        }
    }
    utf8[n] = '\0';
    caret_on_char(utf8);
}

void caret_on_key(int vk)
{
    int len, from, to;

    if (!g_caret_active || !g_caret_text) return;
    len = (int)strlen(g_caret_text);

    switch (vk) {
    case VK_BACK:
        if (g_caret_offset <= 0) return;
        from = caret_prev_char_start(g_caret_text, g_caret_offset);
        caret_replace_range(from, g_caret_offset, NULL);
        caret_update_pos_from_text();
        caret_schedule_commit();
        break;
    case VK_DELETE:
        if (g_caret_offset >= len) return;
        to = caret_next_char_end(g_caret_text, g_caret_offset, len);
        caret_replace_range(g_caret_offset, to, NULL);
        caret_update_pos_from_text();
        caret_schedule_commit();
        break;
    case VK_ESCAPE:
        /* Nessun commit e' stato fatto: basta chiudere e scartare il buffer. */
        caret_close();
        return;
    case VK_LEFT:
        if (g_caret_offset > 0)
            g_caret_offset = caret_prev_char_start(g_caret_text, g_caret_offset);
        caret_update_pos_from_text();
        break;
    case VK_RIGHT:
        if (g_caret_offset < len)
            g_caret_offset = caret_next_char_end(g_caret_text, g_caret_offset, len);
        caret_update_pos_from_text();
        break;
    default:
        return;
    }
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

void caret_on_timer(void)
{
    DLOG("[CARET] timer fired active=%d timer=%d\n", g_caret_active, g_caret_timer);
    if (!g_caret_active || !g_caret_timer) return;
    g_caret_timer = 0;
    if (g_hwnd) KillTimer(g_hwnd, LD_CARET_TIMER_ID);

    if (!g_caret_text || !g_caret_original ||
        strcmp(g_caret_text, g_caret_original) == 0) {
        caret_close();
        return;
    }
    /* Se il documento e' occupato, riprova tra poco senza perdere il testo. */
    if (!g_doc || g_doc_state != DOC_STATE_IDLE || g_render_busy || g_load_active) {
        caret_schedule_commit();
        return;
    }
    if (!g_worker_started) {
        caret_close();
        return;
    }

    DLOG("[CARET] commit para=%d len=%d text='%.40s'\n",
            g_caret_para, (int)strlen(g_caret_text), g_caret_text);
    /* Il documento litehtml ha affinita' di thread col worker: la modifica
     * va accodata come job, NON chiamata dal thread UI (crash). */
    g_caret_reanchor = 1;
    request_caret_apply(g_caret_para, g_caret_text);
    /* Il caret resta "attivo" ma invisibile finche' non arriva il re-anchor. */
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

void caret_reanchor_after_render(void)
{
    LayoutParagraph *para = NULL;
    TextRun *r = NULL;

    if (!g_caret_reanchor) return;
    /* Se un altro job e' gia' partito (render o apply), la cache puo' essere
     * invalidata mentre la leggiamo: rimanda al prossimo RENDER_DONE. */
    if (g_render_busy || g_doc_state != DOC_STATE_IDLE) return;
    g_caret_reanchor = 0;
    if (!g_caret_active || !g_layout_cache || !g_layout_cache->valid) {
        caret_close();
        return;
    }
    {
        LayoutParagraph *p = g_layout_cache->root ? g_layout_cache->root->first_child : NULL;
        while (p && p->id != g_caret_para) p = p->next_sibling;
        para = p;
    }
    if (!para) {
        caret_close();
        return;
    }
    caret_map_runs(para, g_caret_text);
    for (r = para->runs; r; r = r->next) {
        if (r->char_start >= 0 && r->char_end > r->char_start &&
            g_caret_offset >= r->char_start && g_caret_offset <= r->char_end)
            break;
    }
    if (!r) {
        caret_close();
        return;
    }
    g_caret_offset = g_caret_offset < r->char_start ? r->char_start : g_caret_offset;
    if (g_caret_offset > r->char_end) g_caret_offset = r->char_end;
    caret_set_from_run(r, g_caret_offset);
    free(g_caret_original);
    g_caret_original = _strdup(g_caret_text);
    g_caret_gen = g_layout_cache->generation;
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Overlay                                                            */
/* ------------------------------------------------------------------ */

void caret_draw(HWND hwnd, HDC hdc)
{
    RECT rc_content, rc_run, rc_caret;
    char slice[512];
    WCHAR *wide, *wfamily;
    HFONT hfont, old_font;
    COLORREF old_text, old_bk;
    int x, y, h, size;
    int run_start, run_end;

    (void)hwnd;
    if (!g_caret_active || g_caret_reanchor || g_render_busy) return;
    if (!hdc || !g_caret_text) return;
    DLOG("[CARET] draw active=%d offset=%d rc=(%ld,%ld)\n",
            g_caret_active, g_caret_offset, g_caret_rc.left, g_caret_rc.top);

    layout_content_rect(g_doc_h, &rc_content);
    x = (int)((rc_content.left + g_caret_rc.left) * g_zoom);
    y = (int)((rc_content.top + g_caret_rc.top) * g_zoom);
    h = (int)((g_caret_rc.bottom - g_caret_rc.top) * g_zoom);
    if (h < 8) h = (int)(20 * g_zoom);

    rc_caret.left = x;
    rc_caret.top = y;
    rc_caret.right = x + 2;
    rc_caret.bottom = y + h;
    FillRect(hdc, &rc_caret, (HBRUSH)GetStockObject(BLACK_BRUSH));

    /* Anteprima del run modificato: si ridisegna solo la porzione di testo
     * del run, non l'intero paragrafo. */
    run_start = g_caret_run.char_start >= 0 ? g_caret_run.char_start : g_caret_offset;
    run_end = g_caret_run.char_end > run_start ? g_caret_run.char_end : g_caret_offset;
    caret_utf8_slice(g_caret_text, run_start, run_end, slice, sizeof(slice));
    if (!slice[0]) return;

    wide = u8_to_w(slice);
    if (!wide) return;
    wfamily = u8_to_w(g_caret_run.family[0] ? g_caret_run.family : "Aptos");
    if (!wfamily) { free(wide); return; }

    size = g_caret_run.font_size > 0 ? g_caret_run.font_size : 12;
    hfont = CreateFontW(-size, 0, 0, 0,
                       g_caret_run.bold ? FW_BOLD : FW_NORMAL,
                       g_caret_run.italic ? TRUE : FALSE,
                       g_caret_run.underline ? TRUE : FALSE,
                       g_caret_run.strike ? TRUE : FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       wfamily);
    free(wfamily);
    old_font = (HFONT)SelectObject(hdc, hfont);
    old_text = SetTextColor(hdc, g_caret_run.color);
    old_bk = SetBkMode(hdc, TRANSPARENT);

    rc_run.left = (int)((rc_content.left + g_caret_run.x) * g_zoom);
    rc_run.top = (int)((rc_content.top + g_caret_run.y) * g_zoom);
    {
        /* Cancella solo l'area occupata dal run (o dalla preview, se piu'
         * lunga) prima di ridisegnare il testo modificato. */
        int preview_w = caret_prefix_width(&g_caret_run, run_end - run_start, slice);
        int clear_w = g_caret_run.w > preview_w ? g_caret_run.w : preview_w;
        rc_run.right = rc_run.left + (int)(clear_w * g_zoom) + 4;
    }
    rc_run.bottom = rc_run.top + h;
    FillRect(hdc, &rc_run, (HBRUSH)GetStockObject(WHITE_BRUSH));
    DrawTextW(hdc, wide, (int)lstrlenW(wide), &rc_run,
              DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);

    SetBkMode(hdc, old_bk);
    SetTextColor(hdc, old_text);
    SelectObject(hdc, old_font);
    if (hfont) DeleteObject(hfont);
    free(wide);

    /* Il caret va ridisegnato dopo la preview: il FillRect bianco del run
     * potrebbe averlo coperto. */
    FillRect(hdc, &rc_caret, (HBRUSH)GetStockObject(BLACK_BRUSH));
}
