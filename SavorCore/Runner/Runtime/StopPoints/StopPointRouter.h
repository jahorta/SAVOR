#pragma once

#include "PhysicalStopPointManager.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace savor::runtime {

inline constexpr std::size_t kStopPointNativeIngressCapacity = 256;
inline constexpr std::size_t kStopPointRoutingHistoryCapacity = 256;
inline constexpr std::size_t kMaxStopDeliveriesPerHit = 128;
inline constexpr std::size_t kMaxCpuObserversPerHit = 32;

struct StopPointCpuContext
{
    NativeStopPath path = NativeStopPath::Synthetic;
    std::uint32_t pc = 0;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint64_t value = 0;
    bool write = false;
    bool post_write = false;
    std::uint64_t synthetic_identity = 0;
    PowerPC::PowerPCManager* power_pc = nullptr;
    Core::System* system = nullptr;
};

class IStopPointCpuEvaluator
{
public:
    virtual ~IStopPointCpuEvaluator() = default;

    IStopPointCpuEvaluator(const IStopPointCpuEvaluator&) = delete;
    IStopPointCpuEvaluator& operator=(const IStopPointCpuEvaluator&) = delete;

    [[nodiscard]] virtual bool Qualify(
        std::uint32_t qualification_id,
        const StopPointCpuContext& context) noexcept = 0;
    [[nodiscard]] virtual RoutedHitSample Sample(
        std::uint32_t descriptor_id,
        const StopPointCpuContext& context) noexcept = 0;

protected:
    IStopPointCpuEvaluator() = default;
};

enum class StopCpuObservationResult : std::uint8_t
{
    Observed,
    ObservedRequiresReconcile,
    Ignored,
    Failed,
};

class IStopPointCpuObserver
{
public:
    virtual ~IStopPointCpuObserver() = default;

    IStopPointCpuObserver(const IStopPointCpuObserver&) = delete;
    IStopPointCpuObserver& operator=(const IStopPointCpuObserver&) = delete;

    [[nodiscard]] virtual StopCpuObservationResult ObserveRoutedHit(
        std::uint32_t descriptor_id,
        const RoutedStopEvent& event) noexcept = 0;

protected:
    IStopPointCpuObserver() = default;
};

enum class StopCurrentPointPolicy : std::uint8_t
{
    Ignore,
    AcceptIfAvailable,
    Require,
};

struct StopGroupRegistrationOptions
{
    StopCurrentPointPolicy current_point = StopCurrentPointPolicy::Ignore;
};

struct StopPointLifecycleReceipt
{
    bool ok = false;
    StateEpoch state_epoch;
    StopDispatchGeneration dispatch_generation;
    PhysicalPlanGeneration physical_generation;
    PhysicalStopIntegrity physical_integrity = PhysicalStopIntegrity::Preserved;
    std::size_t drained_event_count = 0;
    StopPointError error;
};

struct StopIngressDropDiagnostic
{
    StopSourceId source_id;
    std::uint64_t passive_drop_count = 0;
};

using StopPointIngressNotifier = void (*)(void* context) noexcept;

class StopPointRouter;
struct StopPointLeaseControl;

class StopSubscriptionGroupHandle final
{
public:
    StopSubscriptionGroupHandle() = default;
    ~StopSubscriptionGroupHandle();

    StopSubscriptionGroupHandle(const StopSubscriptionGroupHandle&) = delete;
    StopSubscriptionGroupHandle& operator=(const StopSubscriptionGroupHandle&) = delete;

    StopSubscriptionGroupHandle(StopSubscriptionGroupHandle&& other) noexcept;
    StopSubscriptionGroupHandle& operator=(StopSubscriptionGroupHandle&& other) noexcept;

    [[nodiscard]] bool active() const noexcept
    {
        return lease_.active;
    }

    [[nodiscard]] const StopSubscriptionGroupLease& lease() const noexcept
    {
        return lease_;
    }

    [[nodiscard]] StopGroupReceipt Replace(StopSubscriptionGroupDefinition definition);
    [[nodiscard]] StopReleaseReceipt Release();

private:
    friend class StopPointRouter;

    StopSubscriptionGroupHandle(
        std::weak_ptr<StopPointLeaseControl> control,
        StopSubscriptionGroupLease lease) noexcept;
    void ReleaseNoThrow() noexcept;

    std::weak_ptr<StopPointLeaseControl> control_;
    StopSubscriptionGroupLease lease_;
};

struct StopGroupRegistrationResult
{
    StopGroupReceipt receipt;
    StopSubscriptionGroupHandle handle;
    std::optional<StopRouteReceipt> current_point;
};

class StopPointRouter final : public savor::probe::INativeStopSink
{
public:
    struct Impl;

    explicit StopPointRouter(
        PhysicalStopPointManager& physical_manager,
        IStopPointCpuEvaluator* cpu_evaluator = nullptr,
        IStopPointCpuObserver* cpu_observer = nullptr);
    ~StopPointRouter() override;

    StopPointRouter(const StopPointRouter&) = delete;
    StopPointRouter& operator=(const StopPointRouter&) = delete;

