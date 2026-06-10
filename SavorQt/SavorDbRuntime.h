#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "Common/DbService.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/Workflow/WorkflowCoordinatorService.h"

namespace savorqt {

class SavorDbRuntime final {
public:
    static SavorDbRuntime& instance();

    bool start(const std::filesystem::path& root, std::string* error_out = nullptr);
    void stop();
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] std::filesystem::path root() const;

    bool switchRoot(const std::filesystem::path& root, std::string* error_out = nullptr);
    bool resetRoot(std::string* error_out = nullptr);
    bool relocateRoot(const std::filesystem::path& root, bool cleanup_source, std::string* error_out = nullptr);

    savor::db::core::DBService* service();
    savor::db::IUiReadDb* uiReadDb();
    savor::db::IStateDb* stateDb();
    savor::db::IAnalysisDb* analysisDb();
    savor::db::IAuthoringDb* authoringDb();
    savor::db::IExecutionDb* executionDb();
    savor::db::execution::workflow::IWorkflowOrchestrationQueryService* workflowQueryService();
    savor::db::execution::workflow::IWorkflowOrchestrationCommandService* workflowCommandService();
    savor::db::execution::programdb::ProgramKindRegistry* programKindRegistry();
    savor::db::execution::workflow::WorkflowCoordinatorTelemetry workflowCoordinatorTelemetry() const;
    bool workflowCoordinatorRunning() const;

private:
    SavorDbRuntime() = default;

    bool buildProgramRegistry(std::string* error_out);
    static savor::db::execution::workflow::WorkflowCoordinatorConfig buildWorkflowConfig();

    std::unique_ptr<savor::db::core::DBService> service_;
    savor::db::execution::programdb::ProgramKindRegistry program_registry_;
    std::unique_ptr<savor::db::execution::workflow::WorkflowCoordinatorService> workflow_coordinator_;
    std::filesystem::path root_;
};

} // namespace savorqt
