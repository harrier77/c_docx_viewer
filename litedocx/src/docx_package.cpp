/*
 * docx_package.cpp - DOCX package loading and parsing.
 * Extracts document.xml, styles.xml, numbering.xml from .docx ZIP.
 * Parses into DocxModel for DocxBuilder.
 */
#include "docx_package.h"

#include "docx_style.h"
#include "zip.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace litedocx
{

static std::string read_zip_entry(zip_arc* za, const char* name)
{
    char* out = nullptr;
    size_t out_len = 0;
    if(!zip_read(za, name, &out, &out_len) || !out)
        return "";
    std::string result(out, out_len);
    free(out);
    return result;
}

static std::string find_in_zip(zip_arc* za, const char* pattern)
{
    const int n = zip_entry_count(za);
    for(int i = 0; i < n; ++i)
    {
        const char* name = nullptr;
        size_t len = 0;
        if(!zip_entry_name(za, i, &name, &len) || !name)
            continue;
        std::string entry(name, len);
        if(entry.find(pattern) != std::string::npos)
            return read_zip_entry(za, entry.c_str());
    }
    return "";
}

static void load_media_files(zip_arc* za, DocxModel& model)
{
    const int n = zip_entry_count(za);
    for(int i = 0; i < n; ++i)
    {
        const char* name = nullptr;
        size_t len = 0;
        if(!zip_entry_name(za, i, &name, &len) || !name)
            continue;
        std::string entry(name, len);
        if(entry.find("word/media/") == std::string::npos)
            continue;

        char* out = nullptr;
        size_t out_len = 0;
        if(!zip_read(za, entry.c_str(), &out, &out_len) || !out)
            continue;

        MediaFile media;
        media.name = entry;
        media.data.assign(reinterpret_cast<unsigned char*>(out),
                          reinterpret_cast<unsigned char*>(out) + out_len);
        free(out);
        model.media[entry] = std::move(media);
    }
}

bool load_docx_package(const void* data, size_t size, const std::string& file_name,
                       DocxModel& model, std::string& err)
{
    model.file_name = file_name;

    zip_arc za;
    if(!zip_open(&za, data, size))
    {
        err = "impossibile aprire DOCX come ZIP";
        return false;
    }

    model.document_xml = find_in_zip(&za, "word/document.xml");
    if(model.document_xml.empty())
    {
        model.document_xml = read_zip_entry(&za, "word/document.xml");
        if(model.document_xml.empty())
            model.document_xml = read_zip_entry(&za, "word/document2.xml");
    }
    if(model.document_xml.empty())
    {
        err = "word/document.xml non trovato nel DOCX";
        return false;
    }

    model.styles_xml = find_in_zip(&za, "word/styles.xml");
    if(model.styles_xml.empty())
        model.styles_xml = read_zip_entry(&za, "word/styles.xml");

    model.numbering_xml = find_in_zip(&za, "word/numbering.xml");
    if(model.numbering_xml.empty())
        model.numbering_xml = read_zip_entry(&za, "word/numbering.xml");

    model.theme_xml = find_in_zip(&za, "word/theme/theme1.xml");
    if(model.theme_xml.empty())
        model.theme_xml = read_zip_entry(&za, "word/theme/theme1.xml");

    model.rels_xml = find_in_zip(&za, "word/_rels/document.xml.rels");
    if(model.rels_xml.empty())
        model.rels_xml = read_zip_entry(&za, "word/_rels/document.xml.rels");

    /* REGRESSION FIX: the XML parts above must be parsed into DocxModel before
     * DocxBuilder runs. Without this, model.styles.styles stays empty, every
     * paragraph resolves to the default style and headings/numbering/theme
     * fonts all look like plain Normal text (all paragraphs share class "p1"). */
    if(!model.styles_xml.empty())
        parse_styles(model.styles_xml, model);
    if(!model.numbering_xml.empty())
        parse_numbering(model.numbering_xml, model);
    if(!model.theme_xml.empty())
        parse_theme(model.theme_xml, model);
    if(!model.rels_xml.empty())
        parse_relationships(model.rels_xml, model);

    /* Resolve internal relationship targets to ZIP paths. */
    for(auto& kv : model.rels.items)
    {
        Relationship& r = kv.second;
        r.resolved = r.external ? r.target : resolve_zip_path("word", r.target);
    }

    load_media_files(&za, model);
    return true;
}

std::string serialize_document_xml(const DocxModel& model)
{
    const std::string& src = model.document_xml;
    static const char kXmlDecl[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
    const bool need_decl = src.compare(0, 5, "<?xml") != 0;
    const size_t decl_len = need_decl ? (sizeof(kXmlDecl) - 1) : 0;

    if (!model.paragraph_ooxml.empty() && model.dirty)
    {
        const size_t body_start = src.find("<w:body>");
        const size_t body_end   = (body_start != std::string::npos) ? src.find("</w:body>", body_start + 8) : std::string::npos;
        if (body_start != std::string::npos && body_end != std::string::npos)
        {
            const size_t prefix_len = body_start + 8; // include <w:body>
            const size_t suffix_len = src.size() - body_end;

            // Locate trailing <w:sectPr ...>...</w:sectPr> inside original body
            size_t sect_off = std::string::npos;
            size_t sect_len = 0;
            const size_t sect_start = src.rfind("<w:sectPr", body_end);
            if (sect_start != std::string::npos && sect_start >= prefix_len)
            {
                const size_t sect_end = src.find("</w:sectPr>", sect_start);
                if (sect_end != std::string::npos && sect_end < body_end)
                {
                    sect_off = sect_start;
                    sect_len = (sect_end + 11) - sect_start; // </w:sectPr> is 11 chars
                }
            }

            size_t para_total = 0;
            for (const auto& p : model.paragraph_ooxml)
            {
                para_total += p.size();
            }

            std::string xml;
            xml.reserve(decl_len + prefix_len + para_total + sect_len + suffix_len);
            if (need_decl) xml.append(kXmlDecl, decl_len);
            xml.append(src, 0, prefix_len);
            for (const auto& p : model.paragraph_ooxml)
            {
                xml.append(p);
            }
            if (sect_len > 0)
            {
                xml.append(src, sect_off, sect_len);
            }
            xml.append(src, body_end, suffix_len);
            return xml;
        }
    }

    if (!need_decl)
    {
        return src;
    }
    std::string xml;
    xml.reserve(decl_len + src.size());
    xml.append(kXmlDecl, decl_len);
    xml.append(src);
    return xml;
}

bool read_file_bytes(const std::string& path, std::vector<unsigned char>& out)
{
    std::ifstream f(path, std::ios::binary);
    if(!f)
        return false;
    f.seekg(0, std::ios::end);
    const std::streamoff end = f.tellg();
    if(end < 0)
        return false;
    out.resize(static_cast<size_t>(end));
    f.seekg(0, std::ios::beg);
    if(!out.empty() && !f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size())))
        return false;
    return true;
}

std::string base_name(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    if(slash == std::string::npos)
        return path;
    return path.substr(slash + 1);
}

std::string resolve_zip_path(const std::string& base_dir, const std::string& target)
{
    if(target.empty())
        return target;
    if(target[0] == '/')
        return target.substr(1);

    std::string base = base_dir;
    while(!base.empty() && base.back() != '/' && base.back() != '\\')
        base.pop_back();

    std::string combined = base + target;
    for(char& c : combined)
        if(c == '\\')
            c = '/';

    std::vector<std::string> parts;
    size_t i = 0;
    while(i < combined.size())
    {
        const size_t j = combined.find('/', i);
        const std::string part = combined.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if(part.empty() || part == ".")
        {
        }
        else if(part == "..")
        {
            if(!parts.empty())
                parts.pop_back();
        }
        else
        {
            parts.push_back(part);
        }
        if(j == std::string::npos)
            break;
        i = j + 1;
    }

    std::string resolved;
    for(size_t p = 0; p < parts.size(); ++p)
    {
        if(p)
            resolved += '/';
        resolved += parts[p];
    }
    return resolved;
}

} // namespace litedocx