#include "MovementModel.h"

#include "BattleFrameStateModel.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace savor::predict {
namespace {

const MovementSlotState* find_slot(const std::vector<MovementSlotState>& slots, int slot) {
    const auto it = std::find_if(
        slots.begin(),
        slots.end(),
        [slot](const MovementSlotState& state) {
            return state.slot == slot;
        });
    return it == slots.end() ? nullptr : &*it;
}

MovementRawStagePosition to_movement_raw_stage_position(const BattleFrameVec3& position) {
    return MovementRawStagePosition{
        .raw_x = static_cast<int>(position.x),
        .raw_y = static_cast<int>(position.y),
        .raw_z = static_cast<int>(position.z),
    };
}

bool target_valid_for_action(const std::vector<MovementSlotState>& slots, int target_slot, bool enemy_owned) {
    const auto* target = find_slot(slots, target_slot);
    if (target == nullptr || !target->present || !target->alive) {
        return false;
    }
    return enemy_owned ? target->is_player : !target->is_player;
}

std::vector<int> living_target_candidates(const std::vector<MovementSlotState>& slots, bool enemy_owned) {
    std::vector<int> result;
    for (const auto& slot : slots) {
        if (!slot.present || !slot.alive) {
            continue;
        }
        if (enemy_owned ? slot.is_player : !slot.is_player) {
            result.push_back(slot.slot);
        }
    }
    return result;
}

MovementReachabilityStatus reachability_from_return(int value) {
    switch (value) {
    case 0:
        return MovementReachabilityStatus::Failed0;
    case 1:
        return MovementReachabilityStatus::Adjacent1;
    case 2:
        return MovementReachabilityStatus::AdjustedAdjacent2;
    case 4:
        return MovementReachabilityStatus::Path4;
    default:
        return MovementReachabilityStatus::Ambiguous;
    }
}

bool reachability_allows_direct(MovementReachabilityStatus status) {
    return status == MovementReachabilityStatus::Adjacent1
        || status == MovementReachabilityStatus::AdjustedAdjacent2
        || status == MovementReachabilityStatus::Path4;
}

bool worksheet_proves_direct(const MovementWorksheetSnapshot& worksheet, MovementReachabilityStatus* reachability) {
    if (!worksheet.available) {
        *reachability = MovementReachabilityStatus::Ambiguous;
        return false;
    }
    if (!worksheet.reachability_result.has_value()
        || !worksheet.path_shape_forces_fallback.has_value()
        || !worksheet.dist_to_target.has_value()) {
        *reachability = MovementReachabilityStatus::Ambiguous;
        return false;
    }
    *reachability = reachability_from_return(*worksheet.reachability_result);
    return reachability_allows_direct(*reachability)
        && !*worksheet.path_shape_forces_fallback
        && *worksheet.dist_to_target <= 4;
}

bool worksheet_proves_fallback(const MovementWorksheetSnapshot& worksheet, MovementReachabilityStatus* reachability) {
    if (!worksheet.available) {
        *reachability = MovementReachabilityStatus::Ambiguous;
        return false;
    }
    if (!worksheet.reachability_result.has_value()
        || !worksheet.path_shape_forces_fallback.has_value()
        || !worksheet.dist_to_target.has_value()) {
        *reachability = MovementReachabilityStatus::Ambiguous;
        return false;
    }
    *reachability = reachability_from_return(*worksheet.reachability_result);
    return *worksheet.path_shape_forces_fallback || !reachability_allows_direct(*reachability);
}

bool worksheet_proves_adjacent(const MovementWorksheetSnapshot& worksheet) {
    return worksheet.available && worksheet.target_adjacent.has_value() && *worksheet.target_adjacent;
}

bool worksheet_proves_not_adjacent(const MovementWorksheetSnapshot& worksheet) {
    return worksheet.available && worksheet.target_adjacent.has_value() && !*worksheet.target_adjacent;
}

bool worksheet_proves_enemy_direct_close(const MovementWorksheetSnapshot& worksheet, MovementReachabilityStatus* reachability) {
    if (!worksheet.available
        || !worksheet.helper_8008a174_result.has_value()
        || !worksheet.helper_80082340_result.has_value()
        || !worksheet.dist_to_target.has_value()) {
        *reachability = MovementReachabilityStatus::Ambiguous;
        return false;
    }
    if (*worksheet.helper_8008a174_result != 0
        && *worksheet.helper_80082340_result == 0
        && *worksheet.dist_to_target < 5) {
        *reachability = MovementReachabilityStatus::Path4;
        return true;
    }
    *reachability = reachability_from_return(0);
    return false;
}

void mark_missing_input(MovementSimulation* result, const std::string& reason) {
    result->status = MovementSimulationStatus::MissingInput;
    result->reachability = MovementReachabilityStatus::Ambiguous;
    if (!result->detail.empty()) {
        result->detail += "; ";
    }
    result->detail += reason;
}

void repair_target(const MovementModelInputs& inputs, MovementSimulation* result) {
    if (target_valid_for_action(inputs.slots, result->final_target_slot, inputs.enemy_owned)) {
        return;
    }

    const auto candidates = living_target_candidates(inputs.slots, inputs.enemy_owned);
    if (candidates.empty()) {
        result->can_execute = false;
        result->target_repair_status = MovementSimulationStatus::Skipped;
        result->status = MovementSimulationStatus::Skipped;
        result->detail = "queued target is invalid and no living replacement target exists";
        return;
    }

    if (candidates.size() == 1) {
        result->target_repaired = true;
        result->final_target_slot = candidates.front();
        result->target_repair_status = MovementSimulationStatus::Exact;
        return;
    }

    result->can_execute = false;
    result->target_repair_status = MovementSimulationStatus::Ambiguous;
    result->status = MovementSimulationStatus::Ambiguous;
    result->detail = "multi-candidate target repair requires pathing and qsort ordering";
}

void simulate_pc_attack(const MovementModelInputs& inputs, MovementSimulation* result) {
    repair_target(inputs, result);
    if (!result->can_execute) {
        return;
    }

    int final_param = result->initial_instr_param_0x6;

    if (final_param == 0) {
        MovementReachabilityStatus reachability = MovementReachabilityStatus::Unknown;
        if (worksheet_proves_direct(inputs.actor_worksheet, &reachability)) {
            result->reachability = reachability;
        } else if (worksheet_proves_fallback(inputs.actor_worksheet, &reachability)) {
            final_param = 1;
            result->reachability = reachability;
        } else {
            mark_missing_input(result, "PC direct/fallback setup needs FUN_80083728, FUN_80082340, and dist_to_target_0x14");
        }
    } else {
        if (worksheet_proves_adjacent(inputs.actor_worksheet)) {
            final_param = 0;
            result->reachability = MovementReachabilityStatus::Adjacent1;
        } else if (worksheet_proves_not_adjacent(inputs.actor_worksheet)) {
            result->reachability = MovementReachabilityStatus::Failed0;
        } else {
            mark_missing_input(result, "PC fallback reset gate needs FUN_8008571c and checkTargetAdjacent");
        }
    }

    result->final_instr_param_0x6 = final_param;
    result->selected_worker = final_param == 0
        ? MovementSelectedWorker::PcDirectAttack_80086308
        : MovementSelectedWorker::PcFallbackAttack_80085ce0;
}

void simulate_enemy_attack(const MovementModelInputs& inputs, MovementSimulation* result) {
    const auto* actor = find_slot(inputs.slots, inputs.actor_slot);
    const int movement_flags = actor == nullptr ? 0 : actor->movement_flags;
    const auto setup = simulate_enemy_attack_setup_gate(
        inputs.rng_state,
        EnemyAttackSetupInputs{
            .queued_instruction = inputs.queued_instruction,
            .movement_flags = movement_flags,
        });
    result->end_state = setup.end_state;
    result->draws_consumed = setup.draws_consumed;
    result->setup_rand = setup.setup_rand;
    result->setup_rand_mod10 = setup.setup_rand_mod10;
    result->enemy_setup_path = setup.path;
    result->enemy_direct_close_candidate = setup.direct_close_branch_candidate;

    repair_target(inputs, result);
    if (!result->can_execute) {
        return;
    }

    int final_param = result->initial_instr_param_0x6;
    if (setup.path == EnemyAttackSetupPath::DirectCloseSetupCandidate
        || setup.path == EnemyAttackSetupPath::NoRandomSetupDraw) {
        final_param = 0;
    }

    MovementReachabilityStatus direct_close_reachability = MovementReachabilityStatus::Unknown;
    if (setup.path == EnemyAttackSetupPath::DirectCloseSetupCandidate
        && worksheet_proves_enemy_direct_close(inputs.actor_worksheet, &direct_close_reachability)) {
        final_param = 0;
        result->reachability = direct_close_reachability;
    } else if (worksheet_proves_adjacent(inputs.actor_worksheet)) {
        final_param = 0;
        result->reachability = MovementReachabilityStatus::Adjacent1;
    } else if (worksheet_proves_not_adjacent(inputs.actor_worksheet)) {
        final_param = 1;
        result->reachability = MovementReachabilityStatus::Failed0;
    } else {
        mark_missing_input(result, "enemy direct/fallback setup needs FUN_8008a174/FUN_8008a280 path and checkTargetAdjacent");
    }

    result->final_instr_param_0x6 = final_param;
    result->selected_worker = final_param == 0
        ? MovementSelectedWorker::EnemyDirectAttack_80087f6c
        : MovementSelectedWorker::EnemyFallbackAttack_80087844;
}

PassiveMovementRouteKind classify_passive_route(
    const MovementModelInputs& inputs,
    const MovementSlotState& slot,
    int final_target_slot) {
    if (!slot.present || !slot.alive || slot.slot == inputs.actor_slot) {
        return PassiveMovementRouteKind::Unaffected;
    }
    if (slot.slot == final_target_slot) {
        return PassiveMovementRouteKind::TargetParticipant;
    }
    if (slot.is_player == inputs.enemy_owned) {
        return PassiveMovementRouteKind::SameSideParticipant;
    }
    return PassiveMovementRouteKind::Unaffected;
}

MovementSelectedWorker passive_worker_for_route(PassiveMovementRouteKind route) {
    switch (route) {
    case PassiveMovementRouteKind::TargetParticipant:
    case PassiveMovementRouteKind::SameSideParticipant:
    case PassiveMovementRouteKind::SpecialParticipant:
    case PassiveMovementRouteKind::Unaffected:
        return MovementSelectedWorker::None;
    }
    return MovementSelectedWorker::None;
}

void append_passive_routes(const MovementModelInputs& inputs, MovementSimulation* result) {
    for (const auto& slot : inputs.slots) {
        if (!slot.present || !slot.alive || slot.slot == inputs.actor_slot) {
            continue;
        }
        const auto route = classify_passive_route(inputs, slot, result->final_target_slot);
        result->passive_routes.push_back(PassiveMovementRoute{
            .slot = slot.slot,
            .route = route,
            .selected_worker = passive_worker_for_route(route),
            .status = MovementSimulationStatus::Provisional,
        });
    }
}

} // namespace

