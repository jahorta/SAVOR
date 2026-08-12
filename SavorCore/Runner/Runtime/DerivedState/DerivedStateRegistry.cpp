#include "DerivedStateRegistry.h"

#include "Utils/Hash.h"
#include "../ProgramRuntime/Capabilities/SourceCapabilityPacks.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace savor::runtime::derived {
namespace {

void AppendField(std::string& output, std::string_view value)
{
    output.append(std::to_string(value.size()));
    output.push_back(':');
    output.append(value);
    output.push_back('|');
}

template <typename Value>
void AppendNumber(std::string& output, Value value)
{
    AppendField(output, std::to_string(value));
}

bool Fail(std::string message, std::string* error_out)
{
    if (error_out)
        *error_out = std::move(message);
    return false;
}

DerivedStateBlockDescriptor BattleCoreDescriptor()
{
    using program::capabilities::BattleDerivedReducerIdentity;
    using program::capabilities::BattleDerivedSnapshotSchemaIdentity;
    using program::capabilities::BattleDerivedTurnEntryActionIdentity;
    using program::capabilities::BattleDerivedTurnOrderActionIdentity;
    using program::capabilities::BattleDerivedRewardsActionIdentity;
    const auto snapshot_schema = BattleDerivedSnapshotSchemaIdentity();
    const auto capability_catalog =
        program::capabilities::BuildSourceCapabilityPackCatalog();
    const auto point = [&](std::string_view canonical_id)
        -> program::SemanticPointDescriptor
    {
        std::optional<program::SemanticPointDescriptor> result;
        for (const auto& manifest : capability_catalog.manifests)
        {
            for (const auto& candidate : manifest.semantic_points)
            {
                if (candidate.canonical_id != canonical_id)
                    continue;
                if (result)
                    throw std::logic_error(
                        "Derived-state trigger point is registered more than once");
                result = candidate;
            }
        }
        if (!result)
            throw std::logic_error(
                "Derived-state trigger point is not registered");
        return *result;
    };
    DerivedStateBlockDescriptor descriptor{
        .identity = {
            .canonical_id = std::string(kBattleCoreBlockId),
            .revision = 1,
        },
        .refresh_provider = {
            .canonical_id = std::string(kBattleCoreRefreshProviderId),
            .revision = 1,
            .contract_sha256 = [] {
                constexpr std::string_view contract =
                "savor.derived-state.refresh-provider/v1|"
                "soa.derived.battle.core.refresh|1|"
                "turn-entry:current-turn,main-instance,usable-items-80|"
                "turn-order:current-turn,active-slots-12|"
                "rewards:current-turn,main-instance,item-drops-8";
                return hash::sha256(contract.data(), contract.size());
            }(),
        },
        .groups = {
            {
                .group_id = std::string(kBattleTurnEntryGroupId),
                .revision = 1,
                .triggers = {point("soa.battle.point.TurnInputs")},
                .output_schema = snapshot_schema,
                .query_action = BattleDerivedTurnEntryActionIdentity(),
                .accepts_current_point_initialization = true,
                .maximum_guest_reads = 3,
                .maximum_snapshot_bytes = 2048,
            },
            {
                .group_id = std::string(kBattleTurnOrderGroupId),
                .revision = 1,
                .triggers = {point("soa.battle.point.TurnIsReady")},
                .output_schema = snapshot_schema,
                .query_action = BattleDerivedTurnOrderActionIdentity(),
                .maximum_guest_reads = 2,
                .maximum_snapshot_bytes = 512,
            },
            {
                .group_id = std::string(kBattleRewardsGroupId),
                .revision = 1,
                .triggers = {
                    point("soa.battle.point.EndTurn"),
                    point("soa.battle.point.EndBattleVictory"),
                },
                .output_schema = snapshot_schema,
                .query_action = BattleDerivedRewardsActionIdentity(),
                .maximum_guest_reads = 3,
                .maximum_snapshot_bytes = 512,
            },
        },
        .reducers = {
            BattleDerivedReducerIdentity("soa.battle.derived.current_turn"),
            BattleDerivedReducerIdentity("soa.battle.derived.inventory_count"),
            BattleDerivedReducerIdentity("soa.battle.derived.drop_count"),
            BattleDerivedReducerIdentity("soa.battle.derived.player_count"),
            BattleDerivedReducerIdentity("soa.battle.derived.enemy_count"),
            BattleDerivedReducerIdentity("soa.battle.derived.player_min_position"),
            BattleDerivedReducerIdentity("soa.battle.derived.player_max_position"),
            BattleDerivedReducerIdentity("soa.battle.derived.enemy_min_position"),
            BattleDerivedReducerIdentity("soa.battle.derived.enemy_max_position"),
        },
        .maximum_configuration_bytes = 0,
    };
    descriptor.identity.descriptor_sha256 =
        ComputeDerivedStateBlockDescriptorHashV1(descriptor);
    return descriptor;
}

} // namespace

