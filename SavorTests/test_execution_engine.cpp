#include <gtest/gtest.h>

#include "Runner/Runtime/Execution/ExecutionEngine.h"
#include "Runner/Runtime/Services/Movie/MovieService.h"
#include "Runner/Runtime/Worksets/SavestateArtifactFinalizer.h"
#include "Tas/DtmFile.h"
#include "common/FakeExecutionBackend.h"
#include "common/FakePhysicalStopBackend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace savor::runtime;
using namespace savor::test_support;

constexpr WorksetEpoch kEpoch{7};
constexpr std::uint32_t kWakePc = 0x801DC288u;
constexpr std::uint32_t kInterruptionPc = 0x801DC28Cu;

std::uint64_t AtomicHostTicks(void* context) noexcept
{
    return static_cast<std::atomic<std::uint64_t>*>(context)
        ->load(std::memory_order_acquire);
}

TEST(
    HostActivityTracker,
    OverlappingScopesRetainIndependentCompletionEvidence)
{
    std::atomic<std::uint64_t> ticks{0};
    HostActivityTracker tracker(&AtomicHostTicks, &ticks);

    auto first = tracker.Track();
    ticks.store(5'000, std::memory_order_release);
    auto second = tracker.Track();
    ticks.store(15'000, std::memory_order_release);
    first.Reset();
    EXPECT_EQ(tracker.snapshot().in_flight, 1u);
    ticks.store(25'000, std::memory_order_release);
    second.Reset();

    EXPECT_EQ(tracker.snapshot().in_flight, 0u);
    const auto completed = tracker.DrainCompletedActivities();
    ASSERT_EQ(completed.count, 2u);
    EXPECT_EQ(completed.overflow_count, 0u);
    EXPECT_LT(
        completed.activities[0].sequence,
        completed.activities[1].sequence);
    EXPECT_EQ(completed.activities[0].elapsed.count(), 15000);
    EXPECT_EQ(completed.activities[1].elapsed.count(), 20000);
}

class HealthTestTemporaryDirectory final
{
public:
    HealthTestTemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::temp_directory_path() /
            ("savor-execution-health-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~HealthTestTemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::size_t CountCall(
    const std::vector<std::string>& calls,
    const std::string& expected)
{
    return static_cast<std::size_t>(std::count(
        calls.begin(),
        calls.end(),
        expected));
}

ExecutionRequestPolicy Policy(WorksetEpoch epoch = kEpoch)
{
    ExecutionRequestPolicy policy;
    policy.expected_epoch = epoch;
    return policy;
}

class ExecutionMovieBackend final : public IMovieBackendPort
{
public:
    MoviePlaybackPrepareResult PrepareReadOnlyPlaybackForRestart(
        const std::filesystem::path&) override
    {
        return {
            MovieBackendResult::Failure("unused movie preparation"),
            std::nullopt};
    }

    MovieBackendResult StopCoreForPreparedReadOnlyMovie() override
    {
        return MovieBackendResult::Failure("unused movie core stop");
    }

    MovieBackendResult StartPreparedReadOnlyMovieCorePaused() override
    {
        return MovieBackendResult::Failure("unused movie core start");
    }

    MovieBackendResult ActivatePreparedReadOnlyMoviePlayback() override
    {
        return MovieBackendResult::Failure("unused movie activation");
    }

    MovieBackendResult DiscardPreparedReadOnlyMovie() noexcept override
    {
        observation = InactiveObservation();
        return MovieBackendResult::Success();
    }

    MovieBackendResult StopMovie() noexcept override
    {
        observation = InactiveObservation();
        return MovieBackendResult::Success();
    }

    MovieBackendResult BeginRecording() override
    {
        observation.playing = false;
        observation.recording = true;
        observation.read_only = false;
        return MovieBackendResult::Success();
    }

    MovieBackendResult BranchReadOnlyPlaybackToRecording() override
    {
        if (!observation.playing || observation.recording ||
            !observation.read_only)
        {
            return MovieBackendResult::Failure(
                "read-only playback is unavailable");
        }
        observation.playing = false;
        observation.recording = true;
        observation.read_only = false;
        return MovieBackendResult::Success();
    }

    MovieRecordingFinalizeResult FinalizeRecording(
        const std::filesystem::path&) override
    {
        return {
            MovieBackendResult::Failure("unused recording finalization"),
            std::nullopt};
    }

    MovieBackendResult CancelRecording() noexcept override
    {
        observation = InactiveObservation();
        return MovieBackendResult::Success();
    }

    MovieBackendObservation ObserveMovieWhilePaused() const override
    {
        ++observation_count;
        return observation;
    }

    MovieBackendResult AcquirePauseAtPlaybackEnd() override
    {
        pause_at_playback_end = true;
        return MovieBackendResult::Success();
    }

    MovieBackendResult ReleasePauseAtPlaybackEnd() noexcept override
    {
        pause_at_playback_end = false;
        return MovieBackendResult::Success();
    }

    MovieCheckpointBackendResult CaptureRecordingCheckpoint() override
    {
        return {
            MovieBackendResult::Failure("unused checkpoint capture"),
            {}};
    }

    MovieBackendResult PrepareSavestateRestore(
        const SavestateMovieRestoreContext&) override
    {
        return MovieBackendResult::Success();
    }

    MovieBackendResult CommitSavestateRestore(
        const SavestateMovieRestoreContext& context) override
    {
        observation = InactiveObservation();
        if (!context.movie)
            return MovieBackendResult::Success();

        observation.current_frame = context.movie->current_frame;
        observation.current_input_count =
            context.movie->current_input_count;
        if (context.movie->mode == MovieCheckpointMode::ReadOnlyPlayback)
        {
            observation.playing = true;
            observation.read_only = true;
        }
        else if (context.movie->mode == MovieCheckpointMode::Recording)
        {
            observation.recording = true;
            observation.read_only = false;
        }
        return MovieBackendResult::Success();
    }

    MovieBackendResult RollbackSavestateRestore(
        const SavestateMovieRestoreContext&) noexcept override
    {
        return MovieBackendResult::Success();
    }

    void SetInputCount(std::uint64_t input_count) noexcept
    {
        observation.current_input_count = input_count;
    }

    void EndPlayback() noexcept
    {
        observation.playing = false;
    }

    [[nodiscard]] static MovieBackendObservation InactiveObservation()
    {
        return {
            .result = MovieBackendResult::Success(),
            .read_only = true,
        };
    }

    MovieBackendObservation observation = InactiveObservation();
    mutable std::uint64_t observation_count = 0;
    bool pause_at_playback_end = false;
};

class ExecutionMovieReservations final : public IMovieInputReservationPort
{
public:
    MovieInputReservationReceipt AcquireUnsuspendableMovieReservation() override
    {
        if (held)
        {
            return {
                MovieServiceResult::Failure(
                    MovieServiceErrorCode::ReservationFailure,
                    "movie reservation is already held"),
                {}};
        }
        held = MovieReservationId(1);
        return {MovieServiceResult::Success(), held};
    }

    MovieServiceResult ReleaseMovieReservation(
        MovieReservationId reservation) noexcept override
    {
        if (!held || reservation != held)
        {
            return MovieServiceResult::Failure(
                MovieServiceErrorCode::ReservationFailure,
                "movie reservation does not match");
        }
        held = {};
        return MovieServiceResult::Success();
    }

    MovieReservationId held;
};

StopSubscriptionGroupDefinition WakeGroup(std::uint32_t pc = kWakePc)
{
    return {
        .id = StopSubscriptionGroupId(100),
        .source = {
            .id = StopSourceId(100),
            .stable_name = "test.execution-engine.wake",
            .diagnostic_label = "execution-engine test",
        },
        .subscriptions = {
            {
                .id = StopSubscriptionId(100),
                .point = PcStopPointSpec{pc},
                .route = ForegroundStopWait{
                    .suppress_immediate_reentry = true},
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

class HostActivityRecordingCpuObserver final
    : public IStopPointCpuObserver
{
public:
    explicit HostActivityRecordingCpuObserver(
        HostActivityTracker& host_activity) noexcept
        : host_activity_(host_activity)
    {
    }

    StopCpuObservationResult ObserveRoutedHit(
        std::uint32_t descriptor_id,
        const RoutedStopEvent&) noexcept override
    {
        descriptor_.store(descriptor_id, std::memory_order_relaxed);
        observed_in_flight_.store(
            host_activity_.snapshot().in_flight != 0,
            std::memory_order_relaxed);
        calls_.fetch_add(1, std::memory_order_relaxed);
        return StopCpuObservationResult::Observed;
    }

    [[nodiscard]] std::uint32_t calls() const noexcept
    {
        return calls_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint32_t descriptor() const noexcept
    {
        return descriptor_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool observed_in_flight() const noexcept
    {
        return observed_in_flight_.load(std::memory_order_relaxed);
    }

private:
    HostActivityTracker& host_activity_;
    std::atomic<std::uint32_t> calls_{0};
    std::atomic<std::uint32_t> descriptor_{0};
    std::atomic<bool> observed_in_flight_{false};
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

std::vector<ExecutionHealthWarning> TakeHealthWarnings(
    ExecutionEngine& engine)
{
    std::vector<ExecutionHealthWarning> warnings;
    for (ExecutionEvent& event : engine.DrainEvents())
    {
        if (event.kind == ExecutionEventKind::HealthWarning &&
            event.health_warning.has_value())
        {
            warnings.push_back(std::move(*event.health_warning));
        }
    }
    return warnings;
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

class ExecutionEngineFixture : public testing::Test
{
protected:
    static std::uint64_t HostActivityTicks(void* context) noexcept
    {
        const auto* current =
            static_cast<const std::chrono::steady_clock::time_point*>(
                context);
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                current->time_since_epoch())
                .count());
    }

    void SetUp() override
    {
        movie_service = std::make_unique<MovieService>(
            movie_backend,
            movie_reservations,
            [] { return kEpoch; },
            [] { return MovieServiceResult::Success(); },
            [] { return MovieServiceResult::Success(); },
            [] { return MovieServiceResult::Success(); });
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
        movie_service.reset();
    }

    void RestoreReadOnlyPlayback(
        std::uint64_t input_count = 0,
        std::uint64_t frame = 0)
    {
        MovieCheckpointMetadata movie;
        movie.mode = MovieCheckpointMode::ReadOnlyPlayback;
        movie.cursor_known = true;
        movie.current_frame = frame;
        movie.current_input_count = input_count;
        movie.dtm_bytes.resize(
            savor::tas::DtmFile::kMinHeader +
            static_cast<std::size_t>(input_count) * 8u);
        const SavestateMovieRestoreContext context{
            .workset_epoch = kEpoch,
            .movie = movie,
        };
        const MovieServiceResult prepared =
            movie_service->PrepareSavestateRestore(context);
        ASSERT_TRUE(prepared.ok) << prepared.message;
        const MovieServiceResult committed =
            movie_service->CommitSavestateRestore(context);
        ASSERT_TRUE(committed.ok) << committed.message;
        ASSERT_EQ(movie_service->state(), MovieState::ReadOnlyPlayback);
    }

    void BranchPlaybackToRecording()
    {
        const MovieOperationReceipt recording =
            movie_service->StartRecording();
        ASSERT_TRUE(recording.result.ok) << recording.result.message;
        ASSERT_EQ(recording.state, MovieState::Recording);
    }

    void CreateEngine(
        IInputExecutionBindingPort* input = nullptr,
        std::vector<InterruptionHandlerDescriptor> handlers = {},
        std::chrono::milliseconds maintenance_interval = 10ms,
        std::chrono::milliseconds pause_confirmation_timeout = 5s)
    {
        ExecutionEngineConfig config;
        config.maintenance_interval = maintenance_interval;
        config.pause_confirmation_timeout =
            pause_confirmation_timeout;
        config.now = [this] { return now; };
        config.input_relationships = input;
        config.host_activity = &host_activity;
        config.interruption_handlers = std::move(handlers);
        engine = std::make_unique<ExecutionEngine>(
            execution_backend,
            *movie_service,
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
            .subscriptions = {{
                .id = StopSubscriptionId(500),
                .point = PcStopPointSpec{kInterruptionPc},
                .route = TrustedStopInterruptionRequest{
                    .handler_key = std::move(key)},
                .priority = 100,
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
            physical_backend.InjectJitPcStop(kInterruptionPc);
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
                    StopRouteTerminal::InterruptionRequested;
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
    std::chrono::steady_clock::time_point now{};
    HostActivityTracker host_activity{&HostActivityTicks, &now};
    HostActivityRecordingCpuObserver cpu_observer{host_activity};
    StopPointRouter router{
        physical_manager,
        nullptr,
        &cpu_observer,
        &host_activity};

    std::shared_ptr<FakeExecutionBackendControl> execution_control =
        std::make_shared<FakeExecutionBackendControl>();
    FakeExecutionBackend execution_backend{execution_control};
    ExecutionMovieBackend movie_backend;
    ExecutionMovieReservations movie_reservations;
    std::unique_ptr<MovieService> movie_service;
    std::unique_ptr<ExecutionEngine> engine;
    RecordingInterruptionConsumer interruption_consumer;
    std::optional<StopSubscriptionGroupHandle> interruption_group;
};

TEST_F(ExecutionEngineFixture, InitializesAsIdlePausedAtTheSessionEpoch)
{
    CreateEngine();

    const ExecutionSnapshot snapshot = engine->snapshot();
    EXPECT_EQ(snapshot.activity, ExecutionActivity::IdlePaused);
    EXPECT_EQ(snapshot.workset_epoch, kEpoch);
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

TEST_F(
    ExecutionEngineFixture,
    ContinueRemainsCancellationDrivenWhileViProgressesAcrossArbitraryTime)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    for (std::uint64_t vi = 1; vi <= 24; ++vi)
    {
        now += 1h;
        execution_control->SetViCount(vi);
        engine->Pump();
        EXPECT_TRUE(engine->has_active_operation());
        EXPECT_FALSE(TakeTerminal(*engine).has_value());
    }

    ASSERT_TRUE(
        engine->Cancel(CancellationReason::ExternalRequest).accepted);
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::Cancelled);
    EXPECT_EQ(terminal->evidence.core_state, BackendCoreState::Paused);
    EXPECT_EQ(CountCall(execution_control->Calls(), "pause"), 1u);
}

TEST_F(
    ExecutionEngineFixture,
    CoreHealthWarnsAtTenSecondsAndConfirmsAtTwentySeconds)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    now += 9999ms;
    engine->Pump();
    EXPECT_TRUE(TakeHealthWarnings(*engine).empty());
    EXPECT_TRUE(engine->has_active_operation());

    now += 1ms;
    engine->Pump();
    const auto warnings = TakeHealthWarnings(*engine);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(
        warnings.front().kind,
        ExecutionHealthWarningKind::SuspectedCoreStall);
    EXPECT_EQ(warnings.front().elapsed.count(), 10000);
    EXPECT_EQ(warnings.front().code, "suspected_core_stall");
    EXPECT_TRUE(engine->has_active_operation());

    now += 9999ms;
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    now += 1ms;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::CoreStalled);
    EXPECT_EQ(terminal->error.message, "core_stalled");
    EXPECT_EQ(terminal->integrity, BackendIntegrity::Preserved);
    EXPECT_EQ(terminal->evidence.core_state, BackendCoreState::Paused);
    EXPECT_EQ(CountCall(execution_control->Calls(), "check_health"), 1u);
}

TEST_F(
    ExecutionEngineFixture,
    HostActivityAtTheConfirmationBoundaryPreventsAFalseCoreStall)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    execution_control->QueueQueryCallback([] {});
    execution_control->QueueQueryCallback([this] {
        auto activity = host_activity.Track();
    });
    now += 20s;
    engine->Pump();

    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());
    EXPECT_TRUE(TakeHealthWarnings(*engine).empty());

    ASSERT_TRUE(
        engine->Cancel(CancellationReason::ExternalRequest).accepted);
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::Cancelled);
}

TEST_F(
    ExecutionEngineFixture,
    ViProgressAtTheConfirmationBoundaryPreventsAFalseCoreStall)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    BackendExecutionSnapshot initial =
        execution_control->Snapshot();
    initial.core_state = BackendCoreState::Running;
    initial.pause_confirmed = false;
    initial.vi_count = 0;
    BackendExecutionSnapshot progressed = initial;
    progressed.vi_count = 1;
    execution_control->QueueQuerySnapshot(std::move(initial));
    execution_control->QueueQuerySnapshot(std::move(progressed));

    now += 20s;
    engine->Pump();
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());
    EXPECT_TRUE(TakeHealthWarnings(*engine).empty());

    ASSERT_TRUE(
        engine->Cancel(CancellationReason::ExternalRequest).accepted);
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::Cancelled);
}

TEST_F(
    ExecutionEngineFixture,
    SynchronousHostActivityWarnsAfterReturningAndCannotBecomeACoreStall)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    auto activity = host_activity.Track();
    engine->Pump();
    (void)engine->DrainEvents();

    now += 10s;
    engine->Pump();
    EXPECT_TRUE(TakeHealthWarnings(*engine).empty());
    EXPECT_TRUE(engine->has_active_operation());

    now += 29s;
    engine->Pump();
    EXPECT_TRUE(TakeHealthWarnings(*engine).empty());
    EXPECT_TRUE(engine->has_active_operation());

    now += 1s;
    engine->Pump();
    EXPECT_TRUE(TakeHealthWarnings(*engine).empty());
    EXPECT_TRUE(engine->has_active_operation());

    activity.Reset();
    engine->Pump();
    const auto warnings = TakeHealthWarnings(*engine);
    ASSERT_EQ(warnings.size(), 2u);
    EXPECT_EQ(
        warnings[0].kind,
        ExecutionHealthWarningKind::HostActivityLongRunning);
    EXPECT_EQ(warnings[0].elapsed.count(), 10000);
    EXPECT_EQ(warnings[0].code, "host_activity_long_running");
    EXPECT_EQ(warnings[1].elapsed.count(), 40000);
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    ASSERT_TRUE(
        engine->Cancel(CancellationReason::ExternalRequest).accepted);
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::Cancelled);
}

TEST_F(
    ExecutionEngineFixture,
    MultipleLongHostActivitiesRetainIndependentDiagnostics)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    {
        auto first = host_activity.Track();
        now += 11s;
    }
    {
        auto second = host_activity.Track();
        now += 12s;
    }

    engine->Pump();
    const auto warnings = TakeHealthWarnings(*engine);
    ASSERT_EQ(warnings.size(), 2u);
    EXPECT_EQ(warnings[0].elapsed.count(), 10000);
    EXPECT_EQ(warnings[1].elapsed.count(), 10000);
    EXPECT_NE(
        warnings[0].host_activity_generation,
        std::uint64_t{0});
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    ASSERT_TRUE(
        engine->Cancel(CancellationReason::ExternalRequest).accepted);
    ASSERT_TRUE(DrainTerminal(*engine).has_value());
}

TEST_F(
    ExecutionEngineFixture,
    HostActivityDiagnosticOverflowIsExplicit)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    for (std::size_t index = 0;
         index <
             HostActivityTracker::kCompletedActivityCapacity + 1;
         ++index)
    {
        auto activity = host_activity.Track();
    }

    engine->Pump();
    const auto warnings = TakeHealthWarnings(*engine);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(
        warnings.front().kind,
        ExecutionHealthWarningKind::
            HostActivityDiagnosticOverflow);
    EXPECT_EQ(
        warnings.front().code,
        "host_activity_diagnostic_overflow");
    EXPECT_NE(
        warnings.front().message.find("1 completed scope"),
        std::string::npos);
    EXPECT_TRUE(engine->has_active_operation());

    ASSERT_TRUE(
        engine->Cancel(CancellationReason::ExternalRequest).accepted);
    ASSERT_TRUE(DrainTerminal(*engine).has_value());
}

TEST_F(
    ExecutionEngineFixture,
    CompletedActorBlockingActivityPublishesDeferredWarnings)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    {
        auto activity = host_activity.Track();
        now += 70s;
        // An actor-owned synchronous activity prevents the actor from pumping.
        // The tracker must retain enough evidence to report every threshold
        // crossed once control returns.
    }

    engine->Pump();
    const auto warnings = TakeHealthWarnings(*engine);
    ASSERT_EQ(warnings.size(), 3u);
    EXPECT_EQ(
        warnings[0].kind,
        ExecutionHealthWarningKind::HostActivityLongRunning);
    EXPECT_EQ(warnings[0].elapsed.count(), 10000);
    EXPECT_EQ(warnings[1].elapsed.count(), 40000);
    EXPECT_EQ(warnings[2].elapsed.count(), 70000);
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    ASSERT_TRUE(
        engine->Cancel(CancellationReason::ExternalRequest).accepted);
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::Cancelled);
}

