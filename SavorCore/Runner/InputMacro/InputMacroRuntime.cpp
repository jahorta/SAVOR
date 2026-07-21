#include "InputMacroRuntime.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace savor::inputmacro {

namespace {

bool IsValidU32Address(std::uint32_t address)
{
    return address != 0 && (address & (alignof(std::uint32_t) - 1u)) == 0;
}

bool ContainsKey(std::span<const BPKey> keys, BPKey key)
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

} // namespace

InputMacroRuntime::InputMacroRuntime(IInputMacroHost& host)
    : host_(host)
{
}

InputMacroRuntime::~InputMacroRuntime()
{
    CleanupOnce();
}

const InputMacroStep* InputMacroRuntime::current_step() const noexcept
{
    if (current_index_ >= plan_.steps.size()) return nullptr;
    return &plan_.steps[current_index_];
}

const InputMacroAction* InputMacroRuntime::current_action() const noexcept
{
    const auto* step = current_step();
    return step != nullptr ? &step->action : nullptr;
}

InputMacroStepResult InputMacroRuntime::Start(
    InputMacroPlan plan,
    std::span<const BPKey> provider_keys)
{
    if (state_ == InputMacroRuntimeState::Running) {
        CleanupOnce();
    }
    return StartValidated(std::move(plan), provider_keys);
}

InputMacroStepResult InputMacroRuntime::Replace(
    InputMacroPlan plan,
    std::span<const BPKey> provider_keys)
{
    CleanupOnce();
    return StartValidated(std::move(plan), provider_keys);
}

InputMacroStepResult InputMacroRuntime::StartValidated(
    InputMacroPlan plan,
    std::span<const BPKey> provider_keys)
{
    ResetForNewPlan();
    // Every start attempt owns one cleanup generation, including validation
    // and acquisition failures. Cleanup remains safe before a session exists.
    cleanup_done_ = false;

    auto validation = Validate(plan, provider_keys);
    if (validation.failure != InputMacroFailure::None) {
        state_ = InputMacroRuntimeState::Failed;
        validation.state = state_;
        validation.terminal_status = InputMacroTerminalStatus::Failed;
        last_result_ = std::move(validation);
        CleanupOnce();
        return last_result_;
    }

    plan_ = std::move(plan);
    provider_keys_.assign(provider_keys.begin(), provider_keys.end());
    if (!host_.acquire_exclusive_session(provider_keys_)) {
        state_ = InputMacroRuntimeState::Failed;
        last_result_ = InputMacroStepResult{
            .state = state_,
            .terminal_status = InputMacroTerminalStatus::Failed,
            .failure = InputMacroFailure::SessionUnavailable,
            .diagnostic = "input macro host refused the exclusive session",
        };
        CleanupOnce();
        return last_result_;
    }

    session_active_ = true;
    state_ = InputMacroRuntimeState::Running;
    last_result_ = InputMacroStepResult{
        .state = state_,
        .step_index = 0,
    };
    return last_result_;
}

