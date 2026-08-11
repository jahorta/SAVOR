#include <gtest/gtest.h>

#include "Runner/Runtime/StopPoints/StopPointRouter.h"
#include "common/FakePhysicalStopBackend.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <future>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace savor::runtime;
using namespace savor::test_support;

class RecordingStopConsumer final : public IStopPointConsumer
{
public:
    void OnStopPoint(const StopDelivery& delivery) override
    {
        deliveries.push_back(delivery);
    }

    std::vector<StopDelivery> deliveries;
};

class RejectingCpuEvaluator final : public IStopPointCpuEvaluator
{
public:
    bool Qualify(
        std::uint32_t,
        const StopPointCpuContext&) noexcept override
    {
        return false;
    }

    RoutedHitSample Sample(
        std::uint32_t descriptor_id,
        const StopPointCpuContext&) noexcept override
    {
        return {descriptor_id, 0, false};
    }
};

class ThreadRecordingCpuEvaluator final : public IStopPointCpuEvaluator
{
public:
    bool Qualify(
        std::uint32_t,
        const StopPointCpuContext&) noexcept override
    {
        qualification_thread = std::this_thread::get_id();
        return true;
    }

    RoutedHitSample Sample(
        std::uint32_t descriptor_id,
        const StopPointCpuContext&) noexcept override
    {
        sample_thread = std::this_thread::get_id();
        return {descriptor_id, 42, true};
    }

    std::thread::id qualification_thread;
    std::thread::id sample_thread;
};

class ThreadRecordingConsumer final : public IStopPointConsumer
{
public:
    void OnStopPoint(const StopDelivery&) override
    {
        delivery_thread = std::this_thread::get_id();
        ++delivery_count;
    }

    std::thread::id delivery_thread;
    std::size_t delivery_count = 0;
};

class RecordingCpuObserver final : public IStopPointCpuObserver
{
public:
    RecordingCpuObserver()
    {
        descriptors.reserve(4);
        events.reserve(4);
    }

    StopCpuObservationResult ObserveRoutedHit(
        std::uint32_t descriptor_id,
        const RoutedStopEvent& event) noexcept override
    {
        descriptors.push_back(descriptor_id);
        events.push_back(event);
        return result;
    }

    StopCpuObservationResult result = StopCpuObservationResult::Observed;
    std::vector<std::uint32_t> descriptors;
    std::vector<RoutedStopEvent> events;
};

void CountRawNotification(void* context) noexcept
{
    static_cast<std::atomic<std::uint64_t>*>(context)->fetch_add(
        1,
        std::memory_order_relaxed);
}

StopSubscriptionDefinition PcSubscription(
    std::uint64_t id,
    std::uint32_t pc,
    IStopPointConsumer& consumer,
    StopSubscriptionRoute route = PassiveStopObservation{},
    std::int32_t priority = 0)
{
    return {
        .id = StopSubscriptionId(id),
        .point = PcStopPointSpec{pc},
        .route = std::move(route),
        .priority = priority,
        .consumer = &consumer,
    };
}

StopSubscriptionDefinition MemorySubscription(
    std::uint64_t id,
    std::uint32_t address,
    std::uint32_t size,
    StopMemoryAccess access,
    IStopPointConsumer& consumer)
{
    return {
        .id = StopSubscriptionId(id),
        .point = MemoryStopPointSpec{address, size, access},
        .consumer = &consumer,
    };
}

StopSubscriptionGroupDefinition Group(
    std::uint64_t id,
    std::vector<StopSubscriptionDefinition> subscriptions)
{
    return {
        .id = StopSubscriptionGroupId(id),
        .source = {
            .id = StopSourceId(id),
            .stable_name = "test.source." + std::to_string(id),
            .diagnostic_label = "test",
        },
        .subscriptions = std::move(subscriptions),
    };
}

std::size_t CountCalls(
    const std::vector<FakePhysicalStopCall>& calls,
    FakePhysicalStopOperation operation)
{
    return static_cast<std::size_t>(std::count_if(
        calls.begin(),
        calls.end(),
        [&](const FakePhysicalStopCall& call) {
            return call.operation == operation;
        }));
}

class StopPointRouterFixture : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(router.Initialize(WorksetEpoch(1)).ok);
    }

    void TearDown() override
    {
        const StopPointLifecycleReceipt cleanup =
            router.StopIngressDrainAndCleanup();
        EXPECT_TRUE(cleanup.ok) << cleanup.error.message;
    }

    std::shared_ptr<FakePhysicalStopBackendControl> control =
        std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend{control};
    PhysicalStopPointManager manager{backend};
    StopPointRouter router{manager};
};

TEST(PhysicalStopPointManager, AppliesExactlySkipsNoopAndDetectsUnmanagedDrift)
{
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend(control);
    PhysicalStopPointManager manager(backend);
    const PhysicalStopPointPlan plan{
        .pcs = {{0x80001000u}},
    };

    int logical_commits = 0;
    const auto first = manager.ApplyExactPlan(
        plan,
        [&] { ++logical_commits; });
    ASSERT_TRUE(first.ok) << first.message;
    EXPECT_EQ(first.generation, PhysicalPlanGeneration(1));
    EXPECT_EQ(logical_commits, 1);

    const auto no_op = manager.ApplyExactPlan(
        plan,
        [&] { ++logical_commits; });
    ASSERT_TRUE(no_op.ok) << no_op.message;
    EXPECT_EQ(no_op.generation, PhysicalPlanGeneration(1));
    EXPECT_EQ(logical_commits, 2);
    EXPECT_EQ(
        CountCalls(control->Calls(), FakePhysicalStopOperation::Apply),
        1u);
    EXPECT_EQ(
        CountCalls(
            control->Calls(),
            FakePhysicalStopOperation::PublishUnchanged),
        1u);

    const auto unchanged = manager.ValidateExactPlanUnchanged();
    ASSERT_TRUE(unchanged.ok) << unchanged.message;
    EXPECT_EQ(manager.generation(), PhysicalPlanGeneration(1));

    control->SetQueryOutcome(FakePhysicalStopOutcome{
        .actual_override = PhysicalStopPointPlan{
            .pcs = {{0x80002000u}},
        },
    });
    const auto drift = manager.ValidateExactPlanUnchanged();
    EXPECT_FALSE(drift.ok);
    EXPECT_EQ(drift.integrity, PhysicalStopIntegrity::Unknown);
    EXPECT_TRUE(manager.integrity_unknown());
    EXPECT_EQ(manager.generation(), PhysicalPlanGeneration(1));
}