TEST_F(
    ExecutionEngineFixture,
    IntermittentHostActivityRebaselinesHealthWithThrottleDisabled)
{
    CreateEngine();
    ExecutionRequestPolicy policy = Policy();
    policy.throttle = ExecutionThrottlePolicy::RequireDisabled;
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();
    EXPECT_TRUE(execution_control->Snapshot().throttle_disabled);

    for (int cycle = 0; cycle < 4; ++cycle)
    {
        now += 9s;
        engine->Pump();
        EXPECT_TRUE(engine->has_active_operation());
        EXPECT_TRUE(TakeHealthWarnings(*engine).empty());

        {
            auto activity = host_activity.Track();
            engine->Pump();
            now += 2s;
            engine->Pump();
            EXPECT_TRUE(TakeHealthWarnings(*engine).empty());
        }
        engine->Pump();
        EXPECT_TRUE(engine->has_active_operation());
        EXPECT_FALSE(TakeTerminal(*engine).has_value());
    }

    ASSERT_TRUE(
        engine->Cancel(CancellationReason::ExternalRequest).accepted);
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::Cancelled);
    EXPECT_FALSE(execution_control->Snapshot().throttle_disabled);
}

TEST_F(
    ExecutionEngineFixture,
    BackgroundArtifactFinalizationNeitherMasksNorCreatesCoreStalls)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    const HostActivityTracker::Snapshot before =
        host_activity.snapshot();
    HealthTestTemporaryDirectory temporary;
    WorkerWorksetLimits limits;
    limits.finalizer_threads = 1;
    limits.maximum_pending_finalizers = 2;
    limits.maximum_pending_finalizer_bytes = 1024;
    SavestateArtifactFinalizer finalizer(limits);

    SavestateArtifactFinalizationRequest request;
    request.item = {
        WorkerWorksetId(1),
        WorkerWorksetItemId(1),
        0,
        InvocationId(1),
        AttemptId(1),
    };
    request.state_artifact_id = SavestateArtifactId(1);
    request.logical_artifact_id = "health-test-state";
    request.state = {
        temporary.path() / "health-test.sav",
        ImmutableArtifactBytes::Capture({1, 2, 3, 4}),
        {},
    };
    ASSERT_TRUE(finalizer.Submit(std::move(request)).result.ok);
    finalizer.Shutdown();
    const auto completions = finalizer.DrainResults();
    ASSERT_EQ(completions.size(), 1u);
    ASSERT_TRUE(completions.front().result.ok)
        << completions.front().result.message;

    const HostActivityTracker::Snapshot after =
        host_activity.snapshot();
    EXPECT_EQ(after.generation, before.generation);
    EXPECT_EQ(after.in_flight, before.in_flight);

    now += 20s;
    execution_control->SetViCount(1);
    engine->Pump();
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    now += 20s;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::CoreStalled);
}

