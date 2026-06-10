#pragma once

#include <filesystem>

namespace savor::db {

struct DbConfigPaths {
    std::filesystem::path execution_db_path;
    std::filesystem::path state_db_path;
    std::filesystem::path analysis_db_path;
    std::filesystem::path authoring_db_path;
    std::filesystem::path ui_read_db_path;
    std::filesystem::path archive_db_path;
    std::filesystem::path object_store_root;
    std::filesystem::path archive_store_root;
};

} // namespace savor::db
