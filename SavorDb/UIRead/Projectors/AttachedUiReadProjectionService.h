#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>

namespace savor::db::uiread::projectors {

struct AttachedUiReadProjectionConfig {
    std::filesystem::path ui_read_db_path;
    std::filesystem::path execution_db_path;
    std::filesystem::path state_db_path;
    std::filesystem::path analysis_db_path;
    std::filesystem::path authoring_db_path;
    std::filesystem::path archive_db_path;
    int max_batch_size = 100;
    int max_attempts = 5;
    std::chrono::milliseconds poll_interval{ 250 };
    bool include_archive = true;
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

private:
    bool OpenAndAttach(std::string* error_out);
    bool AttachSourceDatabases(std::string* error_out);
    bool AttachDatabase(const std::filesystem::path& db_path, const std::string& schema_name, std::string* error_out);
    bool ConfigureConnection(std::string* error_out);
    void WorkerLoop();
    void CloseConnection();

    AttachedUiReadProjectionConfig config_{};
    sqlite3* db_ = nullptr;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
    bool running_ = false;
    bool stopping_ = false;
};

} // namespace savor::db::uiread::projectors
