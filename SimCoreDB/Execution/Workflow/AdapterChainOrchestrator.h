#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "../../../../SimCore/Runner/Parallel/PRTypes.h"
#include "../ProgramDB/ProgramKindDescriptor.h"
#include "../ProgramDB/ProgramKindRegistry.h"

namespace simcore::db::execution::workflow {

struct AdapterChainTrace {
    bool job_persistence_invoked = false;
    bool runtime_init_invoked = false;
    bool result_mapper_invoked = false;
    bool result_writer_invoked = false;
    bool transition_handler_invoked = false;
};

struct StepCompletionSnapshot {
    std::int64_t workflow_step_id = 0;
    std::int64_t job_set_id = 0;
    int expected_total = 0;
    int discovered_total = 0;
    int terminal_total = 0;
};

struct StepCompletionGateDecision {
    bool can_transition = false;
    bool terminal_fail = false;
    std::optional<std::string> blocked_reason;
};

class StepCompletionGateService {
public:
    StepCompletionGateDecision Evaluate(const StepCompletionSnapshot& snapshot);

private:
    std::unordered_map<std::string, int> mismatch_attempts_;
};

class AdapterChainOrchestrator {
public:
    AdapterChainOrchestrator(
        const programdb::ProgramKindRegistry* registry,
        StepCompletionGateService* completion_gate);

    std::optional<programdb::WorkflowStepScheduleResult> OnInputComplete(std::string_view step_kind, std::int64_t domain_ref_id, AdapterChainTrace* trace = nullptr) const;
    std::optional<programdb::WorkflowStepScheduleResult> OnGraphInputComplete(
        std::string_view step_kind,
        const programdb::WorkflowGraphStepScheduleContext& context,
        AdapterChainTrace* trace = nullptr) const;
    std::optional<programdb::RuntimeInitRequest> OnJobClaimed(std::string_view step_kind, std::int64_t job_id, AdapterChainTrace* trace = nullptr) const;
    std::optional<programdb::ResultMapPayload> OnJobTerminal(std::string_view step_kind, std::int64_t job_id, const std::string& result_ini, AdapterChainTrace* trace = nullptr, std::string* error_out = nullptr) const;
    std::optional<programdb::ResultMapPayload> OnJobTerminal(std::string_view step_kind, std::int64_t job_id, const simcore::PRResult& result, AdapterChainTrace* trace = nullptr, std::string* error_out = nullptr) const;

    struct StepTerminalResult {
        StepCompletionGateDecision gate;
        std::optional<programdb::WorkflowTransitionDecision> transition;
    };
    StepTerminalResult OnStepTerminal(std::string_view step_kind, const programdb::WorkflowTransitionContext& context, const StepCompletionSnapshot& snapshot, AdapterChainTrace* trace = nullptr) const;

private:
    const programdb::ProgramKindRegistry* registry_ = nullptr;
    StepCompletionGateService* completion_gate_ = nullptr;
};

} // namespace simcore::db::execution::workflow
