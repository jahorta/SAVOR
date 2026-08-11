#include <gtest/gtest.h>

#include "Runner/Runtime/DolphinWrapperBackend.h"
#include "Runner/Runtime/EmulationSession.h"
#include "../SavorProbe/NativeStopHooks.h"
#include "../SavorProbe/ProbeProfile.h"
#include "serial_guard.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
    StopSubscriptionRoute route,
    bool lossless,
    IStopPointConsumer& consumer)
{
    const bool foreground =
        std::holds_alternative<ForegroundStopWait>(route);
    if (auto* passive =
            std::get_if<PassiveStopObservation>(&route))
    {
        passive->lossless = lossless;
    }
    return {
        .id = group_id,
        .source = {
            .id = source_id,
            .stable_name = foreground
                ? "integration.game_mode_controller.wake"
                : "integration.game_mode_controller.observe",
            .diagnostic_label = foreground
                ? "live JIT router wake guard"
                : "live JIT router passive guard",
        },
        .subscriptions = {{
            .id = subscription_id,
            .point = PcStopPointSpec{kGameModeControllerPc},
            .route = std::move(route),
            .consumer = &consumer,
        }},
    };
}

[[nodiscard]] ExecutionRequestPolicy MakeExecutionPolicy(
    WorksetEpoch epoch)
{
    ExecutionRequestPolicy policy;
    policy.expected_epoch = epoch;
    return policy;
}

[[nodiscard]] StopSubscriptionGroupDefinition MakeEngineWakeGroup()
{
    return {
        .id = kWakeGroup,
        .source = {
            .id = kWakeSource,
            .stable_name = "integration.game_mode_controller.engine_wake",
            .diagnostic_label = "headless JIT ExecutionEngine wake guard",
        },
        .subscriptions = {{
            .id = kWakeSubscription,
            .point = PcStopPointSpec{kGameModeControllerPc},
            .route = ForegroundStopWait{
                .suppress_immediate_reentry = true},
        }},
    };
}

[[nodiscard]] std::string MakeLiveCaptureProfile()
{
    savor::probe::Profile profile;
    profile.name = "slice4-live-recurring-pc";
    profile.revision = 1;
    profile.expected_module_sha256 =
        savor::probe::current_module_sha256();
    profile.probes = {
        savor::probe::ProbeDefinition{
            .id = "game_mode_controller",
            .kind = savor::probe::ProbeKind::Pc,
            .subscriptions = savor::probe::Subscription::Capture,
            .address = kGameModeControllerPc,
        },
    };
    return savor::probe::serialize_profile_json(profile);
}