MovementSimulation simulate_first_battle_movement_setup(const MovementModelInputs& inputs) {
    MovementSimulation result;
    result.end_state = inputs.rng_state;
    result.original_target_slot = inputs.target_slot;
    result.final_target_slot = inputs.target_slot;
    result.initial_instr_param_0x6 = inputs.instr_param_0x6;
    result.final_instr_param_0x6 = inputs.instr_param_0x6;

    if (inputs.backend != MovementBackend::HandlerLevelFirstBattle
        && inputs.backend != MovementBackend::FrameStateMachine) {
        result.status = MovementSimulationStatus::Unsupported;
        result.can_execute = false;
        result.detail = "movement backend is not implemented";
        return result;
    }
    if (inputs.queued_instruction != 3) {
        result.status = MovementSimulationStatus::Unsupported;
        result.can_execute = false;
        result.detail = "MovementModel v1 supports basic attack only";
        return result;
    }

    const auto* actor = find_slot(inputs.slots, inputs.actor_slot);
    if (actor == nullptr || !actor->present || !actor->alive) {
        result.status = MovementSimulationStatus::Skipped;
        result.can_execute = false;
        result.detail = "actor is not present and alive";
        return result;
    }

    if (inputs.enemy_owned) {
        simulate_enemy_attack(inputs, &result);
    } else {
        simulate_pc_attack(inputs, &result);
    }

    append_passive_routes(inputs, &result);
    return result;
}

