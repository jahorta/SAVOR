#include <gtest/gtest.h>

#include "Runner/Runtime/EmulationSession.h"
#include "common/FakePhysicalStopBackend.h"
#include "common/ScriptedDolphinBackend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

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

StopSubscriptionGroupDefinition PcGroup(
    std::uint64_t id,
    std::uint32_t pc,
    IStopPointConsumer& consumer,
    StopEpochPolicy epoch_policy = StopEpochPolicy::EndOnEpochChange)
{
    return {
        .id = StopSubscriptionGroupId(id),
        .source = {
            .id = StopSourceId(id + 1000),
            .stable_name = "session.stop.test." + std::to_string(id),
            .diagnostic_label = "EmulationSession stop-point test",
        },
        .epoch_policy = epoch_policy,
        .subscriptions = {{
            .id = StopSubscriptionId(id),
            .point = PcStopPointSpec{pc},
            .delivery = StopDeliveryMode::Observe,
            .policy = StopRoutingPolicy::Pass,
            .consumer = &consumer,
        }},
    };
}

std::size_t CountPhysicalCalls(
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

class SessionStopPointHarness
{
public:
    explicit SessionStopPointHarness(std::uint64_t session_id)
        : session_control(
              std::make_shared<ScriptedDolphinBackendControl>()),
          physical_control(
              std::make_shared<FakePhysicalStopBackendControl>())
    {
        auto physical =
            std::make_unique<FakePhysicalStopBackend>(physical_control);
        physical_backend = physical.get();
        session = std::make_unique<EmulationSession>(
            SessionId(session_id),
            MakeScriptedDolphinBackend(
                session_control,
                std::move(physical)));
    }

    [[nodiscard]] SessionOperationReceipt Open()
    {
        return session->Open({});
    }

    std::shared_ptr<ScriptedDolphinBackendControl> session_control;
    std::shared_ptr<FakePhysicalStopBackendControl> physical_control;
    FakePhysicalStopBackend* physical_backend = nullptr;
    std::unique_ptr<EmulationSession> session;
};

TEST(
    EmulationSessionStopPoints,
    OpenTransactionBindsSinkAndEstablishesExactPlanOrRollsBack)
{
    {
        SessionStopPointHarness harness(1001);
        const SessionOperationReceipt open = harness.Open();
        ASSERT_TRUE(open.ok) << open.backend.message;
        EXPECT_EQ(open.origin_epoch, StateEpoch{});
        EXPECT_EQ(open.resulting_epoch, StateEpoch(1));

        StopPointRouter* const router = harness.session->stop_points();
        ASSERT_NE(router, nullptr);
        EXPECT_TRUE(router->ingress_enabled());
        EXPECT_EQ(router->state_epoch(), StateEpoch(1));
        EXPECT_EQ(
            harness.physical_control->BoundSink(),
            static_cast<savor::probe::INativeStopSink*>(router));

        const auto open_calls = harness.physical_control->Calls();
        ASSERT_GE(open_calls.size(), 2u);
        EXPECT_EQ(
            open_calls[0].operation,
            FakePhysicalStopOperation::BindSink);
        EXPECT_EQ(
            open_calls[1].operation,
            FakePhysicalStopOperation::Apply);
        EXPECT_TRUE(open_calls[1].commit_invoked);
        EXPECT_EQ(
            open_calls[1].generation,
            PhysicalPlanGeneration(1));
        EXPECT_TRUE(open_calls[1].plan.pcs.empty());
        EXPECT_TRUE(open_calls[1].plan.memory.empty());

        RecordingStopConsumer consumer;
        auto registration =
            router->RegisterGroup(PcGroup(1, 0x80001000u, consumer));
        ASSERT_TRUE(registration.receipt.ok)
            << registration.receipt.error.message;
        EXPECT_EQ(
            harness.physical_control->ActualPlan().pcs,
            std::vector<PhysicalPcStop>{{0x80001000u}});
        EXPECT_EQ(
            harness.physical_control->ActualGeneration(),
            PhysicalPlanGeneration(2));

        EXPECT_TRUE(harness.session->Shutdown().ok);
    }

    SessionStopPointHarness rollback(1002);
    rollback.physical_control->SetApplyOutcome(
        FakePhysicalStopOutcome{
            .ok = false,
            .integrity = PhysicalStopIntegrity::Preserved,
            .message = "injected initial-plan failure",
        });
    const SessionOperationReceipt failed_open = rollback.Open();
    EXPECT_FALSE(failed_open.ok);
    EXPECT_EQ(failed_open.resulting_epoch, StateEpoch{});
    EXPECT_EQ(failed_open.disposition, SessionDisposition::Closed);
    EXPECT_EQ(rollback.session->stop_points(), nullptr);
    EXPECT_EQ(rollback.physical_control->BoundSink(), nullptr);
    EXPECT_TRUE(rollback.physical_control->ActualPlan().pcs.empty());
    EXPECT_EQ(rollback.session_control->CloseCount(), 1);

    const auto rollback_calls = rollback.physical_control->Calls();
    EXPECT_EQ(
        CountPhysicalCalls(
            rollback_calls,
            FakePhysicalStopOperation::BindSink),
        1u);
    EXPECT_EQ(
        CountPhysicalCalls(
            rollback_calls,
            FakePhysicalStopOperation::Apply),
        1u);
    EXPECT_EQ(
        CountPhysicalCalls(
            rollback_calls,
            FakePhysicalStopOperation::UnbindSink),
        1u);
    EXPECT_TRUE(rollback.session->Shutdown().ok);

    SessionStopPointHarness stopped(1003);
    stopped.session_control->SetOpenResult(
        BackendResult::Success(),
        BackendCoreState::Stopped);
    const SessionOperationReceipt stopped_open = stopped.Open();
    EXPECT_FALSE(stopped_open.ok);
    EXPECT_EQ(stopped_open.resulting_epoch, StateEpoch{});
    EXPECT_EQ(stopped.session->stop_points(), nullptr);
    EXPECT_EQ(stopped.physical_control->BoundSink(), nullptr);
    EXPECT_TRUE(stopped.physical_control->ActualPlan().pcs.empty());
    EXPECT_TRUE(stopped.physical_control->ActualPlan().memory.empty());
    EXPECT_EQ(stopped.session_control->CloseCount(), 1);
}

TEST(
    EmulationSessionStopPoints,
    PreconfiguredIngressNotificationSurvivesOpenAndEngineResumeDepartsCurrentPoint)
{
    SessionStopPointHarness harness(1010);
    std::atomic<std::uint64_t> notification{0};
    ASSERT_TRUE(
        harness.session->ConfigureStopPointIngressNotification(
            &notification,
            nullptr,
            nullptr));
    ASSERT_TRUE(harness.Open().ok);
    ASSERT_EQ(
        harness.session->snapshot().core_state,
        BackendCoreState::Paused);

    RecordingStopConsumer wake_consumer;
    StopPointRouter* const router = harness.session->stop_points();
    ASSERT_NE(router, nullptr);
    auto wake_definition = PcGroup(40, 0x80001000u, wake_consumer);
    wake_definition.subscriptions[0].delivery = StopDeliveryMode::Wake;
    auto wake = router->RegisterGroup(std::move(wake_definition));
    ASSERT_TRUE(wake.receipt.ok);

    EXPECT_TRUE(
        harness.physical_backend
            ->InjectJitPcStop(0x80001000u)
            .request_break);
    EXPECT_EQ(notification.load(std::memory_order_acquire), 1u);
    ASSERT_EQ(harness.session->DrainStopPointEvents().size(), 1u);
    const ExecutionSubmissionReceipt resumed =
        harness.session->SubmitExecution(InteractiveResumeRequest{
            .expected_epoch = StateEpoch(1),
        });
    ASSERT_TRUE(resumed.accepted) << resumed.error.message;

    RecordingStopConsumer observer;
    auto current = router->RegisterGroup(
        PcGroup(41, 0x80001000u, observer),
        {.current_point = StopCurrentPointPolicy::Require});
    EXPECT_FALSE(current.receipt.ok);
    EXPECT_EQ(
        current.receipt.error.code,
        StopPointErrorCode::CurrentPointUnavailable);
    ASSERT_TRUE(
        harness.session
            ->CancelExecution(CancellationReason::ExternalRequest)
            .accepted);
    harness.session->PumpExecution();
    EXPECT_FALSE(
        harness.session->DrainExecutionEvents().empty());
    EXPECT_TRUE(harness.session->Shutdown().ok);
}

TEST(
    EmulationSessionStopPoints,
    RebootFileAndBufferReplacementAdvanceOnlyOnSuccess)
{
    SessionStopPointHarness harness(1003);
    ASSERT_TRUE(harness.Open().ok);
    ASSERT_NE(harness.session->stop_points(), nullptr);

    const auto expect_epoch = [&](std::uint64_t expected) {
        EXPECT_EQ(
            harness.session->snapshot().state_epoch,
            StateEpoch(expected));
        EXPECT_EQ(
            harness.session->stop_points()->state_epoch(),
            StateEpoch(expected));
    };
    expect_epoch(1);

    EXPECT_TRUE(harness.session->Reboot().ok);
    expect_epoch(2);
    EXPECT_TRUE(
        harness.session->RestoreStateFile("successful.state").ok);
    expect_epoch(3);
    EXPECT_TRUE(
        harness.session->RestoreStateBuffer({0x01, 0x02}).ok);
    expect_epoch(4);

    harness.session_control->SetRebootResult(
        BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "preserved reboot failure",
            BackendIntegrity::Preserved));
    const SessionOperationReceipt failed_reboot =
        harness.session->Reboot();
    EXPECT_FALSE(failed_reboot.ok);
    EXPECT_EQ(failed_reboot.origin_epoch, StateEpoch(4));
    EXPECT_EQ(failed_reboot.resulting_epoch, StateEpoch(4));
    EXPECT_EQ(failed_reboot.disposition, SessionDisposition::Clean);
    expect_epoch(4);
    harness.session_control->SetRebootResult(
        BackendResult::Success());

    harness.session_control->SetRestoreFileResult(
        BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "preserved file restore failure",
            BackendIntegrity::Preserved));
    const SessionOperationReceipt failed_file =
        harness.session->RestoreStateFile("failed.state");
    EXPECT_FALSE(failed_file.ok);
    EXPECT_EQ(failed_file.origin_epoch, StateEpoch(4));
    EXPECT_EQ(failed_file.resulting_epoch, StateEpoch(4));
    EXPECT_EQ(failed_file.disposition, SessionDisposition::Clean);
    expect_epoch(4);
    harness.session_control->SetRestoreFileResult(
        BackendResult::Success());

    harness.session_control->SetRestoreBufferResult(
        BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "preserved buffer restore failure",
            BackendIntegrity::Preserved));
    const SessionOperationReceipt failed_buffer =
        harness.session->RestoreStateBuffer({0x03, 0x04});
    EXPECT_FALSE(failed_buffer.ok);
    EXPECT_EQ(failed_buffer.origin_epoch, StateEpoch(4));
    EXPECT_EQ(failed_buffer.resulting_epoch, StateEpoch(4));
    EXPECT_EQ(failed_buffer.disposition, SessionDisposition::Clean);
    expect_epoch(4);

    const auto calls = harness.physical_control->Calls();
    EXPECT_EQ(
        CountPhysicalCalls(calls, FakePhysicalStopOperation::Apply),
        7u);
    EXPECT_TRUE(harness.session->Shutdown().ok);
}