TEST(PhysicalStopPointManager, PublishesUnchangedLogicalSnapshotUnderCpuExclusion)
{
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend(control);
    PhysicalStopPointManager manager(backend);
    const PhysicalStopPointPlan plan{
        .pcs = {{0x80001000u}},
    };
    ASSERT_TRUE(manager.ApplyExactPlan(plan, {}).ok);

    auto operation_entered = std::make_shared<std::latch>(1);
    auto allow_operation = std::make_shared<std::latch>(1);
    auto commit_entered = std::make_shared<std::latch>(1);
    auto allow_commit = std::make_shared<std::latch>(1);
    control->SetPublishUnchangedGate(FakePhysicalStopOperationGate{
        .operation_entered = operation_entered,
        .allow_operation = allow_operation,
        .commit_entered = commit_entered,
        .allow_commit = allow_commit,
    });
    std::atomic<bool> committed{false};
    auto publication = std::async(std::launch::async, [&] {
        return manager.ApplyExactPlan(
            plan,
            [&] { committed.store(true, std::memory_order_release); });
    });

    operation_entered->wait();
    EXPECT_EQ(publication.wait_for(0ms), std::future_status::timeout);
    EXPECT_FALSE(committed.load(std::memory_order_acquire));
    allow_operation->count_down();
    commit_entered->wait();
    EXPECT_EQ(publication.wait_for(0ms), std::future_status::timeout);
    EXPECT_FALSE(committed.load(std::memory_order_acquire));
    allow_commit->count_down();

    const auto receipt = publication.get();
    EXPECT_TRUE(receipt.ok) << receipt.message;
    EXPECT_TRUE(committed.load(std::memory_order_acquire));
    EXPECT_EQ(receipt.generation, PhysicalPlanGeneration(1));
    EXPECT_EQ(
        CountCalls(control->Calls(), FakePhysicalStopOperation::Apply),
        1u);
    EXPECT_EQ(
        CountCalls(
            control->Calls(),
            FakePhysicalStopOperation::PublishUnchanged),
        1u);
}

TEST_F(StopPointRouterFixture, BuildsPcUnionAndCoalescesOverlappingMemory)
{
    RecordingStopConsumer consumer;
    auto registration = router.RegisterGroup(Group(
        1,
        {
            PcSubscription(1, 0x80001000u, consumer),
            PcSubscription(2, 0x80001000u, consumer),
            MemorySubscription(
                3,
                0x80300000u,
                8,
                StopMemoryAccess::Read,
                consumer),
            MemorySubscription(
                4,
                0x80300004u,
                8,
                StopMemoryAccess::Write,
                consumer),
        }));
    ASSERT_TRUE(registration.receipt.ok) << registration.receipt.error.message;

    const PhysicalStopPointPlan plan = router.DesiredPhysicalPlan();
    EXPECT_EQ(
        plan.pcs,
        std::vector<PhysicalPcStop>{{0x80001000u}});
    EXPECT_EQ(
        plan.memory,
        (std::vector<PhysicalMemoryStop>{{
            .start = 0x80300000u,
            .end = 0x8030000Bu,
            .read = true,
            .write = true,
        }}));
}

TEST_F(StopPointRouterFixture, RegistrationAndReplacementAreTransactional)
{
    RecordingStopConsumer consumer;
    const StopDispatchGeneration initial_generation =
        router.dispatch_generation();
    control->SetApplyOutcome(FakePhysicalStopOutcome{
        .ok = false,
        .message = "injected apply failure",
    });

    auto rejected = router.RegisterGroup(
        Group(1, {PcSubscription(1, 0x80001000u, consumer)}));
    EXPECT_FALSE(rejected.receipt.ok);
    EXPECT_EQ(
        rejected.receipt.error.code,
        StopPointErrorCode::PhysicalReconcileFailed);
    EXPECT_EQ(router.dispatch_generation(), initial_generation);
    EXPECT_TRUE(router.DesiredPhysicalPlan().pcs.empty());

    control->SetApplyOutcome({});
    auto accepted = router.RegisterGroup(
        Group(2, {PcSubscription(2, 0x80002000u, consumer)}));
    ASSERT_TRUE(accepted.receipt.ok) << accepted.receipt.error.message;
    const StopDispatchGeneration accepted_generation =
        router.dispatch_generation();

    control->SetApplyOutcome(FakePhysicalStopOutcome{
        .ok = false,
        .message = "injected replacement failure",
    });
    const StopGroupReceipt replacement = accepted.handle.Replace(
        Group(2, {PcSubscription(2, 0x80003000u, consumer)}));
    EXPECT_FALSE(replacement.ok);
    EXPECT_EQ(router.dispatch_generation(), accepted_generation);
    EXPECT_EQ(
        router.DesiredPhysicalPlan().pcs,
        std::vector<PhysicalPcStop>{{0x80002000u}});
}

