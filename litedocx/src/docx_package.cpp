#include "docx_package.h"

#include "docx_style.h"
#include "../third_party/zip.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace litedocx
{
    namespace
    {
        unsigned le16(const unsigned char* p)
        {
            return static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8);
        }

        unsigned long le32(const unsigned char* p)
        {
            return static_cast<unsigned long>(p[0]) | (static_cast<unsigned long>(p[1]) << 8) |
                   (static_cast<unsigned long>(p[2]) << 16) | (static_cast<unsigned long>(p[3]) << 24);
        }

        bool zip_list(const zip_arc& z, std::vector<std::string>& names)
        {
            const unsigned char* d = z.data;
            size_t               size = z.size;
            if(!d || size < 22)
            {
                return false;
            }
            size_t start = size > 65557 ? size - 65557 : 0;
            size_t eocd  = size;
            for(size_t i = size - 22 + 1; i-- > start;)
            {
                if(le32(d + i) == 0x06054b50u)
                {
                    eocd = i;
                    break;
                }
            }
            if(eocd == size)
            {
                return false;
            }
            unsigned entries = le16(d + eocd + 10);
            size_t   pos     = le32(d + eocd + 16);
            if(entries == 0xFFFF || pos >= size)
            {
                return false;
            }
            for(unsigned i = 0; i < entries; ++i)
            {
                if(pos + 46 > size || le32(d + pos) != 0x02014b50u)
                {
                    return false;
                }
                unsigned nlen = le16(d + pos + 28);
                unsigned elen = le16(d + pos + 30);
                unsigned clen = le16(d + pos + 32);
                if(pos + 46 + nlen > size)
                {
                    return false;
                }
                names.emplace_back(reinterpret_cast<const char*>(d + pos + 46), nlen);
                pos += 46 + nlen + elen + clen;
            }
            return true;
        }

        bool zip_read_string(zip_arc& z, const char* name, std::string& out)
        {
            char*  data = nullptr;
            size_t len  = 0;
            if(!zip_read(&z, name, &data, &len) || !data)
            {
                return false;
            }
            out.assign(data, len);
            free(data);
            return true;
        }

        bool has_suffix(const std::string& s, const std::string& suffix)
        {
            return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
    } // namespace

    std::string base_name(const std::string& path)
    {
        size_t slash = path.find_last_of("\\/");
        std::string n = slash == std::string::npos ? path : path.substr(slash + 1);
        size_t dot = n.find_last_of('.');
        if(dot != std::string::npos && dot != 0)
        {
            n.resize(dot);
        }
        return n;
    }

    std::string resolve_zip_path(const std::string& base_dir, const std::string& target)
    {
        std::string t = target;
        std::replace(t.begin(), t.end(), '\\', '/');
        std::string joined;
        if(!t.empty() && t[0] == '/')
        {
            joined = t.substr(1);
        } else
        {
            joined = base_dir.empty() ? t : base_dir + "/" + t;
        }

        std::vector<std::string> parts;
        size_t                   start = 0;
        while(start <= joined.size())
        {
            size_t end = joined.find('/', start);
            if(end == std::string::npos)
            {
                end = joined.size();
            }
            std::string part = joined.substr(start, end - start);
            if(part == "..")
            {
                if(!parts.empty())
                {
                    parts.pop_back();
                }
            } else if(!part.empty() && part != ".")
            {
                parts.push_back(part);
            }
            if(end == joined.size())
            {
                break;
            }
            start = end + 1;
        }
        std::string out;
        for(const auto& p : parts)
        {
            if(!out.empty())
            {
                out += '/';
            }
            out += p;
        }
        return out;
    }

    bool read_file_bytes(const std::string& path, std::vector<unsigned char>& out)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if(!f)
        {
            return false;
        }
        if(fseek(f, 0, SEEK_END) != 0)
        {
            fclose(f);
            return false;
        }
        long sz = ftell(f);
        if(sz < 0 || sz > 256L * 1024L * 1024L)
        {
            fclose(f);
            return false;
        }
        if(fseek(f, 0, SEEK_SET) != 0)
        {
            fclose(f);
            return false;
        }
        out.resize(static_cast<size_t>(sz));
        size_t got = sz ? fread(out.data(), 1, static_cast<size_t>(sz), f) : 0;
        fclose(f);
        if(got != static_cast<size_t>(sz))
        {
            out.clear();
            return false;
        }
        return true;
    }

    bool load_docx_package(const void* data, size_t size, const std::string& file_name, DocxModel& model,
                           std::string& error)
    {
        zip_arc z;
        if(!zip_open(&z, data, size))
        {
            error = "file .docx non valido o troppo piccolo";
            return false;
        }
        if(!zip_read_string(z, "word/document.xml", model.document_xml))
        {
            error = "word/document.xml non trovato o illeggibile";
            return false;
        }
        model.file_name = file_name;

        zip_read_string(z, "word/styles.xml", model.styles_xml);
        zip_read_string(z, "word/numbering.xml", model.numbering_xml);
        zip_read_string(z, "word/theme/theme1.xml", model.theme_xml);
        zip_read_string(z, "word/_rels/document.xml.rels", model.rels_xml);
        if(!model.styles_xml.empty())
        {
            parse_styles(model.styles_xml, model);
        }
        if(!model.numbering_xml.empty())
        {
            parse_numbering(model.numbering_xml, model);
        }
        if(!model.theme_xml.empty())
        {
            parse_theme(model.theme_xml, model);
        }
        if(!model.rels_xml.empty())
        {
            parse_relationships(model.rels_xml, model);
        }

        // Resolve internal relationship targets.
        for(auto& kv : model.rels.items)
        {
            Relationship& r = kv.second;
            if(r.external)
            {
                r.resolved = r.target;
            } else
            {
                r.resolved = resolve_zip_path("word", r.target);
            }
        }

        // Read every image stored in the package (not only those referenced),
        // so that later rel types (headers/footers) can also use them.
        std::vector<std::string> names;
        zip_list(z, names);
        for(const auto& name : names)
        {
            if(name.size() > 11 && name.compare(0, 11, "word/media/") == 0)
            {
                char*  raw = nullptr;
                size_t len = 0;
                if(zip_read(&z, name.c_str(), &raw, &len) && raw)
                {
                    MediaFile mf;
                    mf.name = name;
                    mf.data.assign(reinterpret_cast<unsigned char*>(raw),
                                   reinterpret_cast<unsigned char*>(raw) + len);
                    model.media[name] = std::move(mf);
                    free(raw);
                }
            }
        }

        return true;
    }
} // namespace litedocx