std::string ComputeDerivedStateBlockDescriptorHashV1(
    const DerivedStateBlockDescriptor& descriptor)
{
    std::string canonical;
    AppendField(canonical, "savor.derived-state.block-descriptor/v1");
    AppendField(canonical, descriptor.identity.canonical_id);
    AppendNumber(canonical, descriptor.identity.revision);
    AppendField(canonical, descriptor.refresh_provider.canonical_id);
    AppendNumber(canonical, descriptor.refresh_provider.revision);
    AppendField(canonical, descriptor.refresh_provider.contract_sha256);
    AppendNumber(canonical, descriptor.maximum_configuration_bytes);
    AppendNumber(canonical, descriptor.groups.size());
    for (const auto& group : descriptor.groups)
    {
        AppendField(canonical, group.group_id);
        AppendNumber(canonical, group.revision);
        AppendField(canonical, group.output_schema.canonical_id);
        AppendNumber(canonical, group.output_schema.version);
        AppendField(canonical, group.output_schema.schema_hash.ToHex());
        AppendField(canonical, group.query_action.canonical_id);
        AppendNumber(canonical, group.query_action.version);
        AppendField(canonical, group.query_action.signature_hash.ToHex());
        AppendNumber(
            canonical,
            group.accepts_current_point_initialization ? 1u : 0u);
        AppendNumber(canonical, group.maximum_guest_reads);
        AppendNumber(canonical, group.maximum_snapshot_bytes);
        AppendNumber(canonical, group.triggers.size());
        for (const auto& trigger : group.triggers)
        {
            AppendField(canonical, trigger.canonical_id);
            AppendNumber(canonical, static_cast<std::uint8_t>(trigger.kind));
            AppendNumber(canonical, trigger.pc);
            AppendNumber(canonical, trigger.memory_address);
            AppendNumber(canonical, trigger.memory_size);
            AppendNumber(canonical, trigger.memory_read ? 1 : 0);
            AppendNumber(canonical, trigger.memory_write ? 1 : 0);
            AppendField(canonical, trigger.synthetic_identity);
        }
    }
    AppendNumber(canonical, descriptor.reducers.size());
    for (const auto& reducer : descriptor.reducers)
    {
        AppendField(canonical, reducer.canonical_id);
        AppendNumber(canonical, reducer.version);
        AppendField(canonical, reducer.signature_hash.ToHex());
    }
    return hash::sha256(canonical.data(), canonical.size());
}

bool DerivedStateRegistry::Register(
    DerivedStateBlockDescriptor descriptor,
    std::string* error_out)
{
    if (!descriptor.identity || !descriptor.refresh_provider ||
        descriptor.groups.empty() ||
        ComputeDerivedStateBlockDescriptorHashV1(descriptor) !=
            descriptor.identity.descriptor_sha256)
    {
        return Fail("Derived-state block descriptor identity is invalid", error_out);
    }
    if (blocks_.contains(descriptor.identity.canonical_id))
        return Fail("Derived-state block identity is duplicated", error_out);

    std::set<std::string> groups;
    std::set<std::string> semantic_points;
    std::set<std::uint32_t> trigger_pcs;
    std::set<std::string> actions;
    for (const auto& group : descriptor.groups)
    {
        if (group.group_id.empty() || group.revision == 0 ||
            group.triggers.empty() ||
            group.output_schema.canonical_id.empty() ||
            group.output_schema.version == 0 ||
            group.output_schema.schema_hash.empty() ||
            group.query_action.canonical_id.empty() ||
            group.query_action.version == 0 ||
            group.query_action.signature_hash.empty() ||
            group.maximum_guest_reads == 0 || group.maximum_snapshot_bytes == 0 ||
            !groups.insert(group.group_id).second ||
            !actions.insert(group.query_action.canonical_id).second ||
            query_blocks_.contains(group.query_action.canonical_id))
        {
            return Fail("Derived-state refresh-group descriptor is invalid", error_out);
        }
        for (const auto& point : group.triggers)
        {
            const bool valid_pc =
                point.kind == program::SemanticPointKind::ProgramCounter &&
                point.pc != 0 && point.memory_address == 0 &&
                point.memory_size == 0 && !point.memory_read &&
                !point.memory_write && point.synthetic_identity.empty();
            if (point.canonical_id.empty() || !valid_pc ||
                !semantic_points.insert(point.canonical_id).second)
            {
                return Fail(
                    "Derived-state semantic trigger is empty or duplicated",
                    error_out);
            }
        }
        for (const auto& point : group.triggers)
        {
            if (!trigger_pcs.insert(point.pc).second)
                return Fail("Derived-state trigger PC is invalid or duplicated", error_out);
        }
    }

    std::set<std::string> reducers;
    for (const auto& reducer : descriptor.reducers)
    {
        if (reducer.canonical_id.empty() || reducer.version == 0 ||
            reducer.signature_hash.empty() ||
            !reducers.insert(reducer.canonical_id).second)
        {
            return Fail(
                "Derived-state reducer identity is empty or duplicated",
                error_out);
        }
    }

    const std::string block_id = descriptor.identity.canonical_id;
    for (const auto& group : descriptor.groups)
    {
        query_blocks_.emplace(
            group.query_action.canonical_id,
            std::pair{group.query_action, block_id});
    }
    blocks_.emplace(block_id, std::move(descriptor));
    return true;
}

