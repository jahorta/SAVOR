#include <gtest/gtest.h>

#include "Runner/Runtime/Execution/ExecutionEngine.h"
#include "common/FakeExecutionBackend.h"
#include "common/FakePhysicalStopBackend.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace savor::runtime;
using namespace savor::test_support;

constexpr StateEpoch kEpoch{7};
constexpr std::uint32_t kWakePc = 0x801DC288u;

std::size_t CountCall(
    const std::vector<std::string>& calls,
    const std::string& expected)
{
    return static_cast<std::size_t>(std::count(
        calls.begin(),
        calls.end(),
        expected));
}

ExecutionRequestPolicy Policy(
    StateEpoch epoch = kEpoch,
    std::chrono::milliseconds active_timeout = 1s)
{
    ExecutionRequestPolicy policy;
    policy.expected_epoch = epoch;
    policy.active_timeout = active_timeout;
    return policy;
}

StopSubscriptionGroupDefinition WakeGroup(std::uint32_t pc = kWakePc)
{
    return {
        .id = StopSubscriptionGroupId(100),
        .source = {
            .id = StopSourceId(100),
            .stable_name = "test.execution-engine.wake",
            .diagnostic_label = "execution-engine test",
        },
        .epoch_policy = StopEpochPolicy::EndOnEpochChange,
        .subscriptions = {
            {
                .id = StopSubscriptionId(100),
                .point = PcStopPointSpec{pc},
                .delivery = StopDeliveryMode::Wake,
                .policy = StopRoutingPolicy::Pass,
                .suppress_immediate_reentry = true,
            },
        },
    };
}

InterruptionHandlerDescriptor Handler(
    std::string key = "test.dialog",
    std::uint8_t maximum_depth = 8)
{
    return {
        .key = std::move(key),
        .allowed_child_operations = {
            ExecutionOperationKind::ContinueUntil,
            ExecutionOperationKind::SafePause,
        },
        .child_active_budget = 200ms,
        .permitted_nested_keys = {"test.dialog"},
        .allow_self_recursion = true,
        .maximum_depth = maximum_depth,
    };
}

class RecordingInterruptionConsumer final : public IStopPointConsumer
{
public:
    void OnStopPoint(const StopDelivery& delivery) override
    {
        deliveries.push_back(delivery);
    }

    std::vector<StopDelivery> deliveries;
};

std::optional<ExecutionTerminalResult> TakeTerminal(
    ExecutionEngine& engine)
{
    for (ExecutionEvent& event : engine.DrainEvents())
    {
        if (event.kind == ExecutionEventKind::Terminal &&
            event.terminal.has_value())
        {
            return std::move(event.terminal);
        }
    }
    return std::nullopt;
}

std::vector<ExecutionTerminalResult> TakeTerminals(
    ExecutionEngine& engine)
{
    std::vector<ExecutionTerminalResult> terminals;
    for (ExecutionEvent& event : engine.DrainEvents())
    {
        if (event.kind == ExecutionEventKind::Terminal &&
            event.terminal.has_value())
        {
            terminals.push_back(std::move(*event.terminal));
        }
    }
    return terminals;
}

std::optional<ExecutionTerminalResult> DrainTerminal(
    ExecutionEngine& engine,
    int maximum_pumps = 32)
{
    if (auto terminal = TakeTerminal(engine))
        return terminal;
    for (int pump = 0; pump < maximum_pumps; ++pump)
    {
        engine.Pump();
        if (auto terminal = TakeTerminal(engine))
            return terminal;
    }
    return std::nullopt;
}

class FakeInputAdvancePort final : public IInputAdvancePort
{
public:
    explicit FakeInputAdvancePort(
        std::shared_ptr<FakeExecutionBackendControl> backend)
        : backend_(std::move(backend))
    {
    }

    InputAdvanceReceipt Validate(
        InputAdvanceBindingId binding,
        StateEpoch epoch) override
    {
        calls.push_back("validate");
        frame_steps_seen.push_back(FrameStepCount());
        last_binding = binding;
        last_epoch = epoch;
        return {
            .ok = true,
            .decision = InputAdvanceDecision::Continue,
        };
    }

    InputAdvanceReceipt PrepareNext(
        InputAdvanceBindingId binding,
        StateEpoch epoch,
        std::uint32_t advance_ordinal) override
    {
        calls.push_back("prepare");
        frame_steps_seen.push_back(FrameStepCount());
        last_binding = binding;
        last_epoch = epoch;
        last_ordinal = advance_ordinal;
        const InputPublicationToken publication(1000 + advance_ordinal);
        last_publication = publication;
        prepared_publications.push_back(publication);
        return {
            .ok = true,
            .decision = InputAdvanceDecision::Continue,
            .publication = publication,
            .publication_evidence = InputPublicationEvidence{
                InputLeaseId(77),
                publication,
                epoch,
                savor::GCInputFrame{
                    .buttons = static_cast<std::uint16_t>(
                        advance_ordinal + 1)}},
        };
    }

    InputAdvanceReceipt ObserveAcknowledgement(
        InputAdvanceBindingId binding,
        InputPublicationToken publication,
        StateEpoch epoch) override
    {
        calls.push_back("observe");
        frame_steps_seen.push_back(FrameStepCount());
        last_binding = binding;
        last_epoch = epoch;
        observed_publication = publication;
        const InputAdvanceDecision decision =
            acknowledgement_index < acknowledgement_decisions.size()
            ? acknowledgement_decisions[acknowledgement_index++]
            : InputAdvanceDecision::Complete;
        return {
            .ok = true,
            .decision = decision,
            .publication = publication,
        };
    }

    InputAdvanceReceipt Complete(
        InputAdvanceBindingId binding,
        StateEpoch epoch) noexcept override
    {
        calls.push_back("complete");
        frame_steps_seen.push_back(FrameStepCount());
        last_binding = binding;
        last_epoch = epoch;
        return {
            .ok = true,
            .decision = InputAdvanceDecision::Complete,
        };
    }