TEST_F(
    ExecutionEngineFixture,
    NativeRouterObservationRebaselinesHealthWithoutFalseCoreStall)
{
    constexpr std::uint32_t kCapturePc = 0x801DC28Cu;
    constexpr std::uint32_t kCaptureDescriptor = 901u;
    RecordingInterruptionConsumer capture_consumer;
    StopGroupRegistrationResult capture_group =
        router.RegisterGroup({
            .id = StopSubscriptionGroupId(901),
            .source = {
                .id = StopSourceId(901),
                .stable_name =
                    "test.execution-engine.capture-observer",
                .diagnostic_label =
                    "execution-engine capture observer",
            },
            .subscriptions = {{
                .id = StopSubscriptionId(901),
                .point = PcStopPointSpec{kCapturePc},
                .route = PassiveStopObservation{
                    .cpu_observer_descriptor_id =
                        kCaptureDescriptor,
                    .lossless = true},
                .consumer = &capture_consumer,
            }},
        });
    ASSERT_TRUE(capture_group.receipt.ok)
        << capture_group.receipt.error.message;

    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    std::uint64_t previous_generation =
        host_activity.snapshot().generation;
    for (int observation = 0; observation < 3; ++observation)
    {
        // Without synchronous host progress this is one second short of
        // the confirmed-stall threshold.
        now += 19s;
        const auto decision =
            physical_backend.InjectJitPcStop(kCapturePc);
        EXPECT_FALSE(decision.request_break);
        EXPECT_FALSE(decision.authoritative_overflow);

        const HostActivityTracker::Snapshot tracked =
            host_activity.snapshot();
        EXPECT_GT(tracked.generation, previous_generation);
        EXPECT_EQ(tracked.in_flight, 0u);
        previous_generation = tracked.generation;

        auto receipts = router.DrainIngress();
        ASSERT_EQ(receipts.size(), 1u);
        EXPECT_EQ(receipts.front().terminal, StopRouteTerminal::None);
        engine->HandleStopPointReceipt(std::move(receipts.front()));
        engine->Pump();

        EXPECT_TRUE(engine->has_active_operation());
        EXPECT_FALSE(TakeTerminal(*engine).has_value());
    }

    EXPECT_EQ(cpu_observer.calls(), 3u);
    EXPECT_EQ(cpu_observer.descriptor(), kCaptureDescriptor);
    EXPECT_TRUE(cpu_observer.observed_in_flight());
    EXPECT_EQ(capture_consumer.deliveries.size(), 3u);

    // The last native dispatch is the new health baseline. It may produce a
    // suspicion warning later, but it must not be classified as a confirmed
    // core stall before another full confirmation interval elapses.
    now += 19999ms;
    engine->Pump();
    const auto warnings = TakeHealthWarnings(*engine);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(
        warnings.front().kind,
        ExecutionHealthWarningKind::SuspectedCoreStall);
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    ASSERT_TRUE(
        engine->Cancel(CancellationReason::ExternalRequest).accepted);
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::Cancelled);

    const StopReleaseReceipt released =
        capture_group.handle.Release();
    EXPECT_TRUE(released.ok) << released.error.message;
}

