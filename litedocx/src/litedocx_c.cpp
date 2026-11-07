/*
 * litedocx_c.cpp - implementation of the litedocx C ABI.
 *
 * The whole OOXML -> litehtml conversion happens here and in the other
 * litedocx translation units.  The C caller only receives an opaque handle.
 * No HTML string is ever created.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wincodec.h>

#include <litehtml.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "docx_builder.h"
#include "docx_model.h"
#include "docx_package.h"
#include "litedocx_c.h"

#ifndef LITEDOCX_DEBUG
#define LITEDOCX_DEBUG 0
#endif

#if LITEDOCX_DEBUG
#define LDBG(msg) do { fprintf(stderr, "%s", msg); fflush(stderr); } while(0)
#else
#define LDBG(msg) ((void)0)
#endif

// Logging per debug Fase D (sempre abilitato)
#define LDLOG(fmt, ...) do { fprintf(stderr, "[LITEDOCX] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

namespace
{
    std::string& last_error()
    {
        static thread_local std::string err;
        return err;
    }

    void set_error(const std::string& e)
    {
        last_error() = e;
    }

    unsigned to_cref(const litehtml::web_color& c)
    {
        return static_cast<unsigned>(c.red) | (static_cast<unsigned>(c.green) << 8) |
               (static_cast<unsigned>(c.blue) << 16);
    }

    /* Convert a UTF-8 null-terminated string to a heap-allocated UTF-16LE buffer.
     * The caller must free() the returned pointer.  Returns NULL on invalid input.
     * Length is in WCHAR characters, not bytes. */
    static WCHAR* u8_to_w16(const char* s, size_t* out_len)
    {
        if (!s) s = "";
        size_t n = strlen(s);
        size_t cap = n + 1;          /* worst-case: 1 source byte → 1 WCHAR */
        WCHAR* out = (WCHAR*)malloc(cap * sizeof(WCHAR));
        if (!out) return NULL;
        size_t i = 0, o = 0;
        while (i < n) {
            unsigned cp;
            unsigned char c = (unsigned char)s[i++];
            if (c < 0x80) {
                cp = c;
            } else if ((c & 0xE0) == 0xC0 && i < n) {
                cp = ((c & 0x1F) << 6) | ((unsigned char)s[i++] & 0x3F);
            } else if ((c & 0xF0) == 0xE0 && i + 1 < n) {
                cp = ((c & 0x0F) << 12) | ((unsigned char)s[i++] & 0x3F) << 6 |
                     ((unsigned char)s[i++] & 0x3F);
            } else if ((c & 0xF8) == 0xF0 && i + 2 < n) {
                cp = ((c & 0x07) << 18) | ((unsigned char)s[i++] & 0x3F) << 12 |
                     ((unsigned char)s[i++] & 0x3F) << 6 |
                     ((unsigned char)s[i++] & 0x3F);
            } else {
                cp = '?';
            }
            if (o + 2 > cap) {
                /* should never happen with the pre-allocated buffer, but guard anyway */
                free(out);
                return NULL;
            }
            if (cp >= 0x10000) {
                cp -= 0x10000;
                out[o++] = (WCHAR)(0xD800 + (cp >> 10));
                out[o++] = (WCHAR)(0xDC00 + (cp & 0x3FF));
            } else {
                out[o++] = (WCHAR)cp;
            }
        }
        out[o] = 0;
        if (out_len) *out_len = o;
        return out;
    }

    /* LRU Cache for UTF-8 to UTF-16 conversion. */
    class Utf8ToUtf16Cache
    {
    public:
        explicit Utf8ToUtf16Cache(size_t max_entries = 256) : m_max_entries(max_entries) {}

        // Get cached or convert and cache. Returns owned WCHAR* that must be freed by caller,
        // or nullptr on failure. The cache owns the memory and will free it on eviction.
        const WCHAR* get_or_convert(const char* s, size_t* out_len)
        {
            if (!s) s = "";
            std::string key(s);

            auto it = m_map.find(key);
            if (it != m_map.end()) {
                // Move to front (most recently used)
                m_list.splice(m_list.begin(), m_list, it->second);
                if (out_len) *out_len = it->second->second.second;
                return it->second->second.first;
            }

            // Convert
            size_t len = 0;
            WCHAR* w = u8_to_w16(s, &len);
            if (!w) return nullptr;

            // Evict if needed
            if (m_list.size() >= m_max_entries) {
                auto& last = m_list.back();
                m_map.erase(last.first);
                free(last.second.first);
                m_list.pop_back();
            }

            // Add to front
            m_list.emplace_front(std::move(key), std::make_pair(w, len));
            m_map[m_list.front().first] = m_list.begin();
            if (out_len) *out_len = len;
            return w;
        }

        void clear()
        {
            for (auto& entry : m_list) {
                free(entry.second.first);
            }
            m_list.clear();
            m_map.clear();
        }

    private:
        size_t m_max_entries;
        // list of (key, (wchar_ptr, length)) - front = MRU, back = LRU
        std::list<std::pair<std::string, std::pair<WCHAR*, size_t>>> m_list;
        std::unordered_map<std::string, decltype(m_list)::iterator> m_map;
    };

    /* Cache per-thread: il rendering può avvenire sul worker mentre il thread
     * UI disegna il documento precedente. Una cache globale non sincronizzata
     * sarebbe una data race. */
    static Utf8ToUtf16Cache& utf_cache()
    {
        static thread_local Utf8ToUtf16Cache cache;
        return cache;
    }

    struct CachedImage
    {
        int     w   = 0;
        int     h   = 0;
        HBITMAP bmp = nullptr;
        void*   host = nullptr;

        ~CachedImage()
        {
            if(bmp)
            {
                DeleteObject(bmp);
            }
        }
    };

    /* Table cell info for custom rendering callbacks. */
    struct TableCellInfo
    {
        int         table_index = 0;
        int         row_index = 0;
        int         col_index = 0;
        int         row_span = 1;
        int         col_span = 1;
        int         is_header = 0;
        int         x = 0, y = 0, w = 0, h = 0;
        unsigned    bg_color = 0;
        int         border_left_w = 0, border_top_w = 0, border_right_w = 0, border_bottom_w = 0;
        unsigned    border_left_c = 0, border_top_c = 0, border_right_c = 0, border_bottom_c = 0;
        int         padding_left = 0, padding_top = 0, padding_right = 0, padding_bottom = 0;
        std::string valign;
        
        /* Check if a point/rect overlaps this cell */
        bool contains(int px, int py, int pw, int ph) const
        {
            return px < x + w && px + pw > x && py < y + h && py + ph > y;
        }
    };

    bool wic_decode_image(IWICImagingFactory* factory, const void* data, size_t size, CachedImage& out)
    {
        if(!factory || !data || size == 0)
        {
            return false;
        }

        IWICStream* stream = nullptr;
        if(FAILED(factory->CreateStream(&stream)) || !stream)
        {
            return false;
        }

        bool ok = false;
        IWICBitmapDecoder*      decoder  = nullptr;
        IWICBitmapFrameDecode*  frame    = nullptr;
        IWICFormatConverter*    converter = nullptr;

        do
        {
            if(FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)),
                                                   static_cast<DWORD>(size))))
            {
                break;
            }
            if(FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
               !decoder)
            {
                break;
            }
            if(FAILED(decoder->GetFrame(0, &frame)) || !frame)
            {
                break;
            }

            UINT w = 0;
            UINT h = 0;
            if(FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > 16384 || h > 16384)
            {
                break;
            }

            if(FAILED(factory->CreateFormatConverter(&converter)) || !converter)
            {
                break;
            }
            if(FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                            WICBitmapPaletteTypeCustom)))
            {
                break;
            }

            BITMAPINFO bi = {};
            bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth       = static_cast<LONG>(w);
            bi.bmiHeader.biHeight      = -static_cast<LONG>(h);
            bi.bmiHeader.biPlanes      = 1;
            bi.bmiHeader.biBitCount    = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            HDC     screen = GetDC(nullptr);
            void*   bits   = nullptr;
            HBITMAP bmp    = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if(screen)
            {
                ReleaseDC(nullptr, screen);
            }
            if(!bmp || !bits)
            {
                if(bmp)
                {
                    DeleteObject(bmp);
                }
                break;
            }

            const UINT stride = w * 4;
            if(FAILED(converter->CopyPixels(nullptr, stride, stride * h, static_cast<BYTE*>(bits))))
            {
                DeleteObject(bmp);
                break;
            }

            out.w   = static_cast<int>(w);
            out.h   = static_cast<int>(h);
            out.bmp = bmp;
            ok      = true;
        } while(false);

        if(converter)
        {
            converter->Release();
        }
        if(frame)
        {
            frame->Release();
        }
        if(decoder)
        {
            decoder->Release();
        }
        if(stream)
        {
            stream->Release();
        }
        return ok;
    }

    /* LRU Cache for text width measurements: key = text + '|' + font_handle */
    class TextWidthCache
    {
    public:
        explicit TextWidthCache(size_t max_entries = 1024) : m_max_entries(max_entries) {}

        // Returns cached width or -1 if not cached
        int get(const char* text, litehtml::uint_ptr hFont) const
        {
            if (!text) return -1;
            std::string key = make_key(text, hFont);
            auto it = m_map.find(key);
            if (it != m_map.end()) {
                // Move to front (MRU)
                m_list.splice(m_list.begin(), m_list, it->second);
                return it->second->second;
            }
            return -1;
        }

        void put(const char* text, litehtml::uint_ptr hFont, int width)
        {
            if (!text) return;
            std::string key = make_key(text, hFont);
            auto it = m_map.find(key);
            if (it != m_map.end()) {
                // Update existing, move to front
                it->second->second = width;
                m_list.splice(m_list.begin(), m_list, it->second);
                return;
            }
            // Evict if needed
            if (m_list.size() >= m_max_entries) {
                m_map.erase(m_list.back().first);
                m_list.pop_back();
            }
            m_list.emplace_front(std::move(key), width);
            m_map[m_list.front().first] = m_list.begin();
        }

        void clear()
        {
            m_list.clear();
            m_map.clear();
        }

    private:
        static std::string make_key(const char* text, litehtml::uint_ptr hFont)
        {
            std::string key(text);
            key.push_back('|');
            key.append(reinterpret_cast<const char*>(&hFont), sizeof(hFont));
            return key;
        }

        size_t m_max_entries;
        mutable std::list<std::pair<std::string, int>> m_list;
        std::unordered_map<std::string, decltype(m_list)::iterator> m_map;
    };

    class ld_container : public litehtml::document_container
    {
      public:
        litedocx::DocxModel* model = nullptr;
        ld_callbacks         cb{};
        int                  viewport_w = 800;
        int                  viewport_h = 600;
        IWICImagingFactory*  wic        = nullptr;
        HDC                  m_measure_dc = nullptr;
        std::unique_ptr<litedocx::DocxModel> owned_model;
        std::map<std::string, std::unique_ptr<CachedImage>> images;
        std::vector<TableCellInfo> table_cells;
        litehtml::document::ptr doc;
        TextWidthCache       text_width_cache;

        void rebuild_table_cells()
        {
            table_cells.clear();
            if(!doc)
                return;
            
            /* Find all tables in the document */
            auto tables = doc->root()->select_all("table");
            int ti = 0;
            for(auto& table : tables)
            {
                if(!table)
                {
                    ++ti;
                    continue;
                }
                
                /* Find all rows in this table */
                auto rows = table->select_all("tr");
                int row_idx = 0;
                for(auto& row : rows)
                {
                    if(!row)
                    {
                        ++row_idx;
                        continue;
                    }
                    
                    /* Find all cells (td/th) in this row - comma selector doesn't work, use separate calls */
                    auto cells = row->select_all("td");
                    auto th_cells = row->select_all("th");
                    cells.insert(cells.end(), th_cells.begin(), th_cells.end());
                    int col_idx = 0;
                    for(auto& cell : cells)
                    {
                        if(!cell)
                        {
                            ++col_idx;
                            continue;
                        }
                        
                        TableCellInfo info;
                        info.table_index = ti;
                        info.row_index = row_idx;
                        info.col_index = col_idx;
                        info.is_header = (cell->tag() == litehtml::_th_) ? 1 : 0;
                        
                        /* Get rowspan/colspan */
                        const char* rs = cell->get_attr("rowspan");
                        if(rs) info.row_span = std::atoi(rs);
                        const char* cs = cell->get_attr("colspan");
                        if(cs) info.col_span = std::atoi(cs);
                        
                        /* Get cell placement */
                        litehtml::position pos = cell->get_placement();
                        info.x = static_cast<int>(pos.x);
                        info.y = static_cast<int>(pos.y);
                        info.w = static_cast<int>(pos.width);
                        info.h = static_cast<int>(pos.height);
                        
                        /* Get computed background color from css_properties */
                        const auto& css = cell->css();
                        const auto& bg = css.get_bg();
                        if(bg.m_color.alpha != 0)
                        {
                            info.bg_color = to_cref(bg.m_color);
                        }
                        
                        /* Get borders from css_properties */
                        const auto& borders = css.get_borders();
                        auto get_border = [&](const litehtml::css_border& cb, int& w, unsigned& c) {
                            w = static_cast<int>(cb.width.calc_percent(96)); // approximate
                            if(w < 1 && cb.style != litehtml::border_style_none) w = 1;
                            if(cb.style != litehtml::border_style_none)
                            {
                                c = to_cref(cb.color);
                            }
                        };
                        get_border(borders.left, info.border_left_w, info.border_left_c);
                        get_border(borders.top, info.border_top_w, info.border_top_c);
                        get_border(borders.right, info.border_right_w, info.border_right_c);
                        get_border(borders.bottom, info.border_bottom_w, info.border_bottom_c);
                        
                        /* Get padding */
                        const auto& padding = css.get_padding();
                        info.padding_left   = static_cast<int>(padding.left.calc_percent(96));
                        info.padding_top    = static_cast<int>(padding.top.calc_percent(96));
                        info.padding_right  = static_cast<int>(padding.right.calc_percent(96));
                        info.padding_bottom = static_cast<int>(padding.bottom.calc_percent(96));
                        
                        /* Get vertical-align */
                        switch(css.get_vertical_align())
                        {
                            case litehtml::va_top:          info.valign = "top"; break;
                            case litehtml::va_middle:       info.valign = "middle"; break;
                            case litehtml::va_bottom:       info.valign = "bottom"; break;
                            case litehtml::va_baseline:     info.valign = "baseline"; break;
                            case litehtml::va_sub:          info.valign = "sub"; break;
                            case litehtml::va_super:        info.valign = "super"; break;
                            case litehtml::va_text_top:     info.valign = "text-top"; break;
                            case litehtml::va_text_bottom:  info.valign = "text-bottom"; break;
                            default: break;
                        }
                        
                        table_cells.push_back(info);
                        ++col_idx;
                    }
                    ++row_idx;
                }
                ++ti;
            }
        }

        /* Find table cell at given position, returns pointer or nullptr */
        const TableCellInfo* find_table_cell(int x, int y, int w, int h) const
        {
            for(const auto& cell : table_cells)
            {
                if(cell.contains(x, y, w, h))
                    return &cell;
            }
            return nullptr;
        }

        /* Convert TableCellInfo to ld_table_cell_style for callbacks */
        static void to_cell_style(const TableCellInfo& src, ld_table_cell_style* dst)
        {
            dst->row_index = src.row_index;
            dst->col_index = src.col_index;
            dst->row_span = src.row_span;
            dst->col_span = src.col_span;
            dst->is_header = src.is_header;
            dst->bg_color = src.bg_color;
            dst->has_borders = (src.border_left_w | src.border_top_w | src.border_right_w | src.border_bottom_w) != 0;
            dst->border_left_w = src.border_left_w;
            dst->border_top_w = src.border_top_w;
            dst->border_right_w = src.border_right_w;
            dst->border_bottom_w = src.border_bottom_w;
            dst->border_left_c = src.border_left_c;
            dst->border_top_c = src.border_top_c;
            dst->border_right_c = src.border_right_c;
            dst->border_bottom_c = src.border_bottom_c;
            dst->padding_left = src.padding_left;
            dst->padding_top = src.padding_top;
            dst->padding_right = src.padding_right;
            dst->padding_bottom = src.padding_bottom;
            dst->valign = src.valign.empty() ? nullptr : src.valign.c_str();
        }


        CachedImage* get_image(const std::string& src, bool allow_load)
        {
            auto it = images.find(src);
            if(it != images.end())
            {
                return it->second.get();
            }
            if(!allow_load || !model)
            {
                return nullptr;
            }

            auto mi = model->media.find(src);
            if(mi == model->media.end())
            {
                // Some documents reference "media/image1.png" directly.
                std::string alt = "word/" + src;
                mi              = model->media.find(alt);
                if(mi == model->media.end())
                {
                    return nullptr;
                }
            }

            auto img = std::make_unique<CachedImage>();
            if(cb.load_image_data)
            {
                img->host = cb.load_image_data(src.c_str(), mi->second.data.data(),
                                               static_cast<int>(mi->second.data.size()));
            }
            if(!wic_decode_image(wic, mi->second.data.data(), mi->second.data.size(), *img))
            {
                if(!img->host)
                {
                    return nullptr;
                }
            }

            CachedImage* raw = img.get();
            images[src]      = std::move(img);
            return raw;
        }

        litehtml::uint_ptr create_font(const litehtml::font_description& descr, const litehtml::document*,
                                       litehtml::font_metrics* fm) override
        {
            lh_font_metrics m = {};
            void*           f = nullptr;
            if(cb.lh.create_font)
            {
                const int bold      = descr.weight >= 700 ? 1 : 0;
                const int italic    = descr.style == litehtml::font_style_italic ? 1 : 0;
                const int underline = (descr.decoration_line & litehtml::text_decoration_line_underline) ? 1 : 0;
                const int strike    = (descr.decoration_line & litehtml::text_decoration_line_line_through) ? 1 : 0;
                f = cb.lh.create_font(descr.family.c_str(), static_cast<int>(descr.size), bold, italic, underline, strike, &m);
            }

            if(fm)
            {
                const int size = static_cast<int>(descr.size);
                fm->font_size   = litehtml::pixel_t(m.font_size ? m.font_size : size);
                fm->height      = litehtml::pixel_t(m.height ? m.height : static_cast<int>(size * 1.35));
                fm->ascent      = litehtml::pixel_t(m.ascent ? m.ascent : size);
                fm->descent     = litehtml::pixel_t(m.descent ? m.descent : size / 4);
                fm->x_height    = litehtml::pixel_t(m.x_height ? m.x_height : size / 2);
                fm->ch_width    = litehtml::pixel_t(m.ch_width ? m.ch_width : size / 2);
                fm->draw_spaces = m.draw_spaces != 0;
                fm->sub_shift   = litehtml::pixel_t(m.sub_shift ? m.sub_shift : size / 5);
                fm->super_shift = litehtml::pixel_t(m.super_shift ? m.super_shift : size / 3);
            }
            return reinterpret_cast<litehtml::uint_ptr>(f);
        }

        void delete_font(litehtml::uint_ptr hFont) override
        {
            if(cb.lh.delete_font && hFont)
            {
                cb.lh.delete_font(reinterpret_cast<void*>(hFont));
            }
        }

        litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr hFont) override
        {
            LDBG((std::string("[LITEDOCX_TEXT_WIDTH] text='") + (text ? text : "NULL") + "'\n").c_str());
            // Check cache first
            int cached = text_width_cache.get(text, hFont);
            if (cached >= 0) {
                return static_cast<litehtml::pixel_t>(cached);
            }

            if(cb.lh.text_width)
            {
                if(!m_measure_dc)
                {
                    return 0;
                }
                size_t wlen = 0;
                const WCHAR* w = utf_cache().get_or_convert(text, &wlen);
                if(!w || w[0] == 0)
                {
                    return 0;
                }
                SIZE sz = {};
                HGDIOBJ old = SelectObject(m_measure_dc, reinterpret_cast<HFONT>(hFont));
                GetTextExtentPoint32W(m_measure_dc, w, static_cast<int>(wlen), &sz);
                SelectObject(m_measure_dc, old);
                int width = static_cast<int>(sz.cx);
                text_width_cache.put(text, hFont, width);
                return static_cast<litehtml::pixel_t>(width);
            }
            // Rough fallback: 0.5 em per byte.  Only used when the host does
            // not provide text metrics.
            int fallback = static_cast<int>(std::strlen(text ? text : "") * 7 / 2);
            text_width_cache.put(text, hFont, fallback);
            return litehtml::pixel_t(fallback);
        }

        void draw_text(litehtml::uint_ptr hdc, const char* text, litehtml::uint_ptr hFont, litehtml::web_color color,
                       const litehtml::position& pos) override
        {
            LDBG((std::string("[LITEDOCX_DRAW_TEXT] text='") + (text ? text : "NULL") + "' at (" + std::to_string(static_cast<int>(pos.x)) + "," + std::to_string(static_cast<int>(pos.y)) + ")\n").c_str());
            if(cb.lh.draw_text)
            {
                size_t wlen = 0;
                const WCHAR* w = utf_cache().get_or_convert(text, &wlen);
                if(!w || w[0] == 0)
                {
                    return;
                }
                RECT rc = {static_cast<int>(pos.x), static_cast<int>(pos.y),
                           static_cast<int>(pos.x + pos.width), static_cast<int>(pos.y + pos.height)};
HGDIOBJ old = SelectObject(reinterpret_cast<HDC>(hdc), reinterpret_cast<HFONT>(hFont));
                SetBkMode(reinterpret_cast<HDC>(hdc), TRANSPARENT);
                SetTextColor(reinterpret_cast<HDC>(hdc), (COLORREF)to_cref(color));
                DrawTextW(reinterpret_cast<HDC>(hdc), w, static_cast<int>(wlen), &rc,
                          DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
                SelectObject(reinterpret_cast<HDC>(hdc), old);
            }
            else
            {
                LDBG("[LITEDOCX_DRAW_TEXT] cb.lh.draw_text is NULL!\n");
            }
        }

        litehtml::pixel_t pt_to_px(float pt) const override
        {
            return pt * 96.0f / 72.0f;
        }

        litehtml::pixel_t get_default_font_size() const override
        {
            return 16;
        }

        const char* get_default_font_name() const override
        {
            return "Segoe UI";
        }

        void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker) override
        {
            HDC dc = reinterpret_cast<HDC>(hdc);
            if(!dc)
            {
                return;
            }

            const int x = static_cast<int>(marker.pos.x);
            const int y = static_cast<int>(marker.pos.y);
            const int w = static_cast<int>(marker.pos.width);
            const int h = static_cast<int>(marker.pos.height);
            if(w <= 0 || h <= 0)
            {
                return;
            }

            if(!marker.image.empty())
            {
                CachedImage* img = get_image(marker.image, true);
                if(img && img->bmp)
                {
                    HDC     mem = CreateCompatibleDC(dc);
                    HGDIOBJ old = SelectObject(mem, img->bmp);
                    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                    AlphaBlend(dc, x, y, w, h, mem, 0, 0, img->w, img->h, bf);
                    SelectObject(mem, old);
                    DeleteDC(mem);
                    return;
                }
            }

            COLORREF col = static_cast<COLORREF>(to_cref(marker.color));
            HBRUSH   br  = CreateSolidBrush(col);
            HGDIOBJ  old = SelectObject(dc, br);

            if(marker.marker_type == litehtml::list_style_type_circle)
            {
                HGDIOBJ pen = SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc, x, y, x + w, y + h);
                SelectObject(dc, pen);
            } else if(marker.marker_type == litehtml::list_style_type_square)
            {
                Rectangle(dc, x, y, x + w, y + h);
            } else if(marker.marker_type != litehtml::list_style_type_none)
            {
                HGDIOBJ pen = SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc, x, y, x + w, y + h);
                SelectObject(dc, pen);
            }

            SelectObject(dc, old);
            DeleteObject(br);
        }

        void load_image(const char* src, const char*, bool) override
        {
            if(src && src[0])
            {
                get_image(src, true);
            }
        }

        void get_image_size(const char* src, const char*, litehtml::size& sz) override
        {
            sz.width  = 0;
            sz.height = 0;
            if(!src || !src[0])
            {
                return;
            }
            CachedImage* img = get_image(src, true);
            if(img)
            {
                sz.width  = img->w;
                sz.height = img->h;
            }
        }

        void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer& layer, const std::string& url,
                        const std::string&) override
        {
            HDC dc = reinterpret_cast<HDC>(hdc);
            if(!dc || url.empty())
            {
                return;
            }

            CachedImage* img = get_image(url, true);
            if(!img)
            {
                return;
            }

            const int x = static_cast<int>(layer.border_box.x);
            const int y = static_cast<int>(layer.border_box.y);
            const int w = static_cast<int>(layer.border_box.width);
            const int h = static_cast<int>(layer.border_box.height);
            if(w <= 0 || h <= 0)
            {
                return;
            }

            if(img->host && cb.draw_image_data)
            {
                cb.draw_image_data(reinterpret_cast<void*>(hdc), img->host, x, y, w, h);
                return;
            }
            if(!img->bmp)
            {
                return;
            }

            HDC      mem = CreateCompatibleDC(dc);
            HGDIOBJ  old = SelectObject(mem, img->bmp);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dc, x, y, w, h, mem, 0, 0, img->w, img->h, bf);
            SelectObject(mem, old);
            DeleteDC(mem);
        }

        void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                             const litehtml::web_color& color) override
        {
            const int x = static_cast<int>(layer.border_box.x);
            const int y = static_cast<int>(layer.border_box.y);
            const int w = static_cast<int>(layer.border_box.width);
            const int h = static_cast<int>(layer.border_box.height);
            LDBG((std::string("[LITEDOCX_DRAW_FILL] at (") + std::to_string(x) + "," + std::to_string(y) + ") " + std::to_string(w) + "x" + std::to_string(h) + "\n").c_str());
            
            /* Check if this is a table cell and call custom callback */
            if(cb.draw_table_cell_bg)
            {
                const TableCellInfo* cell = find_table_cell(x, y, w, h);
                if(cell)
                {
                    ld_table_cell_style style;
                    to_cell_style(*cell, &style);
                    if(cb.draw_table_cell_bg(reinterpret_cast<void*>(hdc), x, y, w, h, &style))
                    {
                        return; /* Custom drawing handled it */
                    }
                }
            }
            
            if(cb.lh.draw_solid_fill)
            {
                cb.lh.draw_solid_fill(reinterpret_cast<void*>(hdc), x, y, w, h, to_cref(color));
                return;
            }

            HDC dc = reinterpret_cast<HDC>(hdc);
            if(!dc || w <= 0 || h <= 0)
            {
                return;
            }
            RECT   rc = {x, y, x + w, y + h};
            HBRUSH br = CreateSolidBrush(static_cast<COLORREF>(to_cref(color)));
            FillRect(dc, &rc, br);
            DeleteObject(br);
        }

        void draw_linear_gradient(litehtml::uint_ptr, const litehtml::background_layer&,
                                  const litehtml::background_layer::linear_gradient&) override
        {
        }
        void draw_radial_gradient(litehtml::uint_ptr, const litehtml::background_layer&,
                                  const litehtml::background_layer::radial_gradient&) override
        {
        }
        void draw_conic_gradient(litehtml::uint_ptr, const litehtml::background_layer&,
                                 const litehtml::background_layer::conic_gradient&) override
        {
        }

        void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders, const litehtml::position& pos,
                          bool) override
        {
            const int x = static_cast<int>(pos.x);
            const int y = static_cast<int>(pos.y);
            const int w = static_cast<int>(pos.width);
            const int h = static_cast<int>(pos.height);
            
            /* Check if this is a table cell and call custom callback */
            if(cb.draw_table_cell_borders)
            {
                const TableCellInfo* cell = find_table_cell(x, y, w, h);
                if(cell)
                {
                    ld_table_cell_style style;
                    to_cell_style(*cell, &style);
                    if(cb.draw_table_cell_borders(reinterpret_cast<void*>(hdc), x, y, w, h, &style))
                    {
                        return; /* Custom drawing handled it */
                    }
                }
            }
            
            if(cb.lh.draw_borders)
            {
                cb.lh.draw_borders(reinterpret_cast<void*>(hdc), x, y, w, h,
                                static_cast<int>(borders.left.width), static_cast<int>(borders.top.width),
                                static_cast<int>(borders.right.width), static_cast<int>(borders.bottom.width),
                                to_cref(borders.left.color), to_cref(borders.top.color), to_cref(borders.right.color),
                                to_cref(borders.bottom.color));
                return;
            }

            HDC dc = reinterpret_cast<HDC>(hdc);
            if(!dc)
            {
                return;
            }
            auto fill = [&](int rx, int ry, int rw, int rh, const litehtml::border& b) {
                if(static_cast<int>(b.width) <= 0 || rw <= 0 || rh <= 0)
                {
                    return;
                }
                RECT   r  = {rx, ry, rx + rw, ry + rh};
                HBRUSH br = CreateSolidBrush(static_cast<COLORREF>(to_cref(b.color)));
                FillRect(dc, &r, br);
                DeleteObject(br);
            };
            fill(x, y, w, static_cast<int>(borders.top.width), borders.top);
            fill(x, y + h - static_cast<int>(borders.bottom.width), w, static_cast<int>(borders.bottom.width),
                 borders.bottom);
            fill(x, y, static_cast<int>(borders.left.width), h, borders.left);
            fill(x + w - static_cast<int>(borders.right.width), y, static_cast<int>(borders.right.width), h,
                 borders.right);
        }

        void set_caption(const char* caption) override
        {
            if(cb.lh.set_caption)
            {
                cb.lh.set_caption(caption ? caption : "");
            }
        }

        void set_base_url(const char*) override {}
        void link(const std::shared_ptr<litehtml::document>&, const litehtml::element::ptr&) override {}

        void on_anchor_click(const char* url, const litehtml::element::ptr&) override
        {
            if(cb.lh.on_anchor_click)
            {
                cb.lh.on_anchor_click(url ? url : "");
            }
        }

        void on_mouse_event(const litehtml::element::ptr&, litehtml::mouse_event) override {}

        void set_cursor(const char* cursor) override
        {
            if(cb.lh.set_cursor)
            {
                cb.lh.set_cursor(cursor ? cursor : "");
            }
        }

        void transform_text(std::string&, litehtml::text_transform) override {}
        void import_css(std::string&, const std::string&, std::string&) override {}
        void set_clip(const litehtml::position&, const litehtml::border_radiuses&) override {}
        void del_clip() override {}

        void get_viewport(litehtml::position& viewport) const override
        {
            if(cb.lh.get_viewport)
            {
                int x = 0, y = 0, w = 0, h = 0;
                cb.lh.get_viewport(&x, &y, &w, &h);
                viewport.x      = x;
                viewport.y      = y;
                viewport.width  = w;
                viewport.height = h;
            } else
            {
                viewport.x      = 0;
                viewport.y      = 0;
                viewport.width  = viewport_w;
                viewport.height = viewport_h;
            }
        }

        litehtml::element::ptr create_element(const char*, const litehtml::string_map&,
                                              const std::shared_ptr<litehtml::document>&) override
        {
            return nullptr;
        }

        void get_media_features(litehtml::media_features& media) const override
        {
            if(cb.lh.get_media_features)
            {
                lh_media_features m = {};
                cb.lh.get_media_features(&m);
                media.type          = static_cast<litehtml::media_type>(m.type);
                media.width         = m.width;
                media.height        = m.height;
                media.device_width  = m.device_width;
                media.device_height = m.device_height;
                media.color         = m.color;
                media.color_index   = m.color_index;
                media.monochrome    = m.monochrome;
                media.resolution    = m.resolution;
            } else
            {
                media.type          = litehtml::media_type_screen;
                media.width         = viewport_w;
                media.height        = viewport_h;
                media.device_width  = viewport_w;
                media.device_height = viewport_h;
                media.color         = 8;
                media.color_index   = 256;
                media.monochrome    = 0;
                media.resolution    = 96;
            }
        }

        void get_language(std::string& language, std::string& culture) const override
        {
            language = "en";
            culture  = "";
        }
    };

    struct ld_doc
    {
        litehtml::document::ptr doc;
        ld_container*           container = nullptr;
        std::string             error;
        int                     page_w = 0;
        int                     page_h = 0;
        bool                    com_initialized = false;
        // Pagination state (Fase 2)
        int                     m_page_height_px = 0;       // CSS page height from DocxBuilder
        std::vector<int>        m_page_heights;             // Computed page heights (lazy)
        std::vector<int>        m_page_y_offsets;           // Y-offset of each page
        int                     m_last_rendered_page = -1;  // Highest page laid out (pagination)
        bool                    m_full_layout_done = false; // Full document::render completed
    };

    void fire_redraw(void* user, const litehtml::position& pos)
    {
        auto* h = static_cast<ld_doc*>(user);
        if(h && h->container && h->container->cb.lh.redraw_box)
        {
            h->container->cb.lh.redraw_box(static_cast<int>(pos.x), static_cast<int>(pos.y), static_cast<int>(pos.width),
                                        static_cast<int>(pos.height));
        }
    }

    void* open_impl(const void* data, size_t size, const std::string& file_name, const ld_callbacks* cb,
                    int viewport_width, int viewport_height)
    {
        if(!data || size == 0)
        {
            set_error("dati DOCX vuoti");
            return nullptr;
        }

        auto* h = new (std::nothrow) ld_doc();
        if(!h)
        {
            set_error("memoria insufficiente");
            return nullptr;
        }

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        h->com_initialized = SUCCEEDED(hr);

        auto* container       = new (std::nothrow) ld_container();
        if(!container)
        {
            if(h->com_initialized)
            {
                CoUninitialize();
            }
            delete h;
            set_error("memoria insufficiente");
            return nullptr;
        }
        h->container = container;
        if(cb)
        {
            container->cb = *cb;
        }
        container->viewport_w = viewport_width > 0 ? viewport_width : 800;
        container->viewport_h = viewport_height > 0 ? viewport_height : 600;
        container->m_measure_dc = GetDC(nullptr);

        if(h->com_initialized)
        {
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                             reinterpret_cast<void**>(&container->wic));
        }

        try
        {
            auto model = std::make_unique<litedocx::DocxModel>();
            std::string err;
            if(!litedocx::load_docx_package(data, size, file_name, *model, err))
            {
                set_error(err.empty() ? "apertura DOCX fallita" : err);
                if(container->wic)
                {
                    container->wic->Release();
                }
                delete container;
                if(h->com_initialized)
                {
                    CoUninitialize();
                }
                delete h;
                return nullptr;
            }

            container->model = model.get();
            litedocx::DocxBuilder builder(*model, container);
            litehtml::document::ptr doc = builder.build();
            if(!doc)
            {
                set_error(builder.error().empty() ? "conversione DOCX fallita" : builder.error());
                container->model = nullptr;
                if(container->wic)
                {
                    container->wic->Release();
                }
                delete container;
                if(h->com_initialized)
                {
                    CoUninitialize();
                }
                delete h;
                return nullptr;
            }

            h->doc    = doc;
            h->page_w = builder.page_width();
            h->page_h = builder.page_height();
            h->m_page_height_px = builder.page_height();  // Cache for pagination

            // Transfer model ownership into the container so that media data
            // remains valid for the whole document lifetime.
            container->owned_model = std::move(model);
            container->doc = doc;
            set_error("");
            return h;
        } catch(const std::exception& ex)
        {
            set_error(ex.what());
        } catch(...)
        {
            set_error("eccezione durante la conversione DOCX");
        }

        if(container->wic)
        {
            container->wic->Release();
        }
        delete container;
        if(h->com_initialized)
        {
            CoUninitialize();
        }
        delete h;
        return nullptr;
    }
} // namespace