[[nodiscard]] std::optional<ExecutionTerminalResult> DriveUntilTerminal(
    EmulationSession& session,
    std::atomic<std::uint64_t>& notification_counter,
    IngressSignal& signal,
    std::chrono::steady_clock::duration timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        auto drained = session.DrainStopPointEvents();
        for (StopRouteReceipt& receipt : drained)
            session.HandleStopPointReceipt(std::move(receipt));
        session.PumpExecution();
        for (ExecutionEvent& event : session.DrainExecutionEvents())
        {
            if (event.kind == ExecutionEventKind::Terminal &&
                event.terminal)
            {
                return std::move(event.terminal);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return std::nullopt;

        const std::uint64_t observed =
            notification_counter.load(std::memory_order_acquire);
        const auto maintenance =
            session.next_execution_wake().value_or(deadline);
        const auto wake = std::min(deadline, maintenance);
        (void)signal.WaitForChange(
            notification_counter,
            observed,
            wake);
    }
}

void ExpectSameIdentity(
    const RoutedStopIdentity& lhs,
    const RoutedStopIdentity& rhs)
{
    EXPECT_EQ(lhs.sequence, rhs.sequence);
    EXPECT_EQ(lhs.sample_snapshot, rhs.sample_snapshot);
    EXPECT_EQ(lhs.workset_epoch, rhs.workset_epoch);
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
    const SessionOperationReceipt opened = session.Open(open_options);
    ASSERT_TRUE(opened.ok) << opened.backend.message;
    ASSERT_EQ(opened.workset_epoch, WorksetEpoch{});
    ASSERT_FALSE(session.execution_snapshot());
    const SessionOperationReceipt initialization =
        session.OpenWorksetInitialization(WorkerWorksetId(1));
    ASSERT_TRUE(initialization.ok) << initialization.backend.message;
    ASSERT_EQ(initialization.workset_epoch, WorksetEpoch(1));
    ASSERT_FALSE(session.execution_snapshot());
    const SessionOperationReceipt committed =
        session.CommitWorksetInitialization(WorkerWorksetId(1));
    ASSERT_TRUE(committed.ok) << committed.backend.message;
    ASSERT_TRUE(session.execution_snapshot());
    ASSERT_TRUE(savor::probe::NativeStopHooksInstalled());

    StopPointRouter* const router = session.stop_points();
    ASSERT_NE(router, nullptr);
    ASSERT_EQ(
        savor::probe::BoundNativeStopSink(),
        static_cast<savor::probe::INativeStopSink*>(router));
    ASSERT_TRUE(router->DesiredPhysicalPlan().pcs.empty());
    ASSERT_TRUE(router->DesiredPhysicalPlan().memory.empty());

    ASSERT_EQ(session.snapshot().core_state, BackendCoreState::Paused);

    // Exercise the production mutation seam while the guest is paused. The
    // executable patch must be checked, invalidated, read back, restored, and
    // invalidated again without advancing Dolphin while patched.
    GuestMemory* const memory = session.guest_memory();
    GuestMutationService* const mutations = session.guest_mutations();
    ASSERT_NE(memory, nullptr);
    ASSERT_NE(mutations, nullptr);
    const GuestReadReceipt original_instruction = memory->ReadScalar(
        kGameModeControllerPc,
        GuestScalarWidth::U32,
        WorksetEpoch(1));
    ASSERT_TRUE(original_instruction.ok)
        << original_instruction.message;
    ASSERT_EQ(original_instruction.value, 0x9421fff0u);
    const ExecutionSnapshot before_patch =
        *session.execution_snapshot();
    const GuestMutationReceipt patch = mutations->Apply({
        .owner = MutationOwnerId(0x7101u),
        .scope = MutationScopeId(0x7101u),
        .epoch = WorksetEpoch(1),
        .address = kGameModeControllerPc,
        .expected = original_instruction.value,
        .replacement = 0x60000000u,
        .mask = 0xffffffffu,
        .kind = GuestMutationKind::ExecutablePatch,
    });
    ASSERT_TRUE(patch.ok) << patch.message;
    const GuestReadReceipt patched_instruction = memory->ReadScalar(
        kGameModeControllerPc,
        GuestScalarWidth::U32,
        WorksetEpoch(1));
    ASSERT_TRUE(patched_instruction.ok);
    EXPECT_EQ(patched_instruction.value, 0x60000000u);
    EXPECT_EQ(
        session.execution_snapshot()->evidence.vi_count,
        before_patch.evidence.vi_count);
    const GuestMutationReceipt restored_patch =
        mutations->Restore(patch.mutation, WorksetEpoch(1));
    ASSERT_TRUE(restored_patch.ok) << restored_patch.message;
    const GuestReadReceipt restored_instruction = memory->ReadScalar(
        kGameModeControllerPc,
        GuestScalarWidth::U32,
        WorksetEpoch(1));
    ASSERT_TRUE(restored_instruction.ok);
    EXPECT_EQ(
        restored_instruction.value,
        original_instruction.value);
    EXPECT_EQ(
        session.execution_snapshot()->evidence.vi_count,
        before_patch.evidence.vi_count);

    // Compile and execute the recurring game loop before installing the
    // physical PC. A later hit therefore exercises Dolphin's ordinary
    // address-specific JIT invalidation path.
    const ExecutionEnvironmentEvidence initial_evidence =
        session.execution_snapshot()->evidence;
    const ExecutionSubmissionReceipt initial_step =
        session.SubmitExecution(StepFramesRequest{
            .policy = MakeExecutionPolicy(WorksetEpoch(1)),
            .count = 1,
        });
    ASSERT_TRUE(initial_step.accepted) << initial_step.error.message;
    const auto initial_step_terminal = DriveUntilTerminal(
        session,
        notification_counter,
        ingress_signal,
        10s);
    ASSERT_TRUE(initial_step_terminal.has_value());
    ASSERT_EQ(
        initial_step_terminal->status,
        ExecutionTerminalStatus::StepsCompleted);
    ASSERT_EQ(
        initial_step_terminal->evidence.core_state,
        BackendCoreState::Paused);
    ASSERT_GT(
        initial_step_terminal->evidence.vi_count,
        initial_evidence.vi_count);

    CaptureService* const capture = session.capture_service();
    ASSERT_NE(capture, nullptr);
    savor::probe::SessionOptions capture_options;
    capture_options.capture_path =
        temporary.path() / "recurring-pc.scap";
    const CaptureServiceReceipt capture_attached =
        capture->Attach({
            .profile_json = MakeLiveCaptureProfile(),
            .options = std::move(capture_options),
            .expected_epoch = WorksetEpoch(1),
        });
    ASSERT_TRUE(capture_attached.ok)
        << capture_attached.error.message;

    RecordingStopConsumer observe_consumer;
    auto observe_registration = router->RegisterGroup(MakePcGroup(
        kObserveGroup,
        kObserveSource,
        kObserveSubscription,
        PassiveStopObservation{},
        false,
        observe_consumer));
    ASSERT_TRUE(observe_registration.receipt.ok)
        << observe_registration.receipt.error.message;

    const ExecutionSubmissionReceipt first_wait =
        session.SubmitExecution(ContinueUntilRequest{
            .policy = MakeExecutionPolicy(WorksetEpoch(1)),
            .wake_group = MakeEngineWakeGroup(),
        });
    ASSERT_TRUE(first_wait.accepted) << first_wait.error.message;

    const PhysicalStopPointPlan armed_plan =
        router->DesiredPhysicalPlan();
    ASSERT_EQ(armed_plan.pcs.size(), 1u);
    EXPECT_EQ(armed_plan.pcs.front().pc, kGameModeControllerPc);
    EXPECT_TRUE(armed_plan.memory.empty());

    const auto first_terminal = DriveUntilTerminal(
        session,
        notification_counter,
        ingress_signal,
        kLiveStopDeadline);
    ASSERT_TRUE(first_terminal.has_value())
        << "Timed out after 30 seconds waiting for the recurring JIT/router "
           "guard at 0x801DC288";
    ASSERT_EQ(
        first_terminal->status,
        ExecutionTerminalStatus::RequestedCompletion)
        << first_terminal->error.message;
    ASSERT_TRUE(first_terminal->stop.has_value());
    const StopRouteReceipt& first_wake = *first_terminal->stop;
    ASSERT_EQ(first_wake.terminal, StopRouteTerminal::ForegroundMatched);
    ASSERT_TRUE(first_wake.event.has_value());
    const RoutedStopEvent& event = *first_wake.event;
    const auto* const routed_pc =
        std::get_if<PcStopPointSpec>(&event.evidence.point);
    ASSERT_NE(routed_pc, nullptr);
    EXPECT_EQ(routed_pc->pc, kGameModeControllerPc);
    EXPECT_EQ(event.evidence.hit_pc, kGameModeControllerPc);
    EXPECT_EQ(event.evidence.path, NativeStopPath::Jit);
    EXPECT_EQ(event.identity.workset_epoch, session.snapshot().workset_epoch);
    EXPECT_TRUE(event.active_foreground_wait);
    EXPECT_TRUE(event.authoritative);
    EXPECT_FALSE(router->authoritative_overflowed());
    EXPECT_EQ(router->passive_drop_count(), 0u);

    ASSERT_GE(first_wake.deliveries.size(), 3u);
    const auto wake_delivery = std::find_if(
        first_wake.deliveries.begin(),
        first_wake.deliveries.end(),
        [](const StopDelivery& delivery) {
            return delivery.subscription_id == kWakeSubscription;
        });
    ASSERT_NE(wake_delivery, first_wake.deliveries.end());
    for (const StopDelivery& delivery : first_wake.deliveries)
        ExpectSameIdentity(event.identity, delivery.event.identity);

    ASSERT_EQ(observe_consumer.deliveries.size(), 1u);
    EXPECT_EQ(session.snapshot().core_state, BackendCoreState::Paused);

    // Requesting the same future-only wait while paused at the retained
    // receipt must arm exact source suppression. The shared physical site
    // remains installed because the passive Observe subscription still owns
    // it, and the next completion must carry a fresh routed sequence.
    const ExecutionSubmissionReceipt second_wait =
        session.SubmitExecution(ContinueUntilRequest{
            .policy = MakeExecutionPolicy(WorksetEpoch(1)),
            .wake_group = MakeEngineWakeGroup(),
        });
    ASSERT_TRUE(second_wait.accepted) << second_wait.error.message;
    const PhysicalStopPointPlan second_armed_plan =
        router->DesiredPhysicalPlan();
    EXPECT_EQ(second_armed_plan, armed_plan);

    const auto second_terminal = DriveUntilTerminal(
        session,
        notification_counter,
        ingress_signal,
        kLiveStopDeadline);
    ASSERT_TRUE(second_terminal.has_value())
        << "Timed out waiting for the future-only second JIT/router hit";
    ASSERT_EQ(
        second_terminal->status,
        ExecutionTerminalStatus::RequestedCompletion)
        << second_terminal->error.message;
    ASSERT_TRUE(second_terminal->stop.has_value());
    ASSERT_TRUE(second_terminal->stop->event.has_value());
    EXPECT_NE(
        second_terminal->stop->identity.sequence,
        first_wake.identity.sequence);
    EXPECT_GT(
        second_terminal->stop->identity.dispatch_generation,
        first_wake.identity.dispatch_generation);
    EXPECT_EQ(
        second_terminal->stop->identity.physical_generation,
        first_wake.identity.physical_generation);
    EXPECT_EQ(
        second_terminal->stop->identity.workset_epoch,
        WorksetEpoch(1));
    EXPECT_EQ(
        second_terminal->stop->event->evidence.path,
        NativeStopPath::Jit);
    // Suppression belongs only to the future-only Wake subscription. The
    // passive Observe subscription still sees the exact source re-entry,
    // followed by the later hit that satisfies the Wake.
    ASSERT_EQ(observe_consumer.deliveries.size(), 3u);
    EXPECT_FALSE(
        observe_consumer.deliveries[1].event.active_foreground_wait);
    EXPECT_LT(
        observe_consumer.deliveries[1].event.identity.sequence,
        second_terminal->stop->identity.sequence);
    ExpectSameIdentity(
        observe_consumer.deliveries[2].event.identity,
        second_terminal->stop->identity);

    const auto before_final_step =
        session.execution_snapshot()->evidence.vi_count;
    const ExecutionSubmissionReceipt final_step =
        session.SubmitExecution(StepFramesRequest{
            .policy = MakeExecutionPolicy(WorksetEpoch(1)),
            .count = 1,
        });
    ASSERT_TRUE(final_step.accepted) << final_step.error.message;
    const auto final_step_terminal = DriveUntilTerminal(
        session,
        notification_counter,
        ingress_signal,
        10s);
    ASSERT_TRUE(final_step_terminal.has_value());
    ASSERT_EQ(
        final_step_terminal->status,
        ExecutionTerminalStatus::StepsCompleted);
    EXPECT_EQ(
        final_step_terminal->evidence.core_state,
        BackendCoreState::Paused);
    EXPECT_GT(
        final_step_terminal->evidence.vi_count,
        before_final_step);

    EXPECT_TRUE(observe_registration.handle.Release().ok);
    const CaptureServiceReceipt capture_detached =
        capture->Detach(capture_attached.attachment);
    ASSERT_TRUE(capture_detached.ok)
        << capture_detached.error.message;
    EXPECT_TRUE(capture_detached.artifacts_finalized);
    EXPECT_TRUE(std::filesystem::is_regular_file(
        temporary.path() / "recurring-pc.scap"));
    EXPECT_TRUE(router->DesiredPhysicalPlan().pcs.empty());
    EXPECT_TRUE(router->DesiredPhysicalPlan().memory.empty());

    const SessionOperationReceipt shutdown = session.Shutdown();
    ASSERT_TRUE(shutdown.ok) << shutdown.backend.message;
    EXPECT_FALSE(savor::probe::NativeStopHooksInstalled());
    EXPECT_EQ(savor::probe::BoundNativeStopSink(), nullptr);
}

} // namespace
