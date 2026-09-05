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
    std::optional<savor::db::execution::workflow::WorkflowStepSettlementSnapshot> GetStepSettlementSnapshotForJob(std::int64_t) const override {
        return std::nullopt;
    }
    std::vector<savor::db::execution::workflow::WorkflowStepSettlementSnapshot> ListSettlementReadyStepSnapshots(std::size_t) const override {
        return {};
    }
    std::vector<savor::db::execution::workflow::WorkflowReadyStepRecord> ListReadySteps(std::size_t) const override {
        return {};
    }
    std::int64_t CountActiveMaterializedWorkflows() const override {
        return 0;
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
    std::optional<savor::db::execution::workflow::WorkflowTransitionActivationRecord>
    GetWorkflowTransitionActivation(std::int64_t, std::string_view) const override {
        return std::nullopt;
    }
};