extern "C" {

void* ld_open_file(const char* path, const ld_callbacks* cb, int viewport_width, int viewport_height)
{
    if(!path || !path[0])
    {
        set_error("percorso DOCX vuoto");
        return nullptr;
    }
    std::vector<unsigned char> data;
    if(!litedocx::read_file_bytes(path, data))
    {
        set_error("impossibile leggere il file DOCX");
        return nullptr;
    }
    return open_impl(data.data(), data.size(), litedocx::base_name(path), cb, viewport_width, viewport_height);
}

void* ld_open_memory(const void* data, size_t size, const ld_callbacks* cb, int viewport_width, int viewport_height)
{
    return open_impl(data, size, "", cb, viewport_width, viewport_height);
}

int ld_render(void* doc, int max_width)
{
    LDLOG("ld_render called, max_width=%d", max_width);
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc)
    {
        return 0;
    }
    try
    {
        if(max_width > 0 && h->container)
        {
            h->container->viewport_w = max_width;
        }
        int w = static_cast<int>(h->doc->render(max_width));
        LDLOG("ld_render done, width=%d", w);

        if(h->container)
        {
            h->container->doc = h->doc;
            h->container->rebuild_table_cells();
        }
        // Full layout fatto: marca flag e invalida cache pagine per ricalcolo geometria reale
        h->m_full_layout_done = true;
        h->m_page_heights.clear();
        h->m_page_y_offsets.clear();
        h->m_last_rendered_page = -1; // extract_page_geometry ricalcolerà geometria REALE alla prossima chiamata
        return w;
    } catch(...)
    {
        set_error("errore durante il layout");
        return 0;
    }
}

