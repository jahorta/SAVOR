#include <gtest/gtest.h>

#include "ProbeProfile.h"
#include "Runner/Breakpoints/BPRegistry.h"
#include "Runner/InputMacro/InputMacroPlan.h"
#include "Runner/Runtime/StopPoints/StopPointTypes.h"
#include "Runner/Script/PhaseScriptOpcodes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace savor;
using namespace savor::inputmacro;
using namespace savor::probe;
using namespace savor::runtime;

class DefinitionOnlyConsumer final : public IStopPointConsumer
{
public:
    void OnStopPoint(const StopDelivery&) override
    {
        ++delivery_count;
    }

    std::uint32_t delivery_count = 0;
};

DefinitionOnlyConsumer definition_consumer;

struct TranslationAudit
{
    std::uint32_t phase_script_vm_executions = 0;
    std::uint32_t physical_backend_calls = 0;
    std::uint32_t logical_group_registrations = 0;
    std::uint32_t logical_group_releases = 0;
};

struct LogicalPcGroup
{
    StopSubscriptionGroupDefinition definition;
    std::vector<BPKey> keys;
};

struct OrderedWakeGroup
{
    LogicalPcGroup group;
    TranslationAudit audit;
};

struct LegacyPcCatalogTranslation
{
    LogicalPcGroup canonical;
    LogicalPcGroup gated;
    LogicalPcGroup predicates;
    TranslationAudit audit;
};

struct NestedWakeGroup
{
    StopSubscriptionGroupId parent_group_id;
    OrderedWakeGroup child;
};

struct ScopedGroupRelease
{
    StopSourceId source_id;
    StopSubscriptionGroupId group_id;
};

struct ScopedMemoryGroup
{
    std::uint32_t legacy_watchpoint_id = 0;
    StopSubscriptionGroupDefinition definition;
    ScopedGroupRelease clear;
    TranslationAudit audit;
};

struct PassiveCaptureGroup
{
    StopSubscriptionGroupDefinition definition;
    std::vector<std::string> probe_ids;
    TranslationAudit audit;
};

StopSourceIdentity Source(
    std::uint64_t id,
    std::string stable_name,
    std::string diagnostic_label)
{
    return {
        .id = StopSourceId(id),
        .stable_name = std::move(stable_name),
        .diagnostic_label = std::move(diagnostic_label),
    };
}

StopSubscriptionDefinition PcSubscription(
    std::uint64_t subscription_id,
    std::uint32_t pc,
    StopDeliveryMode delivery,
    StopRoutingPolicy policy,
    bool suppress_immediate_reentry = false)
{
    return {
        .id = StopSubscriptionId(subscription_id),
        .point = PcStopPointSpec{pc},
        .delivery = delivery,
        .policy = policy,
        .lifetime = StopSubscriptionLifetime::Scoped,
        .suppress_immediate_reentry =
            suppress_immediate_reentry,
        .consumer = &definition_consumer,
    };
}

LogicalPcGroup LowerPcKeys(
    const BreakpointMap& map,
    std::span<const BPKey> keys,
    StopSourceIdentity source,
    StopSubscriptionGroupId group_id,
    StopSubscriptionId first_subscription_id,
    StopDeliveryMode delivery,
    StopRoutingPolicy policy,
    bool suppress_immediate_reentry = false)
{
    LogicalPcGroup lowered;
    lowered.definition.id = group_id;
    lowered.definition.source = std::move(source);

    auto next_subscription_id = first_subscription_id.value();
    for (const BPKey key : keys)
    {
        const BPAddr* address = map.find(key);
        if (address == nullptr || address->pc == 0)
            continue;

        // Preserve the logical-key order even when two names share one PC.
        // Physical union/deduplication belongs to PhysicalStopPointManager.
        lowered.keys.push_back(key);
        lowered.definition.subscriptions.push_back(PcSubscription(
            next_subscription_id++,
            address->pc,
            delivery,
            policy,
            suppress_immediate_reentry));
    }
    return lowered;
}

