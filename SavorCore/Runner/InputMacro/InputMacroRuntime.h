#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "IInputMacroHost.h"

namespace savor::inputmacro {

enum class InputMacroRuntimeState : std::uint8_t {
    Idle,
    Running,
    Completed,
    Failed,
    Cancelled,
};

enum class InputMacroTerminalStatus : std::uint8_t {
    None,
    Completed,
    Failed,
    Cancelled,
};

enum class InputMacroFailure : std::uint8_t {
    None,
    NotRunning,
    EmptyPlan,
    EmptyExpectedKeys,
    InvalidAddress,
    InvalidBaselineReference,
    UndeclaredBreakpoint,
    UnauthorizedBreakpoint,
    SessionUnavailable,
    UnexpectedBreakpoint,
    MemoryReadFailed,
    MissingBaseline,
    HostFailure,
    Cancelled,
};

struct InputMacroStepResult {
    InputMacroRuntimeState state{InputMacroRuntimeState::Idle};
    InputMacroTerminalStatus terminal_status{InputMacroTerminalStatus::None};
    InputMacroFailure failure{InputMacroFailure::None};
    bool step_completed{false};

    std::size_t step_index{0};
    std::string label;
    InputMacroActionKind action_kind{InputMacroActionKind::None};

    std::vector<BPKey> expected_keys;
    BPKey expected_key{0};
    BPKey hit_key{0};
    std::uint32_t hit_pc{0};
    std::uint64_t stop_sequence{0};
    std::uint64_t input_epoch{0};
    GCInputFrame requested_input{};
    std::uint32_t input_poll_count{0};
    bool input_acknowledged{false};

    std::uint32_t memory_address{0};
    std::uint32_t memory_baseline{0};
    std::uint32_t memory_latest{0};
    bool memory_changed{false};
    std::uint32_t memory_poll_count{0};
    std::uint32_t diagnostic_cycle_index{0};

    std::string diagnostic;

    bool terminal() const noexcept
    {
        return terminal_status != InputMacroTerminalStatus::None;
    }
};

class InputMacroRuntime {
public:
    explicit InputMacroRuntime(IInputMacroHost& host);
    ~InputMacroRuntime();

    InputMacroRuntime(const InputMacroRuntime&) = delete;
    InputMacroRuntime& operator=(const InputMacroRuntime&) = delete;
    InputMacroRuntime(InputMacroRuntime&&) = delete;
    InputMacroRuntime& operator=(InputMacroRuntime&&) = delete;

    InputMacroStepResult Start(
        InputMacroPlan plan,
        std::span<const BPKey> provider_keys);
    InputMacroStepResult Replace(
        InputMacroPlan plan,
        std::span<const BPKey> provider_keys);
    InputMacroStepResult ExecuteNext();
    InputMacroStepResult Cancel();

    InputMacroRuntimeState state() const noexcept { return state_; }
    std::size_t current_index() const noexcept { return current_index_; }
    const InputMacroStep* current_step() const noexcept;
    const InputMacroAction* current_action() const noexcept;
    const InputMacroStepResult& last_result() const noexcept { return last_result_; }
    bool active() const noexcept { return state_ == InputMacroRuntimeState::Running; }

private:
    struct BaselineValue {
        std::uint32_t address{0};
        std::uint32_t value{0};
    };

    InputMacroStepResult StartValidated(
        InputMacroPlan plan,
        std::span<const BPKey> provider_keys);
    InputMacroStepResult Validate(
        const InputMacroPlan& plan,
        std::span<const BPKey> provider_keys) const;
    InputMacroStepResult FailCurrent(
        InputMacroFailure failure,
        std::string diagnostic,
        InputMacroTerminalStatus terminal_status = InputMacroTerminalStatus::Failed);
    InputMacroStepResult FailResult(
        InputMacroStepResult result,
        InputMacroFailure failure,
        std::string diagnostic,
        InputMacroTerminalStatus terminal_status = InputMacroTerminalStatus::Failed);
    InputMacroStepResult BaseResultForCurrent() const;
    void FinishSuccessfulStep(InputMacroStepResult& result);
    void CleanupOnce();
    void ResetForNewPlan();

    IInputMacroHost& host_;
    InputMacroPlan plan_;
    std::vector<BPKey> provider_keys_;
    std::size_t current_index_{0};
    std::unordered_map<InputMacroBaselineId, BaselineValue> baselines_;
    InputMacroRuntimeState state_{InputMacroRuntimeState::Idle};
    bool session_active_{false};
    bool cleanup_done_{true};
    InputMacroStepResult last_result_{};
};

} // namespace savor::inputmacro
