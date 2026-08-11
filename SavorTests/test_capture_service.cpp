#include <gtest/gtest.h>

#include "Runner/Runtime/Services/Capture/CaptureService.h"
#include "common/FakePhysicalStopBackend.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace savor::runtime;
using namespace savor::test_support;

constexpr std::uint32_t kInitialPc = 0x80100000u;
constexpr std::uint32_t kReplacementPc = 0x80101000u;

ProbeRouterAdapterConfig AdapterConfig()
{
    return {
        .source = {
            .id = StopSourceId(1701),
            .stable_name = "capture.service.test",
            .diagnostic_label = "capture service test",
        },
        .group_id = StopSubscriptionGroupId(1702),
        .first_subscription_id = StopSubscriptionId(1710),
        .cpu_observer_descriptor_id = 1799,
        .priority = -50,
    };
}

std::string ProfileJson(std::uint64_t revision = 1)
{
    savor::probe::Profile profile;
    profile.name = "capture-service-test";
    profile.revision = revision;
    profile.expected_module_sha256 =
        savor::probe::current_module_sha256();
    profile.probes = {
        savor::probe::ProbeDefinition{
            .id = "passive",
            .kind = savor::probe::ProbeKind::Pc,
            .subscriptions =
                savor::probe::Subscription::Capture |
                savor::probe::Subscription::Control,
            .address = kInitialPc,
        },
    };
    return savor::probe::serialize_profile_json(profile);
}

struct FakeCaptureState
{
    ProbeRouterAdapterConfig config;
    StopSubscriptionGroupDefinition definition;
    std::optional<ProbeRouterReconcileRequest> pending;
    CaptureAdapterFinalization finalization{
        .ok = true,
        .capture_complete = true,
    };
    std::string received_profile_json;
    std::vector<RoutedStopEvent> observed;
    std::vector<StopDelivery> delivered;
    std::vector<std::pair<std::string, std::uint64_t>> markers;
    std::size_t create_count = 0;
    std::size_t start_count = 0;
    std::size_t finalize_count = 0;
    bool active = false;
    bool fail_start = false;
    bool reconcile_on_delivery = false;
};

StopSubscriptionGroupDefinition DefinitionAt(
    const ProbeRouterAdapterConfig& config,
    std::uint32_t pc)
{
    return {
        .id = config.group_id,
        .source = config.source,
        .subscriptions = {
            StopSubscriptionDefinition{
                .id = config.first_subscription_id,
                .point = PcStopPointSpec{pc},
                .route = PassiveStopObservation{
                    .cpu_observer_descriptor_id =
                        config.cpu_observer_descriptor_id,
                    .lossless = true},
                .lifetime = StopSubscriptionLifetime::Scoped,
                .priority = config.priority,
            },
        },
    };
}

class FakeCaptureProfileAdapter final
    : public ICaptureProfileAdapter
{
public:
    explicit FakeCaptureProfileAdapter(
        std::shared_ptr<FakeCaptureState> state)
        : state_(std::move(state))
    {
    }

    bool Start(
        savor::probe::Profile,
        savor::probe::SessionOptions options,
        std::string* error_out) override
    {
        ++state_->start_count;
        state_->received_profile_json =
            std::move(options.metadata.profile_json);
        if (state_->fail_start)
        {
            if (error_out)
                *error_out = "injected capture start failure";
            return false;
        }
        state_->active = true;
        return true;
    }

    bool active() const noexcept override
    {
        return state_->active;
    }

    ProbeRouterGroupBuildResult
    BuildCurrentGroupDefinition() override
    {
        return {
            .ok = true,
            .definition = state_->definition,
        };
    }

    std::optional<ProbeRouterReconcileRequest>
    TakeReconcileRequest() override
    {
        return std::exchange(state_->pending, std::nullopt);
    }

    bool EmitMarker(
        std::string_view id,
        std::uint64_t value) override
    {
        if (!state_->active || id.empty())
            return false;
        state_->markers.emplace_back(id, value);
        return true;
    }

    StopCpuObservationResult ObserveRoutedHit(
        std::uint32_t descriptor_id,
        const RoutedStopEvent& event) noexcept override
    {
        if (descriptor_id !=
            state_->config.cpu_observer_descriptor_id)
        {
            return StopCpuObservationResult::Failed;
        }
        state_->observed.push_back(event);
        return state_->reconcile_on_delivery
            ? StopCpuObservationResult::ObservedRequiresReconcile
            : StopCpuObservationResult::Observed;
    }

    void OnStopPoint(const StopDelivery& delivery) override
    {
        state_->delivered.push_back(delivery);
        if (!state_->reconcile_on_delivery)
            return;
        state_->definition =
            DefinitionAt(state_->config, kReplacementPc);
        state_->pending = ProbeRouterReconcileRequest{
            .request_sequence = 3,
            .action = ProbeRouterReconcileAction::ReplaceGroup,
            .cause = delivery.event.identity,
            .replacement = state_->definition,
        };
    }

    CaptureAdapterFinalization Finalize() noexcept override
    {
        ++state_->finalize_count;
        state_->active = false;
        return state_->finalization;
    }

private:
    std::shared_ptr<FakeCaptureState> state_;
};

