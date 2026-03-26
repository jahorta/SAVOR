#pragma once

#include <filesystem>
#include <string>

namespace simcore::db {

    struct DbSnapshotResult {
        bool ok = false;
        std::string error;
        std::filesystem::path active_root;
        std::filesystem::path snapshot_path;
        std::filesystem::path target_root;
        bool switched_root = false;
    };

    class DbSnapshotService {
    public:
        static DbSnapshotResult SaveSnapshot(const std::filesystem::path& snapshot_path);
        static DbSnapshotResult LoadSnapshot(const std::filesystem::path& snapshot_path, const std::filesystem::path& target_root, bool switch_to_target = true);
    };

} // namespace simcore::db
