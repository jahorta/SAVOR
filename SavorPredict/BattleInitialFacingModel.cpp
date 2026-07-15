#include "BattleInitialFacingModel.h"

#include "BattleInitialTurnTypeModel.h"

#include <sstream>

namespace savor::predict {

BattleInitialFacingResult model_initial_battle_facing(
    const BattleInitialFacingInput& input) {
    BattleInitialFacingResult result;
    std::ostringstream detail;
    detail << "slot=" << input.slot
        << "; present=" << (input.present ? 1 : 0)
        << "; side=" << (input.is_player ? "player" : "enemy");

    if (!input.present) {
        result.status = BattleInitialFacingStatus::Skipped;
        result.provenance = "absent_combatant_has_no_formation_publication";
        result.detail = detail.str();
        return result;
    }
    if (!input.turn_type.has_value()) {
        result.status = BattleInitialFacingStatus::MissingInput;
        result.provenance = "missing_initial_turn_type";
        result.detail = detail.str();
        return result;
    }

    detail << "; turn_type=" << battle_turn_type_name(*input.turn_type);
    switch (*input.turn_type) {
    case soa::battle::TurnType::BackAttack:
        result.facing_angle_0x2c = 0x00000000u;
        break;
    case soa::battle::TurnType::Normal:
        result.facing_angle_0x2c = input.is_player ? 0x00008000u : 0x00000000u;
        break;
    case soa::battle::TurnType::Advantage:
        result.facing_angle_0x2c = 0x00008000u;
        break;
    default:
        result.status = BattleInitialFacingStatus::MissingInput;
        result.provenance = "invalid_initial_turn_type";
        result.detail = detail.str();
        return result;
    }

    result.status = BattleInitialFacingStatus::Exact;
    result.provenance =
        "placeCombatantsOnGrid_80084570 side-and-turn-type formation angle";
    detail << "; facing_angle_0x2c="
        << (*result.facing_angle_0x2c == 0x8000u ? "0x00008000" : "0x00000000");
    result.detail = detail.str();
    return result;
}

const char* battle_initial_facing_status_name(BattleInitialFacingStatus status) {
    switch (status) {
    case BattleInitialFacingStatus::Exact: return "Exact";
    case BattleInitialFacingStatus::Skipped: return "Skipped";
    case BattleInitialFacingStatus::MissingInput: return "MissingInput";
    }
    return "MissingInput";
}

} // namespace savor::predict
