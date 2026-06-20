#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace savor::db::archive {

enum class ArchiveOperationPhase {
    Previewing,
    ExportingRows,
    WritingSavestates,
    RegisteringPackage,
    VerifyingPackage,
    PurgingSource,
    Complete,
    Failed,
};

struct ArchiveOperationProgress {
    ArchiveOperationPhase phase = ArchiveOperationPhase::Previewing;
    std::string message;
    std::int64_t completed_units = 0;
    std::int64_t total_units = 0;
    std::optional<std::uint64_t> bytes_completed;
    std::optional<std::uint64_t> bytes_total;
    bool indeterminate = true;
};

using ArchiveProgressSink = std::function<void(const ArchiveOperationProgress&)>;

inline void EmitArchiveProgress(
    const ArchiveProgressSink& sink,
    ArchiveOperationPhase phase,
    std::string message,
    std::int64_t completed_units = 0,
    std::int64_t total_units = 0,
    bool indeterminate = true,
    std::optional<std::uint64_t> bytes_completed = std::nullopt,
    std::optional<std::uint64_t> bytes_total = std::nullopt) {
    if (!sink) {
        return;
    }

    ArchiveOperationProgress progress{};
    progress.phase = phase;
    progress.message = std::move(message);
    progress.completed_units = completed_units;
    progress.total_units = total_units;
    progress.bytes_completed = bytes_completed;
    progress.bytes_total = bytes_total;
    progress.indeterminate = indeterminate;
    sink(progress);
}

} // namespace savor::db::archive