InputMacroStepResult InputMacroRuntime::Validate(
    const InputMacroPlan& plan,
    std::span<const BPKey> provider_keys) const
{
    if (plan.steps.empty()) {
        return InputMacroStepResult{
            .state = InputMacroRuntimeState::Failed,
            .terminal_status = InputMacroTerminalStatus::Failed,
            .failure = InputMacroFailure::EmptyPlan,
            .diagnostic = "input macro plan is empty",
        };
    }

    for (const BPKey key : provider_keys) {
        if (!bp::BpRegistry::IsAllowed(key, BreakpointConsumer::InputMacroControl)) {
            return InputMacroStepResult{
                .state = InputMacroRuntimeState::Failed,
                .terminal_status = InputMacroTerminalStatus::Failed,
                .failure = InputMacroFailure::UnauthorizedBreakpoint,
                .expected_key = key,
                .diagnostic = "provider declares a breakpoint unavailable to input macro control",
            };
        }
    }

    std::unordered_map<InputMacroBaselineId, std::uint32_t> prior_captures;
    for (std::size_t index = 0; index < plan.steps.size(); ++index) {
        const auto& step = plan.steps[index];
        InputMacroStepResult invalid{
            .state = InputMacroRuntimeState::Failed,
            .terminal_status = InputMacroTerminalStatus::Failed,
            .step_index = index,
            .label = step.label,
            .action_kind = ActionKind(step.action),
        };

        if (const auto* wait = std::get_if<BreakpointWaitAction>(&step.action)) {
            invalid.expected_keys = wait->expected_keys;
            invalid.expected_key = wait->expected_keys.empty() ? 0 : wait->expected_keys.front();
            if (wait->expected_keys.empty()) {
                invalid.failure = InputMacroFailure::EmptyExpectedKeys;
                invalid.diagnostic = "breakpoint-wait action has no expected keys";
                return invalid;
            }
            for (const BPKey key : wait->expected_keys) {
                invalid.expected_key = key;
                if (!ContainsKey(provider_keys, key)) {
                    invalid.failure = InputMacroFailure::UndeclaredBreakpoint;
                    invalid.diagnostic = "breakpoint-wait action uses a key not declared by its provider";
                    return invalid;
                }
                if (!bp::BpRegistry::IsAllowed(key, BreakpointConsumer::InputMacroControl)) {
                    invalid.failure = InputMacroFailure::UnauthorizedBreakpoint;
                    invalid.diagnostic = "breakpoint-wait action uses a key unavailable to input macro control";
                    return invalid;
                }
            }
            continue;
        }

        if (std::holds_alternative<NeutralFramesAction>(step.action)) {
            continue;
        }

        if (const auto* capture = std::get_if<CaptureU32BaselineAction>(&step.action)) {
            invalid.memory_address = capture->address;
            if (!IsValidU32Address(capture->address)) {
                invalid.failure = InputMacroFailure::InvalidAddress;
                invalid.diagnostic = "baseline capture has an invalid u32 address";
                return invalid;
            }
            if (capture->baseline_id.empty()
                || prior_captures.contains(capture->baseline_id)) {
                invalid.failure = InputMacroFailure::InvalidBaselineReference;
                invalid.diagnostic = capture->baseline_id.empty()
                    ? "baseline capture has an empty identifier"
                    : "baseline identifier is captured more than once";
                return invalid;
            }
            prior_captures.emplace(capture->baseline_id, capture->address);
            continue;
        }

        const auto& change = std::get<WaitU32ChangeAction>(step.action);
        invalid.memory_address = change.address;
        invalid.diagnostic_cycle_index = change.diagnostic_cycle_index;
        if (!IsValidU32Address(change.address)) {
            invalid.failure = InputMacroFailure::InvalidAddress;
            invalid.diagnostic = "memory-change wait has an invalid u32 address";
            return invalid;
        }
        if (change.timeout_ms == 0) {
            invalid.failure = InputMacroFailure::InvalidTimeout;
            invalid.diagnostic = "memory-change wait has a zero timeout";
            return invalid;
        }
        const auto baseline = prior_captures.find(change.baseline_id);
        if (change.baseline_id.empty()
            || baseline == prior_captures.end()
            || baseline->second != change.address) {
            invalid.failure = InputMacroFailure::InvalidBaselineReference;
            invalid.diagnostic = "memory-change wait does not reference a prior capture at the same address";
            return invalid;
        }
    }

    return InputMacroStepResult{};
}

InputMacroStepResult InputMacroRuntime::BaseResultForCurrent() const
{
    InputMacroStepResult result{
        .state = state_,
        .step_index = current_index_,
    };
    if (const auto* step = current_step()) {
        result.label = step->label;
        result.action_kind = ActionKind(step->action);
        if (const auto* wait = std::get_if<BreakpointWaitAction>(&step->action)) {
            result.expected_keys = wait->expected_keys;
            result.expected_key = wait->expected_keys.empty() ? 0 : wait->expected_keys.front();
            result.requested_input = wait->input;
        } else if (const auto* capture = std::get_if<CaptureU32BaselineAction>(&step->action)) {
            result.memory_address = capture->address;
        } else if (const auto* change = std::get_if<WaitU32ChangeAction>(&step->action)) {
            result.memory_address = change->address;
            result.diagnostic_cycle_index = change->diagnostic_cycle_index;
        }
    }
    return result;
}

