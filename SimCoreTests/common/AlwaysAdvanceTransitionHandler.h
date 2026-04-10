#pragma once

#include <optional>
#include <string>

#include "Execution/ProgramDB/ProgramKindRegistry.h"

class AlwaysAdvanceTransitionHandler final : public simcore::db::execution::programdb::IWorkflowTransitionHandler {
public:
    simcore::db::execution::programdb::WorkflowTransitionDecision EvaluateTransition(
        const simcore::db::execution::programdb::WorkflowTransitionContext&) const override {
        return {
            .should_advance = true,
            .blocked_reason = std::nullopt,
            .next_step_key = std::optional<std::string>("next"),
        };
    }
};