TEST(
    EmulationSessionStopPoints,
    ReplacementEndsOrRebindsGroupsAtTheNewEpoch)
{
    SessionStopPointHarness harness(1004);
    ASSERT_TRUE(harness.Open().ok);
    StopPointRouter* const router = harness.session->stop_points();
    ASSERT_NE(router, nullptr);

    RecordingStopConsumer ending_consumer;
    RecordingStopConsumer rebinding_consumer;
    auto ending = router->RegisterGroup(PcGroup(
        1,
        0x80001000u,
        ending_consumer,
        StopEpochPolicy::EndOnEpochChange));
    auto rebinding = router->RegisterGroup(PcGroup(
        2,
        0x80002000u,
        rebinding_consumer,
        StopEpochPolicy::RebindAfterRestore));
    ASSERT_TRUE(ending.receipt.ok) << ending.receipt.error.message;
    ASSERT_TRUE(rebinding.receipt.ok)
        << rebinding.receipt.error.message;
    EXPECT_EQ(
        router->DesiredPhysicalPlan().pcs,
        (std::vector<PhysicalPcStop>{
            {0x80001000u},
            {0x80002000u},
        }));

    const SessionOperationReceipt restored =
        harness.session->RestoreStateFile("next-epoch.state");
    ASSERT_TRUE(restored.ok) << restored.backend.message;
    EXPECT_EQ(restored.origin_epoch, StateEpoch(1));
    EXPECT_EQ(restored.resulting_epoch, StateEpoch(2));
    EXPECT_EQ(router->state_epoch(), StateEpoch(2));
    EXPECT_EQ(
        router->DesiredPhysicalPlan().pcs,
        std::vector<PhysicalPcStop>{{0x80002000u}});

    (void)harness.physical_backend->InjectJitPcStop(
        0x80001000u);
    EXPECT_TRUE(harness.session->DrainStopPointEvents().empty());
    EXPECT_TRUE(ending_consumer.deliveries.empty());

    (void)harness.physical_backend->InjectJitPcStop(0x80002000u);
    const auto receipts = harness.session->DrainStopPointEvents();
    ASSERT_EQ(receipts.size(), 1u);
    ASSERT_EQ(rebinding_consumer.deliveries.size(), 1u);
    EXPECT_EQ(
        rebinding_consumer.deliveries[0].event.identity.state_epoch,
        StateEpoch(2));

    const StopReleaseReceipt ended_release =
        ending.handle.Release();
    EXPECT_TRUE(ended_release.ok);
    EXPECT_TRUE(ended_release.already_released);
    const StopReleaseReceipt rebound_release =
        rebinding.handle.Release();
    EXPECT_TRUE(rebound_release.ok);
    EXPECT_FALSE(rebound_release.already_released);
    EXPECT_TRUE(harness.session->Shutdown().ok);
}

