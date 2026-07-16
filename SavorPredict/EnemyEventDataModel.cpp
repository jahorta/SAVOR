#include "EnemyEventDataModel.h"

#include "BattleSourceModel.h"

#include <algorithm>
#include <iterator>

namespace savor::predict {
namespace {

bool same_placement_set(
    const BattleSourceSnapshot& lhs,
    const BattleSourceSnapshot& rhs) {
    if (lhs.placements.size() != rhs.placements.size()) {
        return false;
    }
    for (const auto& placement : lhs.placements) {
        const auto other = battle_source_placement_for_slot(rhs, placement.slot);
        if (!other.has_value()
            || other->is_player != placement.is_player
            || other->combatant_id != placement.combatant_id
            || other->grid_x != placement.grid_x
            || other->grid_z != placement.grid_z
            || other->status != placement.status) {
            return false;
        }
    }
    return true;
}

std::optional<EnemyEventStartPositionSet> canonical_event_positions(
    int enemy_event_id) {
    const auto bundles = load_battle_source_bundles_for_encounter({
        .source_kind = BattleEncounterSourceKind::EventDefinition,
        .encounter_id = enemy_event_id,
    });
    if (bundles.empty()) {
        return std::nullopt;
    }
    const auto& bundle = bundles.front();
    if (std::any_of(
            std::next(bundles.begin()), bundles.end(),
            [&](const BattleSourceBundleLoadResult& candidate) {
                return !same_placement_set(bundle.snapshot, candidate.snapshot);
            })) {
        return std::nullopt;
    }

    EnemyEventStartPositionSet result;
    result.enemy_event_id = bundle.snapshot.encounter_id;
    result.source = "EnemyEvent-identity BattleSourceSnapshot";
    result.positions.reserve(bundle.snapshot.placements.size());
    for (const auto& placement : bundle.snapshot.placements) {
        if (placement.status != BattleSourceFieldStatus::Exact) {
            return std::nullopt;
        }
        result.positions.push_back(BattleStartPosition{
            .slot = placement.slot,
            .present = true,
            .is_player = placement.is_player,
            .combatant_id = placement.combatant_id,
            .combatant_name = placement.combatant_name,
            .grid_x = placement.grid_x,
            .grid_z = placement.grid_z,
        });
    }
    return result;
}

} // namespace

std::optional<EnemyEventStartPositionSet> enemy_event_start_positions(
    int enemy_event_id) {
    return canonical_event_positions(enemy_event_id);
}

std::optional<BattleStartPosition> enemy_event_start_position_for_slot(
    int enemy_event_id,
    int slot) {
    const auto positions = enemy_event_start_positions(enemy_event_id);
    if (!positions.has_value()) {
        return std::nullopt;
    }
    const auto it = std::find_if(
        positions->positions.begin(),
        positions->positions.end(),
        [slot](const BattleStartPosition& position) {
            return position.slot == slot;
        });
    if (it == positions->positions.end()) {
        return std::nullopt;
    }
    return *it;
}

} // namespace savor::predict
