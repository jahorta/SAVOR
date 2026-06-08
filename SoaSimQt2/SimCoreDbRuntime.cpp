#include "SimCoreDbRuntime.h"

#include <QtCore/QCoreApplication>

#include <filesystem>
#include <system_error>
#include <utility>

#include "Execution/ProgramDB/BattleContext/BattleContextProbePhaseRegistration.h"
#include "Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnPhaseRegistration.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbePhaseRegistration.h"
#include "Execution/ProgramDB/TasMovie/TasMoviePhaseRegistration.h"
#include "Runner/IPC/Wire.h"

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
            *error_out = "failed creating SimCoreDB artifact storage root: " + ec.message();
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

    if (!buildProgramRegistry(error_out)) {
        stop();
        return false;
    }

    auto workflow_coordinator =
        std::make_unique<simcore::db::execution::workflow::WorkflowCoordinatorService>(
            service_->ExecutionDb(),
            &program_registry_,
            buildWorkflowConfig(),
            {},
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

void SimCoreDbRuntime::stop() {
    if (workflow_coordinator_ != nullptr) {
        workflow_coordinator_->Stop();
        workflow_coordinator_.reset();
    }
    program_registry_ = simcore::db::execution::programdb::ProgramKindRegistry{};
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

simcore::db::execution::programdb::ProgramKindRegistry* SimCoreDbRuntime::programKindRegistry() {
    return service_ != nullptr && service_->IsRunning() ? &program_registry_ : nullptr;
}

simcore::db::execution::workflow::WorkflowCoordinatorTelemetry SimCoreDbRuntime::workflowCoordinatorTelemetry() const {
    return workflow_coordinator_ != nullptr
        ? workflow_coordinator_->SnapshotTelemetry()
        : simcore::db::execution::workflow::WorkflowCoordinatorTelemetry{};
}

bool SimCoreDbRuntime::workflowCoordinatorRunning() const {
    return workflow_coordinator_ != nullptr && workflow_coordinator_->IsRunning();
}

bool SimCoreDbRuntime::buildProgramRegistry(std::string* error_out) {
    auto* execution_db = executionDb();
    auto* state_db = stateDb();
    auto* analysis_db = analysisDb();
    auto* authoring_db = authoringDb();

    if (execution_db == nullptr || state_db == nullptr || analysis_db == nullptr || authoring_db == nullptr) {
        if (error_out != nullptr) {
            *error_out = "SimCoreDB services are incomplete";
        }
        return false;
    }

    program_registry_ = simcore::db::execution::programdb::ProgramKindRegistry{};
    const auto app_dir = std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
    const auto workspace_root = app_dir / "workflow-runtime";

    simcore::db::execution::programdb::tasmovie::TasMoviePhaseRegistrationConfig tas_config{};
    tas_config.authoring_db = authoring_db;
    tas_config.working_dir_root = workspace_root / "tasmovie";
    simcore::db::execution::programdb::tasmovie::RegisterTasMoviePhaseDescriptor(
        &program_registry_,
        execution_db,
        state_db,
        analysis_db,
        std::move(tas_config));

    simcore::db::execution::programdb::seedprobe::SeedProbePhaseRegistrationConfig seed_config{};
    seed_config.authoring_db = authoring_db;
    simcore::db::execution::programdb::seedprobe::RegisterSeedProbePhaseDescriptors(
        &program_registry_,
        execution_db,
        analysis_db,
        std::move(seed_config));

    simcore::db::execution::programdb::battlecontext::BattleContextProbePhaseRegistrationConfig context_config{};
    context_config.authoring_db = authoring_db;
    context_config.working_dir_root = workspace_root / "battle-context";
    simcore::db::execution::programdb::battlecontext::RegisterBattleContextProbePhaseDescriptor(
        &program_registry_,
        execution_db,
        analysis_db,
        std::move(context_config));

    simcore::db::execution::programdb::battle::BattleSingleTurnPhaseRegistrationConfig battle_config{};
    battle_config.authoring_db = authoring_db;
    battle_config.working_dir_root = workspace_root / "battle-single-turn";
    simcore::db::execution::programdb::battle::RegisterBattleSingleTurnPhaseDescriptor(
        &program_registry_,
        execution_db,
        state_db,
        analysis_db,
        std::move(battle_config));

    if (!program_registry_.HasRequiredAdapters(static_cast<std::int32_t>(simcore::PK_TasMovie))
        || !program_registry_.HasRequiredAdaptersForStepKind("tas_movie")
        || !program_registry_.HasRequiredAdaptersForStepKind("seed_probe_chain")
        || !program_registry_.HasRequiredAdaptersForStepKind("battle_chain")
        || !program_registry_.HasRequiredAdaptersForStepKind("battle.context_probe")
        || !program_registry_.HasRequiredAdaptersForStepKind("battle.single_turn")) {
        if (error_out != nullptr) {
            *error_out = "Workflow program descriptor registration is incomplete";
        }
        return false;
    }

    return true;
}

simcore::db::execution::workflow::WorkflowCoordinatorConfig SimCoreDbRuntime::buildWorkflowConfig() {
    return simcore::db::execution::workflow::WorkflowCoordinatorConfig{
        .workflow_enabled = true,
        .strict_smoke_terminal_on_failure = false,
    };
}

} // namespace soasimqt2
