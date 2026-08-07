#include <gtest/gtest.h>

#include "Runner/Runtime/StopPoints/Capture/ProbeRouterAdapter.h"
#include "common/FakePhysicalStopBackend.h"

#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace savor::probe;
using namespace savor::runtime;
using namespace savor::test_support;

constexpr std::uint32_t kCapturePc = 0x80001000u;
constexpr std::uint32_t kProgressPc = 0x80002000u;
constexpr std::uint32_t kActivationPc = 0x80003000u;
constexpr std::uint32_t kOneShotPc = 0x80004000u;
constexpr std::uint32_t kStaticMemory = 0x80300000u;
constexpr std::uint32_t kDynamicMemory = 0x80310000u;

struct ScopedU32Restore
{
    std::uint32_t& target;
    std::uint32_t original;

    ~ScopedU32Restore()
    {
        target = original;
    }
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

ProbeRouterAdapterConfig AdapterConfig()
{
    return {
        .source = {
            .id = StopSourceId(701),
            .stable_name = "capture.profile.test",
            .diagnostic_label = "capture profile test",
        },
        .group_id = StopSubscriptionGroupId(702),
        .first_subscription_id = StopSubscriptionId(710),
        .cpu_observer_descriptor_id = 799,
        .priority = -50,
    };
}

Profile BaseProfile()
{
    Profile profile;
    profile.name = "probe-router-adapter-test";
    profile.expected_module_sha256 = current_module_sha256();
    profile.limits.queue_bytes =
        16ull * savor::probe::kRawEventSlotBytes;
    profile.limits.max_events = 16;
    profile.limits.progress_events = 16;
    return profile;
}

const StopSubscriptionDefinition* FindPc(
    const StopSubscriptionGroupDefinition& group,
    std::uint32_t pc)
{
    const auto found = std::ranges::find_if(
        group.subscriptions,
        [&](const StopSubscriptionDefinition& subscription) {
            const auto* point =
                std::get_if<PcStopPointSpec>(&subscription.point);
            return point && point->pc == pc;
        });
    return found == group.subscriptions.end() ? nullptr : &*found;
}

const StopSubscriptionDefinition* FindMemory(
    const StopSubscriptionGroupDefinition& group,
    std::uint32_t address)
{
    const auto found = std::ranges::find_if(
        group.subscriptions,
        [&](const StopSubscriptionDefinition& subscription) {
            const auto* point =
                std::get_if<MemoryStopPointSpec>(&subscription.point);
            return point && point->address == address;
        });
    return found == group.subscriptions.end() ? nullptr : &*found;
}

StopDelivery PcDelivery(
    const ProbeRouterAdapterConfig& config,
    std::uint32_t pc,
    std::uint64_t sequence,
    std::uint64_t snapshot,
    std::uint64_t epoch,
    bool active_foreground_wake = false)
{
    StopDelivery delivery;
    delivery.source_id = config.source.id;
    delivery.group_id = config.group_id;
    delivery.subscription_id = config.first_subscription_id;
    delivery.delivery = StopDeliveryMode::Observe;
    delivery.event.identity = {
        RoutedStopSequence(sequence),
        StopSampleSnapshotId(snapshot),
        WorksetEpoch(epoch),
        StopDispatchGeneration(1),
    };
    delivery.event.evidence = {
        NativeStopPath::Jit,
        PcStopPointSpec{pc},
        pc,
        0,
        false,
    };
    delivery.event.active_foreground_wake =
        active_foreground_wake;
    return delivery;
}

StopDelivery MemoryDelivery(
    const ProbeRouterAdapterConfig& config,
    std::uint32_t address,
    std::uint64_t sequence,
    std::uint64_t snapshot,
    std::uint64_t epoch)
{
    StopDelivery delivery;
    delivery.source_id = config.source.id;
    delivery.group_id = config.group_id;
    delivery.subscription_id = config.first_subscription_id;
    delivery.delivery = StopDeliveryMode::Observe;
    delivery.event.identity = {
        RoutedStopSequence(sequence),
        StopSampleSnapshotId(snapshot),
        WorksetEpoch(epoch),
        StopDispatchGeneration(1),
    };
    delivery.event.evidence = {
        NativeStopPath::Memcheck,
        MemoryStopPointSpec{
            address,
            4,
            StopMemoryAccess::Write,
        },
        kActivationPc,
        0x12345678u,
        true,
    };
    return delivery;
}

TEST(CaptureRouterAdapter, LowersOnlySourceScopedPassiveRequirements)
{
    Profile profile = BaseProfile();
    ASSERT_FALSE(profile.expected_module_sha256.empty());
    profile.probes = {
        ProbeDefinition{
            .id = "capture-control",
            .kind = ProbeKind::Pc,
            .subscriptions =
                Subscription::Capture | Subscription::Control,
            .address = kCapturePc,
        },
        ProbeDefinition{
            .id = "progress-only",
            .kind = ProbeKind::Pc,
            .subscriptions = Subscription::Progress,
            .address = kProgressPc,
        },
        ProbeDefinition{
            .id = "static-write",
            .kind = ProbeKind::Memory,
            .subscriptions = Subscription::Capture,
            .address = kStaticMemory,
            .size = 4,
            .memory_access = MemoryAccess::Write,
        },
        ProbeDefinition{
            .id = "overlapping-progress-read",
            .kind = ProbeKind::Memory,
            .subscriptions = Subscription::Progress,
            .address = kStaticMemory + 2,
            .size = 4,
            .memory_access = MemoryAccess::Read,
        },
        ProbeDefinition{
            .id = "dynamic",
            .kind = ProbeKind::Memory,
            .subscriptions = Subscription::Capture,
            .size = 4,
            .memory_access = MemoryAccess::Write,
            .activate_on_pc = kActivationPc,
            .address_program = {0x06, 3, 0x00},
        },
    };

    auto config = AdapterConfig();
    ProbeRouterAdapter adapter(
        Core::System::GetInstance(),
        config);
    std::string error;
    ASSERT_TRUE(adapter.Start(profile, {}, &error)) << error;

    const auto built = adapter.BuildCurrentGroupDefinition();
    ASSERT_TRUE(built.ok) << built.error;
    EXPECT_EQ(built.definition.id, config.group_id);
    EXPECT_EQ(built.definition.source.id, config.source.id);
    ASSERT_EQ(built.definition.subscriptions.size(), 4u);

    const auto* capture = FindPc(built.definition, kCapturePc);
    const auto* progress = FindPc(built.definition, kProgressPc);
    const auto* activation = FindPc(built.definition, kActivationPc);
    const auto* memory = FindMemory(built.definition, kStaticMemory);
    ASSERT_NE(capture, nullptr);
    ASSERT_NE(progress, nullptr);
    ASSERT_NE(activation, nullptr);
    ASSERT_NE(memory, nullptr);
    EXPECT_EQ(capture->delivery, StopDeliveryMode::Observe);
    EXPECT_TRUE(capture->lossless);
    EXPECT_EQ(progress->delivery, StopDeliveryMode::Progress);
    EXPECT_FALSE(progress->lossless);
    EXPECT_EQ(activation->delivery, StopDeliveryMode::Observe);
    EXPECT_TRUE(activation->lossless);
    ASSERT_TRUE(std::holds_alternative<MemoryStopPointSpec>(
        memory->point));
    const auto memory_point =
        std::get<MemoryStopPointSpec>(memory->point);
    EXPECT_EQ(memory_point.size, 6u);
    EXPECT_EQ(memory_point.access, StopMemoryAccess::Access);

    for (const auto& subscription :
        built.definition.subscriptions)
    {
        EXPECT_TRUE(
            subscription.delivery == StopDeliveryMode::Observe ||
            subscription.delivery == StopDeliveryMode::Progress);
        EXPECT_EQ(subscription.policy, StopRoutingPolicy::Pass);
        EXPECT_EQ(
            subscription.lifetime,
            StopSubscriptionLifetime::Scoped);
        EXPECT_TRUE(subscription.interruption_handler_key.empty());
        EXPECT_EQ(
            subscription.cpu_observer_descriptor_id,
            config.cpu_observer_descriptor_id);
        EXPECT_EQ(subscription.consumer, &adapter);
    }
}

TEST(
    CaptureRouterAdapter,
    LowersDispersedProfileBeyondPerHitDeliveryCapacity)
{
    Profile profile = BaseProfile();
    ASSERT_FALSE(profile.expected_module_sha256.empty());
    constexpr std::size_t kProbeCount =
        kMaxStopDeliveriesPerHit + 9;
    profile.probes.reserve(kProbeCount);
    for (std::size_t i = 0; i < kProbeCount; ++i)
    {
        profile.probes.push_back(ProbeDefinition{
            .id = "dispersed-" + std::to_string(i),
            .kind = ProbeKind::Pc,
            .subscriptions = Subscription::Capture,
            .address =
                kCapturePc +
                static_cast<std::uint32_t>(i * 4),
        });
    }

    const auto config = AdapterConfig();
    ProbeRouterAdapter adapter(
        Core::System::GetInstance(),
        config);
    std::string error;
    ASSERT_TRUE(adapter.Start(profile, {}, &error)) << error;

    auto built = adapter.BuildCurrentGroupDefinition();
    ASSERT_TRUE(built.ok) << built.error;
    ASSERT_EQ(
        built.definition.subscriptions.size(),
        kProbeCount);
    EXPECT_EQ(
        std::get<PcStopPointSpec>(
            built.definition.subscriptions.front().point).pc,
        kCapturePc);
    EXPECT_EQ(
        std::get<PcStopPointSpec>(
            built.definition.subscriptions.back().point).pc,
        kCapturePc +
            static_cast<std::uint32_t>(
                (kProbeCount - 1) * 4));
}

TEST(
    CaptureRouterAdapter,
    ActiveForegroundWakeQualifiesControlAndRetainsRoutedIdentity)
{
    Profile profile = BaseProfile();
    ASSERT_FALSE(profile.expected_module_sha256.empty());
    profile.probes = {
        ProbeDefinition{
            .id = "progress-control",
            .kind = ProbeKind::Pc,
            .subscriptions =
                Subscription::Progress | Subscription::Control,
            .address = kCapturePc,
        },
    };

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<savor::capture_format::Event> progress_events;
    SessionOptions options;
    options.progress_callback =
        [&](const savor::capture_format::Event& event, bool) {
            {
                std::lock_guard lock(mutex);
                progress_events.push_back(event);
            }
            changed.notify_all();
        };

    const auto config = AdapterConfig();
    ProbeRouterAdapter adapter(
        Core::System::GetInstance(),
        config);
    std::string error;
    ASSERT_TRUE(
        adapter.Start(profile, std::move(options), &error))
        << error;

    StopDelivery active =
        PcDelivery(config, kCapturePc, 41, 51, 7, true);
    active.event.sample_count = 2;
    active.event.samples[0] = {
        .descriptor_id = 801,
        .value = 0x1122,
        .available = true,
    };
    active.event.samples[1] = {
        .descriptor_id = 802,
        .value = 0,
        .available = false,
    };
    ASSERT_EQ(
        adapter.ObserveRoutedHit(
            config.cpu_observer_descriptor_id,
            active.event),
        StopCpuObservationResult::Observed);
    adapter.OnStopPoint(active);
    ASSERT_EQ(adapter.probe_runtime().metrics().size(), 1u);
    EXPECT_EQ(
        adapter.probe_runtime().metrics()[0].control_publications,
        1u);

    auto inactive =
        PcDelivery(config, kCapturePc, 42, 52, 8, false);
    ASSERT_EQ(
        adapter.ObserveRoutedHit(
            config.cpu_observer_descriptor_id,
            inactive.event),
        StopCpuObservationResult::Observed);
    adapter.OnStopPoint(inactive);
    EXPECT_EQ(
        adapter.probe_runtime().metrics()[0].control_publications,
        1u);

    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 5s, [&] {
            return progress_events.size() == 2;
        }));
    }
    ASSERT_EQ(progress_events.size(), 2u);
    EXPECT_EQ(progress_events[0].capture_sequence, 41u);
    EXPECT_EQ(progress_events[0].snapshot_id, 51u);
    EXPECT_EQ(progress_events[0].guest_workset_epoch, 7u);
    EXPECT_EQ(progress_events[1].capture_sequence, 42u);
    EXPECT_EQ(progress_events[1].snapshot_id, 52u);
    EXPECT_EQ(progress_events[1].guest_workset_epoch, 8u);
    EXPECT_FALSE(adapter.TakeReconcileRequest().has_value());
}

