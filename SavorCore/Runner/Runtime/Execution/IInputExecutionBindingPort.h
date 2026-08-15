#pragma once

#include "ExecutionTypes.h"

#include <string>

namespace savor::runtime {

struct InputExecutionRelationshipOperationReceipt
{
    bool ok = false;
    std::string message;
};

// Diagnostic-only view of the backend evidence for a live relationship. This
// never observes, completes, retires, or otherwise mutates the relationship.
struct InputExecutionRelationshipInspection
{
    bool ok = false;
    bool requires_observation = false;
    std::uint64_t publication_epoch = 0;
    std::uint32_t callback_count = 0;
    std::uint32_t a_control_callback_count = 0;
    savor::GCInputFrame frame{};
    std::string message;
};

class IInputExecutionBindingPort
{
public:
    virtual ~IInputExecutionBindingPort() = default;

    IInputExecutionBindingPort(const IInputExecutionBindingPort&) = delete;
    IInputExecutionBindingPort& operator=(
        const IInputExecutionBindingPort&) = delete;

    [[nodiscard]] virtual InputExecutionRelationshipOperationReceipt Validate(
        InputExecutionRelationshipId relationship,
        WorksetEpoch epoch) = 0;
    [[nodiscard]] virtual InputExecutionRelationshipOperationReceipt Complete(
        InputExecutionRelationshipId relationship,
        WorksetEpoch epoch) noexcept = 0;
    [[nodiscard]] virtual InputExecutionRelationshipOperationReceipt Cancel(
        InputExecutionRelationshipId relationship,
        WorksetEpoch epoch) noexcept = 0;
    [[nodiscard]] virtual InputExecutionRelationshipInspection Inspect(
        InputExecutionRelationshipId relationship,
        WorksetEpoch epoch) const noexcept = 0;

protected:
    IInputExecutionBindingPort() = default;
};

} // namespace savor::runtime
