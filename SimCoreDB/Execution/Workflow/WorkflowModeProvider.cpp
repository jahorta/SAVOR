#include "WorkflowModeProvider.h"

#include <utility>

namespace simcore::db::execution::workflow {

StaticWorkflowModeProvider::StaticWorkflowModeProvider(WorkflowModeSelection selection)
    : selection_(std::move(selection)) {
}

WorkflowModeSelection StaticWorkflowModeProvider::GetModeSelection() const {
    return selection_;
}

WorkflowExecutionMode ParseWorkflowExecutionMode(const std::string& value, WorkflowExecutionMode fallback) {
    if (value == "Workflow") return WorkflowExecutionMode::Workflow;
    return fallback;
}

WorkflowAuthorityPolicy BuildWorkflowAuthorityPolicy(WorkflowExecutionMode mode) {
    switch (mode) {
    case WorkflowExecutionMode::Workflow:
        return WorkflowAuthorityPolicy{
            .run_workflow = true,
        };
    }

    return WorkflowAuthorityPolicy{
        .run_workflow = true,
    };
}

} // namespace simcore::db::execution::workflow
