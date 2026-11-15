#include "xml_dom.h"

#include "../third_party/xmlbuf.h"

#include <cstring>

namespace litedocx
{
    std::string_view xml_local_name(std::string_view name)
    {
        auto pos = name.rfind(':');
        return pos == std::string_view::npos ? name : name.substr(pos + 1);
    }

    bool XmlNode::is(std::string_view local) const
    {
        return xml_local_name(name) == local;
    }

    std::string XmlNode::attr_local(std::string_view local, std::string_view def) const
    {
        for(const auto& a : attrs)
        {
            if(xml_local_name(a.first) == local)
            {
                return a.second;
            }
        }
        return std::string(def);
    }

    const XmlNode* XmlNode::child(std::string_view local) const
    {
        for(const auto& c : children)
        {
            if(c->is(local))
            {
                return c.get();
            }
        }
        return nullptr;
    }

    std::vector<const XmlNode*> XmlNode::children_named(std::string_view local) const
    {
        std::vector<const XmlNode*> out;
        for(const auto& c : children)
        {
            if(c->is(local))
            {
                out.push_back(c.get());
            }
        }
        return out;
    }

    namespace
    {
        // Map key = string_view into original XML buffer; value = owned decoded string
        using AttrMap = std::map<std::string_view, std::string, std::less<>>;

        void parse_attrs(const char* p, size_t n, AttrMap& out)
        {
            const char* end = p + n;
            while(p < end)
            {
                while(p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '/'))
                {
                    ++p;
                }
                if(p >= end)
                {
                    break;
                }
                const char* name = p;
                while(p < end && *p != '=' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>')
                {
                    ++p;
                }
                if(name == p)
                {
                    ++p;
                    continue;
                }
                std::string_view key(name, static_cast<size_t>(p - name));
                while(p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                {
                    ++p;
                }
                std::string value;
                if(p < end && *p == '=')
                {
                    ++p;
                    while(p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                    {
                        ++p;
                    }
                    if(p < end && (*p == '"' || *p == '\''))
                    {
                        char quote = *p++;
                        const char* v = p;
                        while(p < end && *p != quote)
                        {
                            ++p;
                        }
                        value.assign(v, static_cast<size_t>(p - v));
                        if(p < end)
                        {
                            ++p;
                        }
                    } else
                    {
                        const char* v = p;
                        while(p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>')
                        {
                            ++p;
                        }
                        value.assign(v, static_cast<size_t>(p - v));
                    }
                }
                // Decode XML entities in attribute values.
                std::string decoded(value.size() + 1, '\0');
                size_t len = xml_decode(value.data(), value.size(), decoded.data(), decoded.size());
                decoded.resize(len);
                out.emplace(key, std::move(decoded));
            }
        }
    } // namespace

    std::unique_ptr<XmlNode> xml_parse(const char* data, size_t len)
    {
        if(!data || !len)
        {
            return nullptr;
        }

        std::unique_ptr<XmlNode> root;
        std::vector<XmlNode*> stack;
        xscan                    scan;
        xtok                     tok;

        xs_init(&scan, data, len);
        while(xs_next(&scan, &tok))
        {
            if(tok.kind == XT_TEXT)
            {
                if(stack.empty() || !tok.text_len)
                {
                    continue;
                }
                std::string decoded(tok.text_len + 1, '\0');
                size_t      n = xml_decode(tok.text, tok.text_len, decoded.data(), decoded.size());
                decoded.resize(n);
                stack.back()->text += decoded;
                continue;
            }

            if(tok.closing)
            {
                if(stack.empty())
                {
                    continue;
                }
                std::string_view local = xml_local_name(std::string_view(tok.name, tok.name_len));
                for(size_t i = stack.size(); i-- > 0;)
                {
                    if(xml_local_name(stack[i]->name) == local)
                    {
                        stack.resize(i);
                        break;
                    }
                }
                continue;
            }

            auto node = std::unique_ptr<XmlNode>(new XmlNode());
            node->name = std::string_view(tok.name, tok.name_len);
            parse_attrs(tok.attrs, tok.attrs_len, node->attrs);

            XmlNode* raw = node.get();
            if(stack.empty())
            {
                if(!root)
                {
                    root = std::move(node);
                }
            } else
            {
                stack.back()->children.push_back(std::move(node));
            }
            if(!tok.self_closing)
            {
                stack.push_back(raw);
            }
        }
        return root;
    }

    namespace
    {
        void escape_xml_text(const std::string& in, std::string& out)
        {
            out.reserve(out.size() + in.size());
            for(char c : in)
            {
                switch(c)
                {
                    case '&': out += "&amp;"; break;
                    case '<': out += "&lt;"; break;
                    case '>': out += "&gt;"; break;
                    default: out.push_back(c); break;
                }
            }
        }

        void escape_xml_attr(const std::string& in, std::string& out)
        {
            out.reserve(out.size() + in.size());
            for(char c : in)
            {
                switch(c)
                {
                    case '&': out += "&amp;"; break;
                    case '<': out += "&lt;"; break;
                    case '>': out += "&gt;"; break;
                    case '"': out += "&quot;"; break;
                    case '\n': out += "&#10;"; break;
                    case '\r': out += "&#13;"; break;
                    case '\t': out += "&#9;"; break;
                    default: out.push_back(c); break;
                }
            }
        }

        void serialize_into(const XmlNode& node, std::string& out)
        {
            out.push_back('<');
            out.append(node.name.data(), node.name.size());
            for(const auto& a : node.attrs)
            {
                out.push_back(' ');
                out.append(a.first.data(), a.first.size());
                out += "=\"";
                escape_xml_attr(a.second, out);
                out.push_back('"');
            }
            if(node.text.empty() && node.children.empty())
            {
                out += "/>";
                return;
            }
            out.push_back('>');
            escape_xml_text(node.text, out);
            for(const auto& c : node.children)
            {
                if(c)
                {
                    serialize_into(*c, out);
                }
            }
            out += "</";
            out.append(node.name.data(), node.name.size());
            out.push_back('>');
        }
    } // namespace

    std::string xml_serialize(const XmlNode& node)
    {
        std::string out;
        serialize_into(node, out);
        return out;
    }
} // namespace litedocx
