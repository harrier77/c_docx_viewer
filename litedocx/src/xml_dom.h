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
        std::string_view name;
        std::map<std::string_view, std::string, std::less<>> attrs;
        std::string text; // decoded text directly inside this element
        std::vector<std::unique_ptr<XmlNode>> children;

        bool has(std::string_view a) const
        {
            return attrs.find(a) != attrs.end();
        }
        // Returns owned string (small copy) for backward compatibility with callers.
        // Keys in the map are string_view (no allocation for attribute names).
        std::string attr(std::string_view a, std::string_view def = {}) const
        {
            auto it = attrs.find(a);
            return it == attrs.end() ? std::string(def) : it->second;
        }
        std::string attr_local(std::string_view local, std::string_view def = {}) const;
        const XmlNode* child(std::string_view local) const;
        std::vector<const XmlNode*> children_named(std::string_view local) const;
        bool is(std::string_view local) const;
    };

    std::string_view xml_local_name(std::string_view name);

    // Parse a UTF-8 XML document.  Returns the document element, or nullptr.
    std::unique_ptr<XmlNode> xml_parse(const char* data, size_t len);

} // namespace litedocx

#endif // LITEDOCX_XML_DOM_H