LegacyPcCatalogTranslation LowerLegacyPcCatalog(
    const BreakpointMap& map,
    std::span<const BPKey> canonical,
    std::span<const BPKey> gated,
    std::span<const BPKey> predicates)
{
    LegacyPcCatalogTranslation lowered;
    lowered.canonical = LowerPcKeys(
        map,
        canonical,
        Source(
            1001,
            "legacy.phase.canonical",
            "legacy canonical phase points"),
        StopSubscriptionGroupId(1101),
        StopSubscriptionId(1201),
        StopDeliveryMode::Observe,
        StopRoutingPolicy::Pass);
    lowered.gated = LowerPcKeys(
        map,
        gated,
        Source(
            1002,
            "legacy.phase.gated",
            "legacy gated phase points"),
        StopSubscriptionGroupId(1102),
        StopSubscriptionId(1301),
        StopDeliveryMode::Observe,
        StopRoutingPolicy::Pass);
    lowered.predicates = LowerPcKeys(
        map,
        predicates,
        Source(
            1003,
            "legacy.phase.predicates",
            "legacy predicate points"),
        StopSubscriptionGroupId(1103),
        StopSubscriptionId(1401),
        StopDeliveryMode::Observe,
        StopRoutingPolicy::Pass);
    lowered.audit.logical_group_registrations = 3;
    return lowered;
}

OrderedWakeGroup LowerExpectedAlternatives(
    const BreakpointMap& map,
    std::span<const BPKey> expected_keys,
    StopSourceIdentity source,
    StopSubscriptionGroupId group_id,
    StopSubscriptionId first_subscription_id,
    bool suppress_immediate_reentry)
{
    OrderedWakeGroup lowered;
    lowered.group = LowerPcKeys(
        map,
        expected_keys,
        std::move(source),
        group_id,
        first_subscription_id,
        StopDeliveryMode::Wake,
        StopRoutingPolicy::Consume,
        suppress_immediate_reentry);
    lowered.audit.logical_group_registrations = 1;
    return lowered;
}

NestedWakeGroup LowerExpectedOnlyMacroWait(
    const BreakpointMap& map,
    const BreakpointWaitAction& action,
    StopSourceIdentity owning_source,
    StopSubscriptionGroupId parent_group_id,
    StopSubscriptionGroupId child_group_id,
    StopSubscriptionId first_subscription_id)
{
    return {
        .parent_group_id = parent_group_id,
        .child = LowerExpectedAlternatives(
            map,
            action.expected_keys,
            std::move(owning_source),
            child_group_id,
            first_subscription_id,
            true),
    };
}

StopMemoryAccess ToStopMemoryAccess(
    PSMemoryWatchpointAccess access)
{
    switch (access)
    {
    case PSMemoryWatchpointAccess::Read:
        return StopMemoryAccess::Read;
    case PSMemoryWatchpointAccess::Write:
        return StopMemoryAccess::Write;
    case PSMemoryWatchpointAccess::Access:
        return StopMemoryAccess::Access;
    }
    return StopMemoryAccess::Write;
}