void ld_draw(void* doc, void* hdc, int x, int y, int clip_x, int clip_y, int clip_w, int clip_h)
{
    LDBG((std::string("[LITEDOCX_DRAW] at (") + std::to_string(x) + "," + std::to_string(y) + ") clip=(" + std::to_string(clip_x) + "," + std::to_string(clip_y) + "," + std::to_string(clip_w) + "," + std::to_string(clip_h) + ")\n").c_str());
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc)
    {
        return;
    }
    litehtml::position clip(clip_x, clip_y, clip_w, clip_h);
    try
    {
        h->doc->draw(reinterpret_cast<litehtml::uint_ptr>(hdc), x, y, &clip);
        LDBG("[LITEDOCX_DRAW] draw returned\n");
    } catch(...)
    {
        set_error("errore durante il disegno");
    }
}

int ld_width(void* doc)
{
    auto* h = static_cast<ld_doc*>(doc);
    return h && h->doc ? static_cast<int>(h->doc->width()) : 0;
}

int ld_height(void* doc)
{
    auto* h = static_cast<ld_doc*>(doc);
    return h && h->doc ? static_cast<int>(h->doc->height()) : 0;
}

int ld_page_width(void* doc)
{
    auto* h = static_cast<ld_doc*>(doc);
    return h ? h->page_w : 0;
}

