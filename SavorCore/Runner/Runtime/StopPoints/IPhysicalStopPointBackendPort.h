#pragma once

#include "StopPointTypes.h"

#include "../../../../SavorProbe/NativeStopHooks.h"

#include <functional>

namespace savor::runtime {

class IPhysicalStopPointBackendPort
{
public:
    virtual ~IPhysicalStopPointBackendPort() = default;

    IPhysicalStopPointBackendPort(const IPhysicalStopPointBackendPort&) = delete;
    IPhysicalStopPointBackendPort& operator=(const IPhysicalStopPointBackendPort&) = delete;

    [[nodiscard]] virtual PhysicalStopBackendReceipt BindNativeStopSink(
        savor::probe::INativeStopSink& sink) = 0;
    [[nodiscard]] virtual PhysicalStopBackendReceipt UnbindNativeStopSink(
        savor::probe::INativeStopSink& sink) = 0;
    [[nodiscard]] virtual PhysicalStopBackendReceipt QueryPhysicalStopPoints() const = 0;
    [[nodiscard]] virtual PhysicalStopBackendReceipt ApplyExactPhysicalStopPlan(
        const PhysicalStopPointPlan& plan,
        PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded) = 0;
    [[nodiscard]] virtual PhysicalStopBackendReceipt
    PublishExactPhysicalStopPlanUnchanged(
        const PhysicalStopPointPlan& expected_plan,
        PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded) = 0;
    [[nodiscard]] virtual PhysicalStopBackendReceipt RevalidatePhysicalStopPlan(
        const PhysicalStopPointPlan& plan,
        PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded) = 0;
    [[nodiscard]] virtual PhysicalStopBackendReceipt ClearOwnedPhysicalStopPoints(
        PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded) = 0;

protected:
    IPhysicalStopPointBackendPort() = default;
};

} // namespace savor::runtime