InputMacroStepResult InputMacroRuntime::FailCurrent(
    InputMacroFailure failure,
    std::string diagnostic,
    InputMacroTerminalStatus terminal_status)
{
    return FailResult(BaseResultForCurrent(), failure, std::move(diagnostic), terminal_status);
}

InputMacroStepResult InputMacroRuntime::FailResult(
    InputMacroStepResult result,
    InputMacroFailure failure,
    std::string diagnostic,
    InputMacroTerminalStatus terminal_status)
{
    result.failure = failure;
    result.diagnostic = std::move(diagnostic);
    result.terminal_status = terminal_status;
    state_ = terminal_status == InputMacroTerminalStatus::Cancelled
        ? InputMacroRuntimeState::Cancelled
        : InputMacroRuntimeState::Failed;
    result.state = state_;
    CleanupOnce();
    last_result_ = std::move(result);
    return last_result_;
}

void InputMacroRuntime::FinishSuccessfulStep(InputMacroStepResult& result)
{
    result.step_completed = true;
    ++current_index_;
    if (current_index_ == plan_.steps.size()) {
        state_ = InputMacroRuntimeState::Completed;
        result.state = state_;
        result.terminal_status = InputMacroTerminalStatus::Completed;
        CleanupOnce();
    } else {
        result.state = InputMacroRuntimeState::Running;
    }
}