TEST_F(
    ExecutionEngineFixture,
    SafePauseConfirmationRetainsABoundedHostOperationTimeout)
{
    CreateEngine();
    execution_control->SetCoreState(BackendCoreState::Running);
    execution_control->SetPauseChangesState(false);

    const ExecutionSubmissionReceipt submission =
        engine->Submit(SafePauseRequest{
            .policy = Policy(),
            .confirmation_timeout = 25ms,
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    now += 24ms;
    engine->Pump();
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    now += 1ms;
    engine->Pump();
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::CleanupFailure);
    EXPECT_EQ(terminal->integrity, BackendIntegrity::Unknown);
    EXPECT_NE(
        terminal->error.message.find("host-operation bound"),
        std::string::npos);
    execution_control->SetPauseChangesState(true);
    execution_control->SetCoreState(BackendCoreState::Paused);
}

TEST_F(
    ExecutionEngineFixture,
    UnprovenBackendHealthMakesCoreStallIntegrityUnknown)
{
    CreateEngine();
    execution_control->SetHealth({
        false,
        BackendCoreState::Unknown,
        "injected unhealthy backend",
    });
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    now += 20s;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::CoreStalled);
    EXPECT_EQ(terminal->integrity, BackendIntegrity::Unknown);
    EXPECT_NE(
        terminal->error.message.find("injected unhealthy backend"),
        std::string::npos);
}

