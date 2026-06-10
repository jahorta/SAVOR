#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace savor::db {

enum class DbSnapshotPhase {
    Preparing,
    ScanningArtifacts,
    WritingCoreEntries,
    WritingArtifacts,
    Finalizing,
    Starting,
    Reading,
    Writing,
    Complete,
    Failed
};

struct DbSnapshotProgress {
    DbSnapshotPhase phase = DbSnapshotPhase::Preparing;
    std::int64_t completed{};
    std::int64_t current{};
    std::int64_t total{};
    std::string message;
    std::string detail;
};

struct DbSnapshotResult {
    bool ok = false;
    std::string error;

    static DbSnapshotResult Ok();
    static DbSnapshotResult Err(std::string message);
};

struct DbSnapshotService {
    static DbSnapshotResult SaveSnapshot(
        const std::string& snapshot_path,
        std::function<void(const DbSnapshotProgress&)> progress_callback);

    static DbSnapshotResult LoadSnapshot(
        const std::string& snapshot_path,
        const std::string& target_root,
        bool switch_to_target);
};

} // namespace savor::db
