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
#include <litehtml/el_space.h>
#include <litehtml/el_text.h>

#include <algorithm>
#include <cmath>
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

// Console logging switch for the DLL. 0 = no stderr output in release use.
// The viewer's CONSOLE_LOGGING flag does not reach here: litedocx.dll is a
// separate binary, so it has its own master switch.
#ifndef LITEDOCX_CONSOLE_LOG
#define LITEDOCX_CONSOLE_LOG 0
#endif

// Logging per debug Fase D (gated by LITEDOCX_CONSOLE_LOG)
#if LITEDOCX_CONSOLE_LOG
#define LDLOG(fmt, ...) do { fprintf(stderr, "[LITEDOCX] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#else
#define LDLOG(fmt, ...) ((void)0)
#endif

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

    // Helper: log start pixel of each text line to startpix.txt (for justified text alignment debugging)
    // Only logs the first word of each line (detected by y position change)
    // DISABLED: logging suspended
    static void log_start_pixel(int x_px, int y_px, int w_px, int h_px, const char* txt)
    {
        (void)x_px; (void)y_px; (void)w_px; (void)h_px; (void)txt;
        // Logging disabled
    }

    /* Plain-text extraction for the paragraph editing ABI.  html_tag::get_text
     * concatenates el_text children but ignores <br>, and the builder encodes
     * runs of spaces as U+00A0.  Walk the element tree explicitly so the text
     * shown in the editor round-trips through ld_paragraph_set_text. */
    static void collect_plain_text(const litehtml::element::ptr& el, std::string& out)
    {
        if(!el)
        {
            return;
        }
        if(el->is_break())
        {
            out.push_back('\n');
            return;
        }
        if(el->is_text())
        {
            el->get_text(out);
            return;
        }
        for(const auto& child : el->children())
        {
            collect_plain_text(child, out);
        }
    }

    static void normalize_plain_text(std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        for(size_t i = 0; i < text.size();)
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if(c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA0)
            {
                out.push_back(' ');
                i += 2;
            } else if(c == '\r')
            {
                ++i;
            } else
            {
                out.push_back(static_cast<char>(c));
                ++i;
            }
        }
        /* Placeholder paragraphs are built with a single NBSP: expose them as
         * empty text instead of a meaningless space. */
        bool only_space = !out.empty();
        for(char c : out)
        {
            if(c != ' ' && c != '\t' && c != '\n')
            {
                only_space = false;
                break;
            }
        }
        if(only_space)
        {
            out.clear();
        }
        text.swap(out);
    }

    /* Collect every element carrying a stable paragraph index, in DOM order.
     * A plain tree walk is used instead of an attribute selector so the
     * mapping does not depend on CSS selector support in litehtml. */
    static void collect_indexed_paragraphs(const litehtml::element::ptr& el,
                                           std::vector<litehtml::element::ptr>& out)
    {
        if(!el)
        {
            return;
        }
        if(el->get_attr("data-ld-paragraph-index"))
        {
            out.push_back(el);
        }
        for(const auto& child : el->children())
        {
            collect_indexed_paragraphs(child, out);
        }
    }

    /* Run-level formatting in this builder lives on generated <span class="sN">
     * elements (font-family/size/weight/color/...).  The paragraph class only
     * carries block properties (alignment, margins, ...).  Replacing the
     * children with bare text would therefore fall back to the body font.
     * Pick a representative run span so the flattened replacement keeps the
     * paragraph's dominant character formatting. */
    static bool is_run_class_name(const char* cls)
    {
        return cls && cls[0] == 's' && cls[1] >= '0' && cls[1] <= '9';
    }

    static void find_run_style_source(const litehtml::element::ptr& el,
                                      litehtml::element::ptr&       run,
                                      litehtml::element::ptr&       fallback)
    {
        if(!el)
        {
            return;
        }
        const char* cls = el->get_attr("class");
        if(cls && cls[0])
        {
            if(is_run_class_name(cls))
            {
                if(!run)
                {
                    run = el;
                }
            } else if(!fallback)
            {
                fallback = el;
            }
        } else if(!fallback)
        {
            const char* style = el->get_attr("style");
            if(style && style[0])
            {
                fallback = el;
            }
        }
        for(const auto& child : el->children())
        {
            find_run_style_source(child, run, fallback);
        }
    }

    static void append_plain_text_children(const litehtml::element::ptr& para, const std::string& text)
    {
        if(!para)
        {
            return;
        }
        auto doc = para->get_document();
        if(!doc)
        {
            return;
        }

        bool any = false;
        size_t i = 0;
        while(i < text.size())
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if(c == '\r')
            {
                ++i;
                continue;
            }
            if(c == '\n')
            {
                para->appendChild(doc->create_element("br", litehtml::string_map()));
                any = true;
                ++i;
                continue;
            }
            const bool space = (c == ' ' || c == '\t');
            if(space)
            {
                const size_t start = i;
                while(i < text.size() && (text[i] == ' ' || text[i] == '\t'))
                {
                    ++i;
                }
                const size_t len = i - start;
                const bool edge = (start == 0 || i == text.size());
                if(len > 1 || edge)
                {
                    std::string nbsp;
                    nbsp.resize(len * 2);
                    for(size_t k = 0; k < len; ++k)
                    {
                        nbsp[k * 2]     = '\xC2';
                        nbsp[k * 2 + 1] = '\xA0';
                    }
                    para->appendChild(std::make_shared<litehtml::el_text>(nbsp.c_str(), doc));
                } else
                {
                    para->appendChild(std::make_shared<litehtml::el_space>(" ", doc));
                }
                any = true;
            } else
            {
                const size_t start = i;
                while(i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
                {
                    ++i;
                }
                const std::string word(text.data() + start, i - start);
                para->appendChild(std::make_shared<litehtml::el_text>(word.c_str(), doc));
                any = true;
            }
        }
        if(!any)
        {
            para->appendChild(std::make_shared<litehtml::el_text>("\xC2\xA0", doc));
        }
    }

    class ld_container : public litehtml::document_container
    {
      public:
        litedocx::DocxModel* model = nullptr;
        ld_callbacks         cb{};
        int                  viewport_w = 800;
        int                  viewport_h = 600;
        int                  visual_padding_x = 0;  // Visual horizontal padding (margins) for paint
        int                  visual_padding_y = 0;  // Visual vertical padding (margins) for paint
        int                  layout_width = 0;      // Layout width (for wrapping) = max_width from ld_render
        int                  justification_width = 0; // Justification width (fixed, independent of layout_padding)
        IWICImagingFactory*  wic        = nullptr;
        HDC                  m_measure_dc = nullptr;
        std::unique_ptr<litedocx::DocxModel> owned_model;
        std::map<std::string, std::unique_ptr<CachedImage>> images;
        std::vector<TableCellInfo> table_cells;
        litehtml::document::ptr doc;
        TextWidthCache       text_width_cache;

        /* Paragraph editing support.  Entries are rebuilt only after a
         * completed layout; after an edit the cache is cleared so no stale
         * element pointer or placement can be observed. */
        struct ParagraphInfo
        {
            int                    index = 0;
            litehtml::element::ptr element;
            std::string            text;
            int                    x = 0;
            int                    y = 0;
            int                    w = 0;
            int                    h = 0;
            bool                   has_geometry = false;
        };
        std::map<int, ParagraphInfo> paragraphs;
        bool                         paragraphs_valid = false;

        /* Raw OOXML <w:p> source for each paragraph, captured while building
         * the litehtml DOM.  Keyed by the same stable index. */
        std::map<int, std::string>   paragraph_ooxml;
        /* Byte range of each <w:p> inside model->document_xml, so an edit can
         * replace exactly that slice and rebuild. */
        std::map<int, std::pair<size_t, size_t>> paragraph_ranges;

        void clear_paragraphs()
        {
            paragraphs.clear();
            paragraphs_valid = false;
        }

        void rebuild_paragraphs()
        {
            paragraphs.clear();
            paragraphs_valid = false;
            if(!doc || !doc->root())
            {
                return;
            }

            std::vector<litehtml::element::ptr> indexed;
            collect_indexed_paragraphs(doc->root(), indexed);
            for(auto& el : indexed)
            {
                if(!el)
                {
                    continue;
                }
                const char* attr = el->get_attr("data-ld-paragraph-index");
                if(!attr || !attr[0])
                {
                    continue;
                }
                const int index = std::atoi(attr);
                if(index < 0 || paragraphs.find(index) != paragraphs.end())
                {
                    continue;
                }

                ParagraphInfo info;
                info.index = index;
                info.element = el;
                collect_plain_text(el, info.text);
                normalize_plain_text(info.text);

                const litehtml::position pos = el->get_placement();
                info.x = static_cast<int>(pos.x);
                info.y = static_cast<int>(pos.y);
                info.w = static_cast<int>(pos.width);
                info.h = static_cast<int>(pos.height);
                info.has_geometry = (info.w > 0 && info.h > 0);

                paragraphs.emplace(index, std::move(info));
            }
            paragraphs_valid = true;
        }

        const ParagraphInfo* find_paragraph(int index) const
        {
            auto it = paragraphs.find(index);
            return it != paragraphs.end() ? &it->second : nullptr;
        }


        /* Renderer mode: the host owns the draw target.  In this mode the DLL
         * must not assume an HDC: text is measured/drawn through the host
         * callbacks and list markers are delegated when possible. */
        bool has_renderer() const { return cb.renderer != nullptr; }

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
                // setta altezza riga NON APPLICATO
                fm->height      = litehtml::pixel_t(m.height ? m.height : static_cast<int>(size * 1.35));
                //
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
            // Check cache first
            int cached = text_width_cache.get(text, hFont);
            if (cached >= 0) {
                return static_cast<litehtml::pixel_t>(cached);
            }

            /* Renderer mode: the host measures through its own engine so that
             * layout and painting share the same metrics (D7).  No GDI DC is
             * used here, which also keeps this call thread-safe on the worker. */
            if(has_renderer())
            {
                if(cb.lh.text_width)
                {
                    int width = cb.lh.text_width(text ? text : "", reinterpret_cast<void*>(hFont));
                    text_width_cache.put(text, hFont, width);
                    return static_cast<litehtml::pixel_t>(width);
                }
                int fallback = static_cast<int>(std::strlen(text ? text : "") * 7 / 2);
                text_width_cache.put(text, hFont, fallback);
                return litehtml::pixel_t(fallback);
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
                // Ensure consistent map mode with draw DC
                SetMapMode(m_measure_dc, MM_TEXT);
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
            // LDBG((std::string("[LITEDOCX_DRAW_TEXT] text='") + (text ? text : "NULL") + "' at (" + std::to_string(static_cast<int>(pos.x)) + "," + std::to_string(static_cast<int>(pos.y)) + ")\n").c_str());
            
            // Log start pixel for alignment debugging (justified text indent bug)
            log_start_pixel(static_cast<int>(pos.x), static_cast<int>(pos.y), static_cast<int>(pos.width), static_cast<int>(pos.height), text);

            /* Consistent rounding with the legacy measure DC (MM_TEXT uses
             * integer coordinates).  Computed before both branches so the
             * renderer and the GDI path receive identical geometry. */
            int x = static_cast<int>(floor(pos.x + 0.5f));
            int y = static_cast<int>(floor(pos.y + 0.5f));
            int w_px = static_cast<int>(floor(pos.width + 0.5f));
            int h_px = static_cast<int>(floor(pos.height + 0.5f));

            if(has_renderer())
            {
                /* Delegate the whole text drawing to the host.  No SelectObject
                 * or DrawTextW is performed here, so the host renderer (GDI
                 * today, Direct2D later) is the only text engine. */
                if(cb.lh.draw_text)
                {
                    cb.lh.draw_text(cb.renderer, text ? text : "",
                                    reinterpret_cast<void*>(hFont),
                                    to_cref(color), x, y, w_px, h_px);
                }
                return;
            }

            if(cb.lh.draw_text)
            {
                size_t wlen = 0;
                const WCHAR* w = utf_cache().get_or_convert(text, &wlen);
                if(!w || w[0] == 0)
                {
                    return;
                }
                RECT rc = {x, y, x + w_px, y + h_px};
                HDC dc = reinterpret_cast<HDC>(hdc);
                SetMapMode(dc, MM_TEXT);
                HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(hFont));
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, (COLORREF)to_cref(color));
                DrawTextW(dc, w, static_cast<int>(wlen), &rc,
                          DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
                SelectObject(dc, old);
            }
            else
            {
                // LDBG("[LITEDOCX_DRAW_TEXT] cb.lh.draw_text is NULL!\n");
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
            const int x = static_cast<int>(marker.pos.x);
            const int y = static_cast<int>(marker.pos.y);
            const int w = static_cast<int>(marker.pos.width);
            const int h = static_cast<int>(marker.pos.height);
            if(w <= 0 || h <= 0)
            {
                return;
            }

            /* Renderer mode: let the host draw the marker.  If the callback
             * declines (returns 0) we fall back to the legacy GDI code below,
             * which still works when the target happens to be an HDC. */
            if(has_renderer() && cb.draw_list_marker)
            {
                int type = LD_MARKER_OTHER;
                switch(marker.marker_type)
                {
                    case litehtml::list_style_type_circle: type = LD_MARKER_CIRCLE; break;
                    case litehtml::list_style_type_disc:   type = LD_MARKER_DISC;   break;
                    case litehtml::list_style_type_square: type = LD_MARKER_SQUARE; break;
                    case litehtml::list_style_type_none:   type = LD_MARKER_NONE;   break;
                    default:                               type = LD_MARKER_OTHER;  break;
                }
                if(!marker.image.empty())
                {
                    type = LD_MARKER_IMAGE;
                    CachedImage* img = get_image(marker.image, true);
                    if(img && img->bmp && cb.draw_image_bitmap)
                    {
                        cb.draw_image_bitmap(cb.renderer, img->bmp, img->w, img->h, x, y, w, h);
                        return;
                    }
                }
                if(cb.draw_list_marker(cb.renderer, x, y, w, h, type,
                                       to_cref(marker.color), marker.image.c_str()))
                {
                    return;
                }
            }

            HDC dc = reinterpret_cast<HDC>(hdc);
            if(!dc)
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
            if(url.empty())
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
                /* In renderer mode the target is the opaque renderer, not an
                 * HDC.  The host owns both the image and the renderer. */
                void* target = has_renderer() ? cb.renderer : reinterpret_cast<void*>(hdc);
                cb.draw_image_data(target, img->host, x, y, w, h);
                return;
            }
            if(has_renderer())
            {
                /* No host image: hand the internally decoded bitmap to the
                 * renderer.  There is deliberately no AlphaBlend fallback in
                 * renderer mode: a non-GDI renderer must never touch an HDC. */
                if(cb.draw_image_bitmap && img->bmp)
                {
                    cb.draw_image_bitmap(cb.renderer, reinterpret_cast<void*>(img->bmp),
                                         img->w, img->h, x, y, w, h);
                }
                return;
            }
            /* Legacy GDI fallback: only possible when the draw target is a
             * real HDC. */
            if(!img->bmp || !dc)
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
            // LDBG((std::string("[LITEDOCX_DRAW_FILL] at (") + std::to_string(x) + "," + std::to_string(y) + ") " + std::to_string(w) + "x" + std::to_string(h) + "\n").c_str());
            
            /* Check if this is a table cell and call custom callback */
            if(cb.draw_table_cell_bg)
            {
                const TableCellInfo* cell = find_table_cell(x, y, w, h);
                if(cell)
                {
                    ld_table_cell_style style;
                    to_cell_style(*cell, &style);
                    void* target = has_renderer() ? cb.renderer : reinterpret_cast<void*>(hdc);
                    if(cb.draw_table_cell_bg(target, x, y, w, h, &style))
                    {
                        return; /* Custom drawing handled it */
                    }
                }
            }
            
            if(cb.lh.draw_solid_fill)
            {
                void* target = has_renderer() ? cb.renderer : reinterpret_cast<void*>(hdc);
                cb.lh.draw_solid_fill(target, x, y, w, h, to_cref(color));
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
                    void* target = has_renderer() ? cb.renderer : reinterpret_cast<void*>(hdc);
                    if(cb.draw_table_cell_borders(target, x, y, w, h, &style))
                    {
                        return; /* Custom drawing handled it */
                    }
                }
            }
            
            if(cb.lh.draw_borders)
            {
                void* target = has_renderer() ? cb.renderer : reinterpret_cast<void*>(hdc);
                cb.lh.draw_borders(target, x, y, w, h,
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

        int get_visual_padding_x() const override
        {
            return visual_padding_x;
        }
        int get_visual_padding_y() const override
        {
            return visual_padding_y;
        }
        int get_viewport_width() const override
        {
            return viewport_w;
        }
        int get_layout_width() const override
        {
            // layout_width = viewport_w - layout_padding_x (if set), else viewport_w
            // For now, we need to store layout_padding_x in the container
            // But we can compute it from the render call's max_width
            // The max_width passed to ld_render IS the layout width
            // We'll store it when ld_render is called
            return layout_width;
        }
        int get_justification_width() const override
        {
            // Return fixed justification width, or fall back to layout_width
            return justification_width > 0 ? justification_width : layout_width;
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
        bool                    m_fonts_invalidated = false; // Font cache invalidated, need render tree rebuild
        bool                    m_rebuild_needed = false;    // DOM changed (paragraph edit): rebuild render tree
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
        /* Renderer mode: text measurement is delegated to the host, so the
         * DLL never needs its own measure DC (and never touches GDI from the
         * layout worker on the host's behalf). */
        if(!container->cb.renderer)
        {
            container->m_measure_dc = GetDC(nullptr);
        }

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
            container->paragraph_ooxml = builder.paragraph_ooxml();
            container->paragraph_ranges = builder.paragraph_ranges();
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

    /* Bring the litehtml render tree in sync with the element tree before a
     * layout pass.  Called only from the layout worker.  Both zoom changes
     * (font cache) and paragraph edits need a full render-tree rebuild. */
    static void prepare_render_tree(ld_doc* h)
    {
        if(!h || !h->doc)
        {
            return;
        }
        const bool fonts_invalidated = h->m_fonts_invalidated;
        const bool content_changed   = h->m_rebuild_needed;
        if(fonts_invalidated)
        {
            h->doc->clear_font_cache();
            if(h->container)
            {
                h->container->text_width_cache.clear();
            }
        }
        /* FIX: text_width_cache holds prefix widths computed from the
         * pre-edit content. After ld_run_set_text, the content has
         * changed but m_rebuild_needed is true while m_fonts_invalidated
         * remains false, so the cache above (fonts_invalidated branch)
         * is NOT cleared. Without this, ld_run_caret_hit uses stale
         * prefix widths and returns the pre-edit caret position.
         * Always clear when content changed (run edit, paragraph edit). */
        if(content_changed && h->container)
        {
            h->container->text_width_cache.clear();
        }
        if(fonts_invalidated || content_changed)
        {
            h->doc->rebuild_render_tree();
        }
        h->m_fonts_invalidated = false;
        h->m_rebuild_needed     = false;
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
        // CRITICAL: Font cache invalidation due to zoom change.
        // When the user changes zoom (Ctrl+mouse wheel), the viewer calls
        // ld_invalidate_font_cache() which sets m_fonts_invalidated flag.
        // A paragraph edit sets m_rebuild_needed.  Both require a full
        // render-tree rebuild before the layout pass.  This runs only on the
        // worker thread, serialized with drawing and hit-testing by the host.
        prepare_render_tree(h);

        // DO NOT overwrite viewport_w here! viewport_w should remain the visual page width
        // (set by ld_open_file/ld_open_memory) for media queries.
        // max_width is the LAYOUT width (content width for wrapping/justification).
        if(h->container)
        {
            h->container->layout_width = max_width;
        }
        int w = static_cast<int>(h->doc->render(max_width));
        LDLOG("ld_render done, width=%d", w);

        if(h->container)
        {
            h->container->doc = h->doc;
            h->container->rebuild_table_cells();
            /* Geometry cache is valid only after the layout completed. */
            h->container->rebuild_paragraphs();
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
    auto* h = static_cast<ld_doc*>(doc);
    int doc_w = h && h->doc ? (int)h->doc->width() : 0;
    int doc_h = h && h->doc ? (int)h->doc->height() : 0;
#if LITEDOCX_CONSOLE_LOG
    fprintf(stderr, "[LD_DRAW] doc=%p x=%d y=%d clip=(%d,%d,%d,%d) doc_w=%d doc_h=%d\n",
            doc, x, y, clip_x, clip_y, clip_w, clip_h, doc_w, doc_h);
#endif
    // LDBG((std::string("[LITEDOCX_DRAW] at (") + std::to_string(x) + "," + std::to_string(y) + ") clip=(" + std::to_string(clip_x) + "," + std::to_string(clip_y) + "," + std::to_string(clip_w) + "," + std::to_string(clip_h) + ")\n").c_str());
    if(!h || !h->doc)
    {
        return;
    }
    litehtml::position clip(clip_x, clip_y, clip_w, clip_h);
    try
    {
        h->doc->draw(reinterpret_cast<litehtml::uint_ptr>(hdc), x, y, &clip);
        // LDBG("[LITEDOCX_DRAW] draw returned\n");
    } catch(...)
    {
        set_error("errore durante il disegno");
    }
}

void ld_set_visual_padding(void* doc, int visual_padding_x, int visual_padding_y)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(h && h->container)
    {
        h->container->visual_padding_x = visual_padding_x;
        h->container->visual_padding_y = visual_padding_y;
    }
}

void ld_set_justification_width(void* doc, int justification_width)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(h && h->container)
    {
        h->container->justification_width = justification_width;
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

/* ---- Paragraph editing ABI ---- */

static char* ld_dup_utf8(const std::string& s)
{
    char* out = static_cast<char*>(malloc(s.size() + 1));
    if(!out)
    {
        return nullptr;
    }
    memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

int ld_paragraph_hit_test(void* doc, int x, int y, ld_paragraph_hit* out_hit, char** out_text)
{
    if(out_text)
    {
        *out_text = nullptr;
    }
    if(out_hit)
    {
        memset(out_hit, 0, sizeof(*out_hit));
        out_hit->index = -1;
    }

    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container || !h->container->paragraphs_valid)
    {
        return 0;
    }

    try
    {
        const ld_container::ParagraphInfo* best = nullptr;
        long                               best_area = 0;
        for(const auto& kv : h->container->paragraphs)
        {
            const ld_container::ParagraphInfo& p = kv.second;
            if(!p.has_geometry || !p.element)
            {
                continue;
            }
            if(x >= p.x && x < p.x + p.w && y >= p.y && y < p.y + p.h)
            {
                const long area = static_cast<long>(p.w) * static_cast<long>(p.h);
                if(!best || area < best_area)
                {
                    best      = &p;
                    best_area = area;
                }
            }
        }
        if(!best)
        {
            return 0;
        }

        if(out_hit)
        {
            out_hit->index = best->index;
            out_hit->x     = best->x;
            out_hit->y     = best->y;
            out_hit->w     = best->w;
            out_hit->h     = best->h;
        }
        if(out_text)
        {
            *out_text = ld_dup_utf8(best->text);
        }
        return 1;
    } catch(...)
    {
        set_error("errore durante l'hit-test del paragrafo");
        return 0;
    }
}

/* ---- Run hit-test di sola lettura (diagnostica) ---- */

/* Numero di caratteri UTF-8 di s dopo la normalizzazione di
 * normalize_plain_text: NBSP (C2 A0) -> ' ', '\r' scartato.  Gli offset
 * restituiti da ld_run_hit_test sono in questo spazio di testo, cosi'
 * coincidono con ld_paragraph_get_text. */
static size_t ld_norm_text_len(const std::string& s)
{
    size_t n = 0;
    for(size_t i = 0; i < s.size();)
    {
        if(s[i] == '\r')
        {
            ++i;
            continue;
        }
        if((unsigned char)s[i] == 0xC2 && i + 1 < s.size() &&
           (unsigned char)s[i + 1] == 0xA0)
        {
            n += 1;
            i += 2;
        }
        else
        {
            n += 1;
            i += 1;
        }
    }
    return n;
}

/* Testo normalizzato dell'intero sottoalbero di el (usato per i range dei
 * run: ogni figlio diretto del paragrafo e' uno "slot" di run). */
static int ld_subtree_norm_len(const litehtml::element::ptr& el)
{
    if(!el)
    {
        return 0;
    }
    if(el->is_break())
    {
        return 1; /* <br> contribuisce '\n' al testo plain */
    }
    if(el->is_text())
    {
        std::string t;
        el->get_text(t);
        return (int)ld_norm_text_len(t);
    }
    int n = 0;
    for(const auto& c : el->children())
    {
        n += ld_subtree_norm_len(c);
    }
    return n;
}

/* Stato della scansione ricorsiva delle parti di testo (el_text/el_space:
 * litehtml splitta il testo a livello di parola, ogni parte ha il proprio
 * box dopo il layout). */
struct ld_run_scan
{
    int         px;
    int         py;
    int         para_index     = -1;
    int         run_index      = -1; /* slot corrente (figlio diretto) */
    int         run_char_start = 0;  /* offset normalizzato di inizio slot */
    bool        exact          = false;
    bool        fb_valid       = false; /* candidata di fallback */
    int         fb_dist        = 1 << 30;
    int         fb_slot        = -1;
    int         fb_offset      = 0;
    int         fb_len         = 0;
    int         fb_intra       = 0;
    litehtml::position fb_pos;
    int         fb_fm_size     = 0;
    int         fb_fm_height   = 0;
    int         fb_fm_ascent   = 0;
    int         fb_fm_descent  = 0;
    int         fb_fm_ch_width = 0;
    ld_run_hit* out             = nullptr;
};

static void ld_record_hit(ld_run_scan& s, int word_offset,
                          const litehtml::position& pos, int intra,
                          int fm_size, int fm_height, int fm_ascent, int fm_descent,
                          int fm_ch_width)
{
    s.out->para_index      = s.para_index;
    s.out->run_index       = s.run_index;
    s.out->char_offset     = word_offset + intra;
    s.out->run_char_start  = s.run_char_start;
    s.out->run_char_offset = s.out->char_offset - s.out->run_char_start;
    s.out->x               = (int)pos.x;
    s.out->y               = (int)pos.y;
    s.out->w               = (int)pos.width;
    s.out->h               = (int)pos.height;
    s.out->font_size       = fm_size;
    s.out->font_height     = fm_height;
    s.out->font_ascent     = fm_ascent;
    s.out->font_descent    = fm_descent;
    s.out->font_ch_width   = fm_ch_width;
}

/* Visita in ordine DOM: aggiorna *offset (spazio testo normalizzato) e
 * registra il primo box che contiene il punto (exact). Se il punto cade in
 * un gap tra parole, tiene il candidato piu' vicino sulla stessa banda
 * verticale (fallback). Solo lettura: nessuna modifica a DOM o layout. */
static void ld_scan_text_parts(const litehtml::element::ptr& el, int* offset,
                               ld_run_scan& s)
{
    if(s.exact || !el)
    {
        return;
    }
    for(const auto& child : el->children())
    {
        if(s.exact)
        {
            return;
        }
        if(child->is_break())
        {
            *offset += 1;
            continue;
        }
        if(child->is_text())
        {
            std::string t;
            child->get_text(t);
            const int len = (int)ld_norm_text_len(t);
            const litehtml::position pos = child->get_placement();
            /* pixel_t di questa installazione litehtml e' float
             * (pixel_float_t): si confronta in int per evitare gli
             * overload ambigui int/pixel_t. */
            const int bx = (int)pos.x;
            const int by = (int)pos.y;
            const int bw = (int)pos.width;
            const int bh = (int)pos.height;
            if(bw > 0 && bh > 0)
            {
                /* Metriche del font della parte di testo (padre = run/span). */
                int fm_size = 0, fm_height = 0, fm_ascent = 0, fm_descent = 0, fm_ch_width = 0;
                {
                    litehtml::element::ptr p = child->parent();
                    if(p)
                    {
                        const auto& fm = p->css().get_font_metrics();
                        fm_size     = (int)fm.font_size;
                        fm_height   = (int)fm.height;
                        fm_ascent   = (int)fm.ascent;
                        fm_descent  = (int)fm.descent;
                        fm_ch_width = (int)fm.ch_width;
                    }
                }

                const bool in_box = (s.px >= bx && s.px < bx + bw &&
                                     s.py >= by && s.py < by + bh);
                if(in_box)
                {
                    int intra = 0;
                    if(len > 0)
                    {
                        intra = (int)(((double)(s.px - bx) * (double)len /
                                       (double)bw) + 0.5);
                        if(intra < 0)
                            intra = 0;
                        if(intra > len)
                            intra = len;
                    }
                    ld_record_hit(s, *offset, pos, intra,
                                  fm_size, fm_height, fm_ascent, fm_descent, fm_ch_width);
                    s.exact = true;
                    return;
                }
                /* stessa banda verticale, fuori dal box: gap tra parole */
                if(s.py >= by && s.py < by + bh)
                {
                    int dx;
                    if(s.px < bx)
                        dx = bx - s.px;
                    else if(s.px >= bx + bw)
                        dx = s.px - (bx + bw);
                    else
                        dx = 0;
                    if(dx < s.fb_dist)
                    {
                        s.fb_valid       = true;
                        s.fb_dist        = dx;
                        s.fb_slot        = s.run_index;
                        s.fb_offset      = *offset;
                        s.fb_len         = len;
                        s.fb_intra       = (s.px < bx) ? 0 : len;
                        s.fb_pos         = pos;
                        s.fb_fm_size     = fm_size;
                        s.fb_fm_height   = fm_height;
                        s.fb_fm_ascent   = fm_ascent;
                        s.fb_fm_descent  = fm_descent;
                        s.fb_fm_ch_width = fm_ch_width;
                    }
                }
            }
            *offset += len;
        }
        else
        {
            ld_scan_text_parts(child, offset, s);
        }
    }
}

int ld_run_hit_test(void* doc, int x, int y, ld_run_hit* out)
{
    if(out)
    {
        memset(out, 0, sizeof(*out));
        out->para_index      = -1;
        out->run_index       = -1;
        out->char_offset     = -1;
        out->run_char_start  = -1;
        out->run_char_end    = -1;
        out->run_char_offset = -1;
    }
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container || !h->container->paragraphs_valid)
    {
        return 0;
    }
    try
    {
        /* Stesso criterio di ld_paragraph_hit_test: il paragrafo piu'
         * piccolo contenente il punto. */
        const ld_container::ParagraphInfo* best = nullptr;
        long                               best_area = 0;
        for(const auto& kv : h->container->paragraphs)
        {
            const ld_container::ParagraphInfo& p = kv.second;
            if(!p.has_geometry || !p.element)
            {
                continue;
            }
            if(x >= p.x && x < p.x + p.w && y >= p.y && y < p.y + p.h)
            {
                const long area = static_cast<long>(p.w) * static_cast<long>(p.h);
                if(!best || area < best_area)
                {
                    best      = &p;
                    best_area = area;
                }
            }
        }
        if(!best)
        {
            return 0;
        }
        if(out)
        {
            out->para_index = best->index;
        }

        /* Passata 1: range normalizzato [inizio,fine) del testo di ogni
         * figlio diretto (slot di run, in ordine DOM, <br> escluso). */
        std::vector<std::pair<int, int>> ranges;
        ranges.reserve(16);
        {
            int offset = 0;
            for(const auto& child : best->element->children())
            {
                if(child->is_break())
                {
                    offset += 1;
                    continue;
                }
                const int len = ld_subtree_norm_len(child);
                ranges.emplace_back(offset, offset + len);
                offset += len;
            }
        }

        /* Passata 2: ricerca della parte di testo colpita. */
        ld_run_scan scan;
        scan.px        = x;
        scan.py        = y;
        scan.para_index = best->index;
        scan.out       = out;
        int    offset  = 0;
        size_t slot    = 0;
        for(const auto& child : best->element->children())
        {
            if(child->is_break())
            {
                offset += 1;
                continue;
            }
            if(slot < ranges.size())
            {
                scan.run_index      = (int)slot;
                scan.run_char_start = ranges[slot].first;
            }
            else
            {
                scan.run_index      = -1;
                scan.run_char_start = -1;
            }
            ld_scan_text_parts(child, &offset, scan);
            if(scan.exact)
            {
                break;
            }
            ++slot;
        }

        if(!scan.exact && scan.fb_valid)
        {
            /* Click in un gap: usa il candidato piu' vicino. */
            out->run_index       = scan.fb_slot;
            out->char_offset     = scan.fb_offset + scan.fb_intra;
            out->run_char_start  = (scan.fb_slot >= 0 &&
                                    (size_t)scan.fb_slot < ranges.size())
                                       ? ranges[(size_t)scan.fb_slot].first : -1;
            out->run_char_offset = (out->run_char_start >= 0)
                                       ? out->char_offset - out->run_char_start : -1;
            out->x = (int)scan.fb_pos.x;
            out->y = (int)scan.fb_pos.y;
            out->w = (int)scan.fb_pos.width;
            out->h = (int)scan.fb_pos.height;
            out->font_size     = scan.fb_fm_size;
            out->font_height   = scan.fb_fm_height;
            out->font_ascent   = scan.fb_fm_ascent;
            out->font_descent  = scan.fb_fm_descent;
            out->font_ch_width = scan.fb_fm_ch_width;
        }

        if(out && out->run_index >= 0 && (size_t)out->run_index < ranges.size() &&
           (scan.exact || scan.fb_valid))
        {
            out->run_char_end = ranges[(size_t)out->run_index].second;
        }

        set_error("");
        return (scan.exact || scan.fb_valid) ? 1 : 0;
    } catch(...)
    {
        set_error("errore durante l'hit-test del run");
        return 0;
    }
}

/* ---- Caret: hit-test con offset esatto a livello carattere ----
 *
 * Prima fase della Fase 2 (caret inline): sola lettura, nessuna modifica a
 * testo, DOM o cache di layout.  Come ld_run_hit_test individua il paragrafo,
 * lo slot di run e la parte di testo (parola) colpita, ma l'offset
 * intra-parola NON e' stimato proporzionalmente: viene misurato con le
 * stesse metriche font del layout/renderer (container->text_width, la
 * funzione stessa chiamata da litehtml in el_text::compute_styles) sui
 * prefissi del testo della parte colpita.  Restituisce anche la posizione
 * px della barretta del caret in coordinate documento.
 */

/* Prefisso di t che contiene n caratteri nello stesso spazio di
 * ld_paragraph_get_text (NBSP resta in byte cosi' come litehtml lo misura:
 * e' parte del testo originale della parte). */
static std::string ld_norm_prefix_bytes(const std::string& t, int n)
{
    std::string out;
    size_t      i = 0;
    int         k = 0;
    while(i < t.size() && k < n)
    {
        if(t[i] == '\r')
        {
            ++i;
            continue;
        }
        out.push_back(t[i]);
        if((unsigned char)t[i] == 0xC2 && i + 1 < t.size() &&
           (unsigned char)t[i + 1] == 0xA0)
        {
            out.push_back(t[i + 1]);
            i += 2;
        }
        else
        {
            i += 1;
        }
        ++k;
    }
    return out;
}

/* Stato della scansione per ld_run_caret_hit (stessa struttura di
 * ld_run_scan: DOM order, offset normalizzato, slot di run, fallback). */
struct ld_caret_scan
{
    int         px;
    int         py;
    int         para_index     = -1;
    int         run_index      = -1; /* slot corrente (figlio diretto) */
    int         run_char_start = 0;  /* offset normalizzato di inizio slot */
    bool        exact          = false;
    int         exact_vdist    = 1 << 30;
    bool        fb_valid       = false; /* candidata di fallback (gap) */
    int         fb_dy          = 1 << 30;
    int         fb_dist        = 1 << 30;
    int         fb_slot        = -1;
    int         fb_offset      = 0;
    int         fb_intra       = 0;
    litehtml::position fb_pos;
    ld_doc*     host           = nullptr;
    ld_caret_hit* out          = nullptr;
};

static void ld_record_caret_hit(ld_caret_scan& s, int word_offset,
                                const litehtml::position& pos, int intra,
                                int caret_x)
{
    s.out->para_index      = s.para_index;
    s.out->run_index       = s.run_index;
    s.out->char_offset     = word_offset + intra;
    s.out->run_char_start  = s.run_char_start;
    s.out->run_char_offset = s.out->char_offset - s.out->run_char_start;
    s.out->caret_x         = caret_x;
    s.out->caret_y         = (int)pos.y;
    s.out->caret_w         = 1;
    s.out->caret_h         = (int)pos.height;
}

/* Visita in ordine DOM: come ld_scan_text_parts ma con il caret esatto.
 * Per la parte colpita misura la larghezza di ogni prefisso del suo testo
 * con container->text_width (le stesse metriche del layout) e sceglie il
 * confine tra caratteri piu' vicino al punto cliccato. */
static void ld_caret_scan_parts(const litehtml::element::ptr& el, int* offset,
                                ld_caret_scan& s)
{
    if(!el)
    {
        return;
    }
    for(const auto& child : el->children())
    {
        if(child->is_break())
        {
            *offset += 1;
            continue;
        }
        if(child->is_text())
        {
            std::string t;
            child->get_text(t);
            const int len = (int)ld_norm_text_len(t);
            const litehtml::position pos = child->get_placement();
            /* Stesso arrotondamento di draw_text: evita una differenza di 1px
             * tra la x del caret e il bordo effettivo del glifo disegnato. */
            const int bx = static_cast<int>(floor(pos.x + 0.5f));
            const int by = (int)pos.y;
            const int bw = (int)pos.width;
            const int bh = (int)pos.height;
            if(bw > 0 && bh > 0)
            {
                /* Font con cui litehtml misura/disegna questa parte:
                 * quello del padre (el_text::compute_styles). */
                litehtml::uint_ptr font = 0;
                {
                    litehtml::element::ptr p = child->parent();
                    if(p)
                    {
                        font = p->css().get_font();
                    }
                }
                if(s.px >= bx && s.px < bx + bw &&
                   s.py >= by && s.py < by + bh)
                {
                    int cy = by + bh / 2;
                    int vdist = s.py - cy;
                    if(vdist < 0) vdist = -vdist;
                    if(s.exact && vdist > s.exact_vdist)
                    {
                        *offset += len;
                        continue;
                    }
                    s.exact_vdist = vdist;
                    int intra   = 0;
                    int caret_x = s.px;
                    if(len > 0 && font && s.host && s.host->container)
                    {
                        /* Offset esatto: per ogni confine tra caratteri i
                         * la larghezza del prefisso i; si sceglie il
                         * confine piu' vicino al punto cliccato. */
                        const float tx    = (float)(s.px - bx);
                        int         best_i = 0;
                        float       best_w = 0.0f;
                        float       best_d = 1e30f;
                        for(int i = 0; i <= len; ++i)
                        {
                            float w = 0.0f;
                            if(i > 0)
                            {
                                const std::string pre = ld_norm_prefix_bytes(t, i);
                                w = static_cast<float>(
                                    s.host->container->text_width(pre.c_str(), font));
                            }
                            float d = tx - w;
                            if(d < 0)
                            {
                                d = -d;
                            }
                            if(d < best_d)
                            {
                                best_d = d;
                                best_i = i;
                                best_w = w;
                            }
                        }
                        intra   = best_i;
                        caret_x = bx + (int)(best_w + 0.5f);
                    }
                    else if(len > 0)
                    {
                        /* Niente font: ripiega sulla stima proporzionale
                         * di ld_run_hit_test (caret al punto cliccato). */
                        intra = (int)(((double)(s.px - bx) * (double)len /
                                       (double)bw) + 0.5);
                        if(intra < 0)
                            intra = 0;
                        if(intra > len)
                            intra = len;
                        caret_x = s.px;
                    }
                    else
                    {
                        /* Parte vuota: caret sul bordo sinistro. */
                        caret_x = bx;
                    }
                    ld_record_caret_hit(s, *offset, pos, intra, caret_x);
                    s.exact = true;
                    *offset += len;
                    continue;
                }
                /* Fuori dal box: calcola distanza verticale dy e orizzontale dx
                 * per agganciare anche i click negli spazi interlinea o nei gap
                 * tra parole all'interno del paragrafo. */
                int dy = 0;
                if(s.py < by)
                    dy = by - s.py;
                else if(s.py >= by + bh)
                    dy = s.py - (by + bh - 1);

                int dx = 0;
                if(s.px < bx)
                    dx = bx - s.px;
                else if(s.px >= bx + bw)
                    dx = s.px - (bx + bw);

                if(dy < s.fb_dy || (dy == s.fb_dy && dx < s.fb_dist))
                {
                    s.fb_valid  = true;
                    s.fb_dy     = dy;
                    s.fb_dist   = dx;
                    s.fb_slot   = s.run_index;
                    s.fb_offset = *offset;
                    s.fb_intra  = (s.px < bx) ? 0 : len;
                    s.fb_pos    = pos;
                }
            }
            *offset += len;
        }
        else
        {
            ld_caret_scan_parts(child, offset, s);
        }
    }
}

int ld_run_caret_hit(void* doc, int x, int y, ld_caret_hit* out)
{
    if(out)
    {
        memset(out, 0, sizeof(*out));
        out->para_index      = -1;
        out->run_index       = -1;
        out->char_offset     = -1;
        out->run_char_start  = -1;
        out->run_char_end    = -1;
        out->run_char_offset = -1;
        out->caret_x         = -1;
        out->caret_y         = -1;
        out->caret_w         = 0;
        out->caret_h         = 0;
    }
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container || !h->container->paragraphs_valid)
    {
        return 0;
    }
    try
    {
        /* Stesso criterio di ld_run_hit_test: il paragrafo piu' piccolo
         * contenente il punto. */
        const ld_container::ParagraphInfo* best      = nullptr;
        long                               best_area = 0;
        for(const auto& kv : h->container->paragraphs)
        {
            const ld_container::ParagraphInfo& p = kv.second;
            if(!p.has_geometry || !p.element)
            {
                continue;
            }
            if(x >= p.x && x < p.x + p.w && y >= p.y && y < p.y + p.h)
            {
                const long area = static_cast<long>(p.w) * static_cast<long>(p.h);
                if(!best || area < best_area)
                {
                    best      = &p;
                    best_area = area;
                }
            }
        }
        if(!best)
        {
            return 0;
        }
        if(out)
        {
            out->para_index = best->index;
        }

        /* Passata 1: range normalizzato [inizio,fine) di ogni figlio
         * diretto (slot di run, ordine DOM, <br> escluso). */
        std::vector<std::pair<int, int>> ranges;
        ranges.reserve(16);
        {
            int offset = 0;
            for(const auto& child : best->element->children())
            {
                if(child->is_break())
                {
                    offset += 1;
                    continue;
                }
                const int len = ld_subtree_norm_len(child);
                ranges.emplace_back(offset, offset + len);
                offset += len;
            }
        }

        /* Passata 2: ricerca della parte di testo colpita + caret. */
        ld_caret_scan scan;
        scan.px    = x;
        scan.py    = y;
        scan.para_index = best->index;
        scan.host  = h;
        scan.out   = out;
        int    offset = 0;
        size_t slot   = 0;
        for(const auto& child : best->element->children())
        {
            if(child->is_break())
            {
                offset += 1;
                continue;
            }
            if(slot < ranges.size())
            {
                scan.run_index      = (int)slot;
                scan.run_char_start = ranges[slot].first;
            }
            else
            {
                scan.run_index      = -1;
                scan.run_char_start = -1;
            }
            ld_caret_scan_parts(child, &offset, scan);
            ++slot;
        }

        if(!scan.exact && scan.fb_valid)
        {
            /* Click in un gap: caret al bordo della parte piu' vicina
             * sulla stessa banda verticale. */
            out->run_index       = scan.fb_slot;
            out->char_offset     = scan.fb_offset + scan.fb_intra;
            out->run_char_start  = (scan.fb_slot >= 0 &&
                                    (size_t)scan.fb_slot < ranges.size())
                                       ? ranges[(size_t)scan.fb_slot].first : -1;
            out->run_char_offset = (out->run_char_start >= 0)
                                       ? out->char_offset - out->run_char_start : -1;
            out->caret_x = (int)scan.fb_pos.x +
                           (scan.fb_intra ? (int)scan.fb_pos.width : 0);
            out->caret_y = (int)scan.fb_pos.y;
            out->caret_w = 1;
            out->caret_h = (int)scan.fb_pos.height;
        }

        if(out && out->run_index >= 0 && (size_t)out->run_index < ranges.size() &&
           (scan.exact || scan.fb_valid))
        {
            out->run_char_end = ranges[(size_t)out->run_index].second;
        }

        set_error("");
        return (scan.exact || scan.fb_valid) ? 1 : 0;
    } catch(...)
    {
        set_error("errore durante il caret hit-test");
        return 0;
    }
}

/* ---- Caret: posizione deterministica da offset logico (post-edit) ----
 *
 * Dopo una modifica inline il viewer conosce l'offset esatto del caret dentro
 * il run (spazio testo normalizzato di ld_paragraph_get_text).  Chiedere al
 * viewer un hit-test per coordinate sarebbe pericoloso: la stima approssimata
 * del passo carattere puo' finire nel glifo successivo o in un run adiacente
 * con font diverso.  Questa funzione restituisce invece la posizione esatta
 * misurando i prefissi del testo con le stesse metriche del renderer, senza
 * toccare testo, DOM o cache di layout. */

/* Parte di testo di uno slot con geometria valida, in ordine DOM. */
struct ld_caret_pos_part
{
    std::string            text;
    int                    rel_off = 0; /* offset normalizzato dentro lo slot */
    litehtml::position     pos;
    litehtml::uint_ptr     font = 0;
    bool                   valid = false;
};

static void ld_collect_caret_pos_parts(const litehtml::element::ptr& el,
                                       int* rel_off,
                                       std::vector<ld_caret_pos_part>& parts)
{
    if(!el)
    {
        return;
    }
    if(el->is_break())
    {
        *rel_off += 1; /* <br> conta come un carattere nel testo normalizzato */
        return;
    }
    if(el->is_text())
    {
        std::string t;
        el->get_text(t);
        const litehtml::position pos = el->get_placement();
        ld_caret_pos_part part;
        part.text    = t;
        part.rel_off = *rel_off;
        part.pos     = pos;
        part.valid   = ((int)pos.width > 0 && (int)pos.height > 0);
        if(litehtml::element::ptr p = el->parent())
        {
            part.font = p->css().get_font();
        }
        parts.push_back(part);
        *rel_off += (int)ld_norm_text_len(t);
        return;
    }
    for(const auto& child : el->children())
    {
        ld_collect_caret_pos_parts(child, rel_off, parts);
    }
}

/* Primo font con un advance positivo dentro uno slot (usato per ch_width). */
static int ld_first_valid_ch_width(const litehtml::element::ptr& el)
{
    if(!el)
    {
        return 0;
    }
    if(el->css().get_font())
    {
        const int w = static_cast<int>(el->css().get_font_metrics().ch_width);
        if(w > 0)
        {
            return w;
        }
    }
    for(const auto& child : el->children())
    {
        const int w = ld_first_valid_ch_width(child);
        if(w > 0)
        {
            return w;
        }
    }
    return 0;
}

int ld_run_caret_pos(void* doc, int para_index, int run_index,
                     int run_char_offset, ld_caret_hit* out)
{
    if(out)
    {
        memset(out, 0, sizeof(*out));
        out->para_index      = -1;
        out->run_index       = -1;
        out->char_offset     = -1;
        out->run_char_start  = -1;
        out->run_char_end    = -1;
        out->run_char_offset = -1;
        out->caret_x         = -1;
        out->caret_y         = -1;
        out->caret_w         = 0;
        out->caret_h         = 0;
    }
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container || !h->container->paragraphs_valid ||
       !out || run_index < 0)
    {
        return 0;
    }
    try
    {
        const ld_container::ParagraphInfo* info = h->container->find_paragraph(para_index);
        if(!info || !info->element)
        {
            return 0;
        }

        /* Stesso criterio di ld_run_hit_test / ld_run_set_text: figli diretti
         * in ordine DOM, <br> escluso dallo slot ma +1 sull'offset testo. */
        litehtml::element::ptr slot;
        int run_start = -1;
        int run_end   = -1;
        {
            int offset     = 0;
            int slot_index = 0;
            for(const auto& child : info->element->children())
            {
                if(child->is_break())
                {
                    offset += 1;
                    continue;
                }
                const int len = ld_subtree_norm_len(child);
                if(slot_index == run_index)
                {
                    slot      = child;
                    run_start = offset;
                    run_end   = offset + len;
                    break;
                }
                offset += len;
                ++slot_index;
            }
        }
        if(!slot || run_end < run_start)
        {
            return 0;
        }

        int rel = run_char_offset;
        if(rel < 0) rel = 0;
        if(rel > run_end - run_start) rel = run_end - run_start;

        /* Raccoglie le parti di testo dello slot in ordine DOM: la parte che
         * contiene l'offset (o l'ultima valida per l'offset finale) determina
         * la riga e il font con cui misurare il prefisso. */
        std::vector<ld_caret_pos_part> parts;
        int rel_off = 0;
        ld_collect_caret_pos_parts(slot, &rel_off, parts);

        /* Ultima parte con geometria: l'offset finale va agganciato li',
         * anche se in coda ci sono spazi collassati senza box. */
        size_t last_valid = parts.size();
        for(size_t i = 0; i < parts.size(); ++i)
        {
            if(parts[i].valid)
            {
                last_valid = i;
            }
        }

        bool placed = false;
        for(size_t i = 0; i < parts.size() && !placed; ++i)
        {
            const ld_caret_pos_part& part = parts[i];
            if(!part.valid)
            {
                continue;
            }
            const int len  = (int)ld_norm_text_len(part.text);
            const bool last = (i == last_valid);
            if(rel < part.rel_off + len || (rel == part.rel_off + len && last))
            {
                int intra = rel - part.rel_off;
                if(intra < 0) intra = 0;
                if(intra > len) intra = len;

                int w = 0;
                if(intra > 0 && part.font && h->container)
                {
                    const std::string pre = ld_norm_prefix_bytes(part.text, intra);
                    w = h->container->text_width(pre.c_str(), part.font);
                }

                out->caret_x = static_cast<int>(floor(part.pos.x + 0.5f)) +
                               static_cast<int>(w + 0.5f);
                out->caret_y = static_cast<int>(floor(part.pos.y + 0.5f));
                out->caret_w = 1;
                out->caret_h = static_cast<int>(floor(part.pos.height + 0.5f));
                placed = true;
            }
        }

        if(!placed)
        {
            /* Run vuoto (o senza geometria): fallback sicuro sullo slot,
             * poi sul paragrafo. */
            litehtml::position anchor = slot->get_placement();
            if((int)anchor.height <= 0)
            {
                anchor = info->element->get_placement();
            }
            out->caret_x = static_cast<int>(floor(anchor.x + 0.5f));
            out->caret_y = static_cast<int>(floor(anchor.y + 0.5f));
            out->caret_w = 1;
            out->caret_h = static_cast<int>(floor(anchor.height + 0.5f));
            if(out->caret_h <= 0)
            {
                out->caret_h = 1;
            }
        }

        out->para_index      = para_index;
        out->run_index       = run_index;
        out->run_char_start  = run_start;
        out->run_char_end    = run_end;
        out->run_char_offset = rel;
        out->char_offset     = run_start + rel;

        set_error("");
        return 1;
    } catch(...)
    {
        set_error("errore durante il calcolo della posizione del caret");
        return 0;
    }
}

int ld_run_font_ch_width(void* doc, int para_index, int run_index)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container || !h->container->paragraphs_valid ||
       run_index < 0)
    {
        return 0;
    }
    try
    {
        const ld_container::ParagraphInfo* info = h->container->find_paragraph(para_index);
        if(!info || !info->element)
        {
            return 0;
        }
        int slot_index = 0;
        for(const auto& child : info->element->children())
        {
            if(child->is_break())
            {
                continue;
            }
            if(slot_index == run_index)
            {
                /* Un run senza span e' un nodo di testo: il font e' sul
                 * padre (come in el_text::compute_styles). */
                litehtml::element::ptr font_el =
                    child->is_text() ? child->parent() : child;
                return ld_first_valid_ch_width(font_el);
            }
            ++slot_index;
        }
    } catch(...)
    {
        return 0;
    }
    return 0;
}