TEST_F(StopPointRouterFixture, RecoverableReleaseFailureKeepsLeaseAndPlan)
{
    RecordingStopConsumer consumer;
    auto registration = router.RegisterGroup(
        Group(1, {PcSubscription(1, 0x80001000u, consumer)}));
    ASSERT_TRUE(registration.receipt.ok);
    const StopDispatchGeneration before = router.dispatch_generation();

    control->SetApplyOutcome(FakePhysicalStopOutcome{
        .ok = false,
        .message = "injected release failure",
    });
    const StopReleaseReceipt failed = registration.handle.Release();
    EXPECT_FALSE(failed.ok);
    EXPECT_TRUE(registration.handle.active());
    EXPECT_EQ(router.dispatch_generation(), before);
    EXPECT_EQ(
        router.DesiredPhysicalPlan().pcs,
        std::vector<PhysicalPcStop>{{0x80001000u}});

    control->SetApplyOutcome({});
    const StopReleaseReceipt released = registration.handle.Release();
    EXPECT_TRUE(released.ok);
    EXPECT_FALSE(registration.handle.active());
    EXPECT_TRUE(router.DesiredPhysicalPlan().pcs.empty());
}

TEST_F(StopPointRouterFixture, RoutesByPriorityThenRegistrationOrder)
{
    RecordingStopConsumer consumer;
    auto high_priority_observer = router.RegisterGroup(Group(
        1,
        {PcSubscription(
            1,
            0x80001000u,
            consumer,
            PassiveStopObservation{},
            50)}));
    auto earlier_observer = router.RegisterGroup(Group(
        2,
        {PcSubscription(
            2,
            0x80001000u,
            consumer,
            PassiveStopObservation{},
            5)}));
    auto later_observe = router.RegisterGroup(Group(
        3,
        {PcSubscription(
            3,
            0x80001000u,
            consumer,
            PassiveStopObservation{},
            5)}));
    auto wake = router.RegisterGroup(Group(
        4,
        {PcSubscription(
            4,
            0x80001000u,
            consumer,
            ForegroundStopWait{})}));
    ASSERT_TRUE(high_priority_observer.receipt.ok);
    ASSERT_TRUE(earlier_observer.receipt.ok);
    ASSERT_TRUE(later_observe.receipt.ok);
    ASSERT_TRUE(wake.receipt.ok);

    const auto decision =
        backend.InjectJitPcStop(0x80001000u);
    EXPECT_TRUE(decision.request_break);
    const auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    EXPECT_EQ(receipts[0].terminal, StopRouteTerminal::ForegroundMatched);
    ASSERT_EQ(receipts[0].deliveries.size(), 4u);
    EXPECT_EQ(
        receipts[0].deliveries[0].subscription_id,
        StopSubscriptionId(1));
    EXPECT_EQ(
        receipts[0].deliveries[1].subscription_id,
        StopSubscriptionId(2));
    EXPECT_EQ(
        receipts[0].deliveries[2].subscription_id,
        StopSubscriptionId(3));
    EXPECT_EQ(
        receipts[0].deliveries[3].subscription_id,
        StopSubscriptionId(4));
}

TEST_F(StopPointRouterFixture, NativeHitDuringRegistrationUsesWholeOldSnapshot)
{
    RecordingStopConsumer consumer;
    auto existing = router.RegisterGroup(
        Group(1, {PcSubscription(1, 0x80001000u, consumer)}));
    ASSERT_TRUE(existing.receipt.ok);

    auto operation_entered = std::make_shared<std::latch>(1);
    auto allow_operation = std::make_shared<std::latch>(1);
    control->SetApplyGate(FakePhysicalStopOperationGate{
        .operation_entered = operation_entered,
        .allow_operation = allow_operation,
    });
    std::thread native([&] {
        operation_entered->wait();
        (void)backend.InjectJitPcStop(0x80001000u);
        allow_operation->count_down();
    });
    auto added = router.RegisterGroup(
        Group(2, {PcSubscription(2, 0x80002000u, consumer)}));
    native.join();
    ASSERT_TRUE(added.receipt.ok) << added.receipt.error.message;

    const auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    EXPECT_EQ(receipts[0].terminal, StopRouteTerminal::Stale);
    EXPECT_TRUE(consumer.deliveries.empty());
}

TEST_F(StopPointRouterFixture, NativeHitDuringReleaseCannotUsePartialSnapshot)
{
    RecordingStopConsumer consumer;
    auto registration = router.RegisterGroup(
        Group(1, {PcSubscription(1, 0x80001000u, consumer)}));
    ASSERT_TRUE(registration.receipt.ok);

    auto operation_entered = std::make_shared<std::latch>(1);
    auto allow_operation = std::make_shared<std::latch>(1);
    control->SetApplyGate(FakePhysicalStopOperationGate{
        .operation_entered = operation_entered,
        .allow_operation = allow_operation,
    });
    std::thread native([&] {
        operation_entered->wait();
        (void)backend.InjectJitPcStop(0x80001000u);
        allow_operation->count_down();
    });
    const StopReleaseReceipt released = registration.handle.Release();
    native.join();
    ASSERT_TRUE(released.ok) << released.error.message;

    const auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    EXPECT_EQ(receipts[0].terminal, StopRouteTerminal::Stale);
    EXPECT_TRUE(consumer.deliveries.empty());
}

TEST_F(StopPointRouterFixture, EnforcesOneForegroundWaitGroup)
{
    RecordingStopConsumer consumer;
    auto first = router.RegisterGroup(Group(
        1,
        {PcSubscription(
            1,
            0x80001000u,
            consumer,
            ForegroundStopWait{})}));
    ASSERT_TRUE(first.receipt.ok);

    auto second = router.RegisterGroup(Group(
        2,
        {PcSubscription(
            2,
            0x80002000u,
            consumer,
            ForegroundStopWait{})}));
    EXPECT_FALSE(second.receipt.ok);
    EXPECT_EQ(
        second.receipt.error.code,
        StopPointErrorCode::ForegroundWaitAlreadyRegistered);
}

