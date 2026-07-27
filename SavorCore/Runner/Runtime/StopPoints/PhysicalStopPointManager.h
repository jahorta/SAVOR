#pragma once

#include "IPhysicalStopPointBackendPort.h"

#include <functional>

namespace savor::runtime {

class PhysicalStopPointManager final
{
public:
    explicit PhysicalStopPointManager(IPhysicalStopPointBackendPort& backend) noexcept;
    ~PhysicalStopPointManager() = default;

    PhysicalStopPointManager(const PhysicalStopPointManager&) = delete;
    PhysicalStopPointManager& operator=(const PhysicalStopPointManager&) = delete;

    [[nodiscard]] PhysicalStopBackendReceipt BindNativeStopSink(
        savor::probe::INativeStopSink& sink);
    [[nodiscard]] PhysicalStopBackendReceipt UnbindNativeStopSink();

    [[nodiscard]] PhysicalStopBackendReceipt ApplyExactPlan(
        const PhysicalStopPointPlan& plan,
        const std::function<void()>& commit_while_cpu_excluded,
        bool force_reconcile = false);
    [[nodiscard]] PhysicalStopBackendReceipt RevalidateAfterJit(
        const std::function<void()>& commit_while_cpu_excluded = {});
    [[nodiscard]] PhysicalStopBackendReceipt ValidateExactPlanUnchanged();
    [[nodiscard]] PhysicalStopBackendReceipt ClearOwnedStopPoints(
        const std::function<void()>& commit_while_cpu_excluded = {});

    [[nodiscard]] const PhysicalStopPointPlan& current_plan() const noexcept
    {
        return current_plan_;
    }

    [[nodiscard]] PhysicalPlanGeneration generation() const noexcept
    {
        return generation_;
    }

    [[nodiscard]] PhysicalPlanGeneration next_generation() const noexcept
    {
        return NextGeneration();
    }

    [[nodiscard]] bool sink_bound() const noexcept
    {
        return bound_sink_ != nullptr;
    }

    [[nodiscard]] bool has_exact_plan() const noexcept
    {
        return has_exact_plan_;
    }

    [[nodiscard]] bool integrity_unknown() const noexcept
    {
        return integrity_unknown_;
    }

private:
    enum class BackendOperation
    {
        Apply,
        Revalidate,
        Clear,
    };

    [[nodiscard]] PhysicalStopBackendReceipt Execute(
        BackendOperation operation,
        const PhysicalStopPointPlan& requested,
        const std::function<void()>& commit_while_cpu_excluded);
    [[nodiscard]] static PhysicalStopBackendReceipt Failure(
        PhysicalStopIntegrity integrity,
        PhysicalPlanGeneration generation,
        PhysicalStopPointPlan actual,
        std::string message);
    [[nodiscard]] PhysicalPlanGeneration NextGeneration() const noexcept;

    IPhysicalStopPointBackendPort& backend_;
    savor::probe::INativeStopSink* bound_sink_ = nullptr;
    PhysicalStopPointPlan current_plan_;
    PhysicalPlanGeneration generation_;
    bool has_exact_plan_ = false;
    bool integrity_unknown_ = false;
};

} // namespace savor::runtime
