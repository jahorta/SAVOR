#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Execution/CoordinatorRuntime.h"

namespace savor::db {
struct IExecutionDb;
namespace core {
class DBService;
}
}

namespace savor::e2e {

struct ScenarioAssessment {
    std::vector<std::string> invariant_failures;
    std::vector<std::string> warnings;

    void Require(bool condition, std::string message);
    void Warn(std::string message);

    [[nodiscard]] bool Passed() const noexcept {
        return invariant_failures.empty();
    }

    [[nodiscard]] std::string FailureSummary(
        std::string_view scenario) const;
};

void AssessCommonScenarioExecution(
    savor::db::IExecutionDb* execution_db,
    std::span<const savor::db::execution::workflow::WorkflowGraphSnapshot>
        workflows,
    const savor::runner::parallel::savordb::CoordinatorRuntimeTelemetry& telemetry,
    std::span<const savor::runner::parallel::savordb::
        JobExecutionWorkerDispatchSnapshot> ready_workers,
    ScenarioAssessment* assessment);

void EmitScenarioAssessment(
    std::string_view scenario,
    const ScenarioAssessment& assessment,
    const std::function<void(const std::string&)>& sink);

void ReportCommonScenarioTrajectory(
    savor::db::core::DBService* db_service,
    std::span<const savor::db::execution::workflow::WorkflowGraphSnapshot>
        workflows,
    std::string_view scenario,
    const std::function<void(const std::string&)>& sink,
    ScenarioAssessment* assessment = nullptr);

} // namespace savor::e2e