TEST(
    EmulationSessionStopPoints,
    PostReplacementReconcileFailureTaintsAndClosesSession)
{
    SessionStopPointHarness harness(1005);
    ASSERT_TRUE(harness.Open().ok);
    StopPointRouter* const router = harness.session->stop_points();
    ASSERT_NE(router, nullptr);

    RecordingStopConsumer consumer;
    auto registration =
        router->RegisterGroup(PcGroup(1, 0x80001000u, consumer));
    ASSERT_TRUE(registration.receipt.ok)
        << registration.receipt.error.message;

    harness.physical_control->SetApplyOutcome(
        FakePhysicalStopOutcome{
            .ok = false,
            .integrity = PhysicalStopIntegrity::Unknown,
            .message = "injected post-restore reconcile failure",
        });
    const SessionOperationReceipt restored =
        harness.session->RestoreStateBuffer({0x01});
    EXPECT_FALSE(restored.ok);
    EXPECT_EQ(restored.origin_epoch, StateEpoch(1));
    EXPECT_EQ(restored.resulting_epoch, StateEpoch(2));
    EXPECT_EQ(restored.disposition, SessionDisposition::Tainted);
    EXPECT_EQ(
        restored.backend.integrity,
        BackendIntegrity::Unknown);

    const SessionSnapshot snapshot = harness.session->snapshot();
    EXPECT_FALSE(snapshot.open);
    EXPECT_EQ(snapshot.core_state, BackendCoreState::Closed);
    EXPECT_EQ(snapshot.state_epoch, StateEpoch(2));
    EXPECT_EQ(snapshot.disposition, SessionDisposition::Tainted);
    EXPECT_FALSE(router->ingress_enabled());
    EXPECT_EQ(harness.physical_control->BoundSink(), nullptr);
    EXPECT_TRUE(harness.physical_control->ActualPlan().pcs.empty());
    EXPECT_EQ(harness.session_control->CloseCount(), 1);

    const auto calls = harness.physical_control->Calls();
    EXPECT_EQ(
        CountPhysicalCalls(calls, FakePhysicalStopOperation::Clear),
        1u);
    EXPECT_EQ(
        CountPhysicalCalls(
            calls,
            FakePhysicalStopOperation::UnbindSink),
        1u);
    (void)harness.session->Shutdown();
}

