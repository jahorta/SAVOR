#pragma once

#include "Runner/Runtime/StopPoints/IPhysicalStopPointBackendPort.h"

#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace savor::test_support {

enum class FakePhysicalStopOperation : std::uint8_t
{
    BindSink,
    UnbindSink,
    Query,
    Apply,
    PublishUnchanged,
    Revalidate,
    Clear,
    InjectPc,
    InjectMemory,
    InjectJitInvalidation,
    InjectRestore,
};

struct FakePhysicalStopCall
{
    std::uint64_t sequence = 0;
    FakePhysicalStopOperation operation = FakePhysicalStopOperation::Query;
    runtime::PhysicalStopPointPlan plan;
    runtime::PhysicalPlanGeneration generation;
    std::thread::id thread;
    bool commit_invoked = false;
};

struct FakePhysicalStopOutcome
{
    bool ok = true;
    runtime::PhysicalStopIntegrity integrity =
        runtime::PhysicalStopIntegrity::Preserved;
    std::string message;
    std::optional<runtime::PhysicalStopPointPlan> actual_override;
};

struct FakePhysicalStopOperationGate
{
    std::shared_ptr<std::latch> operation_entered;
    std::shared_ptr<std::latch> allow_operation;
    std::shared_ptr<std::latch> commit_entered;
    std::shared_ptr<std::latch> allow_commit;
};

struct FakePhysicalStopBackendControl
{
    mutable std::mutex mutex;
    std::optional<std::thread::id> owner_thread;
    bool owner_thread_violation = false;
    std::vector<FakePhysicalStopCall> calls;
    std::uint64_t next_call_sequence = 1;

    savor::probe::INativeStopSink* bound_sink = nullptr;
    runtime::PhysicalStopPointPlan actual_plan;
    runtime::PhysicalPlanGeneration actual_generation;

    FakePhysicalStopOutcome bind_outcome;
    FakePhysicalStopOutcome unbind_outcome;
    FakePhysicalStopOutcome query_outcome;
    FakePhysicalStopOutcome apply_outcome;
    FakePhysicalStopOutcome publish_unchanged_outcome;
    FakePhysicalStopOutcome revalidate_outcome;
    FakePhysicalStopOutcome clear_outcome;

    FakePhysicalStopOperationGate apply_gate;
    FakePhysicalStopOperationGate publish_unchanged_gate;
    FakePhysicalStopOperationGate revalidate_gate;
    FakePhysicalStopOperationGate clear_gate;

    std::function<void()> jit_invalidation_handler;
    std::function<void(runtime::WorksetEpoch)> restore_handler;
    std::vector<runtime::WorksetEpoch> injected_restore_epochs;

    void SetBindOutcome(FakePhysicalStopOutcome outcome);
    void SetUnbindOutcome(FakePhysicalStopOutcome outcome);
    void SetQueryOutcome(FakePhysicalStopOutcome outcome);
    void SetApplyOutcome(FakePhysicalStopOutcome outcome);
    void SetPublishUnchangedOutcome(FakePhysicalStopOutcome outcome);
    void SetRevalidateOutcome(FakePhysicalStopOutcome outcome);
    void SetClearOutcome(FakePhysicalStopOutcome outcome);

    void SetApplyGate(FakePhysicalStopOperationGate gate);
    void SetPublishUnchangedGate(FakePhysicalStopOperationGate gate);
    void SetRevalidateGate(FakePhysicalStopOperationGate gate);
    void SetClearGate(FakePhysicalStopOperationGate gate);

    void SetJitInvalidationHandler(std::function<void()> handler);
    void SetRestoreHandler(
        std::function<void(runtime::WorksetEpoch)> handler);

    [[nodiscard]] std::vector<FakePhysicalStopCall> Calls() const;
    [[nodiscard]] runtime::PhysicalStopPointPlan ActualPlan() const;
    [[nodiscard]] runtime::PhysicalPlanGeneration ActualGeneration() const;
    [[nodiscard]] savor::probe::INativeStopSink* BoundSink() const;
    [[nodiscard]] bool HasOwnerViolation() const;
    [[nodiscard]] std::optional<std::thread::id> OwnerThread() const;
    [[nodiscard]] std::vector<runtime::WorksetEpoch> InjectedRestoreEpochs() const;
};

class FakePhysicalStopBackend final
    : public runtime::IPhysicalStopPointBackendPort
{
public:
    explicit FakePhysicalStopBackend(
        std::shared_ptr<FakePhysicalStopBackendControl> control);

    [[nodiscard]] runtime::PhysicalStopBackendReceipt BindNativeStopSink(
        savor::probe::INativeStopSink& sink) override;
    [[nodiscard]] runtime::PhysicalStopBackendReceipt UnbindNativeStopSink(
        savor::probe::INativeStopSink& sink) override;
    [[nodiscard]] runtime::PhysicalStopBackendReceipt
    QueryPhysicalStopPoints() const override;
    [[nodiscard]] runtime::PhysicalStopBackendReceipt
    ApplyExactPhysicalStopPlan(
        const runtime::PhysicalStopPointPlan& plan,
        runtime::PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded) override;
    [[nodiscard]] runtime::PhysicalStopBackendReceipt
    PublishExactPhysicalStopPlanUnchanged(
        const runtime::PhysicalStopPointPlan& expected_plan,
        runtime::PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded) override;
    [[nodiscard]] runtime::PhysicalStopBackendReceipt
    RevalidatePhysicalStopPlan(
        const runtime::PhysicalStopPointPlan& plan,
        runtime::PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded) override;
    [[nodiscard]] runtime::PhysicalStopBackendReceipt
    ClearOwnedPhysicalStopPoints(
        runtime::PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded) override;

    [[nodiscard]] savor::probe::NativeStopDecision InjectPcStop(
        savor::probe::NativeStopOrigin origin,
        std::uint32_t pc,
        PowerPC::PowerPCManager* power_pc = nullptr);
    [[nodiscard]] savor::probe::NativeStopDecision InjectJitPcStop(
        std::uint32_t pc,
        PowerPC::PowerPCManager* power_pc = nullptr);
    [[nodiscard]] savor::probe::NativeStopDecision InjectMemoryStop(
        std::uint32_t pc,
        std::uint32_t address,
        std::uint32_t size,
        std::uint64_t value,
        bool write,
        bool post_write = true,
        Core::System* system = nullptr);

    [[nodiscard]] bool InjectJitInvalidation();
    [[nodiscard]] bool InjectRestore(runtime::WorksetEpoch new_epoch);

private:
    [[nodiscard]] runtime::PhysicalStopBackendReceipt RunPlanOperation(
        FakePhysicalStopOperation operation,
        const runtime::PhysicalStopPointPlan& requested_plan,
        runtime::PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded);

    std::shared_ptr<FakePhysicalStopBackendControl> control_;
};

[[nodiscard]] std::unique_ptr<runtime::IPhysicalStopPointBackendPort>
MakeFakePhysicalStopBackend(
    std::shared_ptr<FakePhysicalStopBackendControl> control);

} // namespace savor::test_support