TEST_F(
    ExecutionEngineFixture,
    AlreadyPausedCoreStallStillRequiresBackendHealthProof)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    execution_control->SetHealth({
        false,
        BackendCoreState::Unknown,
        "injected immediate-pause health failure",
    });
    BackendExecutionSnapshot running =
        execution_control->Snapshot();
    running.core_state = BackendCoreState::Running;
    running.pause_confirmed = false;
    BackendExecutionSnapshot paused = running;
    paused.core_state = BackendCoreState::Paused;
    paused.pause_confirmed = true;
    execution_control->QueueQuerySnapshot(std::move(running));
    execution_control->QueueQuerySnapshot(
        execution_control->Snapshot());
    execution_control->QueueQuerySnapshot(std::move(paused));

    now += 20s;
    engine->Pump();
    const auto terminal = TakeTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::CoreStalled);
    EXPECT_EQ(terminal->integrity, BackendIntegrity::Unknown);
    EXPECT_NE(
        terminal->error.message.find(
            "injected immediate-pause health failure"),
        std::string::npos);
    EXPECT_EQ(CountCall(execution_control->Calls(), "check_health"), 1u);
}

TEST_F(
    ExecutionEngineFixture,
    StoppedCoreFailsInsteadOfWaitingForever)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    execution_control->SetCoreState(BackendCoreState::Stopped);
    execution_control->SetHealth({
        false,
        BackendCoreState::Stopped,
        "injected stopped core",
    });
    engine->Pump();

    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::BackendFailure);
    EXPECT_EQ(terminal->integrity, BackendIntegrity::Unknown);
    EXPECT_NE(
        terminal->error.message.find("injected stopped core"),
        std::string::npos);
    EXPECT_FALSE(engine->has_active_operation());
    EXPECT_EQ(TakeTerminals(*engine).size(), 0u);
}

TEST_F(
    ExecutionEngineFixture,
    UnconfirmedPauseFailsAfterTheHostConfirmationBound)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    BackendExecutionSnapshot unconfirmed =
        execution_control->Snapshot();
    unconfirmed.core_state = BackendCoreState::Paused;
    unconfirmed.pause_confirmed = false;
    execution_control->SetSnapshot(std::move(unconfirmed));
    execution_control->SetPauseChangesState(false);

    engine->Pump();
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    now += 5s;
    engine->Pump();
    now += 5s;
    engine->Pump();
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::CleanupFailure);
    EXPECT_EQ(terminal->integrity, BackendIntegrity::Unknown);
    EXPECT_FALSE(engine->has_active_operation());

    execution_control->SetPauseChangesState(true);
    execution_control->SetCoreState(BackendCoreState::Paused);
}

TEST_F(
    ExecutionEngineFixture,
    UnconfirmedPauseDeadlinePrecedesALongerMaintenanceCadence)
{
    CreateEngine(nullptr, {}, 60s, 5s);
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    BackendExecutionSnapshot unconfirmed =
        execution_control->Snapshot();
    unconfirmed.core_state = BackendCoreState::Paused;
    unconfirmed.pause_confirmed = false;
    execution_control->SetSnapshot(std::move(unconfirmed));
    engine->Pump();

    ASSERT_TRUE(engine->next_wake().has_value());
    EXPECT_EQ(*engine->next_wake(), now + 5s);

    execution_control->SetCoreState(BackendCoreState::Running);
    engine->Pump();
    ASSERT_TRUE(
        engine->Cancel(CancellationReason::ExternalRequest).accepted);
    ASSERT_TRUE(DrainTerminal(*engine).has_value());
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
    EXPECT_EQ(terminal->workset_epoch, kEpoch);
}