/* ---- Read-only preview info (shadow text during inline typing) ---- */

static bool ld_prev_ieq(const char* a, const char* b)
{
    if(!a || !b)
    {
        return false;
    }
    while(*a && *b)
    {
        char ca = *a;
        char cb = *b;
        if(ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
        if(cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
        if(ca != cb)
        {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static bool ld_prev_in_table(const litehtml::element::ptr& el)
{
    for(litehtml::element::ptr p = el; p; p = p->parent())
    {
        const char* tag = p->get_tagName();
        if(ld_prev_ieq(tag, "table") || ld_prev_ieq(tag, "td") || ld_prev_ieq(tag, "th"))
        {
            return true;
        }
    }
    return false;
}

static bool ld_prev_has_break(const litehtml::element::ptr& el)
{
    if(!el)
    {
        return false;
    }
    if(el->is_break())
    {
        return true;
    }
    for(const auto& c : el->children())
    {
        if(ld_prev_has_break(c))
        {
            return true;
        }
    }
    return false;
}

/* Trasparente o bianco dimostrabile, senza immagini/gradienti reali.
 * litehtml inserisce un placeholder image di tipo type_none anche quando
 * non c'e' background-image: va ignorato come fa background::is_empty(). */
static bool ld_prev_bg_ok(const litehtml::element::ptr& el)
{
    if(!el)
    {
        return true;
    }
    const litehtml::background& bg = el->css().get_bg();
    for(const auto& img : bg.m_image)
    {
        if(!img.is_empty())
        {
            return false;   /* immagine o gradiente vero */
        }
    }
    const litehtml::web_color& c = bg.m_color;
    if(c.alpha == 0)
    {
        return true;
    }
    return c.alpha == 255 && c.red == 255 && c.green == 255 && c.blue == 255;
}

static bool ld_prev_is_ancestor(const litehtml::element::ptr& anc,
                                const litehtml::element::ptr& el)
{
    for(litehtml::element::ptr p = el; p; p = p->parent())
    {
        if(p == anc)
        {
            return true;
        }
    }
    return false;
}

/* Qualsiasi elemento renderizzato non appartenente al nostro slot che
 * overlaps la stessa riga a destra dell'inizio del run rende impossibile
 * garantire che una stringa piu' lunga non copra contenuto esistente. */
static bool ld_prev_conflict(const litehtml::element::ptr& el,
                             const litehtml::element::ptr& slot,
                             int y0, int y1, int x0)
{
    if(!el)
    {
        return false;
    }
    /* Il nostro slot e' sotto el: e' contenuto nostro, non un conflitto. */
    if(el == slot || ld_prev_is_ancestor(slot, el))
    {
        return false;
    }
    /* el e' un antenato del nostro slot: non ha un box proprio da controllare,
     * ma i suoi altri figli (slot vicini) vanno visitati. */
    if(!ld_prev_is_ancestor(el, slot))
    {
        const litehtml::position pos = el->get_placement();
        const int px = static_cast<int>(pos.x);
        const int py = static_cast<int>(pos.y);
        const int pw = static_cast<int>(pos.width);
        const int ph = static_cast<int>(pos.height);
        if(pw > 0 && ph > 0 && py < y1 && py + ph > y0 && px >= x0 - 1)
        {
            return true;
        }
    }
    for(const auto& c : el->children())
    {
        if(ld_prev_conflict(c, slot, y0, y1, x0))
        {
            return true;
        }
    }
    return false;
}

int ld_run_preview_info(void* doc, int para_index, int run_index, int caret_y, ld_run_preview* out)
{
    if(out)
    {
        memset(out, 0, sizeof(*out));
    }
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container || !h->container->paragraphs_valid ||
       !out || run_index < 0)
    {
        return 0;
    }
    try
    {
        const ld_container::ParagraphInfo* info = h->container->find_paragraph(para_index);
        if(!info || !info->element || !info->has_geometry)
        {
            return 0;
        }
        if(ld_prev_in_table(info->element) || ld_prev_has_break(info->element))
        {
            return 0;
        }

        /* Stesso criterio di ld_run_caret_pos / ld_run_set_text:
         * figlio diretto numero run_index, <br> escluso dallo slot. */
        litehtml::element::ptr slot;
        int run_start = -1;
        int run_end   = -1;
        {
            int offset     = 0;
            int slot_index = 0;
            for(const auto& child : info->element->children())
            {
                if(child->is_break())
                {
                    offset += 1;
                    continue;
                }
                const int len = ld_subtree_norm_len(child);
                if(slot_index == run_index)
                {
                    slot      = child;
                    run_start = offset;
                    run_end   = offset + len;
                    break;
                }
                offset += len;
                ++slot_index;
            }
        }
        if(!slot || run_end <= run_start)
        {
            return 0;
        }

        /* Tutto il contenuto dello slot deve essere testo puro.  litehtml
         * splitta il testo in parole (el_text/el_space): un run puo' avere
         * piu' foglie e puo' andare a capo.  Ogni riga e' un blocco contiguo
         * indipendente, selezionato in base alla posizione del caret. */
        std::vector<litehtml::element::ptr> leaves;
        if(slot->is_text())
        {
            leaves.push_back(slot);
        }
        else
        {
            for(const auto& child : slot->children())
            {
                if(!child->is_text())
                {
                    return 0;   /* contenuto misto: nessuna preview */
                }
                leaves.push_back(child);
            }
        }
        if(leaves.empty())
        {
            return 0;
        }

        /* Nessun newline nel testo del run.  Il testo normalizzato viene
         * ricostruito foglia per foglia: gli offset di riga devono essere
         * BYTE di questo testo (g_inline_run_text e' UTF-8), non caratteri. */
        std::string norm;
        std::vector<std::pair<int, int> > leaf_bytes;
        int total_chars = 0;
        for(const auto& leaf : leaves)
        {
            std::string t;
            leaf->get_text(t);
            if(t.find('\n') != std::string::npos ||
               t.find('\r') != std::string::npos)
            {
                return 0;
            }
            total_chars += static_cast<int>(ld_norm_text_len(t));
            const int b0 = static_cast<int>(norm.size());
            /* Stessa normalizzazione di normalize_plain_text, senza la
             * regola "solo spazi -> vuoto" che vale per il paragrafo intero. */
            for(size_t k = 0; k < t.size();)
            {
                const unsigned char c = static_cast<unsigned char>(t[k]);
                if(c == 0xC2 && k + 1 < t.size() &&
                   static_cast<unsigned char>(t[k + 1]) == 0xA0)
                {
                    norm.push_back(' ');
                    k += 2;
                }
                else
                {
                    norm.push_back(static_cast<char>(c));
                    ++k;
                }
            }
            leaf_bytes.push_back(std::make_pair(b0, static_cast<int>(norm.size())));
        }
        if(total_chars != run_end - run_start)
        {
            return 0;
        }

        /* Nessun altro slot dopo questo. */
        int slots = 0;
        for(const auto& child : info->element->children())
        {
            if(!child->is_break())
            {
                ++slots;
            }
        }
        if(run_index != slots - 1)
        {
            return 0;
        }

        /* Genitore comune delle foglie: come in el_text::draw e' li' che
         * litehtml legge font e colore. */
        litehtml::element::ptr font_el = leaves[0]->parent();
        if(!font_el)
        {
            return 0;
        }
        for(const auto& leaf : leaves)
        {
            if(leaf->parent() != font_el)
            {
                return 0;
            }
        }

        /* Raggruppa le foglie per riga (stesso y/h), verificando la
         * contiguita' interna e registrando l'intervallo di byte di ogni
         * riga dentro il testo del run. */
        struct ld_prev_line
        {
            int y = 0, h = 0;
            int x0 = 0, x1 = 0;
            int cs = 0, ce = 0;
            int last_right = -1;
        };
        std::vector<ld_prev_line> lines;
        for(size_t leaf_i = 0; leaf_i < leaves.size(); ++leaf_i)
        {
            const litehtml::element::ptr& leaf = leaves[leaf_i];
            const int byte_start = leaf_bytes[leaf_i].first;
            const int byte_end   = leaf_bytes[leaf_i].second;
            const litehtml::position pos = leaf->get_placement();
            const int lx = static_cast<int>(floor(pos.x + 0.5f));
            const int ly = static_cast<int>(floor(pos.y + 0.5f));
            const int lw = static_cast<int>(floor(pos.width + 0.5f));
            const int lh = static_cast<int>(floor(pos.height + 0.5f));
            if(lw <= 0 || lh <= 0)
            {
                return 0;   /* foglia non renderizzata */
            }
            ld_prev_line* ln = nullptr;
            for(auto& candidate : lines)
            {
                if(candidate.y == ly && candidate.h == lh)
                {
                    ln = &candidate;
                    break;
                }
            }
            if(!ln)
            {
                lines.push_back(ld_prev_line());
                ln = &lines.back();
                ln->y  = ly;
                ln->h  = lh;
                ln->x0 = lx;
                ln->cs = byte_start;
            }
            else if(ln->last_right >= 0 && lx < ln->last_right - 1)
            {
                return 0;   /* buco nella riga: non verificabile */
            }
            if(lx + lw > ln->x1)
            {
                ln->x1 = lx + lw;
            }
            ln->ce = byte_end;
            ln->last_right = lx + lw;
        }

        /* La riga da mostrare e' quella che contiene il caret, in coordinate
         * documento (g_caret_y e' esattamente la y della foglia colpita). */
        const ld_prev_line* sel = nullptr;
        for(const auto& ln : lines)
        {
            if(caret_y >= ln.y && caret_y < ln.y + ln.h)
            {
                sel = &ln;
                break;
            }
        }
        if(!sel || sel->ce <= sel->cs)
        {
            return 0;
        }

        const int x = sel->x0;
        const int y = sel->y;
        const int w = sel->x1 - sel->x0;
        const int h = sel->h;
        if(w <= 0 || h <= 0)
        {
            return 0;
        }
        if(ld_prev_conflict(info->element, slot, y, y + h, x))
        {
            return 0;
        }

        /* Fondo noto: nessuna immagine/gradiente, trasparente o bianco.
         * Nessun bordo o decorazione che il FillRect bianco cancellerebbe. */
        if(!ld_prev_bg_ok(info->element) || !ld_prev_bg_ok(slot) ||
           !ld_prev_bg_ok(font_el))
        {
            return 0;
        }
        for(litehtml::element::ptr p = info->element->parent(); p; p = p->parent())
        {
            if(!ld_prev_bg_ok(p) || p->css().get_text_decoration_line() != 0)
            {
                return 0;
            }
        }
        if(slot->css().get_borders().is_visible() ||
           font_el->css().get_borders().is_visible() ||
           slot->css().get_text_decoration_line() != 0 ||
           font_el->css().get_text_decoration_line() != 0 ||
           info->element->css().get_text_decoration_line() != 0)
        {
            return 0;
        }

        const litehtml::uint_ptr font = font_el->css().get_font();
        const litehtml::web_color col = font_el->css().get_color();
        if(!font || col.alpha != 255)
        {
            return 0;
        }

        const int safe_w = info->x + info->w - x - 1;
        if(safe_w <= 0)
        {
            return 0;
        }

        out->x         = x;
        out->y         = y;
        out->w         = w;
        out->h         = h;
        out->char_start = sel->cs;
        out->char_end   = sel->ce;
        out->font      = reinterpret_cast<void*>(font);
        out->color     = to_cref(col);
        out->safe_w    = safe_w;
        set_error("");
        return 1;
    }
    catch(...)
    {
        set_error("errore durante la query di preview del run");
        return 0;
    }
}
char* ld_paragraph_get_text(void* doc, int index)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container || !h->container->paragraphs_valid)
    {
        return nullptr;
    }
    try
    {
        const ld_container::ParagraphInfo* info = h->container->find_paragraph(index);
        if(!info)
        {
            return nullptr;
        }
        return ld_dup_utf8(info->text);
    } catch(...)
    {
        set_error("errore durante la lettura del paragrafo");
        return nullptr;
    }
}

int ld_paragraph_set_text(void* doc, int index, const char* text)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container || !text)
    {
        set_error("parametri modifica paragrafo non validi");
        return 0;
    }

    try
    {
        ld_container* c = h->container;
        const ld_container::ParagraphInfo* info = c->find_paragraph(index);
        if(!info || !info->element)
        {
            set_error("indice paragrafo non valido");
            return 0;
        }

        litehtml::element::ptr para = info->element;

        /* Choose the run whose formatting is kept.  Crucially, the existing
         * element is REUSED: litehtml applies class-based CSS only when the
         * stylesheet is matched during document finalization.  A span created
         * later would carry the class name but never receive its font rules,
         * which is exactly why the text fell back to the body font.  Reusing
         * an element that already matched the stylesheet keeps font-family,
         * size, weight and colour. */
        litehtml::element::ptr style_run;
        litehtml::element::ptr style_fallback;
        for(const auto& child : para->children())
        {
            find_run_style_source(child, style_run, style_fallback);
        }
        litehtml::element::ptr keep = style_run ? style_run : style_fallback;

        /* Geometry and text snapshots are stale until the next completed
         * layout.  Drop every cached pointer/placement and force a full
         * layout (range rendering must not early-out on old pagination).
         * Invalidate BEFORE mutating the DOM so an allocation failure can
         * never leave a "valid" cache pointing at changed content. */
        c->clear_paragraphs();
        h->m_rebuild_needed    = true;
        h->m_full_layout_done  = false;
        h->m_page_heights.clear();
        h->m_page_y_offsets.clear();
        h->m_last_rendered_page = -1;

        /* The paragraph element, its class and data-ld-paragraph-index stay
         * untouched.  clearRecursive() also empties the kept run element while
         * leaving its attributes and matched styles intact. */
        para->clearRecursive();
        litehtml::element::ptr target = para;
        if(keep)
        {
            para->appendChild(keep);
            target = keep;
        }
        append_plain_text_children(target, std::string(text));

        set_error("");
        return 1;
    } catch(...)
    {
        set_error("errore durante la modifica del paragrafo");
        return 0;
    }
}

