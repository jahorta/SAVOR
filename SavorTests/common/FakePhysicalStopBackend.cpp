#include "FakePhysicalStopBackend.h"

#include <utility>

namespace savor::test_support {
namespace {

void WaitForGate(const std::shared_ptr<std::latch>& gate)
{
    if (gate)
        gate->wait();
}

void SignalGate(const std::shared_ptr<std::latch>& gate)
{
    if (gate)
        gate->count_down();
}

runtime::PhysicalStopBackendReceipt MakeReceipt(
    const FakePhysicalStopOutcome& outcome,
    runtime::PhysicalPlanGeneration generation,
    const runtime::PhysicalStopPointPlan& fallback_actual)
{
    return {
        .ok = outcome.ok,
        .integrity = outcome.integrity,
        .generation = generation,
        .actual = outcome.actual_override.value_or(fallback_actual),
        .message = outcome.message,
    };
}

} // namespace

void FakePhysicalStopBackendControl::SetBindOutcome(
    FakePhysicalStopOutcome outcome)
{
    std::lock_guard lock(mutex);
    bind_outcome = std::move(outcome);
}

void FakePhysicalStopBackendControl::SetUnbindOutcome(
    FakePhysicalStopOutcome outcome)
{
    std::lock_guard lock(mutex);
    unbind_outcome = std::move(outcome);
}

void FakePhysicalStopBackendControl::SetQueryOutcome(
    FakePhysicalStopOutcome outcome)
{
    std::lock_guard lock(mutex);
    query_outcome = std::move(outcome);
}

void FakePhysicalStopBackendControl::SetApplyOutcome(
    FakePhysicalStopOutcome outcome)
{
    std::lock_guard lock(mutex);
    apply_outcome = std::move(outcome);
}

void FakePhysicalStopBackendControl::SetPublishUnchangedOutcome(
    FakePhysicalStopOutcome outcome)
{
    std::lock_guard lock(mutex);
    publish_unchanged_outcome = std::move(outcome);
}

void FakePhysicalStopBackendControl::SetRevalidateOutcome(
    FakePhysicalStopOutcome outcome)
{
    std::lock_guard lock(mutex);
    revalidate_outcome = std::move(outcome);
}

void FakePhysicalStopBackendControl::SetClearOutcome(
    FakePhysicalStopOutcome outcome)
{
    std::lock_guard lock(mutex);
    clear_outcome = std::move(outcome);
}

void FakePhysicalStopBackendControl::SetApplyGate(
    FakePhysicalStopOperationGate gate)
{
    std::lock_guard lock(mutex);
    apply_gate = std::move(gate);
}

void FakePhysicalStopBackendControl::SetPublishUnchangedGate(
    FakePhysicalStopOperationGate gate)
{
    std::lock_guard lock(mutex);
    publish_unchanged_gate = std::move(gate);
}

void FakePhysicalStopBackendControl::SetRevalidateGate(
    FakePhysicalStopOperationGate gate)
{
    std::lock_guard lock(mutex);
    revalidate_gate = std::move(gate);
}

void FakePhysicalStopBackendControl::SetClearGate(
    FakePhysicalStopOperationGate gate)
{
    std::lock_guard lock(mutex);
    clear_gate = std::move(gate);
}

void FakePhysicalStopBackendControl::SetJitInvalidationHandler(
    std::function<void()> handler)
{
    std::lock_guard lock(mutex);
    jit_invalidation_handler = std::move(handler);
}

void FakePhysicalStopBackendControl::SetRestoreHandler(
    std::function<void(runtime::WorksetEpoch)> handler)
{
    std::lock_guard lock(mutex);
    restore_handler = std::move(handler);
}

std::vector<FakePhysicalStopCall>
FakePhysicalStopBackendControl::Calls() const
{
    std::lock_guard lock(mutex);
    return calls;
}

runtime::PhysicalStopPointPlan
FakePhysicalStopBackendControl::ActualPlan() const
{
    std::lock_guard lock(mutex);
    return actual_plan;
}

runtime::PhysicalPlanGeneration
FakePhysicalStopBackendControl::ActualGeneration() const
{
    std::lock_guard lock(mutex);
    return actual_generation;
}

savor::probe::INativeStopSink*
FakePhysicalStopBackendControl::BoundSink() const
{
    std::lock_guard lock(mutex);
    return bound_sink;
}

bool FakePhysicalStopBackendControl::HasOwnerViolation() const
{
    std::lock_guard lock(mutex);
    return owner_thread_violation;
}

std::optional<std::thread::id>
FakePhysicalStopBackendControl::OwnerThread() const
{
    std::lock_guard lock(mutex);
    return owner_thread;
}

std::vector<runtime::WorksetEpoch>
FakePhysicalStopBackendControl::InjectedRestoreEpochs() const
{
    std::lock_guard lock(mutex);
    return injected_restore_epochs;
}

FakePhysicalStopBackend::FakePhysicalStopBackend(
    std::shared_ptr<FakePhysicalStopBackendControl> control)
    : control_(std::move(control))
{
}

runtime::PhysicalStopBackendReceipt
FakePhysicalStopBackend::BindNativeStopSink(
    savor::probe::INativeStopSink& sink)
{
    std::lock_guard lock(control_->mutex);
    const std::thread::id thread = std::this_thread::get_id();
    if (!control_->owner_thread)
        control_->owner_thread = thread;
    else if (*control_->owner_thread != thread)
        control_->owner_thread_violation = true;
    control_->calls.push_back(FakePhysicalStopCall{
        .sequence = control_->next_call_sequence++,
        .operation = FakePhysicalStopOperation::BindSink,
        .generation = control_->actual_generation,
        .thread = thread,
    });

    FakePhysicalStopOutcome outcome = control_->bind_outcome;
    if (outcome.ok && control_->bound_sink && control_->bound_sink != &sink)
    {
        outcome.ok = false;
        outcome.message = "a different native stop sink is already bound";
    }
    if (outcome.ok)
        control_->bound_sink = &sink;
    return MakeReceipt(
        outcome,
        control_->actual_generation,
        control_->actual_plan);
}

runtime::PhysicalStopBackendReceipt
FakePhysicalStopBackend::UnbindNativeStopSink(
    savor::probe::INativeStopSink& sink)
{
    std::lock_guard lock(control_->mutex);
    const std::thread::id thread = std::this_thread::get_id();
    if (!control_->owner_thread)
        control_->owner_thread = thread;
    else if (*control_->owner_thread != thread)
        control_->owner_thread_violation = true;
    control_->calls.push_back(FakePhysicalStopCall{
        .sequence = control_->next_call_sequence++,
        .operation = FakePhysicalStopOperation::UnbindSink,
        .generation = control_->actual_generation,
        .thread = thread,
    });

    FakePhysicalStopOutcome outcome = control_->unbind_outcome;
    if (outcome.ok && control_->bound_sink != &sink)
    {
        outcome.ok = false;
        outcome.message = "the requested native stop sink is not bound";
    }
    if (outcome.ok)
        control_->bound_sink = nullptr;
    return MakeReceipt(
        outcome,
        control_->actual_generation,
        control_->actual_plan);
}

runtime::PhysicalStopBackendReceipt
FakePhysicalStopBackend::QueryPhysicalStopPoints() const
{
    std::lock_guard lock(control_->mutex);
    const std::thread::id thread = std::this_thread::get_id();
    if (!control_->owner_thread)
        control_->owner_thread = thread;
    else if (*control_->owner_thread != thread)
        control_->owner_thread_violation = true;
    control_->calls.push_back(FakePhysicalStopCall{
        .sequence = control_->next_call_sequence++,
        .operation = FakePhysicalStopOperation::Query,
        .plan = control_->actual_plan,
        .generation = control_->actual_generation,
        .thread = thread,
    });
    return MakeReceipt(
        control_->query_outcome,
        control_->actual_generation,
        control_->actual_plan);
}

runtime::PhysicalStopBackendReceipt
FakePhysicalStopBackend::ApplyExactPhysicalStopPlan(
    const runtime::PhysicalStopPointPlan& plan,
    runtime::PhysicalPlanGeneration generation,
    const std::function<void()>& commit_while_cpu_excluded)
{
    return RunPlanOperation(
        FakePhysicalStopOperation::Apply,
        plan,
        generation,
        commit_while_cpu_excluded);
}

runtime::PhysicalStopBackendReceipt
FakePhysicalStopBackend::PublishExactPhysicalStopPlanUnchanged(
    const runtime::PhysicalStopPointPlan& expected_plan,
    runtime::PhysicalPlanGeneration generation,
    const std::function<void()>& commit_while_cpu_excluded)
{
    return RunPlanOperation(
        FakePhysicalStopOperation::PublishUnchanged,
        expected_plan,
        generation,
        commit_while_cpu_excluded);
}

runtime::PhysicalStopBackendReceipt
FakePhysicalStopBackend::RevalidatePhysicalStopPlan(
    const runtime::PhysicalStopPointPlan& plan,
    runtime::PhysicalPlanGeneration generation,
    const std::function<void()>& commit_while_cpu_excluded)
{
    return RunPlanOperation(
        FakePhysicalStopOperation::Revalidate,
        plan,
        generation,
        commit_while_cpu_excluded);
}

runtime::PhysicalStopBackendReceipt
FakePhysicalStopBackend::ClearOwnedPhysicalStopPoints(
    runtime::PhysicalPlanGeneration generation,
    const std::function<void()>& commit_while_cpu_excluded)
{
    return RunPlanOperation(
        FakePhysicalStopOperation::Clear,
        {},
        generation,
        commit_while_cpu_excluded);
}

runtime::PhysicalStopBackendReceipt
FakePhysicalStopBackend::RunPlanOperation(
    FakePhysicalStopOperation operation,
    const runtime::PhysicalStopPointPlan& requested_plan,
    runtime::PhysicalPlanGeneration generation,
    const std::function<void()>& commit_while_cpu_excluded)
{
    FakePhysicalStopOutcome outcome;
    FakePhysicalStopOperationGate gate;
    std::uint64_t call_sequence = 0;
    runtime::PhysicalStopPointPlan prior_actual;
    {
        std::lock_guard lock(control_->mutex);
        const std::thread::id thread = std::this_thread::get_id();
        if (!control_->owner_thread)
            control_->owner_thread = thread;
        else if (*control_->owner_thread != thread)
            control_->owner_thread_violation = true;

        switch (operation)
        {
        case FakePhysicalStopOperation::Apply:
            outcome = control_->apply_outcome;
            gate = std::exchange(
                control_->apply_gate,
                FakePhysicalStopOperationGate{});
            break;
        case FakePhysicalStopOperation::PublishUnchanged:
            outcome = control_->publish_unchanged_outcome;
            gate = std::exchange(
                control_->publish_unchanged_gate,
                FakePhysicalStopOperationGate{});
            if (control_->actual_plan != requested_plan ||
                control_->actual_generation != generation)
            {
                outcome.ok = false;
                outcome.integrity =
                    runtime::PhysicalStopIntegrity::Unknown;
                outcome.message =
                    "fake exact unchanged publication detected drift";
            }
            break;
        case FakePhysicalStopOperation::Revalidate:
            outcome = control_->revalidate_outcome;
            gate = std::exchange(
                control_->revalidate_gate,
                FakePhysicalStopOperationGate{});
            break;
        case FakePhysicalStopOperation::Clear:
            outcome = control_->clear_outcome;
            gate = std::exchange(
                control_->clear_gate,
                FakePhysicalStopOperationGate{});
            break;
        default:
            outcome = {
                .ok = false,
                .integrity = runtime::PhysicalStopIntegrity::Preserved,
                .message = "unsupported fake physical stop operation",
            };
            break;
        }

        prior_actual = control_->actual_plan;
        call_sequence = control_->next_call_sequence++;
        control_->calls.push_back(FakePhysicalStopCall{
            .sequence = call_sequence,
            .operation = operation,
            .plan = requested_plan,
            .generation = generation,
            .thread = thread,
        });
    }

    SignalGate(gate.operation_entered);
    WaitForGate(gate.allow_operation);

    if (!outcome.ok)
        return MakeReceipt(outcome, generation, prior_actual);

    const runtime::PhysicalStopPointPlan committed_plan =
        outcome.actual_override.value_or(requested_plan);
    if (operation != FakePhysicalStopOperation::PublishUnchanged)
    {
        std::lock_guard lock(control_->mutex);
        control_->actual_plan = committed_plan;
        control_->actual_generation = generation;
    }

    SignalGate(gate.commit_entered);
    WaitForGate(gate.allow_commit);
    if (commit_while_cpu_excluded)
        commit_while_cpu_excluded();

    {
        std::lock_guard lock(control_->mutex);
        for (auto& call : control_->calls)
        {
            if (call.sequence == call_sequence)
            {
                call.commit_invoked =
                    static_cast<bool>(commit_while_cpu_excluded);
                break;
            }
        }
    }

    return {
        .ok = true,
        .integrity = outcome.integrity,
        .generation = generation,
        .actual = committed_plan,
        .message = outcome.message,
    };
}

savor::probe::NativeStopDecision
FakePhysicalStopBackend::InjectPcStop(
    savor::probe::NativeStopOrigin origin,
    std::uint32_t pc,
    PowerPC::PowerPCManager* power_pc)
{
    savor::probe::INativeStopSink* sink = nullptr;
    {
        std::lock_guard lock(control_->mutex);
        sink = control_->bound_sink;
        control_->calls.push_back(FakePhysicalStopCall{
            .sequence = control_->next_call_sequence++,
            .operation = FakePhysicalStopOperation::InjectPc,
            .generation = control_->actual_generation,
            .thread = std::this_thread::get_id(),
        });
    }
    if (!sink)
        return {};
    return sink->OnPcStop(savor::probe::NativePcStop{
        .origin = origin,
        .pc = pc,
        .power_pc = power_pc,
    });
}

savor::probe::NativeStopDecision
FakePhysicalStopBackend::InjectJitPcStop(
    std::uint32_t pc,
    PowerPC::PowerPCManager* power_pc)
{
    return InjectPcStop(
        savor::probe::NativeStopOrigin::Jit,
        pc,
        power_pc);
}

savor::probe::NativeStopDecision
FakePhysicalStopBackend::InjectMemoryStop(
    std::uint32_t pc,
    std::uint32_t address,
    std::uint32_t size,
    std::uint64_t value,
    bool write,
    bool post_write,
    Core::System* system)
{
    savor::probe::INativeStopSink* sink = nullptr;
    {
        std::lock_guard lock(control_->mutex);
        sink = control_->bound_sink;
        control_->calls.push_back(FakePhysicalStopCall{
            .sequence = control_->next_call_sequence++,
            .operation = FakePhysicalStopOperation::InjectMemory,
            .generation = control_->actual_generation,
            .thread = std::this_thread::get_id(),
        });
    }
    if (!sink)
        return {};
    return sink->OnMemoryStop(savor::probe::NativeMemoryStop{
        .origin = savor::probe::NativeStopOrigin::Memcheck,
        .pc = pc,
        .address = address,
        .size = size,
        .value = value,
        .write = write,
        .post_write = post_write,
        .system = system,
    });
}

bool FakePhysicalStopBackend::InjectJitInvalidation()
{
    std::function<void()> handler;
    {
        std::lock_guard lock(control_->mutex);
        handler = control_->jit_invalidation_handler;
        control_->calls.push_back(FakePhysicalStopCall{
            .sequence = control_->next_call_sequence++,
            .operation = FakePhysicalStopOperation::InjectJitInvalidation,
            .generation = control_->actual_generation,
            .thread = std::this_thread::get_id(),
        });
    }
    if (!handler)
        return false;
    handler();
    return true;
}

bool FakePhysicalStopBackend::InjectRestore(runtime::WorksetEpoch new_epoch)
{
    std::function<void(runtime::WorksetEpoch)> handler;
    {
        std::lock_guard lock(control_->mutex);
        handler = control_->restore_handler;
        control_->injected_restore_epochs.push_back(new_epoch);
        control_->calls.push_back(FakePhysicalStopCall{
            .sequence = control_->next_call_sequence++,
            .operation = FakePhysicalStopOperation::InjectRestore,
            .generation = control_->actual_generation,
            .thread = std::this_thread::get_id(),
        });
    }
    if (!handler)
        return false;
    handler(new_epoch);
    return true;
}

std::unique_ptr<runtime::IPhysicalStopPointBackendPort>
MakeFakePhysicalStopBackend(
    std::shared_ptr<FakePhysicalStopBackendControl> control)
{
    return std::make_unique<FakePhysicalStopBackend>(std::move(control));
}

} // namespace savor::test_support
