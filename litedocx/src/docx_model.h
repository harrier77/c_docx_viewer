#ifndef LITEDOCX_DOCX_MODEL_H
#define LITEDOCX_DOCX_MODEL_H

#include <map>
#include <string>
#include <vector>

namespace litedocx
{
    struct RunProps
    {
        int         bold       = -1;
        int         italic     = -1;
        int         underline  = -1;
        int         strike     = -1;
        int         caps       = -1;
        int         small_caps = -1;
        int         size_half  = -1; // w:sz, half-points
        int         vert_align = -1; // 0 normal, 1 superscript, 2 subscript
        std::string color;           // RRGGBB
        std::string font;            // resolved family
        std::string font_theme;      // minorHAnsi / majorHAnsi ...
        std::string highlight;       // RRGGBB or empty
        std::string shading;         // RRGGBB or empty
        std::string rstyle;

        void merge(const RunProps& o);
    };

    struct BorderSide
    {
        int         width_px = -1; // -1 unset, 0 none
        std::string color;         // RRGGBB
        int         space_pt = 0;
    };

    struct ParaProps
    {
        std::string style;
        std::string jc;
        int         ind_left      = -1;
        int         ind_right     = -1;
        int         ind_first     = -1;
        int         ind_hanging   = -1;
        int         space_before  = -1;
        int         space_after   = -1;
        int         line          = -1;
        std::string line_rule; // auto / exact / atLeast
        int         num_id        = -1;
        int         num_level     = -1;
        int         page_break_before = 0;
        int         keep_next     = 0;
        std::string shading;
        BorderSide  border_top;
        BorderSide  border_left;
        BorderSide  border_bottom;
        BorderSide  border_right;

        void merge(const ParaProps& o);
    };

    struct DocDefaults
    {
        RunProps  run;
        ParaProps para;
    };

    struct StyleDef
    {
        std::string id;
        std::string name;
        std::string based_on;
        std::string type; // paragraph/character/table/numbering
        bool        is_default = false;
        RunProps    run;
        ParaProps   para;
    };

    struct StyleSheet
    {
        DocDefaults                         defaults;
        std::map<std::string, StyleDef>     styles;
        std::string                         default_para;
    };

    struct NumberingLevel
    {
        int         ilvl      = 0;
        std::string format    = "decimal";
        std::string text      = "%1.";
        int         start     = 1;
        int         ind_left  = -1;
        int         ind_first = -1;
    };

    struct AbstractNum
    {
        int                                  id = -1;
        std::map<int, NumberingLevel>        levels;
    };

    struct NumDef
    {
        int num_id      = -1;
        int abstract_id = -1;
    };

    struct Numbering
    {
        std::map<int, AbstractNum> abstracts;
        std::map<int, NumDef>      nums;
    };

    struct Relationship
    {
        std::string id;
        std::string type;
        std::string target;
        std::string resolved; // ZIP path for internal targets, URL for external ones
        bool        external = false;
    };

    struct Relationships
    {
        std::map<std::string, Relationship> items;

        const Relationship* find(const std::string& id) const
        {
            auto it = items.find(id);
            return it == items.end() ? nullptr : &it->second;
        }
    };

    struct Theme
    {
        std::string minor_font;
        std::string major_font;
    };

    struct Section
    {
        int page_w_tw   = 12240; // 8.5in
        int page_h_tw   = 15840; // 11in
        int margin_t_tw = 1440;
        int margin_b_tw = 1440;
        int margin_l_tw = 1440;
        int margin_r_tw = 1440;
        int header_tw   = 720;
        int footer_tw   = 720;
    };

    struct MediaFile
    {
        std::string                name; // normalized, e.g. "media/image1.png"
        std::vector<unsigned char> data;
        int                        px_w = 0;
        int                        px_h = 0;
    };

    struct DocxModel
    {
        StyleSheet                         styles;
        Numbering                          numbering;
        Relationships                      rels;
        Theme                              theme;
        Section                            section;
        std::map<std::string, MediaFile>   media;
        std::string                        document_xml;
        std::string                        styles_xml;
        std::string                        numbering_xml;
        std::string                        theme_xml;
        std::string                        rels_xml;
        std::string                        file_name;
    };

    /* Conversion helpers (twips -> CSS px at 96 dpi). */
    inline int twip_to_px(int tw)
    {
        if(tw <= 0)
        {
            return 0;
        }
        return (tw + 7) / 15;
    }

    inline int half_point_to_px(int half)
    {
        if(half <= 0)
        {
            return 0;
        }
        return (half * 2 + 2) / 3;
    }

    inline int emu_to_px(long long emu)
    {
        if(emu <= 0)
        {
            return 0;
        }
        return static_cast<int>((emu + 4762) / 9525);
    }

    inline int eighth_point_to_px(int eighths)
    {
        if(eighths <= 0)
        {
            return 0;
        }
        int px = (eighths * 2 + 5) / 6;
        return px < 1 ? 1 : px;
    }

    inline bool is_hex_color(const std::string& s)
    {
        if(s.size() != 6)
        {
            return false;
        }
        for(char c : s)
        {
            if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            {
                return false;
            }
        }
        return true;
    }

    std::string norm_color(const std::string& s);

} // namespace litedocx

#endif // LITEDOCX_DOCX_MODEL_H