TEST_F(
    StopPointRouterFixture,
    RequestInterruptionHandlerLeavesCoreStoppedForActorResolution)
{
    RecordingStopConsumer consumer;
    auto handler_request = PcSubscription(
        1,
        0x80001000u,
        consumer,
        TrustedStopInterruptionRequest{
            .handler_key = "dismiss-text-box"});
    auto registration =
        router.RegisterGroup(Group(1, {std::move(handler_request)}));
    ASSERT_TRUE(registration.receipt.ok);

    const auto decision = backend.InjectJitPcStop(0x80001000u);
    EXPECT_TRUE(decision.request_break);
    const auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    EXPECT_EQ(
        receipts[0].terminal,
        StopRouteTerminal::InterruptionRequested);
    EXPECT_TRUE(receipts[0].core_must_remain_stopped);
    ASSERT_TRUE(receipts[0].interruption_handler_request.has_value());
    EXPECT_EQ(
        receipts[0]
            .interruption_handler_request
            ->interruption_handler_key,
        "dismiss-text-box");
}

TEST(StopPointRouter, PublishesQualifiedMissAsTypedUnclaimedEvidence)
{
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend(control);
    PhysicalStopPointManager manager(backend);
    RejectingCpuEvaluator evaluator;
    StopPointRouter router(manager, &evaluator);
    ASSERT_TRUE(router.Initialize(WorksetEpoch(1)).ok);

    RecordingStopConsumer consumer;
    auto definition = PcSubscription(1, 0x80001000u, consumer);
    definition.qualification_id = 7;
    auto registration =
        router.RegisterGroup(Group(1, {std::move(definition)}));
    ASSERT_TRUE(registration.receipt.ok);

    const auto decision =
        backend.InjectJitPcStop(0x80001000u);
    EXPECT_FALSE(decision.request_break);
    const auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    EXPECT_EQ(receipts[0].terminal, StopRouteTerminal::Unclaimed);
    EXPECT_TRUE(receipts[0].deliveries.empty());
    ASSERT_TRUE(receipts[0].event.has_value());
    EXPECT_EQ(
        std::get<PcStopPointSpec>(
            receipts[0].event->evidence.point).pc,
        0x80001000u);
    EXPECT_TRUE(router.StopIngressDrainAndCleanup().ok);
}

TEST_F(StopPointRouterFixture, IgnoresNativeCallsOutsidePhysicalUnionBeforeSequencing)
{
    RecordingStopConsumer consumer;
    auto registration = router.RegisterGroup(
        Group(1, {PcSubscription(1, 0x80001000u, consumer)}));
    ASSERT_TRUE(registration.receipt.ok);

    EXPECT_FALSE(
        backend.InjectJitPcStop(0x80002000u).request_break);
    EXPECT_TRUE(router.DrainIngress().empty());

    (void)backend.InjectJitPcStop(0x80001000u);
    const auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    EXPECT_EQ(receipts[0].identity.sequence, RoutedStopSequence(1));
}

TEST_F(
    StopPointRouterFixture,
    AcceptsMoreThanPerHitCapacityWhenLogicalSitesAreDispersed)
{
    constexpr std::size_t kSubscriptionCount =
        kMaxStopDeliveriesPerHit + 9;
    constexpr std::uint32_t kFirstPc = 0x80100000u;

    RecordingStopConsumer consumer;
    std::vector<StopSubscriptionDefinition> subscriptions;
    subscriptions.reserve(kSubscriptionCount);
    for (std::size_t i = 0; i < kSubscriptionCount; ++i)
    {
        subscriptions.push_back(PcSubscription(
            1000 + i,
            kFirstPc + static_cast<std::uint32_t>(i * 4),
            consumer));
    }

    auto registration =
        router.RegisterGroup(Group(100, std::move(subscriptions)));
    ASSERT_TRUE(registration.receipt.ok)
        << registration.receipt.error.message;
    ASSERT_EQ(
        router.DesiredPhysicalPlan().pcs.size(),
        kSubscriptionCount);

    const std::uint32_t last_pc =
        kFirstPc +
        static_cast<std::uint32_t>((kSubscriptionCount - 1) * 4);
    EXPECT_FALSE(backend.InjectJitPcStop(last_pc).request_break);
    const auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    ASSERT_EQ(receipts[0].deliveries.size(), 1u);
    EXPECT_EQ(
        std::get<PcStopPointSpec>(
            receipts[0].deliveries[0].event.evidence.point).pc,
        last_pc);
    EXPECT_EQ(consumer.deliveries.size(), 1u);
}

TEST_F(
    StopPointRouterFixture,
    RejectsCandidateWhoseSingleHitExceedsDeliveryCapacityAtomically)
{
    constexpr std::size_t kSubscriptionCount =
        kMaxStopDeliveriesPerHit + 1;
    constexpr std::uint32_t kSharedPc = 0x80110000u;
    constexpr std::uint32_t kExistingPc = 0x80120000u;

    RecordingStopConsumer consumer;
    auto existing = router.RegisterGroup(
        Group(
            100,
            {PcSubscription(100, kExistingPc, consumer)}));
    ASSERT_TRUE(existing.receipt.ok)
        << existing.receipt.error.message;
    const StopDispatchGeneration generation_before =
        router.dispatch_generation();
    const PhysicalStopPointPlan plan_before =
        router.DesiredPhysicalPlan();
    const std::size_t applies_before = CountCalls(
        control->Calls(),
        FakePhysicalStopOperation::Apply);

    std::vector<StopSubscriptionDefinition> subscriptions;
    subscriptions.reserve(kSubscriptionCount);
    for (std::size_t i = 0; i < kSubscriptionCount; ++i)
    {
        subscriptions.push_back(PcSubscription(
            1000 + i,
            kSharedPc,
            consumer));
    }

    auto rejected =
        router.RegisterGroup(Group(200, std::move(subscriptions)));
    EXPECT_FALSE(rejected.receipt.ok);
    EXPECT_EQ(
        rejected.receipt.error.code,
        StopPointErrorCode::InvalidArgument);
    EXPECT_NE(
        rejected.receipt.error.message.find(
            "delivery capacity"),
        std::string::npos);
    EXPECT_EQ(router.dispatch_generation(), generation_before);
    EXPECT_EQ(router.DesiredPhysicalPlan(), plan_before);
    EXPECT_EQ(
        CountCalls(
            control->Calls(),
            FakePhysicalStopOperation::Apply),
        applies_before);

    EXPECT_FALSE(backend.InjectJitPcStop(kSharedPc).request_break);
    EXPECT_TRUE(router.DrainIngress().empty());
    EXPECT_TRUE(consumer.deliveries.empty());
}