TEST_F(
    ExecutionEngineFixture,
    RoutedCompletionPublishesCompletedHostWarningBeforeTerminal)
{
    CreateEngine();

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    (void)engine->DrainEvents();

    {
        auto routing_activity = host_activity.Track();
        now += 11s;
    }
    const auto decision = physical_backend.InjectJitPcStop(kWakePc);
    ASSERT_TRUE(decision.request_break);
    auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(std::move(receipts.front()));

    const std::vector<ExecutionEvent> events =
        engine->DrainEvents();
    const auto warning = std::find_if(
        events.begin(),
        events.end(),
        [](const ExecutionEvent& event) {
            return event.health_warning &&
                event.health_warning->kind ==
                    ExecutionHealthWarningKind::
                        HostActivityLongRunning;
        });
    const auto terminal = std::find_if(
        events.begin(),
        events.end(),
        [](const ExecutionEvent& event) {
            return event.terminal.has_value();
        });
    ASSERT_NE(warning, events.end());
    ASSERT_NE(terminal, events.end());
    EXPECT_LT(
        std::distance(events.begin(), warning),
        std::distance(events.begin(), terminal));
    EXPECT_EQ(warning->health_warning->elapsed.count(), 10000);
    EXPECT_EQ(
        terminal->terminal->status,
        ExecutionTerminalStatus::RequestedCompletion);
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

TEST_F(
    ExecutionEngineFixture,
    TransientUnconfirmedPauseTimeDoesNotCountTowardCoreStall)
{
    CreateEngine();

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    now += 9s;
    engine->Pump();
    EXPECT_TRUE(TakeHealthWarnings(*engine).empty());

    BackendExecutionSnapshot transient =
        execution_control->Snapshot();
    transient.core_state = BackendCoreState::Paused;
    transient.pause_confirmed = false;
    execution_control->SetSnapshot(std::move(transient));
    now += 4s;
    engine->Pump();
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    execution_control->SetCoreState(BackendCoreState::Running);
    engine->Pump();
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_TRUE(TakeHealthWarnings(*engine).empty());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    now += 9s;
    engine->Pump();
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_TRUE(TakeHealthWarnings(*engine).empty());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    ASSERT_TRUE(
        engine->Cancel(CancellationReason::ExternalRequest).accepted);
    ASSERT_TRUE(DrainTerminal(*engine).has_value());
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
    RestoreReadOnlyPlayback();
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

    movie_backend.EndPlayback();
    BackendExecutionSnapshot externally_paused =
        execution_control->Snapshot();
    externally_paused.core_state = BackendCoreState::Paused;
    externally_paused.pause_confirmed = false;
    execution_control->SetSnapshot(std::move(externally_paused));
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::MovieEnded);
    EXPECT_EQ(
        terminal->evidence.movie_state,
        MovieState::PlaybackEnded);
    EXPECT_EQ(terminal->evidence.core_state, BackendCoreState::Paused);
    EXPECT_EQ(CountCall(execution_control->Calls(), "pause"), 1u);
}

TEST_F(
    ExecutionEngineFixture,
    RunningInactiveMaintenanceUsesOnlyTheMovieServiceSnapshot)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission = engine->Submit(
        ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    const std::uint64_t observations_before =
        movie_backend.observation_count;

    for (int iteration = 0; iteration < 5; ++iteration)
    {
        now += 11ms;
        engine->Pump();
    }

    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_EQ(movie_backend.observation_count, observations_before);
}

TEST_F(
    ExecutionEngineFixture,
    RunningReadOnlyPlaybackMaintenanceUsesOnlyTheMovieServiceSnapshot)
{
    RestoreReadOnlyPlayback();
    CreateEngine();
    const ExecutionSubmissionReceipt submission = engine->Submit(
        ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    const std::uint64_t observations_before =
        movie_backend.observation_count;

    for (int iteration = 0; iteration < 5; ++iteration)
    {
        now += 11ms;
        engine->Pump();
    }

    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_EQ(movie_backend.observation_count, observations_before);
}

TEST_F(
    ExecutionEngineFixture,
    RunningRecordingMaintenanceUsesOnlyTheMovieServiceSnapshot)
{
    RestoreReadOnlyPlayback();
    CreateEngine();
    BranchPlaybackToRecording();
    const ExecutionSubmissionReceipt submission = engine->Submit(
        ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    const std::uint64_t observations_before =
        movie_backend.observation_count;

    for (int iteration = 0; iteration < 5; ++iteration)
    {
        now += 11ms;
        engine->Pump();
    }

    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_EQ(movie_backend.observation_count, observations_before);
}

TEST_F(
    ExecutionEngineFixture,
    MovieEndFailureBeforeInitialResumePreservesItsError)
{
    RestoreReadOnlyPlayback();
    movie_backend.EndPlayback();
    CreateEngine();
    ExecutionRequestPolicy policy = Policy();
    policy.movie_ended = MovieEndedPolicy::Fail;

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::MovieEnded);
    EXPECT_EQ(terminal->error.code, ExecutionErrorCode::InvalidState);
    EXPECT_EQ(
        terminal->error.message,
        "movie ended before the requested completion");
    EXPECT_EQ(CountCall(execution_control->Calls(), "resume"), 0u);
}

TEST_F(
    ExecutionEngineFixture,
    SameEngineObservesPlaybackBranchToRecordingWithoutMovieEndFailure)
{
    RestoreReadOnlyPlayback(10);
    CreateEngine();
    BranchPlaybackToRecording();
    ExecutionRequestPolicy policy = Policy();
    policy.movie_ended = MovieEndedPolicy::Fail;

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    EXPECT_EQ(CountCall(execution_control->Calls(), "resume"), 1u);
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    (void)physical_backend.InjectJitPcStop(kWakePc);
    auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(std::move(receipts.front()));

    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::RequestedCompletion);
    EXPECT_EQ(terminal->evidence.movie_state, MovieState::Recording);
}

TEST_F(
    ExecutionEngineFixture,
    CursorOverrunBeforeInitialResumeCompletesWithoutResuming)
{
    RestoreReadOnlyPlayback(11);
    CreateEngine();
    ExecutionRequestPolicy policy = Policy();
    policy.movie_ended = MovieEndedPolicy::Complete;

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
            .expected_movie_input_count = 10,
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::CursorOverrun);
    EXPECT_EQ(terminal->evidence.movie_input_count, 11u);
    EXPECT_EQ(CountCall(execution_control->Calls(), "resume"), 0u);
}

TEST_F(
    ExecutionEngineFixture,
    CursorOverrunIsDetectedAtTheNextAuthoritativePause)
{
    RestoreReadOnlyPlayback(10);
    CreateEngine();
    ExecutionRequestPolicy policy = Policy();
    policy.movie_ended = MovieEndedPolicy::Complete;

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
            .expected_movie_input_count = 10,
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    EXPECT_EQ(CountCall(execution_control->Calls(), "resume"), 1u);
    const std::uint64_t observations_before =
        movie_backend.observation_count;
    movie_backend.SetInputCount(12);
    now += 11ms;
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());
    EXPECT_EQ(movie_backend.observation_count, observations_before);

    execution_control->SetCoreState(BackendCoreState::Paused);
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::CursorOverrun);
    EXPECT_EQ(terminal->evidence.movie_input_count, 12u);
    EXPECT_EQ(terminal->evidence.core_state, BackendCoreState::Paused);
}

TEST_F(
    ExecutionEngineFixture,
    CursorOverrunTakesPriorityOverOwnedMovieEnd)
{
    RestoreReadOnlyPlayback(21);
    movie_backend.EndPlayback();
    CreateEngine();
    ExecutionRequestPolicy policy = Policy();
    policy.movie_ended = MovieEndedPolicy::Complete;

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
            .expected_movie_input_count = 20,
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::CursorOverrun);
}