MovementWorksheetSnapshot project_enemy_event0_movement_worksheet_snapshot(const MovementModelInputs& inputs) {
    MovementWorksheetSnapshot snapshot;
    const auto* actor = find_slot(inputs.slots, inputs.actor_slot);
    const auto* target = find_slot(inputs.slots, inputs.target_slot);
    if (actor == nullptr || target == nullptr
        || !actor->start_position.has_value()
        || !target->start_position.has_value()) {
        return snapshot;
    }

    const auto& actor_pos = *actor->start_position;
    const auto& target_pos = *target->start_position;
    if (!actor_pos.present || !target_pos.present
        || actor_pos.grid_x < 0 || actor_pos.grid_z < 0
        || target_pos.grid_x < 0 || target_pos.grid_z < 0) {
        return snapshot;
    }

    const int dx = std::abs(actor_pos.grid_x - target_pos.grid_x);
    const int dz = std::abs(actor_pos.grid_z - target_pos.grid_z);
    const bool same_axis = dx == 0 || dz == 0;
    const int axis_distance = same_axis ? std::max(dx, dz) : dx + dz;
    const bool direct_path = same_axis && axis_distance <= 4;

    snapshot.available = true;
    snapshot.actor_grid_position = MovementGridPosition{
        .grid_x = actor_pos.grid_x,
        .grid_z = actor_pos.grid_z,
    };
    snapshot.target_grid_position = MovementGridPosition{
        .grid_x = target_pos.grid_x,
        .grid_z = target_pos.grid_z,
    };
    snapshot.actor_raw_stage_position = to_movement_raw_stage_position(first_battle_grid_to_raw_stage_position(
        *snapshot.actor_grid_position,
        actor->width,
        actor->depth));
    snapshot.target_raw_stage_position = to_movement_raw_stage_position(first_battle_grid_to_raw_stage_position(
        *snapshot.target_grid_position,
        target->width,
        target->depth));
    snapshot.target_adjacent = dx + dz <= 1;
    snapshot.reachability_result = direct_path
        ? (axis_distance <= 1 ? 1 : 4)
        : 0;
    snapshot.path_shape_forces_fallback = !direct_path;
    snapshot.dist_to_target = axis_distance;
    snapshot.helper_8008a174_result = 1;
    snapshot.helper_80082340_result = direct_path ? 0 : 1;
    snapshot.source = inputs.backend == MovementBackend::FrameStateMachine
        ? "enemy_event_0_frame_state_projection"
        : "enemy_event_0_alx_grid_projection";
    return snapshot;
}

