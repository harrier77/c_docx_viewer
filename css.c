#include "css.h"

#include <stdio.h>
#include <string.h>

static char g_css[4096];

const char *docx_default_css(int font_px)
{
    int fp = (font_px > 0 && font_px < 200) ? font_px : 16;
    int h1 = fp * 7 / 4;
    int h2 = fp * 3 / 2;
    int h3 = fp * 5 / 4;
    int h4 = fp * 9 / 8;
    int h5 = fp;
    int h6 = fp * 15 / 16;

    if (h6 < 1)
        h6 = 1;

    snprintf(g_css, sizeof(g_css),
             "body{margin:0;padding:18px 22px;color:#1f2328;background:#ffffff;"
             "font-family:\"Aptos\",\"Segoe UI\",Arial,sans-serif;"
             "font-size:%dpx;line-height:1.5;}\n"
             "h1,h2,h3,h4,h5,h6{color:#0f4761;font-weight:bold;"
             "line-height:1.25;margin:18px 0 8px 0;}\n"
             "h1{font-size:%dpx;}\n"
             "h2{font-size:%dpx;}\n"
             "h3{font-size:%dpx;}\n"
             "h4{font-size:%dpx;}\n"
             "h5{font-size:%dpx;}\n"
             "h6{font-size:%dpx;}\n"
             "p{margin:0 0 10px 0;}\n"
             "a{color:#0969da;text-decoration:underline;}\n"
             "strong{font-weight:bold;}\n"
             "em{font-style:italic;}\n"
             "u{text-decoration:underline;}\n"
             "table{border-collapse:collapse;margin:10px 0;}\n"
             "td{border:1px solid #aaa;padding:6px 10px;vertical-align:top;}\n",
             fp, h1, h2, h3, h4, h5, h6);
    return g_css;
}