    InputAdvanceReceipt Cancel(
        InputAdvanceBindingId binding,
        StateEpoch epoch) noexcept override
    {
        calls.push_back("cancel");
        frame_steps_seen.push_back(FrameStepCount());
        last_binding = binding;
        last_epoch = epoch;
        return {
            .ok = true,
            .decision = InputAdvanceDecision::Cancelled,
        };
    }

    std::vector<std::string> calls;
    std::vector<std::size_t> frame_steps_seen;
    InputAdvanceBindingId last_binding;
    InputPublicationToken last_publication;
    InputPublicationToken observed_publication;
    std::vector<InputPublicationToken> prepared_publications;
    StateEpoch last_epoch;
    std::uint32_t last_ordinal = 0;
    std::vector<InputAdvanceDecision> acknowledgement_decisions{
        InputAdvanceDecision::Complete,
    };
    std::size_t acknowledgement_index = 0;

private:
    std::size_t FrameStepCount() const
    {
        return CountCall(backend_->Calls(), "frame_step");
    }

    std::shared_ptr<FakeExecutionBackendControl> backend_;
};

class ExecutionEngineFixture : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(router.Initialize(kEpoch).ok);
    }

    void TearDown() override
    {
        if (engine)
        {
            const BackendResult shutdown = engine->Shutdown();
            EXPECT_TRUE(shutdown.ok) << shutdown.message;
            engine.reset();
        }
        if (interruption_group)
        {
            const StopReleaseReceipt released =
                interruption_group->Release();
            EXPECT_TRUE(released.ok) << released.error.message;
            interruption_group.reset();
        }
        const StopPointLifecycleReceipt cleanup =
            router.StopIngressDrainAndCleanup();
        EXPECT_TRUE(cleanup.ok) << cleanup.error.message;
    }

    void CreateEngine(
        IInputAdvancePort* input = nullptr,
        std::vector<InterruptionHandlerDescriptor> handlers = {})
    {
        ExecutionEngineConfig config;
        config.maintenance_interval = 10ms;
        config.now = [this] { return now; };
        config.input_advance = input;
        config.interruption_handlers = std::move(handlers);
        engine = std::make_unique<ExecutionEngine>(
            execution_backend,
            router,
            std::move(config));
        const BackendResult initialized = engine->Initialize(kEpoch);
        ASSERT_TRUE(initialized.ok) << initialized.message;
    }

    StopRouteReceipt RouteInterruption(
        std::string key = "test.dialog")
    {
        StopSubscriptionGroupDefinition definition{
            .id = StopSubscriptionGroupId(500),
            .source = {
                .id = StopSourceId(500),
                .stable_name = "test.execution-engine.interruption",
                .diagnostic_label = "execution-engine interruption test",
            },
            .epoch_policy = StopEpochPolicy::EndOnEpochChange,
            .subscriptions = {{
                .id = StopSubscriptionId(500),
                .point = PcStopPointSpec{kWakePc},
                .delivery = StopDeliveryMode::Intercept,
                .policy =
                    StopRoutingPolicy::RequestInterruptionHandler,
                .priority = 100,
                .interruption_handler_key = std::move(key),
                .lossless = true,
                .consumer = &interruption_consumer,
            }},
        };

        if (interruption_group)
        {
            const StopGroupReceipt replaced =
                interruption_group->Replace(std::move(definition));
            if (!replaced.ok)
            {
                ADD_FAILURE() << replaced.error.message;
                return {};
            }
        }
        else
        {
            StopGroupRegistrationResult registered =
                router.RegisterGroup(std::move(definition));
            if (!registered.receipt.ok)
            {
                ADD_FAILURE() << registered.receipt.error.message;
                return {};
            }
            interruption_group.emplace(
                std::move(registered.handle));
        }

        const auto decision =
            physical_backend.InjectJitPcStop(kWakePc);
        if (!decision.request_break)
        {
            ADD_FAILURE()
                << "interruption route did not request a stopped core";
            return {};
        }
        std::vector<StopRouteReceipt> receipts =
            router.DrainIngress();
        const auto routed = std::find_if(
            receipts.begin(),
            receipts.end(),
            [](const StopRouteReceipt& receipt) {
                return receipt.terminal ==
                    StopRouteTerminal::InterruptionHandlerRequested;
            });
        if (routed == receipts.end())
        {
            ADD_FAILURE()
                << "router did not produce an interruption receipt";
            return {};
        }
        return std::move(*routed);
    }

    std::shared_ptr<FakePhysicalStopBackendControl> physical_control =
        std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend physical_backend{physical_control};
    PhysicalStopPointManager physical_manager{physical_backend};
    StopPointRouter router{physical_manager};

    std::shared_ptr<FakeExecutionBackendControl> execution_control =
        std::make_shared<FakeExecutionBackendControl>();
    FakeExecutionBackend execution_backend{execution_control};
    std::chrono::steady_clock::time_point now{};
    std::unique_ptr<ExecutionEngine> engine;
    RecordingInterruptionConsumer interruption_consumer;
    std::optional<StopSubscriptionGroupHandle> interruption_group;
};

TEST_F(ExecutionEngineFixture, InitializesAsIdlePausedAtTheSessionEpoch)
{
    CreateEngine();

    const ExecutionSnapshot snapshot = engine->snapshot();
    EXPECT_EQ(snapshot.activity, ExecutionActivity::IdlePaused);
    EXPECT_EQ(snapshot.state_epoch, kEpoch);
    EXPECT_FALSE(snapshot.active_operation.has_value());
    EXPECT_EQ(snapshot.evidence.core_state, BackendCoreState::Paused);
    EXPECT_TRUE(snapshot.evidence.pause_confirmed);
    EXPECT_EQ(CountCall(execution_control->Calls(), "pause"), 0u);
    EXPECT_FALSE(execution_control->HasOwnerViolation());
}

