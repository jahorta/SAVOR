#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "Execution/Workflow/SqliteExecutionDb.h"

class NullWorkflowQueryService final : public simcore::db::execution::workflow::IWorkflowOrchestrationQueryService {
public:
    std::vector<simcore::db::execution::workflow::WorkflowInstanceRecord> ListWorkflowInstances(
        simcore::db::execution::workflow::WorkflowInstanceState,
        std::int64_t,
        std::int64_t) const override {
        return {};
    }
    std::optional<simcore::db::execution::workflow::WorkflowGraphSnapshot> GetWorkflowGraph(std::int64_t) const override {
        return std::nullopt;
    }
    std::vector<simcore::db::execution::workflow::WorkflowReadyStepRecord> ListReadySteps(std::size_t) const override {
        return {};
    }
    std::vector<simcore::db::execution::workflow::WorkflowStepRecord> ListBlockedSteps(std::int64_t) const override {
        return {};
    }
    std::vector<std::pair<std::int64_t, std::int64_t>> GetStepToJobSetMap(std::int64_t) const override {
        return {};
    }
};
