#ifndef LITEDOCX_XML_DOM_H
#define LITEDOCX_XML_DOM_H

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace litedocx
{
    struct XmlNode
    {
        std::string name;
        std::map<std::string, std::string> attrs;
        std::string text; // decoded text directly inside this element
        std::vector<std::unique_ptr<XmlNode>> children;

        bool has(const std::string& a) const
        {
            return attrs.find(a) != attrs.end();
        }
        std::string attr(const std::string& a, const std::string& def = std::string()) const
        {
            auto it = attrs.find(a);
            return it == attrs.end() ? def : it->second;
        }
        std::string attr_local(const std::string& local, const std::string& def = std::string()) const;
        const XmlNode* child(const std::string& local) const;
        std::vector<const XmlNode*> children_named(const std::string& local) const;
        bool is(const std::string& local) const;
    };

    std::string xml_local_name(const std::string& name);

    // Parse a UTF-8 XML document.  Returns the document element, or nullptr.
    std::unique_ptr<XmlNode> xml_parse(const char* data, size_t len);

} // namespace litedocx

#endif // LITEDOCX_XML_DOM_H
