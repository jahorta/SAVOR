#pragma once

#include <filesystem>
#include <string>

bool ExportCurrentDbSchemas(
    const std::filesystem::path& migration_root,
    std::string* error_out);
