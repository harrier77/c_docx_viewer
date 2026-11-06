#include "docx_style.h"

#include "xml_dom.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace litedocx
{
    namespace
    {
        bool truthy(const XmlNode* n)
        {
            if(!n)
            {
                return false;
            }
            if(!n->has("w:val"))
            {
                return true;
            }
            std::string v = n->attr("w:val");
            return !(v == "0" || v == "false" || v == "off" || v == "none" || v == "nil");
        }

        int int_attr(const XmlNode* n, const char* name, int def = -1)
        {
            if(!n || !n->has(name))
            {
                return def;
            }
            return std::atoi(n->attr(name).c_str());
        }

        std::string color_attr(const XmlNode* n, const char* name)
        {
            if(!n || !n->has(name))
            {
                return {};
            }
            return norm_color(n->attr(name));
        }

        void parse_border_side(const XmlNode* n, BorderSide& out)
        {
            if(!n)
            {
                return;
            }
            std::string val = n->attr("w:val", "single");
            if(val == "none" || val == "nil")
            {
                out.width_px = 0;
            } else
            {
                int sz = int_attr(n, "w:sz", 4);
                out.width_px = eighth_point_to_px(sz);
                if(out.width_px < 1)
                {
                    out.width_px = 1;
                }
            }
            std::string col = color_attr(n, "w:color");
            out.color        = col.empty() ? "000000" : col;
            out.space_pt     = int_attr(n, "w:space", 0);
        }

        void parse_borders_box(const XmlNode* parent, const char* child_name, BorderSide& top, BorderSide& left,
                               BorderSide& bottom, BorderSide& right)
        {
            if(!parent)
            {
                return;
            }
            const XmlNode* b = parent->child(child_name);
            if(!b)
            {
                return;
            }
            parse_border_side(b->child("top"), top);
            parse_border_side(b->child("left"), left);
            parse_border_side(b->child("start"), left);
            parse_border_side(b->child("bottom"), bottom);
            parse_border_side(b->child("right"), right);
            parse_border_side(b->child("end"), right);
        }

        void apply_style_chain(const DocxModel& model, const std::string& id, bool run, bool para, RunProps& r,
                               ParaProps& p)
        {
            std::string current = id;
            std::vector<const StyleDef*> chain;
            for(int hops = 0; hops < 32 && !current.empty(); ++hops)
            {
                auto it = model.styles.styles.find(current);
                if(it == model.styles.styles.end())
                {
                    break;
                }
                chain.push_back(&it->second);
                current = it->second.based_on;
            }
            std::reverse(chain.begin(), chain.end());
            for(const auto* st : chain)
            {
                if(run)
                {
                    r.merge(st->run);
                }
                if(para)
                {
                    p.merge(st->para);
                }
            }
        }
    } // namespace

    std::string norm_color(const std::string& s)
    {
        std::string v;
        for(char c : s)
        {
            if(c != '#' && c != ' ' && c != '\t' && c != '\r' && c != '\n')
            {
                v.push_back(c);
            }
        }
        if(v.size() != 6 || !is_hex_color(v) || v == "auto")
        {
            return {};
        }
        for(char& c : v)
        {
            if(c >= 'A' && c <= 'F')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return v;
    }

    void RunProps::merge(const RunProps& o)
    {
        if(o.bold >= 0)
            bold = o.bold;
        if(o.italic >= 0)
            italic = o.italic;
        if(o.underline >= 0)
            underline = o.underline;
        if(o.strike >= 0)
            strike = o.strike;
        if(o.caps >= 0)
            caps = o.caps;
        if(o.small_caps >= 0)
            small_caps = o.small_caps;
        if(o.size_half > 0)
            size_half = o.size_half;
        if(o.vert_align >= 0)
            vert_align = o.vert_align;
        if(!o.color.empty())
            color = o.color;
        if(!o.font.empty())
            font = o.font;
        if(!o.font_theme.empty())
            font_theme = o.font_theme;
        if(!o.highlight.empty())
            highlight = o.highlight;
        if(!o.shading.empty())
            shading = o.shading;
        if(!o.rstyle.empty())
            rstyle = o.rstyle;
    }

    void ParaProps::merge(const ParaProps& o)
    {
        if(!o.style.empty())
            style = o.style;
        if(!o.jc.empty())
            jc = o.jc;
        if(o.ind_left >= 0)
            ind_left = o.ind_left;
        if(o.ind_right >= 0)
            ind_right = o.ind_right;
        if(o.ind_first >= 0)
            ind_first = o.ind_first;
        if(o.ind_hanging >= 0)
            ind_hanging = o.ind_hanging;
        if(o.space_before >= 0)
            space_before = o.space_before;
        if(o.space_after >= 0)
            space_after = o.space_after;
        if(o.line >= 0)
            line = o.line;
        if(!o.line_rule.empty())
            line_rule = o.line_rule;
        if(o.num_id >= 0)
            num_id = o.num_id;
        if(o.num_level >= 0)
            num_level = o.num_level;
        if(o.page_break_before)
            page_break_before = 1;
        if(o.keep_next)
            keep_next = 1;
        if(!o.shading.empty())
            shading = o.shading;
        if(o.border_top.width_px >= 0)
            border_top = o.border_top;
        if(o.border_left.width_px >= 0)
            border_left = o.border_left;
        if(o.border_bottom.width_px >= 0)
            border_bottom = o.border_bottom;
        if(o.border_right.width_px >= 0)
            border_right = o.border_right;
    }

    void parse_run_props(const XmlNode* rpr, RunProps& out)
    {
        if(!rpr)
        {
            return;
        }
        if(const XmlNode* n = rpr->child("b"))
            out.bold = truthy(n) ? 1 : 0;
        if(const XmlNode* n = rpr->child("i"))
            out.italic = truthy(n) ? 1 : 0;
        if(const XmlNode* n = rpr->child("u"))
        {
            std::string v = n->attr("w:val", "single");
            out.underline = (v == "none" || v == "nil") ? 0 : 1;
        }
        if(const XmlNode* n = rpr->child("strike"))
            out.strike = truthy(n) ? 1 : 0;
        if(!out.strike && (rpr->child("dstrike")))
            out.strike = truthy(rpr->child("dstrike")) ? 1 : 0;
        if(const XmlNode* n = rpr->child("caps"))
            out.caps = truthy(n) ? 1 : 0;
        if(const XmlNode* n = rpr->child("smallCaps"))
            out.small_caps = truthy(n) ? 1 : 0;
        if(const XmlNode* n = rpr->child("color"))
        {
            std::string c = color_attr(n, "w:val");
            if(!c.empty())
                out.color = c;
        }
        if(const XmlNode* n = rpr->child("sz"))
        {
            int v = int_attr(n, "w:val", 0);
            if(v > 0)
                out.size_half = v;
        }
        if(const XmlNode* n = rpr->child("rFonts"))
        {
            std::string f = n->attr("w:ascii");
            if(f.empty())
                f = n->attr("w:hAnsi");
            if(f.empty())
                f = n->attr("w:cs");
            if(f.empty())
                f = n->attr("w:eastAsia");
            if(!f.empty())
                out.font = f;
            std::string th = n->attr("w:asciiTheme");
            if(th.empty())
                th = n->attr("w:hAnsiTheme");
            if(!th.empty())
                out.font_theme = th;
        }
        if(const XmlNode* n = rpr->child("vertAlign"))
        {
            std::string v = n->attr("w:val");
            if(v == "superscript")
                out.vert_align = 1;
            else if(v == "subscript")
                out.vert_align = 2;
            else
                out.vert_align = 0;
        }
        if(const XmlNode* n = rpr->child("highlight"))
        {
            std::string v = n->attr("w:val");
            if(v == "yellow")
                out.highlight = "ffff00";
            else if(v == "green")
                out.highlight = "00ff00";
            else if(v == "cyan")
                out.highlight = "00ffff";
            else if(v == "magenta")
                out.highlight = "ff00ff";
            else if(v == "blue")
                out.highlight = "0000ff";
            else if(v == "red")
                out.highlight = "ff0000";
            else if(v == "darkBlue")
                out.highlight = "000080";
            else if(v == "darkRed")
                out.highlight = "800000";
            else if(v == "darkGreen")
                out.highlight = "008000";
            else if(v == "gray" || v == "lightGray")
                out.highlight = "c0c0c0";
            else if(v == "black")
                out.highlight = "000000";
        }
        if(const XmlNode* n = rpr->child("shd"))
        {
            std::string c = color_attr(n, "w:fill");
            if(!c.empty())
                out.shading = c;
        }
        if(const XmlNode* n = rpr->child("rStyle"))
        {
            std::string v = n->attr("w:val");
            if(!v.empty())
                out.rstyle = v;
        }
    }

    void parse_para_props(const XmlNode* ppr, ParaProps& out)
    {
        if(!ppr)
        {
            return;
        }
        if(const XmlNode* n = ppr->child("pStyle"))
            out.style = n->attr("w:val");
        if(const XmlNode* n = ppr->child("jc"))
            out.jc = n->attr("w:val");
        if(const XmlNode* n = ppr->child("ind"))
        {
            if(n->has("w:left"))
                out.ind_left = int_attr(n, "w:left");
            else if(n->has("w:start"))
                out.ind_left = int_attr(n, "w:start");
            if(n->has("w:right"))
                out.ind_right = int_attr(n, "w:right");
            else if(n->has("w:end"))
                out.ind_right = int_attr(n, "w:end");
            if(n->has("w:firstLine"))
                out.ind_first = int_attr(n, "w:firstLine");
            if(n->has("w:hanging"))
                out.ind_hanging = int_attr(n, "w:hanging");
        }
        if(const XmlNode* n = ppr->child("spacing"))
        {
            if(n->has("w:before"))
                out.space_before = int_attr(n, "w:before");
            if(n->has("w:after"))
                out.space_after = int_attr(n, "w:after");
            if(n->has("w:line"))
                out.line = int_attr(n, "w:line");
            std::string rule = n->attr("w:lineRule");
            if(!rule.empty())
                out.line_rule = rule;
        }
        if(const XmlNode* n = ppr->child("numPr"))
        {
            if(const XmlNode* id = n->child("numId"))
                out.num_id = int_attr(id, "w:val", -1);
            if(const XmlNode* il = n->child("ilvl"))
                out.num_level = int_attr(il, "w:val", 0);
        }
        if(const XmlNode* n = ppr->child("pageBreakBefore"))
            out.page_break_before = truthy(n) ? 1 : 0;
        if(const XmlNode* n = ppr->child("keepNext"))
            out.keep_next = truthy(n) ? 1 : 0;
        if(const XmlNode* n = ppr->child("shd"))
        {
            std::string c = color_attr(n, "w:fill");
            if(!c.empty())
                out.shading = c;
        }
        parse_borders_box(ppr, "pBdr", out.border_top, out.border_left, out.border_bottom, out.border_right);
    }

    bool parse_styles(const std::string& xml, DocxModel& model)
    {
        auto root = xml_parse(xml.c_str(), xml.size());
        if(!root)
        {
            return false;
        }
        const XmlNode* styles = root->is("styles") ? root.get() : root->child("styles");
        if(!styles)
        {
            return false;
        }

        if(const XmlNode* dd = styles->child("docDefaults"))
        {
            if(const XmlNode* rd = dd->child("rPrDefault"))
            {
                parse_run_props(rd->child("rPr"), model.styles.defaults.run);
            }
            if(const XmlNode* pd = dd->child("pPrDefault"))
            {
                parse_para_props(pd->child("pPr"), model.styles.defaults.para);
            }
        }

        for(const XmlNode* sn : styles->children_named("style"))
        {
            StyleDef st;
            st.id     = sn->attr("w:styleId");
            st.type   = sn->attr("w:type");
            st.is_default = sn->attr("w:default") == "1" || sn->attr("w:default") == "true";
            if(const XmlNode* name = sn->child("name"))
                st.name = name->attr("w:val");
            if(const XmlNode* based = sn->child("basedOn"))
                st.based_on = based->attr("w:val");
            parse_run_props(sn->child("rPr"), st.run);
            parse_para_props(sn->child("pPr"), st.para);
            if(st.type == "paragraph" && st.is_default)
            {
                model.styles.default_para = st.id;
            }
            if(!st.id.empty())
            {
                model.styles.styles[st.id] = st;
            }
        }
        if(model.styles.default_para.empty())
        {
            model.styles.default_para = "Normal";
        }
        return true;
    }

    bool parse_numbering(const std::string& xml, DocxModel& model)
    {
        auto root = xml_parse(xml.c_str(), xml.size());
        if(!root)
        {
            return false;
        }
        const XmlNode* numbering = root->is("numbering") ? root.get() : root->child("numbering");
        if(!numbering)
        {
            return false;
        }

        for(const XmlNode* an : numbering->children_named("abstractNum"))
        {
            AbstractNum a;
            a.id = int_attr(an, "w:abstractNumId", -1);
            for(const XmlNode* lvl : an->children_named("lvl"))
            {
                NumberingLevel l;
                l.ilvl = int_attr(lvl, "w:ilvl", 0);
                if(const XmlNode* n = lvl->child("numFmt"))
                    l.format = n->attr("w:val", "decimal");
                if(const XmlNode* n = lvl->child("lvlText"))
                    l.text = n->attr("w:val");
                if(const XmlNode* n = lvl->child("start"))
                    l.start = int_attr(n, "w:val", 1);
                if(const XmlNode* n = lvl->child("pPr"))
                {
                    if(const XmlNode* ind = n->child("ind"))
                    {
                        if(ind->has("w:left"))
                            l.ind_left = int_attr(ind, "w:left");
                        else if(ind->has("w:start"))
                            l.ind_left = int_attr(ind, "w:start");
                        if(ind->has("w:firstLine"))
                            l.ind_first = int_attr(ind, "w:firstLine");
                        else if(ind->has("w:hanging"))
                            l.ind_first = -int_attr(ind, "w:hanging");
                    }
                }
                a.levels[l.ilvl] = l;
            }
            if(a.id >= 0)
            {
                model.numbering.abstracts[a.id] = a;
            }
        }
        for(const XmlNode* num : numbering->children_named("num"))
        {
            NumDef d;
            d.num_id = int_attr(num, "w:numId", -1);
            if(const XmlNode* abs = num->child("abstractNumId"))
                d.abstract_id = int_attr(abs, "w:val", -1);
            if(d.num_id >= 0 && d.abstract_id >= 0)
            {
                model.numbering.nums[d.num_id] = d;
            }
        }
        return true;
    }

    bool parse_theme(const std::string& xml, DocxModel& model)
    {
        auto root = xml_parse(xml.c_str(), xml.size());
        if(!root)
        {
            return false;
        }
        const XmlNode* scheme = root->child("themeElements");
        if(!scheme)
        {
            // Some producers omit themeElements wrapper nesting selectors; search a bit.
            scheme = root.get();
        }
        if(const XmlNode* fs = scheme->child("fontScheme"))
        {
            if(const XmlNode* minor = fs->child("minorFont"))
            {
                if(const XmlNode* lat = minor->child("latin"))
                    model.theme.minor_font = lat->attr("typeface");
            }
            if(const XmlNode* major = fs->child("majorFont"))
            {
                if(const XmlNode* lat = major->child("latin"))
                    model.theme.major_font = lat->attr("typeface");
            }
        }
        return true;
    }

    bool parse_relationships(const std::string& xml, DocxModel& model)
    {
        auto root = xml_parse(xml.c_str(), xml.size());
        if(!root)
        {
            return false;
        }
        const XmlNode* rels = root->is("Relationships") ? root.get() : root->child("Relationships");
        if(!rels)
        {
            return false;
        }
        for(const XmlNode* r : rels->children_named("Relationship"))
        {
            Relationship rel;
            rel.id       = r->attr("Id");
            rel.type     = r->attr("Type");
            rel.target   = r->attr("Target");
            rel.external = r->attr("TargetMode") == "External";
            if(!rel.id.empty())
            {
                model.rels.items[rel.id] = rel;
            }
        }
        return true;
    }

    ParaProps resolve_para(const DocxModel& model, const ParaProps& direct)
    {
        ParaProps out = model.styles.defaults.para;
        std::string id = direct.style;
        if(id.empty())
        {
            id = model.styles.default_para;
        }
        RunProps dummy_r;
        apply_style_chain(model, id, false, true, dummy_r, out);
        out.merge(direct);
        return out;
    }

    RunProps resolve_run(const DocxModel& model, const ParaProps& para, const RunProps& direct)
    {
        RunProps out = model.styles.defaults.run;
        std::string para_style = para.style.empty() ? model.styles.default_para : para.style;
        ParaProps dummy_p;
        apply_style_chain(model, para_style, true, false, out, dummy_p);
        if(!direct.rstyle.empty())
        {
            apply_style_chain(model, direct.rstyle, true, false, out, dummy_p);
        }
        out.merge(direct);
        return out;
    }

    const NumberingLevel* find_numbering_level(const DocxModel& model, int num_id, int level)
    {
        auto n = model.numbering.nums.find(num_id);
        if(n == model.numbering.nums.end())
        {
            return nullptr;
        }
        auto a = model.numbering.abstracts.find(n->second.abstract_id);
        if(a == model.numbering.abstracts.end())
        {
            return nullptr;
        }
        auto l = a->second.levels.find(level);
        if(l == a->second.levels.end())
        {
            l = a->second.levels.find(0);
        }
        return l == a->second.levels.end() ? nullptr : &l->second;
    }

    std::string jc_to_css(const std::string& jc)
    {
        if(jc == "left" || jc == "start")
            return "left";
        if(jc == "right" || jc == "end")
            return "right";
        if(jc == "center")
            return "center";
        if(jc == "both" || jc == "bothStr" || jc == "distribute" || jc == "thaiDistribute" ||
           jc == "lowKashida" || jc == "mediumKashida" || jc == "highKashida")
            return "justify";
        return {};
    }

    std::string list_style_for_format(const std::string& fmt)
    {
        if(fmt == "bullet" || fmt == "none")
            return fmt == "none" ? "none" : "disc";
        if(fmt == "decimal")
            return "decimal";
        if(fmt == "decimalZero")
            return "decimal-leading-zero";
        if(fmt == "lowerLetter" || fmt == "lowerAlpha")
            return "lower-alpha";
        if(fmt == "upperLetter" || fmt == "upperAlpha")
            return "upper-alpha";
        if(fmt == "lowerRoman")
            return "lower-roman";
        if(fmt == "upperRoman")
            return "upper-roman";
        if(fmt == "lowerGreek")
            return "lower-greek";
        if(fmt == "decimalEnclosedCircle" || fmt == "decimalEnclosedParen")
            return "decimal";
        return "decimal";
    }

    bool list_is_bullet(const std::string& fmt)
    {
        return fmt == "bullet" || fmt == "none";
    }
} // namespace litedocx