class FakeCaptureBackend final : public ICaptureBackendPort
{
public:
    explicit FakeCaptureBackend(
        std::shared_ptr<FakeCaptureState> state)
        : state_(std::move(state))
    {
    }

    std::unique_ptr<ICaptureProfileAdapter>
    CreateCaptureProfileAdapter(
        const ProbeRouterAdapterConfig& config,
        std::string*) override
    {
        ++state_->create_count;
        state_->config = config;
        if (state_->definition.subscriptions.empty())
            state_->definition = DefinitionAt(config, kInitialPc);
        return std::make_unique<FakeCaptureProfileAdapter>(state_);
    }

private:
    std::shared_ptr<FakeCaptureState> state_;
};

struct ServiceFixture
{
    ServiceFixture()
        : stop_control(
              std::make_shared<FakePhysicalStopBackendControl>()),
          stop_backend(stop_control),
          manager(stop_backend),
          capture_backend(capture_state),
          service(capture_backend, AdapterConfig()),
          router(manager, nullptr, &service)
    {
        EXPECT_TRUE(router.Initialize(WorksetEpoch(1)).ok);
        EXPECT_TRUE(
            service.BindRouter(router, WorksetEpoch(1)).ok);
    }

    ~ServiceFixture()
    {
        (void)service.Shutdown();
        (void)router.StopIngressDrainAndCleanup();
    }

    CaptureServiceReceipt Attach()
    {
        return service.Attach({
            .profile_json = ProfileJson(),
            .expected_epoch = WorksetEpoch(1),
        });
    }

    std::shared_ptr<FakeCaptureState> capture_state =
        std::make_shared<FakeCaptureState>();
    std::shared_ptr<FakePhysicalStopBackendControl> stop_control;
    FakePhysicalStopBackend stop_backend;
    PhysicalStopPointManager manager;
    FakeCaptureBackend capture_backend;
    CaptureService service;
    StopPointRouter router;
};

TEST(CaptureService, FailedAttachCleanupFailureTaintsAndBlocksReuse)
{
    ServiceFixture fixture;
    fixture.capture_state->fail_start = true;
    fixture.capture_state->finalization = {
        .ok = false,
        .capture_complete = false,
        .message = "injected failed-attach cleanup",
    };

    const auto failed = fixture.Attach();
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(
        failed.error.code,
        CaptureServiceErrorCode::AdapterStartFailed);
    EXPECT_TRUE(failed.requires_session_taint);
    EXPECT_NE(
        failed.error.message.find("injected failed-attach cleanup"),
        std::string::npos);

    fixture.capture_state->fail_start = false;
    fixture.capture_state->finalization = {
        .ok = true,
        .capture_complete = true,
    };
    const auto blocked = fixture.Attach();
    EXPECT_FALSE(blocked.ok);
    EXPECT_EQ(
        blocked.error.code,
        CaptureServiceErrorCode::FinalizationFailed);
}

