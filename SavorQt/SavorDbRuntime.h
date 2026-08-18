#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "Common/DbService.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"

namespace savorqt {

// Owns durable database access and the immutable production descriptor
// registry only. Workflow materialization, result processing, workers, and job
// dispatch are authorized exclusively by CoordinatorRuntime.
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

private:
    SavorDbRuntime() = default;

    bool buildProgramRegistry(std::string* error_out);

    std::unique_ptr<savor::db::core::DBService> service_;
    savor::db::execution::programdb::ProgramKindRegistry program_registry_;
    std::filesystem::path root_;
};

} // namespace savorqt
