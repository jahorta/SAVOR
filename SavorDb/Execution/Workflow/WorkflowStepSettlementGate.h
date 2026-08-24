#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace savor::db::execution::workflow {

struct StepSettlementSnapshot {
    std::int64_t workflow_step_id = 0;
    std::int64_t job_set_id = 0;
    int expected_total = 0;
    int discovered_total = 0;
    int settled_total = 0;
    int succeeded_total = 0;
    int failed_total = 0;
    int interrupted_total = 0;
    int superseded_total = 0;
    int canceled_total = 0;
    int priority = 0;
};

struct StepSettlementGateDecision {
    bool can_transition = false;
    bool workflow_fail = false;
    std::optional<std::string> blocked_reason;
};

class StepSettlementGateService {
public:
    StepSettlementGateDecision Evaluate(const StepSettlementSnapshot& snapshot);

private:
    std::unordered_map<std::string, int> mismatch_attempts_;
};

} // namespace savor::db::execution::workflow