TEST_F(ExecutionEngineFixture, RejectsASecondOperationWhileInteractiveResumeIsActive)
{
    CreateEngine();

    const ExecutionSubmissionReceipt running =
        engine->Submit(InteractiveResumeRequest{.expected_epoch = kEpoch});
    ASSERT_TRUE(running.accepted) << running.error.message;
    EXPECT_EQ(
        engine->snapshot().activity,
        ExecutionActivity::InteractiveRunning);

    const ExecutionSubmissionReceipt second =
        engine->Submit(StepFramesRequest{.policy = Policy(), .count = 1});
    EXPECT_FALSE(second.accepted);
    EXPECT_EQ(second.error.code, ExecutionErrorCode::Busy);
    EXPECT_EQ(CountCall(execution_control->Calls(), "frame_step"), 0u);

    const ExecutionControlReceipt cancelled =
        engine->Cancel(CancellationReason::ExternalRequest);
    ASSERT_TRUE(cancelled.accepted) << cancelled.error.message;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::Cancelled);
    EXPECT_EQ(
        engine->snapshot().activity,
        ExecutionActivity::IdlePaused);
    EXPECT_EQ(execution_control->Snapshot().core_state, BackendCoreState::Paused);
    EXPECT_EQ(CountCall(execution_control->Calls(), "resume"), 1u);
    EXPECT_EQ(CountCall(execution_control->Calls(), "pause"), 1u);
}

TEST_F(
    ExecutionEngineFixture,
    AssignsMonotonicIdsAndEmitsExactlyOneTerminalPerOperation)
{
    CreateEngine();

    const ExecutionSubmissionReceipt first =
        engine->Submit(StepFramesRequest{.policy = Policy(), .count = 1});
    ASSERT_TRUE(first.accepted) << first.error.message;
    const auto first_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(first_terminal.has_value());
    EXPECT_EQ(first_terminal->operation_id, first.operation_id);
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    const ExecutionSubmissionReceipt second =
        engine->Submit(StepFramesRequest{.policy = Policy(), .count = 1});
    ASSERT_TRUE(second.accepted) << second.error.message;
    EXPECT_LT(first.operation_id.value(), second.operation_id.value());
    const auto second_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(second_terminal.has_value());
    EXPECT_EQ(second_terminal->operation_id, second.operation_id);
    EXPECT_FALSE(TakeTerminal(*engine).has_value());
}

TEST_F(ExecutionEngineFixture, InteractiveResumeAcceptsASerializedSafePause)
{
    CreateEngine();

    const ExecutionSubmissionReceipt running =
        engine->Submit(InteractiveResumeRequest{.expected_epoch = kEpoch});
    ASSERT_TRUE(running.accepted) << running.error.message;
    const ExecutionSubmissionReceipt pause =
        engine->Submit(SafePauseRequest{.policy = Policy()});
    ASSERT_TRUE(pause.accepted) << pause.error.message;

    engine->Pump();
    const auto terminals = TakeTerminals(*engine);
    ASSERT_EQ(terminals.size(), 2u);
    EXPECT_EQ(terminals[0].operation_id, running.operation_id);
    EXPECT_EQ(terminals[0].kind, ExecutionOperationKind::InteractiveResume);
    EXPECT_EQ(terminals[0].status, ExecutionTerminalStatus::Paused);
    EXPECT_EQ(terminals[1].operation_id, pause.operation_id);
    EXPECT_EQ(terminals[1].kind, ExecutionOperationKind::SafePause);
    EXPECT_EQ(terminals[1].status, ExecutionTerminalStatus::Paused);
    EXPECT_EQ(engine->snapshot().activity, ExecutionActivity::IdlePaused);
}

TEST_F(ExecutionEngineFixture, CompletesRequestedFrameCount)
{
    CreateEngine();

    const ExecutionSubmissionReceipt frames =
        engine->Submit(StepFramesRequest{.policy = Policy(), .count = 3});
    ASSERT_TRUE(frames.accepted) << frames.error.message;
    const auto frame_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(frame_terminal.has_value());
    EXPECT_EQ(
        frame_terminal->status,
        ExecutionTerminalStatus::StepsCompleted);
    EXPECT_EQ(frame_terminal->completed_count, 3u);
    EXPECT_EQ(CountCall(execution_control->Calls(), "frame_step"), 3u);
}

TEST_F(ExecutionEngineFixture, SafePauseStopsARunningBackend)
{
    CreateEngine();
    execution_control->SetCoreState(BackendCoreState::Running);

    const ExecutionSubmissionReceipt pause =
        engine->Submit(SafePauseRequest{.policy = Policy()});
    ASSERT_TRUE(pause.accepted) << pause.error.message;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::Paused);
    EXPECT_EQ(terminal->evidence.core_state, BackendCoreState::Paused);
    EXPECT_EQ(CountCall(execution_control->Calls(), "pause"), 1u);
}

TEST_F(ExecutionEngineFixture, ContinueTimesOutAgainstTheInjectedClock)
{
    CreateEngine();
    ExecutionRequestPolicy policy = Policy(kEpoch, 100ms);

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    EXPECT_TRUE(engine->has_active_operation());

    now += 101ms;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::TimedOut);
    EXPECT_EQ(terminal->evidence.core_state, BackendCoreState::Paused);
    EXPECT_EQ(CountCall(execution_control->Calls(), "pause"), 1u);
}

TEST_F(ExecutionEngineFixture, ViWarmupAndProgressDelayAStallTerminal)
{
    CreateEngine();
    ExecutionRequestPolicy policy = Policy(kEpoch, 2s);
    policy.vi_stall = {
        .enabled = true,
        .warmup = 100ms,
        .maximum_stall = 50ms,
    };

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    now += 90ms;
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    execution_control->SetViCount(1);
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    now += 40ms;
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    now += 21ms;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::ViStalled);
    EXPECT_EQ(terminal->evidence.vi_count, 1u);
    EXPECT_EQ(terminal->evidence.core_state, BackendCoreState::Paused);
}

TEST_F(ExecutionEngineFixture, ContinueCompletesFromTheSharedRouterReceipt)
{
    CreateEngine();

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    const auto decision = physical_backend.InjectJitPcStop(kWakePc);
    EXPECT_TRUE(decision.request_break);
    auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    engine->HandleStopPointReceipt(std::move(receipts.front()));

    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::RequestedCompletion);
    ASSERT_TRUE(terminal->stop.has_value());
    ASSERT_TRUE(terminal->stop->event.has_value());
    EXPECT_EQ(terminal->stop->event->evidence.hit_pc, kWakePc);
    EXPECT_EQ(terminal->state_epoch, kEpoch);
}

