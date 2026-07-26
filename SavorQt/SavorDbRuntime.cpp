#include "SavorDbRuntime.h"

#include <QtCore/QCoreApplication>

#include <filesystem>
#include <system_error>
#include <utility>
#include <memory>

#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"

namespace savorqt {
namespace {

savor::db::DbConfigPaths BuildPaths(const std::filesystem::path& root) {
    return savor::db::DbConfigPaths{
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
            *error_out = "failed creating SavorDb root: " + ec.message();
        }
        return false;
    }
    std::filesystem::create_directories(root / "object_store", ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = "failed creating SavorDb artifact storage root: " + ec.message();
        }
        return false;
    }
    std::filesystem::create_directories(root / "archive_store", ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = "failed creating SavorDb archive store root: " + ec.message();
        }
        return false;
    }
    return true;
}

} // namespace

SavorDbRuntime& SavorDbRuntime::instance() {
    static SavorDbRuntime runtime;
    return runtime;
}

bool SavorDbRuntime::start(const std::filesystem::path& root, std::string* error_out) {
    if (service_ != nullptr && service_->IsRunning()) {
        return true;
    }
    if (root.empty()) {
        if (error_out != nullptr) {
            *error_out = "SavorDb root is empty";
        }
        return false;
    }
    if (!EnsureStorageRoot(root, error_out)) {
        return false;
    }

    auto service = std::make_unique<savor::db::core::DBService>(BuildPaths(root));
    if (!service->Start(error_out)) {
        return false;
    }
    root_ = root;
    service_ = std::move(service);

    if (!buildProgramRegistry(error_out)) {
        stop();
        return false;
    }

	std::function<void(const std::string&)> event_line_callback = [](const std::string& line) {
        (void)line;
		};

    auto workflow_coordinator =
        std::make_unique<savor::db::execution::workflow::WorkflowCoordinatorService>(
            service_->ExecutionDb(),
            &program_registry_,
            buildWorkflowConfig(),
            std::move(event_line_callback),
            nullptr,
            service_->AuthoringDb());
    std::string workflow_error;
    if (!workflow_coordinator->Start(&workflow_error)) {
        if (error_out != nullptr) {
            *error_out = "workflow coordinator startup failed: " + workflow_error;
        }
        stop();
        return false;
    }
    workflow_coordinator_ = std::move(workflow_coordinator);
    return true;
}

void SavorDbRuntime::stop() {
    if (workflow_coordinator_ != nullptr) {
        workflow_coordinator_->Stop();
        workflow_coordinator_.reset();
    }
    program_registry_ = savor::db::execution::programdb::ProgramKindRegistry{};
    if (service_ != nullptr) {
        service_->Stop();
        service_.reset();
    }
}

bool SavorDbRuntime::isRunning() const {
    return service_ != nullptr && service_->IsRunning();
}

std::filesystem::path SavorDbRuntime::root() const {
    return root_;
}

bool SavorDbRuntime::switchRoot(const std::filesystem::path& root, std::string* error_out) {
    stop();
    return start(root, error_out);
}

bool SavorDbRuntime::resetRoot(std::string* error_out) {
    if (root_.empty()) {
        if (error_out != nullptr) {
            *error_out = "SavorDb root is empty";
        }
        return false;
    }
    const auto target = root_;
    stop();
    std::error_code ec;
    std::filesystem::remove_all(target, ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = "failed deleting SavorDb root: " + ec.message();
        }
        return false;
    }
    return start(target, error_out);
}

bool SavorDbRuntime::relocateRoot(const std::filesystem::path& root, bool cleanup_source, std::string* error_out) {
    if (root_.empty()) {
        if (error_out != nullptr) {
            *error_out = "SavorDb root is empty";
        }
        return false;
    }
    const auto source = root_;
    stop();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = "failed creating target SavorDb root: " + ec.message();
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
            *error_out = "failed copying SavorDb root: " + ec.message();
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

savor::db::core::DBService* SavorDbRuntime::service() {
    return service_.get();
}

savor::db::IUiReadDb* SavorDbRuntime::uiReadDb() {
    return service_ != nullptr ? service_->UiReadDb() : nullptr;
}

savor::db::IStateDb* SavorDbRuntime::stateDb() {
    return service_ != nullptr ? service_->StateDb() : nullptr;
}

savor::db::IAnalysisDb* SavorDbRuntime::analysisDb() {
    return service_ != nullptr ? service_->AnalysisDb() : nullptr;
}

savor::db::IAuthoringDb* SavorDbRuntime::authoringDb() {
    return service_ != nullptr ? service_->AuthoringDb() : nullptr;
}

savor::db::IExecutionDb* SavorDbRuntime::executionDb() {
    return service_ != nullptr ? service_->ExecutionDb() : nullptr;
}

savor::db::execution::workflow::IWorkflowOrchestrationQueryService* SavorDbRuntime::workflowQueryService() {
    auto* db = executionDb();
    return db != nullptr ? db->WorkflowQueryService() : nullptr;
}

savor::db::execution::workflow::IWorkflowOrchestrationCommandService* SavorDbRuntime::workflowCommandService() {
    auto* db = executionDb();
    return db != nullptr ? db->WorkflowCommandService() : nullptr;
}

savor::db::execution::programdb::ProgramKindRegistry* SavorDbRuntime::programKindRegistry() {
    return service_ != nullptr && service_->IsRunning() ? &program_registry_ : nullptr;
}

savor::db::execution::workflow::WorkflowCoordinatorTelemetry SavorDbRuntime::workflowCoordinatorTelemetry() const {
    return workflow_coordinator_ != nullptr
        ? workflow_coordinator_->SnapshotTelemetry()
        : savor::db::execution::workflow::WorkflowCoordinatorTelemetry{};
}

bool SavorDbRuntime::workflowCoordinatorRunning() const {
    return workflow_coordinator_ != nullptr && workflow_coordinator_->IsRunning();
}

bool SavorDbRuntime::buildProgramRegistry(std::string* error_out) {
    const auto app_dir = std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
    const auto workspace_root = app_dir / "workflow-runtime";
    auto config =
        savor::db::execution::programdb::MakeProductionProgramKindRegistryConfig(
            workspace_root);
    const savor::db::execution::programdb::ProductionProgramKindRegistryDependencies dependencies{
        .execution_db = executionDb(),
        .state_db = stateDb(),
        .analysis_db = analysisDb(),
        .authoring_db = authoringDb(),
    };
    return savor::db::execution::programdb::BuildProductionProgramKindRegistry(
        dependencies,
        std::move(config),
        &program_registry_,
        error_out);
}

savor::db::execution::workflow::WorkflowCoordinatorConfig SavorDbRuntime::buildWorkflowConfig() {
    return savor::db::execution::workflow::WorkflowCoordinatorConfig{
        .workflow_enabled = true,
        .strict_smoke_terminal_on_failure = false,
    };
}

} // namespace savorqt