std::optional<ScopedMemoryGroup> LowerMemoryArmAndClear(
    const PSOp& arm,
    const PSOp& clear,
    StopSourceIdentity source,
    StopSubscriptionGroupId group_id,
    StopSubscriptionId subscription_id)
{
    if (arm.code != PSOpCode::ARM_MEMORY_WATCHPOINT ||
        clear.code != PSOpCode::CLEAR_MEMORY_WATCHPOINTS ||
        arm.memwatch.use_address_key != 0 ||
        arm.memwatch.address == 0 ||
        arm.memwatch.size == 0)
    {
        return std::nullopt;
    }

    ScopedMemoryGroup lowered;
    lowered.legacy_watchpoint_id = arm.memwatch.id;
    lowered.definition.id = group_id;
    lowered.definition.source = std::move(source);
    lowered.definition.subscriptions.push_back({
        .id = subscription_id,
        .point = MemoryStopPointSpec{
            arm.memwatch.address,
            arm.memwatch.size,
            ToStopMemoryAccess(arm.memwatch.access),
        },
        .delivery = StopDeliveryMode::Wake,
        .policy = StopRoutingPolicy::Consume,
        .lifetime = StopSubscriptionLifetime::Scoped,
        .consumer = &definition_consumer,
    });
    lowered.clear = {
        .source_id = lowered.definition.source.id,
        .group_id = lowered.definition.id,
    };
    lowered.audit.logical_group_registrations = 1;
    lowered.audit.logical_group_releases = 1;
    return lowered;
}

StopMemoryAccess ToStopMemoryAccess(MemoryAccess access)
{
    switch (access)
    {
    case MemoryAccess::Read:
        return StopMemoryAccess::Read;
    case MemoryAccess::Write:
        return StopMemoryAccess::Write;
    case MemoryAccess::Access:
        return StopMemoryAccess::Access;
    }
    return StopMemoryAccess::Write;
}

PassiveCaptureGroup LowerCaptureSites(
    std::span<const ProbeDefinition> probes,
    StopSourceIdentity source,
    StopSubscriptionGroupId group_id,
    StopSubscriptionId first_subscription_id)
{
    PassiveCaptureGroup lowered;
    lowered.definition.id = group_id;
    lowered.definition.source = std::move(source);

    auto next_subscription_id = first_subscription_id.value();
    for (const ProbeDefinition& probe : probes)
    {
        if (probe.kind == ProbeKind::Marker ||
            probe.address == 0)
        {
            continue;
        }

        const bool progress_only =
            has_subscription(
                probe.subscriptions,
                Subscription::Progress) &&
            !has_subscription(
                probe.subscriptions,
                Subscription::Capture) &&
            !has_subscription(
                probe.subscriptions,
                Subscription::Control);
        StopPointSpec point;
        if (probe.kind == ProbeKind::Pc)
        {
            point = PcStopPointSpec{probe.address};
        }
        else
        {
            if (probe.size == 0)
                continue;
            point = MemoryStopPointSpec{
                probe.address,
                probe.size,
                ToStopMemoryAccess(probe.memory_access),
            };
        }

        lowered.probe_ids.push_back(probe.id);
        lowered.definition.subscriptions.push_back({
            .id = StopSubscriptionId(next_subscription_id++),
            .point = std::move(point),
            .delivery = progress_only
                ? StopDeliveryMode::Progress
                : StopDeliveryMode::Observe,
            .policy = StopRoutingPolicy::Pass,
            .lifetime = StopSubscriptionLifetime::Scoped,
            .lossless = !progress_only,
            .consumer = &definition_consumer,
        });
    }
    lowered.audit.logical_group_registrations = 1;
    return lowered;
}

std::vector<std::uint32_t> PcOrder(
    const StopSubscriptionGroupDefinition& group)
{
    std::vector<std::uint32_t> pcs;
    pcs.reserve(group.subscriptions.size());
    for (const StopSubscriptionDefinition& subscription :
         group.subscriptions)
    {
        const auto* pc =
            std::get_if<PcStopPointSpec>(&subscription.point);
        if (pc != nullptr)
            pcs.push_back(pc->pc);
    }
    return pcs;
}

void ExpectPureTranslation(const TranslationAudit& audit)
{
    EXPECT_EQ(audit.phase_script_vm_executions, 0u);
    EXPECT_EQ(audit.physical_backend_calls, 0u);
    EXPECT_EQ(definition_consumer.delivery_count, 0u);
}