void ld_free_string(char* str)
{
    if(str)
    {
        free(str);
    }
}

/* ---- Persistenza OOXML dell'edit del run ----
 *
 * Lo slot cliccato (modello del builder: figlio diretto del paragrafo)
 * corrisponde a un w:r nel frammento OOXML memorizzato per quel paragrafo.
 * Il mapping si fa per OFFSET, non per ordinale: il walker ripercorre lo
 * stesso DFS di DocxBuilder::process_inline_children (docx_builder.cpp) e
 * accumula la lunghezza del testo normalizzato che il builder emetterebbe
 * (NBSP compatto come ld_norm_text_len, w:tab = 4 spazi, w:br/w:cr/
 * noBreakHyphen/sym = 1 carattere).  Gli slot senza testo (bookmark, div di
 * page-break, span vuoti) non spostano l'offset e quindi non influenzano il
 * match; hyperlink/ins/smartTag/fldSimple/sdt cambiano solo il nesting, non
 * l'ordine DFS.  Se il totale degli offset calcolato sull'OOXML non coincide
 * con quello dell'albero render (paragrafo modificato in memoria in passato,
 * formule oMath, ...), il mapping non e' affidabile e l'edit resta
 * in-memory (non persistente). */

static int ld_apply_paragraph_ooxml(ld_doc* h, ld_container* c, int index,
                                    const char* ooxml, size_t len);

