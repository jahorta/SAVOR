#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "Execution/Workflow/SqliteExecutionDb.h"

class NullWorkflowQueryService : public savor::db::execution::workflow::IWorkflowOrchestrationQueryService {
public:
    std::vector<savor::db::execution::workflow::WorkflowInstanceRecord> ListWorkflowInstances(
        savor::db::execution::workflow::WorkflowInstanceState,
        std::int64_t,
        std::int64_t) const override {
        return {};
    }
    std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot> GetWorkflowGraph(std::int64_t) const override {
        return std::nullopt;
    }
    std::optional<savor::db::execution::workflow::WorkflowStepTerminalSnapshot> GetStepTerminalSnapshotForJob(std::int64_t) const override {
        return std::nullopt;
    }
    std::vector<savor::db::execution::workflow::WorkflowStepTerminalSnapshot> ListTerminalReadyStepSnapshots(std::size_t) const override {
        return {};
    }
    std::vector<savor::db::execution::workflow::WorkflowReadyStepRecord> ListReadySteps(std::size_t) const override {
        return {};
    }
    std::vector<savor::db::execution::workflow::WorkflowStepRecord> ListBlockedSteps(std::int64_t) const override {
        return {};
    }
    std::vector<std::pair<std::int64_t, std::int64_t>> GetStepToJobSetMap(std::int64_t) const override {
        return {};
    }
    std::vector<savor::db::execution::workflow::WorkflowStepOutputRecord> ListStepOutputs(std::int64_t) const override {
        return {};
    }
};
