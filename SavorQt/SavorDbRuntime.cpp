#include "SavorDbRuntime.h"

#include <QtCore/QCoreApplication>

#include <array>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Common/DatabaseBootstrap.h"
#include "Common/WorkspaceStagingCleanup.h"

namespace savorqt {
namespace {

savor::db::DbConfigPaths BuildPaths(
    const std::filesystem::path& root,
    const std::filesystem::path& artifact_workspace_root) {
    auto paths = savor::db::bootstrap::MakeDatabaseRootConfigPaths(root);
    paths.artifact_workspace_root = artifact_workspace_root;
    return paths;
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

    auto service = std::make_unique<savor::db::core::DBService>(
        BuildPaths(root, resultStagingRoot()));
    if (!service->Start(error_out)) {
        return false;
    }
    root_ = root;
    service_ = std::move(service);

    if (!buildProgramRegistry(error_out)) {
        stop();
        return false;
    }

    return true;
}

void SavorDbRuntime::stop() {
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

bool SavorDbRuntime::resetRoot(
    savor::db::bootstrap::DatabaseBootstrapProfileId profile_id,
    savor::db::bootstrap::DatabaseRootBootstrapResult* result_out,
    std::string* error_out) {
    if (root_.empty()) {
        if (error_out != nullptr) {
            *error_out = "SavorDb root is empty";
        }
        return false;
    }
    if (service_ == nullptr || !service_->IsRunning()) {
        if (error_out != nullptr) *error_out = "SavorDb service is not running";
        return false;
    }

    program_registry_ = savor::db::execution::programdb::ProgramKindRegistry{};
    const savor::db::bootstrap::DatabaseRootBootstrapService bootstrap;
    const bool recreated = bootstrap.RecreateRoot(
        *service_,
        {.target_root = root_, .profile_id = profile_id},
        result_out,
        error_out);

    std::string registry_error;
    if (service_->IsRunning() && !buildProgramRegistry(&registry_error)) {
        if (error_out != nullptr) {
            if (!error_out->empty()) *error_out += "; ";
            *error_out += "failed rebuilding program registry: " + registry_error;
        }
        return false;
    }
    return recreated;
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

std::filesystem::path SavorDbRuntime::resultStagingRoot() const {
    return std::filesystem::path(
        QCoreApplication::applicationDirPath().toStdWString())
        / "workflow-runtime";
}

bool SavorDbRuntime::resetResultStaging(
    std::string* summary_out,
    std::string* error_out) {
    if (!isRunning() || executionDb() == nullptr) {
        if (error_out) *error_out = "SavorDb runtime is not running";
        return false;
    }
    std::int64_t pending = 0;
    if (!executionDb()->GetResultStagingCleanupCount(&pending, error_out))
        return false;

    savor::db::WorkspaceStagingCleanupSummary cleanup{};
    const auto application_root = std::filesystem::path(
        QCoreApplication::applicationDirPath().toStdWString());
    const std::array<std::filesystem::path, 1> directories{
        "workflow-runtime"};
    if (!savor::db::ResetWorkspaceStagingDirectories(
            application_root, directories, &cleanup, error_out))
        return false;
    std::int64_t cleared = 0;
    if (!executionDb()->ClearResultStagingCleanupQueue(&cleared, error_out))
        return false;
    if (summary_out) {
        *summary_out = "Result staging reset: removed "
            + std::to_string(cleanup.files) + " files, "
            + std::to_string(cleanup.directories) + " directories, "
            + std::to_string(cleanup.bytes) + " bytes; resolved "
            + std::to_string(cleared) + " of " + std::to_string(pending)
            + " pending cleanup rows.";
    }
    if (error_out) error_out->clear();
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

bool SavorDbRuntime::buildProgramRegistry(std::string* error_out) {
    const auto app_dir = std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
    const auto workspace_root = resultStagingRoot();
    auto config =
        savor::db::execution::programdb::MakeProductionProgramKindRegistryConfig(
            workspace_root,
            app_dir / "SavorWorker.exe");
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

} // namespace savorqt