struct ld_ooxml_run_scan
{
    int                    offset       = 0;  /* offset corrente (testo normalizzato) */
    int                    target_start = -1;
    int                    target_end   = -1;
    const litedocx::XmlNode* run        = nullptr; /* w:r corrispondente, se trovato */
};

/* Lunghezza normalizzata del testo emesso dal builder per un w:r.
 * *editable = 0 se il run contiene altro oltre a w:t (tab, br, sym,
 * disegni): quei run non si lasciano riscrivere con la sola sostituzione
 * del w:t. */
static int ld_ooxml_run_text_len(const litedocx::XmlNode* run, int* editable)
{
    int len = 0;
    *editable = 1;
    for(const auto& c : run->children)
    {
        if(!c || c->is("rPr"))
        {
            continue;
        }
        if(c->is("t"))
        {
            if(!c->text.empty())
            {
                len += (int)ld_norm_text_len(c->text);
            }
            continue;
        }
        if(c->is("br") || c->is("cr"))     { len += 1; *editable = 0; continue; }
        if(c->is("tab"))                   { len += 4; *editable = 0; continue; }
        if(c->is("noBreakHyphen"))         { len += 1; *editable = 0; continue; }
        if(c->is("sym"))
        {
            len += c->attr("w:char").empty() ? 0 : 1;
            *editable = 0;
            continue;
        }
        if(c->is("drawing") || c->is("pict") || c->is("object"))
        {
            *editable = 0;
            continue;
        }
        /* fldChar/instrText/delText/lastRenderedPageBreak/ecc.: il builder
         * li ignora (zero testo). */
    }
    return len;
}

