#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "WorkflowSchedulerAdapter.h"

namespace savor::runner::parallel::savordb {

struct StepInputAggregationConfig {
    std::chrono::milliseconds timeout{ 2000 };
    int max_timeout_retries = 1;
};

struct StepInputAggregationStatus {
    bool input_complete = false;
    bool terminal_failure_ready = false;
    bool timed_out = false;
    std::optional<std::int64_t> input_latency_ms;
};

class StepInputAggregationService {
public:
    using EmitEventFn = std::function<void(
        const WorkflowReadyStep& step,
        const std::string& event_kind,
        const std::optional<std::string>& source_key,
        const std::optional<std::string>& request_id,
        const std::optional<std::string>& message)>;

    explicit StepInputAggregationService(
        StepInputAggregationConfig config = {},
        EmitEventFn emit_event = {});

    StepInputAggregationStatus Evaluate(
        const WorkflowReadyStep& step,
        std::chrono::steady_clock::time_point now,
        bool auto_simulate_async_fragment = true);

    bool SubmitFragment(
        const WorkflowReadyStep& step,
        const std::string& source_key,
        const std::optional<std::string>& request_id,
        std::chrono::steady_clock::time_point now);

private:
    struct StepAssemblyContext {
        std::int64_t workflow_instance_id = 0;
        std::string step_key;
        std::vector<std::string> required_inputs;
        std::unordered_map<std::string, std::string> pending_request_ids_by_source;
        std::unordered_map<std::string, std::string> accepted_request_by_source;
        std::unordered_set<std::string> ready_sources;
        std::chrono::steady_clock::time_point started_at{};
        std::chrono::steady_clock::time_point deadline{};
        int timeout_retries = 0;
        bool input_complete_emitted = false;
        bool terminal_failure_ready = false;
        bool async_fragment_simulated = false;
    };

    std::string ContextKey(const WorkflowReadyStep& step) const;
    bool IsSeedProbePilotStep(const WorkflowReadyStep& step) const;
    std::vector<std::string> RequiredInputsFor(const WorkflowReadyStep& step) const;

    StepInputAggregationConfig config_{};
    EmitEventFn emit_event_;
    std::unordered_map<std::string, StepAssemblyContext> contexts_;
};

} // namespace savor::runner::parallel::savordb