int ld_page_height(void* doc)
{
    auto* h = static_cast<ld_doc*>(doc);
    return h ? h->page_h : 0;
}

int ld_mouse_over(void* doc, int x, int y, int client_x, int client_y)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc)
    {
        return 0;
    }
    try
    {
        return h->doc->on_mouse_over(x, y, client_x, client_y,
                                     [h](const litehtml::position& p) { fire_redraw(h, p); })
                   ? 1
                   : 0;
    } catch(...)
    {
        return 0;
    }
}

int ld_lbutton_down(void* doc, int x, int y, int client_x, int client_y)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc)
    {
        return 0;
    }
    try
    {
        return h->doc->on_lbutton_down(x, y, client_x, client_y,
                                       [h](const litehtml::position& p) { fire_redraw(h, p); })
                   ? 1
                   : 0;
    } catch(...)
    {
        return 0;
    }
}

int ld_lbutton_up(void* doc, int x, int y, int client_x, int client_y)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc)
    {
        return 0;
    }
    try
    {
        return h->doc->on_lbutton_up(x, y, client_x, client_y,
                                     [h](const litehtml::position& p) { fire_redraw(h, p); })
                   ? 1
                   : 0;
    } catch(...)
    {
        return 0;
    }
}

int ld_mouse_leave(void* doc)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc)
    {
        return 0;
    }
    try
    {
        return h->doc->on_mouse_leave([h](const litehtml::position& p) { fire_redraw(h, p); }) ? 1 : 0;
    } catch(...)
    {
        return 0;
    }
}