/* DFS sul frammento OOXML nello stesso ordine del builder.  Completa sempre
 * la scansione (serve l'offset totale per la verifica di allineamento) e
 * registra il PRIMO w:r il cui range coincide con quello del run cliccato. */
static void ld_ooxml_walk(const litedocx::XmlNode* node, ld_ooxml_run_scan* s,
                          int* unsupported)
{
    if(!node)
    {
        return;
    }
    if(node->is("pPr"))
    {
        return;
    }
    if(node->is("r"))
    {
        int         editable = 0;
        const int   len      = ld_ooxml_run_text_len(node, &editable);
        const int   start    = s->offset;
        s->offset += len;
        if(!s->run && len > 0 && editable &&
           start == s->target_start && start + len == s->target_end)
        {
            s->run = node;
        }
        return;
    }
    if(node->is("hyperlink"))
    {
        /* Con href i run interni annidano dentro <a>, senza no diventano
         * slot diretti del paragrafo: in entrambi i casi l'ordine DFS e
         * l'offset non cambiano. */
        for(const auto& c : node->children)
        {
            ld_ooxml_walk(c.get(), s, unsupported);
        }
        return;
    }
    if(node->is("bookmarkStart"))
    {
        /* Slot zero-length (un <a> vuoto) se ha un nome: non sposta l'offset. */
        return;
    }
    if(node->is("del") || node->is("moveFrom") || node->is("commentRangeStart") ||
       node->is("commentRangeEnd") || node->is("commentReference") ||
       node->is("bookmarkEnd") || node->is("proofErr") ||
       node->is("lastRenderedPageBreak") || node->is("fldChar") ||
       node->is("instrText") || node->is("delText"))
    {
        return;
    }
    if(node->is("sdt"))
    {
        if(const litedocx::XmlNode* c = node->child("sdtContent"))
        {
            for(const auto& cc : c->children)
            {
                ld_ooxml_walk(cc.get(), s, unsupported);
            }
        }
        return;
    }
    if(node->is("ins") || node->is("moveTo") || node->is("smartTag") ||
       node->is("customXml") || node->is("fldSimple") || node->is("sdtContent") ||
       node->is("dir") || node->is("bdo"))
    {
        for(const auto& cc : node->children)
        {
            ld_ooxml_walk(cc.get(), s, unsupported);
        }
        return;
    }
    if(node->is("oMath") || node->is("oMathPara"))
    {
        /* Il builder emette il testo della formula via collect_text: non
         * specchiato dal walker. */
        *unsupported = 1;
        return;
    }
    /* drawing/pict/object a livello paragrafo ed elementi sconosciuti: il
     * builder li emette senza testo o li ignora: offset invariato. */
}