TEST_F(
    ExecutionEngineFixture,
    ContinueIgnoresTransientUnconfirmedPauseBeforeARoutedCompletion)
{
    CreateEngine();

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    BackendExecutionSnapshot transient =
        execution_control->Snapshot();
    transient.core_state = BackendCoreState::Paused;
    transient.pause_confirmed = false;
    execution_control->SetSnapshot(std::move(transient));
    engine->Pump();

    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    execution_control->SetCoreState(BackendCoreState::Running);
    engine->Pump();
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    const auto decision = physical_backend.InjectJitPcStop(kWakePc);
    EXPECT_TRUE(decision.request_break);
    auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    engine->HandleStopPointReceipt(std::move(receipts.front()));

    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::RequestedCompletion);
    ASSERT_TRUE(terminal->stop.has_value());
    ASSERT_TRUE(terminal->stop->event.has_value());
    EXPECT_EQ(terminal->stop->event->evidence.hit_pc, kWakePc);
}

TEST_F(ExecutionEngineFixture, RestoresThePriorThrottleStateOnCompletion)
{
    CreateEngine();
    ExecutionRequestPolicy policy = Policy();
    policy.throttle = ExecutionThrottlePolicy::RequireDisabled;

    const ExecutionSubmissionReceipt submission =
        engine->Submit(
            StepFramesRequest{.policy = std::move(policy), .count = 1});
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::StepsCompleted);

    const auto calls = execution_control->Calls();
    const auto disabled = std::find(
        calls.begin(),
        calls.end(),
        "throttle_disable");
    const auto step = std::find(calls.begin(), calls.end(), "frame_step");
    const auto restored = std::find(
        calls.begin(),
        calls.end(),
        "throttle_enable");
    ASSERT_NE(disabled, calls.end());
    ASSERT_NE(step, calls.end());
    ASSERT_NE(restored, calls.end());
    EXPECT_LT(
        std::distance(calls.begin(), disabled),
        std::distance(calls.begin(), step));
    EXPECT_LT(
        std::distance(calls.begin(), step),
        std::distance(calls.begin(), restored));
    EXPECT_FALSE(execution_control->Snapshot().throttle_disabled);
}

TEST_F(ExecutionEngineFixture, MovieEndCompletesAccordingToPolicy)
{
    execution_control->SetMovieState(BackendMovieState::Inactive);
    CreateEngine();
    ExecutionRequestPolicy policy = Policy();
    policy.movie_ended = MovieEndedPolicy::Complete;

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
    });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    execution_control->SetMovieState(BackendMovieState::Playing);
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    execution_control->SetMovieState(BackendMovieState::Ended);
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::MovieEnded);
    EXPECT_EQ(
        terminal->evidence.movie_state,
        ExecutionMovieState::Ended);
    EXPECT_EQ(terminal->evidence.core_state, BackendCoreState::Paused);
}

TEST_F(
    ExecutionEngineFixture,
    AcceptedWakePrecedesCancellationAndAnExpiredMaintenanceBudget)
{
    CreateEngine();
    CancellationSource cancellation(InvocationId(77));
    ExecutionRequestPolicy policy = Policy(kEpoch, 100ms);
    policy.cancellation = cancellation.token();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    now += 101ms;
    ASSERT_TRUE(cancellation.request_cancellation(
        CancellationReason::ExternalRequest));
    execution_control->SetCoreState(BackendCoreState::Paused);
    (void)physical_backend.InjectJitPcStop(kWakePc);
    auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    engine->HandleStopPointReceipt(std::move(receipts.front()));

    const auto terminal = TakeTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::RequestedCompletion);
    EXPECT_FALSE(
        engine->Cancel(CancellationReason::ExternalRequest).accepted);
}

TEST_F(
    ExecutionEngineFixture,
    CancellationAcceptedBeforeAStopAndTimeoutRemainsTheOnlyTerminal)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(kEpoch, 100ms),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    const ExecutionControlReceipt cancellation =
        engine->Cancel(CancellationReason::ExternalRequest);
    ASSERT_TRUE(cancellation.accepted) << cancellation.error.message;
    now += 101ms;

    (void)physical_backend.InjectJitPcStop(kWakePc);
    auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    engine->HandleStopPointReceipt(std::move(receipts.front()));

    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::Cancelled);
    EXPECT_FALSE(TakeTerminal(*engine).has_value());
}

TEST_F(ExecutionEngineFixture, RejectsStateEpochMismatchWithoutAdvancing)
{
    CreateEngine();

    const ExecutionSubmissionReceipt submission =
        engine->Submit(StepFramesRequest{
            .policy = Policy(StateEpoch(8)),
            .count = 1,
        });

    EXPECT_FALSE(submission.accepted);
    EXPECT_EQ(
        submission.error.code,
        ExecutionErrorCode::StateEpochMismatch);
    EXPECT_EQ(CountCall(execution_control->Calls(), "frame_step"), 0u);
}

