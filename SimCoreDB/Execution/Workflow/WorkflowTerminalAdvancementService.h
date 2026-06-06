#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "AdapterChainOrchestrator.h"
#include "WorkflowOrchestration.h"

namespace simcore::db::execution::workflow {

struct WorkflowTerminalAdvancementResult {
    bool snapshot_found = false;
    bool gate_can_transition = false;
    bool step_marked_terminal = false;
    bool transition_evaluated = false;
    bool advanced_next_step = false;
    int spawned_step_count = 0;
    bool workflow_completed = false;
    std::optional<std::string> blocked_reason;
};

class WorkflowTerminalAdvancementService {
public:
    WorkflowTerminalAdvancementService(
        const AdapterChainOrchestrator* orchestrator,
        IWorkflowOrchestrationQueryService* query_service,
        IWorkflowOrchestrationCommandService* command_service);

    bool AdvanceForTerminalJob(
        std::int64_t job_id,
        WorkflowTerminalAdvancementResult* result_out,
        std::string* error_out,
        std::optional<programdb::ResultMapPayload> result_payload = std::nullopt) const;

    bool AdvanceSnapshot(
        const WorkflowStepTerminalSnapshot& snapshot,
        WorkflowTerminalAdvancementResult* result_out,
        std::string* error_out,
        std::optional<programdb::ResultMapPayload> result_payload = std::nullopt) const;

private:
    const AdapterChainOrchestrator* orchestrator_ = nullptr;
    IWorkflowOrchestrationQueryService* query_service_ = nullptr;
    IWorkflowOrchestrationCommandService* command_service_ = nullptr;
};

} // namespace simcore::db::execution::workflow