/* Prova a rendere persistente l'edit del run andando a toccare il w:t del
 * w:r corrispondente nel document.xml in memoria, poi splicando il frammento
 * w:p modificato (ld_apply_paragraph_ooxml: validazione, splice, rollback,
 * rebuild completo dell'albero).  Ritorna 1 se persistito: in quel caso
 * l'albero render e' GIA' stato rigenerato dal rebuild e NON va mutato di
 * nuovo.  Ritorna 0 se il paragrafo non e' mappabile sull'OOXML (nessun
 * documento memorizzato, offset non allineati, costrutti non supportati):
 * il chiamante cade sul percorso in-memory.  Diagnostica su stderr. */
static int ld_run_persist_ooxml(ld_doc* h, int index, int target_start,
                                int target_end, int render_total, const char* text)
{
    if(!h || !h->container || !h->container->owned_model)
    {
        return 0;
    }
    litedocx::DocxModel* model = h->container->owned_model.get();

    auto range = h->container->paragraph_ranges.find(index);
    if(range == h->container->paragraph_ranges.end())
    {
        return 0;
    }
    const size_t begin = range->second.first;
    const size_t end   = range->second.second;
    if(begin >= end || end > model->document_xml.size())
    {
        return 0;
    }

    const std::string frag_src = model->document_xml.substr(begin, end - begin);
    std::unique_ptr<litedocx::XmlNode> frag =
        litedocx::xml_parse(frag_src.data(), frag_src.size());
    if(!frag || !frag->is("p"))
    {
        fprintf(stderr, "[RUN][OOXML] frammento w:p non valido: fallback\n");
        fflush(stderr);
        return 0;
    }

    ld_ooxml_run_scan scan;
    scan.target_start = target_start;
    scan.target_end   = target_end;
    int unsupported   = 0;
    /* Come in DocxBuilder::add_paragraph: ogni figlio diretto del w:p passa
     * per il dispatch di process_inline_children (il w:p stesso non emette
     * testo).  Il ramo di default del walker NON ricorre, quindi la
     * discesa parte da qui. */
    for(const auto& c : frag->children)
    {
        ld_ooxml_walk(c.get(), &scan, &unsupported);
    }

    if(scan.offset != render_total)
    {
        fprintf(stderr, "[RUN][OOXML] offset non allineati (render=%d ooxml=%d): fallback\n",
                render_total, scan.offset);
        fflush(stderr);
        return 0;
    }
    if(unsupported)
    {
        fprintf(stderr, "[RUN][OOXML] paragrafo con formule (oMath): fallback\n");
        fflush(stderr);
        return 0;
    }
    if(!scan.run)
    {
        fprintf(stderr, "[RUN][OOXML] nessun w:r per il range [%d..%d): fallback\n",
                target_start, target_end);
        fflush(stderr);
        return 0;
    }

    /* Policy: il w:r deve avere testo puro (solo w:t). */
    const litedocx::XmlNode* first_t = nullptr;
    int t_count = 0;
    for(const auto& c : scan.run->children)
    {
        if(!c || c->is("rPr"))
        {
            continue;
        }
        if(c->is("t"))
        {
            if(!first_t)
            {
                first_t = c.get();
            }
            ++t_count;
            continue;
        }
        if(c->is("br") || c->is("cr") || c->is("tab") || c->is("noBreakHyphen") ||
           c->is("sym") || c->is("drawing") || c->is("pict") || c->is("object"))
        {
            fprintf(stderr, "[RUN][OOXML] run con contenuto non testuale: fallback\n");
            fflush(stderr);
            return 0;
        }
    }
    if(!first_t)
    {
        fprintf(stderr, "[RUN][OOXML] run senza w:t: fallback\n");
        fflush(stderr);
        return 0;
    }

    /* Modifica il DOM OOXML: il primo w:t prende il nuovo testo (gli
     * eventuali w:t aggiuntivi vengono rimossi).  xml:space="preserve"
     * protegge gli spazi ai bordi; xml_serialize si occupa dell'escaping. */
    litedocx::XmlNode* run = const_cast<litedocx::XmlNode*>(scan.run);
    litedocx::XmlNode* wt  = const_cast<litedocx::XmlNode*>(first_t);
    wt->text = text;
    wt->attrs.clear();
    wt->attrs.emplace("xml:space", std::string("preserve"));
    if(t_count > 1)
    {
        run->children.erase(
            std::remove_if(run->children.begin(), run->children.end(),
                           [&wt](const std::unique_ptr<litedocx::XmlNode>& c) {
                               return c && c->is("t") && c.get() != wt;
                           }),
            run->children.end());
    }

    const std::string frag_new = litedocx::xml_serialize(*frag);
    if(!ld_apply_paragraph_ooxml(h, h->container, index,
                                 frag_new.c_str(), frag_new.size()))
    {
        fprintf(stderr, "[RUN][OOXML] splice/rebuild fallito ('%s'): fallback\n",
                ld_last_error());
        fflush(stderr);
        return 0;
    }

    fprintf(stderr, "[RUN][OOXML] run persistito in document.xml (para=%d range [%d..%d))\n",
            index, target_start, target_end);
    fflush(stderr);
    return 1;
}