TEST_F(
    ExecutionEngineFixture,
    ResumeParentRestoresTheContinueAndItsFrozenActiveBudget)
{
    CreateEngine(nullptr, {Handler()});
    ExecutionRequestPolicy policy = Policy(kEpoch, 100ms);
    policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt parent =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(parent.accepted) << parent.error.message;

    now += 40ms;
    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(RouteInterruption());
    const ExecutionSnapshot suspended = engine->snapshot();
    ASSERT_TRUE(suspended.active_interruption_frame.has_value());
    EXPECT_EQ(suspended.interruption_depth, 1u);
    EXPECT_EQ(suspended.activity, ExecutionActivity::HandlingInterruption);

    ExecutionRequestPolicy child_policy = Policy();
    child_policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt child =
        engine->SubmitInterruptionChild(
            *suspended.active_interruption_frame,
            ContinueUntilRequest{
                .policy = std::move(child_policy),
                .wake_group = WakeGroup(),
            });
    ASSERT_TRUE(child.accepted) << child.error.message;

    now += 201ms;
    const auto child_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(child_terminal.has_value());
    EXPECT_EQ(child_terminal->operation_id, child.operation_id);
    EXPECT_EQ(child_terminal->status, ExecutionTerminalStatus::TimedOut);
    EXPECT_EQ(engine->snapshot().activity, ExecutionActivity::HandlingInterruption);

    // Handler decision time is bounded while it is awaiting a command.
    // Resume within that bound; the parent's own budget remained frozen for
    // the entire interruption.
    now += 100ms;
    const ExecutionControlReceipt resumed =
        engine->CompleteInterruptionHandler(
            *suspended.active_interruption_frame,
            InterruptionHandlerOutcome::ResumeParent);
    ASSERT_TRUE(resumed.accepted) << resumed.error.message;
    EXPECT_EQ(engine->snapshot().activity, ExecutionActivity::Continuing);

    now += 59ms;
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    now += 2ms;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->operation_id, parent.operation_id);
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::TimedOut);
}

