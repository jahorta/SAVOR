#include "Execution/Workflow/WorkflowModeProvider.h"

#include <utility>

namespace simcore::db::execution::workflow {

StaticWorkflowModeProvider::StaticWorkflowModeProvider(WorkflowModeSelection selection)
    : selection_(std::move(selection)) {
}

WorkflowModeSelection StaticWorkflowModeProvider::GetModeSelection() const {
    return selection_;
}

WorkflowExecutionMode ParseWorkflowExecutionMode(const std::string& value, WorkflowExecutionMode fallback) {
    if (value == "LegacyOnly") return WorkflowExecutionMode::LegacyOnly;
    if (value == "DualWriteObserve") return WorkflowExecutionMode::DualWriteObserve;
    if (value == "WorkflowPrimary") return WorkflowExecutionMode::WorkflowPrimary;
    if (value == "WorkflowOnly") return WorkflowExecutionMode::WorkflowOnly;
    return fallback;
}

} // namespace simcore::db::execution::workflow
