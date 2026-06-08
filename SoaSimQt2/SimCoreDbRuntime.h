#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "Common/DbService.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/Workflow/WorkflowCoordinatorService.h"

namespace soasimqt2 {

class SimCoreDbRuntime final {
public:
    static SimCoreDbRuntime& instance();

    bool start(const std::filesystem::path& root, std::string* error_out = nullptr);
    void stop();
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] std::filesystem::path root() const;

    bool switchRoot(const std::filesystem::path& root, std::string* error_out = nullptr);
    bool resetRoot(std::string* error_out = nullptr);
    bool relocateRoot(const std::filesystem::path& root, bool cleanup_source, std::string* error_out = nullptr);

    simcore::db::core::DBService* service();
    simcore::db::IUiReadDb* uiReadDb();
    simcore::db::IStateDb* stateDb();
    simcore::db::IAnalysisDb* analysisDb();
    simcore::db::IAuthoringDb* authoringDb();
    simcore::db::IExecutionDb* executionDb();
    simcore::db::execution::workflow::IWorkflowOrchestrationQueryService* workflowQueryService();
    simcore::db::execution::workflow::IWorkflowOrchestrationCommandService* workflowCommandService();
    simcore::db::execution::programdb::ProgramKindRegistry* programKindRegistry();
    simcore::db::execution::workflow::WorkflowCoordinatorTelemetry workflowCoordinatorTelemetry() const;
    bool workflowCoordinatorRunning() const;

private:
    SimCoreDbRuntime() = default;

    bool buildProgramRegistry(std::string* error_out);
    static simcore::db::execution::workflow::WorkflowCoordinatorConfig buildWorkflowConfig();

    std::unique_ptr<simcore::db::core::DBService> service_;
    simcore::db::execution::programdb::ProgramKindRegistry program_registry_;
    std::unique_ptr<simcore::db::execution::workflow::WorkflowCoordinatorService> workflow_coordinator_;
    std::filesystem::path root_;
};

} // namespace soasimqt2