TEST_F(
    ExecutionEngineFixture,
    ParkedParentWakeIsPassiveDuringFrameChildAndRestoredOnResume)
{
    InterruptionHandlerDescriptor handler = Handler();
    handler.allowed_child_operations.push_back(
        ExecutionOperationKind::StepFrames);
    CreateEngine(nullptr, {std::move(handler)});

    ExecutionRequestPolicy parent_policy = Policy();
    parent_policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt parent =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(parent_policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(parent.accepted) << parent.error.message;

    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(RouteInterruption());
    const auto frame = engine->snapshot().active_interruption_frame;
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(interruption_group.has_value());
    ASSERT_TRUE(interruption_group->Release().ok);
    interruption_group.reset();

    const ExecutionSubmissionReceipt child =
        engine->SubmitInterruptionChild(
            *frame,
            StepFramesRequest{.policy = Policy(), .count = 1});
    ASSERT_TRUE(child.accepted) << child.error.message;

    const auto parked_hit =
        physical_backend.InjectJitPcStop(kWakePc);
    EXPECT_FALSE(parked_hit.request_break);
    auto parked_receipts = router.DrainIngress();
    ASSERT_EQ(parked_receipts.size(), 1u);
    EXPECT_EQ(parked_receipts.front().terminal, StopRouteTerminal::None);
    ASSERT_EQ(parked_receipts.front().deliveries.size(), 1u);
    EXPECT_EQ(
        parked_receipts.front().deliveries.front().delivery,
        StopDeliveryMode::Observe);
    engine->HandleStopPointReceipt(std::move(parked_receipts.front()));

    const auto child_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(child_terminal.has_value());
    EXPECT_EQ(child_terminal->operation_id, child.operation_id);
    EXPECT_EQ(
        child_terminal->status,
        ExecutionTerminalStatus::StepsCompleted);

    const ExecutionControlReceipt resumed =
        engine->CompleteInterruptionHandler(
            *frame,
            InterruptionHandlerOutcome::ResumeParent);
    ASSERT_TRUE(resumed.accepted) << resumed.error.message;

    const auto restored_hit =
        physical_backend.InjectJitPcStop(kWakePc);
    EXPECT_TRUE(restored_hit.request_break);
    execution_control->SetCoreState(BackendCoreState::Paused);
    auto restored_receipts = router.DrainIngress();
    ASSERT_EQ(restored_receipts.size(), 1u);
    EXPECT_EQ(
        restored_receipts.front().terminal,
        StopRouteTerminal::WokeForeground);
    engine->HandleStopPointReceipt(std::move(restored_receipts.front()));

    const auto parent_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(parent_terminal.has_value());
    EXPECT_EQ(parent_terminal->operation_id, parent.operation_id);
    EXPECT_EQ(
        parent_terminal->status,
        ExecutionTerminalStatus::RequestedCompletion);
}

TEST_F(
    ExecutionEngineFixture,
    NestedHandlerDecisionBudgetsFreezeWhileTheirChildrenRun)
{
    InterruptionHandlerDescriptor outer = Handler("handler.outer");
    outer.permitted_nested_keys = {"handler.inner"};
    InterruptionHandlerDescriptor inner = Handler("handler.inner");
    inner.allowed_child_operations.push_back(
        ExecutionOperationKind::StepFrames);
    CreateEngine(nullptr, {std::move(outer), std::move(inner)});

    ExecutionRequestPolicy root_policy = Policy();
    root_policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt root =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(root_policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(root.accepted) << root.error.message;

    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(RouteInterruption("handler.outer"));
    const auto outer_frame = engine->snapshot().active_interruption_frame;
    ASSERT_TRUE(outer_frame.has_value());

    ExecutionRequestPolicy outer_child_policy = Policy();
    outer_child_policy.interruptions =
        ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt outer_child =
        engine->SubmitInterruptionChild(
            *outer_frame,
            ContinueUntilRequest{
                .policy = std::move(outer_child_policy),
                .wake_group = WakeGroup(),
            });
    ASSERT_TRUE(outer_child.accepted) << outer_child.error.message;

    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(RouteInterruption("handler.inner"));
    const auto inner_frame = engine->snapshot().active_interruption_frame;
    ASSERT_TRUE(inner_frame.has_value());

    const ExecutionSubmissionReceipt inner_child =
        engine->SubmitInterruptionChild(
            *inner_frame,
            StepFramesRequest{.policy = Policy(), .count = 1});
    ASSERT_TRUE(inner_child.accepted) << inner_child.error.message;

    // The fake backend normally completes a frame immediately. Hold this
    // child in its running phase so its own bounded budget, rather than
    // instantaneous fake completion, decides the result.
    execution_control->SetCoreState(BackendCoreState::Running);
    now += 500ms;
    const auto inner_child_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(inner_child_terminal.has_value());
    EXPECT_EQ(inner_child_terminal->operation_id, inner_child.operation_id);
    EXPECT_EQ(
        inner_child_terminal->status,
        ExecutionTerminalStatus::TimedOut);

    const ExecutionControlReceipt inner_resumed =
        engine->CompleteInterruptionHandler(
            *inner_frame,
            InterruptionHandlerOutcome::ResumeParent);
    ASSERT_TRUE(inner_resumed.accepted) << inner_resumed.error.message;

    ASSERT_TRUE(interruption_group.has_value());
    ASSERT_TRUE(interruption_group->Release().ok);
    interruption_group.reset();
    const auto child_hit =
        physical_backend.InjectJitPcStop(kWakePc);
    ASSERT_TRUE(child_hit.request_break);
    execution_control->SetCoreState(BackendCoreState::Paused);
    auto child_receipts = router.DrainIngress();
    ASSERT_EQ(child_receipts.size(), 1u);
    engine->HandleStopPointReceipt(std::move(child_receipts.front()));
    const auto outer_child_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(outer_child_terminal.has_value());
    EXPECT_EQ(
        outer_child_terminal->operation_id,
        outer_child.operation_id);

    const ExecutionControlReceipt outer_resumed =
        engine->CompleteInterruptionHandler(
            *outer_frame,
            InterruptionHandlerOutcome::ResumeParent);
    ASSERT_TRUE(outer_resumed.accepted) << outer_resumed.error.message;

    // Resuming at the same retained PC consumes exactly one source
    // instruction re-entry without disabling the shared physical site.
    const auto suppressed_root_reentry =
        physical_backend.InjectJitPcStop(kWakePc);
    EXPECT_FALSE(suppressed_root_reentry.request_break);
    for (StopRouteReceipt& receipt : router.DrainIngress())
        engine->HandleStopPointReceipt(std::move(receipt));

    const auto root_hit =
        physical_backend.InjectJitPcStop(kWakePc);
    ASSERT_TRUE(root_hit.request_break);
    execution_control->SetCoreState(BackendCoreState::Paused);
    auto root_receipts = router.DrainIngress();
    ASSERT_EQ(root_receipts.size(), 1u);
    engine->HandleStopPointReceipt(std::move(root_receipts.front()));
    const auto root_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(root_terminal.has_value());
    EXPECT_EQ(root_terminal->operation_id, root.operation_id);
}

TEST_F(
    ExecutionEngineFixture,
    CancellationDuringInterruptionChildPublishesOneParentTerminal)
{
    InterruptionHandlerDescriptor handler = Handler();
    handler.allowed_child_operations.push_back(
        ExecutionOperationKind::StepFrames);
    CreateEngine(nullptr, {std::move(handler)});

    ExecutionRequestPolicy parent_policy = Policy();
    parent_policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt parent =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(parent_policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(parent.accepted) << parent.error.message;
    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(RouteInterruption());
    const auto frame = engine->snapshot().active_interruption_frame;
    ASSERT_TRUE(frame.has_value());

    const ExecutionSubmissionReceipt child =
        engine->SubmitInterruptionChild(
            *frame,
            StepFramesRequest{.policy = Policy(), .count = 1});
    ASSERT_TRUE(child.accepted) << child.error.message;

    const ExecutionControlReceipt cancelled =
        engine->Cancel(CancellationReason::ExternalRequest);
    ASSERT_TRUE(cancelled.accepted) << cancelled.error.message;
    const auto terminals = TakeTerminals(*engine);
    ASSERT_EQ(terminals.size(), 2u);
    EXPECT_EQ(
        std::count_if(
            terminals.begin(),
            terminals.end(),
            [&](const auto& terminal) {
                return terminal.operation_id == parent.operation_id;
            }),
        1);
    EXPECT_EQ(
        std::count_if(
            terminals.begin(),
            terminals.end(),
            [&](const auto& terminal) {
                return terminal.operation_id == child.operation_id;
            }),
        1);
    EXPECT_TRUE(std::all_of(
        terminals.begin(),
        terminals.end(),
        [](const auto& terminal) {
            return terminal.status == ExecutionTerminalStatus::Cancelled;
        }));
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());
}

TEST_F(
    ExecutionEngineFixture,
    InfrastructureFailureIsDistinctFromForbiddenNestedHandlers)
{
    InterruptionHandlerDescriptor outer = Handler("handler.outer");
    outer.allow_self_recursion = false;
    outer.permitted_nested_keys.clear();
    CreateEngine(
        nullptr,
        {outer, Handler("handler.undeclared")});

    const auto submit_parent = [&]() {
        ExecutionRequestPolicy policy = Policy();
        policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
        return engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    };

    const ExecutionSubmissionReceipt infrastructure_parent =
        submit_parent();
    ASSERT_TRUE(infrastructure_parent.accepted);
    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(RouteInterruption("handler.outer"));
    const auto infrastructure_frame =
        engine->snapshot().active_interruption_frame;
    ASSERT_TRUE(infrastructure_frame.has_value());
    ASSERT_TRUE(engine->CompleteInterruptionHandler(
        *infrastructure_frame,
        InterruptionHandlerOutcome::InfrastructureFailure,
        "test infrastructure failure").accepted);
    const auto infrastructure_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(infrastructure_terminal.has_value());
    EXPECT_EQ(
        infrastructure_terminal->status,
        ExecutionTerminalStatus::InterruptionFailed);
    EXPECT_EQ(
        infrastructure_terminal->error.code,
        ExecutionErrorCode::BackendFailure);
    EXPECT_EQ(
        infrastructure_terminal->integrity,
        BackendIntegrity::Unknown);

    for (const std::string nested_key :
        {"handler.outer", "handler.undeclared"})
    {
        const ExecutionSubmissionReceipt parent = submit_parent();
        ASSERT_TRUE(parent.accepted);
        execution_control->SetCoreState(BackendCoreState::Paused);
        engine->HandleStopPointReceipt(
            RouteInterruption("handler.outer"));
        const auto frame = engine->snapshot().active_interruption_frame;
        ASSERT_TRUE(frame.has_value());

        ExecutionRequestPolicy child_policy = Policy();
        child_policy.interruptions =
            ExecutionInterruptionPolicy::AllowKnown;
        const ExecutionSubmissionReceipt child =
            engine->SubmitInterruptionChild(
                *frame,
                ContinueUntilRequest{
                    .policy = std::move(child_policy),
                    .wake_group = WakeGroup(),
                });
        ASSERT_TRUE(child.accepted);
        execution_control->SetCoreState(BackendCoreState::Paused);
        engine->HandleStopPointReceipt(RouteInterruption(nested_key));
        const auto child_terminal = DrainTerminal(*engine);
        ASSERT_TRUE(child_terminal.has_value());
        EXPECT_EQ(
            child_terminal->status,
            ExecutionTerminalStatus::InterruptionUnavailable);
        EXPECT_EQ(
            child_terminal->error.code,
            ExecutionErrorCode::InterruptionPolicyViolation);

        ASSERT_TRUE(engine->CompleteInterruptionHandler(
            *frame,
            InterruptionHandlerOutcome::AbortParent).accepted);
        ASSERT_TRUE(DrainTerminal(*engine).has_value());
    }
}

TEST_F(ExecutionEngineFixture, AbortParentProducesADistinctTerminal)
{
    CreateEngine(nullptr, {Handler()});
    ExecutionRequestPolicy policy = Policy();
    policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt parent =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(parent.accepted) << parent.error.message;

    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(RouteInterruption());
    const auto frame = engine->snapshot().active_interruption_frame;
    ASSERT_TRUE(frame.has_value());

    const ExecutionControlReceipt aborted =
        engine->CompleteInterruptionHandler(
            *frame,
            InterruptionHandlerOutcome::AbortParent,
            "test handler rejected the interruption");
    ASSERT_TRUE(aborted.accepted) << aborted.error.message;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->operation_id, parent.operation_id);
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::InterruptionAborted);
    EXPECT_EQ(
        terminal->error.code,
        ExecutionErrorCode::InterruptionPolicyViolation);
}

