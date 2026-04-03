#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace simcore::db {

    enum class DbSnapshotPhase {
        Preparing,
        ScanningArtifacts,
        WritingCoreEntries,
        WritingArtifacts,
        Finalizing,
    };

    struct DbSnapshotProgress {
        DbSnapshotPhase phase = DbSnapshotPhase::Preparing;
        uint64_t completed = 0;
        uint64_t total = 0;
        std::string detail;
    };

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
        using ProgressCallback = std::function<void(const DbSnapshotProgress&)>;

        static DbSnapshotResult SaveSnapshot(const std::filesystem::path& snapshot_path, ProgressCallback on_progress = {});
        static DbSnapshotResult LoadSnapshot(const std::filesystem::path& snapshot_path, const std::filesystem::path& target_root, bool switch_to_target = true);
    };

} // namespace simcore::db
