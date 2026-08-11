#pragma once

#include "ExecutionTypes.h"

#include <string>

namespace savor::runtime {

struct InputExecutionRelationshipOperationReceipt
{
    bool ok = false;
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

protected:
    IInputExecutionBindingPort() = default;
};

} // namespace savor::runtime
