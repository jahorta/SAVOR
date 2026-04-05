#pragma once

#include <string>

#include "WorkflowOrchestration.h"

namespace simcore::db::execution::workflow {

class StaticWorkflowModeProvider final : public IWorkflowModeProvider {
public:
    explicit StaticWorkflowModeProvider(WorkflowModeSelection selection);
    WorkflowModeSelection GetModeSelection() const override;

private:
    WorkflowModeSelection selection_;
};

WorkflowExecutionMode ParseWorkflowExecutionMode(const std::string& value, WorkflowExecutionMode fallback);

} // namespace simcore::db::execution::workflow
