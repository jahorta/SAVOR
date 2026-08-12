#pragma once

#include "DerivedStateTypes.h"
#include "../ProgramRuntime/Registry/CapabilityPackRegistry.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::runtime::derived {

struct DerivedStateRefreshProviderIdentityV1
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string contract_sha256;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return !canonical_id.empty() && revision != 0 &&
            contract_sha256.size() == 64;
    }

    auto operator<=>(const DerivedStateRefreshProviderIdentityV1&) const = default;
};

struct DerivedStateRefreshGroupDescriptor
{
    std::string group_id;
    std::uint32_t revision = 0;
    std::vector<program::SemanticPointDescriptor> triggers;
    program::SchemaIdentity output_schema;
    program::ExactDependencyIdentity query_action;
    bool accepts_current_point_initialization = false;
    std::uint32_t maximum_guest_reads = 0;
    std::size_t maximum_snapshot_bytes = 0;

    auto operator<=>(const DerivedStateRefreshGroupDescriptor&) const = default;
};

struct DerivedStateBlockDescriptor
{
    DerivedStateBlockIdentityV1 identity;
    DerivedStateRefreshProviderIdentityV1 refresh_provider;
    std::vector<DerivedStateRefreshGroupDescriptor> groups;
    std::vector<program::ExactDependencyIdentity> reducers;
    std::size_t maximum_configuration_bytes = 0;

    auto operator<=>(const DerivedStateBlockDescriptor&) const = default;
};

class DerivedStateRegistry final
{
public:
    [[nodiscard]] bool Register(
        DerivedStateBlockDescriptor descriptor,
        std::string* error_out = nullptr);

    [[nodiscard]] const DerivedStateBlockDescriptor* FindBlock(
        std::string_view canonical_id) const noexcept;

    [[nodiscard]] const DerivedStateBlockDescriptor* FindBlockForQueryAction(
        const program::ExactDependencyIdentity& action) const noexcept;

    [[nodiscard]] const DerivedStateRefreshGroupDescriptor* FindGroup(
        std::string_view block_id,
        std::string_view group_id) const noexcept;

    [[nodiscard]] std::vector<DerivedStateBlockIdentityV1> identities() const;
    [[nodiscard]] std::string canonical_sha256() const;

private:
    std::map<std::string, DerivedStateBlockDescriptor, std::less<>> blocks_;
    std::map<std::string, std::pair<program::ExactDependencyIdentity, std::string>, std::less<>>
        query_blocks_;
};

[[nodiscard]] std::string ComputeDerivedStateBlockDescriptorHashV1(
    const DerivedStateBlockDescriptor& descriptor);

[[nodiscard]] const DerivedStateRegistry& ProductionDerivedStateRegistry();

[[nodiscard]] WorksetDerivedStateBindingV1 ResolveWorksetDerivedStateBindingV1(
    std::span<const std::string> block_ids,
    std::string* error_out = nullptr);

inline constexpr std::string_view kBattleCoreBlockId =
    "soa.derived.battle.core";
inline constexpr std::string_view kBattleCoreRefreshProviderId =
    "soa.derived.battle.core.refresh";
inline constexpr std::string_view kBattleTurnEntryGroupId = "turn_entry";
inline constexpr std::string_view kBattleTurnOrderGroupId = "turn_order";
inline constexpr std::string_view kBattleRewardsGroupId = "rewards";

inline constexpr std::string_view kBattleQueryTurnEntryActionId =
    "soa.battle.derived.query_turn_entry";
inline constexpr std::string_view kBattleQueryTurnOrderActionId =
    "soa.battle.derived.query_turn_order";
inline constexpr std::string_view kBattleQueryRewardsActionId =
    "soa.battle.derived.query_rewards";

} // namespace savor::runtime::derived
