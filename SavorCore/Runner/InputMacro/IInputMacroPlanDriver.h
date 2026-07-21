#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "InputMacroRuntime.h"

namespace savor::inputmacro {

// Snapshot of the most recent guest stop visible to an adaptive macro driver.
// stop_sequence advances only for stops produced by the input-macro host; the
// initial source stop is represented by sequence zero.
struct InputMacroStopInfo {
    BPKey key{0};
    std::uint32_t pc{0};
    std::uint64_t stop_sequence{0};
    std::uint64_t input_epoch{0};
    GCInputFrame requested_input{};
    std::uint32_t input_poll_count{0};
    bool input_acknowledged{false};
};

class IInputMacroDriverHost {
public:
    virtual ~IInputMacroDriverHost() = default;

    virtual InputMacroStopInfo current_stop() const = 0;
    virtual bool read_guest_memory(
        std::uint32_t address,
        std::span<std::byte> output) const = 0;
    virtual std::uint64_t current_vi() const { return 0; }
};

enum class InputMacroDriverStatus : std::uint8_t {
    PlanReady,
    Completed,
    Failed,
};

struct InputMacroDriverDecision {
    InputMacroDriverStatus status{InputMacroDriverStatus::Failed};
    InputMacroPlan plan;
    InputMacroFailure failure{InputMacroFailure::None};
    std::string diagnostic;
};

class IInputMacroPlanDriver {
public:
    virtual ~IInputMacroPlanDriver() = default;

    virtual std::span<const BPKey> declared_breakpoint_keys() const = 0;
    virtual InputMacroDriverDecision Start(IInputMacroDriverHost& host) = 0;
    virtual InputMacroDriverDecision Advance(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& completed_segment_result) = 0;
    virtual void Cancel() noexcept = 0;
};

} // namespace savor::inputmacro
