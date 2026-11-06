#ifndef LITEDOCX_DOCX_BUILDER_H
#define LITEDOCX_DOCX_BUILDER_H

#include "docx_model.h"
#include "docx_style.h"
#include "xml_dom.h"

#include <litehtml.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>

namespace litedocx
{
    struct CellOwner
    {
        litehtml::element::ptr td;
        int                    rowspan = 1;
    };

    struct TableBorderInfo
    {
        BorderSide outer_top;
        BorderSide outer_left;
        BorderSide outer_bottom;
        BorderSide outer_right;
        BorderSide inside_h;
        BorderSide inside_v;
        int        cell_margin_t = -1;
        int        cell_margin_l = -1;
        int        cell_margin_b = -1;
        int        cell_margin_r = -1;
    };

    /* ---------------------------------------------------------------
     * StyleCache — deduplica style identici in classi CSS.
     * Invece di style="..." su ogni elemento, usa class="p3 s7".
     * --------------------------------------------------------------- */
    class StyleCache
    {
      public:
        StyleCache() : m_para_count(0), m_run_count(0) {}

        // ParaProps -> class name (es. "p12"). Restituisce class name vuoto se nessuna proprietà.
        const std::string& get_para_class(const ParaProps& p)
        {
            std::string key = para_props_to_key(p);
            if(key.empty()) return m_empty;
            auto it = m_para_map.find(key);
            if(it != m_para_map.end()) return it->second;
            ++m_para_count;
            std::string cls = "p" + std::to_string(m_para_count);
            auto result = m_para_map.emplace(std::move(key), cls);
            m_para_css.emplace_back(cls, para_props_to_css(p));
            return result.first->second;
        }

        // RunProps -> class name (es. "s5"). Restituisce class name vuoto se nessuna proprietà.
        const std::string& get_run_class(const RunProps& r)
        {
            std::string key = run_props_to_key(r);
            if(key.empty()) return m_empty;
            auto it = m_run_map.find(key);
            if(it != m_run_map.end()) return it->second;
            ++m_run_count;
            std::string cls = "s" + std::to_string(m_run_count);
            auto result = m_run_map.emplace(std::move(key), cls);
            m_run_css.emplace_back(cls, run_props_to_css(r));
            return result.first->second;
        }

        // Genera CSS combinato per tutte le classi raccolte.
        std::string generate_css() const
        {
            std::string css;
            css.reserve((m_para_css.size() + m_run_css.size() + m_dynamic_css.size()) * 128);
            for(const auto& kv : m_para_css) {
                css += ".";
                css += kv.first;
                css += "{";
                css += kv.second;
                css += "}\n";
            }
            for(const auto& kv : m_run_css) {
                css += ".";
                css += kv.first;
                css += "{";
                css += kv.second;
                css += "}\n";
            }
            for(const auto& kv : m_dynamic_css) {
                css += kv.first;
                css += "{";
                css += kv.second;
                css += "}\n";
            }
            return css;
        }

        // Per liste: crea/riusa classi per tag specifici (ul, ol, li, p) con CSS base fisso.
        const std::string& get_or_create_list_class(const char* tag, const std::string& base_css)
        {
            std::string key = std::string(tag) + "|" + base_css;
            auto it = m_list_map.find(key);
            if(it != m_list_map.end()) return it->second;
            ++m_list_count;
            std::string cls = "l" + std::to_string(m_list_count);
            auto result = m_list_map.emplace(std::move(key), cls);
            m_list_css.emplace_back(cls, base_css);
            return result.first->second;
        }

        // Regole CSS dinamiche per selettori arbitrari (es. list-style-type su ol specifico)
        void add_dynamic_rule(const std::string& selector, const std::string& css)
        {
            m_dynamic_css.emplace_back(selector, css);
        }

        void clear()
        {
            m_para_map.clear();
            m_run_map.clear();
            m_list_map.clear();
            m_para_css.clear();
            m_run_css.clear();
            m_list_css.clear();
            m_dynamic_css.clear();
            m_para_count = 0;
            m_run_count = 0;
            m_list_count = 0;
        }

      private:
        std::unordered_map<std::string, std::string> m_list_map;
        std::vector<std::pair<std::string, std::string>> m_list_css;
        std::vector<std::pair<std::string, std::string>> m_dynamic_css;
        int m_list_count = 0;