TEST(CaptureService, OwnsOneOpaquePassiveAttachment)
{
    ServiceFixture fixture;
    const auto attached = fixture.Attach();
    ASSERT_TRUE(attached.ok) << attached.error.message;
    EXPECT_TRUE(attached.attachment);
    EXPECT_EQ(fixture.capture_state->create_count, 1u);
    EXPECT_EQ(fixture.capture_state->start_count, 1u);
    EXPECT_EQ(
        fixture.capture_state->received_profile_json,
        ProfileJson());
    ASSERT_EQ(
        fixture.router.DesiredPhysicalPlan().pcs.size(),
        1u);
    EXPECT_EQ(
        fixture.router.DesiredPhysicalPlan().pcs[0].pc,
        kInitialPc);
    EXPECT_TRUE(attached.physical_generation);
    EXPECT_EQ(
        attached.physical_generation,
        fixture.router.physical_generation());

    const auto duplicate = fixture.Attach();
    EXPECT_FALSE(duplicate.ok);
    EXPECT_EQ(
        duplicate.error.code,
        CaptureServiceErrorCode::AlreadyAttached);
    EXPECT_EQ(fixture.capture_state->create_count, 1u);
}

TEST(CaptureService, EmitsMarkerThroughAttachedPassiveProfile)
{
    ServiceFixture fixture;
    const auto attached = fixture.Attach();
    ASSERT_TRUE(attached.ok) << attached.error.message;

    const auto marked = fixture.service.Mark(
        "program.phase",
        17);
    ASSERT_TRUE(marked.ok) << marked.error.message;
    ASSERT_EQ(fixture.capture_state->markers.size(), 1u);
    EXPECT_EQ(
        fixture.capture_state->markers[0],
        std::make_pair(std::string("program.phase"), std::uint64_t{17}));

    EXPECT_EQ(
        fixture.service.Mark(
            {},
            0)
            .error.code,
        CaptureServiceErrorCode::InvalidArgument);
}

TEST(
    CaptureService,
    StableEndpointPreservesRoutedIdentityAndExistingWakeFact)
{
    ServiceFixture fixture;
    const auto attached = fixture.Attach();
    ASSERT_TRUE(attached.ok) << attached.error.message;

    struct WakeConsumer final : IStopPointConsumer
    {
        void OnStopPoint(const StopDelivery&) override
        {
        }
    } wake_consumer;
    auto wake = fixture.router.RegisterGroup({
        .id = StopSubscriptionGroupId(1800),
        .source = {
            .id = StopSourceId(1800),
            .stable_name = "capture.service.wake",
            .diagnostic_label = "capture service wake",
        },
        .subscriptions = {
            StopSubscriptionDefinition{
                .id = StopSubscriptionId(1801),
                .point = PcStopPointSpec{kInitialPc},
                .route = ForegroundStopWait{},
                .lifetime = StopSubscriptionLifetime::Scoped,
                .consumer = &wake_consumer,
            },
        },
    });
    ASSERT_TRUE(wake.receipt.ok)
        << wake.receipt.error.message;

    const auto decision =
        fixture.stop_backend.InjectJitPcStop(kInitialPc);
    EXPECT_TRUE(decision.request_break);
    const auto receipts = fixture.router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    ASSERT_EQ(fixture.capture_state->observed.size(), 1u);
    ASSERT_EQ(fixture.capture_state->delivered.size(), 1u);
    EXPECT_TRUE(
        fixture.capture_state->observed[0]
            .active_foreground_wait);
    EXPECT_EQ(
        fixture.capture_state->observed[0].identity,
        fixture.capture_state->delivered[0]
            .event.identity);
    EXPECT_EQ(
        fixture.capture_state->observed[0].identity,
        receipts[0].identity);
    EXPECT_EQ(
        fixture.capture_state->delivered[0].event
            .active_foreground_wait,
        true);

    EXPECT_TRUE(wake.handle.Release().ok);
}

