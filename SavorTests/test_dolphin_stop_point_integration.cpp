#include <gtest/gtest.h>

#include "Runner/Runtime/DolphinWrapperBackend.h"
#include "Runner/Runtime/EmulationSession.h"
#include "../SavorProbe/NativeStopHooks.h"
#include "serial_guard.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace savor::runtime;

constexpr std::uint32_t kGameModeControllerPc = 0x801dc288u;
constexpr auto kLiveStopDeadline = 30s;

const std::filesystem::path kIso =
    R"(D:\SoATAS\SkiesofArcadiaLegends(USA).gcm)";
const std::filesystem::path kDolphinBase =
    R"(D:\SoATAS\dolphin-2506a-x64)";

constexpr StopSourceId kObserveSource{7102};
constexpr StopSubscriptionGroupId kObserveGroup{7202};
constexpr StopSubscriptionId kObserveSubscription{7302};
constexpr StopSourceId kWakeSource{7103};
constexpr StopSubscriptionGroupId kWakeGroup{7203};
constexpr StopSubscriptionId kWakeSubscription{7303};

class ScopedTemporaryDirectory
{
public:
    explicit ScopedTemporaryDirectory(std::string_view label)
    {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path() /
            ("savor_slice2_" + std::string(label) + "_" +
             std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    ScopedTemporaryDirectory(const ScopedTemporaryDirectory&) = delete;
    ScopedTemporaryDirectory& operator=(
        const ScopedTemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class RecordingStopConsumer final : public IStopPointConsumer
{
public:
    void OnStopPoint(const StopDelivery& delivery) override
    {
        deliveries.push_back(delivery);
    }

    std::vector<StopDelivery> deliveries;
};

class IngressSignal final
{
public:
    static void Notify(void* context) noexcept
    {
        static_cast<IngressSignal*>(context)->changed_.notify_one();
    }

    [[nodiscard]] bool WaitForChange(
        const std::atomic<std::uint64_t>& counter,
        std::uint64_t observed,
        std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_until(
            lock,
            deadline,
            [&counter, observed] {
                return counter.load(std::memory_order_acquire) != observed;
            });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
};

[[nodiscard]] StopSubscriptionGroupDefinition MakePcGroup(
    StopSubscriptionGroupId group_id,
    StopSourceId source_id,
    StopSubscriptionId subscription_id,
    StopDeliveryMode delivery,
    bool lossless,
    IStopPointConsumer& consumer)
{
    return {
        .id = group_id,
        .source = {
            .id = source_id,
            .stable_name = delivery == StopDeliveryMode::Wake
                ? "integration.game_mode_controller.wake"
                : "integration.game_mode_controller.observe",
            .diagnostic_label = delivery == StopDeliveryMode::Wake
                ? "live JIT router wake guard"
                : "live JIT router passive guard",
        },
        .epoch_policy = StopEpochPolicy::RebindAfterRestore,
        .subscriptions = {{
            .id = subscription_id,
            .point = PcStopPointSpec{kGameModeControllerPc},
            .delivery = delivery,
            .policy = StopRoutingPolicy::Pass,
            .lossless = lossless,
            .suppress_immediate_reentry =
                delivery == StopDeliveryMode::Wake,
            .consumer = &consumer,
        }},
    };
}

[[nodiscard]] const StopRouteReceipt* FindWake(
    const std::vector<StopRouteReceipt>& receipts,
    StateEpoch expected_epoch)
{
    for (const StopRouteReceipt& receipt : receipts)
    {
        if (receipt.terminal != StopRouteTerminal::WokeForeground ||
            receipt.identity.state_epoch != expected_epoch ||
            !receipt.event)
        {
            continue;
        }
        const auto* routed_pc =
            std::get_if<PcStopPointSpec>(&receipt.event->evidence.point);
        if (routed_pc && routed_pc->pc == kGameModeControllerPc)
            return &receipt;
    }
    return nullptr;
}

template <typename Predicate>
[[nodiscard]] bool DrainUntil(
    EmulationSession& session,
    std::atomic<std::uint64_t>& notification_counter,
    IngressSignal& signal,
    std::vector<StopRouteReceipt>& receipts,
    Predicate&& complete,
    std::chrono::steady_clock::duration timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        const std::uint64_t observed =
            notification_counter.load(std::memory_order_acquire);
        auto drained = session.DrainStopPointEvents();
        receipts.insert(
            receipts.end(),
            std::make_move_iterator(drained.begin()),
            std::make_move_iterator(drained.end()));
        if (complete(receipts))
            return true;

        if (notification_counter.load(std::memory_order_acquire) != observed)
            continue;

        if (std::chrono::steady_clock::now() >= deadline ||
            !signal.WaitForChange(
                notification_counter,
                observed,
                deadline))
        {
            return false;
        }
    }
}

void ExpectSameIdentity(
    const RoutedStopIdentity& lhs,
    const RoutedStopIdentity& rhs)
{
    EXPECT_EQ(lhs.sequence, rhs.sequence);
    EXPECT_EQ(lhs.sample_snapshot, rhs.sample_snapshot);
    EXPECT_EQ(lhs.state_epoch, rhs.state_epoch);
    EXPECT_EQ(lhs.dispatch_generation, rhs.dispatch_generation);
    EXPECT_EQ(lhs.physical_generation, rhs.physical_generation);
}

TEST(
    RealDolphinStopPointIntegration,
    JitRouterWakeStopsAtRecurringGameModeController)
{
    tests::SerialGuard serial;

    ASSERT_TRUE(std::filesystem::is_regular_file(kIso))
        << kIso.string();
    ASSERT_TRUE(std::filesystem::is_directory(kDolphinBase))
        << kDolphinBase.string();
    ASSERT_FALSE(savor::probe::NativeStopHooksInstalled());
    ASSERT_EQ(savor::probe::BoundNativeStopSink(), nullptr);

    ScopedTemporaryDirectory temporary("jit_router_guard");
    std::atomic<std::uint64_t> notification_counter{0};
    IngressSignal ingress_signal;
    auto backend = std::make_unique<DolphinWrapperBackend>(
        DolphinBackendCpuCore::Jit64);
    EmulationSession session(
        SessionId(0x7101u),
        std::move(backend));
    ASSERT_TRUE(session.ConfigureStopPointIngressNotification(
        &notification_counter,
        &ingress_signal,
        &IngressSignal::Notify));

    SessionOpenOptions open_options;
    open_options.backend.runtime_root =
        temporary.path() / "runtime";
    open_options.backend.user_directory =
        temporary.path() / "user";
    open_options.backend.dolphin_base_directory = kDolphinBase;
    open_options.backend.iso_path = kIso;
    open_options.backend.force_resync_from_base = true;
    open_options.backend.visual = false;
    open_options.screenshot_directory =
        temporary.path() / "screenshots";

    const SessionOperationReceipt opened = session.Open(open_options);
    ASSERT_TRUE(opened.ok) << opened.backend.message;
    ASSERT_EQ(opened.origin_epoch, StateEpoch{});
    ASSERT_EQ(opened.resulting_epoch, StateEpoch(1));
    ASSERT_TRUE(savor::probe::NativeStopHooksInstalled());

    StopPointRouter* const router = session.stop_points();
    ASSERT_NE(router, nullptr);
    ASSERT_EQ(
        savor::probe::BoundNativeStopSink(),
        static_cast<savor::probe::INativeStopSink*>(router));
    ASSERT_TRUE(router->DesiredPhysicalPlan().pcs.empty());
    ASSERT_TRUE(router->DesiredPhysicalPlan().memory.empty());

    if (session.snapshot().core_state == BackendCoreState::Running)
    {
        const SessionOperationReceipt paused = session.Pause(5s);
        ASSERT_TRUE(paused.ok) << paused.backend.message;
    }
    ASSERT_EQ(session.snapshot().core_state, BackendCoreState::Paused);

    // Compile and execute the recurring game loop before installing the
    // physical PC. A later hit therefore exercises Dolphin's ordinary
    // address-specific JIT invalidation path.
    const SessionOperationReceipt stepped = session.StepFrame(10s);
    ASSERT_TRUE(stepped.ok) << stepped.backend.message;
    ASSERT_EQ(session.snapshot().core_state, BackendCoreState::Paused);

    RecordingStopConsumer observe_consumer;
    auto observe_registration = router->RegisterGroup(MakePcGroup(
        kObserveGroup,
        kObserveSource,
        kObserveSubscription,
        StopDeliveryMode::Observe,
        false,
        observe_consumer));
    ASSERT_TRUE(observe_registration.receipt.ok)
        << observe_registration.receipt.error.message;

    RecordingStopConsumer wake_consumer;
    auto wake_registration = router->RegisterGroup(MakePcGroup(
        kWakeGroup,
        kWakeSource,
        kWakeSubscription,
        StopDeliveryMode::Wake,
        true,
        wake_consumer));
    ASSERT_TRUE(wake_registration.receipt.ok)
        << wake_registration.receipt.error.message;

    const PhysicalStopPointPlan armed_plan =
        router->DesiredPhysicalPlan();
    ASSERT_EQ(armed_plan.pcs.size(), 1u);
    EXPECT_EQ(armed_plan.pcs.front().pc, kGameModeControllerPc);
    EXPECT_TRUE(armed_plan.memory.empty());

    const SessionOperationReceipt resumed = session.Resume();
    ASSERT_TRUE(resumed.ok) << resumed.backend.message;

    std::vector<StopRouteReceipt> routed;
    const bool reached_target = DrainUntil(
        session,
        notification_counter,
        ingress_signal,
        routed,
        [](const std::vector<StopRouteReceipt>& receipts) {
            return FindWake(receipts, StateEpoch(1)) != nullptr;
        },
        kLiveStopDeadline);
    ASSERT_TRUE(reached_target)
        << "Timed out after 30 seconds waiting for the recurring JIT/router "
           "guard at 0x801DC288; routed_receipts="
        << routed.size();

    const StopRouteReceipt* const wake =
        FindWake(routed, StateEpoch(1));
    ASSERT_NE(wake, nullptr);
    ASSERT_TRUE(wake->event.has_value());
    const RoutedStopEvent& event = *wake->event;
    const auto* const routed_pc =
        std::get_if<PcStopPointSpec>(&event.evidence.point);
    ASSERT_NE(routed_pc, nullptr);
    EXPECT_EQ(routed_pc->pc, kGameModeControllerPc);
    EXPECT_EQ(event.evidence.hit_pc, kGameModeControllerPc);
    EXPECT_EQ(event.evidence.path, NativeStopPath::Jit);
    EXPECT_EQ(event.identity.state_epoch, session.snapshot().state_epoch);
    EXPECT_TRUE(event.active_foreground_wake);
    EXPECT_TRUE(event.authoritative);
    EXPECT_FALSE(router->authoritative_overflowed());
    EXPECT_EQ(router->passive_drop_count(), 0u);

    ASSERT_EQ(wake->deliveries.size(), 2u);
    EXPECT_EQ(
        wake->deliveries[0].delivery,
        StopDeliveryMode::Observe);
    EXPECT_EQ(
        wake->deliveries[1].delivery,
        StopDeliveryMode::Wake);
    ExpectSameIdentity(
        wake->deliveries[0].event.identity,
        wake->deliveries[1].event.identity);
    ExpectSameIdentity(
        event.identity,
        wake->deliveries[0].event.identity);

    ASSERT_EQ(observe_consumer.deliveries.size(), 1u);
    ASSERT_EQ(wake_consumer.deliveries.size(), 1u);
    ExpectSameIdentity(
        observe_consumer.deliveries.front().event.identity,
        wake_consumer.deliveries.front().event.identity);

    const SessionOperationReceipt paused = session.Pause(5s);
    ASSERT_TRUE(paused.ok) << paused.backend.message;
    EXPECT_EQ(session.snapshot().core_state, BackendCoreState::Paused);

    EXPECT_TRUE(observe_registration.handle.Release().ok);
    const PhysicalStopPointPlan wake_only_plan =
        router->DesiredPhysicalPlan();
    ASSERT_EQ(wake_only_plan.pcs.size(), 1u);
    EXPECT_EQ(wake_only_plan.pcs.front().pc, kGameModeControllerPc);
    EXPECT_TRUE(wake_only_plan.memory.empty());

    EXPECT_TRUE(wake_registration.handle.Release().ok);
    EXPECT_TRUE(router->DesiredPhysicalPlan().pcs.empty());
    EXPECT_TRUE(router->DesiredPhysicalPlan().memory.empty());

    const SessionOperationReceipt shutdown = session.Shutdown();
    ASSERT_TRUE(shutdown.ok) << shutdown.backend.message;
    EXPECT_FALSE(savor::probe::NativeStopHooksInstalled());
    EXPECT_EQ(savor::probe::BoundNativeStopSink(), nullptr);
}

} // namespace
