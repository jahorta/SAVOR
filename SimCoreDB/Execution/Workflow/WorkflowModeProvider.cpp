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
    if (value == "LegacyOnly") return WorkflowExecutionMode::LegacyOnly;
    if (value == "DualWriteObserve") return WorkflowExecutionMode::DualWriteObserve;
    if (value == "WorkflowPrimary") return WorkflowExecutionMode::WorkflowPrimary;
    if (value == "WorkflowOnly") return WorkflowExecutionMode::WorkflowOnly;
    return fallback;
}

WorkflowAuthorityPolicy BuildWorkflowAuthorityPolicy(WorkflowExecutionMode mode) {
    switch (mode) {
    case WorkflowExecutionMode::LegacyOnly:
        return WorkflowAuthorityPolicy{
            .run_legacy = true,
            .run_workflow = false,
            .legacy_authoritative = true,
            .workflow_authoritative = false,
        };
    case WorkflowExecutionMode::DualWriteObserve:
        return WorkflowAuthorityPolicy{
            .run_legacy = true,
            .run_workflow = true,
            .legacy_authoritative = true,
            .workflow_authoritative = false,
        };
    case WorkflowExecutionMode::WorkflowPrimary:
        return WorkflowAuthorityPolicy{
            .run_legacy = true,
            .run_workflow = true,
            .legacy_authoritative = false,
            .workflow_authoritative = true,
        };
    case WorkflowExecutionMode::WorkflowOnly:
        return WorkflowAuthorityPolicy{
            .run_legacy = false,
            .run_workflow = true,
            .legacy_authoritative = false,
            .workflow_authoritative = true,
        };
    }

    return WorkflowAuthorityPolicy{};
}

} // namespace simcore::db::execution::workflow
