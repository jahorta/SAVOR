#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "UiReadProjectionService.h"

namespace savor::db::uiread::projectors {

struct AttachedUiReadProjectionConfig {
    std::filesystem::path ui_read_db_path;
    std::filesystem::path execution_db_path;
    std::filesystem::path state_db_path;
    std::filesystem::path analysis_db_path;
    std::filesystem::path authoring_db_path;
    std::filesystem::path archive_db_path;
    int max_batch_size = 5000;
    int max_dirty_materialization_batch_size = 1000;
    int max_attempts = 5;
    std::chrono::milliseconds poll_interval{ 250 };
    bool include_archive = true;
};

struct AttachedUiReadProjectionTelemetrySnapshot {
    bool running = false;
    std::uint64_t run_once_count = 0;
    std::uint64_t succeeded_run_once_count = 0;
    std::uint64_t failed_run_once_count = 0;
    std::uint64_t last_run_duration_ms = 0;
    std::uint64_t max_run_duration_ms = 0;
    std::int64_t configured_max_batch_size = 0;
    std::int64_t configured_max_dirty_materialization_batch_size = 0;
    std::int64_t configured_max_attempts = 0;
};

class AttachedUiReadProjectionService final {
public:
    explicit AttachedUiReadProjectionService(AttachedUiReadProjectionConfig config);
    ~AttachedUiReadProjectionService();

    AttachedUiReadProjectionService(const AttachedUiReadProjectionService&) = delete;
    AttachedUiReadProjectionService& operator=(const AttachedUiReadProjectionService&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    [[nodiscard]] bool IsRunning() const;

    bool RunOnce(std::string* error_out = nullptr);
    void Wake();
    [[nodiscard]] AttachedUiReadProjectionTelemetrySnapshot SnapshotTelemetry() const;

private:
    [[nodiscard]] UiReadProjectionConfig MakeConfig() const;

    AttachedUiReadProjectionConfig config_{};
    mutable std::mutex mtx_;
    std::unique_ptr<UiReadProjectionService> service_;
};

} // namespace savor::db::uiread::projectors