/* Sostituisce il testo di un singolo run (slot) di un paragrafo, preservando
 * la formattazione di tutti gli altri run.  Lo slot e' lo stesso individuato
 * da ld_run_hit_test: figlio diretto numero run_index in ordine DOM, <br>
 * esclusi.  Due casi:
 *  - span: l'elemento ESISTENTE viene riutilizzato (clearRecursive svuota il
 *    contenuto ma conserva attributi e stili matched, quindi il font resta
 *    corretto) e il nuovo testo viene appeso come parti plain;
 *  - nodo di testo puro (run senza formattazione, el_text/el_space): lo slot
 *    viene rimosso e i pezzi del nuovo testo vengono appesi al paragrafo,
 *    ripristinando poi in coda i fratelli successivi (appendChild non ha
 *    insert_before, ma l'ordine si ricostruisce riappendendo).
 * Invalidazione geometria/cache ESATTAMENTE come ld_paragraph_set_text e
 * SEMPRE prima di mutare il DOM.  '\n' e '\r' sono rifiutati: un run e'
 * inline, un'interruzione di riga dentro lo span romperebbe il modello degli
 * slot.  Se il paragrafo ha OOXML memorizzato e il mapping con document.xml
 * e' allineato (ld_run_persist_ooxml), la modifica viene invece applicata
 * DIRETTAMENTE a document.xml (w:t del w:r corrispondente) e l'albero viene
 * rigenerato dal rebuild: l'edit persiste nel salvataggio del file.  Ritorna
 * 1 se text e' il nuovo testo del run. */
int ld_run_set_text(void* doc, int index, int run_index, const char* text)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container || !text)
    {
        set_error("parametri modifica run non validi");
        return 0;
    }
    if(strchr(text, '\n') || strchr(text, '\r'))
    {
        set_error("il testo di un run non puo' contenere fine riga");
        return 0;
    }

    try
    {
        ld_container* c = h->container;
        const ld_container::ParagraphInfo* info = c->find_paragraph(index);
        if(!info || !info->element)
        {
            set_error("indice paragrafo non valido");
            return 0;
        }

        /* Stesso criterio di ld_run_hit_test: figli diretti in ordine DOM,
         * <br> esclusi dal conteggio degli slot.  La stessa scansiona
         * restituisce lo slot, il suo range normalizzato [start,end) e il
         * testo totale del paragrafo (tutti e tre servono al mapping
         * OOXML). */
        litehtml::element::ptr slot;
        int target_start = -1;
        int target_end   = -1;
        int render_total = 0;
        {
            int offset = 0;
            int n      = 0;
            for(const auto& child : info->element->children())
            {
                if(child->is_break())
                {
                    offset += 1;
                    continue;
                }
                const int len = ld_subtree_norm_len(child);
                if(n == run_index)
                {
                    slot         = child;
                    target_start = offset;
                    target_end   = offset + len;
                }
                offset += len;
                ++n;
            }
            render_total = offset;
        }
        if(!slot || target_end <= target_start)
        {
            set_error("indice run non valido");
            return 0;
        }

        /* Persistenza: se il paragrafo ha OOXML memorizzato e il mapping
         * con document.xml e' allineato, la modifica va direttamente nel
         * w:t del w:r e il rebuild rigenera l'albero render: l'edit compare
         * anche nel salvataggio.  A fallimento si cade sul percorso
         * in-memory (solo a video). */
        {
            int persisted = 0;
            try
            {
                persisted = ld_run_persist_ooxml(h, index, target_start,
                                                 target_end, render_total, text);
            } catch(...)
            {
                persisted = 0;
            }
            if(persisted)
            {
                set_error("");
                return 1;
            }
            fprintf(stderr, "[RUN][OOXML] fallback in-memory (edit non persistente)\n");
            fflush(stderr);
        }

        /* Cache e geometria diventano stale: invalida PRIMA di mutare il
         * DOM, cosi' un fallimento non puo' lasciare cache "valida" su
         * contenuto cambiato. */
        c->clear_paragraphs();
        h->m_rebuild_needed    = true;
        h->m_full_layout_done  = false;
        h->m_page_heights.clear();
        h->m_page_y_offsets.clear();
        h->m_last_rendered_page = -1;
        /* FIX: clear text_width_cache in the in-memory fallback path.
         * The OOXML persist path (ld_run_persist_ooxml ->
         * ld_apply_paragraph_ooxml) already clears the cache before
         * returning. This fallback (OOXML persist failed) also needs
         * the cache cleared, otherwise ld_run_caret_hit will use
         * stale prefix widths and return the pre-edit position.
         * NOTE: this is a defense-in-depth measure. The primary fix
         * is in ld_apply_paragraph_ooxml (which clears the cache after
         * rebuild) and in ld_render (which clears on content_changed). */
        if(h->container)
            h->container->text_width_cache.clear();

        if(slot->is_text())
        {
            /* Run senza formattazione: il nodo di testo non puo' avere
             * figli, quindi lo si sostituisce con i pezzi del nuovo testo
             * appesi direttamente al paragrafo. */
            litehtml::element::ptr para = info->element;
            std::vector<litehtml::element::ptr> tail;
            bool seen = false;
            for(const auto& child : para->children())
            {
                if(child == slot)
                {
                    seen = true;
                    continue;
                }
                if(seen)
                {
                    tail.push_back(child);
                }
            }
            if(!para->removeChild(slot))
            {
                set_error("rimozione del run non riuscita");
                return 0;
            }
            append_plain_text_children(para, std::string(text));
            for(const auto& t : tail)
            {
                para->appendChild(t);
            }
        }
        else
        {
            /* Span riusato: attributi e stili matched restano intatti. */
            slot->clearRecursive();
            append_plain_text_children(slot, std::string(text));
        }

        set_error("");
        return 1;
    } catch(...)
    {
        set_error("errore durante la modifica del run");
        return 0;
    }
}

/* ---- Inline editing: insert/delete at character offset within a run ---- */

int ld_run_edit(void* doc, int index, int run_index, int char_offset,
                int op, const char* text)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container)
    {
        set_error("parametri modifica run non validi");
        return 0;
    }

    try
    {
        ld_container* c = h->container;
        const ld_container::ParagraphInfo* info = c->find_paragraph(index);
        if(!info || !info->element)
        {
            set_error("indice paragrafo non valido");
            return 0;
        }

        /* Locate the run slot and its normalized range, same as ld_run_set_text. */
        litehtml::element::ptr slot;
        int target_start = -1;
        int target_end   = -1;
        int render_total = 0;
        {
            int offset = 0;
            int n      = 0;
            for(const auto& child : info->element->children())
            {
                if(child->is_break())
                {
                    offset += 1;
                    continue;
                }
                const int len = ld_subtree_norm_len(child);
                if(n == run_index)
                {
                    slot         = child;
                    target_start = offset;
                    target_end   = offset + len;
                }
                offset += len;
                ++n;
            }
            render_total = offset;
        }
        if(!slot || target_end <= target_start)
        {
            set_error("indice run non valido");
            return 0;
        }

        /* Get current run text in normalized space (same as ld_paragraph_get_text). */
        std::string run_text;
        collect_plain_text(slot, run_text);
        normalize_plain_text(run_text);

        /* Convert char_offset (paragraph space) to run-local offset. */
        int run_local_offset = char_offset - target_start;
        if(run_local_offset < 0)
            run_local_offset = 0;
        if(run_local_offset > (int)run_text.size())
            run_local_offset = (int)run_text.size();

        std::string new_run_text;
        if(op == 0) /* insert */
        {
            if(!text || !*text)
            {
                set_error("testo da inserire vuoto");
                return 0;
            }
            new_run_text = run_text.substr(0, run_local_offset);
            new_run_text += text;
            new_run_text += run_text.substr(run_local_offset);
        }
        else if(op == 1) /* backspace: delete before offset */
        {
            if(run_local_offset <= 0)
            {
                /* Nothing to delete in this run; caller should handle run merge */
                set_error("nessun carattere da cancellare");
                return 0;
            }
            new_run_text = run_text.substr(0, run_local_offset - 1);
            new_run_text += run_text.substr(run_local_offset);
        }
        else if(op == 2) /* forward delete: delete at offset */
        {
            if(run_local_offset >= (int)run_text.size())
            {
                set_error("nessun carattere da cancellare");
                return 0;
            }
            new_run_text = run_text.substr(0, run_local_offset);
            new_run_text += run_text.substr(run_local_offset + 1);
        }
        else
        {
            set_error("operazione non supportata");
            return 0;
        }

        /* Validate: no newlines in run text. */
        if(strchr(new_run_text.c_str(), '\n') || strchr(new_run_text.c_str(), '\r'))
        {
            set_error("il testo di un run non puo' contenere fine riga");
            return 0;
        }

        /* Inline edits during typing: disable OOXML persistence to avoid
         * rebuild on every keystroke. The edit is in-memory only.
         * The updated OOXML can be retrieved later via ld_get_document_xml.
         * This is achieved by skipping the ld_run_persist_ooxml call. */

        /* Cache and geometry become stale: invalidate BEFORE mutating DOM. */
        c->clear_paragraphs();
        h->m_rebuild_needed    = true;
        h->m_full_layout_done  = false;
        h->m_page_heights.clear();
        h->m_page_y_offsets.clear();
        h->m_last_rendered_page = -1;

        if(slot->is_text())
        {
            /* Run without formatting: replace the text node. */
            litehtml::element::ptr para = info->element;
            std::vector<litehtml::element::ptr> tail;
            bool seen = false;
            for(const auto& child : para->children())
            {
                if(child == slot)
                {
                    seen = true;
                    continue;
                }
                if(seen)
                {
                    tail.push_back(child);
                }
            }
            if(!para->removeChild(slot))
            {
                set_error("rimozione del run non riuscita");
                return 0;
            }
            append_plain_text_children(para, new_run_text);
            for(const auto& t : tail)
            {
                para->appendChild(t);
            }
        }
        else
        {
            /* Span reused: attributes and matched styles stay intact.
             * Use append_plain_text_children to split into words/spaces for wrapping. */
            slot->clearRecursive();
            append_plain_text_children(slot, new_run_text);
        }

        set_error("");
        return 1;
    } catch(...)
    {
        set_error("errore durante l'edit inline del run");
        return 0;
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
        try
        {
            prepare_render_tree(h);
            int w = max_width > 0 ? max_width : (h->container ? h->container->viewport_w : 800);
            if (h->container)
            {
                h->container->layout_width = w;
            }
            int rendered_w = static_cast<int>(h->doc->render(w));
            h->m_full_layout_done = true;
            if (h->container)
            {
                h->container->doc = h->doc;
                h->container->rebuild_table_cells();
                h->container->rebuild_paragraphs();
            }
            // Dopo il full layout, m_last_rendered_page viene settato da extract_page_geometry
            // quando verrà chiamato da ld_get_page_count/height
            h->m_page_heights.clear();
            h->m_page_y_offsets.clear();
            extract_page_geometry(h);
            return rendered_w;
        } catch(...)
        {
            set_error("errore durante il layout");
            return 0;
        }
    }
    
    LDLOG("ld_render_range: already laid out, returning width");
    // Already laid out
    return ld_width(doc);
}