int ld_find_box(void* doc, const char* id, int* px, int* py, int* pw, int* ph)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !id || !id[0])
    {
        return 0;
    }
    try
    {
        auto fill = [&](const litehtml::element::ptr& el) {
            litehtml::position pos = el->get_placement();
            if(px)
                *px = static_cast<int>(pos.x);
            if(py)
                *py = static_cast<int>(pos.y);
            if(pw)
                *pw = static_cast<int>(pos.width);
            if(ph)
                *ph = static_cast<int>(pos.height);
        };

        auto list = h->doc->root()->select_all("[data-field]");
        for(auto& el : list)
        {
            if(!el)
                continue;
            const char* a = el->get_attr("data-field");
            if(a && std::strcmp(a, id) == 0)
            {
                fill(el);
                return 1;
            }
        }

        list = h->doc->root()->select_all("[id]");
        for(auto& el : list)
        {
            if(!el)
                continue;
            const char* a = el->get_attr("id");
            if(a && std::strcmp(a, id) == 0)
            {
                fill(el);
                return 1;
            }
        }
    } catch(...)
    {
    }
    return 0;
}

const char* ld_last_error(void)
{
    return last_error().c_str();
}

const char* ld_version(void)
{
    return "litedocx 0.1";
}

int ld_table_count(void* doc)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container)
    {
        return 0;
    }
    try
    {
        auto tables = h->doc->root()->select_all("table");
        return static_cast<int>(tables.size());
    } catch(...)
    {
        return 0;
    }
}

