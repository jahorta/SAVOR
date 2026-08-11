#pragma once

#include <optional>
#include <functional>
#include <string>
#include <vector>

#include "Cli.h"
#include "Common/DbService.h"
#include "Runner/Runtime/IProgramRuntimePort.h"
#include "ScenarioEntry.h"
#include "SplitCoordinatorRuntime.h"

namespace savor::e2e {

enum class SeedProbeInfrastructureHealth {
    Clean = 0,
    Degraded,
    Failed,
};

struct SeedProbeWorkflowValidationOptions {
    std::string graph_node_key = "probe_1";
    std::optional<std::int64_t> expected_entry_savestate_id;
    bool require_single_seedprobe_step = true;
};

bool CheckSeedProbeInvariants(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    const std::optional<
        savor::db::execution::workflow::WorkflowGraphSnapshot>& graph,
    const SplitCoordinatorTelemetry& telemetry,
    const std::vector<
        savor::runner::parallel::savordb::ReadyWorkerDispatchSnapshot>&
        ready_workers,
    const savor::runtime::ProgramModuleIdentity& expected_seed_probe_module,
    const SeedProbeWorkflowValidationOptions& options,
    SeedProbeInfrastructureHealth* health_out,
    std::vector<std::string>* health_issues_out,
    std::string* error_out);

void ReportSeedProbeTrajectory(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    const std::optional<
        savor::db::execution::workflow::WorkflowGraphSnapshot>& graph,
    const SeedProbeWorkflowValidationOptions& options,
    const std::function<void(const std::string&)>& sink);

bool RunSeedProbeRealWorkerSmoke(
    const CliOptions& options,
    const ResolvedE2eScenarioEntry& entry,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

bool RunSeedProbeWorkflowGraphRealWorkerSmoke(
    const CliOptions& options,
    const ResolvedE2eScenarioEntry& entry,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

} // namespace savor::e2e
