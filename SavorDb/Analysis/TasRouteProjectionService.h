#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace savor::db {
struct IAnalysisDb;
}

namespace savor::db::analysis {

struct TasRouteProjectionConfig {
    std::filesystem::path execution_db_path;
    std::filesystem::path analysis_db_path;
    std::chrono::milliseconds poll_interval{500};
};

// TAS Routes is a read model. This service may lag or report diagnostics, but
// it never participates in workflow admission or job materialization.
class TasRouteProjectionService final {
public:
    TasRouteProjectionService(
        TasRouteProjectionConfig config,
        savor::db::IAnalysisDb* analysis_db);
    ~TasRouteProjectionService();

    TasRouteProjectionService(const TasRouteProjectionService&) = delete;
    TasRouteProjectionService& operator=(const TasRouteProjectionService&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    void Wake();
    bool RunOnce(std::string* error_out = nullptr);

private:
    void Run();

    TasRouteProjectionConfig config_{};
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace savor::db::analysis
