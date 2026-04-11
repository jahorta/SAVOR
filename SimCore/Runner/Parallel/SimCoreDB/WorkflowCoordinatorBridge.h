#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

namespace simcore::runner::parallel::simcoredb {

struct TerminalJobSetSignal {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t job_set_id = 0;
    std::string terminal_state;
};

struct WorkflowCreatedSignal {
    std::int64_t workflow_instance_id = 0;
};

class WorkflowCoordinatorBridge {
public:
    using MaterializationCallback = std::function<void(std::int64_t workflow_step_id, std::int64_t job_set_id)>;
    using TerminalCallback = std::function<void(const TerminalJobSetSignal& signal)>;
    using WorkflowCreatedCallback = std::function<void(const WorkflowCreatedSignal& signal)>;

    void SetMaterializationCallback(MaterializationCallback callback);
    void SetTerminalCallback(TerminalCallback callback);
    void SetWorkflowCreatedCallback(WorkflowCreatedCallback callback);

    void NotifyMaterialized(std::int64_t workflow_step_id, std::int64_t job_set_id) const;
    bool NotifyTerminal(const TerminalJobSetSignal& signal);
    bool NotifyWorkflowCreated(const WorkflowCreatedSignal& signal);

private:
    static std::string BuildTerminalDedupKey(const TerminalJobSetSignal& signal);
    static std::string BuildWorkflowCreatedDedupKey(const WorkflowCreatedSignal& signal);

    MaterializationCallback materialization_callback_;
    TerminalCallback terminal_callback_;
    WorkflowCreatedCallback workflow_created_callback_;
    std::unordered_set<std::string> seen_terminal_signals_;
    std::unordered_set<std::string> seen_workflow_created_signals_;
};

} // namespace simcore::runner::parallel::simcoredb