TEST(
    LegacyStopPointTranslation,
    MapsCanonicalGatedAndPredicatePcSetsToIndependentSourceScopes)
{
    const BreakpointMap map = bp::BpRegistry::BuildRuntimeMap();
    const std::vector<BPKey> canonical{
        bp::battle::TurnInputs,
        bp::battle::TurnIsReady,
    };
    const std::vector<BPKey> gated{
        bp::battle::StartAction,
        bp::battle::EndAction,
    };
    const std::vector<BPKey> predicates{
        bp::battle::EndTurn,
    };

    const auto lowered =
        LowerLegacyPcCatalog(map, canonical, gated, predicates);

    EXPECT_EQ(lowered.canonical.keys, canonical);
    EXPECT_EQ(lowered.gated.keys, gated);
    EXPECT_EQ(lowered.predicates.keys, predicates);
    EXPECT_NE(
        lowered.canonical.definition.source.id,
        lowered.gated.definition.source.id);
    EXPECT_NE(
        lowered.canonical.definition.source.id,
        lowered.predicates.definition.source.id);
    EXPECT_NE(
        lowered.gated.definition.source.id,
        lowered.predicates.definition.source.id);
    EXPECT_EQ(
        lowered.canonical.definition.source.stable_name,
        "legacy.phase.canonical");
    EXPECT_EQ(
        lowered.gated.definition.source.stable_name,
        "legacy.phase.gated");
    EXPECT_EQ(
        lowered.predicates.definition.source.stable_name,
        "legacy.phase.predicates");

    for (const LogicalPcGroup* group : {
             &lowered.canonical,
             &lowered.gated,
             &lowered.predicates})
    {
        for (const auto& subscription :
             group->definition.subscriptions)
        {
            EXPECT_TRUE(std::holds_alternative<PcStopPointSpec>(
                subscription.point));
            EXPECT_EQ(
                subscription.lifetime,
                StopSubscriptionLifetime::Scoped);
        }
    }
    EXPECT_EQ(lowered.audit.logical_group_registrations, 3u);
    ExpectPureTranslation(lowered.audit);
}

TEST(
    LegacyStopPointTranslation,
    PreservesExpectedAlternativeOrderInOneWakeGroup)
{
    const BreakpointMap map = bp::BpRegistry::BuildRuntimeMap();
    const std::vector<BPKey> expected{
        bp::battle::StartTurn,
        bp::battle::StartAction,
        bp::battle::EndAction,
    };

    const auto lowered = LowerExpectedAlternatives(
        map,
        expected,
        Source(2001, "program.await", "program await"),
        StopSubscriptionGroupId(2101),
        StopSubscriptionId(2201),
        false);

    ASSERT_EQ(lowered.group.definition.subscriptions.size(), 3u);
    EXPECT_EQ(lowered.group.keys, expected);
    EXPECT_EQ(
        PcOrder(lowered.group.definition),
        (std::vector<std::uint32_t>{
            map.find(bp::battle::StartTurn)->pc,
            map.find(bp::battle::StartAction)->pc,
            map.find(bp::battle::EndAction)->pc,
        }));
    EXPECT_EQ(
        std::get<PcStopPointSpec>(
            lowered.group.definition.subscriptions[0].point)
            .pc,
        std::get<PcStopPointSpec>(
            lowered.group.definition.subscriptions[1].point)
            .pc);
    for (const auto& subscription :
         lowered.group.definition.subscriptions)
    {
        EXPECT_EQ(subscription.delivery, StopDeliveryMode::Wake);
        EXPECT_EQ(
            subscription.policy,
            StopRoutingPolicy::Consume);
        EXPECT_EQ(
            subscription.lifetime,
            StopSubscriptionLifetime::Scoped);
    }
    EXPECT_EQ(lowered.audit.logical_group_registrations, 1u);
    ExpectPureTranslation(lowered.audit);
}