const char* movement_simulation_status_name(MovementSimulationStatus status) {
    switch (status) {
    case MovementSimulationStatus::Exact:
        return "Exact";
    case MovementSimulationStatus::Provisional:
        return "Provisional";
    case MovementSimulationStatus::Skipped:
        return "Skipped";
    case MovementSimulationStatus::MissingInput:
        return "MissingInput";
    case MovementSimulationStatus::Unsupported:
        return "Unsupported";
    case MovementSimulationStatus::Ambiguous:
        return "Ambiguous";
    }
    return "Unsupported";
}

const char* movement_selected_worker_name(MovementSelectedWorker worker) {
    switch (worker) {
    case MovementSelectedWorker::None:
        return "None";
    case MovementSelectedWorker::PcDirectAttack_80086308:
        return "PcDirectAttack_80086308";
    case MovementSelectedWorker::PcFallbackAttack_80085ce0:
        return "PcFallbackAttack_80085ce0";
    case MovementSelectedWorker::EnemyDirectAttack_80087f6c:
        return "EnemyDirectAttack_80087f6c";
    case MovementSelectedWorker::EnemyFallbackAttack_80087844:
        return "EnemyFallbackAttack_80087844";
    }
    return "None";
}

const char* movement_reachability_status_name(MovementReachabilityStatus status) {
    switch (status) {
    case MovementReachabilityStatus::Unknown:
        return "Unknown";
    case MovementReachabilityStatus::Failed0:
        return "Failed0";
    case MovementReachabilityStatus::Adjacent1:
        return "Adjacent1";
    case MovementReachabilityStatus::AdjustedAdjacent2:
        return "AdjustedAdjacent2";
    case MovementReachabilityStatus::Path4:
        return "Path4";
    case MovementReachabilityStatus::Ambiguous:
        return "Ambiguous";
    }
    return "Unknown";
}

const char* passive_movement_route_kind_name(PassiveMovementRouteKind route) {
    switch (route) {
    case PassiveMovementRouteKind::Unaffected:
        return "Unaffected";
    case PassiveMovementRouteKind::TargetParticipant:
        return "TargetParticipant";
    case PassiveMovementRouteKind::SameSideParticipant:
        return "SameSideParticipant";
    case PassiveMovementRouteKind::SpecialParticipant:
        return "SpecialParticipant";
    }
    return "Unaffected";
}

} // namespace savor::predict