int ld_get_table_cells(void* doc, int table_index, ld_table_cell_info** out_cells, int* out_count)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container)
    {
        if(out_count) *out_count = 0;
        if(out_cells) *out_cells = nullptr;
        return 0;
    }
    try
    {
        const auto& cells = h->container->table_cells;
        int count = 0;
        for(const auto& c : cells)
        {
            if(c.table_index == table_index)
                ++count;
        }
        
        if(out_count) *out_count = count;
        
        if(!out_cells || count == 0)
        {
            return count;
        }
        
        ld_table_cell_info* arr = (ld_table_cell_info*)malloc(count * sizeof(ld_table_cell_info));
        if(!arr)
        {
            if(out_count) *out_count = 0;
            return 0;
        }
        
        int idx = 0;
        for(const auto& c : cells)
        {
            if(c.table_index == table_index)
            {
                arr[idx].table_index = c.table_index;
                arr[idx].row_index = c.row_index;
                arr[idx].col_index = c.col_index;
                arr[idx].row_span = c.row_span;
                arr[idx].col_span = c.col_span;
                arr[idx].is_header = c.is_header;
                arr[idx].x = c.x;
                arr[idx].y = c.y;
                arr[idx].w = c.w;
                arr[idx].h = c.h;
                arr[idx].bg_color = c.bg_color;
                arr[idx].border_left_w = c.border_left_w;
                arr[idx].border_top_w = c.border_top_w;
                arr[idx].border_right_w = c.border_right_w;
                arr[idx].border_bottom_w = c.border_bottom_w;
                arr[idx].border_left_c = c.border_left_c;
                arr[idx].border_top_c = c.border_top_c;
                arr[idx].border_right_c = c.border_right_c;
                arr[idx].border_bottom_c = c.border_bottom_c;
                arr[idx].padding_left = c.padding_left;
                arr[idx].padding_top = c.padding_top;
                arr[idx].padding_right = c.padding_right;
                arr[idx].padding_bottom = c.padding_bottom;
                arr[idx].valign = c.valign.empty() ? nullptr : c.valign.c_str();
                ++idx;
            }
        }
        *out_cells = arr;
        return count;
    } catch(...)
    {
        if(out_count) *out_count = 0;
        if(out_cells) *out_cells = nullptr;
        return 0;
    }
}

