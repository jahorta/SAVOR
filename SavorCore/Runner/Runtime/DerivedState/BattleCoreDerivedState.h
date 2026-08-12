#pragma once

#include "DerivedStateTypes.h"

#include <cstdint>
#include <vector>

namespace savor::runtime::derived {

struct BattleItemTotalV1
{
    std::uint16_t item_id = 0;
    std::uint32_t count = 0;

    auto operator<=>(const BattleItemTotalV1&) const = default;
};

struct BattleTurnEntrySnapshotV1
{
    DerivedStateSnapshotProvenanceV1 provenance;
    std::uint32_t current_turn = 0;
    std::vector<BattleItemTotalV1> inventory;

    auto operator<=>(const BattleTurnEntrySnapshotV1&) const = default;
};

struct BattleTurnOrderSnapshotV1
{
    DerivedStateSnapshotProvenanceV1 provenance;
    std::uint32_t current_turn = 0;
    std::vector<std::uint8_t> active_slots;

    auto operator<=>(const BattleTurnOrderSnapshotV1&) const = default;
};

struct BattleRewardsSnapshotV1
{
    DerivedStateSnapshotProvenanceV1 provenance;
    std::uint32_t current_turn = 0;
    std::vector<BattleItemTotalV1> drops;

    auto operator<=>(const BattleRewardsSnapshotV1&) const = default;
};

struct DerivedStateQueryV1
{
    DerivedStateFreshness freshness = DerivedStateFreshness::LatestInItem;
    WorksetEpoch workset_epoch;
    WorkerWorksetItemId item_id;
    RoutedStopIdentity routed_stop;
};

} // namespace savor::runtime::derived
