#pragma once

#include "ExecutionTypes.h"

#include <cstdint>
#include <optional>
#include <string>

namespace savor::runtime {

enum class InputAdvanceDecision : std::uint8_t
{
    Continue,
    Retry,
    Complete,
    Cancelled,
    Failed,
};

struct InputAdvanceReceipt
{
    bool ok = false;
    InputAdvanceDecision decision = InputAdvanceDecision::Failed;
    InputPublicationToken publication;
    std::string message;
    std::optional<InputPublicationEvidence> publication_evidence;
};

class IInputAdvancePort
{
public:
    virtual ~IInputAdvancePort() = default;

    IInputAdvancePort(const IInputAdvancePort&) = delete;
    IInputAdvancePort& operator=(const IInputAdvancePort&) = delete;

    [[nodiscard]] virtual InputAdvanceReceipt Validate(
        InputAdvanceBindingId binding,
        WorksetEpoch epoch) = 0;
    [[nodiscard]] virtual InputAdvanceReceipt PrepareNext(
        InputAdvanceBindingId binding,
        WorksetEpoch epoch,
        std::uint32_t advance_ordinal) = 0;
    [[nodiscard]] virtual InputAdvanceReceipt ObserveAcknowledgement(
        InputAdvanceBindingId binding,
        InputPublicationToken publication,
        WorksetEpoch epoch) = 0;
    [[nodiscard]] virtual InputAdvanceReceipt Complete(
        InputAdvanceBindingId binding,
        WorksetEpoch epoch) noexcept = 0;
    [[nodiscard]] virtual InputAdvanceReceipt Cancel(
        InputAdvanceBindingId binding,
        WorksetEpoch epoch) noexcept = 0;

protected:
    IInputAdvancePort() = default;
};

} // namespace savor::runtime