TEST(
    CaptureRouterAdapter,
    CpuObserverSamplesPredicatesAndAddressesBeforeActorDelivery)
{
    Profile profile = BaseProfile();
    ASSERT_FALSE(profile.expected_module_sha256.empty());
    profile.probes = {
        ProbeDefinition{
            .id = "hit-time-progress",
            .kind = ProbeKind::Pc,
            .subscriptions = Subscription::Progress,
            .address = kActivationPc,
            .predicate = {
                PredicateInstruction{
                    .op = PredicateOp::PushGpr,
                    .operand = 3,
                },
                PredicateInstruction{
                    .op = PredicateOp::PushConstant,
                    .operand = 5,
                },
                PredicateInstruction{
                    .op = PredicateOp::Equal,
                },
            },
            .samples = {
                SampleDefinition{
                    .name = "gpr3-at-hit",
                    .kind = SampleKind::Gpr,
                    .base_register = 3,
                },
            },
        },
        ProbeDefinition{
            .id = "hit-time-address",
            .kind = ProbeKind::Memory,
            .subscriptions = Subscription::Capture,
            .size = 4,
            .memory_access = MemoryAccess::Write,
            .activate_on_pc = kActivationPc,
            .address_program = {0x06, 4, 0x00},
        },
    };

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<savor::capture_format::Event> progress_events;
    SessionOptions options;
    options.progress_callback =
        [&](const savor::capture_format::Event& event, bool) {
            {
                std::lock_guard lock(mutex);
                progress_events.push_back(event);
            }
            changed.notify_all();
        };

    const auto config = AdapterConfig();
    ProbeRouterAdapter adapter(
        Core::System::GetInstance(),
        config);
    std::string error;
    ASSERT_TRUE(
        adapter.Start(profile, std::move(options), &error))
        << error;

    auto& ppc_state =
        Core::System::GetInstance().GetPowerPC().GetPPCState();
    ScopedU32Restore restore_gpr3{
        ppc_state.gpr[3],
        ppc_state.gpr[3],
    };
    ScopedU32Restore restore_gpr4{
        ppc_state.gpr[4],
        ppc_state.gpr[4],
    };
    ppc_state.gpr[3] = 5;
    ppc_state.gpr[4] = kDynamicMemory;

    auto delivery =
        PcDelivery(config, kActivationPc, 81, 91, 11);
    ASSERT_EQ(
        adapter.ObserveRoutedHit(
            config.cpu_observer_descriptor_id,
            delivery.event),
        StopCpuObservationResult::ObservedRequiresReconcile);

    // Model the CPU continuing before actor delivery. All profile sampling,
    // predicate evaluation, and dynamic address derivation must already be
    // complete.
    ppc_state.gpr[3] = 9;
    ppc_state.gpr[4] = kDynamicMemory + 0x100;
    adapter.OnStopPoint(delivery);

    const auto metrics = adapter.probe_runtime().metrics();
    ASSERT_EQ(metrics.size(), 2u);
    EXPECT_EQ(metrics[0].hits, 1u);
    EXPECT_EQ(metrics[0].sampled, 1u);
    EXPECT_EQ(metrics[0].predicate_rejections, 0u);

    auto reconcile = adapter.TakeReconcileRequest();
    ASSERT_TRUE(reconcile.has_value());
    EXPECT_NE(
        FindMemory(reconcile->replacement, kDynamicMemory),
        nullptr);
    EXPECT_EQ(
        FindMemory(
            reconcile->replacement,
            kDynamicMemory + 0x100),
        nullptr);

    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 5s, [&] {
            return progress_events.size() == 1;
        }));
    }
    ASSERT_EQ(progress_events.size(), 1u);
    EXPECT_EQ(progress_events[0].capture_sequence, 81u);
    EXPECT_EQ(progress_events[0].snapshot_id, 91u);
    EXPECT_EQ(progress_events[0].guest_workset_epoch, 11u);
    const auto sample = std::ranges::find_if(
        progress_events[0].fields,
        [](const savor::capture_format::Field& field) {
            return field.name == "gpr3-at-hit";
        });
    ASSERT_NE(sample, progress_events[0].fields.end());
    EXPECT_EQ(sample->status, savor::capture_format::FieldStatus::Present);
    EXPECT_EQ(sample->value, 5u);

    // Actor delivery is bookkeeping only; it must not re-run the profile.
    const auto after_actor = adapter.probe_runtime().metrics();
    ASSERT_EQ(after_actor.size(), 2u);
    EXPECT_EQ(after_actor[0].hits, 1u);
    EXPECT_EQ(after_actor[0].sampled, 1u);
}

