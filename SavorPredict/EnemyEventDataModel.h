#pragma once

#include <optional>
#include <vector>

namespace savor::predict {

struct BattleStartPosition {
    int slot = -1;
    bool present = false;
    bool is_player = false;
    int combatant_id = -1;
    const char* combatant_name = "None";
    // ALX enemyevent.csv stores grid-space coordinates only. Movement worksheets
    // also carry raw stage-unit position fields, which are modeled separately.
    int grid_x = -1;
    int grid_z = -1;
};

struct EnemyEventStartPositionSet {
    int enemy_event_id = -1;
    const char* source = "ALX-5.0.0 enemyevent.csv";
    std::vector<BattleStartPosition> positions;
};

std::optional<EnemyEventStartPositionSet> enemy_event_start_positions(int enemy_event_id);
std::optional<BattleStartPosition> enemy_event_start_position_for_slot(
    int enemy_event_id,
    int slot);

} // namespace savor::predict