      private:
        static std::string para_props_to_key(const ParaProps& p)
        {
            std::ostringstream oss;
            oss << p.jc << '|'
                << p.ind_left << '|' << p.ind_right << '|' << p.ind_first << '|' << p.ind_hanging << '|'
                << p.space_before << '|' << p.space_after << '|' << p.line << '|' << p.line_rule << '|'
                << p.num_id << '|' << p.num_level << '|'
                << p.page_break_before << '|' << p.keep_next << '|'
                << p.shading << '|'
                << p.border_top.width_px << '|' << p.border_top.color << '|'
                << p.border_left.width_px << '|' << p.border_left.color << '|'
                << p.border_bottom.width_px << '|' << p.border_bottom.color << '|'
                << p.border_right.width_px << '|' << p.border_right.color;
            return oss.str();
        }

        static std::string run_props_to_key(const RunProps& r)
        {
            std::ostringstream oss;
            oss << r.bold << '|' << r.italic << '|' << r.underline << '|' << r.strike << '|'
                << r.caps << '|' << r.small_caps << '|'
                << r.size_half << '|' << r.vert_align << '|'
                << r.color << '|' << r.font << '|' << r.font_theme << '|'
                << r.highlight << '|' << r.shading << '|' << r.rstyle;
            return oss.str();
        }

        static std::string para_props_to_css(const ParaProps& p)
        {
            std::string css;
            std::string jc = litedocx::jc_to_css(p.jc);
            if(!jc.empty()) css += "text-align:" + jc + ";";
            if(p.ind_left >= 0)      { char b[32]; std::snprintf(b, sizeof(b), "margin-left:%dpx;", twip_to_px(p.ind_left)); css += b; }
            if(p.ind_right >= 0)     { char b[32]; std::snprintf(b, sizeof(b), "margin-right:%dpx;", twip_to_px(p.ind_right)); css += b; }
            int first = p.ind_first;
            if(p.ind_hanging >= 0) first = -p.ind_hanging;
            if(first >= 0) { char b[32]; std::snprintf(b, sizeof(b), "text-indent:%dpx;", twip_to_px(first)); css += b; }
            else if(first < 0) { char b[32]; std::snprintf(b, sizeof(b), "text-indent:%dpx;", -twip_to_px(-first)); css += b; }
            if(p.space_before >= 0)  { char b[32]; std::snprintf(b, sizeof(b), "margin-top:%dpx;", twip_to_px(p.space_before)); css += b; }
            if(p.space_after >= 0)   { char b[32]; std::snprintf(b, sizeof(b), "margin-bottom:%dpx;", twip_to_px(p.space_after)); css += b; }
            if(p.line > 0) {
                if(p.line_rule.empty() || p.line_rule == "auto") {
                    char b[64]; std::snprintf(b, sizeof(b), "line-height:%.3f;", p.line / 240.0); css += b;
                } else {
                    char b[32]; std::snprintf(b, sizeof(b), "line-height:%dpx;", twip_to_px(p.line)); css += b;
                }
            }
            if(!p.shading.empty()) css += "background-color:#" + p.shading + ";";
            auto add_b = [&](const char* prop, const BorderSide& s) {
                if(s.width_px < 0) return;
                char b[128];
                if(s.width_px == 0) std::snprintf(b, sizeof(b), "%s:0;", prop);
                else std::snprintf(b, sizeof(b), "%s:%dpx solid #%s;", prop, s.width_px, s.color.empty() ? "000000" : s.color.c_str());
                css += b;
            };
            add_b("border-top", p.border_top);
            add_b("border-left", p.border_left);
            add_b("border-bottom", p.border_bottom);
            add_b("border-right", p.border_right);
            return css;
        }

