#include "EnemyEventDataModel.h"

#include "BattleSourceModel.h"

#include <algorithm>

namespace savor::predict {
namespace {

std::optional<EnemyEventStartPositionSet> canonical_first_battle_positions() {
    const auto bundle = load_battle_source_bundle(
        "first-battle-soldiers-us-final");
    if (!bundle.ok || bundle.snapshot.encounter_id < 0) {
        return std::nullopt;
    }

    EnemyEventStartPositionSet result;
    result.enemy_event_id = bundle.snapshot.encounter_id;
    result.source = "hash-validated BattleSourceSnapshot";
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
    const auto first_battle = canonical_first_battle_positions();
    if (!first_battle.has_value()
        || first_battle->enemy_event_id != enemy_event_id) {
        return std::nullopt;
    }
    return first_battle;
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