InputMacroStepResult InputMacroRuntime::ExecuteNext()
{
    if (state_ != InputMacroRuntimeState::Running || current_step() == nullptr) {
        InputMacroStepResult result{
            .state = state_,
            .failure = InputMacroFailure::NotRunning,
            .step_index = current_index_,
            .diagnostic = "input macro runtime is not running",
        };
        if (state_ == InputMacroRuntimeState::Completed) {
            result.terminal_status = InputMacroTerminalStatus::Completed;
        } else if (state_ == InputMacroRuntimeState::Cancelled) {
            result.terminal_status = InputMacroTerminalStatus::Cancelled;
        } else if (state_ == InputMacroRuntimeState::Failed) {
            result.terminal_status = InputMacroTerminalStatus::Failed;
        }
        last_result_ = std::move(result);
        return last_result_;
    }

    auto result = BaseResultForCurrent();
    const auto& action = current_step()->action;

    if (const auto* wait = std::get_if<BreakpointWaitAction>(&action)) {
        const auto host_result = host_.run_to_breakpoints(*wait);
        result.hit_key = host_result.hit_key;
        result.hit_pc = host_result.hit_pc;
        result.stop_sequence = host_result.stop_sequence;
        result.input_epoch = host_result.input_epoch;
        // The plan is authoritative for the requested frame. Host telemetry
        // describes whether that request was observed, not a replacement for
        // the request itself.
        result.requested_input = wait->input;
        result.input_poll_count = host_result.input_poll_count;
        result.input_acknowledged = host_result.input_acknowledged;
        result.elapsed_ms = host_result.elapsed_ms;
        if (host_result.status == InputMacroHostStatus::Cancelled) {
            return FailResult(std::move(result), InputMacroFailure::Cancelled, "breakpoint wait was cancelled",
                InputMacroTerminalStatus::Cancelled);
        }
        if (host_result.status == InputMacroHostStatus::TimedOut) {
            return FailResult(std::move(result), InputMacroFailure::BreakpointTimeout, "breakpoint wait timed out");
        }
        if (host_result.status != InputMacroHostStatus::Succeeded || !host_result.hit) {
            return FailResult(std::move(result), InputMacroFailure::HostFailure, "breakpoint wait failed in the host");
        }
        if (std::find(wait->expected_keys.begin(), wait->expected_keys.end(), host_result.hit_key)
            == wait->expected_keys.end()) {
            return FailResult(
                std::move(result),
                InputMacroFailure::UnexpectedBreakpoint,
                "breakpoint wait stopped at an unexpected key");
        }
        FinishSuccessfulStep(result);
        last_result_ = std::move(result);
        return last_result_;
    }

    if (const auto* neutral = std::get_if<NeutralFramesAction>(&action)) {
        const auto host_status = host_.step_neutral_frames(neutral->frame_count);
        if (host_status == InputMacroHostStatus::Cancelled) {
            return FailResult(std::move(result), InputMacroFailure::Cancelled, "neutral-frame stepping was cancelled",
                InputMacroTerminalStatus::Cancelled);
        }
        if (host_status != InputMacroHostStatus::Succeeded) {
            return FailResult(std::move(result), InputMacroFailure::HostFailure, "neutral-frame stepping failed in the host");
        }
        FinishSuccessfulStep(result);
        last_result_ = std::move(result);
        return last_result_;
    }

    if (const auto* capture = std::get_if<CaptureU32BaselineAction>(&action)) {
        std::uint32_t value = 0;
        if (!host_.read_u32(capture->address, value)) {
            return FailResult(std::move(result), InputMacroFailure::MemoryReadFailed, "baseline memory read failed");
        }
        baselines_[capture->baseline_id] = BaselineValue{capture->address, value};
        result.memory_address = capture->address;
        result.memory_baseline = value;
        result.memory_latest = value;
        FinishSuccessfulStep(result);
        last_result_ = std::move(result);
        return last_result_;
    }

    const auto& change = std::get<WaitU32ChangeAction>(action);
    const auto baseline = baselines_.find(change.baseline_id);
    if (baseline == baselines_.end() || baseline->second.address != change.address) {
        return FailResult(std::move(result), InputMacroFailure::MissingBaseline,
            "memory-change wait has no captured runtime baseline");
    }

    result.memory_baseline = baseline->second.value;
    const auto host_result = host_.wait_for_u32_change(
        change.address,
        baseline->second.value,
        change.timeout_ms);
    result.memory_latest = host_result.latest_value;
    result.memory_changed = host_result.status == InputMacroHostStatus::Succeeded
        && host_result.latest_value != baseline->second.value;
    result.memory_poll_count = host_result.poll_count;
    result.elapsed_ms = host_result.elapsed_ms;
    if (host_result.status == InputMacroHostStatus::Cancelled) {
        return FailResult(std::move(result), InputMacroFailure::Cancelled, "memory-change wait was cancelled",
            InputMacroTerminalStatus::Cancelled);
    }
    if (host_result.status == InputMacroHostStatus::ReadFailed) {
        return FailResult(std::move(result), InputMacroFailure::MemoryReadFailed,
            "memory-change wait encountered a read failure");
    }
    if (host_result.status == InputMacroHostStatus::TimedOut) {
        return FailResult(std::move(result), InputMacroFailure::MemoryTimeout,
            "memory-change wait timed out");
    }
    if (host_result.status != InputMacroHostStatus::Succeeded || !result.memory_changed) {
        return FailResult(std::move(result), InputMacroFailure::HostFailure,
            "memory-change host returned success without observing a change");
    }
    FinishSuccessfulStep(result);
    last_result_ = std::move(result);
    return last_result_;
}

InputMacroStepResult InputMacroRuntime::Cancel()
{
    if (state_ != InputMacroRuntimeState::Running) {
        last_result_ = InputMacroStepResult{
            .state = state_,
            .failure = InputMacroFailure::NotRunning,
            .step_index = current_index_,
            .diagnostic = "input macro runtime is not running",
        };
        return last_result_;
    }
    return FailCurrent(InputMacroFailure::Cancelled, "input macro runtime was cancelled",
        InputMacroTerminalStatus::Cancelled);
}

void InputMacroRuntime::CleanupOnce()
{
    if (cleanup_done_) return;
    cleanup_done_ = true;

    host_.set_neutral_input();
    host_.clear_macro_memory_watchpoints();
    if (session_active_) {
        host_.release_exclusive_session();
        session_active_ = false;
    }
    host_.restore_breakpoint_state();
}

void InputMacroRuntime::ResetForNewPlan()
{
    plan_ = {};
    provider_keys_.clear();
    current_index_ = 0;
    baselines_.clear();
    state_ = InputMacroRuntimeState::Idle;
    session_active_ = false;
    cleanup_done_ = true;
    last_result_ = {};
}

} // namespace savor::inputmacro
