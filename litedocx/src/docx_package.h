#ifndef LITEDOCX_DOCX_PACKAGE_H
#define LITEDOCX_DOCX_PACKAGE_H

#include "docx_model.h"

#include <cstddef>
#include <string>

namespace litedocx
{
    bool load_docx_package(const void* data, size_t size, const std::string& file_name, DocxModel& model,
                           std::string& error);

    bool read_file_bytes(const std::string& path, std::vector<unsigned char>& out);

    std::string resolve_zip_path(const std::string& base_dir, const std::string& target);
    std::string base_name(const std::string& path);

} // namespace litedocx

#endif // LITEDOCX_DOCX_PACKAGE_H