        static std::string run_props_to_css(const RunProps& r)
        {
            std::string css;
            if(r.bold == 1) css += "font-weight:bold;";
            else if(r.bold == 0) css += "font-weight:normal;";
            if(r.italic == 1) css += "font-style:italic;";
            else if(r.italic == 0) css += "font-style:normal;";
            std::string deco;
            if(r.underline == 1) deco += "underline ";
            if(r.strike == 1) deco += "line-through ";
            if(!deco.empty()) css += "text-decoration:" + deco + ";";
            if(!r.color.empty()) css += "color:#" + r.color + ";";
            if(!r.highlight.empty()) css += "background-color:#" + r.highlight + ";";
            else if(!r.shading.empty()) css += "background-color:#" + r.shading + ";";
            if(r.size_half > 0) { char b[32]; std::snprintf(b, sizeof(b), "font-size:%dpx;", half_point_to_px(r.size_half)); css += b; }
            std::string fam = r.font.empty() ? r.font_theme : r.font;
            if(!fam.empty()) {
                // escape quotes
                std::string q = "\"";
                for(char c : fam) { if(c == '"' || c == '\\') q.push_back('\\'); q.push_back(c); }
                q += "\"";
                css += "font-family:" + q + ";";
            }
            if(r.caps == 1 || r.small_caps == 1) css += "text-transform:uppercase;";
            if(r.vert_align == 1) css += "vertical-align:super;font-size:65%;";
            else if(r.vert_align == 2) css += "vertical-align:sub;font-size:65%;";
            return css;
        }

        std::unordered_map<std::string, std::string> m_para_map; // key -> class
        std::unordered_map<std::string, std::string> m_run_map;
        std::vector<std::pair<std::string, std::string>> m_para_css; // class -> css
        std::vector<std::pair<std::string, std::string>> m_run_css;
        int m_para_count = 0;
        int m_run_count = 0;
        static const std::string m_empty;
    };

    class DocxBuilder
    {
      public:
        DocxBuilder(DocxModel& model, litehtml::document_container* container);
        ~DocxBuilder();

        litehtml::document::ptr build();
        int                    page_width() const;
        int                    page_height() const;
        const std::string&     error() const
        {
            return m_error;
        }

      private:
        struct ListCtx
        {
            litehtml::element::ptr list;
            litehtml::element::ptr item;
            int                    level  = 0;
            int                    num_id = -1;
        };

        DocxModel&              m_model;
        litehtml::document::ptr m_doc;
        litehtml::element::ptr  m_root;
        litehtml::element::ptr  m_body;
        const XmlNode*          m_body_node = nullptr;
        std::string             m_error;
        int                     m_page_w_px = 0;
        int                     m_page_h_px = 0;
        StyleCache              m_style_cache;

        void parse_section();
        void build_body();
        void process_sequence(const litehtml::element::ptr& parent, const std::vector<const XmlNode*>& nodes);
        void process_block(const XmlNode* node, const litehtml::element::ptr& parent,
                           std::vector<ListCtx>& lists);
        void process_paragraph(const XmlNode* p, const litehtml::element::ptr& parent, bool in_list,
                               std::vector<ListCtx>& lists);
        void add_numbered_paragraph(const XmlNode* p, const ParaProps& props, const litehtml::element::ptr& parent,
                                    std::vector<ListCtx>& lists);
        void process_run(const XmlNode* r, const litehtml::element::ptr& parent, const ParaProps& para_props,
                         bool& content);
        void process_inline_children(const XmlNode* node, const litehtml::element::ptr& parent,
                                     const ParaProps& para_props, bool& content);
        void process_hyperlink(const XmlNode* link, const litehtml::element::ptr& parent,
                               const ParaProps& para_props, bool& content);
        void process_table(const XmlNode* tbl, const litehtml::element::ptr& parent);
        void process_drawing(const XmlNode* node, const litehtml::element::ptr& parent, bool& content);
        void process_math(const XmlNode* node, const litehtml::element::ptr& parent, bool& content);
        void process_bookmark(const XmlNode* node, const litehtml::element::ptr& parent);

        void append_text(const litehtml::element::ptr& parent, const std::string& text);
        void append_nbsp(const litehtml::element::ptr& parent);
        void add_page_break(const litehtml::element::ptr& parent);

        litehtml::element::ptr make_element(const char* tag, const std::string& css,
                                            const litehtml::string_map& attrs = litehtml::string_map());
        litehtml::element::ptr make_element(const char* tag, const std::string& css, const char* attr_name,
                                            const std::string& attr_value);

        // Nuove: usano cache classi invece di style inline
        const std::string& get_para_class(const ParaProps& p) { return m_style_cache.get_para_class(p); }
        const std::string& get_run_class(const RunProps& r) { return m_style_cache.get_run_class(r); }
        std::string font_family(const RunProps& r) const;
        static void flatten_blocks(const XmlNode* node, std::vector<const XmlNode*>& out);
    };

} // namespace litedocx

#endif // LITEDOCX_DOCX_BUILDER_H
