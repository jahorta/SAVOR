#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace savor::db::execution::workflow {

struct StepCompletionSnapshot {
    std::int64_t workflow_step_id = 0;
    std::int64_t job_set_id = 0;
    int expected_total = 0;
    int discovered_total = 0;
    int terminal_total = 0;
    int failed_total = 0;
    int priority = 0;
};

struct StepCompletionGateDecision {
    bool can_transition = false;
    bool terminal_fail = false;
    std::optional<std::string> blocked_reason;
};

class StepCompletionGateService {
public:
    StepCompletionGateDecision Evaluate(const StepCompletionSnapshot& snapshot);

private:
    std::unordered_map<std::string, int> mismatch_attempts_;
};

} // namespace savor::db::execution::workflow
