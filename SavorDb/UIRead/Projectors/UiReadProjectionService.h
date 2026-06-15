#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>

namespace savor::db::uiread::projectors {

struct UiReadProjectionConfig {
    std::filesystem::path ui_read_db_path;
    std::filesystem::path execution_db_path;
    std::filesystem::path state_db_path;
    std::filesystem::path analysis_db_path;
    std::filesystem::path archive_db_path;
    int max_batch_size = 100;
    int max_attempts = 5;
    std::chrono::milliseconds poll_interval{ 250 };
};

struct UiReadProjectionStreamTelemetrySnapshot {
    std::string stream_id;
    std::string source_context;
    std::string source_outbox_table;
    bool running = false;
    std::uint64_t run_once_count = 0;
    std::uint64_t succeeded_run_once_count = 0;
    std::uint64_t failed_run_once_count = 0;
    std::uint64_t processed_event_count = 0;
    std::uint64_t dead_letter_count = 0;
    std::uint64_t last_run_duration_ms = 0;
    std::uint64_t max_run_duration_ms = 0;
    std::int64_t last_outbox_id = 0;
    std::int64_t source_high_water_outbox_id = 0;
    std::int64_t lag_count = 0;
    std::int64_t lag_age_ms = 0;
    std::string last_error;
};

struct UiReadProjectionTelemetrySnapshot {
    bool running = false;
    std::uint64_t run_once_count = 0;
    std::uint64_t succeeded_run_once_count = 0;
    std::uint64_t failed_run_once_count = 0;
    std::uint64_t last_run_duration_ms = 0;
    std::uint64_t max_run_duration_ms = 0;
    std::int64_t configured_max_batch_size = 0;
    std::int64_t configured_max_attempts = 0;
    std::vector<UiReadProjectionStreamTelemetrySnapshot> streams;
};

class UiReadProjectionService final {
public:
    struct StreamRuntime;

    explicit UiReadProjectionService(UiReadProjectionConfig config);
    ~UiReadProjectionService();

    UiReadProjectionService(const UiReadProjectionService&) = delete;
    UiReadProjectionService& operator=(const UiReadProjectionService&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    [[nodiscard]] bool IsRunning() const;

    bool RunOnce(std::string* error_out = nullptr);
    void Wake();
    [[nodiscard]] UiReadProjectionTelemetrySnapshot SnapshotTelemetry() const;

private:
    bool OpenStream(StreamRuntime& stream, std::string* error_out);
    void CloseStream(StreamRuntime& stream);
    bool RunStreamOnce(StreamRuntime& stream, std::string* error_out);
    void WorkerLoop(StreamRuntime* stream);
    [[nodiscard]] bool IsStoppingRequested() const;
    void InterruptStreams() const;

    UiReadProjectionConfig config_{};
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<StreamRuntime>> streams_;
    bool running_ = false;
    bool stopping_ = false;
};

} // namespace savor::db::uiread::projectors
