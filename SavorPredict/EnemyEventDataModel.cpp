#include "EnemyEventDataModel.h"

#include <algorithm>

namespace savor::predict {
namespace {

EnemyEventStartPositionSet first_battle_positions() {
    return EnemyEventStartPositionSet{
        .enemy_event_id = 0,
        .positions = {
            BattleStartPosition{.slot = 0, .present = true, .is_player = true, .combatant_id = 0, .combatant_name = "Vyse", .grid_x = 4, .grid_z = 6},
            BattleStartPosition{.slot = 1, .present = true, .is_player = true, .combatant_id = 1, .combatant_name = "Aika", .grid_x = 6, .grid_z = 6},
            BattleStartPosition{.slot = 2, .present = false, .is_player = true, .combatant_id = -1, .combatant_name = "None", .grid_x = -1, .grid_z = -1},
            BattleStartPosition{.slot = 3, .present = false, .is_player = true, .combatant_id = -1, .combatant_name = "None", .grid_x = -1, .grid_z = -1},
            BattleStartPosition{.slot = 4, .present = true, .is_player = false, .combatant_id = 0, .combatant_name = "Soldier", .grid_x = 4, .grid_z = 2},
            BattleStartPosition{.slot = 5, .present = true, .is_player = false, .combatant_id = 0, .combatant_name = "Soldier", .grid_x = 6, .grid_z = 2},
            BattleStartPosition{.slot = 6, .present = false, .is_player = false, .combatant_id = 255, .combatant_name = "None", .grid_x = -1, .grid_z = -1},
            BattleStartPosition{.slot = 7, .present = false, .is_player = false, .combatant_id = 255, .combatant_name = "None", .grid_x = -1, .grid_z = -1},
            BattleStartPosition{.slot = 8, .present = false, .is_player = false, .combatant_id = 255, .combatant_name = "None", .grid_x = -1, .grid_z = -1},
            BattleStartPosition{.slot = 9, .present = false, .is_player = false, .combatant_id = 255, .combatant_name = "None", .grid_x = -1, .grid_z = -1},
            BattleStartPosition{.slot = 10, .present = false, .is_player = false, .combatant_id = 255, .combatant_name = "None", .grid_x = -1, .grid_z = -1},
        },
    };
}

EnemyEventStartPositionSet second_battle_positions() {
    return EnemyEventStartPositionSet{
        .enemy_event_id = 1,
        .positions = {
            BattleStartPosition{.slot = 0, .present = true, .is_player = true, .combatant_id = 0, .combatant_name = "Vyse", .grid_x = 4, .grid_z = 8},
            BattleStartPosition{.slot = 1, .present = true, .is_player = true, .combatant_id = 1, .combatant_name = "Aika", .grid_x = 6, .grid_z = 8},
            BattleStartPosition{.slot = 2, .present = false, .is_player = true, .combatant_id = -1, .combatant_name = "None", .grid_x = -1, .grid_z = -1},
            BattleStartPosition{.slot = 3, .present = false, .is_player = true, .combatant_id = -1, .combatant_name = "None", .grid_x = -1, .grid_z = -1},
            BattleStartPosition{.slot = 4, .present = true, .is_player = false, .combatant_id = 1, .combatant_name = "Guard", .grid_x = 4, .grid_z = 2},
            BattleStartPosition{.slot = 5, .present = true, .is_player = false, .combatant_id = 1, .combatant_name = "Guard", .grid_x = 6, .grid_z = 2},
            BattleStartPosition{.slot = 6, .present = true, .is_player = false, .combatant_id = 1, .combatant_name = "Guard", .grid_x = 2, .grid_z = 3},
            BattleStartPosition{.slot = 7, .present = true, .is_player = false, .combatant_id = 1, .combatant_name = "Guard", .grid_x = 8, .grid_z = 3},
            BattleStartPosition{.slot = 8, .present = false, .is_player = false, .combatant_id = 255, .combatant_name = "None", .grid_x = -1, .grid_z = -1},
            BattleStartPosition{.slot = 9, .present = false, .is_player = false, .combatant_id = 255, .combatant_name = "None", .grid_x = -1, .grid_z = -1},
            BattleStartPosition{.slot = 10, .present = false, .is_player = false, .combatant_id = 255, .combatant_name = "None", .grid_x = -1, .grid_z = -1},
        },
    };
}

} // namespace

std::optional<EnemyEventStartPositionSet> enemy_event_start_positions(int enemy_event_id) {
    switch (enemy_event_id) {
    case 0:
        return first_battle_positions();
    case 1:
        return second_battle_positions();
    default:
        return std::nullopt;
    }
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