void ld_invalidate_font_cache(void* doc)
{
    // Mark font cache as invalidated; actual clearing and render tree rebuild
    // happens on the worker thread at the start of the next ld_render call.
    auto* h = static_cast<ld_doc*>(doc);
    if (!h || !h->doc)
        return;
    h->m_fonts_invalidated = true;
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
/* ---------------------------------------------------------------
 * Paragraph HTML API (Opzione B: edit litehtml DOM)
 * --------------------------------------------------------------- */

// Serialize inner HTML of a paragraph (children only)
static std::string serialize_paragraph_inner_html(const litehtml::element::ptr& para)
{
    std::string out;
    for(const auto& child : para->children())
    {
        if(child->is_text())
        {
            std::string txt;
            child->get_text(txt);
            // Escape for text content
            for(char c : txt)
            {
                switch(c)
                {
                    case '&':  out += "&"; break;
                    case '<':  out += "<"; break;
                    case '>':  out += ">"; break;
                    default:   out.push_back(c);
                }
            }
        }
        else
        {
            const char* tag = child->get_tagName();
            if(tag && tag[0])
            {
                out.push_back('<');
                out += tag;
                auto attrs = child->dump_get_attrs();
                for(const auto& kv : attrs)
                {
                    out.push_back(' ');
                    out += std::get<0>(kv);
                    out += "=\"";
                    std::string val = std::get<1>(kv);
                    std::string escaped;
                    for(char c : val)
                    {
                        switch(c)
                        {
                            case '&':  escaped += "&"; break;
                            case '<':  escaped += "<"; break;
                            case '>':  escaped += ">"; break;
                            case '"':  escaped += "\""; break;
                            default:   escaped.push_back(c);
                        }
                    }
                    out += escaped;
                    out.push_back('"');
                }
                out.push_back('>');

                for(const auto& gc : child->children())
                {
                    // Recursively serialize - only handle text grandchildren
                    if(gc->is_text())
                    {
                        std::string txt;
                        gc->get_text(txt);
                        for(char c : txt)
                        {
                            switch(c)
                            {
                                case '&':  out += "&"; break;
                                case '<':  out += "<"; break;
                                case '>':  out += ">"; break;
                                default:   out.push_back(c);
                            }
                        }
                    }
                }

                static const char* void_tags[] = {"br", "img", "hr", "input", "meta", "link", nullptr};
                bool is_void = false;
                for(int i = 0; void_tags[i]; ++i)
                {
                    if(strcmp(tag, void_tags[i]) == 0)
                    {
                        is_void = true;
                        break;
                    }
                }
                if(!is_void)
                {
                    out += "</";
                    out += tag;
                    out.push_back('>');
                }
            }
        }
    }
    return out;
}

int ld_paragraph_get_xml(void* doc, int index, char** out_xml)
{
    if(!out_xml) return 0;
    *out_xml = nullptr;
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container || !h->container->paragraphs_valid)
    {
        return 0;
    }
    try
    {
        const ld_container::ParagraphInfo* info = h->container->find_paragraph(index);
        if(!info || !info->element)
        {
            return 0;
        }
        std::string html = serialize_paragraph_inner_html(info->element);
        *out_xml = ld_dup_utf8(html.c_str());
        return 1;
    } catch(...)
    {
        set_error("errore durante la serializzazione XML paragrafo");
        return 0;
    }
}

int ld_paragraph_set_xml(void* doc, int index, const char* xml)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->doc || !h->container || !xml)
    {
        set_error("parametri modifica XML paragrafo non validi");
        return 0;
    }

    try
    {
        ld_container* c = h->container;
        const ld_container::ParagraphInfo* info = c->find_paragraph(index);
        if(!info || !info->element)
        {
            set_error("indice paragrafo non valido");
            return 0;
        }

        litehtml::element::ptr para = info->element;

        // Find a run to keep for style
        litehtml::element::ptr style_run;
        litehtml::element::ptr style_fallback;
        for(const auto& child : para->children())
        {
            find_run_style_source(child, style_run, style_fallback);
        }
        litehtml::element::ptr keep = style_run ? style_run : style_fallback;

        // Invalidate layout
        c->clear_paragraphs();
        h->m_rebuild_needed    = true;
        h->m_full_layout_done  = false;
        h->m_page_heights.clear();
        h->m_page_y_offsets.clear();
        h->m_last_rendered_page = -1;

        // Clear paragraph
        para->clearRecursive();
        litehtml::element::ptr target = para;
        if(keep)
        {
            para->appendChild(keep);
            target = keep;
        }

        // Parse HTML fragment and append children
        // Wrap in a div to parse as fragment
        std::string wrapped = "<div>" + std::string(xml) + "</div>";
        auto temp_doc = litehtml::document::createFromString(wrapped.c_str(), nullptr, nullptr);
        if(temp_doc && temp_doc->root())
        {
            for(const auto& child : temp_doc->root()->children())
            {
                if(child && strcmp(child->get_tagName(), "div") == 0)
                {
                    for(const auto& gc : child->children())
                    {
                        std::string child_html;
                        // Serialize this child
                        if(gc->is_text())
                        {
                            std::string txt;
                            gc->get_text(txt);
                            child_html = txt;
                        }
                        else
                        {
                            const char* tag = gc->get_tagName();
                            if(tag && tag[0])
                            {
                                child_html.push_back('<');
                                child_html += tag;
                                auto attrs = gc->dump_get_attrs();
                                for(const auto& kv : attrs)
                                {
                                    child_html.push_back(' ');
                                    child_html += std::get<0>(kv);
                                    child_html += "=\"";
                                    std::string val = std::get<1>(kv);
                                    std::string escaped;
                                    for(char c : val)
                                    {
                                        switch(c)
                                        {
                                            case '&':  escaped += "&"; break;
                                            case '<':  escaped += "<"; break;
                                            case '>':  escaped += ">"; break;
                                            case '"':  escaped += "\""; break;
                                            default:   escaped.push_back(c);
                                        }
                                    }
                                    child_html += escaped;
                                    child_html.push_back('"');
                                }
                                child_html.push_back('>');
                                for(const auto& ggc : gc->children())
                                {
                                    if(ggc->is_text())
                                    {
                                        std::string txt;
                                        ggc->get_text(txt);
                                        for(char c : txt)
                                        {
                                            switch(c)
                                            {
                                                case '&':  child_html += "&"; break;
                                                case '<':  child_html += "<"; break;
                                                case '>':  child_html += ">"; break;
                                                default:   child_html.push_back(c);
                                            }
                                        }
                                    }
                                }
                                static const char* void_tags[] = {"br", "img", "hr", "input", "meta", "link", nullptr};
                                bool is_void = false;
                                for(int i = 0; void_tags[i]; ++i)
                                {
                                    if(strcmp(tag, void_tags[i]) == 0)
                                    {
                                        is_void = true;
                                        break;
                                    }
                                }
                                if(!is_void)
                                {
                                    child_html += "</";
                                    child_html += tag;
                                    child_html.push_back('>');
                                }
                            }
                        }
                        if(!child_html.empty())
                        {
                            auto parsed = litehtml::document::createFromString(child_html.c_str(), h->container, nullptr);
                            if(parsed && parsed->root())
                            {
                                for(const auto& pc : parsed->root()->children())
                                {
                                    target->appendChild(pc);
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }

        set_error("");
        return 1;
    } catch(...)
    {
        set_error("errore durante la modifica XML paragrafo");
        return 0;
    }
}

/* ld_paragraph_get_ooxml: raw OOXML <w:p> captured while loading the DOCX.
 * This is the real DOCX markup (w:p / w:r / w:t ...), not the litehtml
 * serialization returned by ld_paragraph_get_xml(). The caller must free the
 * string with ld_free_string(). Returns 1 on success. */
int ld_paragraph_get_ooxml(void* doc, int index, char** out_xml)
{
    if(!out_xml)
    {
        return 0;
    }
    *out_xml = nullptr;

    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->container)
    {
        set_error("parametri ld_paragraph_get_ooxml non validi");
        return 0;
    }

    const auto& map = h->container->paragraph_ooxml;
    auto        it  = map.find(index);
    if(it == map.end() || it->second.empty())
    {
        set_error("nessun OOXML originale per questo paragrafo");
        return 0;
    }

    *out_xml = ld_dup_utf8(it->second);
    if(!*out_xml)
    {
        set_error("memoria insufficiente");
        return 0;
    }
    set_error("");
    return 1;
}

/* Nucleo comune di ld_paragraph_set_ooxml: valida il frammento (un solo
 * w:p), lo splicca nel byte-range del paragrafo in model->document_xml e
 * ricostruisce l'intero albero render con rollback a fallimento.  Chiamato
 * sia dall'apply OOXML esplicito sia dalla persistenza dell'edit del run
 * (ld_run_set_text).  Il chiamante deve aver gia' verificato owned_model e
 * la lunghezza del frammento. */
static int ld_apply_paragraph_ooxml(ld_doc* h, ld_container* c, int index,
                                    const char* ooxml, size_t len)
{
    litedocx::DocxModel* model = c->owned_model.get();

    /* Validate the fragment: exactly one <w:p> and nothing else around it. */
    {
        std::unique_ptr<litedocx::XmlNode> frag = litedocx::xml_parse(ooxml, len);
        if(!frag || !frag->is("p"))
        {
            set_error("l'OOXML deve contenere un singolo elemento w:p");
            return 0;
        }
        if(frag->src_end <= frag->src_begin)
        {
            set_error("OOXML non bilanciato (tag w:p non chiuso)");
            return 0;
        }
        for(size_t i = frag->src_end; i < len; ++i)
        {
            const char ch = ooxml[i];
            if(ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            {
                set_error("l'OOXML deve contenere un solo w:p (trovato markup extra)");
                return 0;
            }
        }
    }

    auto range = c->paragraph_ranges.find(index);
    if(range == c->paragraph_ranges.end())
    {
        set_error("nessun intervallo OOXML per questo paragrafo");
        return 0;
    }
    const size_t begin = range->second.first;
    const size_t end   = range->second.second;
    if(begin >= end || end > model->document_xml.size())
    {
        set_error("intervallo OOXML non valido per questo paragrafo");
        return 0;
    }

    /* Splice the new fragment in place, remembering the old source so the
     * operation can be rolled back if the rebuild fails. */
    const std::string old_document_xml = model->document_xml;
    std::string new_document_xml;
    new_document_xml.reserve(old_document_xml.size() - (end - begin) + len);
    new_document_xml.append(old_document_xml, 0, begin);
    new_document_xml.append(ooxml, len);
    new_document_xml.append(old_document_xml, end, std::string::npos);

    litehtml::document::ptr new_doc;
    try
    {
        model->document_xml = std::move(new_document_xml);
        litedocx::DocxBuilder builder(*model, c);
        new_doc = builder.build();
        if(!new_doc)
        {
            model->document_xml = old_document_xml;
            set_error(builder.error().empty() ? "ricostruzione del documento fallita" : builder.error());
            return 0;
        }

        /* Swap the render tree in one shot. */
        c->clear_paragraphs();
        c->doc = new_doc;
        h->doc = new_doc;
        c->paragraph_ooxml  = builder.paragraph_ooxml();
        c->paragraph_ranges = builder.paragraph_ranges();

        h->page_w = builder.page_width();
        h->page_h = builder.page_height();
        h->m_page_height_px = builder.page_height();
        h->m_rebuild_needed     = true;
        h->m_full_layout_done   = false;
        h->m_page_heights.clear();
        h->m_page_y_offsets.clear();
        h->m_last_rendered_page = -1;
        /* FIX: clear text_width_cache after DOM rebuild.
         * The render tree has been rebuilt with new text content, but
         * the prefix-width cache still holds values computed from the
         * old text. Without this, ld_run_caret_hit will use stale
         * widths and return the pre-edit caret position. This is the
         * PRIMARY fix for the stale-caret bug -- it runs in the OOXML
         * persist path (the fast path used by inline editing). */
        c->text_width_cache.clear();
    } catch(const std::exception& ex)
    {
        model->document_xml = old_document_xml;
        set_error(ex.what());
        return 0;
    } catch(...)
    {
        model->document_xml = old_document_xml;
        set_error("errore durante la ricostruzione del paragrafo OOXML");
        return 0;
    }

    set_error("");
    return 1;
}

/* ld_paragraph_set_ooxml: replace the raw OOXML <w:p> of a paragraph and
 * rebuild the document.  The caller must later run a full ld_render() to
 * get new layout/pagination.  Returns 1 on success, 0 on failure (document
 * left untouched). */
int ld_paragraph_set_ooxml(void* doc, int index, const char* ooxml)
{
    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->container || !h->container->owned_model || !ooxml)
    {
        set_error("parametri ld_paragraph_set_ooxml non validi");
        return 0;
    }

    ld_container* c = h->container;

    const size_t len = std::strlen(ooxml);
    if(len == 0)
    {
        set_error("OOXML del paragrafo vuoto");
        return 0;
    }
    if(len > (size_t)1 << 20) // 1 MiB: un singolo paragrafo non dovrebbe mai essere cosi' grande
    {
        set_error("OOXML del paragrafo troppo grande");
        return 0;
    }

    return ld_apply_paragraph_ooxml(h, c, index, ooxml, len);
}

/* ld_get_document_xml: return the complete, up-to-date word/document.xml.
 * The model->document_xml is kept in sync by ld_paragraph_set_ooxml which
 * rebuilds it when a paragraph is edited. */
int ld_get_document_xml(void* doc, char** out_xml)
{
    if(!out_xml)
    {
        return 0;
    }
    *out_xml = nullptr;

    auto* h = static_cast<ld_doc*>(doc);
    if(!h || !h->container || !h->container->owned_model)
    {
        set_error("parametri ld_get_document_xml non validi");
        return 0;
    }

    const std::string& xml = h->container->owned_model->document_xml;
    if(xml.empty())
    {
        set_error("document.xml non disponibile");
        return 0;
    }

    *out_xml = ld_dup_utf8(xml);
    if(!*out_xml)
    {
        set_error("memoria insufficiente");
        return 0;
    }
    set_error("");
    return 1;
}
} // extern "C"