const DerivedStateBlockDescriptor* DerivedStateRegistry::FindBlock(
    std::string_view canonical_id) const noexcept
{
    const auto it = blocks_.find(canonical_id);
    return it == blocks_.end() ? nullptr : &it->second;
}

const DerivedStateBlockDescriptor* DerivedStateRegistry::FindBlockForQueryAction(
    const program::ExactDependencyIdentity& action) const noexcept
{
    const auto query = query_blocks_.find(action.canonical_id);
    return query == query_blocks_.end() || query->second.first != action
        ? nullptr
        : FindBlock(query->second.second);
}

const DerivedStateRefreshGroupDescriptor* DerivedStateRegistry::FindGroup(
    std::string_view block_id,
    std::string_view group_id) const noexcept
{
    const auto* block = FindBlock(block_id);
    if (!block)
        return nullptr;
    const auto it = std::ranges::find(
        block->groups, group_id, &DerivedStateRefreshGroupDescriptor::group_id);
    return it == block->groups.end() ? nullptr : &*it;
}

std::vector<DerivedStateBlockIdentityV1> DerivedStateRegistry::identities() const
{
    std::vector<DerivedStateBlockIdentityV1> result;
    result.reserve(blocks_.size());
    for (const auto& [_, block] : blocks_)
        result.push_back(block.identity);
    return result;
}

std::string DerivedStateRegistry::canonical_sha256() const
{
    std::string canonical;
    AppendField(canonical, "savor.derived-state.registry/v1");
    for (const auto& [_, block] : blocks_)
        AppendField(canonical, block.identity.descriptor_sha256);
    return hash::sha256(canonical.data(), canonical.size());
}

const DerivedStateRegistry& ProductionDerivedStateRegistry()
{
    static const DerivedStateRegistry registry = []
    {
        DerivedStateRegistry result;
        std::string error;
        if (!result.Register(BattleCoreDescriptor(), &error))
            std::terminate();
        return result;
    }();
    return registry;
}

WorksetDerivedStateBindingV1 ResolveWorksetDerivedStateBindingV1(
    std::span<const std::string> block_ids,
    std::string* error_out)
{
    WorksetDerivedStateBindingV1 result;
    result.blocks.clear();
    std::vector<std::string> sorted(block_ids.begin(), block_ids.end());
    std::ranges::sort(sorted);
    if (std::ranges::adjacent_find(sorted) != sorted.end())
    {
        if (error_out)
            *error_out = "Derived-state block request contains a duplicate ID";
        return {};
    }
    for (const auto& id : sorted)
    {
        const auto* descriptor = ProductionDerivedStateRegistry().FindBlock(id);
        if (!descriptor)
        {
            if (error_out)
                *error_out = "Derived-state block request names an unknown block: " + id;
            return {};
        }
        DerivedStateBlockBindingV1 block{
            .identity = descriptor->identity,
            .configuration = {},
        };
        block.configuration_sha256 = hash::sha256(nullptr, 0);
        block.content_sha256 = ComputeDerivedStateBlockBindingHashV1(block);
        result.blocks.push_back(std::move(block));
    }
    result.content_sha256 = ComputeWorksetDerivedStateBindingHashV1(result);
    return result;
}

} // namespace savor::runtime::derived