TEST(
    EmulationSessionStopPoints,
    JitRevalidationAdvancesPhysicalGenerationWithoutChangingEpoch)
{
    SessionStopPointHarness harness(1006);
    ASSERT_TRUE(harness.Open().ok);
    StopPointRouter* const router = harness.session->stop_points();
    ASSERT_NE(router, nullptr);

    RecordingStopConsumer consumer;
    auto registration =
        router->RegisterGroup(PcGroup(1, 0x80001000u, consumer));
    ASSERT_TRUE(registration.receipt.ok)
        << registration.receipt.error.message;

    const StateEpoch epoch = harness.session->snapshot().state_epoch;
    const StopDispatchGeneration dispatch =
        router->dispatch_generation();
    const PhysicalPlanGeneration physical =
        harness.physical_control->ActualGeneration();

    const SessionOperationReceipt revalidated =
        harness.session->RevalidateStopPointsAfterJit();
    ASSERT_TRUE(revalidated.ok) << revalidated.backend.message;
    EXPECT_EQ(revalidated.origin_epoch, epoch);
    EXPECT_EQ(revalidated.resulting_epoch, epoch);
    EXPECT_EQ(harness.session->snapshot().state_epoch, epoch);
    EXPECT_EQ(router->state_epoch(), epoch);
    EXPECT_EQ(
        router->dispatch_generation().value(),
        dispatch.value() + 1);
    EXPECT_EQ(
        harness.physical_control->ActualGeneration().value(),
        physical.value() + 1);

    const auto calls = harness.physical_control->Calls();
    EXPECT_EQ(
        CountPhysicalCalls(
            calls,
            FakePhysicalStopOperation::Revalidate),
        1u);
    EXPECT_TRUE(harness.session->Shutdown().ok);
}