    [[nodiscard]] StopPointLifecycleReceipt Initialize(StateEpoch first_epoch);
    [[nodiscard]] StopPointLifecycleReceipt PrepareStateReplacement(
        StateEpoch expected_epoch);
    [[nodiscard]] StopPointLifecycleReceipt CommitStateReplacement(
        StateEpoch new_epoch);
    [[nodiscard]] StopPointLifecycleReceipt RollbackStateReplacement(
        StateEpoch expected_epoch);
    [[nodiscard]] StopPointLifecycleReceipt RevalidateAfterJit();
    [[nodiscard]] StopPointLifecycleReceipt ValidateBreakpointChangeNotification();
    [[nodiscard]] StopPointLifecycleReceipt StopIngressDrainAndCleanup();
    [[nodiscard]] StopPointError SetIngressNotificationCounter(
        std::atomic<std::uint64_t>* counter);
    [[nodiscard]] StopPointError SetIngressNotifier(
        void* context,
        StopPointIngressNotifier notifier);

    [[nodiscard]] StopGroupRegistrationResult RegisterGroup(
        StopSubscriptionGroupDefinition definition,
        StopGroupRegistrationOptions options = {});
    [[nodiscard]] StopGroupReceipt ReplaceGroup(
        StopSubscriptionGroupLease& lease,
        StopSubscriptionGroupDefinition definition);
    [[nodiscard]] StopReleaseReceipt ReleaseGroup(
        StopSubscriptionGroupLease& lease);
    [[nodiscard]] StopRouteReceipt AcceptCurrentPoint(
        const StopSubscriptionGroupLease& lease);
    [[nodiscard]] StopPointError ArmInterruptionSuppression(
        const StopRouteReceipt& receipt,
        const StopInterruptionHandlerRequest& request);
    [[nodiscard]] StopPointError DepartCurrentPoint();

    [[nodiscard]] std::vector<StopRouteReceipt> DrainIngress();
    [[nodiscard]] std::vector<StopRouteReceipt> RoutingHistory() const;
    [[nodiscard]] std::vector<StopIngressDropDiagnostic>
    PassiveDropDiagnostics() const;
    [[nodiscard]] PhysicalStopPointPlan DesiredPhysicalPlan() const;

    [[nodiscard]] StateEpoch state_epoch() const noexcept
    {
        return state_epoch_;
    }

    [[nodiscard]] StopDispatchGeneration dispatch_generation() const noexcept
    {
        return dispatch_generation_;
    }

    [[nodiscard]] bool ingress_enabled() const noexcept
    {
        return ingress_enabled_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool authoritative_overflowed() const noexcept
    {
        return authoritative_overflow_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t passive_drop_count() const noexcept
    {
        return passive_drop_count_.load(std::memory_order_acquire);
    }

    [[nodiscard]] savor::probe::NativeStopDecision OnPcStop(
        const savor::probe::NativePcStop& stop) noexcept override;
    [[nodiscard]] savor::probe::NativeStopDecision OnMemoryStop(
        const savor::probe::NativeMemoryStop& stop) noexcept override;

    [[nodiscard]] StopRouteReceipt InjectSyntheticStop(
        SyntheticStopPointSpec point,
        bool authoritative = false);

private:
    friend class StopSubscriptionGroupHandle;

    std::unique_ptr<Impl> impl_;

    [[nodiscard]] StopGroupReceipt ReplaceFromHandle(
        StopSubscriptionGroupLease& lease,
        StopSubscriptionGroupDefinition definition);
    [[nodiscard]] StopReleaseReceipt ReleaseFromHandle(
        StopSubscriptionGroupLease& lease);
    void ReleaseFromHandleNoThrow(StopSubscriptionGroupLease& lease) noexcept;
    [[nodiscard]] savor::probe::NativeStopDecision RouteNative(
        const StopPointCpuContext& context,
        RoutedStopEvidence evidence) noexcept;
    void NotifyIngressPublication() noexcept;
    void WaitForNativeIngressQuiescence();

    PhysicalStopPointManager& physical_manager_;
    IStopPointCpuEvaluator* cpu_evaluator_ = nullptr;
    IStopPointCpuObserver* cpu_observer_ = nullptr;
    std::shared_ptr<StopPointLeaseControl> lease_control_;
    std::thread::id owner_thread_;
    StateEpoch state_epoch_;
    StopDispatchGeneration dispatch_generation_;
    std::atomic<bool> ingress_enabled_{false};
    std::atomic<bool> authoritative_overflow_{false};
    std::atomic<std::size_t> passive_drop_count_{0};
    std::atomic<std::size_t> native_inflight_{0};
    std::atomic_flag native_reader_active_ = ATOMIC_FLAG_INIT;
    std::atomic<const void*> native_snapshot_hazard_{nullptr};
    std::atomic<std::uint64_t>* ingress_notification_counter_ = nullptr;
    void* ingress_notifier_context_ = nullptr;
    StopPointIngressNotifier ingress_notifier_ = nullptr;
    std::atomic<bool> initialized_{false};
    bool replacing_state_ = false;
    bool stopping_ = false;
    bool stopped_ = false;
};

} // namespace savor::runtime