TEST(
    CaptureService,
    AppliesDynamicProfileReconciliationOnlyOnActorRequest)
{
    ServiceFixture fixture;
    fixture.capture_state->reconcile_on_delivery = true;
    const auto attached = fixture.Attach();
    ASSERT_TRUE(attached.ok) << attached.error.message;

    (void)fixture.stop_backend.InjectJitPcStop(kInitialPc);
    const auto receipts = fixture.router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    ASSERT_TRUE(receipts[0].event.has_value());
    EXPECT_TRUE(
        receipts[0].event
            ->requires_physical_reconcile_before_resume);

    // CPU observation and actor delivery only queue the request.
    ASSERT_EQ(
        fixture.router.DesiredPhysicalPlan().pcs.size(),
        1u);
    EXPECT_EQ(
        fixture.router.DesiredPhysicalPlan().pcs[0].pc,
        kInitialPc);

    const auto reconciled =
        fixture.service.ReconcileBeforeResume();
    ASSERT_TRUE(reconciled.ok)
        << reconciled.error.message;
    ASSERT_EQ(
        fixture.router.DesiredPhysicalPlan().pcs.size(),
        1u);
    EXPECT_EQ(
        fixture.router.DesiredPhysicalPlan().pcs[0].pc,
        kReplacementPc);
    EXPECT_EQ(
        fixture.service.snapshot().reconcile_count,
        1u);
}

TEST(CaptureService, DetachReleasesGroupAndFinalizesArtifacts)
{
    ServiceFixture fixture;
    fixture.capture_state->finalization = {
        .ok = true,
        .capture_complete = false,
        .capture_drop_count = 4,
        .progress_drop_count = 7,
        .incomplete_reason = "injected overflow",
    };
    const auto attached = fixture.Attach();
    ASSERT_TRUE(attached.ok) << attached.error.message;

    const auto detached =
        fixture.service.Detach(attached.attachment);
    ASSERT_TRUE(detached.ok)
        << detached.error.message;
    EXPECT_TRUE(detached.artifacts_finalized);
    EXPECT_FALSE(detached.capture_complete);
    EXPECT_EQ(detached.capture_drop_count, 4u);
    EXPECT_EQ(detached.progress_drop_count, 7u);
    EXPECT_EQ(
        detached.incomplete_reason,
        "injected overflow");
    EXPECT_EQ(fixture.capture_state->finalize_count, 1u);
    EXPECT_FALSE(fixture.service.snapshot().attached);
    EXPECT_TRUE(
        fixture.router.DesiredPhysicalPlan().pcs.empty());
}

TEST(CaptureService, FinalizationFailureRequiresTaintAndBlocksNewAttachment)
{
    ServiceFixture fixture;
    fixture.capture_state->finalization = {
        .ok = false,
        .capture_complete = false,
        .incomplete_reason = "injected incomplete capture",
        .message = "injected artifact close failure",
    };
    const auto attached = fixture.Attach();
    ASSERT_TRUE(attached.ok) << attached.error.message;

    const auto detached =
        fixture.service.Detach(attached.attachment);
    EXPECT_FALSE(detached.ok);
    EXPECT_EQ(
        detached.error.code,
        CaptureServiceErrorCode::FinalizationFailed);
    EXPECT_FALSE(detached.artifacts_finalized);
    EXPECT_TRUE(detached.requires_session_taint);
    EXPECT_FALSE(fixture.service.snapshot().attached);

    fixture.capture_state->finalization = {
        .ok = true,
        .capture_complete = true,
    };
    const auto replacement = fixture.Attach();
    EXPECT_FALSE(replacement.ok);
    EXPECT_EQ(
        replacement.error.code,
        CaptureServiceErrorCode::FinalizationFailed);

    const auto first_shutdown = fixture.service.Shutdown();
    const auto second_shutdown = fixture.service.Shutdown();
    EXPECT_FALSE(first_shutdown.ok);
    EXPECT_TRUE(first_shutdown.requires_session_taint);
    EXPECT_EQ(
        second_shutdown.error.code,
        first_shutdown.error.code);
    EXPECT_EQ(
        second_shutdown.requires_session_taint,
        first_shutdown.requires_session_taint);
}

} // namespace