TEST(
    EmulationSessionStopPoints,
    ShutdownDrainsIngressAndClearsOwnershipBeforeBackendDestruction)
{
    RecordingStopConsumer consumer;
    SessionStopPointHarness harness(1007);
    ASSERT_TRUE(harness.Open().ok);
    StopPointRouter* const router = harness.session->stop_points();
    ASSERT_NE(router, nullptr);

    auto registration =
        router->RegisterGroup(PcGroup(1, 0x80001000u, consumer));
    ASSERT_TRUE(registration.receipt.ok)
        << registration.receipt.error.message;

    for (std::uint32_t index = 0; index < 3; ++index)
    {
        (void)harness.physical_backend->InjectJitPcStop(
            0x80001000u);
    }
    EXPECT_TRUE(consumer.deliveries.empty());

    std::atomic<bool> destruction_observed{false};
    std::atomic<bool> cleanup_preceded_destruction{false};
    harness.session_control->SetDestructionObserver([&] {
        cleanup_preceded_destruction.store(
            harness.physical_control->BoundSink() == nullptr &&
                harness.physical_control->ActualPlan().pcs.empty() &&
                harness.physical_control->ActualPlan().memory.empty() &&
                consumer.deliveries.size() == 3,
            std::memory_order_release);
        destruction_observed.store(true, std::memory_order_release);
    });

    const SessionOperationReceipt shutdown =
        harness.session->Shutdown();
    ASSERT_TRUE(shutdown.ok) << shutdown.backend.message;
    EXPECT_EQ(consumer.deliveries.size(), 3u);
    EXPECT_EQ(harness.session->stop_points(), nullptr);
    EXPECT_TRUE(
        destruction_observed.load(std::memory_order_acquire));
    EXPECT_TRUE(
        cleanup_preceded_destruction.load(
            std::memory_order_acquire));
    EXPECT_EQ(harness.session_control->DestructionCount(), 1);
    EXPECT_EQ(harness.physical_control->BoundSink(), nullptr);
    EXPECT_TRUE(harness.physical_control->ActualPlan().pcs.empty());

    const auto calls = harness.physical_control->Calls();
    EXPECT_EQ(
        CountPhysicalCalls(calls, FakePhysicalStopOperation::Clear),
        1u);
    EXPECT_EQ(
        CountPhysicalCalls(
            calls,
            FakePhysicalStopOperation::UnbindSink),
        1u);
    EXPECT_TRUE(harness.session->Shutdown().ok);
    EXPECT_EQ(harness.session_control->DestructionCount(), 1);
}

} // namespace