void ld_free_table_cells(ld_table_cell_info* cells)
{
    if(cells)
    {
        free(cells);
    }
}

// ===== Paginated layout API (Fase 4) =====

static void extract_page_geometry(ld_doc* h)
{
    LDLOG("extract_page_geometry called, m_full_layout_done=%d, m_page_heights.size()=%zu", h ? h->m_full_layout_done : -99, h ? h->m_page_heights.size() : 99);
    if (!h || !h->doc || !h->doc->root())
        return;
    
    // Already extracted?
    if (!h->m_page_heights.empty())
    {
        LDLOG("extract_page_geometry: already extracted, returning cached");
        return;
    }
    
    // Se full layout NON fatto: restituisci stima (non triggerare render su UI thread)
    if (!h->m_full_layout_done)
    {
        LDLOG("extract_page_geometry: full layout not done, returning estimate (m_page_height_px=%d)", h->m_page_height_px);
        h->m_page_y_offsets.push_back(0);
        h->m_page_heights.push_back(h->m_page_height_px > 0 ? h->m_page_height_px : 1000);
        return;
    }
    
    LDLOG("extract_page_geometry: full layout done, computing real geometry from page-breaks");
    // Full layout fatto: calcola geometria REALE dai page-break
    auto page_breaks = h->doc->root()->select_all("[data-page-break]");
    
    int last_y = 0;
    for (const auto& el : page_breaks)
    {
        if (!el) continue;
        litehtml::position pos = el->get_placement();
        int page_h = static_cast<int>(pos.y - last_y);
        if (page_h > 0)
        {
            h->m_page_y_offsets.push_back(last_y);
            h->m_page_heights.push_back(page_h);
            last_y = static_cast<int>(pos.y);
        }
    }
    
    // Last page: from last page-break to end of document
    int doc_h = static_cast<int>(h->doc->height());
    if (doc_h > last_y)
    {
        h->m_page_y_offsets.push_back(last_y);
        h->m_page_heights.push_back(doc_h - last_y);
    }
    else if (h->m_page_heights.empty())
    {
        // No page breaks at all -> single page
        h->m_page_y_offsets.push_back(0);
        h->m_page_heights.push_back(doc_h > 0 ? doc_h : h->m_page_height_px);
    }
    
    h->m_last_rendered_page = static_cast<int>(h->m_page_heights.size()) - 1;
    LDLOG("extract_page_geometry: computed %d pages, m_last_rendered_page=%d", h->m_last_rendered_page + 1, h->m_last_rendered_page);
}