TEST(StopPointRouter, InvokesTrustedCpuObserverOnceAfterFinalWakeSelection)
{
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend(control);
    PhysicalStopPointManager manager(backend);
    RecordingCpuObserver observer;
    observer.result =
        StopCpuObservationResult::ObservedRequiresReconcile;
    StopPointRouter router(manager, nullptr, &observer);
    ASSERT_TRUE(router.Initialize(WorksetEpoch(1)).ok);

    RecordingStopConsumer consumer;
    auto passive = PcSubscription(1, 0x80001000u, consumer);
    passive.route = PassiveStopObservation{
        .cpu_observer_descriptor_id = 77,
        .lossless = false};
    auto wake = PcSubscription(
        2,
        0x80001000u,
        consumer,
        ForegroundStopWait{});
    auto registration = router.RegisterGroup(
        Group(1, {std::move(passive), std::move(wake)}));
    ASSERT_TRUE(registration.receipt.ok);

    EXPECT_TRUE(
        backend.InjectJitPcStop(0x80001000u).request_break);
    ASSERT_EQ(observer.descriptors.size(), 1u);
    EXPECT_EQ(observer.descriptors[0], 77u);
    ASSERT_EQ(observer.events.size(), 1u);
    EXPECT_TRUE(observer.events[0].active_foreground_wait);
    const auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    ASSERT_TRUE(receipts[0].event.has_value());
    EXPECT_TRUE(
        receipts[0].event
            ->requires_physical_reconcile_before_resume);
    EXPECT_TRUE(receipts[0].core_must_remain_stopped);
    EXPECT_TRUE(router.StopIngressDrainAndCleanup().ok);
}

TEST(
    StopPointRouter,
    EmptyIngressDrainDoesNotCreateHostActivityButDeliveryDoes)
{
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend(control);
    PhysicalStopPointManager manager(backend);
    HostActivityTracker host_activity;
    StopPointRouter router(manager, nullptr, nullptr, &host_activity);
    ASSERT_TRUE(router.Initialize(WorksetEpoch(1)).ok);

    const HostActivityTracker::Snapshot before_empty =
        host_activity.snapshot();
    EXPECT_TRUE(router.DrainIngress().empty());
    const HostActivityTracker::Snapshot after_empty =
        host_activity.snapshot();
    EXPECT_EQ(after_empty.generation, before_empty.generation);
    EXPECT_EQ(after_empty.in_flight, 0u);

    RecordingStopConsumer consumer;
    auto registration = router.RegisterGroup(
        Group(1, {PcSubscription(1, 0x80001000u, consumer)}));
    ASSERT_TRUE(registration.receipt.ok);
    ASSERT_FALSE(
        backend.InjectJitPcStop(0x80001000u).request_break);

    const HostActivityTracker::Snapshot before_delivery =
        host_activity.snapshot();
    const auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    EXPECT_EQ(consumer.deliveries.size(), 1u);
    const HostActivityTracker::Snapshot after_delivery =
        host_activity.snapshot();
    EXPECT_GT(after_delivery.generation, before_delivery.generation);
    EXPECT_EQ(after_delivery.in_flight, 0u);

    EXPECT_TRUE(router.StopIngressDrainAndCleanup().ok);
}

TEST(StopPointRouter, CpuObserverFailureFailsClosedBeforeActorDelivery)
{
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend(control);
    PhysicalStopPointManager manager(backend);
    RecordingCpuObserver observer;
    observer.result = StopCpuObservationResult::Failed;
    StopPointRouter router(manager, nullptr, &observer);
    ASSERT_TRUE(router.Initialize(WorksetEpoch(1)).ok);

    RecordingStopConsumer consumer;
    auto definition = PcSubscription(1, 0x80001000u, consumer);
    definition.route = PassiveStopObservation{
        .cpu_observer_descriptor_id = 88,
        .lossless = false};
    auto registration =
        router.RegisterGroup(Group(1, {std::move(definition)}));
    ASSERT_TRUE(registration.receipt.ok);

    EXPECT_TRUE(
        backend.InjectJitPcStop(0x80001000u).request_break);
    const auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    EXPECT_EQ(receipts[0].terminal, StopRouteTerminal::RoutingFailure);
    EXPECT_TRUE(receipts[0].core_must_remain_stopped);
    EXPECT_TRUE(router.StopIngressDrainAndCleanup().ok);
}

