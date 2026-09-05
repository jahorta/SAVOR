#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "../ProgramDB/ProgramKindRegistry.h"
#include "WorkflowGraphRoutingService.h"
#include "WorkflowOrchestration.h"
#include "WorkflowStepSettlementGate.h"

namespace savor::db {
struct IExecutionDb;
}

namespace savor::db::execution::workflow {

struct WorkflowSettlementAdvancementResult {
    bool snapshot_found = false;
    bool gate_can_transition = false;
    bool step_marked_terminal = false;
    bool transition_evaluated = false;
    bool advanced_next_step = false;
    int spawned_step_count = 0;
    bool workflow_completed = false;
    bool workflow_failed = false;
    std::optional<std::string> blocked_reason;
    std::optional<WorkflowGraphRoutingFailure> graph_routing_failure;
};

struct WorkflowTransitionActivationRequest {
    std::int64_t workflow_instance_id = 0;
    std::int64_t source_workflow_step_id = 0;
    WorkflowTransitionActivationKind activation_kind =
        WorkflowTransitionActivationKind::Settlement;
    std::string activation_key;
    std::string trigger_fingerprint;
    std::string requested_by;
};

struct WorkflowTransitionActivationResolution {
    WorkflowTransitionActivationRecord activation;
    programdb::WorkflowTransitionDecision decision;
    bool already_applied = false;
    bool newly_frozen = false;
};

class WorkflowTransitionApplicationService {
public:
    WorkflowTransitionApplicationService(
        IWorkflowOrchestrationQueryService* query_service,
        IWorkflowOrchestrationCommandService* command_service,
        int successor_step_priority_boost = 10);

    bool ResolveActivation(
        const WorkflowTransitionActivationRequest& request,
        const std::function<programdb::WorkflowTransitionDecision()>& evaluate,
        WorkflowTransitionActivationResolution* resolution_out,
        std::string* error_out) const;

    bool MarkActivationApplied(
        const WorkflowTransitionActivationResolution& resolution,
        std::string disposition,
        std::string_view requested_by,
        std::string* error_out) const;

    void RecordActivationFailure(
        const WorkflowTransitionActivationResolution& resolution,
        std::string_view diagnostic) const;

    bool ApplyDynamicSteps(
        std::int64_t workflow_instance_id,
        std::int64_t default_parent_workflow_step_id,
        int source_priority,
        const programdb::WorkflowTransitionDecision& transition,
        std::string_view requested_by,
        int* spawned_step_count_out,
        std::string* error_out) const;

private:
    IWorkflowOrchestrationQueryService* query_service_ = nullptr;
    IWorkflowOrchestrationCommandService* command_service_ = nullptr;
    int successor_step_priority_boost_ = 10;
};

class WorkflowSettlementAdvancementService {
public:
    WorkflowSettlementAdvancementService(
        const programdb::ProgramKindRegistry* program_kind_registry,
        StepSettlementGateService* completion_gate,
        savor::db::IExecutionDb* execution_db,
        IWorkflowOrchestrationQueryService* query_service,
        IWorkflowOrchestrationCommandService* command_service,
        const WorkflowGraphRoutingService* graph_routing_service = nullptr,
        int successor_step_priority_boost = 10);
    WorkflowSettlementAdvancementService(
        const programdb::ProgramKindRegistry* program_kind_registry,
        StepSettlementGateService* completion_gate,
        IWorkflowOrchestrationQueryService* query_service,
        IWorkflowOrchestrationCommandService* command_service,
        const WorkflowGraphRoutingService* graph_routing_service = nullptr,
        int successor_step_priority_boost = 10);

    bool AdvanceForSettledJob(
        std::int64_t job_id,
        WorkflowSettlementAdvancementResult* result_out,
        std::string* error_out,
        std::optional<programdb::ProgramJobContinuationOutput> output = std::nullopt) const;

    bool AdvanceSnapshot(
        const WorkflowStepSettlementSnapshot& snapshot,
        WorkflowSettlementAdvancementResult* result_out,
        std::string* error_out,
        std::optional<programdb::ProgramJobContinuationOutput> output = std::nullopt) const;

private:
    const programdb::ProgramKindRegistry* program_kind_registry_ = nullptr;
    StepSettlementGateService* completion_gate_ = nullptr;
    savor::db::IExecutionDb* execution_db_ = nullptr;
    IWorkflowOrchestrationQueryService* query_service_ = nullptr;
    IWorkflowOrchestrationCommandService* command_service_ = nullptr;
    const WorkflowGraphRoutingService* graph_routing_service_ = nullptr;
    int successor_step_priority_boost_ = 10;
};

} // namespace savor::db::execution::workflow