int ld_get_page_count(void* doc)
{
    LDLOG("ld_get_page_count called");
    auto* h = static_cast<ld_doc*>(doc);
    if (!h || !h->doc)
        return 0;
    
    extract_page_geometry(h);
    int count = static_cast<int>(h->m_page_heights.size());
    LDLOG("ld_get_page_count returning %d", count);
    return count;
}

int ld_get_page_height(void* doc, int page_idx)
{
    LDLOG("ld_get_page_height called, page_idx=%d", page_idx);
    auto* h = static_cast<ld_doc*>(doc);
    if (!h || !h->doc || page_idx < 0)
        return 0;
    
    extract_page_geometry(h);
    if (page_idx >= static_cast<int>(h->m_page_heights.size()))
        return 0;
    
    int hgt = h->m_page_heights[page_idx];
    LDLOG("ld_get_page_height returning %d", hgt);
    return hgt;
}

int ld_render_range(void* doc, int page_idx, int viewport_h, int max_width)
{
    LDLOG("ld_render_range called, page_idx=%d, viewport_h=%d, max_width=%d", page_idx, viewport_h, max_width);
    auto* h = static_cast<ld_doc*>(doc);
    if (!h || !h->doc)
        return 0;
    
    // For now: ensure full layout on first call, then no-op
    if (h->m_last_rendered_page == -1)
    {
        LDLOG("ld_render_range: first call, doing full layout");
        int w = max_width > 0 ? max_width : (h->container ? h->container->viewport_w : 800);
        int rendered_w = static_cast<int>(h->doc->render(w));
        if (h->container)
        {
            h->container->doc = h->doc;
            h->container->rebuild_table_cells();
        }
        // Dopo il full layout, m_last_rendered_page viene settato da extract_page_geometry
        // quando verrà chiamato da ld_get_page_count/height
        extract_page_geometry(h);
        return rendered_w;
    }
    
    LDLOG("ld_render_range: already laid out, returning width");
    // Already laid out
    return ld_width(doc);
}

void ld_clear_page_cache(void* doc)
{
    auto* h = static_cast<ld_doc*>(doc);
    if (!h)
        return;
    h->m_page_heights.clear();
    h->m_page_y_offsets.clear();
    h->m_last_rendered_page = -1;
    h->m_full_layout_done = false; // Reset anche flag full layout
    // Nota: non resettiamo m_page_height_px (stima CSS) per evitare di perderla
}

void ld_destroy(void* doc)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h)
    {
        return;
    }
    h->doc.reset();
    if(h->container)
    {
        if(h->container->wic)
        {
            h->container->wic->Release();
        }
        if(h->container->m_measure_dc)
        {
            ReleaseDC(nullptr, h->container->m_measure_dc);
        }
        h->container->model = nullptr;
        h->container->images.clear();
        h->container->text_width_cache.clear();
        delete h->container;
    }
    // Clear UTF-8 to UTF-16 cache to free memory
    utf_cache().clear();
    if(h->com_initialized)
    {
        CoUninitialize();
    }
    delete h;
}

} // extern "C"