TEST(
    LegacyStopPointTranslation,
    LowersExpectedOnlyMacroWaitToNestedSourceScopedWakeGroup)
{
    const BreakpointMap map = bp::BpRegistry::BuildRuntimeMap();
    const BreakpointWaitAction wait{
        .expected_keys = {
            bp::battle::BattleMacroMainMenuMoveHigher,
            bp::battle::BattleMacroMainMenuMoveLower,
        },
        .input = GCInputFrame{},
        .hold_input_through_hit_opcode = true,
    };
    const StopSourceIdentity source = Source(
        3001,
        "interaction.battle-command",
        "battle command interaction");

    const auto lowered = LowerExpectedOnlyMacroWait(
        map,
        wait,
        source,
        StopSubscriptionGroupId(3101),
        StopSubscriptionGroupId(3102),
        StopSubscriptionId(3201));

    EXPECT_EQ(
        lowered.parent_group_id,
        StopSubscriptionGroupId(3101));
    EXPECT_NE(
        lowered.child.group.definition.id,
        lowered.parent_group_id);
    EXPECT_EQ(lowered.child.group.definition.source.id, source.id);
    EXPECT_EQ(
        lowered.child.group.definition.source.stable_name,
        source.stable_name);
    EXPECT_EQ(lowered.child.group.keys, wait.expected_keys);
    ASSERT_EQ(
        lowered.child.group.definition.subscriptions.size(),
        wait.expected_keys.size());
    for (const auto& subscription :
         lowered.child.group.definition.subscriptions)
    {
        EXPECT_EQ(subscription.delivery, StopDeliveryMode::Wake);
        EXPECT_EQ(
            subscription.policy,
            StopRoutingPolicy::Consume);
        EXPECT_TRUE(subscription.suppress_immediate_reentry);
    }
    ExpectPureTranslation(lowered.child.audit);
}

TEST(
    LegacyStopPointTranslation,
    ReplacesDisableAllStepOffWithPerSubscriptionSuppression)
{
    const BreakpointMap map = bp::BpRegistry::BuildRuntimeMap();
    const std::vector<BPKey> expected{
        bp::battle::BattleMacroInputReadyGate,
    };
    const auto step_off = LowerExpectedAlternatives(
        map,
        expected,
        Source(4001, "interaction.step-off", "step-off wait"),
        StopSubscriptionGroupId(4101),
        StopSubscriptionId(4201),
        true);
    const auto unrelated = LowerExpectedAlternatives(
        map,
        expected,
        Source(4002, "other.await", "unrelated wait"),
        StopSubscriptionGroupId(4102),
        StopSubscriptionId(4301),
        false);

    ASSERT_EQ(step_off.group.definition.subscriptions.size(), 1u);
    ASSERT_EQ(unrelated.group.definition.subscriptions.size(), 1u);
    EXPECT_TRUE(
        step_off.group.definition.subscriptions[0]
            .suppress_immediate_reentry);
    EXPECT_FALSE(
        unrelated.group.definition.subscriptions[0]
            .suppress_immediate_reentry);
    EXPECT_NE(
        step_off.group.definition.source.id,
        unrelated.group.definition.source.id);
    EXPECT_EQ(step_off.audit.logical_group_releases, 0u);
    EXPECT_EQ(unrelated.audit.logical_group_releases, 0u);
    ExpectPureTranslation(step_off.audit);
    ExpectPureTranslation(unrelated.audit);
}

