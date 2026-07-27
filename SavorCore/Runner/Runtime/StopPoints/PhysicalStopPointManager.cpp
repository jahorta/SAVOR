#include "PhysicalStopPointManager.h"

#include <exception>
#include <limits>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] PhysicalStopBackendReceipt SuccessReceipt(
    PhysicalPlanGeneration generation,
    PhysicalStopPointPlan actual)
{
    return {
        true,
        PhysicalStopIntegrity::Preserved,
        generation,
        std::move(actual),
        {},
    };
}

} // namespace

PhysicalStopPointManager::PhysicalStopPointManager(
    IPhysicalStopPointBackendPort& backend) noexcept
    : backend_(backend)
{
}

PhysicalStopBackendReceipt PhysicalStopPointManager::BindNativeStopSink(
    savor::probe::INativeStopSink& sink)
{
    if (bound_sink_ == &sink)
        return SuccessReceipt(generation_, current_plan_);
    if (bound_sink_ != nullptr)
    {
        return Failure(
            PhysicalStopIntegrity::Preserved,
            generation_,
            current_plan_,
            "A different native stop sink is already bound");
    }

    PhysicalStopBackendReceipt receipt = backend_.BindNativeStopSink(sink);
    if (!receipt.ok)
    {
        if (receipt.integrity == PhysicalStopIntegrity::Unknown)
            integrity_unknown_ = true;
        return receipt;
    }

    bound_sink_ = &sink;
    return receipt;
}

PhysicalStopBackendReceipt PhysicalStopPointManager::UnbindNativeStopSink()
{
    if (bound_sink_ == nullptr)
        return SuccessReceipt(generation_, current_plan_);

    PhysicalStopBackendReceipt receipt =
        backend_.UnbindNativeStopSink(*bound_sink_);
    if (!receipt.ok)
    {
        if (receipt.integrity == PhysicalStopIntegrity::Unknown)
            integrity_unknown_ = true;
        return receipt;
    }

    bound_sink_ = nullptr;
    return receipt;
}

PhysicalStopBackendReceipt PhysicalStopPointManager::ApplyExactPlan(
    const PhysicalStopPointPlan& plan,
    const std::function<void()>& commit_while_cpu_excluded,
    bool force_reconcile)
{
    if (!force_reconcile && has_exact_plan_ && plan == current_plan_)
    {
        bool commit_started = false;
        bool commit_completed = false;
        const auto guarded_commit = [&] {
            commit_started = true;
            if (commit_while_cpu_excluded)
                commit_while_cpu_excluded();
            commit_completed = true;
        };
        PhysicalStopBackendReceipt receipt;
        try
        {
            receipt = backend_.PublishExactPhysicalStopPlanUnchanged(
                current_plan_,
                generation_,
                guarded_commit);
        }
        catch (const std::exception& ex)
        {
            integrity_unknown_ = integrity_unknown_ || commit_started;
            return Failure(
                commit_started
                    ? PhysicalStopIntegrity::Unknown
                    : PhysicalStopIntegrity::Preserved,
                generation_,
                current_plan_,
                std::string("Physical stop-point publication threw: ") + ex.what());
        }
        catch (...)
        {
            integrity_unknown_ = integrity_unknown_ || commit_started;
            return Failure(
                commit_started
                    ? PhysicalStopIntegrity::Unknown
                    : PhysicalStopIntegrity::Preserved,
                generation_,
                current_plan_,
                "Physical stop-point publication threw");
        }
        if (!receipt.ok)
        {
            if (receipt.integrity == PhysicalStopIntegrity::Unknown ||
                commit_started)
            {
                integrity_unknown_ = true;
            }
            if (commit_started &&
                receipt.integrity == PhysicalStopIntegrity::Preserved)
            {
                receipt.integrity = PhysicalStopIntegrity::Unknown;
            }
            return receipt;
        }
        if (!commit_completed ||
            receipt.generation != generation_ ||
            receipt.actual != current_plan_)
        {
            integrity_unknown_ = true;
            return Failure(
                PhysicalStopIntegrity::Unknown,
                receipt.generation,
                std::move(receipt.actual),
                "Exact unchanged stop-point publication was not atomic");
        }
        return receipt;
    }

    return Execute(BackendOperation::Apply, plan, commit_while_cpu_excluded);
}

PhysicalStopBackendReceipt PhysicalStopPointManager::RevalidateAfterJit(
    const std::function<void()>& commit_while_cpu_excluded)
{
    if (!has_exact_plan_)
    {
        return Failure(
            PhysicalStopIntegrity::Preserved,
            generation_,
            current_plan_,
            "Cannot revalidate stop points before an exact plan has been applied");
    }
    return Execute(
        BackendOperation::Revalidate,
        current_plan_,
        commit_while_cpu_excluded);
}