TEST(StopPointRouter, CpuEvaluationAndActorDeliveryStayOnOwnedThreads)
{
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend(control);
    PhysicalStopPointManager manager(backend);
    ThreadRecordingCpuEvaluator evaluator;
    StopPointRouter router(manager, &evaluator);
    ASSERT_TRUE(router.Initialize(WorksetEpoch(1)).ok);

    ThreadRecordingConsumer consumer;
    auto definition = PcSubscription(1, 0x80001000u, consumer);
    definition.qualification_id = 1;
    definition.sample_descriptor_ids = {2};
    auto registration =
        router.RegisterGroup(Group(1, {std::move(definition)}));
    ASSERT_TRUE(registration.receipt.ok);

    std::thread::id native_thread;
    std::thread native([&] {
        native_thread = std::this_thread::get_id();
        (void)backend.InjectJitPcStop(0x80001000u);
    });
    native.join();
    EXPECT_EQ(consumer.delivery_count, 0u);
    EXPECT_EQ(evaluator.qualification_thread, native_thread);
    EXPECT_EQ(evaluator.sample_thread, native_thread);

    ASSERT_EQ(router.DrainIngress().size(), 1u);
    EXPECT_EQ(consumer.delivery_count, 1u);
    EXPECT_EQ(consumer.delivery_thread, std::this_thread::get_id());
    EXPECT_TRUE(router.StopIngressDrainAndCleanup().ok);
}

