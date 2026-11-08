#include "docx_css.h"

#include "docx_style.h"

#include <cstdio>

namespace litedocx
{
    namespace
    {
        std::string quoted(const std::string& s)
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
    } // namespace

    std::string docx_base_css(const DocxModel& model)
    {
        std::string font = model.styles.defaults.run.font;
        if(font.empty())
        {
            std::string theme = model.styles.defaults.run.font_theme;
            if(theme.find("major") != std::string::npos)
            {
                font = model.theme.major_font;
            } else
            {
                font = model.theme.minor_font;
            }
        }
        if(font.empty())
        {
            font = "Aptos";
        }

        int size_half = model.styles.defaults.run.size_half > 0 ? model.styles.defaults.run.size_half : 22;
        int size_px   = half_point_to_px(size_half);
        if(size_px <= 0)
        {
            size_px = 15;
        }

        char buf[1024];
        std::snprintf(buf, sizeof(buf),
                      "html,body{margin:0;padding:0;}\n"
                      "body{color:#000000;background:#ffffff;font-family:%s,\"Segoe UI\",Arial,sans-serif;"
                      "font-size:%dpx;line-height:1.15;}\n"
                      "p{margin:0;padding:0;line-height:1.15;}\n"
                      "div{margin:0;padding:0;}\n"
                      "span{white-space:normal;}\n"
                      "table{border-collapse:collapse;border-spacing:0;}\n"
                      "td,th{padding:0;vertical-align:top;}\n"
                      "ul,ol{margin:0;padding-left:22px;}\n"
                      "li{margin:0;padding:0;}\n"
                      "a{color:#0563c1;text-decoration:underline;}\n"
                      "img{max-width:100%%;}\n",
                      quoted(font).c_str(), size_px);
        return std::string(buf);
    }
} // namespace litedocx
