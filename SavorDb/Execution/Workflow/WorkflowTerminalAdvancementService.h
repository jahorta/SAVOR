#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "../ProgramDB/ProgramKindRegistry.h"
#include "WorkflowGraphRoutingService.h"
#include "WorkflowOrchestration.h"
#include "WorkflowStepCompletionGate.h"

namespace savor::db {
struct IExecutionDb;
}

namespace savor::db::execution::workflow {

struct WorkflowTerminalAdvancementResult {
    bool snapshot_found = false;
    bool gate_can_transition = false;
    bool step_marked_terminal = false;
    bool transition_evaluated = false;
    bool advanced_next_step = false;
    int spawned_step_count = 0;
    bool workflow_completed = false;
    bool workflow_failed = false;
    std::optional<std::string> blocked_reason;
};

class WorkflowTerminalAdvancementService {
public:
    WorkflowTerminalAdvancementService(
        const programdb::ProgramKindRegistry* program_kind_registry,
        StepCompletionGateService* completion_gate,
        savor::db::IExecutionDb* execution_db,
        IWorkflowOrchestrationQueryService* query_service,
        IWorkflowOrchestrationCommandService* command_service,
        const WorkflowGraphRoutingService* graph_routing_service = nullptr,
        int successor_step_priority_boost = 10);
    WorkflowTerminalAdvancementService(
        const programdb::ProgramKindRegistry* program_kind_registry,
        StepCompletionGateService* completion_gate,
        IWorkflowOrchestrationQueryService* query_service,
        IWorkflowOrchestrationCommandService* command_service,
        const WorkflowGraphRoutingService* graph_routing_service = nullptr,
        int successor_step_priority_boost = 10);

    bool AdvanceForTerminalJob(
        std::int64_t job_id,
        WorkflowTerminalAdvancementResult* result_out,
        std::string* error_out,
        std::optional<programdb::ProgramJobContinuationOutput> output = std::nullopt) const;

    bool AdvanceSnapshot(
        const WorkflowStepTerminalSnapshot& snapshot,
        WorkflowTerminalAdvancementResult* result_out,
        std::string* error_out,
        std::optional<programdb::ProgramJobContinuationOutput> output = std::nullopt) const;

private:
    const programdb::ProgramKindRegistry* program_kind_registry_ = nullptr;
    StepCompletionGateService* completion_gate_ = nullptr;
    savor::db::IExecutionDb* execution_db_ = nullptr;
    IWorkflowOrchestrationQueryService* query_service_ = nullptr;
    IWorkflowOrchestrationCommandService* command_service_ = nullptr;
    const WorkflowGraphRoutingService* graph_routing_service_ = nullptr;
    int successor_step_priority_boost_ = 10;
};

} // namespace savor::db::execution::workflow