TEST(
    CaptureRouterAdapter,
    WakeActivationRequiresActorReplacementBeforeResume)
{
    Profile profile = BaseProfile();
    ASSERT_FALSE(profile.expected_module_sha256.empty());
    profile.probes = {
        ProbeDefinition{
            .id = "wake-activated-memory",
            .kind = ProbeKind::Memory,
            .subscriptions =
                Subscription::Capture | Subscription::Control,
            .size = 4,
            .memory_access = MemoryAccess::Write,
            .activate_on_pc = kActivationPc,
            .address_program = {0x06, 3, 0x00},
        },
    };

    auto control =
        std::make_shared<FakePhysicalStopBackendControl>();
    FakePhysicalStopBackend backend(control);
    PhysicalStopPointManager manager(backend);
    const auto config = AdapterConfig();
    ProbeRouterAdapter adapter(
        Core::System::GetInstance(),
        config);
    StopPointRouter router(manager, nullptr, &adapter);

    ASSERT_TRUE(router.Initialize(WorksetEpoch(1)).ok);
    std::string error;
    ASSERT_TRUE(adapter.Start(profile, {}, &error)) << error;
    auto capture_definition =
        adapter.BuildCurrentGroupDefinition();
    ASSERT_TRUE(capture_definition.ok)
        << capture_definition.error;
    auto capture_registration = router.RegisterGroup(
        std::move(capture_definition.definition));
    ASSERT_TRUE(capture_registration.receipt.ok)
        << capture_registration.receipt.error.message;

    RecordingStopConsumer wake_consumer;
    StopSubscriptionGroupDefinition wake_definition{
        .id = StopSubscriptionGroupId(880),
        .source = {
            .id = StopSourceId(880),
            .stable_name = "interaction.test",
            .diagnostic_label = "interaction test wake",
        },
        .subscriptions = {
            StopSubscriptionDefinition{
                .id = StopSubscriptionId(881),
                .point = PcStopPointSpec{kActivationPc},
                .delivery = StopDeliveryMode::Wake,
                .policy = StopRoutingPolicy::Consume,
                .lifetime = StopSubscriptionLifetime::Scoped,
                .consumer = &wake_consumer,
            },
        },
    };
    auto wake_registration =
        router.RegisterGroup(std::move(wake_definition));
    ASSERT_TRUE(wake_registration.receipt.ok)
        << wake_registration.receipt.error.message;

    auto& ppc_state =
        Core::System::GetInstance().GetPowerPC().GetPPCState();
    ScopedU32Restore restore_gpr3{
        ppc_state.gpr[3],
        ppc_state.gpr[3],
    };
    ppc_state.gpr[3] = kDynamicMemory;

    const auto decision = backend.InjectJitPcStop(
        kActivationPc,
        &Core::System::GetInstance().GetPowerPC());
    EXPECT_TRUE(decision.request_break);

    const auto receipts = router.DrainIngress();
    ASSERT_EQ(receipts.size(), 1u);
    EXPECT_EQ(
        receipts[0].terminal,
        StopRouteTerminal::WokeForeground);
    EXPECT_TRUE(receipts[0].core_must_remain_stopped);
    ASSERT_TRUE(receipts[0].event.has_value());
    EXPECT_TRUE(receipts[0].event->active_foreground_wake);
    EXPECT_TRUE(
        receipts[0].event
            ->requires_physical_reconcile_before_resume);
    ASSERT_EQ(wake_consumer.deliveries.size(), 1u);
    EXPECT_TRUE(
        router.DesiredPhysicalPlan().memory.empty());

    auto reconcile = adapter.TakeReconcileRequest();
    ASSERT_TRUE(reconcile.has_value());
    EXPECT_EQ(
        reconcile->action,
        ProbeRouterReconcileAction::ReplaceGroup);
    EXPECT_EQ(
        reconcile->cause.sequence,
        receipts[0].identity.sequence);
    EXPECT_NE(
        FindMemory(reconcile->replacement, kDynamicMemory),
        nullptr);

    const auto replaced = capture_registration.handle.Replace(
        std::move(reconcile->replacement));
    ASSERT_TRUE(replaced.ok) << replaced.error.message;
    EXPECT_TRUE(std::ranges::any_of(
        router.DesiredPhysicalPlan().memory,
        [](const PhysicalMemoryStop& stop) {
            return stop.start == kDynamicMemory &&
                stop.end == kDynamicMemory + 3 &&
                !stop.read &&
                stop.write;
        }));

    EXPECT_TRUE(wake_registration.handle.Release().ok);
    EXPECT_TRUE(capture_registration.handle.Release().ok);
    EXPECT_TRUE(router.StopIngressDrainAndCleanup().ok);
    adapter.Stop();
}

