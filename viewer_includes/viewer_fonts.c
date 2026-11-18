/*
 * viewer_fonts.c - cache dei font DirectWrite e callback di creazione/testo.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

static FontCacheEntry *font_cache_find(const char *family, int size, int bold,
                                       int italic, int underline, int strike)
{
    FontCacheEntry *e = g_font_cache;
    while (e) {
        if (e->size == size && e->bold == bold && e->italic == italic &&
            e->underline == underline && e->strike == strike &&
            strcmp(e->family, family) == 0) {
            return e;
        }
        e = e->next;
    }
    return NULL;
}

static FontCacheEntry *font_cache_add(const char *family, int size, int bold,
                                      int italic, int underline, int strike,
                                      IDWriteTextFormat *format,
                                      const DWRITE_FONT_METRICS *metrics,
                                      float x_height)
{
    FontCacheEntry *e = (FontCacheEntry *)calloc(1, sizeof(FontCacheEntry));
    if (!e) return NULL;
    
    e->family = _strdup(family);
    if (!e->family) {
        free(e);
        return NULL;
    }
    e->size = size;
    e->bold = bold;
    e->italic = italic;
    e->underline = underline;
    e->strike = strike;
    e->format = format;
    e->metrics = *metrics;
    e->x_height = x_height;
    e->next = g_font_cache;
    g_font_cache = e;
    return e;
}

static void *cb_create_font(const char *fam, int size, int bold, int italic,
                            int underline, int strike, lh_font_metrics *fm)
{
    WCHAR *wfam = NULL;
    const char *family = fam && fam[0] ? fam : "Aptos";
    IDWriteTextFormat *format = NULL;
    DWRITE_FONT_METRICS metrics;
    HRESULT hr;
    FontCacheEntry *cached;
    DwFontHandle *handle;
    float x_height = 0.0f;
    DWRITE_FONT_WEIGHT weight = bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style = italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
    float px_size = (float)size;

    if (size < 6) size = 6;
    px_size = (float)size;

    // fprintf(stderr, "[FONT_CREATE] fam='%s' size=%d bold=%d italic=%d underline=%d strike=%d\n",
    //         family, size, bold, italic, underline, strike);

    /* Check cache first */
    cached = font_cache_find(family, size, bold, italic, underline, strike);
    if (cached) {
        format = cached->format;
        IDWriteTextFormat_AddRef(format);
        metrics = cached->metrics;
        x_height = cached->x_height;
        // fprintf(stderr, "[FONT_CREATE] cache hit for '%s' %dpx\n", family, size);
    } else {
        wfam = u8_to_w(family);
        if (!wfam) {
            // fprintf(stderr, "[FONT_CREATE] utf8_to_wide failed\n");
            return NULL;
        }

        if (!g_dwrite_factory) {
            // fprintf(stderr, "[FONT_CREATE] no DWrite factory\n");
            free(wfam);
            return NULL;
        }

        /* Use user default locale for proper font fallback */
        hr = IDWriteFactory_CreateTextFormat(g_dwrite_factory, wfam, NULL,
            weight, style, stretch, px_size, L"", &format);
        free(wfam);
        wfam = NULL;

        if (FAILED(hr) || !format) {
            // fprintf(stderr, "[FONT_CREATE] CreateTextFormat failed hr=0x%08lX\n", (unsigned long)hr);
            return NULL;
        }

        /* Get font metrics: IDWriteTextFormat doesn't expose GetFontFace in this MinGW version.
         * Use fallback metrics based on font size. */
        {
            /* Fallback metrics based on typical font proportions */
            ZeroMemory(&metrics, sizeof(metrics));
            metrics.designUnitsPerEm = 2048;
            metrics.ascent = (UINT16)(2048 * 0.8f);
            metrics.descent = (UINT16)(2048 * 0.2f);
            metrics.lineGap = (UINT16)(2048 * 0.15f);
            x_height = (float)metrics.ascent * 0.5f;
        }

        /* Cache the font */
        cached = font_cache_add(family, size, bold, italic, underline, strike,
                                 format, &metrics, x_height);
        if (!cached) {
            IDWriteTextFormat_Release(format);
            return NULL;
        }
    }

    /* Create the opaque handle returned to the layout/DLL */
    handle = (DwFontHandle *)calloc(1, sizeof(DwFontHandle));
    if (!handle) {
        IDWriteTextFormat_Release(format);
        return NULL;
    }
    
    handle->format = format;
    handle->metrics = metrics;
    handle->x_height = x_height;
    handle->underline = underline;
    handle->strike = strike;
    handle->size_px = size;
    
    /* Convert design units to pixels */
    if (metrics.designUnitsPerEm > 0) {
        float scale = px_size / (float)metrics.designUnitsPerEm;
        handle->ascent = metrics.ascent * scale;
        handle->descent = metrics.descent * scale;
        handle->line_gap = metrics.lineGap * scale;
    } else {
        handle->ascent = px_size * 0.8f;
        handle->descent = px_size * 0.2f;
        handle->line_gap = px_size * 0.15f;
    }

    if (fm) {
        /* height = ascent + descent + line_gap (similar to tmHeight) */
        float height_f = handle->ascent + handle->descent + handle->line_gap;
        fm->font_size   = size;
        fm->height = (int)(height_f * 1.15f);  /* Word-like 1.15 line spacing */
        fm->ascent = (int)(handle->ascent + 0.5f);
        fm->descent = (int)(handle->descent + 0.5f);
        fm->x_height = (int)(x_height + 0.5f);
        //fm->ch_width = (int)(px_size * 0.6f);  /* approximate average char width */
        fm->ch_width = (int)(px_size * 0.55f);  /* FIX char_w CARET: calibrated to match the real glyph advance */
        fm->draw_spaces = (italic || underline || strike) ? 1 : 0;
        fm->sub_shift   = size / 5;
        fm->super_shift = size / 3;
    }

    return handle;
}


static void cb_delete_font(void *font)
{
    if (font) {
        DwFontHandle *handle = (DwFontHandle *)font;
        if (handle->format) {
            IDWriteTextFormat_Release(handle->format);
        }
        free(handle);
    }
}

static int cb_text_width(const char *text, void *font)
{
    DwFontHandle *handle = (DwFontHandle *)font;
    WCHAR *ws;
    IDWriteTextLayout *layout = NULL;
    DWRITE_TEXT_METRICS metrics;
    int width = 0;
    HRESULT hr;

    if (!text || !text[0] || !handle || !handle->format)
        return 0;

    ws = u8_to_w(text);
    if (!ws)
        return 0;

    hr = IDWriteFactory_CreateTextLayout(g_dwrite_factory, ws, lstrlenW(ws),
        handle->format, 10000.0f, 10000.0f, &layout);
    free(ws);
    
    if (SUCCEEDED(hr) && layout) {
        hr = IDWriteTextLayout_GetMetrics(layout, &metrics);
        if (SUCCEEDED(hr)) {
            width = (int)(metrics.widthIncludingTrailingWhitespace + 0.5f);
        }
        IDWriteTextLayout_Release(layout);
    }

    return width;
}