TEST_F(
    ExecutionEngineFixture,
    OwnedMovieEndAtOrBelowExpectedCountCompletesAsMovieEnded)
{
    RestoreReadOnlyPlayback(19);
    movie_backend.EndPlayback();
    CreateEngine();
    ExecutionRequestPolicy policy = Policy();
    policy.movie_ended = MovieEndedPolicy::Complete;

    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
            .expected_movie_input_count = 20,
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::MovieEnded);
    EXPECT_EQ(terminal->evidence.movie_input_count, 19u);
}

TEST_F(
    ExecutionEngineFixture,
    BreakpointDuringPauseConfirmationReplacesPendingCursorOverrun)
{
    RestoreReadOnlyPlayback(10);
    execution_control->SetPauseChangesState(false);
    CreateEngine();
    ExecutionRequestPolicy policy = Policy();
    policy.movie_ended = MovieEndedPolicy::Complete;
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
            .expected_movie_input_count = 10,
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    movie_backend.SetInputCount(11);
    now += 11ms;
    engine->Pump();
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());
    (void)physical_backend.InjectJitPcStop(kWakePc);
    auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    execution_control->SetCoreState(BackendCoreState::Paused);
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

TEST_F(
    ExecutionEngineFixture,
    AcceptedWakePrecedesCancellationMovieEndAndHealthMaintenance)
{
    RestoreReadOnlyPlayback();
    CreateEngine();
    CancellationSource cancellation(InvocationId(77));
    ExecutionRequestPolicy policy = Policy();
    policy.cancellation = cancellation.token();
    policy.movie_ended = MovieEndedPolicy::Complete;
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;
    engine->Pump();
    (void)engine->DrainEvents();

    now += 20s;
    movie_backend.EndPlayback();
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
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());
}

TEST_F(
    ExecutionEngineFixture,
    CancellationAcceptedBeforeAStopRemainsTheOnlyTerminal)
{
    CreateEngine();
    const ExecutionSubmissionReceipt submission =
        engine->Submit(ContinueUntilRequest{
            .policy = Policy(),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(submission.accepted) << submission.error.message;

    const ExecutionControlReceipt cancellation =
        engine->Cancel(CancellationReason::ExternalRequest);
    ASSERT_TRUE(cancellation.accepted) << cancellation.error.message;
    (void)physical_backend.InjectJitPcStop(kWakePc);
    auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    engine->HandleStopPointReceipt(std::move(receipts.front()));

    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, ExecutionTerminalStatus::Cancelled);
    EXPECT_FALSE(TakeTerminal(*engine).has_value());
}

TEST_F(ExecutionEngineFixture, RejectsWorksetEpochMismatchWithoutAdvancing)
{
    CreateEngine();

    const ExecutionSubmissionReceipt submission =
        engine->Submit(StepFramesRequest{
            .policy = Policy(WorksetEpoch(8)),
            .count = 1,
        });

    EXPECT_FALSE(submission.accepted);
    EXPECT_EQ(
        submission.error.code,
        ExecutionErrorCode::WorksetEpochMismatch);
    EXPECT_EQ(CountCall(execution_control->Calls(), "frame_step"), 0u);
}

TEST_F(
    ExecutionEngineFixture,
    InterruptionChildAndCompletionWaitForAuthoritativePauseConfirmation)
{
    CreateEngine(nullptr, {Handler()}, 60s, 5s);
    ExecutionRequestPolicy policy = Policy();
    policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt parent =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(parent.accepted) << parent.error.message;

    execution_control->SetPauseChangesState(false);
    engine->HandleStopPointReceipt(RouteInterruption());
    const auto frame = engine->snapshot().active_interruption_frame;
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(engine->next_wake().has_value());
    EXPECT_EQ(*engine->next_wake(), now + 5s);

    const ExecutionSubmissionReceipt premature_child =
        engine->SubmitInterruptionChild(
            *frame,
            ContinueUntilRequest{
                .policy = Policy(),
                .wake_group = WakeGroup(kWakePc + 4),
            });
    EXPECT_FALSE(premature_child.accepted);
    EXPECT_EQ(
        premature_child.error.code,
        ExecutionErrorCode::InvalidState);
    const ExecutionControlReceipt premature_completion =
        engine->CompleteInterruptionHandler(
            *frame,
            InterruptionHandlerOutcome::AbortParent);
    EXPECT_FALSE(premature_completion.accepted);
    EXPECT_EQ(
        premature_completion.error.code,
        ExecutionErrorCode::InvalidState);

    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->Pump();
    const ExecutionControlReceipt completion =
        engine->CompleteInterruptionHandler(
            *frame,
            InterruptionHandlerOutcome::AbortParent);
    ASSERT_TRUE(completion.accepted) << completion.error.message;
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->operation_id, parent.operation_id);
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::InterruptionAborted);
}

TEST_F(
    ExecutionEngineFixture,
    InterruptionPauseConfirmationTimeoutFailsClosedExactlyOnce)
{
    CreateEngine(nullptr, {Handler()}, 60s, 5s);
    ExecutionRequestPolicy policy = Policy();
    policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt parent =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(parent.accepted) << parent.error.message;

    execution_control->SetPauseChangesState(false);
    engine->HandleStopPointReceipt(RouteInterruption());
    ASSERT_TRUE(
        engine->snapshot().active_interruption_frame.has_value());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    now += 5s;
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    now += 5s;
    engine->Pump();
    const auto terminal = TakeTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->operation_id, parent.operation_id);
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::CleanupFailure);
    EXPECT_EQ(terminal->integrity, BackendIntegrity::Unknown);
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    execution_control->SetCoreState(BackendCoreState::Paused);
}

TEST_F(
    ExecutionEngineFixture,
    CancellationDuringPendingHandlerPauseUsesTheSafePauseBound)
{
    CreateEngine(nullptr, {Handler()}, 60s, 5s);
    ExecutionRequestPolicy policy = Policy();
    policy.interruptions = ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt parent =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(parent.accepted) << parent.error.message;

    execution_control->SetPauseChangesState(false);
    engine->HandleStopPointReceipt(RouteInterruption());
    ASSERT_TRUE(
        engine->snapshot().active_interruption_frame.has_value());

    const ExecutionControlReceipt cancelled =
        engine->Cancel(CancellationReason::ExternalRequest);
    ASSERT_TRUE(cancelled.accepted) << cancelled.error.message;
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    now += 5s;
    engine->Pump();
    const auto terminal = TakeTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->operation_id, parent.operation_id);
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::CleanupFailure);
    EXPECT_NE(
        terminal->status,
        ExecutionTerminalStatus::Cancelled);
    EXPECT_EQ(terminal->integrity, BackendIntegrity::Unknown);
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    execution_control->SetCoreState(BackendCoreState::Paused);
}