TEST(
    LegacyStopPointTranslation,
    MapsMemoryArmAndClearToOneExactScopedGroup)
{
    constexpr std::uint32_t kAddress = 0x803469A8u;
    const PSOp arm = OpArmMemoryWatchpoint(
        77,
        kAddress,
        4,
        PSMemoryWatchpointAccess::Access);
    const PSOp clear = OpClearMemoryWatchpoints();

    const auto lowered = LowerMemoryArmAndClear(
        arm,
        clear,
        Source(5001, "legacy.memory.77", "legacy memory watch 77"),
        StopSubscriptionGroupId(5101),
        StopSubscriptionId(5201));

    ASSERT_TRUE(lowered.has_value());
    EXPECT_EQ(lowered->legacy_watchpoint_id, 77u);
    ASSERT_EQ(lowered->definition.subscriptions.size(), 1u);
    const auto& subscription =
        lowered->definition.subscriptions.front();
    ASSERT_TRUE(std::holds_alternative<MemoryStopPointSpec>(
        subscription.point));
    const auto point =
        std::get<MemoryStopPointSpec>(subscription.point);
    EXPECT_EQ(point.address, kAddress);
    EXPECT_EQ(point.size, 4u);
    EXPECT_EQ(point.access, StopMemoryAccess::Access);
    EXPECT_EQ(
        subscription.lifetime,
        StopSubscriptionLifetime::Scoped);
    EXPECT_EQ(subscription.delivery, StopDeliveryMode::Wake);
    EXPECT_EQ(
        lowered->clear.source_id,
        lowered->definition.source.id);
    EXPECT_EQ(
        lowered->clear.group_id,
        lowered->definition.id);
    EXPECT_EQ(lowered->audit.logical_group_registrations, 1u);
    EXPECT_EQ(lowered->audit.logical_group_releases, 1u);
    ExpectPureTranslation(lowered->audit);
}

TEST(
    LegacyStopPointTranslation,
    MapsCaptureSitesToPassiveObserveAndProgressSubscriptions)
{
    const std::vector<ProbeDefinition> probes{
        ProbeDefinition{
            .id = "capture-control",
            .kind = ProbeKind::Pc,
            .subscriptions =
                Subscription::Capture | Subscription::Control,
            .address = 0x80001000u,
        },
        ProbeDefinition{
            .id = "progress",
            .kind = ProbeKind::Pc,
            .subscriptions = Subscription::Progress,
            .address = 0x80002000u,
        },
        ProbeDefinition{
            .id = "capture-memory",
            .kind = ProbeKind::Memory,
            .subscriptions = Subscription::Capture,
            .address = 0x803469A8u,
            .size = 4,
            .memory_access = MemoryAccess::Write,
        },
    };

    const auto lowered = LowerCaptureSites(
        probes,
        Source(6001, "capture.profile", "capture profile"),
        StopSubscriptionGroupId(6101),
        StopSubscriptionId(6201));

    EXPECT_EQ(
        lowered.probe_ids,
        (std::vector<std::string>{
            "capture-control",
            "progress",
            "capture-memory",
        }));
    ASSERT_EQ(lowered.definition.subscriptions.size(), 3u);
    EXPECT_EQ(
        lowered.definition.subscriptions[0].delivery,
        StopDeliveryMode::Observe);
    EXPECT_EQ(
        lowered.definition.subscriptions[1].delivery,
        StopDeliveryMode::Progress);
    EXPECT_EQ(
        lowered.definition.subscriptions[2].delivery,
        StopDeliveryMode::Observe);
    for (const auto& subscription :
         lowered.definition.subscriptions)
    {
        EXPECT_TRUE(
            subscription.delivery == StopDeliveryMode::Observe ||
            subscription.delivery == StopDeliveryMode::Progress);
        EXPECT_EQ(subscription.policy, StopRoutingPolicy::Pass);
        EXPECT_EQ(
            subscription.lifetime,
            StopSubscriptionLifetime::Scoped);
        EXPECT_NE(subscription.delivery, StopDeliveryMode::Wake);
        EXPECT_NE(subscription.delivery, StopDeliveryMode::Guard);
        EXPECT_NE(
            subscription.delivery,
            StopDeliveryMode::Intercept);
    }
    EXPECT_TRUE(lowered.definition.subscriptions[0].lossless);
    EXPECT_FALSE(lowered.definition.subscriptions[1].lossless);
    EXPECT_TRUE(lowered.definition.subscriptions[2].lossless);
    ExpectPureTranslation(lowered.audit);
}

} // namespace