TEST(
    CaptureRouterAdapter,
    DynamicAndExhaustedRequirementsBecomeActorReconcileRequests)
{
    Profile profile = BaseProfile();
    ASSERT_FALSE(profile.expected_module_sha256.empty());
    profile.probes = {
        ProbeDefinition{
            .id = "dynamic-one-shot",
            .kind = ProbeKind::Memory,
            .subscriptions = Subscription::Capture,
            .size = 4,
            .memory_access = MemoryAccess::Write,
            .activate_on_pc = kActivationPc,
            .one_shot = true,
            .address_program = {0x06, 3, 0x00},
        },
        ProbeDefinition{
            .id = "pc-one-shot",
            .kind = ProbeKind::Pc,
            .subscriptions = Subscription::Capture,
            .address = kOneShotPc,
            .one_shot = true,
        },
    };

    const auto config = AdapterConfig();
    ProbeRouterAdapter adapter(
        Core::System::GetInstance(),
        config);
    std::string error;
    ASSERT_TRUE(adapter.Start(profile, {}, &error)) << error;

    auto initial = adapter.BuildCurrentGroupDefinition();
    ASSERT_TRUE(initial.ok) << initial.error;
    EXPECT_NE(FindPc(initial.definition, kActivationPc), nullptr);
    EXPECT_NE(FindPc(initial.definition, kOneShotPc), nullptr);
    EXPECT_EQ(FindMemory(initial.definition, kDynamicMemory), nullptr);

    auto& power_pc =
        Core::System::GetInstance().GetPowerPC().GetPPCState();
    ScopedU32Restore restore_gpr3{
        power_pc.gpr[3],
        power_pc.gpr[3],
    };
    power_pc.gpr[3] = kDynamicMemory;

    auto activation =
        PcDelivery(config, kActivationPc, 61, 71, 9);
    ASSERT_EQ(
        adapter.ObserveRoutedHit(
            config.cpu_observer_descriptor_id,
            activation.event),
        StopCpuObservationResult::ObservedRequiresReconcile);
    adapter.OnStopPoint(activation);
    auto activated = adapter.TakeReconcileRequest();
    ASSERT_TRUE(activated.has_value());
    EXPECT_EQ(
        activated->action,
        ProbeRouterReconcileAction::ReplaceGroup);
    EXPECT_EQ(activated->cause.sequence, RoutedStopSequence(61));
    EXPECT_NE(
        FindMemory(activated->replacement, kDynamicMemory),
        nullptr);

    auto memory =
        MemoryDelivery(config, kDynamicMemory, 62, 72, 9);
    ASSERT_EQ(
        adapter.ObserveRoutedHit(
            config.cpu_observer_descriptor_id,
            memory.event),
        StopCpuObservationResult::ObservedRequiresReconcile);
    adapter.OnStopPoint(memory);
    auto memory_exhausted = adapter.TakeReconcileRequest();
    ASSERT_TRUE(memory_exhausted.has_value());
    EXPECT_EQ(
        FindMemory(memory_exhausted->replacement, kDynamicMemory),
        nullptr);
    EXPECT_NE(
        FindPc(memory_exhausted->replacement, kActivationPc),
        nullptr);

    auto one_shot =
        PcDelivery(config, kOneShotPc, 63, 73, 9);
    ASSERT_EQ(
        adapter.ObserveRoutedHit(
            config.cpu_observer_descriptor_id,
            one_shot.event),
        StopCpuObservationResult::ObservedRequiresReconcile);
    adapter.OnStopPoint(one_shot);
    auto pc_exhausted = adapter.TakeReconcileRequest();
    ASSERT_TRUE(pc_exhausted.has_value());
    EXPECT_EQ(
        FindPc(pc_exhausted->replacement, kOneShotPc),
        nullptr);
    EXPECT_NE(
        FindPc(pc_exhausted->replacement, kActivationPc),
        nullptr);
}

} // namespace
