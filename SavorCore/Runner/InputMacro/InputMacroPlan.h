#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "../../Core/Input/InputPlan.h"
#include "../Breakpoints/BpRegistry.h"

namespace savor::inputmacro {

using InputMacroBaselineId = std::string;

struct BreakpointWaitAction {
    std::vector<BPKey> expected_keys;
    GCInputFrame input{};
    bool hold_input_through_hit_opcode{false};
};

struct NeutralFramesAction {
    std::uint32_t frame_count{0};
};

struct CaptureU32BaselineAction {
    InputMacroBaselineId baseline_id;
    std::uint32_t address{0};
};

struct WaitU32ChangeAction {
    InputMacroBaselineId baseline_id;
    std::uint32_t address{0};
    // Compatibility tag used by providers to route per-gate diagnostics. The
    // generic runtime does not interpret this value.
    std::uint32_t diagnostic_cycle_index{0};
};

using InputMacroAction = std::variant<
    BreakpointWaitAction,
    NeutralFramesAction,
    CaptureU32BaselineAction,
    WaitU32ChangeAction>;

struct InputMacroStep {
    std::string label;
    InputMacroAction action{BreakpointWaitAction{}};
};

struct InputMacroPlan {
    std::vector<InputMacroStep> steps;
};

enum class InputMacroActionKind : std::uint8_t {
    None,
    BreakpointWait,
    NeutralFrames,
    CaptureU32Baseline,
    WaitU32Change,
};

inline InputMacroActionKind ActionKind(const InputMacroAction& action)
{
    return std::visit([](const auto& value) -> InputMacroActionKind {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, BreakpointWaitAction>) {
            return InputMacroActionKind::BreakpointWait;
        } else if constexpr (std::is_same_v<T, NeutralFramesAction>) {
            return InputMacroActionKind::NeutralFrames;
        } else if constexpr (std::is_same_v<T, CaptureU32BaselineAction>) {
            return InputMacroActionKind::CaptureU32Baseline;
        } else {
            static_assert(std::is_same_v<T, WaitU32ChangeAction>);
            return InputMacroActionKind::WaitU32Change;
        }
    }, action);
}

} // namespace savor::inputmacro