TEST_F(
    ExecutionEngineFixture,
    CancellationFromANestedWaitingHandlerCleansEachParentExactlyOnce)
{
    InterruptionHandlerDescriptor outer = Handler("handler.outer");
    outer.permitted_nested_keys = {"handler.inner"};
    CreateEngine(
        nullptr,
        {std::move(outer), Handler("handler.inner")});

    ExecutionRequestPolicy root_policy = Policy();
    root_policy.interruptions =
        ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt root =
        engine->Submit(ContinueUntilRequest{
            .policy = std::move(root_policy),
            .wake_group = WakeGroup(),
        });
    ASSERT_TRUE(root.accepted) << root.error.message;

    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(
        RouteInterruption("handler.outer"));
    const auto outer_frame =
        engine->snapshot().active_interruption_frame;
    ASSERT_TRUE(outer_frame.has_value());

    ExecutionRequestPolicy child_policy = Policy();
    child_policy.interruptions =
        ExecutionInterruptionPolicy::AllowKnown;
    const ExecutionSubmissionReceipt child =
        engine->SubmitInterruptionChild(
            *outer_frame,
            ContinueUntilRequest{
                .policy = std::move(child_policy),
                .wake_group = WakeGroup(),
            });
    ASSERT_TRUE(child.accepted) << child.error.message;

    execution_control->SetCoreState(BackendCoreState::Paused);
    engine->HandleStopPointReceipt(
        RouteInterruption("handler.inner"));
    ASSERT_TRUE(
        engine->snapshot().active_interruption_frame.has_value());

    const ExecutionControlReceipt cancelled =
        engine->Cancel(CancellationReason::ExternalRequest);
    ASSERT_TRUE(cancelled.accepted) << cancelled.error.message;
    const auto terminals = TakeTerminals(*engine);
    ASSERT_EQ(terminals.size(), 2u);
    EXPECT_EQ(
        std::count_if(
            terminals.begin(),
            terminals.end(),
            [&](const ExecutionTerminalResult& terminal) {
                return terminal.operation_id == child.operation_id;
            }),
        1);
    EXPECT_EQ(
        std::count_if(
            terminals.begin(),
            terminals.end(),
            [&](const ExecutionTerminalResult& terminal) {
                return terminal.operation_id == root.operation_id;
            }),
        1);
    EXPECT_TRUE(std::all_of(
        terminals.begin(),
        terminals.end(),
        [](const ExecutionTerminalResult& terminal) {
            return terminal.status ==
                ExecutionTerminalStatus::Cancelled;
        }));
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());
}

TEST_F(
    ExecutionEngineFixture,
    HandlerSuspensionAndChildExecutionDoNotCreateElapsedDeadlines)
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
                // Interruption points are reserved and cannot also be owned
                // by a foreground wait. Use a distinct child endpoint.
                .wake_group = WakeGroup(kWakePc + 8),
            });
    ASSERT_TRUE(child.accepted) << child.error.message;

    now += 24h;
    execution_control->SetViCount(1);
    engine->Pump();
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    const auto child_hit =
        physical_backend.InjectJitPcStop(kWakePc + 8);
    ASSERT_TRUE(child_hit.request_break);
    execution_control->SetCoreState(BackendCoreState::Paused);
    auto child_receipts = router.DrainIngress();
    ASSERT_EQ(child_receipts.size(), 1u);
    engine->HandleStopPointReceipt(std::move(child_receipts.front()));
    const auto child_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(child_terminal.has_value());
    EXPECT_EQ(child_terminal->operation_id, child.operation_id);
    EXPECT_EQ(
        child_terminal->status,
        ExecutionTerminalStatus::RequestedCompletion);
    EXPECT_EQ(engine->snapshot().activity, ExecutionActivity::HandlingInterruption);

    now += 24h;
    const ExecutionControlReceipt resumed =
        engine->CompleteInterruptionHandler(
            *suspended.active_interruption_frame,
            InterruptionHandlerOutcome::ResumeParent);
    ASSERT_TRUE(resumed.accepted) << resumed.error.message;
    EXPECT_EQ(engine->snapshot().activity, ExecutionActivity::Continuing);

    execution_control->SetViCount(2);
    now += 24h;
    engine->Pump();
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    ASSERT_TRUE(interruption_group.has_value());
    ASSERT_TRUE(interruption_group->Release().ok);
    interruption_group.reset();
    // The child completed at a different semantic point. The next visit to
    // the parent's Wake PC is therefore a legitimate future hit, not an
    // immediate source-instruction re-entry to suppress.
    const auto parent_hit =
        physical_backend.InjectJitPcStop(kWakePc);
    ASSERT_TRUE(parent_hit.request_break);
    execution_control->SetCoreState(BackendCoreState::Paused);
    auto parent_receipts = router.DrainIngress();
    ASSERT_EQ(parent_receipts.size(), 1u);
    engine->HandleStopPointReceipt(std::move(parent_receipts.front()));
    const auto terminal = DrainTerminal(*engine);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->operation_id, parent.operation_id);
    EXPECT_EQ(
        terminal->status,
        ExecutionTerminalStatus::RequestedCompletion);
}

TEST_F(
    ExecutionEngineFixture,
    ParkedParentForegroundWaitIsUnregisteredDuringFrameChildAndRestoredOnResume)
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
    EXPECT_TRUE(parked_receipts.empty());

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
        StopRouteTerminal::ForegroundMatched);
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
    NestedHandlersAndChildrenRemainCancellationDriven)
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

    // Holding a guest-dependent child operation across arbitrary injected
    // time cannot manufacture an elapsed-time terminal.
    execution_control->SetCoreState(BackendCoreState::Running);
    execution_control->SetViCount(1);
    now += 24h;
    engine->Pump();
    EXPECT_TRUE(engine->has_active_operation());
    EXPECT_FALSE(TakeTerminal(*engine).has_value());

    execution_control->SetCoreState(BackendCoreState::Paused);
    execution_control->SetViCount(2);
    const auto inner_child_terminal = DrainTerminal(*engine);
    ASSERT_TRUE(inner_child_terminal.has_value());
    EXPECT_EQ(inner_child_terminal->operation_id, inner_child.operation_id);
    EXPECT_EQ(
        inner_child_terminal->status,
        ExecutionTerminalStatus::StepsCompleted);

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

} // namespace
