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
                                      IDWriteFontFace *font_face,
                                      float advance_scale,
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
    e->font_face = font_face;
    e->advance_scale = advance_scale;
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

    DLOG("[FONT_CREATE] fam='%s' size=%d bold=%d italic=%d underline=%d strike=%d\n",
            family, size, bold, italic, underline, strike);

    /* Check cache first */
    cached = font_cache_find(family, size, bold, italic, underline, strike);
    if (cached) {
        format = cached->format;
        IDWriteTextFormat_AddRef(format);
        metrics = cached->metrics;
        x_height = cached->x_height;
        DLOG("[FONT_CREATE] cache hit for '%s' %dpx\n", family, size);
    } else {
        wfam = u8_to_w(family);
        if (!wfam) {
            DLOG("[FONT_CREATE] utf8_to_wide failed\n");
            return NULL;
        }

        if (!g_dwrite_factory) {
            DLOG("[FONT_CREATE] no DWrite factory\n");
            free(wfam);
            return NULL;
        }

        hr = IDWriteFactory_CreateTextFormat(g_dwrite_factory, wfam, NULL,
            weight, style, stretch, px_size, L"en-US", &format);

        if (FAILED(hr) || !format) {
            DLOG("[FONT_CREATE] CreateTextFormat failed hr=0x%08lX\n", (unsigned long)hr);
            free(wfam);
            return NULL;
        }

        /* Obtain IDWriteFontFace for fast per-glyph width measurement */
        IDWriteFontFace *font_face = NULL;
        float advance_scale = 0.0f;
        {
            IDWriteFontCollection *collection = NULL;
            if (SUCCEEDED(IDWriteFactory_GetSystemFontCollection(g_dwrite_factory, &collection, FALSE)) && collection) {
                UINT32 fam_idx = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(IDWriteFontCollection_FindFamilyName(collection, wfam, &fam_idx, &exists)) && exists) {
                    IDWriteFontFamily *dw_fam = NULL;
                    if (SUCCEEDED(IDWriteFontCollection_GetFontFamily(collection, fam_idx, &dw_fam)) && dw_fam) {
                        IDWriteFont *dw_font = NULL;
                        if (SUCCEEDED(IDWriteFontFamily_GetFirstMatchingFont(dw_fam, weight, stretch, style, &dw_font)) && dw_font) {
                            if (SUCCEEDED(IDWriteFont_CreateFontFace(dw_font, &font_face)) && font_face) {
                                DWRITE_FONT_METRICS face_metrics;
                                IDWriteFontFace_GetMetrics(font_face, &face_metrics);
                                if (face_metrics.designUnitsPerEm > 0) {
                                    advance_scale = px_size / (float)face_metrics.designUnitsPerEm;
                                }
                            }
                            IDWriteFont_Release(dw_font);
                        }
                        IDWriteFontFamily_Release(dw_fam);
                    }
                }
                IDWriteFontCollection_Release(collection);
            }
        }
        free(wfam);
        wfam = NULL;

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
                                 format, font_face, advance_scale, &metrics, x_height);
        if (!cached) {
            if (font_face) IDWriteFontFace_Release(font_face);
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
    handle->cache_entry = cached;
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
        fm->ch_width = (int)(px_size * 0.6f);  /* approximate average char width */
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

#if TIMING_LOGS
static int g_tw_fast_ok = 0;
static int g_tw_fallback = 0;
static int g_tw_no_face = 0;
#endif

static int cb_text_width(const char *text, void *font)
{
    DwFontHandle *handle = (DwFontHandle *)font;
    FontCacheEntry *ce;
    WCHAR *ws;
    IDWriteTextLayout *layout = NULL;
    DWRITE_TEXT_METRICS metrics;
    int width = 0;
    HRESULT hr;

    if (!text || !text[0] || !handle || !handle->format)
        return 0;

    /* Fast path: sum cached per-codepoint glyph advances via IDWriteFontFace
     * for ASCII / Latin-1 runs, avoiding IDWriteTextLayout COM allocations. */
    ce = handle->cache_entry;
    if (ce && ce->font_face && ce->advance_scale > 0.0f) {
        const unsigned char *p = (const unsigned char *)text;
        float total_w = 0.0f;
        int fast_ok = 1;
        while (*p) {
            UINT32 cp;
            if (p[0] < 0x80) {
                cp = p[0];
                p += 1;
            } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
                cp = ((UINT32)(p[0] & 0x1F) << 6) | (UINT32)(p[1] & 0x3F);
                if (cp >= 256) { fast_ok = 0; break; }
                p += 2;
            } else {
                fast_ok = 0;
                break;
            }
            if (cp < 0x20) { fast_ok = 0; break; }
            if (ce->char_cached[cp] == 0) {
                UINT16 glyph_idx = 0;
                if (SUCCEEDED(IDWriteFontFace_GetGlyphIndices(ce->font_face, &cp, 1, &glyph_idx)) && glyph_idx != 0) {
                    DWRITE_GLYPH_METRICS gm;
                    if (SUCCEEDED(IDWriteFontFace_GetDesignGlyphMetrics(ce->font_face, &glyph_idx, 1, &gm, FALSE))) {
                        ce->char_advances[cp] = (float)gm.advanceWidth * ce->advance_scale;
                        ce->char_cached[cp] = 1;
                    } else {
                        ce->char_cached[cp] = 2;
                    }
                } else {
                    ce->char_cached[cp] = 2;
                }
            }
            if (ce->char_cached[cp] != 1) {
                fast_ok = 0;
                break;
            }
            total_w += ce->char_advances[cp];
        }
        if (fast_ok) {
#if TIMING_LOGS
            g_tw_fast_ok++;
#endif
            return (int)(total_w + 0.5f);
        }
#if TIMING_LOGS
        g_tw_fallback++;
    } else {
        g_tw_no_face++;
#endif
    }

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