TEST(StopPointRouter, PublishesIngressNotificationWithoutCallingActorCode)
{
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend(control);
    PhysicalStopPointManager manager(backend);
    StopPointRouter router(manager);
    std::atomic<std::uint64_t> notification{0};
    std::atomic<std::uint64_t> raw_notification{0};
    ASSERT_FALSE(router.SetIngressNotificationCounter(&notification));
    ASSERT_FALSE(router.SetIngressNotifier(
        &raw_notification,
        &CountRawNotification));
    ASSERT_TRUE(router.Initialize(WorksetEpoch(1)).ok);

    RecordingStopConsumer consumer;
    auto registration = router.RegisterGroup(
        Group(1, {PcSubscription(1, 0x80001000u, consumer)}));
    ASSERT_TRUE(registration.receipt.ok);
    EXPECT_EQ(
        router.SetIngressNotificationCounter(nullptr).code,
        StopPointErrorCode::InvalidArgument);

    EXPECT_FALSE(
        backend.InjectJitPcStop(0x80001000u).request_break);
    EXPECT_EQ(notification.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(raw_notification.load(std::memory_order_acquire), 1u);
    ASSERT_EQ(router.DrainIngress().size(), 1u);

    ASSERT_TRUE(router.StopIngressDrainAndCleanup().ok);
    EXPECT_FALSE(
        backend.InjectJitPcStop(0x80001000u).request_break);
    EXPECT_EQ(notification.load(std::memory_order_acquire), 1u);
}

TEST_F(StopPointRouterFixture, AuthoritativeOverflowUsesEmergencySlotAndFailsClosed)
{
    RecordingStopConsumer consumer;
    auto registration = router.RegisterGroup(Group(
        1,
        {PcSubscription(
            1,
            0x80001000u,
            consumer,
            ForegroundStopWait{})}));
    ASSERT_TRUE(registration.receipt.ok);

    savor::probe::NativeStopDecision overflow;
    for (std::size_t i = 0;
        i < kStopPointNativeIngressCapacity + 1;
        ++i)
    {
        overflow = backend.InjectJitPcStop(0x80001000u);
    }
    EXPECT_TRUE(overflow.request_break);
    EXPECT_TRUE(overflow.authoritative_overflow);
    EXPECT_TRUE(router.authoritative_overflowed());
    EXPECT_FALSE(router.ingress_enabled());

    const auto receipts = router.DrainIngress();
    ASSERT_EQ(
        receipts.size(),
        kStopPointNativeIngressCapacity + 1);
    EXPECT_EQ(receipts.back().terminal, StopRouteTerminal::Overflow);
    EXPECT_EQ(
        receipts.back().error.code,
        StopPointErrorCode::IngressOverflow);
}

TEST_F(StopPointRouterFixture, PassiveOverflowDropsWithoutDisablingIngress)
{
    RecordingStopConsumer consumer;
    auto registration = router.RegisterGroup(
        Group(1, {PcSubscription(1, 0x80001000u, consumer)}));
    ASSERT_TRUE(registration.receipt.ok);

    for (std::size_t i = 0; i < kStopPointNativeIngressCapacity + 10; ++i)
        (void)backend.InjectJitPcStop(0x80001000u);

    EXPECT_TRUE(router.ingress_enabled());
    EXPECT_EQ(router.passive_drop_count(), 10u);
    EXPECT_EQ(
        router.DrainIngress().size(),
        kStopPointNativeIngressCapacity);
}

TEST_F(StopPointRouterFixture, AccountsPassiveDropsPerQualifiedSource)
{
    RecordingStopConsumer consumer;
    auto first = router.RegisterGroup(
        Group(10, {PcSubscription(10, 0x80001000u, consumer)}));
    auto second = router.RegisterGroup(
        Group(20, {PcSubscription(20, 0x80002000u, consumer)}));
    ASSERT_TRUE(first.receipt.ok);
    ASSERT_TRUE(second.receipt.ok);

    for (std::size_t i = 0; i < kStopPointNativeIngressCapacity; ++i)
        (void)backend.InjectJitPcStop(0x80001000u);
    (void)backend.InjectJitPcStop(0x80001000u);
    (void)backend.InjectJitPcStop(0x80002000u);

    const auto diagnostics = router.PassiveDropDiagnostics();
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(diagnostics[0].source_id, StopSourceId(10));
    EXPECT_EQ(diagnostics[0].passive_drop_count, 1u);
    EXPECT_EQ(diagnostics[1].source_id, StopSourceId(20));
    EXPECT_EQ(diagnostics[1].passive_drop_count, 1u);
}

TEST_F(StopPointRouterFixture, OneShotHitRemovesItsPhysicalStopBeforeResume)
{
    RecordingStopConsumer consumer;
    auto one_shot = PcSubscription(
        1,
        0x80001000u,
        consumer,
        ForegroundStopWait{});
    one_shot.lifetime = StopSubscriptionLifetime::OneShot;
    auto registration =
        router.RegisterGroup(Group(1, {std::move(one_shot)}));
    ASSERT_TRUE(registration.receipt.ok);

    EXPECT_TRUE(
        backend.InjectJitPcStop(0x80001000u).request_break);
    const auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    ASSERT_TRUE(receipts[0].event.has_value());
    EXPECT_TRUE(
        receipts[0].event
            ->requires_physical_reconcile_before_resume);
    EXPECT_TRUE(router.DesiredPhysicalPlan().pcs.empty());
    const auto release = registration.handle.Release();
    EXPECT_TRUE(release.ok);
    EXPECT_TRUE(release.already_released);
}

TEST_F(StopPointRouterFixture, MatchesMemoryAccessAndSyntheticIdentity)
{
    RecordingStopConsumer consumer;
    StopSubscriptionDefinition synthetic{
        .id = StopSubscriptionId(2),
        .point = SyntheticStopPointSpec{99},
        .consumer = &consumer,
    };
    auto registration = router.RegisterGroup(Group(
        1,
        {
            MemorySubscription(
                1,
                0x80300000u,
                4,
                StopMemoryAccess::Write,
                consumer),
            std::move(synthetic),
        }));
    ASSERT_TRUE(registration.receipt.ok);

    (void)backend.InjectMemoryStop(
        0x80001000u,
        0x80300000u,
        4,
        7,
        false);
    EXPECT_TRUE(router.DrainIngress().empty());
    (void)backend.InjectMemoryStop(
        0x80001000u,
        0x80300000u,
        4,
        8,
        true);
    const auto memory = router.DrainIngress();
    ASSERT_EQ(memory.size(), 1u);
    ASSERT_TRUE(memory[0].event.has_value());
    EXPECT_EQ(memory[0].event->evidence.value, 8u);

    const StopRouteReceipt routed =
        router.InjectSyntheticStop(SyntheticStopPointSpec{99});
    ASSERT_EQ(routed.deliveries.size(), 1u);
    EXPECT_EQ(
        routed.deliveries[0].subscription_id,
        StopSubscriptionId(2));
}

TEST_F(StopPointRouterFixture, AcceptsRetainedCurrentPointAndSuppressesOneReentry)
{
    RecordingStopConsumer first_consumer;
    auto first = router.RegisterGroup(Group(
        1,
        {PcSubscription(
            1,
            0x80001000u,
            first_consumer,
            ForegroundStopWait{})}));
    ASSERT_TRUE(first.receipt.ok);
    (void)backend.InjectJitPcStop(0x80001000u);
    const auto first_receipts = router.DrainIngress();
    ASSERT_EQ(first_receipts.size(), 1u);
    ASSERT_TRUE(first.handle.Release().ok);

    RecordingStopConsumer second_consumer;
    auto definition = Group(
        2,
        {PcSubscription(
            2,
            0x80001000u,
            second_consumer,
            ForegroundStopWait{
                .suppress_immediate_reentry = true})});
    auto second = router.RegisterGroup(
        std::move(definition),
        {.current_point = StopCurrentPointPolicy::AcceptIfAvailable});
    ASSERT_TRUE(second.receipt.ok) << second.receipt.error.message;
    ASSERT_TRUE(second.current_point.has_value());
    ASSERT_EQ(second.current_point->deliveries.size(), 1u);
    EXPECT_EQ(
        second.current_point->identity.sequence,
        first_receipts[0].identity.sequence);
    ASSERT_EQ(second_consumer.deliveries.size(), 1u);

    (void)backend.InjectJitPcStop(0x80001000u);
    (void)router.DrainIngress();
    EXPECT_EQ(second_consumer.deliveries.size(), 1u);
    (void)backend.InjectJitPcStop(0x80001000u);
    (void)router.DrainIngress();
    EXPECT_EQ(second_consumer.deliveries.size(), 2u);
}

TEST(StopPointRouter, RejectsCurrentPointWhenNewSampleWasNotCapturedAtHitTime)
{
    auto control = std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend(control);
    PhysicalStopPointManager manager(backend);
    RejectingCpuEvaluator evaluator;
    StopPointRouter router(manager, &evaluator);
    ASSERT_TRUE(router.Initialize(WorksetEpoch(1)).ok);

    RecordingStopConsumer consumer;
    auto wake = router.RegisterGroup(Group(
        1,
        {PcSubscription(
            1,
            0x80001000u,
            consumer,
            ForegroundStopWait{})}));
    ASSERT_TRUE(wake.receipt.ok);
    (void)backend.InjectJitPcStop(0x80001000u);
    ASSERT_EQ(router.DrainIngress().size(), 1u);

    auto sampled = PcSubscription(2, 0x80001000u, consumer);
    sampled.sample_descriptor_ids = {123};
    auto current = router.RegisterGroup(
        Group(2, {std::move(sampled)}),
        {.current_point = StopCurrentPointPolicy::Require});
    EXPECT_FALSE(current.receipt.ok);
    EXPECT_EQ(
        current.receipt.error.code,
        StopPointErrorCode::CurrentPointUnavailable);
    EXPECT_TRUE(router.StopIngressDrainAndCleanup().ok);
}

TEST_F(StopPointRouterFixture, ExplicitCurrentPointAcceptanceEndsOnDeparture)
{
    RecordingStopConsumer wake_consumer;
    auto wake = router.RegisterGroup(Group(
        1,
        {PcSubscription(
            1,
            0x80001000u,
            wake_consumer,
            ForegroundStopWait{})}));
    ASSERT_TRUE(wake.receipt.ok);
    (void)backend.InjectJitPcStop(0x80001000u);
    ASSERT_EQ(router.DrainIngress().size(), 1u);

    RecordingStopConsumer observer;
    auto registration = router.RegisterGroup(
        Group(2, {PcSubscription(2, 0x80001000u, observer)}));
    ASSERT_TRUE(registration.receipt.ok);
    const StopRouteReceipt accepted =
        router.AcceptCurrentPoint(registration.handle.lease());
    ASSERT_EQ(accepted.deliveries.size(), 1u);
    EXPECT_EQ(observer.deliveries.size(), 1u);

    EXPECT_FALSE(router.DepartCurrentPoint());
    const StopRouteReceipt departed =
        router.AcceptCurrentPoint(registration.handle.lease());
    EXPECT_EQ(departed.terminal, StopRouteTerminal::Unclaimed);
    EXPECT_EQ(
        departed.error.code,
        StopPointErrorCode::CurrentPointUnavailable);
}

TEST_F(StopPointRouterFixture, RevalidatesJitAndRejectsUnmanagedBreakpointDrift)
{
    RecordingStopConsumer consumer;
    auto registration = router.RegisterGroup(
        Group(1, {PcSubscription(1, 0x80001000u, consumer)}));
    ASSERT_TRUE(registration.receipt.ok);

    const StopDispatchGeneration dispatch_before =
        router.dispatch_generation();
    const PhysicalPlanGeneration before = manager.generation();
    (void)backend.InjectJitPcStop(0x80001000u);
    const auto revalidated = router.RevalidateAfterJit();
    ASSERT_TRUE(revalidated.ok) << revalidated.error.message;
    EXPECT_GT(manager.generation().value(), before.value());
    EXPECT_GT(
        router.dispatch_generation().value(),
        dispatch_before.value());

    const auto stale = router.DrainIngress();
    ASSERT_EQ(stale.size(), 1u);
    EXPECT_EQ(stale[0].terminal, StopRouteTerminal::Stale);
    EXPECT_EQ(
        stale[0].error.code,
        StopPointErrorCode::StaleDispatchGeneration);
    EXPECT_EQ(
        stale[0].identity.physical_generation,
        before);

    const PhysicalPlanGeneration after_revalidate = manager.generation();
    const auto unchanged = router.ValidateBreakpointChangeNotification();
    ASSERT_TRUE(unchanged.ok) << unchanged.error.message;
    EXPECT_EQ(manager.generation(), after_revalidate);

    control->SetQueryOutcome(FakePhysicalStopOutcome{
        .actual_override = PhysicalStopPointPlan{},
    });
    const auto drift = router.ValidateBreakpointChangeNotification();
    EXPECT_FALSE(drift.ok);
    EXPECT_EQ(
        drift.error.code,
        StopPointErrorCode::PhysicalIntegrityUnknown);
    EXPECT_FALSE(router.ingress_enabled());
    EXPECT_EQ(manager.generation(), after_revalidate);
}

TEST_F(
    StopPointRouterFixture,
    StoppedMovieCoreBoundaryRequiresEmptyPlanAndRejectsQueuedIngress)
{
    const WorksetEpoch epoch = router.workset_epoch();
    const StopDispatchGeneration dispatch_before =
        router.dispatch_generation();
    const PhysicalPlanGeneration physical_before =
        manager.generation();
    (void)backend.InjectJitPcStop(0x80001000u);

    ASSERT_TRUE(router.ValidateEmptyForMovieCoreStop().ok);
    const StopPointLifecycleReceipt reconciled =
        router.EnterStoppedMovieCoreBoundary();
    ASSERT_TRUE(reconciled.ok) << reconciled.error.message;
    EXPECT_EQ(reconciled.workset_epoch, epoch);
    // With an empty physical plan the injected stop is passed immediately;
    // the boundary still proves the ingress queue is empty before returning.
    EXPECT_EQ(reconciled.drained_event_count, 0u);
    EXPECT_GT(
        reconciled.dispatch_generation.value(),
        dispatch_before.value());
    EXPECT_GT(
        reconciled.physical_generation.value(),
        physical_before.value());
    EXPECT_TRUE(router.DesiredPhysicalPlan().pcs.empty());

    RecordingStopConsumer consumer;
    auto registration = router.RegisterGroup(
        Group(1, {PcSubscription(1, 0x80001000u, consumer)}));
    ASSERT_TRUE(registration.receipt.ok)
        << registration.receipt.error.message;
    ASSERT_TRUE(router.RevalidateAfterJit().ok);
    EXPECT_EQ(router.DesiredPhysicalPlan().pcs,
              std::vector<PhysicalPcStop>{{0x80001000u}});
    ASSERT_TRUE(registration.handle.Release().ok);
}

TEST_F(
    StopPointRouterFixture,
    MovieCoreStopValidationRejectsAnInstalledProgramPlan)
{
    RecordingStopConsumer consumer;
    auto registration = router.RegisterGroup(
        Group(1, {PcSubscription(1, 0x80001000u, consumer)}));
    ASSERT_TRUE(registration.receipt.ok);

    const StopPointLifecycleReceipt rejected =
        router.ValidateEmptyForMovieCoreStop();
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error.code, StopPointErrorCode::InvalidState);

    ASSERT_TRUE(registration.handle.Release().ok);
}

TEST_F(StopPointRouterFixture, RoutingHistoryIsBounded)
{
    RecordingStopConsumer consumer;
    auto registration = router.RegisterGroup(
        Group(1, {PcSubscription(1, 0x80001000u, consumer)}));
    ASSERT_TRUE(registration.receipt.ok);

    for (std::size_t i = 0; i < kStopPointRoutingHistoryCapacity + 20; ++i)
    {
        (void)backend.InjectJitPcStop(0x80001000u);
        ASSERT_EQ(router.DrainIngress().size(), 1u);
    }
    EXPECT_EQ(
        router.RoutingHistory().size(),
        kStopPointRoutingHistoryCapacity);
}

} // namespace
