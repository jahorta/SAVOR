#pragma once

#include <cstdint>
#include <span>

#include "InputMacroPlan.h"

namespace savor::inputmacro {

enum class InputMacroHostStatus : std::uint8_t {
    Succeeded,
    TimedOut,
    Cancelled,
    ReadFailed,
    Failed,
};

struct BreakpointWaitResult {
    InputMacroHostStatus status{InputMacroHostStatus::Failed};
    bool hit{false};
    BPKey hit_key{0};
    std::uint32_t hit_pc{0};
    std::uint64_t stop_sequence{0};
    std::uint64_t input_epoch{0};
    GCInputFrame requested_input{};
    std::uint32_t input_poll_count{0};
    bool input_acknowledged{false};
    std::uint32_t elapsed_ms{0};
};

struct MemoryChangeResult {
    InputMacroHostStatus status{InputMacroHostStatus::Failed};
    std::uint32_t latest_value{0};
    std::uint32_t poll_count{0};
    std::uint32_t elapsed_ms{0};
};

class IInputMacroHost {
public:
    virtual ~IInputMacroHost() = default;

    virtual bool acquire_exclusive_session(std::span<const BPKey> provider_keys) = 0;
    virtual void release_exclusive_session() = 0;

    virtual BreakpointWaitResult run_to_breakpoints(const BreakpointWaitAction& action) = 0;
    virtual InputMacroHostStatus step_neutral_frames(std::uint32_t frame_count) = 0;
    virtual bool read_u32(std::uint32_t address, std::uint32_t& value_out) = 0;
    virtual MemoryChangeResult wait_for_u32_change(
        std::uint32_t address,
        std::uint32_t baseline,
        std::uint32_t timeout_ms) = 0;

    // The runtime invokes these in this exact order during cleanup. Session
    // release is skipped only when acquisition never succeeded.
    virtual void set_neutral_input() = 0;
    virtual void clear_macro_memory_watchpoints() = 0;
    virtual void restore_breakpoint_state() = 0;
};

} // namespace savor::inputmacro
