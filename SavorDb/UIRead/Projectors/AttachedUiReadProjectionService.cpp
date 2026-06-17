#include "AttachedUiReadProjectionService.h"

#include <utility>

namespace savor::db::uiread::projectors {

AttachedUiReadProjectionService::AttachedUiReadProjectionService(AttachedUiReadProjectionConfig config)
    : config_(std::move(config)) {
}

AttachedUiReadProjectionService::~AttachedUiReadProjectionService() {
    Stop();
}

UiReadProjectionConfig AttachedUiReadProjectionService::MakeConfig() const {
    return UiReadProjectionConfig{
        .ui_read_db_path = config_.ui_read_db_path,
        .execution_db_path = config_.execution_db_path,
        .state_db_path = config_.state_db_path,
        .analysis_db_path = config_.analysis_db_path,
        .archive_db_path = config_.archive_db_path,
        .max_batch_size = config_.max_batch_size,
        .max_dirty_materialization_batch_size = config_.max_dirty_materialization_batch_size,
        .max_attempts = config_.max_attempts,
        .poll_interval = config_.poll_interval,
    };
}

bool AttachedUiReadProjectionService::Start(std::string* error_out) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (service_ != nullptr && service_->IsRunning()) {
        return true;
    }

    service_ = std::make_unique<UiReadProjectionService>(MakeConfig());
    if (!service_->Start(error_out)) {
        service_.reset();
        return false;
    }
    return true;
}

void AttachedUiReadProjectionService::Stop() {
    std::unique_ptr<UiReadProjectionService> service;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        service = std::move(service_);
    }
    if (service != nullptr) {
        service->Stop();
    }
}

bool AttachedUiReadProjectionService::IsRunning() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return service_ != nullptr && service_->IsRunning();
}

bool AttachedUiReadProjectionService::RunOnce(std::string* error_out) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (service_ == nullptr) {
        service_ = std::make_unique<UiReadProjectionService>(MakeConfig());
    }
    return service_->RunOnce(error_out);
}

void AttachedUiReadProjectionService::Wake() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (service_ != nullptr) {
        service_->Wake();
    }
}

AttachedUiReadProjectionTelemetrySnapshot AttachedUiReadProjectionService::SnapshotTelemetry() const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (service_ == nullptr) {
        return AttachedUiReadProjectionTelemetrySnapshot{
            .configured_max_batch_size = config_.max_batch_size,
            .configured_max_dirty_materialization_batch_size = config_.max_dirty_materialization_batch_size,
            .configured_max_attempts = config_.max_attempts,
        };
    }

    const auto snapshot = service_->SnapshotTelemetry();
    return AttachedUiReadProjectionTelemetrySnapshot{
        .running = snapshot.running,
        .run_once_count = snapshot.run_once_count,
        .succeeded_run_once_count = snapshot.succeeded_run_once_count,
        .failed_run_once_count = snapshot.failed_run_once_count,
        .last_run_duration_ms = snapshot.last_run_duration_ms,
        .max_run_duration_ms = snapshot.max_run_duration_ms,
        .configured_max_batch_size = snapshot.configured_max_batch_size,
        .configured_max_dirty_materialization_batch_size = snapshot.configured_max_dirty_materialization_batch_size,
        .configured_max_attempts = snapshot.configured_max_attempts,
    };
}

} // namespace savor::db::uiread::projectors
