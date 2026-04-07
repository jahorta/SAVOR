#include "StepInputAggregationService.h"

#include <algorithm>

namespace simcore::runner::parallel::simcoredb {

namespace {

bool IsSeedProbeKind(const std::string& step_kind) {
    constexpr const char* kPrefix = "seedprobe.";
    return step_kind.rfind(kPrefix, 0) == 0;
}

} // namespace

StepInputAggregationService::StepInputAggregationService(
    StepInputAggregationConfig config,
    EmitEventFn emit_event)
    : config_(config)
    , emit_event_(std::move(emit_event)) {
}

StepInputAggregationStatus StepInputAggregationService::Evaluate(
    const WorkflowReadyStep& step,
    std::chrono::steady_clock::time_point now,
    bool auto_simulate_async_fragment) {
    StepInputAggregationStatus status;
    if (!IsSeedProbePilotStep(step)) {
        status.input_complete = true;
        status.input_latency_ms = 0;
        return status;
    }

    const auto key = ContextKey(step);
    auto [it, inserted] = contexts_.try_emplace(key);
    auto& ctx = it->second;
    if (inserted) {
        ctx.workflow_instance_id = step.workflow_instance_id;
        ctx.step_key = step.step_key;
        ctx.required_inputs = RequiredInputsFor(step);
        ctx.started_at = now;
        ctx.deadline = now + config_.timeout;
        for (const auto& source : ctx.required_inputs) {
            const auto request_id = key + ":" + source + ":retry" + std::to_string(ctx.timeout_retries);
            ctx.pending_request_ids_by_source[source] = request_id;
            if (emit_event_) {
                emit_event_(
                    step,
                    "Execution.WorkflowStepInputRequested.v1",
                    source,
                    request_id,
                    std::optional<std::string>("all-inputs-required"));
            }
        }

        const auto sync_it = ctx.pending_request_ids_by_source.find("sync");
        if (sync_it != ctx.pending_request_ids_by_source.end()) {
            (void)SubmitFragment(step, "sync", sync_it->second, now);
        }
    }

    if (auto_simulate_async_fragment && !ctx.async_fragment_simulated) {
        const auto async_it = ctx.pending_request_ids_by_source.find("async");
        if (async_it != ctx.pending_request_ids_by_source.end()) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx.started_at);
            if (age >= std::chrono::milliseconds(5)) {
                (void)SubmitFragment(step, "async", async_it->second, now);
                ctx.async_fragment_simulated = true;
            }
        }
    }

    const bool all_ready = std::all_of(
        ctx.required_inputs.begin(),
        ctx.required_inputs.end(),
        [&](const std::string& source) { return ctx.ready_sources.find(source) != ctx.ready_sources.end(); });
    if (all_ready) {
        status.input_complete = true;
        if (!ctx.input_complete_emitted && emit_event_) {
            emit_event_(
                step,
                "Execution.WorkflowStepInputComplete.v1",
                std::nullopt,
                std::nullopt,
                std::optional<std::string>("all-required-inputs-ready"));
            ctx.input_complete_emitted = true;
        }
        status.input_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx.started_at).count();
        return status;
    }

    if (now >= ctx.deadline) {
        status.timed_out = true;
        if (ctx.timeout_retries < config_.max_timeout_retries) {
            ++ctx.timeout_retries;
            ctx.deadline = now + config_.timeout;
            ctx.async_fragment_simulated = false;
            for (const auto& source : ctx.required_inputs) {
                if (ctx.ready_sources.find(source) != ctx.ready_sources.end()) {
                    continue;
                }
                const auto request_id = key + ":" + source + ":retry" + std::to_string(ctx.timeout_retries);
                ctx.pending_request_ids_by_source[source] = request_id;
                if (emit_event_) {
                    emit_event_(
                        step,
                        "Execution.WorkflowStepInputRequested.v1",
                        source,
                        request_id,
                        std::optional<std::string>("retry-on-timeout"));
                }
            }
        } else {
            ctx.terminal_failure_ready = true;
            status.terminal_failure_ready = true;
            status.input_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx.started_at).count();
        }
    }

    return status;
}

bool StepInputAggregationService::SubmitFragment(
    const WorkflowReadyStep& step,
    const std::string& source_key,
    const std::optional<std::string>& request_id,
    std::chrono::steady_clock::time_point now) {
    const auto key = ContextKey(step);
    const auto ctx_it = contexts_.find(key);
    if (ctx_it == contexts_.end()) {
        return false;
    }

    auto& ctx = ctx_it->second;
    if (ctx.ready_sources.find(source_key) != ctx.ready_sources.end()) {
        return true;
    }

    const auto pending_it = ctx.pending_request_ids_by_source.find(source_key);
    if (pending_it == ctx.pending_request_ids_by_source.end()) {
        return false;
    }

    if (request_id.has_value() && pending_it->second != request_id.value()) {
        return false;
    }

    ctx.ready_sources.insert(source_key);
    ctx.accepted_request_by_source[source_key] = pending_it->second;

    if (emit_event_) {
        emit_event_(
            step,
            "Execution.WorkflowStepInputFragmentReady.v1",
            source_key,
            pending_it->second,
            std::optional<std::string>("fragment-ready"));
    }

    (void)now;
    return true;
}

std::string StepInputAggregationService::ContextKey(const WorkflowReadyStep& step) const {
    return std::to_string(step.workflow_instance_id) + ":" + step.step_key;
}

bool StepInputAggregationService::IsSeedProbePilotStep(const WorkflowReadyStep& step) const {
    return IsSeedProbeKind(step.step_kind);
}

std::vector<std::string> StepInputAggregationService::RequiredInputsFor(const WorkflowReadyStep& step) const {
    if (IsSeedProbePilotStep(step)) {
        return { "sync", "async" };
    }
    return {};
}

} // namespace simcore::runner::parallel::simcoredb