PhysicalStopBackendReceipt PhysicalStopPointManager::ValidateExactPlanUnchanged()
{
    if (!has_exact_plan_)
    {
        return Failure(
            PhysicalStopIntegrity::Preserved,
            generation_,
            current_plan_,
            "Cannot validate stop points before an exact plan has been applied");
    }

    PhysicalStopBackendReceipt receipt;
    try
    {
        receipt = backend_.QueryPhysicalStopPoints();
    }
    catch (const std::exception& ex)
    {
        integrity_unknown_ = true;
        return Failure(
            PhysicalStopIntegrity::Unknown,
            generation_,
            current_plan_,
            std::string("Physical stop-point query threw: ") + ex.what());
    }
    catch (...)
    {
        integrity_unknown_ = true;
        return Failure(
            PhysicalStopIntegrity::Unknown,
            generation_,
            current_plan_,
            "Physical stop-point query threw");
    }

    if (!receipt.ok)
    {
        if (receipt.integrity == PhysicalStopIntegrity::Unknown)
            integrity_unknown_ = true;
        return receipt;
    }
    if (receipt.generation != generation_ || receipt.actual != current_plan_)
    {
        integrity_unknown_ = true;
        return Failure(
            PhysicalStopIntegrity::Unknown,
            receipt.generation,
            std::move(receipt.actual),
            "Unmanaged physical stop-point drift was detected");
    }

    return receipt;
}

PhysicalStopBackendReceipt PhysicalStopPointManager::ClearOwnedStopPoints(
    const std::function<void()>& commit_while_cpu_excluded)
{
    return Execute(
        BackendOperation::Clear,
        PhysicalStopPointPlan{},
        commit_while_cpu_excluded);
}

PhysicalStopBackendReceipt PhysicalStopPointManager::Execute(
    BackendOperation operation,
    const PhysicalStopPointPlan& requested,
    const std::function<void()>& commit_while_cpu_excluded)
{
    const PhysicalPlanGeneration requested_generation = NextGeneration();
    if (!requested_generation)
    {
        return Failure(
            PhysicalStopIntegrity::Unknown,
            generation_,
            current_plan_,
            "Physical stop-point generation exhausted");
    }

    bool commit_started = false;
    bool commit_completed = false;
    const auto guarded_commit = [&] {
        commit_started = true;
        if (commit_while_cpu_excluded)
            commit_while_cpu_excluded();
        commit_completed = true;
    };

    PhysicalStopBackendReceipt receipt;
    try
    {
        switch (operation)
        {
        case BackendOperation::Apply:
            receipt = backend_.ApplyExactPhysicalStopPlan(
                requested,
                requested_generation,
                guarded_commit);
            break;
        case BackendOperation::Revalidate:
            receipt = backend_.RevalidatePhysicalStopPlan(
                requested,
                requested_generation,
                guarded_commit);
            break;
        case BackendOperation::Clear:
            receipt = backend_.ClearOwnedPhysicalStopPoints(
                requested_generation,
                guarded_commit);
            break;
        }
    }
    catch (const std::exception& ex)
    {
        integrity_unknown_ = integrity_unknown_ || commit_started;
        return Failure(
            commit_started
                ? PhysicalStopIntegrity::Unknown
                : PhysicalStopIntegrity::Preserved,
            generation_,
            current_plan_,
            std::string("Physical stop-point backend threw: ") + ex.what());
    }
    catch (...)
    {
        integrity_unknown_ = integrity_unknown_ || commit_started;
        return Failure(
            commit_started
                ? PhysicalStopIntegrity::Unknown
                : PhysicalStopIntegrity::Preserved,
            generation_,
            current_plan_,
            "Physical stop-point backend threw");
    }

    if (!receipt.ok)
    {
        if (receipt.integrity == PhysicalStopIntegrity::Unknown || commit_started)
            integrity_unknown_ = true;
        if (commit_started && receipt.integrity == PhysicalStopIntegrity::Preserved)
            receipt.integrity = PhysicalStopIntegrity::Unknown;
        return receipt;
    }

    if (!commit_completed)
    {
        integrity_unknown_ = true;
        return Failure(
            PhysicalStopIntegrity::Unknown,
            receipt.generation,
            std::move(receipt.actual),
            "Physical stop-point backend reported success without committing the dispatch snapshot");
    }
    if (receipt.generation != requested_generation)
    {
        integrity_unknown_ = true;
        return Failure(
            PhysicalStopIntegrity::Unknown,
            receipt.generation,
            std::move(receipt.actual),
            "Physical stop-point backend returned the wrong generation");
    }
    if (receipt.actual != requested)
    {
        integrity_unknown_ = true;
        return Failure(
            PhysicalStopIntegrity::Unknown,
            receipt.generation,
            std::move(receipt.actual),
            "Physical stop-point backend did not apply the exact requested plan");
    }

    current_plan_ = requested;
    generation_ = requested_generation;
    has_exact_plan_ = true;
    integrity_unknown_ = false;
    return receipt;
}

PhysicalStopBackendReceipt PhysicalStopPointManager::Failure(
    PhysicalStopIntegrity integrity,
    PhysicalPlanGeneration generation,
    PhysicalStopPointPlan actual,
    std::string message)
{
    return {
        false,
        integrity,
        generation,
        std::move(actual),
        std::move(message),
    };
}

PhysicalPlanGeneration PhysicalStopPointManager::NextGeneration() const noexcept
{
    const std::uint64_t current = generation_.value();
    if (current == std::numeric_limits<std::uint64_t>::max())
        return {};
    return PhysicalPlanGeneration(current + 1);
}

} // namespace savor::runtime
