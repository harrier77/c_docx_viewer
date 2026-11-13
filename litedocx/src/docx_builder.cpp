#include "docx_builder.h"

#include "docx_css.h"
#include "docx_style.h"

#include <litehtml/el_space.h>
#include <litehtml/el_text.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace litedocx
{
    const std::string StyleCache::m_empty = "";

    namespace
    {
        void add_px(std::string& css, const char* prop, int px)
        {
            if(px < 0)
            {
                return;
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s:%dpx;", prop, px);
            css += buf;
        }

        void add_color(std::string& css, const char* prop, const std::string& color)
        {
            if(color.empty())
            {
                return;
            }
            css += prop;
            css += ":#";
            css += color;
            css += ";";
        }

        void add_border(std::string& css, const char* prop, const BorderSide& side)
        {
            if(side.width_px < 0)
            {
                return;
            }
            char buf[128];
            if(side.width_px == 0)
            {
                std::snprintf(buf, sizeof(buf), "%s:0;", prop);
            } else
            {
                std::snprintf(buf, sizeof(buf), "%s:%dpx solid #%s;", prop, side.width_px,
                              side.color.empty() ? "000000" : side.color.c_str());
            }
            css += buf;
        }

        std::string quoted_family(const std::string& s)
        {
            std::string out = "\"";
            for(char c : s)
            {
                if(c == '"' || c == '\\')
                {
                    out.push_back('\\');
                }
                out.push_back(c);
            }
            out += "\"";
            return out;
        }

        const XmlNode* find_descendant(const XmlNode* node, const std::string& local)
        {
            if(!node)
            {
                return nullptr;
            }
            for(const auto& c : node->children)
            {
                if(c->is(local))
                {
                    return c.get();
                }
                if(const XmlNode* r = find_descendant(c.get(), local))
                {
                    return r;
                }
            }
            return nullptr;
        }

        void collect_text(const XmlNode* node, std::string& out)
        {
            if(!node)
            {
                return;
            }
            if(node->is("t") || node->is("instrText"))
            {
                out += node->text;
            }
            for(const auto& c : node->children)
            {
                collect_text(c.get(), out);
            }
        }

        BorderSide parse_border_node(const XmlNode* n)
        {
            BorderSide b;
            if(!n)
            {
                return b;
            }
            std::string val = n->attr("w:val", "single");
            if(val == "none" || val == "nil")
            {
                b.width_px = 0;
            } else
            {
                b.width_px = eighth_point_to_px(std::atoi(n->attr("w:sz", "4").c_str()));
                if(b.width_px < 1)
                {
                    b.width_px = 1;
                }
            }
            b.color = norm_color(n->attr("w:color", "000000"));
            if(b.color.empty())
            {
                b.color = "000000";
            }
            b.space_pt = std::atoi(n->attr("w:space", "0").c_str());
            return b;
        }

        struct CellInfo
        {
            int         span       = 1;
            int         vmerge     = 0; // 0 none, 1 restart, 2 continue
            int         width_px   = -1;
            double      width_pct  = -1;
            std::string shading;
            std::string valign;
            BorderSide  top;
            BorderSide  left;
            BorderSide  bottom;
            BorderSide  right;
            int         mar_t = -1;
            int         mar_l = -1;
            int         mar_b = -1;
            int         mar_r = -1;
        };

        CellInfo parse_cell_info(const XmlNode* tc)
        {
            CellInfo ci;
            const XmlNode* pr = tc ? tc->child("tcPr") : nullptr;
            if(!pr)
            {
                return ci;
            }
            if(const XmlNode* n = pr->child("gridSpan"))
            {
                ci.span = std::atoi(n->attr("w:val", "1").c_str());
                if(ci.span < 1)
                {
                    ci.span = 1;
                }
                if(ci.span > 64)
                {
                    ci.span = 64;
                }
            }
            if(const XmlNode* n = pr->child("vMerge"))
            {
                ci.vmerge = n->attr("w:val") == "restart" ? 1 : 2;
            }
            if(const XmlNode* n = pr->child("tcW"))
            {
                std::string type = n->attr("w:type", "dxa");
                int         w    = std::atoi(n->attr("w:w", "0").c_str());
                if(type == "pct")
                {
                    ci.width_pct = w / 50.0;
                } else if(type == "dxa")
                {
                    ci.width_px = twip_to_px(w);
                }
            }
            if(const XmlNode* n = pr->child("shd"))
            {
                ci.shading = norm_color(n->attr("w:fill"));
            }
            if(const XmlNode* n = pr->child("vAlign"))
            {
                ci.valign = n->attr("w:val");
            }
            if(const XmlNode* n = pr->child("tcMar"))
            {
                auto m = [&](const char* name) {
                    const XmlNode* x = n->child(name);
                    return x ? twip_to_px(std::atoi(x->attr("w:w", "0").c_str())) : -1;
                };
                ci.mar_t = m("top");
                ci.mar_l = m("left");
                if(ci.mar_l < 0)
                    ci.mar_l = m("start");
                ci.mar_b = m("bottom");
                ci.mar_r = m("right");
                if(ci.mar_r < 0)
                    ci.mar_r = m("end");
            }
            if(const XmlNode* b = pr->child("tcBorders"))
            {
                ci.top    = parse_border_node(b->child("top"));
                ci.left   = parse_border_node(b->child("left"));
                if(ci.left.width_px < 0)
                    ci.left = parse_border_node(b->child("start"));
                ci.bottom = parse_border_node(b->child("bottom"));
                ci.right  = parse_border_node(b->child("right"));
                if(ci.right.width_px < 0)
                    ci.right = parse_border_node(b->child("end"));
            }
            return ci;
        }

        void collect_rows(const XmlNode* node, std::vector<const XmlNode*>& out)
        {
            if(!node)
            {
                return;
            }
            for(const auto& c : node->children)
            {
                if(c->is("tr"))
                {
                    out.push_back(c.get());
                } else if(c->is("sdt") || c->is("sdtContent") || c->is("customXml"))
                {
                    collect_rows(c.get(), out);
                }
            }
        }
    } // namespace

    DocxBuilder::DocxBuilder(DocxModel& model, litehtml::document_container* container) :
        m_model(model)
    {
        m_doc = std::make_shared<litehtml::document>(container);
        m_doc->set_document_mode(litehtml::no_quirks_mode);
        m_root = m_doc->create_element("html", litehtml::string_map());
        m_body = m_doc->create_element("body", litehtml::string_map());
        if(m_root && m_body)
        {
            m_root->appendChild(m_body);
        }
    }

    DocxBuilder::~DocxBuilder() = default;

    litehtml::document::ptr DocxBuilder::build()
    {
        if(!m_root || !m_body)
        {
            m_error = "creazione dell'albero litehtml fallita";
            return nullptr;
        }

        m_style_cache.clear();

        auto dom = xml_parse(m_model.document_xml.c_str(), m_model.document_xml.size());
        if(!dom)
        {
            m_error = "word/document.xml non e' un XML valido";
            return nullptr;
        }
        const XmlNode* doc_node = dom->is("document") ? dom.get() : dom->child("document");
        m_body_node             = doc_node ? doc_node->child("body") : nullptr;
        if(!m_body_node)
        {
            m_error = "word/document.xml non contiene w:body";
            return nullptr;
        }
        parse_section();

        std::string body_css;
        add_px(body_css, "padding-top", twip_to_px(m_model.section.margin_t_tw));
        /* Ripristiniamo i margini originali del documento Word per il padding del body,
         * così il testo riacquista il suo posizionamento naturale rispetto alla pagina. */
        add_px(body_css, "padding-right", twip_to_px(m_model.section.margin_r_tw));
        add_px(body_css, "padding-bottom", twip_to_px(m_model.section.margin_b_tw));
        add_px(body_css, "padding-left", twip_to_px(m_model.section.margin_l_tw));
        body_css += "background:#ffffff;";
        m_body->set_attr("style", body_css.c_str());

        build_body();

        std::string base_css = docx_base_css(m_model);
        base_css += m_style_cache.generate_css();
        m_doc->finalize_from_external_root(m_root, litehtml::master_css, base_css);
        return m_doc;
    }

    int DocxBuilder::page_width() const
    {
        return m_page_w_px;
    }

    int DocxBuilder::page_height() const
    {
        return m_page_h_px;
    }

    void DocxBuilder::parse_section()
    {
        Section& s = m_model.section;
        const XmlNode* sect = nullptr;
        if(m_body_node)
        {
            for(const auto& c : m_body_node->children)
            {
                if(c->is("sectPr"))
                {
                    sect = c.get();
                }
            }
        }
        if(sect)
        {
            if(const XmlNode* n = sect->child("pgSz"))
            {
                if(n->has("w:w"))
                    s.page_w_tw = std::atoi(n->attr("w:w").c_str());
                if(n->has("w:h"))
                    s.page_h_tw = std::atoi(n->attr("w:h").c_str());
            }
            if(const XmlNode* n = sect->child("pgMar"))
            {
                auto get = [&](const char* name, int& dst) {
                    if(n->has(name))
                        dst = std::atoi(n->attr(name).c_str());
                };
                get("w:top", s.margin_t_tw);
                get("w:bottom", s.margin_b_tw);
                get("w:left", s.margin_l_tw);
                get("w:right", s.margin_r_tw);
                get("w:header", s.header_tw);
                get("w:footer", s.footer_tw);
            }
        }
        m_page_w_px = twip_to_px(s.page_w_tw);
        m_page_h_px = twip_to_px(s.page_h_tw);
        if(m_page_w_px < 100)
            m_page_w_px = 816;
        if(m_page_h_px < 100)
            m_page_h_px = 1056;
    }

    void DocxBuilder::build_body()
    {
        std::vector<const XmlNode*> blocks;
        for(const auto& c : m_body_node->children)
        {
            flatten_blocks(c.get(), blocks);
        }
        process_sequence(m_body, blocks);
        if(m_body->children().empty())
        {
            auto p = make_element("p", std::string());
            m_body->appendChild(p);
            append_nbsp(p);
        }
    }

    void DocxBuilder::flatten_blocks(const XmlNode* node, std::vector<const XmlNode*>& out)
    {
        if(!node)
        {
            return;
        }
        if(node->is("p") || node->is("tbl"))
        {
            out.push_back(node);
            return;
        }
        if(node->is("sdt"))
        {
            if(const XmlNode* content = node->child("sdtContent"))
            {
                flatten_blocks(content, out);
            }
            return;
        }
        if(node->is("sdtContent") || node->is("customXml") || node->is("ins") || node->is("moveTo"))
        {
            for(const auto& c : node->children)
            {
                flatten_blocks(c.get(), out);
            }
        }
    }

    void DocxBuilder::process_sequence(const litehtml::element::ptr& parent, const std::vector<const XmlNode*>& nodes)
    {
        std::vector<ListCtx> lists;
        for(const auto* node : nodes)
        {
            process_block(node, parent, lists);
        }
    }

    void DocxBuilder::process_block(const XmlNode* node, const litehtml::element::ptr& parent,
                                    std::vector<ListCtx>& lists)
    {
        if(!node)
        {
            return;
        }
        if(node->is("p"))
        {
            ParaProps direct;
            parse_para_props(node->child("pPr"), direct);
            ParaProps resolved = resolve_para(m_model, direct);
            if(resolved.num_id >= 0 && resolved.num_level >= 0)
            {
                add_numbered_paragraph(node, resolved, parent, lists);
            } else
            {
                lists.clear();
                process_paragraph(node, parent, false, lists);
            }
            return;
        }
        if(node->is("tbl"))
        {
            lists.clear();
            process_table(node, parent);
        }
    }

    void DocxBuilder::add_numbered_paragraph(const XmlNode* p, const ParaProps& props,
                                             const litehtml::element::ptr& parent, std::vector<ListCtx>& lists)
    {
        int level  = std::max(0, std::min(props.num_level, 8));
        int num_id = props.num_id;

        while(static_cast<int>(lists.size()) > level + 1)
        {
            lists.pop_back();
        }
        while(!lists.empty() && static_cast<int>(lists.size()) == level + 1 && lists.back().num_id != num_id)
        {
            lists.pop_back();
        }

        // Classi CSS riutilizzabili per liste
        const std::string list_ul_cls = m_style_cache.get_or_create_list_class("ul", "margin:0;padding-left:20px;");
        const std::string list_ol_cls = m_style_cache.get_or_create_list_class("ol", "margin:0;padding-left:20px;");
        const std::string li_cls      = m_style_cache.get_or_create_list_class("li", "margin:0;padding:0;");
        const std::string p_cls       = m_style_cache.get_or_create_list_class("p", "margin:0;padding:0;");

        size_t old_size = lists.size();
        for(int l = static_cast<int>(old_size); l <= level; ++l)
        {
            litehtml::element::ptr container =
                l == 0 ? parent : (lists[static_cast<size_t>(l - 1)].item ? lists[static_cast<size_t>(l - 1)].item
                                                                          : lists[static_cast<size_t>(l - 1)].list);
            const NumberingLevel* nl = find_numbering_level(m_model, num_id, l);
            bool                  bullet = nl ? list_is_bullet(nl->format) : false;
            
            litehtml::string_map attrs;
            litehtml::element::ptr list;
            if(bullet)
            {
                list = make_element("ul", std::string(), attrs);
                list->set_attr("class", list_ul_cls.c_str());
            } else
            {
                if(nl && nl->start != 1)
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%d", nl->start);
                    attrs["start"] = buf;
                }
                list = make_element("ol", std::string(), attrs);
                list->set_attr("class", list_ol_cls.c_str());
                // list-style-type verrà aggiunto via CSS dinamico se necessario
                if(nl) {
                    std::string lstype = list_style_for_format(nl->format);
                    if(!lstype.empty()) {
                        m_style_cache.add_dynamic_rule("." + list_ol_cls, "list-style-type:" + lstype + ";");
                    }
                }
            }
            container->appendChild(list);

            ListCtx ctx;
            ctx.list   = list;
            ctx.level  = l;
            ctx.num_id = num_id;
            lists.push_back(ctx);

            if(l < level)
            {
                auto li = make_element("li", std::string());
                li->set_attr("class", li_cls.c_str());
                list->appendChild(li);
                auto sp = make_element("p", std::string());
                sp->set_attr("class", p_cls.c_str());
                li->appendChild(sp);
                append_nbsp(sp);
                lists.back().item = li;
            }
        }

        if(static_cast<int>(lists.size()) > level)
        {
            auto li = make_element("li", std::string());
            li->set_attr("class", li_cls.c_str());
            lists[static_cast<size_t>(level)].list->appendChild(li);
            lists[static_cast<size_t>(level)].item = li;
            process_paragraph(p, li, true, lists);
        }
    }

    void DocxBuilder::process_paragraph(const XmlNode* p, const litehtml::element::ptr& parent, bool in_list,
                                        std::vector<ListCtx>& lists)
    {
        (void)lists;
        (void)in_list;
        ParaProps direct;
        parse_para_props(p->child("pPr"), direct);
        ParaProps props = resolve_para(m_model, direct);

        if(props.page_break_before)
        {
            add_page_break(parent);
        }

        const std::string& para_cls = get_para_class(props);
        litehtml::string_map attrs;
        if(!para_cls.empty()) attrs["class"] = para_cls;
        auto        para = make_element("p", std::string(), attrs);
        /* Stable identity for the C ABI paragraph editor.  Only paragraphs
         * that come from a source w:p are indexed; builder-only placeholders
         * are left untagged on purpose. */
        para->set_attr("data-ld-paragraph-index", std::to_string(m_paragraph_index++).c_str());
        parent->appendChild(para);

        bool content = false;
        for(const auto& c : p->children)
        {
            if(c->is("pPr"))
            {
                continue;
            }
            process_inline_children(c.get(), para, props, content);
        }
        if(!content || para->children().empty())
        {
            append_nbsp(para);
        }
    }

    void DocxBuilder::process_inline_children(const XmlNode* node, const litehtml::element::ptr& parent,
                                              const ParaProps& para_props, bool& content)
    {
        if(!node)
        {
            return;
        }
        if(node->is("r"))
        {
            process_run(node, parent, para_props, content);
            return;
        }
        if(node->is("hyperlink"))
        {
            process_hyperlink(node, parent, para_props, content);
            return;
        }
        if(node->is("bookmarkStart"))
        {
            process_bookmark(node, parent);
            return;
        }
        if(node->is("del") || node->is("moveFrom") || node->is("commentRangeStart") ||
           node->is("commentRangeEnd") || node->is("commentReference") || node->is("bookmarkEnd") ||
           node->is("proofErr") || node->is("lastRenderedPageBreak") || node->is("fldChar") ||
           node->is("instrText") || node->is("delText"))
        {
            return;
        }
        if(node->is("sdt"))
        {
            if(const XmlNode* c = node->child("sdtContent"))
            {
                for(const auto& cc : c->children)
                {
                    process_inline_children(cc.get(), parent, para_props, content);
                }
            }
            return;
        }
        if(node->is("ins") || node->is("moveTo") || node->is("smartTag") || node->is("customXml") ||
           node->is("fldSimple") || node->is("sdtContent") || node->is("dir") || node->is("bdo"))
        {
            for(const auto& cc : node->children)
            {
                process_inline_children(cc.get(), parent, para_props, content);
            }
            return;
        }
        if(node->is("oMath") || node->is("oMathPara"))
        {
            process_math(node, parent, content);
            return;
        }
        if(node->is("drawing") || node->is("pict") || node->is("object"))
        {
            process_drawing(node, parent, content);
            return;
        }
    }

    void DocxBuilder::process_run(const XmlNode* r, const litehtml::element::ptr& parent,
                                  const ParaProps& para_props, bool& content)
    {
        RunProps direct;
        parse_run_props(r->child("rPr"), direct);
        RunProps props = resolve_run(m_model, para_props, direct);

        const std::string& run_cls = get_run_class(props);
        litehtml::string_map attrs;
        if(!run_cls.empty()) attrs["class"] = run_cls;
        auto span = make_element("span", std::string(), attrs);
        parent->appendChild(span);

        for(const auto& c : r->children)
        {
            if(c->is("rPr"))
            {
                continue;
            }
            if(c->is("t"))
            {
                std::string text = c->text;
                if(!text.empty())
                {
                    append_text(span, text);
                    content = true;
                }
                continue;
            }
            if(c->is("br") || c->is("cr"))
            {
                if(c->is("br") && c->attr("w:type") == "page")
                {
                    auto br = make_element("br", std::string());
                    span->appendChild(br);
                    add_page_break(parent);
                } else
                {
                    auto br = make_element("br", std::string());
                    span->appendChild(br);
                }
                content = true;
                continue;
            }
            if(c->is("tab"))
            {
                append_text(span, "    ");
                content = true;
                continue;
            }
            if(c->is("noBreakHyphen"))
            {
                auto t = std::make_shared<litehtml::el_text>("-", m_doc);
                span->appendChild(t);
                content = true;
                continue;
            }
            if(c->is("drawing") || c->is("pict") || c->is("object"))
            {
                process_drawing(c.get(), span, content);
                continue;
            }
            if(c->is("sym"))
            {
                std::string ch = c->attr("w:char");
                if(!ch.empty())
                {
                    // Word stores the Unicode value in hex in w:char.
                    unsigned cp = static_cast<unsigned>(std::strtoul(ch.c_str(), nullptr, 16));
                    if(cp > 0xF000)
                    {
                        cp -= 0xF000;
                    }
                    std::string u;
                    if(cp < 0x80)
                    {
                        u.push_back(static_cast<char>(cp));
                    } else if(cp < 0x800)
                    {
                        u.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        u.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else
                    {
                        u.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        u.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        u.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    append_text(span, u);
                    content = true;
                }
                continue;
            }
        }
    }

    void DocxBuilder::process_hyperlink(const XmlNode* link, const litehtml::element::ptr& parent,
                                        const ParaProps& para_props, bool& content)
    {
        std::string href;
        std::string rid = link->attr("r:id");
        if(!rid.empty())
        {
            if(const Relationship* rel = m_model.rels.find(rid))
            {
                href = rel->target;
            }
        }
        if(href.empty())
        {
            std::string anchor = link->attr("w:anchor");
            if(!anchor.empty())
            {
                href = "#" + anchor;
            }
        }

        litehtml::element::ptr target = parent;
        if(!href.empty())
        {
            litehtml::string_map attrs;
            attrs["href"] = href;
            target        = make_element("a", std::string(), attrs);
            parent->appendChild(target);
        }

        bool local = false;
        for(const auto& c : link->children)
        {
            process_inline_children(c.get(), target, para_props, local);
        }
        content = content || local;
    }

    void DocxBuilder::process_bookmark(const XmlNode* node, const litehtml::element::ptr& parent)
    {
        std::string name = node->attr("w:name");
        if(name.empty() || name == "_GoBack")
        {
            return;
        }
        litehtml::string_map attrs;
        attrs["id"]        = name;
        attrs["data-field"] = name;
        auto a = make_element("a", "display:inline;", attrs);
        parent->appendChild(a);
    }

    void DocxBuilder::process_drawing(const XmlNode* node, const litehtml::element::ptr& parent, bool& content)
    {
        std::string       rid;
        const XmlNode*    blip = find_descendant(node, "blip");
        if(blip)
        {
            rid = blip->attr("r:embed");
            if(rid.empty())
            {
                rid = blip->attr("r:link");
            }
        }
        if(rid.empty())
        {
            const XmlNode* imagedata = find_descendant(node, "imagedata");
            if(imagedata)
            {
                rid = imagedata->attr("r:id");
            }
        }

        int width  = 0;
        int height = 0;
        const XmlNode* extent = find_descendant(node, "extent");
        if(extent)
        {
            width  = emu_to_px(std::atoll(extent->attr("cx", "0").c_str()));
            height = emu_to_px(std::atoll(extent->attr("cy", "0").c_str()));
        }
        if(width <= 0 || height <= 0)
        {
            const XmlNode* ext = find_descendant(node, "ext");
            if(ext)
            {
                if(width <= 0)
                    width = emu_to_px(std::atoll(ext->attr("cx", "0").c_str()));
                if(height <= 0)
                    height = emu_to_px(std::atoll(ext->attr("cy", "0").c_str()));
            }
        }

        std::string media_path;
        if(!rid.empty())
        {
            if(const Relationship* rel = m_model.rels.find(rid))
            {
                media_path = rel->resolved;
            }
        }

        if(!media_path.empty() && m_model.media.find(media_path) != m_model.media.end())
        {
            std::string css;
            add_px(css, "width", width);
            add_px(css, "height", height);
            litehtml::string_map attrs;
            attrs["src"] = media_path;
            auto img     = make_element("img", css, attrs);
            parent->appendChild(img);
            content = true;
            return;
        }

        // Unsupported object / missing image: keep a visible placeholder.
        litehtml::string_map attrs;
        std::string           css = "display:inline-block;";
        if(width > 0 && height > 0)
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "width:%dpx;height:%dpx;", width, height);
            css += buf;
        } else
        {
            css += "width:120px;height:64px;";
        }
        css += "background:#f2f4f7;border:1px solid #c9ced6;color:#6b7280;font-size:11px;text-align:center;";
        auto sp = make_element("span", css);
        sp->appendChild(std::make_shared<litehtml::el_text>("[immagine]", m_doc));
        parent->appendChild(sp);
        content = true;
    }

    void DocxBuilder::process_math(const XmlNode* node, const litehtml::element::ptr& parent, bool& content)
    {
        std::string text;
        collect_text(node, text);
        if(text.empty())
        {
            text = "[formula]";
        }
        auto span = make_element("span", "font-style:italic;color:#444;");
        span->appendChild(std::make_shared<litehtml::el_text>(text.c_str(), m_doc));
        parent->appendChild(span);
        content = true;
    }

    void DocxBuilder::process_table(const XmlNode* tbl, const litehtml::element::ptr& parent)
    {
        TableBorderInfo bi;
        std::string     table_css = "border-collapse:collapse;border-spacing:0;";
        const XmlNode*  pr        = tbl->child("tblPr");
        if(pr)
        {
            if(const XmlNode* n = pr->child("tblW"))
            {
                std::string type = n->attr("w:type", "auto");
                int         w    = std::atoi(n->attr("w:w", "0").c_str());
                if(type == "dxa" && w > 0)
                {
                    add_px(table_css, "width", twip_to_px(w));
                } else if(type == "pct" && w > 0)
                {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "width:%.2f%%;", w / 50.0);
                    table_css += buf;
                }
            }
            if(const XmlNode* n = pr->child("jc"))
            {
                std::string jc = n->attr("w:val");
                if(jc == "center")
                {
                    table_css += "margin-left:auto;margin-right:auto;";
                } else if(jc == "right" || jc == "end")
                {
                    table_css += "margin-left:auto;";
                }
            }
            if(const XmlNode* n = pr->child("shd"))
            {
                add_color(table_css, "background-color", norm_color(n->attr("w:fill")));
            }
            if(const XmlNode* b = pr->child("tblBorders"))
            {
                bi.outer_top    = parse_border_node(b->child("top"));
                bi.outer_left   = parse_border_node(b->child("left"));
                if(bi.outer_left.width_px < 0)
                    bi.outer_left = parse_border_node(b->child("start"));
                bi.outer_bottom = parse_border_node(b->child("bottom"));
                bi.outer_right  = parse_border_node(b->child("right"));
                if(bi.outer_right.width_px < 0)
                    bi.outer_right = parse_border_node(b->child("end"));
                bi.inside_h     = parse_border_node(b->child("insideH"));
                bi.inside_v     = parse_border_node(b->child("insideV"));
            }
            if(const XmlNode* m = pr->child("tblCellMar"))
            {
                auto get = [&](const char* name) {
                    const XmlNode* n = m->child(name);
                    return n ? twip_to_px(std::atoi(n->attr("w:w", "0").c_str())) : -1;
                };
                bi.cell_margin_t = get("top");
                bi.cell_margin_l = get("left");
                if(bi.cell_margin_l < 0)
                    bi.cell_margin_l = get("start");
                bi.cell_margin_b = get("bottom");
                bi.cell_margin_r = get("right");
                if(bi.cell_margin_r < 0)
                    bi.cell_margin_r = get("end");
            }
        }
        add_border(table_css, "border-top", bi.outer_top);
        add_border(table_css, "border-left", bi.outer_left);
        add_border(table_css, "border-bottom", bi.outer_bottom);
        add_border(table_css, "border-right", bi.outer_right);
        for(const auto& c : tbl->children)
        {
            if(c->is("tblPr") && c->child("tblStyle"))
            {
                // Table style borders are not fully expanded; direct properties above win.
            }
        }

        // Parse tblGrid → preferred column widths (in px). Critical for correct column count & layout.
        std::vector<int> grid_widths_px;
        if(const XmlNode* grid = tbl->child("tblGrid"))
        {
            for(const auto& gc : grid->children)
            {
                if(!gc->is("gridCol"))
                    continue;
                int w_tw = std::atoi(gc->attr("w:w", "0").c_str());
                grid_widths_px.push_back(w_tw > 0 ? twip_to_px(w_tw) : 0);
            }
        }
        const int grid_cols = static_cast<int>(grid_widths_px.size());

        // Force fixed layout when we know the grid – prevents litehtml from collapsing columns.
        if(grid_cols > 0)
        {
            table_css += "table-layout:fixed;";
            int total_w = 0;
            for(int w : grid_widths_px)
                total_w += w;
            if(total_w > 0)
                add_px(table_css, "width", total_w);
        }

        auto table = make_element("table", table_css);
        parent->appendChild(table);

        // <colgroup> helps establish the column structure early.
        if(grid_cols > 0)
        {
            auto colgroup = make_element("colgroup", std::string());
            for(int w : grid_widths_px)
            {
                std::string col_css;
                if(w > 0)
                    add_px(col_css, "width", w);
                auto col = make_element("col", col_css);
                colgroup->appendChild(col);
            }
            table->appendChild(colgroup);
        }

        /* Table rows must be wrapped in tbody/thead/tfoot for litehtml */
        auto tbody = make_element("tbody", std::string());
        table->appendChild(tbody);

        std::vector<const XmlNode*> rows;
        collect_rows(tbl, rows);
        if(rows.empty())
        {
            auto tr = make_element("tr", std::string());
            auto td = make_element("td", "padding:2px 5px;");
            auto p  = make_element("p", "margin:0;padding:0;");
            td->appendChild(p);
            append_nbsp(p);
            tr->appendChild(td);
            tbody->appendChild(tr);
            return;
        }

        std::vector<CellOwner> owners;
        if(grid_cols > 0)
            owners.resize(static_cast<size_t>(grid_cols));
        int row_index = 0;
        for(const auto* row : rows)
        {
            std::string row_css;
            if(const XmlNode* rp = row->child("trPr"))
            {
                if(const XmlNode* h = rp->child("trHeight"))
                {
                    int px = twip_to_px(std::atoi(h->attr("w:val", "0").c_str()));
                    add_px(row_css, "height", px);
                }
                if(const XmlNode* shd = rp->child("shd"))
                {
                    add_color(row_css, "background-color", norm_color(shd->attr("w:fill")));
                }
            }
            auto tr = make_element("tr", row_css);
            tbody->appendChild(tr);

            int col = 0;
            std::vector<litehtml::element*> vmerge_touched;
            for(const auto& c : row->children)
            {
                if(!c->is("tc"))
                {
                    continue;
                }
                CellInfo ci = parse_cell_info(c.get());
                if(ci.vmerge == 2 && col < static_cast<int>(owners.size()) && owners[static_cast<size_t>(col)].td)
                {
                    auto& owner = owners[static_cast<size_t>(col)];
                    if(std::find(vmerge_touched.begin(), vmerge_touched.end(), owner.td.get()) == vmerge_touched.end())
                    {
                        owner.rowspan++;
                        owner.td->set_attr("rowspan", std::to_string(owner.rowspan).c_str());
                        vmerge_touched.push_back(owner.td.get());
                    }
                    col += ci.span;
                    continue;
                }

                auto td = make_element("td", std::string());
                if(ci.span > 1)
                {
                    td->set_attr("colspan", std::to_string(ci.span).c_str());
                }
                if(ci.vmerge == 1)
                {
                    td->set_attr("rowspan", "1");
                }

                std::string css;
                if(ci.width_px > 0)
                {
                    add_px(css, "width", ci.width_px);
                } else if(ci.width_pct > 0)
                {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "width:%.2f%%;", ci.width_pct);
                    css += buf;
                } else if(grid_cols > 0 && col < grid_cols)
                {
                    // Fallback: sum of the grid columns this cell spans
                    int sum = 0;
                    int end = std::min(col + ci.span, grid_cols);
                    for(int k = col; k < end; ++k)
                        sum += grid_widths_px[static_cast<size_t>(k)];
                    if(sum > 0)
                        add_px(css, "width", sum);
                }
                add_color(css, "background-color", ci.shading);
                int pt = ci.mar_t >= 0 ? ci.mar_t : (bi.cell_margin_t >= 0 ? bi.cell_margin_t : 2);
                int pl = ci.mar_l >= 0 ? ci.mar_l : (bi.cell_margin_l >= 0 ? bi.cell_margin_l : 5);
                int pb = ci.mar_b >= 0 ? ci.mar_b : (bi.cell_margin_b >= 0 ? bi.cell_margin_b : 2);
                int prr = ci.mar_r >= 0 ? ci.mar_r : (bi.cell_margin_r >= 0 ? bi.cell_margin_r : 5);
                char pad[96];
                std::snprintf(pad, sizeof(pad), "padding:%dpx %dpx %dpx %dpx;", pt, prr, pb, pl);
                css += pad;
                if(ci.valign == "center")
                    css += "vertical-align:middle;";
                else if(ci.valign == "bottom")
                    css += "vertical-align:bottom;";
                else
                    css += "vertical-align:top;";

                // Cell borders: explicit tcBorders, otherwise table inside borders.
                if(ci.top.width_px >= 0)
                    add_border(css, "border-top", ci.top);
                else
                    add_border(css, "border-top", bi.inside_h);
                if(ci.left.width_px >= 0)
                    add_border(css, "border-left", ci.left);
                if(ci.bottom.width_px >= 0)
                    add_border(css, "border-bottom", ci.bottom);
                else
                    add_border(css, "border-bottom", bi.inside_h);
                if(ci.right.width_px >= 0)
                    add_border(css, "border-right", ci.right);
                else
                    add_border(css, "border-right", bi.inside_v);

                td->set_attr("style", css.c_str());
                tr->appendChild(td);

                if(col + ci.span > static_cast<int>(owners.size()))
                {
                    owners.resize(static_cast<size_t>(col + ci.span));
                }
                for(int k = 0; k < ci.span; ++k)
                {
                    owners[static_cast<size_t>(col + k)] = CellOwner();
                    if(ci.vmerge == 1)
                    {
                        owners[static_cast<size_t>(col + k)].td      = td;
                        owners[static_cast<size_t>(col + k)].rowspan = 1;
                    }
                }
                col += ci.span;

                std::vector<const XmlNode*> blocks;
                for(const auto& cc : c->children)
                {
                    if(cc->is("tcPr"))
                    {
                        continue;
                    }
                    flatten_blocks(cc.get(), blocks);
                }
                if(blocks.empty())
                {
                    auto p = make_element("p", "margin:0;padding:0;");
                    td->appendChild(p);
                    append_nbsp(p);
                } else
                {
                    process_sequence(td, blocks);
                }
            }

            // Ensure the row covers the full grid: emit empty cells for any remaining columns
            // that are not covered by a continuing vertical merge.
            if(grid_cols > 0)
            {
                while(col < grid_cols)
                {
                    if(col < static_cast<int>(owners.size()) && owners[static_cast<size_t>(col)].td)
                    {
                        // Covered by a vertical merge from a previous row – skip
                        col += 1; // or owners[...].span if we tracked it, but keep simple
                        continue;
                    }
                    auto td = make_element("td", "padding:2px 5px;");
                    auto p  = make_element("p", "margin:0;padding:0;");
                    td->appendChild(p);
                    append_nbsp(p);
                    tr->appendChild(td);
                    if(col >= static_cast<int>(owners.size()))
                        owners.resize(static_cast<size_t>(col + 1));
                    owners[static_cast<size_t>(col)] = CellOwner();
                    ++col;
                }
            }
            ++row_index;
        }
        (void)row_index;
    }

     void DocxBuilder::append_text(const litehtml::element::ptr& parent, const std::string& text)
       {
           if (text.empty()) return;

           // Reserve capacity for nbsp building (max 2 bytes per char for UTF-8 NBSP)
           // and for word segments (worst case: one long word = text.size())
           if (m_nbsp_buf.capacity() < text.size() * 2) {
               m_nbsp_buf.reserve(text.size() * 2);
           }
           if (m_word_buf.capacity() < text.size()) {
               m_word_buf.reserve(text.size());
           }

           size_t i = 0;
           while(i < text.size())
           {
               bool space = text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n';
               size_t start = i;
               if(space)
               {
                   while(i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n'))
                   {
                       ++i;
                   }
                   size_t len = i - start;
                   bool edge = start == 0 || i == text.size();
                   if(len > 1 || edge)
                   {
                       m_nbsp_buf.clear();
                       // Build NBSP string: \xC2\xA0 repeated len times
                       m_nbsp_buf.resize(len * 2);
                       for (size_t k = 0; k < len; ++k) {
                           m_nbsp_buf[k * 2] = '\xC2';
                           m_nbsp_buf[k * 2 + 1] = '\xA0';
                       }
                       parent->appendChild(std::make_shared<litehtml::el_text>(m_nbsp_buf.c_str(), m_doc));
                   } else
                   {
                       parent->appendChild(std::make_shared<litehtml::el_space>(" ", m_doc));
                   }
               } else
               {
                   while(i < text.size() && !(text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n'))
                   {
                       ++i;
                   }
                   // Use reusable buffer instead of substr() to avoid temporary allocation
                   m_word_buf.assign(text.data() + start, i - start);
                   parent->appendChild(std::make_shared<litehtml::el_text>(m_word_buf.c_str(), m_doc));
               }
           }
       }

    void DocxBuilder::append_nbsp(const litehtml::element::ptr& parent)
    {
        parent->appendChild(std::make_shared<litehtml::el_text>("\xC2\xA0", m_doc));
    }

    void DocxBuilder::add_page_break(const litehtml::element::ptr& parent)
    {
        int content_h = m_page_h_px - twip_to_px(m_model.section.margin_t_tw) -
                        twip_to_px(m_model.section.margin_b_tw);
        if(content_h < 100)
        {
            content_h = 600;
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "height:%dpx;", content_h);
        auto div = make_element("div", buf, "data-page-break", std::to_string(m_page_break_count++));
        parent->appendChild(div);
    }

    litehtml::element::ptr DocxBuilder::make_element(const char* tag, const std::string& css,
                                                     const litehtml::string_map& attrs)
    {
        litehtml::string_map a = attrs;
        if(!css.empty())
        {
            a["style"] = css;
        }
        return m_doc->create_element(tag, a);
    }

    litehtml::element::ptr DocxBuilder::make_element(const char* tag, const std::string& css, const char* attr_name,
                                                     const std::string& attr_value)
    {
        litehtml::string_map attrs;
        attrs[attr_name] = attr_value;
        return make_element(tag, css, attrs);
    }

    std::string DocxBuilder::font_family(const RunProps& r) const
    {
        if(!r.font.empty())
        {
            return r.font;
        }
        if(r.font_theme.find("major") != std::string::npos)
        {
            return m_model.theme.major_font;
        }
        if(r.font_theme.find("minor") != std::string::npos)
        {
            return m_model.theme.minor_font;
        }
        return std::string();
    }
} // namespace litedocx
