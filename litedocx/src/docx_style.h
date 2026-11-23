#ifndef LITEDOCX_DOCX_STYLE_H
#define LITEDOCX_DOCX_STYLE_H

#include "docx_model.h"

#include <string>

namespace litedocx
{
    bool parse_styles(const std::string& xml, DocxModel& model);
    bool parse_numbering(const std::string& xml, DocxModel& model);
    bool parse_theme(const std::string& xml, DocxModel& model);
    bool parse_relationships(const std::string& xml, DocxModel& model);

    void parse_run_props(const struct XmlNode* rpr, RunProps& out);
    void parse_para_props(const struct XmlNode* ppr, ParaProps& out);

    ParaProps resolve_para(const DocxModel& model, const ParaProps& direct);
    RunProps  resolve_run(const DocxModel& model, const ParaProps& para, const RunProps& direct);

    const NumberingLevel* find_numbering_level(const DocxModel& model, int num_id, int level);

    std::string jc_to_css(const std::string& jc);
    std::string list_style_for_format(const std::string& fmt);
    bool        list_is_bullet(const std::string& fmt);

    /* Paragraph-write tracing: true for Heading/Title/Titolo style ids.
     * Shared by the builder (DOM write) and the OOXML extractor. */
    inline bool style_is_heading(const std::string& id)
    {
        std::string lower;
        lower.reserve(id.size());
        for(char c : id)
            lower += (char)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
        return lower.find("heading") != std::string::npos ||
               lower.find("title")   != std::string::npos ||
               lower.find("titolo")  != std::string::npos;
    }

} // namespace litedocx

#endif // LITEDOCX_DOCX_STYLE_H