TEST_F(ExecutionEngineFixture, UnknownInterruptionHandlerFailsClosed)
{
    CreateEngine();
    ExecutionRequestPolicy policy = Policy();
    policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt parent =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(parent.accepted) << parent.error.message;

    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(
        RouteInterruption("not.registered"));
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::InterruptionUnavailable);
    EXPECT_EQ(
        terminal->error.code,
        ExecutionErrorCode::InterruptionUnavailable);
}

TEST_F(ExecutionEngineFixture, NestedInterruptionHonorsTheDescriptorDepthLimit)
{
    CreateEngine(nullptr, {Handler("test.dialog", 1)});
    ExecutionRequestPolicy parent_policy = Policy();
    parent_policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt parent =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(parent_policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(parent.accepted) << parent.error.message;

    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(RouteInterruption());
    const auto frame = engine->snapshot().active_interruption_frame;
    ASSERT_TRUE(frame.has_value());

    ExecutionRequestPolicy child_policy = Policy();
    child_policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt child =
        engine->SubmitInterruptionChild(
            *frame,
            ContinueUntilRequest{
                .policy = std::move(child_policy),
                .wake_group = WakeGroup(),
            });
    ASSERT_TRUE(child.accepted) << child.error.message;

    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(
        RouteInterruption("test.dialog"));
    const auto child_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(child_terminal.has_value());
    EXPECT_EQ(child_terminal->operation_id, child.operation_id);
    EXPECT_EQ(
        child_terminal->status,
        ExecutionTerminalStatus::InterruptionDepthExceeded);
    EXPECT_EQ(
        child_terminal->error.code,
        ExecutionErrorCode::InterruptionDepthExceeded);

    const ExecutionControlReceipt parent_cleanup =
        engine->CompleteInterruptionHandler(
            *frame,
            InterruptionHandlerOutcome::AbortParent);
    ASSERT_TRUE(parent_cleanup.accepted) << parent_cleanup.error.message;
    const auto parent_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(parent_terminal.has_value());
    EXPECT_EQ(parent_terminal->operation_id, parent.operation_id);
    EXPECT_EQ(
        parent_terminal->status,
        ExecutionTerminalStatus::InterruptionAborted);
}

TEST_F(ExecutionEngineFixture, InterruptionNestingIsHardCappedAtEightFrames)
{
    CreateEngine(nullptr, {Handler("test.dialog", 8)});
    ExecutionRequestPolicy root_policy = Policy();
    root_policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt root =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(root_policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(root.accepted) << root.error.message;

    for (std::uint8_t depth = 1; depth <= 8; ++depth)
    {
        execution_control->SetCoreState(BackendCoreState::Paused);
        engine->HandleStopPointReceipt(
            RouteInterruption("test.dialog"));
        const ExecutionSnapshot suspended = engine->snapshot();
        ASSERT_TRUE(suspended.active_interruption_frame.has_value());
        EXPECT_EQ(suspended.interruption_depth, depth);

        if (depth == 8)
            break;

        ExecutionRequestPolicy child_policy = Policy();
        child_policy.interruptions =
            ExecutionInterruptionPolicy::AllowKnown;
        const ExecutionSubmissionReceipt child =
            engine->SubmitInterruptionChild(
                *suspended.active_interruption_frame,
                ContinueUntilRequest{
                    .policy = std::move(child_policy),
                    .wake_group = WakeGroup(),
                });
        ASSERT_TRUE(child.accepted) << child.error.message;
    }

    const auto eighth_frame =
        engine->snapshot().active_interruption_frame;
    ASSERT_TRUE(eighth_frame.has_value());
    ExecutionRequestPolicy ninth_policy = Policy();
    ninth_policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt ninth =
        engine->SubmitInterruptionChild(
            *eighth_frame,
            ContinueUntilRequest{
                .policy = std::move(ninth_policy),
                .wake_group = WakeGroup(),
            });
    ASSERT_TRUE(ninth.accepted) << ninth.error.message;

    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(
        RouteInterruption("test.dialog"));
    const auto depth_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(depth_terminal.has_value());
    EXPECT_EQ(depth_terminal->operation_id, ninth.operation_id);
    EXPECT_EQ(
        depth_terminal->status,
        ExecutionTerminalStatus::InterruptionDepthExceeded);

    for (std::uint8_t remaining = 8; remaining > 0; --remaining)
    {
        const ExecutionSnapshot suspended = engine->snapshot();
        ASSERT_TRUE(suspended.active_interruption_frame.has_value());
        const ExecutionControlReceipt unwind =
            engine->CompleteInterruptionHandler(
                *suspended.active_interruption_frame,
                InterruptionHandlerOutcome::AbortParent);
        ASSERT_TRUE(unwind.accepted) << unwind.error.message;
        ASSERT_TRUE(DrainTerminal(*engine).has_value());
        EXPECT_EQ(engine->snapshot().interruption_depth, remaining - 1);
    }
}

TEST_F(
    ExecutionEngineFixture,
    InputAdvancePreparesBeforeFrameAndObservesAcknowledgementAfterward)
{
    FakeInputAdvancePort input(execution_control);
    CreateEngine(&input);

    const InputAdvanceBindingId binding(44);
    const ExecutionSubmissionReceipt submission =
        engine->Submit(InputSynchronizedAdvanceRequest{
            .policy = Policy(),
            .binding = binding,
            .maximum_advances = 1,
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::StepsCompleted);
    EXPECT_EQ(terminal->completed_count, 1u);
    EXPECT_EQ(
        input.calls,
        (std::vector<std::string>{
            "validate",
            "prepare",
            "observe",
            "complete"}));
    ASSERT_EQ(input.frame_steps_seen.size(), 4u);
    EXPECT_EQ(input.frame_steps_seen[0], 0u);
    EXPECT_EQ(input.frame_steps_seen[1], 0u);
    EXPECT_EQ(input.frame_steps_seen[2], 1u);
    EXPECT_EQ(input.frame_steps_seen[3], 1u);
    EXPECT_EQ(input.last_binding, binding);
    EXPECT_EQ(input.last_epoch, kEpoch);
    EXPECT_EQ(input.last_ordinal, 0u);
    EXPECT_EQ(input.observed_publication, input.last_publication);
    ASSERT_TRUE(terminal->input_publication);
    EXPECT_EQ(
        terminal->input_publication->lease,
        InputLeaseId(77));
    EXPECT_EQ(
        terminal->input_publication->publication,
        input.last_publication);
    EXPECT_EQ(terminal->input_publication->epoch, kEpoch);
    EXPECT_EQ(
        terminal->input_publication->frame.buttons,
        1u);
    EXPECT_EQ(CountCall(input.calls, "validate"), 1u);
    const auto validation =
        std::ranges::find(input.calls, "validate");
    const auto preparation =
        std::ranges::find(input.calls, "prepare");
    ASSERT_NE(validation, input.calls.end());
    ASSERT_NE(preparation, input.calls.end());
    EXPECT_LT(validation, preparation);
}

TEST_F(ExecutionEngineFixture, InputAdvanceRetryUsesAFreshPublication)
{
    FakeInputAdvancePort input(execution_control);
    input.acknowledgement_decisions = {
        InputAdvanceDecision::Retry,
        InputAdvanceDecision::Complete,
    };
    CreateEngine(&input);

    const ExecutionSubmissionReceipt submission =
        engine->Submit(InputSynchronizedAdvanceRequest{
            .policy = Policy(),
            .binding = InputAdvanceBindingId(45),
            .maximum_advances = 2,
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::StepsCompleted);
    EXPECT_EQ(terminal->completed_count, 2u);
    EXPECT_EQ(CountCall(input.calls, "validate"), 1u);
    EXPECT_EQ(CountCall(input.calls, "prepare"), 2u);
    EXPECT_EQ(CountCall(input.calls, "observe"), 2u);
    EXPECT_EQ(CountCall(input.calls, "complete"), 1u);
    EXPECT_EQ(CountCall(input.calls, "cancel"), 0u);
    ASSERT_EQ(input.prepared_publications.size(), 2u);
    EXPECT_NE(
        input.prepared_publications[0],
        input.prepared_publications[1]);
    ASSERT_TRUE(terminal->input_publication);
    EXPECT_EQ(
        terminal->input_publication->publication,
        input.prepared_publications.back());
    EXPECT_EQ(
        terminal->input_publication->frame.buttons,
        2u);
    EXPECT_EQ(CountCall(execution_control->Calls(), "frame_step"), 2u);
}

TEST_F(ExecutionEngineFixture, InputAdvanceCancellationUnwindsBeforeAcknowledgement)
{
    FakeInputAdvancePort input(execution_control);
    CreateEngine(&input);

    const ExecutionSubmissionReceipt submission =
        engine->Submit(InputSynchronizedAdvanceRequest{
            .policy = Policy(),
            .binding = InputAdvanceBindingId(46),
            .maximum_advances = 2,
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    const ExecutionControlReceipt cancelled =
        engine->Cancel(CancellationReason::ExternalRequest);
    ASSERT_TRUE(cancelled.accepted) << cancelled.error.message;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::Cancelled);
    EXPECT_EQ(
        input.calls,
        (std::vector<std::string>{"validate", "prepare", "cancel"}));
    EXPECT_EQ(CountCall(execution_control->Calls(), "frame_step"), 1u);
}

TEST_F(ExecutionEngineFixture, InputAdvanceWithoutAPortIsUnsupported)
{
    CreateEngine();

    const ExecutionSubmissionReceipt submission =
        engine->Submit(InputSynchronizedAdvanceRequest{
            .policy = Policy(),
            .binding = InputAdvanceBindingId(47),
            .maximum_advances = 1,
        });
    EXPECT_FALSE(submission.accepted);
    EXPECT_EQ(submission.error.code, ExecutionErrorCode::Unsupported);
    EXPECT_FALSE(DrainTerminal(*engine).has_value());
    EXPECT_EQ(CountCall(execution_control->Calls(), "frame_step"), 0u);
}

} // namespace
