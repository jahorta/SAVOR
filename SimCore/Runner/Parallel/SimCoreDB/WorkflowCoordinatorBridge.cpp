#include "WorkflowCoordinatorBridge.h"

#include <utility>

namespace simcore::runner::parallel::simcoredb {

void WorkflowCoordinatorBridge::SetMaterializationCallback(MaterializationCallback callback) {
    materialization_callback_ = std::move(callback);
}

void WorkflowCoordinatorBridge::SetTerminalCallback(TerminalCallback callback) {
    terminal_callback_ = std::move(callback);
}

void WorkflowCoordinatorBridge::SetWorkflowCreatedCallback(WorkflowCreatedCallback callback) {
    workflow_created_callback_ = std::move(callback);
}

void WorkflowCoordinatorBridge::NotifyMaterialized(std::int64_t workflow_step_id, std::int64_t job_set_id) const {
    if (!materialization_callback_) {
        return;
    }
    materialization_callback_(workflow_step_id, job_set_id);
}

bool WorkflowCoordinatorBridge::NotifyTerminal(const TerminalJobSetSignal& signal) {
    const auto dedup_key = BuildTerminalDedupKey(signal);
    if (!seen_terminal_signals_.emplace(dedup_key).second) {
        return false;
    }

    if (terminal_callback_) {
        terminal_callback_(signal);
    }
    return true;
}

bool WorkflowCoordinatorBridge::NotifyWorkflowCreated(const WorkflowCreatedSignal& signal) {
    const auto dedup_key = BuildWorkflowCreatedDedupKey(signal);
    if (!seen_workflow_created_signals_.emplace(dedup_key).second) {
        return false;
    }

    if (workflow_created_callback_) {
        workflow_created_callback_(signal);
    }
    return true;
}

std::string WorkflowCoordinatorBridge::BuildTerminalDedupKey(const TerminalJobSetSignal& signal) {
    return std::to_string(signal.workflow_instance_id)
        + ":" + std::to_string(signal.workflow_step_id)
        + ":" + std::to_string(signal.job_set_id)
        + ":" + signal.terminal_state;
}

std::string WorkflowCoordinatorBridge::BuildWorkflowCreatedDedupKey(const WorkflowCreatedSignal& signal) {
    return std::to_string(signal.workflow_instance_id);
}

} // namespace simcore::runner::parallel::simcoredb
