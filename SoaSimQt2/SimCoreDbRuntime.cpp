#include "SimCoreDbRuntime.h"

#include <filesystem>
#include <system_error>

namespace soasimqt2 {
namespace {

simcore::db::DbConfigPaths BuildPaths(const std::filesystem::path& root) {
    return simcore::db::DbConfigPaths{
        .execution_db_path = root / "execution.db",
        .state_db_path = root / "state.db",
        .analysis_db_path = root / "analysis.db",
        .authoring_db_path = root / "authoring.db",
        .ui_read_db_path = root / "ui_read.db",
        .archive_db_path = root / "archive.db",
        .object_store_root = root / "object_store",
        .archive_store_root = root / "archive_store",
    };
}

bool EnsureStorageRoot(const std::filesystem::path& root, std::string* error_out) {
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = "failed creating SimCoreDB root: " + ec.message();
        }
        return false;
    }
    std::filesystem::create_directories(root / "object_store", ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = "failed creating SimCoreDB object store root: " + ec.message();
        }
        return false;
    }
    std::filesystem::create_directories(root / "archive_store", ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = "failed creating SimCoreDB archive store root: " + ec.message();
        }
        return false;
    }
    return true;
}

} // namespace

SimCoreDbRuntime& SimCoreDbRuntime::instance() {
    static SimCoreDbRuntime runtime;
    return runtime;
}

bool SimCoreDbRuntime::start(const std::filesystem::path& root, std::string* error_out) {
    if (service_ != nullptr && service_->IsRunning()) {
        return true;
    }
    if (root.empty()) {
        if (error_out != nullptr) {
            *error_out = "SimCoreDB root is empty";
        }
        return false;
    }
    if (!EnsureStorageRoot(root, error_out)) {
        return false;
    }

    auto service = std::make_unique<simcore::db::core::DBService>(BuildPaths(root));
    if (!service->Start(error_out)) {
        return false;
    }
    root_ = root;
    service_ = std::move(service);
    return true;
}

void SimCoreDbRuntime::stop() {
    if (service_ != nullptr) {
        service_->Stop();
        service_.reset();
    }
}

bool SimCoreDbRuntime::isRunning() const {
    return service_ != nullptr && service_->IsRunning();
}

std::filesystem::path SimCoreDbRuntime::root() const {
    return root_;
}

bool SimCoreDbRuntime::switchRoot(const std::filesystem::path& root, std::string* error_out) {
    stop();
    return start(root, error_out);
}

bool SimCoreDbRuntime::resetRoot(std::string* error_out) {
    if (root_.empty()) {
        if (error_out != nullptr) {
            *error_out = "SimCoreDB root is empty";
        }
        return false;
    }
    const auto target = root_;
    stop();
    std::error_code ec;
    std::filesystem::remove_all(target, ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = "failed deleting SimCoreDB root: " + ec.message();
        }
        return false;
    }
    return start(target, error_out);
}

bool SimCoreDbRuntime::relocateRoot(const std::filesystem::path& root, bool cleanup_source, std::string* error_out) {
    if (root_.empty()) {
        if (error_out != nullptr) {
            *error_out = "SimCoreDB root is empty";
        }
        return false;
    }
    const auto source = root_;
    stop();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = "failed creating target SimCoreDB root: " + ec.message();
        }
        (void)start(source, nullptr);
        return false;
    }
    std::filesystem::copy(
        source,
        root,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
        ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = "failed copying SimCoreDB root: " + ec.message();
        }
        (void)start(source, nullptr);
        return false;
    }
    if (!start(root, error_out)) {
        (void)start(source, nullptr);
        return false;
    }
    if (cleanup_source) {
        std::filesystem::remove_all(source, ec);
    }
    return true;
}

simcore::db::core::DBService* SimCoreDbRuntime::service() {
    return service_.get();
}

simcore::db::IUiReadDb* SimCoreDbRuntime::uiReadDb() {
    return service_ != nullptr ? service_->UiReadDb() : nullptr;
}

simcore::db::IStateDb* SimCoreDbRuntime::stateDb() {
    return service_ != nullptr ? service_->StateDb() : nullptr;
}

simcore::db::IAnalysisDb* SimCoreDbRuntime::analysisDb() {
    return service_ != nullptr ? service_->AnalysisDb() : nullptr;
}

simcore::db::IAuthoringDb* SimCoreDbRuntime::authoringDb() {
    return service_ != nullptr ? service_->AuthoringDb() : nullptr;
}

simcore::db::IExecutionDb* SimCoreDbRuntime::executionDb() {
    return service_ != nullptr ? service_->ExecutionDb() : nullptr;
}

simcore::db::execution::workflow::IWorkflowOrchestrationQueryService* SimCoreDbRuntime::workflowQueryService() {
    auto* db = executionDb();
    return db != nullptr ? db->WorkflowQueryService() : nullptr;
}

simcore::db::execution::workflow::IWorkflowOrchestrationCommandService* SimCoreDbRuntime::workflowCommandService() {
    auto* db = executionDb();
    return db != nullptr ? db->WorkflowCommandService() : nullptr;
}

} // namespace soasimqt2
