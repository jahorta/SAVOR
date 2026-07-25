#include "BattlePredictor.h"

#include "ActionViewStdJsonLoader.h"
#include "BattleFrameSchedulerModel.h"
#include "BattlePredictionScenario.h"
#include "BattleVisualRngModel.h"
#include "ActionViewStdResourceResolver.h"
#include "CombatantInstructionModeModel.h"
#include "MovementModel.h"
#include "PreAiCameraModel.h"
#include "QueuedInstructionParamModel.h"
#include "RngCore.h"

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

namespace savor::predict {
namespace {

struct QueuedPredictionAction {
    int actor_slot = -1;
    int target_slot = -1;
    bool attack = false;
    bool guard = false;
    bool enemy_owned = false;
    std::optional<std::int16_t> initial_instr_param_0x6;
    int instr_param_0x6 = 0;
    BasicAttackExecutionRoute execution_route = BasicAttackExecutionRoute::Unknown;
    std::string instruction_parameter_provenance;
    int game_action_ordinal = -1;
    std::optional<int> frame_action_ordinal;
    std::string source;
};

struct DropRowSimulation {
    int row_index = -1;
    std::uint16_t rand_value = 0;
    int rand_mod100 = 0;
    int chance = 0;
    int item_id = -1;
    int amount = 0;
    bool success = false;
};

struct DropTableSimulation {
    std::uint32_t end_state = 0;
    int draws_consumed = 0;
    std::optional<int> item_id;
    std::optional<int> amount;
    std::vector<DropRowSimulation> rows;
};

std::string hex_seed(std::uint32_t seed) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << seed;
    return out.str();
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << c; break;
        }
    }
    return out.str();
}

std::optional<int> element_effectiveness_tenths(
    const soa::ElementalEffectiveness& effectiveness,
    int element) {
    switch (element) {
    case 0: return effectiveness.green;
    case 1: return effectiveness.red;
    case 2: return effectiveness.purple;
    case 3: return effectiveness.blue;
    case 4: return effectiveness.Yellow;
    case 5: return effectiveness.Silver;
    default: return std::nullopt;
    }
}

int first_alive_enemy_slot(const std::vector<BattlePredictionSlotState>& slots) {
    for (const auto& slot : slots) {
        if (slot.present && !slot.is_player && slot.alive) {
            return slot.slot;
        }
    }
    return -1;
}

bool has_alive_enemy(const std::vector<BattlePredictionSlotState>& slots) {
    return first_alive_enemy_slot(slots) >= 0;
}

bool has_alive_pc(const std::vector<BattlePredictionSlotState>& slots) {
    for (const auto& slot : slots) {
        if (slot.present && slot.is_player && slot.alive) {
            return true;
        }
    }
    return false;
}

int present_alive_pc_count(const std::vector<BattlePredictionSlotState>& slots) {
    int count = 0;
    for (const auto& slot : slots) {
        if (slot.present && slot.is_player && slot.alive) {
            ++count;
        }
    }
    return count;
}

BattlePredictionSlotState make_slot_state(
    const BattlePredictionProfile& profile,
    const soa::battle::ctx::BattleContext& context,
    int slot_index,
    std::vector<std::string>& warnings) {
    BattlePredictionSlotState state;
    state.slot = slot_index;
    if (slot_index < 0 || slot_index >= soa::battle::ctx::SLOT_COUNT) {
        return state;
    }

    const auto& source = context.slots_[slot_index];
    state.present = source.present != 0;
    state.is_player = source.is_player != 0;
    state.alive = state.present && source.is_alive != 0 && source.instance.Current_HP > 0;
    state.id = source.id;
    state.current_hp = static_cast<int>(source.instance.Current_HP);
    state.max_hp = static_cast<int>(source.instance.Max_HP);
    state.status_flags = source.instance.status_flags;
    state.movement_flags = source.instance.movement_flags;
    state.base_counter_chance = source.instance.base_counter_chance;
    state.current_counter_chance = source.instance.current_counter_chance;
    state.counter_chance_increment = source.instance.counter_chance;
    state.agile = source.instance.current_base_stats.Agile;
    if (state.present && source.instance.current_base_stats.Quick > 0) {
        state.quick = source.instance.current_base_stats.Quick;
        state.quick_known = true;
    }
    state.attack = source.instance.current_derived_stats.Attack;
    state.defense = source.instance.current_derived_stats.Defense;
    state.hit = source.instance.current_derived_stats.HitChance;
    state.dodge = source.instance.current_derived_stats.DodgeChance;
    state.element = source.instance.current_weapon_element;
    state.attack_inputs_known =
        state.attack > 0 && state.hit > 0 && state.defense >= 0 && state.dodge >= 0;
    (void)profile;

    if (state.present && !state.quick_known) {
        warnings.push_back(
            "quick stat is not materialized in BattleContext for slot "
            + std::to_string(slot_index)
            + "; the affected turn-order subsystem will report MissingInput");
    }
    return state;
}

std::vector<BattlePredictionSlotState> make_initial_slots(
    const BattlePredictionProfile& profile,
    const soa::battle::ctx::BattleContext& context,
    std::vector<std::string>& warnings) {
    std::vector<BattlePredictionSlotState> slots;
    slots.reserve(soa::battle::ctx::SLOT_COUNT);
    for (int slot = 0; slot < soa::battle::ctx::SLOT_COUNT; ++slot) {
        slots.push_back(make_slot_state(profile, context, slot, warnings));
    }
    return slots;
}

BattlePredictionSlotState* find_slot(std::vector<BattlePredictionSlotState>& slots, int slot) {
    if (slot < 0 || slot >= static_cast<int>(slots.size())) {
        return nullptr;
    }
    return &slots[slot];
}

const BattlePredictionSlotState* find_slot(const std::vector<BattlePredictionSlotState>& slots, int slot) {
    if (slot < 0 || slot >= static_cast<int>(slots.size())) {
        return nullptr;
    }
    return &slots[slot];
}

const soa::battle::ctx::BattleSlot* context_slot(
    const soa::battle::ctx::BattleContext& context,
    int slot) {
    if (slot < 0 || slot >= soa::battle::ctx::SLOT_COUNT) {
        return nullptr;
    }
    return &context.slots_[slot];
}

std::vector<MovementSlotState> movement_slots_from_prediction_slots(
    const std::vector<BattlePredictionSlotState>& slots,
    const soa::battle::ctx::BattleContext& context) {
    std::vector<MovementSlotState> result;
    result.reserve(slots.size());
    for (const auto& slot : slots) {
        int width = 0;
        int depth = 0;
        if (const auto* source = context_slot(context, slot.slot); source != nullptr) {
            width = source->instance.width;
            depth = source->instance.depth;
        }
        result.push_back(MovementSlotState{
            .slot = slot.slot,
            .present = slot.present,
            .is_player = slot.is_player,
            .alive = slot.alive,
            .status_flags = slot.status_flags,
            .movement_flags = slot.movement_flags,
            .motion_base_speed = slot.motion_base_speed,
            .motion_alt_speed = slot.motion_alt_speed,
            .motion_speeds_known = slot.motion_speeds_known,
            .motion_turn_speed = slot.motion_turn_speed,
            .motion_turn_speed_bits = slot.motion_turn_speed_bits,
            .motion_turn_speed_known = slot.motion_turn_speed_known,
            .width = width,
            .depth = depth,
            .start_position = slot.start_position,
        });
    }
    return result;
}

void sync_frame_runtime_from_slots(
    BattleFrameRuntime* runtime,
    const std::vector<BattlePredictionSlotState>& slots) {
    if (runtime == nullptr || !runtime->initialized) {
        return;
    }
    for (auto& combatant : runtime->state.combatants) {
        const auto* slot = find_slot(slots, combatant.slot);
        if (slot == nullptr) {
            continue;
        }
        combatant.present = slot->present;
        combatant.alive = slot->alive;
    }
}

void append_event(BattlePredictionResult& result, BattlePredictionEvent event) {
    if (event.status == BattlePredictionEventStatus::Ambiguous
        || event.status == BattlePredictionEventStatus::MissingInput) {
        event.draws_consumed = 0;
        event.rng_seed_before.reset();
        event.rng_seed_after.reset();
        event.rand_value.reset();
    }
    event.sequence = static_cast<int>(result.events.size()) + 1;
    result.total_draws_consumed += event.draws_consumed;
    if (event.status == BattlePredictionEventStatus::MissingInput) {
        result.has_missing_input_events = true;
    }
    if (event.status == BattlePredictionEventStatus::Provisional) {
        result.has_provisional_events = true;
    }
    if (event.status == BattlePredictionEventStatus::Ambiguous) {
        result.has_ambiguous_events = true;
    }
    if (event.status == BattlePredictionEventStatus::Unsupported) {
        result.has_unsupported_events = true;
    }
    result.events.push_back(std::move(event));
}

std::string start_position_detail(const BattleStartPosition& position) {
    std::ostringstream detail;
    detail << position.combatant_name
           << " id=" << position.combatant_id
           << " grid_x=" << position.grid_x
           << " grid_z=" << position.grid_z
           << " side=" << (position.is_player ? "pc" : "enemy");
    return detail.str();
}

void append_slot_position_if_known(
    std::ostringstream& detail,
    const std::vector<BattlePredictionSlotState>& slots,
    int slot,
    std::string_view label) {
    const auto* state = find_slot(slots, slot);
    if (state == nullptr || !state->start_position.has_value()) {
        return;
    }
    detail << "; " << label << "_start=("
           << state->start_position->grid_x << ","
           << state->start_position->grid_z << ")";
}

void append_worksheet_projection_detail(
    std::ostringstream& detail,
    const MovementWorksheetSnapshot& worksheet) {
    if (!worksheet.available) {
        return;
    }
    if (!worksheet.source.empty()) {
        detail << "; worksheet_source=" << worksheet.source;
    }
    if (worksheet.actor_grid_position.has_value()) {
        detail << "; actor_grid=("
               << worksheet.actor_grid_position->grid_x << ","
               << worksheet.actor_grid_position->grid_z << ")";
    }
    if (worksheet.target_grid_position.has_value()) {
        detail << "; target_grid=("
               << worksheet.target_grid_position->grid_x << ","
               << worksheet.target_grid_position->grid_z << ")";
    }
    if (worksheet.actor_raw_stage_position.has_value()
        || worksheet.target_raw_stage_position.has_value()) {
        detail << "; raw_stage_position=known";
    } else {
        detail << "; raw_stage_position=not_projected_from_alx_grid";
    }
    if (worksheet.dist_to_target.has_value()) {
        detail << "; grid_distance=" << *worksheet.dist_to_target;
    }
    if (worksheet.path_shape_forces_fallback.has_value()) {
        detail << "; grid_path_fallback="
               << (*worksheet.path_shape_forces_fallback ? "true" : "false");
    }
}

bool apply_source_start_positions(
    BattlePredictionResult& result,
    const BattleSourceSnapshot& source,
    std::vector<BattlePredictionSlotState>& slots) {
    if (source.placements.empty()) {
        append_event(result, {
            .phase = "encounter_setup",
            .label = "missing_input_source_start_positions",
            .status = BattlePredictionEventStatus::MissingInput,
            .detail = "validated source snapshot contains no encounter placements",
        });
        return false;
    }

    for (const auto& placement : source.placements) {
        auto* slot = find_slot(slots, placement.slot);
        if (slot == nullptr || !slot->present
            || slot->is_player != placement.is_player
            || slot->id != placement.combatant_id
            || placement.status != BattleSourceFieldStatus::Exact) {
            append_event(result, {
                .phase = "encounter_setup",
                .label = "source_start_position_mismatch",
                .status = BattlePredictionEventStatus::MissingInput,
                .actor_slot = placement.slot,
                .detail = "source placement does not match the captured combatant slot",
            });
            return false;
        }
        slot->start_position = BattleStartPosition{
            .slot = placement.slot,
            .present = true,
            .is_player = placement.is_player,
            .combatant_id = placement.combatant_id,
            .combatant_name = placement.combatant_name,
            .grid_x = placement.grid_x,
            .grid_z = placement.grid_z,
        };
        if (!slot->start_position.has_value()) {
            continue;
        }
        append_event(result, {
            .phase = "encounter_setup",
            .label = "source_start_position",
            .status = BattlePredictionEventStatus::Exact,
            .actor_slot = placement.slot,
            .detail = "manifest=" + source.manifest_key + "; "
                + start_position_detail(*slot->start_position)
                + "; provenance=" + placement.provenance,
        });
    }
    return true;
}

bool valid_battle_turn_type(soa::battle::TurnType turn_type) {
    const int value = static_cast<int>(turn_type);
    return value >= static_cast<int>(soa::battle::TurnType::BackAttack)
        && value <= static_cast<int>(soa::battle::TurnType::Advantage);
}

BattlePredictionEventStatus prediction_status_from_initial_turn_type_status(
    BattleInitialTurnTypeStatus status) {
    switch (status) {
    case BattleInitialTurnTypeStatus::Exact:
        return BattlePredictionEventStatus::Exact;
    case BattleInitialTurnTypeStatus::MissingInput:
        return BattlePredictionEventStatus::MissingInput;
    case BattleInitialTurnTypeStatus::Unsupported:
        return BattlePredictionEventStatus::Unsupported;
    }
    return BattlePredictionEventStatus::Unsupported;
}

std::optional<soa::battle::TurnType> resolve_prediction_initial_turn_type(
    BattlePredictionResult& result,
    std::uint32_t& rng_state,
    const BattlePredictionInput& input,
    const BattleSourceSnapshot& source) {
    if (input.start_boundary == BattlePredictionStartBoundary::CapturedTurnStart) {
        if (!valid_battle_turn_type(input.context.turn_type)) {
            append_event(result, {
                .phase = "initial_state",
                .label = "captured_turn_type_missing",
                .status = BattlePredictionEventStatus::MissingInput,
                .detail = "captured-turn start requires a valid BattleContext turn type",
            });
            return std::nullopt;
        }
        result.initial_turn_type = input.context.turn_type;
        append_event(result, {
            .phase = "initial_state",
            .label = "captured_turn_type",
            .status = BattlePredictionEventStatus::Exact,
            .turn_type = static_cast<int>(input.context.turn_type),
            .detail = "captured-turn boundary begins after calculateTurnType_80010CF4; "
                "the context value is part of that later state snapshot",
        });
    } else {
        if (source.encounter_id < 0) {
            append_event(result, {
                .phase = "battle_coordinator",
                .label = "initial_turn_type_missing_input",
                .status = BattlePredictionEventStatus::MissingInput,
                .detail = "battle-coordinator start requires encounter source and turn-type prerequisites",
            });
            return std::nullopt;
        }

        const auto modeled = model_initial_battle_turn_type({
            .prerequisites = BattleInitialTurnTypePrerequisites{
                .encounter_source = source.encounter_source_kind,
                .encounter_id = source.encounter_id,
            },
            .rng_seed_before = rng_state,
        });
        if (modeled.status != BattleInitialTurnTypeStatus::Exact
            || !modeled.turn_type.has_value()) {
            append_event(result, {
                .phase = "battle_coordinator",
                .label = "initial_turn_type_unresolved",
                .status = prediction_status_from_initial_turn_type_status(modeled.status),
                .detail = modeled.detail + "; provenance=" + modeled.provenance,
            });
            return std::nullopt;
        }

        if (modeled.advantage_draw.has_value()) {
            const auto& draw = *modeled.advantage_draw;
            append_event(result, {
                .phase = "battle_coordinator",
                .label = "turn_type_advantage_check",
                .status = BattlePredictionEventStatus::Exact,
                .rng_seed_before = draw.seed_before,
                .rng_seed_after = draw.seed_after,
                .draws_consumed = 1,
                .rand_value = draw.rand_value,
                .detail = "caller=calculateTurnType_80010CF4; source=rand_8025ECC4; "
                    "roll_mod_101=" + std::to_string(draw.roll_mod_101)
                    + "; threshold=" + std::to_string(draw.threshold),
            });
        }
        if (modeled.back_attack_draw.has_value()) {
            const auto& draw = *modeled.back_attack_draw;
            append_event(result, {
                .phase = "battle_coordinator",
                .label = "turn_type_back_attack_check",
                .status = BattlePredictionEventStatus::Exact,
                .rng_seed_before = draw.seed_before,
                .rng_seed_after = draw.seed_after,
                .draws_consumed = 1,
                .rand_value = draw.rand_value,
                .detail = "caller=calculateTurnType_80010CF4; source=rand_8025ECC4; "
                    "roll_mod_101=" + std::to_string(draw.roll_mod_101)
                    + "; threshold=" + std::to_string(draw.threshold),
            });
        }

        rng_state = modeled.rng_seed_after;
        result.initial_turn_type = modeled.turn_type;
        append_event(result, {
            .phase = "battle_coordinator",
            .label = modeled.draws_consumed == 0
                ? "event_turn_type_normal"
                : "initial_turn_type_selected",
            .status = BattlePredictionEventStatus::Exact,
            .rng_seed_before = rng_state,
            .rng_seed_after = rng_state,
            .turn_type = static_cast<int>(*modeled.turn_type),
            .detail = modeled.detail + "; provenance=" + modeled.provenance,
        });

        if (valid_battle_turn_type(input.context.turn_type)) {
            const bool context_matches = input.context.turn_type == *modeled.turn_type;
            append_event(result, {
                .phase = "battle_coordinator",
                .label = context_matches
                    ? "battle_context_turn_type_matches"
                    : "battle_context_turn_type_mismatch_ignored",
                .status = context_matches
                    ? BattlePredictionEventStatus::Exact
                    : BattlePredictionEventStatus::Skipped,
                .turn_type = static_cast<int>(input.context.turn_type),
                .detail = "BattleContext turn type is comparison evidence only at "
                    "BattleCoordinatorStart; predicted="
                    + std::string(battle_turn_type_name(*modeled.turn_type))
                    + "; captured="
                    + battle_turn_type_name(input.context.turn_type),
            });
        }
    }

    return result.initial_turn_type;
}

bool has_event(
    const BattlePredictionResult& result,
    std::string_view phase,
    std::string_view label) {
    return std::any_of(
        result.events.begin(),
        result.events.end(),
        [&](const BattlePredictionEvent& event) {
            return event.phase == phase && event.label == label;
        });
}

bool has_phase_event(
    const BattlePredictionResult& result,
    std::string_view phase) {
    return std::any_of(
        result.events.begin(),
        result.events.end(),
        [&](const BattlePredictionEvent& event) {
            return event.phase == phase;
        });
}

bool has_event_with_status(
    const BattlePredictionResult& result,
    std::string_view phase,
    std::string_view label,
    BattlePredictionEventStatus status) {
    return std::any_of(
        result.events.begin(),
        result.events.end(),
        [&](const BattlePredictionEvent& event) {
            return event.phase == phase && event.label == label && event.status == status;
        });
}

bool has_event_status_in_phase(
    const BattlePredictionResult& result,
    std::string_view phase,
    BattlePredictionEventStatus status) {
    return std::any_of(
        result.events.begin(),
        result.events.end(),
        [&](const BattlePredictionEvent& event) {
            return event.phase == phase && event.status == status;
        });
}

void add_validation(
    BattlePredictionResult& result,
    std::string scope,
    BattlePredictionValidationStatus status,
    std::string detail,
    int draws_exact_through = -1) {
    result.validation.push_back(BattlePredictionValidationItem{
        .scope = std::move(scope),
        .status = status,
        .draws_exact_through = draws_exact_through,
        .detail = std::move(detail),
    });
}

void append_validation_statuses(BattlePredictionResult& result) {
    result.validation.clear();

    if (has_event(result, "profile", "profile_contract_rejected")) {
        add_validation(
            result,
            "profile_contract",
            BattlePredictionValidationStatus::Unsupported,
            "the prediction input does not satisfy the selected profile contract");
    } else if (has_event(result, "profile", "profile_contract_override")) {
        add_validation(
            result,
            "profile_contract",
            BattlePredictionValidationStatus::Provisional,
            "profile constraints were explicitly overridden for a research run");
    } else if (has_event(result, "profile", "profile_contract_validated")) {
        add_validation(
            result,
            "profile_contract",
            BattlePredictionValidationStatus::Validated,
            "turn, encounter, and roster match the selected profile");
    } else {
        add_validation(
            result,
            "profile_contract",
            BattlePredictionValidationStatus::NotExercised,
            "profile validation did not run");
    }

    if (has_event_status_in_phase(
            result,
            "battle_coordinator",
            BattlePredictionEventStatus::MissingInput)
        || has_event_status_in_phase(
            result,
            "initial_state",
            BattlePredictionEventStatus::MissingInput)) {
        add_validation(
            result,
            "initial_turn_type",
            BattlePredictionValidationStatus::MissingInput,
            "the selected start boundary could not establish a turn type");
    } else if (result.initial_turn_type.has_value()) {
        add_validation(
            result,
            "initial_turn_type",
            BattlePredictionValidationStatus::Exact,
            "turn type is owned by the selected predictor start boundary");
    } else {
        add_validation(
            result,
            "initial_turn_type",
            BattlePredictionValidationStatus::NotExercised,
            "initial turn-type resolution did not run");
    }

    add_validation(
        result,
        "pre_ai",
        BattlePredictionValidationStatus::Provisional,
        "v1 macro contract is fake_attacks + 1 + pc_count; direct 800608DC capture remains perturbing");

    if (has_event_status_in_phase(result, "encounter_setup", BattlePredictionEventStatus::MissingInput)) {
        add_validation(
            result,
            "start_positions",
            BattlePredictionValidationStatus::MissingInput,
            "requested enemy-event start-position row is not encoded");
    } else if (has_event(result, "encounter_setup", "source_start_position")) {
        add_validation(
            result,
            "start_positions",
            BattlePredictionValidationStatus::Exact,
            "combatant start positions are sourced from an encounter-resolved SPICE ALX snapshot");
    } else {
        add_validation(
            result,
            "start_positions",
            BattlePredictionValidationStatus::NotExercised,
            "no validated source placements were installed");
    }

    if (has_event_status_in_phase(result, "enemy_ai", BattlePredictionEventStatus::Unsupported)) {
        add_validation(
            result,
            "soldier_ai",
            BattlePredictionValidationStatus::Unsupported,
            "enemy AI prediction encountered an unsupported event");
    } else if (has_phase_event(result, "enemy_ai")) {
        add_validation(
            result,
            "soldier_ai",
            BattlePredictionValidationStatus::Validated,
            "first-battle Soldier AI action, target, and attack-parameter draws are validated by aggregate traces");
    } else {
        add_validation(
            result,
            "soldier_ai",
            BattlePredictionValidationStatus::NotExercised,
            "no enemy AI events were predicted");
    }

    if (has_event_status_in_phase(result, "turn_order", BattlePredictionEventStatus::MissingInput)) {
        add_validation(
            result,
            "turn_order",
            BattlePredictionValidationStatus::MissingInput,
            "turn order is missing fixed-priority input data",
            result.exact_draws_through_turn_order);
    } else if (result.exact_through_turn_order) {
        add_validation(
            result,
            "turn_order",
            BattlePredictionValidationStatus::Validated,
            "draw count and execution order are exact for the modeled queue",
            result.exact_draws_through_turn_order);
    } else if (has_event_status_in_phase(result, "turn_order", BattlePredictionEventStatus::Provisional)) {
        add_validation(
            result,
            "turn_order",
            BattlePredictionValidationStatus::Provisional,
            "turn order used qsort priority-tie behavior that is modeled but still pending broader stress testing",
            result.exact_draws_through_turn_order);
    } else if (has_event_status_in_phase(result, "turn_order", BattlePredictionEventStatus::Ambiguous)) {
        add_validation(
            result,
            "turn_order",
            BattlePredictionValidationStatus::Ambiguous,
            "turn order is ambiguous because ordering behavior was not exact",
            result.exact_draws_through_turn_order);
    } else {
        add_validation(
            result,
            "turn_order",
            BattlePredictionValidationStatus::Provisional,
            "turn-order prediction did not reach the exact validation boundary",
            result.exact_draws_through_turn_order);
    }

    if (has_event_status_in_phase(result, "movement_setup", BattlePredictionEventStatus::MissingInput)) {
        add_validation(
            result,
            "movement_setup",
            BattlePredictionValidationStatus::MissingInput,
            "movement setup needs worksheet/path inputs that are not materialized");
    } else if (has_event_status_in_phase(result, "movement_setup", BattlePredictionEventStatus::Unsupported)) {
        add_validation(
            result,
            "movement_setup",
            BattlePredictionValidationStatus::Unsupported,
            "movement setup encountered an unsupported action");
    } else if (has_event_status_in_phase(result, "movement_setup", BattlePredictionEventStatus::Ambiguous)) {
        add_validation(
            result,
            "movement_setup",
            BattlePredictionValidationStatus::Ambiguous,
            "direct/fallback movement mode requires movement worksheet/path fields that are not fully materialized yet");
    } else if (has_event(result, "frame_scheduler", "runtime_initialized")
        && has_phase_event(result, "frame_scheduler")) {
        add_validation(
            result,
            "movement_setup",
            BattlePredictionValidationStatus::Provisional,
            "persistent frame scheduler is active for all scheduled first-battle combatant movement workers; worker branch exactness remains under validation");
    } else if (has_phase_event(result, "movement_setup")) {
        add_validation(
            result,
            "movement_setup",
            BattlePredictionValidationStatus::Provisional,
            "first-battle movement setup is modeled; representative live worker-selection validation remains open");
    } else {
        add_validation(
            result,
            "movement_setup",
            BattlePredictionValidationStatus::NotExercised,
            "no movement setup events were predicted");
    }

    if (has_event_with_status(result, "movement_setup", "worker_select", BattlePredictionEventStatus::MissingInput)) {
        add_validation(
            result,
            "movement_instr_param",
            BattlePredictionValidationStatus::MissingInput,
            "final instr_param_0x6 depends on movement worksheet/path inputs unavailable in the current BattleContext");
    } else if (has_event_with_status(result, "movement_setup", "worker_select", BattlePredictionEventStatus::Ambiguous)) {
        add_validation(
            result,
            "movement_instr_param",
            BattlePredictionValidationStatus::Ambiguous,
            "final instr_param_0x6 depends on reachability/path state unavailable in the current BattleContext");
    } else if (has_event(result, "movement_setup", "worker_select")) {
        add_validation(
            result,
            "movement_instr_param",
            BattlePredictionValidationStatus::Provisional,
            "final instr_param_0x6 is supplied by MovementModel and feeds attack resolution");
    } else {
        add_validation(
            result,
            "movement_instr_param",
            BattlePredictionValidationStatus::NotExercised,
            "no attack worker selection was predicted");
    }

    if (has_event_with_status(result, "movement_setup", "pc_attack_retarget", BattlePredictionEventStatus::Ambiguous)
        || has_event_with_status(result, "movement_setup", "enemy_attack_retarget", BattlePredictionEventStatus::Ambiguous)) {
        add_validation(
            result,
            "retargeting",
            BattlePredictionValidationStatus::Ambiguous,
            "multi-candidate retargeting requires pathing and qsort candidate ordering beyond first-battle v1");
    } else if (has_event(result, "movement_setup", "pc_attack_retarget")
        || has_event(result, "movement_setup", "enemy_attack_retarget")) {
        add_validation(
            result,
            "retargeting",
            BattlePredictionValidationStatus::Validated,
            "single living replacement target retargeting is validated as RNG-neutral for first-battle v1");
    } else {
        add_validation(
            result,
            "retargeting",
            BattlePredictionValidationStatus::NotExercised,
            "no invalid queued target required repair");
    }

    if (has_event(result, "death_drop", "enemy_drop")
        || has_event(result, "death_drop", "enemy_no_drop")) {
        add_validation(
            result,
            "drop",
            BattlePredictionValidationStatus::Validated,
            "first-battle Soldier drop row order and stop-after-success are validated for Electri Box, Moonberry, and no-drop outcomes; live ordering shows lethal damage enters death/drop before the following combat-effect RNG burst");
    } else {
        add_validation(
            result,
            "drop",
            BattlePredictionValidationStatus::NotExercised,
            "no lethal enemy damage reached the drop table");
    }

    if (has_event(result, "frame_scheduler", "fun_8002eb4c_action_service")) {
        add_validation(
            result,
            "action_service_eb4c",
            BattlePredictionValidationStatus::Provisional,
            "the frame scheduler reached FUN_8002EB4C from a flagged SET COMMAND action-service child and selected mode 0x0C/0x0D");
    } else {
        add_validation(
            result,
            "action_service_eb4c",
            BattlePredictionValidationStatus::NotExercised,
            "no flagged action-service child reached FUN_8002EB4C");
    }

    if (has_event(result, "frame_scheduler", "unsupported_effect_source_key")
        || has_event(result, "frame_scheduler", "unsupported_effect_source_actor_slot")) {
        add_validation(
            result,
            "effect_burst",
            BattlePredictionValidationStatus::Unsupported,
            "an effect source key lacked a supported first-battle burst model");
    } else if (has_event(result, "frame_scheduler", "combat_effect_chunk")) {
        add_validation(
            result,
            "effect_burst",
            BattlePredictionValidationStatus::Provisional,
            "the frame scheduler splits supported first-battle effect bursts into provisional chunks while preserving source-key draw totals");
    } else {
        add_validation(
            result,
            "effect_burst",
            BattlePredictionValidationStatus::NotExercised,
            "no landed attack effect burst was predicted");
    }

    if (has_event(result, "frame_scheduler", "combat_effect_chunk")) {
        add_validation(
            result,
            "action_source_selection",
            BattlePredictionValidationStatus::Provisional,
            "source keys are selected from first-battle actor/crit/counter rules; the FUN_8006782c -> FUN_8006721c live bridge remains open");
    } else {
        add_validation(
            result,
            "action_source_selection",
            BattlePredictionValidationStatus::NotExercised,
            "no action visual RNG source key was needed");
    }

    if (has_event(result, "frame_scheduler", "missing_input_action_view_camera_aux_table")
        || has_event(result, "frame_scheduler", "missing_input_action_view_camera_mode0e_count")) {
        add_validation(
            result,
            "action_view_selector",
            BattlePredictionValidationStatus::MissingInput,
            "action-view selector needs the selected _0_STD aux table data");
    } else if (has_event(result, "frame_scheduler", "ambiguous_action_view_camera_dispatch_evidence")) {
        add_validation(
            result,
            "action_view_selector",
            BattlePredictionValidationStatus::Ambiguous,
            "action-view dispatch evidence contains conflicting record mode or RNG owner facts");
    } else if (has_event(result, "frame_scheduler", "mode0e_action_view_camera")
        || has_event(result, "frame_scheduler", "mode0_action_view_camera_fallback")
        || has_event(result, "frame_scheduler", "action_view_record_mode_no_camera_rng")) {
        add_validation(
            result,
            "action_view_selector",
            BattlePredictionValidationStatus::Provisional,
            "selector-backed action-view camera owner was modeled; branch-level live validation remains required");
    } else if (has_event(result, "frame_scheduler", "mode0_action_view_camera_rewrite_gate")) {
        add_validation(
            result,
            "action_view_selector",
            BattlePredictionValidationStatus::Provisional,
            "prediction used aggregate first-battle action-view camera fallback because per-action selector state was not supplied");
    } else {
        add_validation(
            result,
            "action_view_selector",
            BattlePredictionValidationStatus::NotExercised,
            "no action-view camera selector event was predicted");
    }

    if (has_event_with_status(
            result,
            "frame_scheduler",
            "view_placement_direct_view",
            BattlePredictionEventStatus::MissingInput)
        || has_event_with_status(
            result,
            "frame_scheduler",
            "view_placement_end_turn",
            BattlePredictionEventStatus::MissingInput)) {
        add_validation(
            result,
            "view_placement",
            BattlePredictionValidationStatus::MissingInput,
            "frame-backed view placement could not construct a complete provisional request");
    } else if (has_event(result, "frame_scheduler", "view_placement_direct_view")
        || has_event(result, "frame_scheduler", "view_placement_end_turn")) {
        add_validation(
            result,
            "view_placement",
            BattlePredictionValidationStatus::Provisional,
            "frame workers invoke the validated cache primitive with provisional readiness, geometry, initial-state, and scheduling assumptions; RNG offsets remain diagnostic");
    } else {
        add_validation(
            result,
            "view_placement",
            BattlePredictionValidationStatus::NotExercised,
            "the frame scheduler did not invoke view placement");
    }

    if (has_event_status_in_phase(result, "action_view_pathing_tail", BattlePredictionEventStatus::MissingInput)) {
        add_validation(
            result,
            "action_view_pathing_tail",
            BattlePredictionValidationStatus::MissingInput,
            "action-view pathing tail needs complete producer-derived frame state");
    } else if (has_event_status_in_phase(result, "action_view_pathing_tail", BattlePredictionEventStatus::Unsupported)) {
        add_validation(
            result,
            "action_view_pathing_tail",
            BattlePredictionValidationStatus::Unsupported,
            "action-view pathing tail encountered an unsupported profile or callback state");
    } else if (has_event_status_in_phase(result, "action_view_pathing_tail", BattlePredictionEventStatus::Ambiguous)) {
        add_validation(
            result,
            "action_view_pathing_tail",
            BattlePredictionValidationStatus::Ambiguous,
            "action-view pathing tail requires frame state that was not available");
    } else if (has_phase_event(result, "action_view_pathing_tail")) {
        add_validation(
            result,
            "action_view_pathing_tail",
            BattlePredictionValidationStatus::Provisional,
            "frame-backed FUN_800519f4/FUN_80011694 pathing tail consumes producer-derived combatant state; broader profile and callback timing remain under validation");
    } else {
        add_validation(
            result,
            "action_view_pathing_tail",
            BattlePredictionValidationStatus::NotExercised,
            "no landed attack reached the action-view pathing tail");
    }

    if (has_phase_event(result, "counter") || has_phase_event(result, "counter_follow_up")) {
        add_validation(
            result,
            "counter",
            BattlePredictionValidationStatus::Provisional,
            "counter increment and forced follow-up model are implemented, but full live actor/target gate fields remain open");
    } else {
        add_validation(
            result,
            "counter",
            BattlePredictionValidationStatus::NotExercised,
            "no counter gate was reached");
    }

    BattlePredictionValidationStatus total_status = BattlePredictionValidationStatus::Provisional;
    std::string total_detail =
        "total draw count after turn order is provisional; trust exact_draws_through_turn_order as the current validation boundary";
    if (result.has_missing_input_events) {
        total_status = BattlePredictionValidationStatus::MissingInput;
        total_detail =
            "missing required predictor inputs prevent continuing prediction";
    } else if (result.has_unsupported_events || !result.errors.empty()) {
        total_status = BattlePredictionValidationStatus::Unsupported;
        total_detail =
            "unsupported events or errors prevent total draw-count validation";
    } else if (result.has_ambiguous_events) {
        total_status = BattlePredictionValidationStatus::Ambiguous;
        total_detail =
            "ambiguous events prevent total draw-count validation";
    } else if (result.has_provisional_events) {
        total_status = BattlePredictionValidationStatus::Provisional;
        total_detail =
            "provisional events make total draw-count validation provisional";
    }
    add_validation(
        result,
        "total_draws",
        total_status,
        total_detail,
        result.exact_draws_through_turn_order);
}

void advance_without_rand_values(std::uint32_t& state, int draws) {
    for (int i = 0; i < draws; ++i) {
        state = advance_once(state);
    }
}

std::optional<BasicAttackInputs> basic_attack_inputs_for(
    const soa::battle::ctx::BattleContext& context,
    const std::vector<BattlePredictionSlotState>& slots,
    int attacker_slot,
    int target_slot,
    int instr_param_0x6) {
    const auto* attacker = find_slot(slots, attacker_slot);
    const auto* target = find_slot(slots, target_slot);
    const auto* target_context_slot = context_slot(context, target_slot);
    if (attacker == nullptr || target == nullptr || target_context_slot == nullptr
        || !attacker->attack_inputs_known || !target->attack_inputs_known) {
        return std::nullopt;
    }

    BasicAttackInputs inputs;
    inputs.attacker_attack = attacker->attack;
    inputs.attacker_hit = attacker->hit;
    inputs.attacker_agile = attacker->agile;
    inputs.attacker_element = attacker->element;
    inputs.target_defense = target->defense;
    inputs.target_dodge = target->dodge;
    const auto effectiveness = element_effectiveness_tenths(
        target_context_slot->instance.current_elemental_eff,
        attacker->element);
    if (!effectiveness.has_value() || *effectiveness <= 0) {
        return std::nullopt;
    }
    inputs.target_element_effectiveness_tenths = *effectiveness;
    inputs.target_status_flags = static_cast<int>(target->status_flags);
    inputs.instr_param_0x6 = instr_param_0x6;
    return inputs;
}

DropTableSimulation simulate_drop_table_from_enemy_definition(
    std::uint32_t state,
    const soa::EnemyDefinition& enemy_def) {
    DropTableSimulation result;
    result.end_state = state;

    for (int row = 0; row < 4; ++row) {
        const auto& item = enemy_def.items[row];
        if (item.chance == 0 || item.amount == 0 || item.itemId <= 0) {
            continue;
        }

        const auto draw = draw_rand15(result.end_state);
        result.end_state = draw.next_state;
        ++result.draws_consumed;

        DropRowSimulation row_result;
        row_result.row_index = row;
        row_result.rand_value = draw.value;
        row_result.rand_mod100 = draw.value % 100;
        row_result.chance = item.chance;
        row_result.item_id = item.itemId;
        row_result.amount = item.amount;
        row_result.success = row_result.rand_mod100 < row_result.chance;
        result.rows.push_back(row_result);

        if (row_result.success) {
            result.item_id = row_result.item_id;
            result.amount = row_result.amount;
            break;
        }
    }
    return result;
}

std::string drop_detail(const DropTableSimulation& drop) {
    std::ostringstream out;
    out << "drop table rows evaluated=" << drop.rows.size();
    if (drop.item_id.has_value()) {
        out << "; item_id=" << *drop.item_id << " amount=" << drop.amount.value_or(0);
    } else {
        out << "; no drop";
    }
    return out.str();
}

void append_pre_ai_events(
    BattlePredictionResult& result,
    std::uint32_t& state,
    int fake_attacks,
    int pc_count) {
    const auto model = model_pre_ai_camera_draws(fake_attacks, pc_count);

    if (model.fake_attack_draws > 0) {
        const auto before = state;
        advance_without_rand_values(state, model.fake_attack_draws);
        append_event(result, {
            .phase = "pre_ai",
            .label = "fake_attack_draws",
            .status = BattlePredictionEventStatus::Exact,
            .rng_seed_before = before,
            .rng_seed_after = state,
            .draws_consumed = model.fake_attack_draws,
            .detail = "v1 assumes every fake attack consumes one RNG draw",
        });
    }

    if (model.expected_camera_draws > 0) {
        const auto before = state;
        advance_without_rand_values(state, model.expected_camera_draws);
        append_event(result, {
            .phase = "pre_ai",
            .label = "camera_draws",
            .status = BattlePredictionEventStatus::Exact,
            .rng_seed_before = before,
            .rng_seed_after = state,
            .draws_consumed = model.expected_camera_draws,
            .detail = "v1 assumes battle-start camera plus one targeting camera draw per PC",
        });
    }
}

void append_player_commands(
    BattlePredictionResult& result,
    const soa::battle::actions::TurnPlan& turn_plan,
    const std::vector<BattlePredictionSlotState>& slots,
    std::vector<QueuedPredictionAction>& actions) {
    for (const auto& command : turn_plan.commands) {
        const int actor_slot = static_cast<int>(command.actor_slot);
        const auto* actor = find_slot(slots, actor_slot);
        if (actor == nullptr || !actor->present || !actor->alive) {
            append_event(result, {
                .phase = "player_command",
                .label = "skipped_missing_actor",
                .status = BattlePredictionEventStatus::Skipped,
                .actor_slot = actor_slot,
                .detail = "player command actor is not present and alive",
            });
            continue;
        }

        if (command.macro == soa::battle::actions::BattleAction::Attack) {
            const int target_slot =
                soa::battle::actions::resolveTargetIndex(command.params.target_slot);
            if (target_slot < 0) {
                append_event(result, {
                    .phase = "player_command",
                    .label = "missing_input_attack_target",
                    .status = BattlePredictionEventStatus::MissingInput,
                    .actor_slot = actor_slot,
                    .detail = "attack has no concrete live target",
                });
                continue;
            }
            const auto produced = model_queued_instruction_command({
                .command = QueuedInstructionCommandKind::Attack,
                .movement_flags = actor->movement_flags,
            });
            BattlePredictionEvent parameter_event;
            parameter_event.phase = "player_command";
            parameter_event.label = "queued_instruction_parameter";
            parameter_event.status = produced.parameter.status
                    == QueuedInstructionParamStatus::Validated
                ? BattlePredictionEventStatus::Exact
                : produced.parameter.status == QueuedInstructionParamStatus::MissingInput
                    ? BattlePredictionEventStatus::MissingInput
                    : BattlePredictionEventStatus::Provisional;
            parameter_event.actor_slot = actor_slot;
            parameter_event.target_slot = target_slot;
            if (produced.parameter.raw_value.has_value()) {
                parameter_event.instr_param_0x6 = *produced.parameter.raw_value;
                parameter_event.initial_instr_param_0x6 = *produced.parameter.raw_value;
            }
            parameter_event.detail =
                "instruction="
                + (produced.instruction.has_value()
                    ? std::to_string(*produced.instruction)
                    : std::string("missing"))
                + "; kind="
                + queued_instruction_param_kind_name(produced.parameter.kind)
                + "; stage="
                + queued_instruction_param_stage_name(produced.parameter.stage)
                + "; provenance=" + produced.parameter.provenance;
            append_event(result, std::move(parameter_event));
            if (!produced.parameter.raw_value.has_value()) {
                continue;
            }

            actions.push_back({
                .actor_slot = actor_slot,
                .target_slot = target_slot,
                .attack = true,
                .enemy_owned = false,
                .initial_instr_param_0x6 = *produced.parameter.raw_value,
                .instr_param_0x6 = *produced.parameter.raw_value,
                .instruction_parameter_provenance = produced.parameter.provenance,
                .source = "turn_plan",
            });
            continue;
        }

        if (command.macro == soa::battle::actions::BattleAction::Defend) {
            append_event(result, {
                .phase = "player_command",
                .label = "guard_no_turn",
                .status = BattlePredictionEventStatus::Exact,
                .actor_slot = actor_slot,
                .detail = "guarding characters do not enter turn-order execution",
            });
            actions.push_back({
                .actor_slot = actor_slot,
                .guard = true,
                .enemy_owned = false,
                .source = "turn_plan_guard",
            });
            continue;
        }

        append_event(result, {
            .phase = "player_command",
            .label = "unsupported_player_action",
            .status = BattlePredictionEventStatus::Unsupported,
            .actor_slot = actor_slot,
            .detail = "v1 BattlePredictor supports Attack and Guard only",
        });
    }
}

void append_enemy_ai(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const BattlePredictionProfile& profile,
    const std::vector<BattlePredictionSlotState>& slots,
    std::vector<QueuedPredictionAction>& actions) {
    for (int slot = 4; slot < soa::battle::ctx::SLOT_COUNT; ++slot) {
        const auto* actor = find_slot(slots, slot);
        if (actor == nullptr || !actor->present || !actor->alive) {
            continue;
        }
        if (is_first_battle_soldiers_profile_name(profile.name) && slot != 4 && slot != 5) {
            append_event(result, {
                .phase = "enemy_ai",
                .label = "unsupported_enemy_slot",
                .status = BattlePredictionEventStatus::Unsupported,
                .actor_slot = slot,
                .detail = "first-battle profile only supports Soldier AI in slots 4 and 5",
            });
            continue;
        }

        const auto before = state;
        const auto decision = resolve_soldier_ai(slot, state);
        const int draws = decision.attacks ? 3 : 1;
        std::ostringstream detail;
        detail << (decision.attacks ? "attack" : "guard")
               << "; action_rand=" << decision.action_rand;
        if (decision.target_pc_slot.has_value()) {
            detail << "; target_slot=" << *decision.target_pc_slot;
        }
        if (decision.attack_param_rand.has_value()) {
            detail << "; instr_param_0x6="
                   << soldier_attack_param_from_rand(*decision.attack_param_rand);
        }

        append_event(result, {
            .phase = "enemy_ai",
            .label = "soldier_ai",
            .status = BattlePredictionEventStatus::Exact,
            .actor_slot = slot,
            .target_slot = decision.target_pc_slot.value_or(-1),
            .rng_seed_before = before,
            .rng_seed_after = state,
            .draws_consumed = draws,
            .rand_value = decision.action_rand,
            .detail = detail.str(),
        });

        if (decision.attacks && decision.target_pc_slot.has_value() && decision.attack_param_rand.has_value()) {
            actions.push_back({
                .actor_slot = slot,
                .target_slot = *decision.target_pc_slot,
                .attack = true,
                .enemy_owned = true,
                .initial_instr_param_0x6 = static_cast<std::int16_t>(
                    soldier_attack_param_from_rand(*decision.attack_param_rand)),
                .instr_param_0x6 = soldier_attack_param_from_rand(*decision.attack_param_rand),
                .instruction_parameter_provenance =
                    "Soldier AI publishes the initial basic-attack parameter before execution rewrite",
                .source = "soldier_ai",
            });
        } else if (!decision.attacks) {
            actions.push_back({
                .actor_slot = slot,
                .guard = true,
                .enemy_owned = true,
                .source = "soldier_ai_guard",
            });
        }
    }
}

std::vector<TurnOrderEntryInput> turn_order_entries_for_actions(
    const std::vector<BattlePredictionSlotState>& slots,
    const std::vector<QueuedPredictionAction>& actions,
    BattlePredictionResult& result) {
    std::vector<TurnOrderEntryInput> entries;
    entries.reserve(actions.size());
    for (const auto& action : actions) {
        if (!action.attack) {
            continue;
        }
        const auto* actor = find_slot(slots, action.actor_slot);
        if (actor == nullptr || !actor->alive) {
            continue;
        }
        if (!actor->quick_known) {
            append_event(result, {
                .phase = "turn_order",
                .label = "missing_input_quick",
                .status = BattlePredictionEventStatus::MissingInput,
                .actor_slot = action.actor_slot,
                .detail = "turn-order prediction needs quick for every queued actor",
            });
            continue;
        }
        entries.push_back({
            .slot = action.actor_slot,
            .quick = actor->quick,
            .queued_instruction = 3,
        });
    }
    return entries;
}

const QueuedPredictionAction* find_action_for_slot(
    const std::vector<QueuedPredictionAction>& actions,
    int slot) {
    for (const auto& action : actions) {
        if (action.actor_slot == slot && action.attack) {
            return &action;
        }
    }
    return nullptr;
}

void append_turn_order(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const std::vector<BattlePredictionSlotState>& slots,
    const std::vector<QueuedPredictionAction>& actions,
    std::vector<int>& execution_slots) {
    const auto entries = turn_order_entries_for_actions(slots, actions, result);
    const auto before = state;
    const auto turn_order = simulate_turn_order(state, entries);
    state = turn_order.end_state;
    execution_slots = turn_order.execution_slots;

    std::uint32_t entry_rng_state = before;
    for (std::size_t i = 0; i < turn_order.entries.size(); ++i) {
        const auto& entry = turn_order.entries[i];
        std::optional<std::uint32_t> entry_seed_before;
        std::optional<std::uint32_t> entry_seed_after;
        if (entry.priority_rand.has_value()) {
            const auto draw = draw_rand15(entry_rng_state);
            entry_seed_before = entry_rng_state;
            entry_seed_after = draw.next_state;
            entry_rng_state = draw.next_state;
        }

        std::optional<int> qsort_index;
        for (std::size_t sorted_index = 0; sorted_index < turn_order.qsort_sorted_indices.size(); ++sorted_index) {
            if (turn_order.qsort_sorted_indices[sorted_index] == static_cast<int>(i)) {
                qsort_index = static_cast<int>(sorted_index);
                break;
            }
        }

        std::optional<int> execution_index;
        for (std::size_t order_index = 0; order_index < execution_slots.size(); ++order_index) {
            if (execution_slots[order_index] == entry.input.slot) {
                execution_index = static_cast<int>(order_index);
                break;
            }
        }

        const BattlePredictionEventStatus entry_status = !entry.assigned_priority.has_value()
            ? BattlePredictionEventStatus::MissingInput
            : (turn_order.priority_ties_ambiguous
                ? BattlePredictionEventStatus::Provisional
                : BattlePredictionEventStatus::Exact);

        append_event(result, {
            .phase = "turn_order",
            .label = "entry",
            .status = entry_status,
            .actor_slot = entry.input.slot,
            .rng_seed_before = entry_seed_before,
            .rng_seed_after = entry_seed_after,
            .draws_consumed = entry.priority_rand.has_value() ? 1 : 0,
            .rand_value = entry.priority_rand,
            .queue_index = static_cast<int>(i),
            .quick = entry.input.quick,
            .fixed_priority_result = entry.input.fixed_priority_result,
            .jitter_modulus = turn_order.jitter_modulus,
            .assigned_priority = entry.assigned_priority,
            .qsort_index = qsort_index,
            .execution_index = execution_index,
            .detail = std::string("priority_path=") + turn_order_priority_path_name(entry.path),
        });
    }

    std::ostringstream detail;
    detail << "queued=" << turn_order.queued_count
           << "; order=";
    for (std::size_t i = 0; i < execution_slots.size(); ++i) {
        if (i != 0) {
            detail << ",";
        }
        detail << execution_slots[i];
    }
    if (!turn_order.priorities_complete) {
        detail << "; unresolved fixed priority input prevents turn-order prediction";
    } else if (turn_order.priority_ties_ambiguous) {
        detail << "; priority tie uses modeled qsort behavior pending broader stress testing";
    } else if (!turn_order.execution_order_exact) {
        detail << "; execution order is not exact";
    }

    const BattlePredictionEventStatus resolve_status = !turn_order.priorities_complete
        ? BattlePredictionEventStatus::MissingInput
        : (turn_order.priority_ties_ambiguous
            ? BattlePredictionEventStatus::Provisional
            : (turn_order.execution_order_exact
                ? BattlePredictionEventStatus::Exact
                : BattlePredictionEventStatus::Ambiguous));

    append_event(result, {
        .phase = "turn_order",
        .label = "resolve_turn_order",
        .status = resolve_status,
        .rng_seed_before = before,
        .rng_seed_after = state,
        .draws_consumed = turn_order.draws_consumed,
        .detail = detail.str(),
    });

    result.exact_through_turn_order =
        !result.has_missing_input_events
        && !result.has_provisional_events
        && !result.has_unsupported_events
        && !result.has_ambiguous_events
        && turn_order.execution_order_exact;
    result.exact_draws_through_turn_order = result.total_draws_consumed;
}

BattlePredictionEventStatus battle_event_status_from_movement_status(MovementSimulationStatus status);
int movement_reachability_code(MovementReachabilityStatus status);
BattlePredictionEventStatus battle_event_status_from_frame_status(BattleFrameEventStatus status);
void append_frame_scheduler_events(
    BattlePredictionResult& result,
    const BattleFrameRunResult& frame_result,
    BattlePredictionEventStatus status);

bool append_movement_setup(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const soa::battle::ctx::BattleContext& context,
    const std::vector<BattlePredictionSlotState>& slots,
    BattleFrameRuntime* frame_runtime,
    QueuedPredictionAction& action) {
    const auto before = state;
    MovementModelInputs movement_inputs{
        .rng_state = state,
        .actor_slot = action.actor_slot,
        .target_slot = action.target_slot,
        .queued_instruction = action.attack ? 3 : (action.guard ? 4 : 0),
        .instr_param_0x6 = action.instr_param_0x6,
        .enemy_owned = action.enemy_owned,
        .slots = movement_slots_from_prediction_slots(slots, context),
    };
    if (result.encounter_id == 0) {
        movement_inputs.actor_worksheet = frame_runtime != nullptr
                && frame_runtime->initialized
                && !action.enemy_owned
            ? project_battle_frame_movement_worksheet_snapshot(
                *frame_runtime,
                action.actor_slot,
                action.target_slot)
            : project_enemy_event0_movement_worksheet_snapshot(movement_inputs);
    }
    const auto movement = simulate_first_battle_movement_setup(movement_inputs);
    state = movement.end_state;

    if (movement.target_repaired || !movement.can_execute) {
        std::ostringstream detail;
        detail << "original_target=" << movement.original_target_slot
               << "; final_target=" << movement.final_target_slot;
        if (!movement.detail.empty()) {
            detail << "; " << movement.detail;
        }
        append_event(result, {
            .phase = "movement_setup",
            .label = action.enemy_owned ? "enemy_attack_retarget" : "pc_attack_retarget",
            .status = battle_event_status_from_movement_status(movement.target_repair_status),
            .actor_slot = action.actor_slot,
            .target_slot = movement.final_target_slot,
            .detail = detail.str(),
        });
    }

    if (movement.draws_consumed > 0) {
        append_event(result, {
            .phase = "movement_setup",
            .label = "enemy_setup_draw",
            .status = BattlePredictionEventStatus::Exact,
            .actor_slot = action.actor_slot,
            .target_slot = movement.final_target_slot,
            .rng_seed_before = before,
            .rng_seed_after = state,
            .draws_consumed = movement.draws_consumed,
            .rand_value = movement.setup_rand,
            .instr_param_0x6 = movement.final_instr_param_0x6,
            .detail = enemy_attack_setup_path_name(movement.enemy_setup_path),
        });
    }

    if (!movement.can_execute) {
        return false;
    }

    action.target_slot = movement.final_target_slot;
    action.instr_param_0x6 = movement.final_instr_param_0x6;
    action.execution_route = movement.execution_route;

    if (frame_runtime == nullptr || !frame_runtime->initialized) {
        append_event(result, {
            .phase = "frame_scheduler",
            .label = "missing_frame_runtime",
            .status = BattlePredictionEventStatus::MissingInput,
            .actor_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .detail = "movement scheduling requires an initialized frame runtime",
        });
        return false;
    }

    const auto schedule = schedule_first_turn_actor_action(
        *frame_runtime,
        BattleFrameScheduleActionInput{
            .action_ordinal = action.game_action_ordinal,
            .actor_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .enemy_owned = action.enemy_owned,
            .combatant_command_parameter = action.instr_param_0x6,
            .initial_instruction_parameter = action.initial_instr_param_0x6,
            .final_instruction_parameter = static_cast<std::int16_t>(
                action.instr_param_0x6),
            .execution_route = action.execution_route,
            .selected_worker = movement.selected_worker,
            .action_kind = action.attack
                ? BattleMovementActionKind::BasicAttack
                : action.guard
                    ? BattleMovementActionKind::Guard
                    : BattleMovementActionKind::Unknown,
            .relation_scope = action.attack
                ? BattleMovementRelationScope::SingleTarget
                : BattleMovementRelationScope::Unsupported,
            .turn_type = static_cast<BattleMovementTurnType>(static_cast<int>(
                frame_runtime->state.initial_turn_type)),
        });
    if (!schedule.scheduled) {
        append_event(result, {
            .phase = "frame_scheduler",
            .label = "action_schedule_rejected",
            .status = schedule.status == BattleMovementInvocationStatus::MissingInput
                ? BattlePredictionEventStatus::MissingInput
                : BattlePredictionEventStatus::Ambiguous,
            .actor_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .action_ordinal = schedule.action_ordinal >= 0
                ? std::optional<int>{schedule.action_ordinal}
                : std::nullopt,
            .detail = schedule.detail,
        });
        return false;
    }
    action.frame_action_ordinal = schedule.action_ordinal;

    std::ostringstream detail;
    detail << "initial_instr_param_0x6=" << movement.initial_instr_param_0x6
           << "; final_instr_param_0x6=" << movement.final_instr_param_0x6
           << "; execution_route="
           << basic_attack_execution_route_name(movement.execution_route)
           << "; route_consistent="
           << (movement.execution_route_consistent ? 1 : 0)
           << "; reachability=" << movement_reachability_status_name(movement.reachability);
    append_slot_position_if_known(detail, slots, action.actor_slot, "actor");
    append_slot_position_if_known(detail, slots, action.target_slot, "target");
    append_worksheet_projection_detail(detail, movement_inputs.actor_worksheet);
    if (!movement.detail.empty()) {
        detail << "; " << movement.detail;
    }
    append_event(result, {
        .phase = "movement_setup",
        .label = "worker_select",
        .status = battle_event_status_from_movement_status(movement.status),
        .actor_slot = action.actor_slot,
        .target_slot = action.target_slot,
        .instr_param_0x6 = action.instr_param_0x6,
        .initial_instr_param_0x6 = movement.initial_instr_param_0x6,
        .final_instr_param_0x6 = movement.final_instr_param_0x6,
        .basic_attack_execution_route =
            basic_attack_execution_route_name(movement.execution_route),
        .movement_reachability = movement_reachability_code(movement.reachability),
        .movement_worker = movement_selected_worker_name(movement.selected_worker),
        .detail = detail.str(),
    });

    return true;
}

BattlePredictionEventStatus battle_event_status_from_movement_status(MovementSimulationStatus status) {
    switch (status) {
    case MovementSimulationStatus::Exact:
        return BattlePredictionEventStatus::Exact;
    case MovementSimulationStatus::Provisional:
        return BattlePredictionEventStatus::Provisional;
    case MovementSimulationStatus::Skipped:
        return BattlePredictionEventStatus::Skipped;
    case MovementSimulationStatus::MissingInput:
        return BattlePredictionEventStatus::MissingInput;
    case MovementSimulationStatus::Unsupported:
        return BattlePredictionEventStatus::Unsupported;
    case MovementSimulationStatus::Ambiguous:
        return BattlePredictionEventStatus::Ambiguous;
    }
    return BattlePredictionEventStatus::Unsupported;
}

BattlePredictionEventStatus battle_event_status_from_frame_status(BattleFrameEventStatus status) {
    switch (status) {
    case BattleFrameEventStatus::Matched:
        return BattlePredictionEventStatus::Exact;
    case BattleFrameEventStatus::Provisional:
        return BattlePredictionEventStatus::Provisional;
    case BattleFrameEventStatus::Skipped:
        return BattlePredictionEventStatus::Skipped;
    case BattleFrameEventStatus::MissingInput:
        return BattlePredictionEventStatus::MissingInput;
    case BattleFrameEventStatus::Unsupported:
        return BattlePredictionEventStatus::Unsupported;
    case BattleFrameEventStatus::Ambiguous:
        return BattlePredictionEventStatus::Ambiguous;
    }
    return BattlePredictionEventStatus::Unsupported;
}

int movement_reachability_code(MovementReachabilityStatus status) {
    switch (status) {
    case MovementReachabilityStatus::Failed0:
        return 0;
    case MovementReachabilityStatus::Adjacent1:
        return 1;
    case MovementReachabilityStatus::AdjustedAdjacent2:
        return 2;
    case MovementReachabilityStatus::Path4:
        return 4;
    case MovementReachabilityStatus::Unknown:
    case MovementReachabilityStatus::Ambiguous:
        return -1;
    }
    return -1;
}

std::string grid_detail(const MovementGridPosition& position) {
    std::ostringstream out;
    out << "(" << position.grid_x << "," << position.grid_z << ")";
    return out.str();
}

std::string frame_vec_detail(const BattleFrameVec3& position) {
    std::ostringstream out;
    out << "(" << position.x << "," << position.y << "," << position.z << ")";
    return out.str();
}

void append_frame_scheduler_events(
    BattlePredictionResult& result,
    const BattleFrameRunResult& frame_result,
    BattlePredictionEventStatus status) {
    for (const auto& frame_event : frame_result.events) {
        std::ostringstream detail;
        detail << "callback=" << frame_event.callback
               << "; worker_kind=" << battle_frame_worker_kind_name(frame_event.worker_kind)
               << "; frame_status=" << battle_frame_event_status_name(frame_event.status);
        if (frame_event.action_ordinal >= 0) {
            detail << "; action_ordinal=" << frame_event.action_ordinal
                   << "; controller_family="
                   << battle_movement_controller_family_name(frame_event.controller_family)
                   << "; relation_route="
                   << battle_movement_relation_route_name(frame_event.relation_route)
                   << "; action_phase="
                   << battle_frame_action_phase_name(frame_event.action_phase)
                   << "; activation_timing="
                   << battle_movement_activation_timing_name(frame_event.activation_timing)
                   << "; semantic_target_slot=" << frame_event.target_slot
                   << "; thread_order_index=" << frame_event.thread_order_index
                   << "; controller_state="
                   << battle_movement_controller_state_name(frame_event.old_controller_state)
                   << "->"
                   << battle_movement_controller_state_name(frame_event.new_controller_state);
            detail << "; callback_pc=" << hex_seed(frame_event.callback_pc)
                   << "; deferred_callback_pc="
                   << hex_seed(frame_event.deferred_callback_pc)
                   << "; completion_mask=0x" << std::hex
                   << frame_event.passive_completion_mask_before << "->0x"
                   << frame_event.passive_completion_mask_after << std::dec;
            if (!frame_event.completion_reason.empty()) {
                detail << "; completion_reason=" << frame_event.completion_reason;
            }
        }
        if (frame_event.combatant_instruction_revision > 0) {
            detail << "; combatant_instruction_revision="
                   << frame_event.combatant_instruction_revision
                   << "; combatant_instruction_phase="
                   << battle_frame_combatant_instruction_phase_name(
                          frame_event.instruction_phase_before)
                   << "->"
                   << battle_frame_combatant_instruction_phase_name(
                          frame_event.instruction_phase_after);
        }
        if (frame_event.combatant_state_available) {
            detail << "; action_mode=" << frame_event.old_action_mode
                   << "->" << frame_event.new_action_mode
                   << " (" << battle_frame_action_mode_name(frame_event.old_action_mode)
                   << "->" << battle_frame_action_mode_name(frame_event.new_action_mode) << ")"
                   << "; grid=" << grid_detail(frame_event.old_grid)
                   << "->" << grid_detail(frame_event.new_grid)
                   << "; pos_holder=" << frame_vec_detail(frame_event.old_pos_holder)
                   << "->" << frame_vec_detail(frame_event.new_pos_holder)
                   << "; combatant_cur_pos_0x1c="
                   << frame_vec_detail(frame_event.old_combatant_cur_pos_0x1c)
                   << "->" << frame_vec_detail(frame_event.new_combatant_cur_pos_0x1c)
                   << "; facing_angle_0x2c="
                   << hex_seed(frame_event.old_combatant_facing_angle_0x2c)
                   << "->" << hex_seed(frame_event.new_combatant_facing_angle_0x2c)
                   << "; action_motion_position_synced="
                   << (frame_event.action_motion_position_synced ? 1 : 0);
        }
        if (!frame_event.rng_label.empty()) {
            detail << "; rng_label=" << frame_event.rng_label
                   << "; draws=" << frame_event.draws_consumed;
        }
        if (frame_event.action_motion_setup_event || frame_event.move_increment_apply_event) {
            detail << "; pos_to_move_to_0x110=" << frame_vec_detail(frame_event.pos_to_move_to_0x110)
                   << "; move_increment_0x104=" << frame_vec_detail(frame_event.move_increment_0x104)
                   << "; selected_motion_speed=" << frame_event.selected_motion_speed;
        }
        if (frame_event.move_increment_apply_event) {
            detail << "; applied_move_increment=" << frame_vec_detail(frame_event.applied_move_increment)
                   << "; motion_reached_target=" << (frame_event.motion_reached_target ? 1 : 0);
        }
        if (frame_event.visual_candidate_selected_index.has_value()) {
            detail << "; visual_candidate_selected_index="
                   << *frame_event.visual_candidate_selected_index;
        }
        if (frame_event.visual_command_kind != CombatantVisualCommandKind::Unknown
            || !frame_event.visual_resource.empty()) {
            detail << "; visual_command_kind="
                   << combatant_visual_command_kind_name(frame_event.visual_command_kind)
                   << "; visual_resource=" << frame_event.visual_resource
                   << "; visual_record_index=" << frame_event.visual_record_index
                   << "; visual_epoch=" << frame_event.visual_epoch
                   << "; visual_task_sequence=" << frame_event.visual_task_sequence
                   << "; visual_payload_mode=" << frame_event.visual_payload_mode
                   << "; visual_effective_mode=" << frame_event.visual_effective_mode
                   << "; visual_child_kind=" << frame_event.visual_child_kind;
        }
        if (!frame_event.detail.empty()) {
            detail << "; " << frame_event.detail;
        }
        const auto event_status = frame_event.status == BattleFrameEventStatus::Matched
            ? status
            : battle_event_status_from_frame_status(frame_event.status);
        std::string event_label = "worker_frame";
        if (!frame_event.rng_label.empty()) {
            event_label = frame_event.rng_label;
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::FrameStartPositionSync) {
            event_label = "frame_start_position_sync";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::MovementInvocationActivate) {
            event_label = "movement_invocation_activate";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::MovementControllerHandoff) {
            event_label = "movement_controller_handoff";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::MovementInvocationSkipped) {
            event_label = "movement_invocation_skipped";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::CombatantInstructionPublish) {
            event_label = "combatant_instruction_publish";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::CombatantInstructionWait) {
            event_label = "combatant_instruction_wait";
        } else if (frame_event.worker_kind
                == BattleFrameWorkerKind::CombatantInstruction
            && frame_event.step_kind
                == BattleFrameWorkerStepKind::ActionMotionSetup_8001fabc) {
            event_label = "combatant_instruction_motion_setup";
        } else if (frame_event.worker_kind
                == BattleFrameWorkerKind::CombatantInstruction
            && frame_event.step_kind
                == BattleFrameWorkerStepKind::ActionMotionRotateStep_8001b630_80061114) {
            event_label = "combatant_instruction_rotation";
        } else if (frame_event.worker_kind
                == BattleFrameWorkerKind::CombatantInstruction
            && (frame_event.step_kind
                    == BattleFrameWorkerStepKind::ActionMotionMoveStep_8001e910
                || frame_event.step_kind
                    == BattleFrameWorkerStepKind::MoveIncrementApply_80061340)) {
            event_label = "combatant_instruction_movement";
        } else if (frame_event.worker_kind
                == BattleFrameWorkerKind::CombatantInstruction
            && frame_event.step_kind
                == BattleFrameWorkerStepKind::MotionStopResult_8001eb54) {
            event_label = "combatant_instruction_stop";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::PassiveRelayPublish) {
            event_label = "passive_relay_publication";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::PassiveRelayAdvance) {
            event_label = "passive_relay_advance";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::PassiveDispatchPublish) {
            event_label = "passive_dispatch_publication";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::PassiveFamilySelect) {
            event_label = "passive_family_selection";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::PassiveCompletionDeferred) {
            event_label = "passive_completion_deferred";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::PassiveCompletionClear) {
            event_label = "passive_completion_clear";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::PassiveDeathClear) {
            event_label = "passive_death_clear";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::ActionComplete) {
            event_label = "action_complete";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualControllerVisit) {
            event_label = "visual_controller_visit";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualInstructionDecision) {
            event_label = "visual_instruction_decision";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualInstructionStatePublish) {
            event_label = "persistent_instruction_callback_publish";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::InstructionCallbackControlReset) {
            event_label = "instruction_callback_control_reset";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::InstructionCallbackControlResetConsume) {
            event_label = "instruction_callback_control_reset_consume";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::ActionMotionInvocationDecision) {
            event_label = "action_motion_invocation_decision";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::ActionMotionPlaybackInstall) {
            event_label = "action_motion_playback_install";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualStdRowProducerVisit) {
            event_label = "visual_std_row_producer_visit";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualInstructionInstall) {
            event_label = "visual_instruction_install";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualCommandPublish) {
            event_label = "visual_command_publish";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualChildState0) {
            event_label = "visual_child_state0";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualChildDelay) {
            event_label = "visual_child_delay";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualChildNested) {
            event_label = "visual_child_nested";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualChildCleanup) {
            event_label = "visual_child_cleanup";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualMode0Rewrite) {
            event_label = "visual_mode0_rewrite";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualMode0eCamera) {
            event_label = "visual_mode0e_camera";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualMode1Pathing) {
            event_label = "visual_mode1_pathing";
        } else if (frame_event.step_kind
            == BattleFrameWorkerStepKind::VisualUnsupportedWait) {
            event_label = "visual_unsupported_wait";
        }
        append_event(result, {
            .phase = "frame_scheduler",
            .label = event_label,
            .status = event_status,
            .actor_slot = frame_event.slot,
            .target_slot = frame_event.target_slot,
            .rng_seed_before = frame_event.rng_seed_before,
            .rng_seed_after = frame_event.rng_seed_after,
            .draws_consumed = frame_event.draws_consumed,
            .rand_value = frame_event.rand_value,
            .effect_source_key = frame_event.effect_source_key,
            .action_ordinal = frame_event.action_ordinal >= 0
                ? std::optional<int>{frame_event.action_ordinal}
                : std::nullopt,
            .frame_index = frame_event.frame_index,
            .facing_angle_0x2c = frame_event.combatant_state_available
                ? std::optional<std::uint32_t>{
                      frame_event.new_combatant_facing_angle_0x2c}
                : std::nullopt,
            .movement_worker = frame_event.callback,
            .movement_controller_family = frame_event.action_ordinal >= 0
                ? battle_movement_controller_family_name(frame_event.controller_family)
                : "",
            .movement_relation_route = frame_event.action_ordinal >= 0
                ? battle_movement_relation_route_name(frame_event.relation_route)
                : "",
            .movement_action_phase = frame_event.action_ordinal >= 0
                ? battle_frame_action_phase_name(frame_event.action_phase)
                : "",
            .movement_activation_timing = frame_event.action_ordinal >= 0
                ? battle_movement_activation_timing_name(frame_event.activation_timing)
                : "",
            .movement_thread_order_index = frame_event.thread_order_index >= 0
                ? std::optional<int>{frame_event.thread_order_index}
                : std::nullopt,
            .movement_callback_pc = frame_event.callback_pc != 0
                ? std::optional<std::uint32_t>{frame_event.callback_pc}
                : std::nullopt,
            .movement_deferred_callback_pc = frame_event.deferred_callback_pc != 0
                ? std::optional<std::uint32_t>{frame_event.deferred_callback_pc}
                : std::nullopt,
            .passive_completion_mask_before = frame_event.action_ordinal >= 0
                ? std::optional<std::uint16_t>{frame_event.passive_completion_mask_before}
                : std::nullopt,
            .passive_completion_mask_after = frame_event.action_ordinal >= 0
                ? std::optional<std::uint16_t>{frame_event.passive_completion_mask_after}
                : std::nullopt,
            .passive_completion_reason = frame_event.completion_reason,
            .visual_command_kind = frame_event.visual_command_kind
                    != CombatantVisualCommandKind::Unknown
                ? combatant_visual_command_kind_name(frame_event.visual_command_kind)
                : "",
            .visual_resource = frame_event.visual_resource,
            .visual_record_index = frame_event.visual_record_index >= 0
                ? std::optional<int>{frame_event.visual_record_index}
                : std::nullopt,
            .visual_epoch = frame_event.visual_epoch != 0
                ? std::optional<std::uint64_t>{frame_event.visual_epoch}
                : std::nullopt,
            .visual_task_sequence = frame_event.visual_task_sequence >= 0
                ? std::optional<int>{frame_event.visual_task_sequence}
                : std::nullopt,
            .visual_payload_mode = frame_event.visual_payload_mode >= 0
                ? std::optional<int>{frame_event.visual_payload_mode}
                : std::nullopt,
            .visual_effective_mode = frame_event.visual_effective_mode >= 0
                ? std::optional<int>{frame_event.visual_effective_mode}
                : std::nullopt,
            .visual_child_kind = frame_event.visual_child_kind,
            .detail = detail.str(),
        });
    }
    for (const auto& warning : frame_result.warnings) {
        append_event(result, {
            .phase = "frame_scheduler",
            .label = "frame_warning",
            .status = BattlePredictionEventStatus::Ambiguous,
            .detail = warning,
        });
    }
}

bool has_pending_frame_worker(
    const BattleFrameRuntime& runtime,
    BattleFrameWorkerKind kind,
    int slot,
    std::optional<int> effect_source_key = std::nullopt) {
    return std::any_of(
        runtime.workers.begin(),
        runtime.workers.end(),
        [kind, slot, effect_source_key](const BattleFrameWorker& worker) {
            if (worker.complete || worker.kind != kind) {
                return false;
            }
            if (slot >= 0 && worker.slot != slot) {
                return false;
            }
            if (effect_source_key.has_value()) {
                return worker.effect_source_key.has_value()
                    && *worker.effect_source_key == *effect_source_key;
            }
            return true;
        });
}

bool append_frame_until_no_pending_worker(
    BattlePredictionResult& result,
    std::uint32_t& state,
    BattleFrameRuntime& runtime,
    BattleFrameWorkerKind kind,
    int slot,
    std::optional<int> effect_source_key,
    int max_frames,
    std::string wait_label) {
    for (int frame = 0; frame < max_frames; ++frame) {
        if (!has_pending_frame_worker(runtime, kind, slot, effect_source_key)) {
            return true;
        }
        const auto frame_result = run_first_turn_frame(runtime, state);
        append_frame_scheduler_events(
            result,
            frame_result,
            frame_result.ok
                ? BattlePredictionEventStatus::Provisional
                : BattlePredictionEventStatus::Ambiguous);
        if (!frame_result.ok || frame_result.frames_executed == 0) {
            return false;
        }
    }

    if (has_pending_frame_worker(runtime, kind, slot, effect_source_key)) {
        append_event(result, {
            .phase = "frame_scheduler",
            .label = "frame_wait_cap_reached",
            .status = BattlePredictionEventStatus::Ambiguous,
            .actor_slot = slot,
            .effect_source_key = effect_source_key,
            .movement_worker = battle_frame_worker_kind_name(kind),
            .detail = "wait_label=" + wait_label
                + "; pending worker did not complete before frame cap",
        });
        return false;
    }
    return true;
}

CombatantInstructionActionKind instruction_action_kind_for(const QueuedPredictionAction& action) {
    if (action.attack) {
        return CombatantInstructionActionKind::BasicAttack;
    }
    if (action.guard) {
        return CombatantInstructionActionKind::Guard;
    }
    return CombatantInstructionActionKind::Unknown;
}

void append_instruction_mode_transition_event(
    BattlePredictionResult& result,
    const QueuedPredictionAction& action,
    const CombatantInstructionModeResult& transition) {
    BattlePredictionEvent event;
    event.phase = "instruction_mode";
    event.label = "mode_transition";
    switch (transition.status) {
    case CombatantInstructionModeStatus::Validated:
        event.status = BattlePredictionEventStatus::Exact;
        break;
    case CombatantInstructionModeStatus::Provisional:
        event.status = BattlePredictionEventStatus::Provisional;
        break;
    case CombatantInstructionModeStatus::Skipped:
        event.status = BattlePredictionEventStatus::Skipped;
        break;
    case CombatantInstructionModeStatus::MissingInput:
        event.status = BattlePredictionEventStatus::MissingInput;
        break;
    case CombatantInstructionModeStatus::Unsupported:
        event.status = BattlePredictionEventStatus::Unsupported;
        break;
    }
    event.actor_slot = action.actor_slot;
    event.target_slot = action.target_slot;
    event.initial_instr_param_0x6 = action.initial_instr_param_0x6;
    event.final_instr_param_0x6 = action.instr_param_0x6;
    event.basic_attack_execution_route =
        basic_attack_execution_route_name(action.execution_route);
    if (transition.queued_state.has_value()) {
        event.queued_std_action_state =
            static_cast<int>(*transition.queued_state);
    }
    event.instruction_mode_0x6 = transition.instruction_mode_0x6;
    event.instruction_mode_provenance = transition.provenance;
    event.detail =
        "status=" + std::string(combatant_instruction_mode_status_name(transition.status))
        + "; provenance=" + transition.provenance
        + "; " + transition.detail;
    append_event(result, std::move(event));
}

bool append_post_attack_effect_chunks_frame(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const QueuedPredictionAction& action,
    bool attack_landed,
    bool attack_was_critical,
    bool counter_follow_up,
    std::optional<int> instruction_mode_0x6,
    BattleFrameRuntime* frame_runtime) {
    if (!attack_landed) {
        return true;
    }
    if (frame_runtime == nullptr || !frame_runtime->initialized) {
        append_event(result, {
            .phase = "frame_scheduler",
            .label = "missing_effect_chunk_frame_runtime",
            .status = BattlePredictionEventStatus::MissingInput,
            .actor_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .detail = "frame-backed effect chunks need an initialized frame runtime",
        });
        return false;
    }

    const auto source_key = first_battle_basic_attack_effect_source_key(
        action.actor_slot,
        attack_was_critical,
        counter_follow_up,
        instruction_mode_0x6);
    if (!source_key.has_value()) {
        append_event(result, {
            .phase = "frame_scheduler",
            .label = "unsupported_effect_source_actor_slot",
            .status = BattlePredictionEventStatus::Unsupported,
            .actor_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .instruction_mode_0x6 = instruction_mode_0x6,
            .detail = "first-battle effect source key supports basic attacks from slots 0, 1, 4, and 5 only",
        });
        return false;
    }

    if (!schedule_effect_chunks_for_source_key(
            *frame_runtime,
            action.actor_slot,
            action.target_slot,
            *source_key)) {
        append_event(result, {
            .phase = "frame_scheduler",
            .label = "unsupported_effect_source_key",
            .status = BattlePredictionEventStatus::Unsupported,
            .actor_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .effect_source_key = source_key,
            .detail = "no first-battle effect burst chunk model is available for this source key",
        });
        return false;
    }

    return append_frame_until_no_pending_worker(
        result,
        state,
        *frame_runtime,
        BattleFrameWorkerKind::EffectChunk,
        action.actor_slot,
        *source_key,
        96,
        "effect_chunks");
}

bool append_counter_check(
    BattlePredictionResult& result,
    std::uint32_t& state,
    std::vector<BattlePredictionSlotState>& slots,
    const QueuedPredictionAction& action,
    bool critical) {
    auto* attacker = find_slot(slots, action.actor_slot);
    auto* target = find_slot(slots, action.target_slot);
    if (attacker == nullptr || target == nullptr || !target->alive) {
        return false;
    }

    const auto before = state;
    const int current_before = target->current_counter_chance;
    const auto counter = simulate_counter_check(
        state,
        CounterInputs{
            .attacker_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .target_status_flags = static_cast<int>(target->status_flags),
            .target_movement_flags = target->movement_flags,
            .target_base_counter_chance = target->base_counter_chance,
            .target_current_counter_chance = target->current_counter_chance,
            .attacker_action_marker = 0,
            .attack_was_critical = critical,
        });
    state = counter.end_state;
    target->current_counter_chance = counter.updated_current_counter_chance;

    std::ostringstream detail;
    detail << counter_result_reason_name(counter.reason)
        << "; current_counter_chance=" << current_before
        << "->" << target->current_counter_chance;
    if (counter.counter) {
        detail << "; follow-up uses setupTurnAction_80082134 self-target branch "
               << "and FUN_80081de0 forced-hit damage path";
    }

    append_event(result, {
        .phase = "counter",
        .label = counter.counter ? "counter_queued" : "counter_not_queued",
        .status = BattlePredictionEventStatus::Exact,
        .actor_slot = action.target_slot,
        .target_slot = action.actor_slot,
        .rng_seed_before = before,
        .rng_seed_after = state,
        .draws_consumed = counter.draws_consumed,
        .rand_value = counter.counter_rand,
        .detail = detail.str(),
    });
    return counter.counter;
}

void append_death_and_drop(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const soa::battle::ctx::BattleContext& context,
    const QueuedPredictionAction& action) {
    const auto* target_context = context_slot(context, action.target_slot);
    if (target_context == nullptr || target_context->is_player || !target_context->has_enemy_def) {
        return;
    }

    append_event(result, {
        .phase = "death_drop",
        .label = "death_handler",
        .status = BattlePredictionEventStatus::Exact,
        .actor_slot = action.actor_slot,
        .target_slot = action.target_slot,
        .detail = "lethal enemy damage enters death/drop path",
    });

    const auto before = state;
    const auto drop = simulate_drop_table_from_enemy_definition(state, target_context->enemy_def);
    state = drop.end_state;
    BattlePredictionEvent event;
    event.phase = "death_drop";
    event.label = drop.item_id.has_value() ? "enemy_drop" : "enemy_no_drop";
    event.status = BattlePredictionEventStatus::Exact;
    event.actor_slot = action.actor_slot;
    event.target_slot = action.target_slot;
    event.rng_seed_before = before;
    event.rng_seed_after = state;
    event.draws_consumed = drop.draws_consumed;
    event.item_id = drop.item_id;
    event.amount = drop.amount;
    event.detail = drop_detail(drop);
    append_event(result, std::move(event));
}

bool basic_attack_damage_applies(const BasicAttackSimulation& attack) {
    return attack.hit_check != 0 && attack.hit_check != 4;
}

void append_counter_skipped_lethal_damage(
    BattlePredictionResult& result,
    const QueuedPredictionAction& action,
    int hp_before,
    int damage) {
    append_event(result, {
        .phase = "counter",
        .label = "counter_skipped_lethal_damage",
        .status = BattlePredictionEventStatus::Exact,
        .actor_slot = action.target_slot,
        .target_slot = action.actor_slot,
        .damage = damage,
        .hp_before = hp_before,
        .hp_after = std::max(0, hp_before - damage),
        .detail = "performAttack skips shouldCounter when predicted target HP would be reduced to zero",
    });
}

void append_damage_application(
    BattlePredictionResult& result,
    std::vector<BattlePredictionSlotState>& slots,
    const QueuedPredictionAction& action,
    const BasicAttackSimulation& attack) {
    auto* target = find_slot(slots, action.target_slot);
    if (target == nullptr || !target->alive || !basic_attack_damage_applies(attack)) {
        return;
    }

    const int hp_before = target->current_hp;
    const int hp_after = std::max(0, hp_before - attack.damage);
    const int current_counter_before = target->current_counter_chance;
    const auto counter_increment = simulate_counter_chance_increment_after_damage({
        .hit_check = attack.hit_check,
        .current_counter_chance = target->current_counter_chance,
        .counter_chance_increment = target->counter_chance_increment,
    });
    target->current_counter_chance = counter_increment.updated_current_counter_chance;
    target->current_hp = hp_after;
    if (hp_after == 0) {
        target->alive = false;
    }

    std::ostringstream detail;
    detail << "zzDealDamage_8002dc14 damage application; zzIncreaseCounterChance_80010538 "
        << (counter_increment.incremented ? "incremented" : "did not increment")
        << " current_counter_chance=" << current_counter_before
        << "->" << target->current_counter_chance
        << "; increment=" << target->counter_chance_increment
        << "; hit_check=" << attack.hit_check;

    append_event(result, {
        .phase = "damage_application",
        .label = hp_after == 0 ? "damage_applied_lethal" : "damage_applied",
        .status = BattlePredictionEventStatus::Exact,
        .actor_slot = action.actor_slot,
        .target_slot = action.target_slot,
        .damage = attack.damage,
        .hp_before = hp_before,
        .hp_after = hp_after,
        .detail = detail.str(),
    });
}

void append_counter_follow_up(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const soa::battle::ctx::BattleContext& context,
    std::vector<BattlePredictionSlotState>& slots,
    BattleFrameRuntime* frame_runtime,
    const QueuedPredictionAction& triggering_action) {
    QueuedPredictionAction counter_action;
    counter_action.actor_slot = triggering_action.target_slot;
    counter_action.target_slot = triggering_action.actor_slot;
    counter_action.attack = true;
    counter_action.enemy_owned = counter_action.actor_slot >= 4;
    counter_action.instr_param_0x6 = 0;
    counter_action.source = "counter_follow_up";

    auto* actor = find_slot(slots, counter_action.actor_slot);
    auto* target = find_slot(slots, counter_action.target_slot);
    if (actor == nullptr || target == nullptr || !actor->alive || !target->alive) {
        append_event(result, {
            .phase = "counter_follow_up",
            .label = "skipped_dead_or_missing_actor",
            .status = BattlePredictionEventStatus::Skipped,
            .actor_slot = counter_action.actor_slot,
            .target_slot = counter_action.target_slot,
            .detail = "countering actor or target is not alive when the follow-up resolves",
        });
        return;
    }

    const auto inputs = basic_attack_inputs_for(
        context,
        slots,
        counter_action.actor_slot,
        counter_action.target_slot,
        counter_action.instr_param_0x6);
    if (!inputs.has_value()) {
        append_event(result, {
            .phase = "counter_follow_up",
            .label = "missing_input_attack_inputs",
            .status = BattlePredictionEventStatus::MissingInput,
            .actor_slot = counter_action.actor_slot,
            .target_slot = counter_action.target_slot,
            .detail = "counter follow-up damage inputs are unavailable",
        });
        return;
    }

    const auto before = state;
    const int hp_before = target->current_hp;
    const auto attack = simulate_forced_basic_attack_damage_burst(state, *inputs);
    state = attack.end_state;
    append_event(result, {
        .phase = "counter_follow_up",
        .label = "forced_hit_damage",
        .status = BattlePredictionEventStatus::Exact,
        .actor_slot = counter_action.actor_slot,
        .target_slot = counter_action.target_slot,
        .rng_seed_before = before,
        .rng_seed_after = state,
        .draws_consumed = attack.draws_consumed,
        .attack_result = attack.attack_result,
        .damage = attack.damage,
        .hp_before = hp_before,
        .hp_after = basic_attack_damage_applies(attack) ? std::max(0, hp_before - attack.damage) : hp_before,
        .detail = "FUN_80081de0 counter follow-up uses forced result helper FUN_80010b5c; "
                  "no hit draw, no crit draw, and no recursive counter roll",
    });

    append_damage_application(result, slots, counter_action, attack);
    sync_frame_runtime_from_slots(frame_runtime, slots);
    const bool target_dead = [&]() {
        const auto* updated_target = find_slot(slots, counter_action.target_slot);
        return updated_target != nullptr && !updated_target->alive;
    }();
    if (target_dead) {
        append_death_and_drop(result, state, context, counter_action);
    }

    append_post_attack_effect_chunks_frame(
        result,
        state,
        counter_action,
        attack.attack_result != 0,
        false,
        true,
        std::nullopt,
        frame_runtime);
    if (result.has_missing_input_events) {
        return;
    }

    if (target_dead) {
        return;
    }
}

void append_attack_resolution(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const soa::battle::ctx::BattleContext& context,
    std::vector<BattlePredictionSlotState>& slots,
    BattleFrameRuntime* frame_runtime,
    const QueuedPredictionAction& action) {
    auto* actor = find_slot(slots, action.actor_slot);
    auto* target = find_slot(slots, action.target_slot);
    if (actor == nullptr || target == nullptr || !actor->alive || !target->alive) {
        append_event(result, {
            .phase = "attack_resolution",
            .label = "skipped_dead_or_missing_actor",
            .status = BattlePredictionEventStatus::Skipped,
            .actor_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .detail = "actor or target is not alive at execution time",
        });
        return;
    }

    const auto inputs = basic_attack_inputs_for(
        context,
        slots,
        action.actor_slot,
        action.target_slot,
        action.instr_param_0x6);
    if (!inputs.has_value()) {
        append_event(result, {
            .phase = "attack_resolution",
            .label = "missing_input_attack_inputs",
            .status = BattlePredictionEventStatus::MissingInput,
            .actor_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .detail = "attack, hit, defense, dodge, or elemental effectiveness data is unavailable",
        });
        return;
    }

    if (frame_runtime != nullptr && frame_runtime->initialized) {
        schedule_first_turn_mechanical_attack(*frame_runtime, action.actor_slot, action.target_slot);
        const bool mechanical_ready = append_frame_until_no_pending_worker(
            result,
            state,
            *frame_runtime,
            BattleFrameWorkerKind::MechanicalAttack,
            action.actor_slot,
            std::nullopt,
            512,
            "mechanical_attack");
        if (!mechanical_ready) {
            return;
        }
        if (result.has_missing_input_events) {
            return;
        }
    }

    const auto before = state;
    const int hp_before = target->current_hp;
    const auto attack = simulate_basic_attack_burst(state, *inputs);
    state = attack.end_state;
    const bool damage_applies = basic_attack_damage_applies(attack);
    const int hp_after = damage_applies ? std::max(0, hp_before - attack.damage) : hp_before;

    BattlePredictionEvent event;
    event.phase = "attack_resolution";
    event.label = attack.attack_result == 0
        ? "attack_miss"
        : (attack.attack_result == 2 ? "attack_crit" : "attack_hit");
    event.status = result.has_ambiguous_events
        ? BattlePredictionEventStatus::Provisional
        : BattlePredictionEventStatus::Exact;
    event.actor_slot = action.actor_slot;
    event.target_slot = action.target_slot;
    event.rng_seed_before = before;
    event.rng_seed_after = state;
    event.draws_consumed = attack.draws_consumed;
    event.rand_value = attack.hit_rand;
    event.attack_result = attack.attack_result;
    event.damage = attack.damage;
    event.hp_before = hp_before;
    event.hp_after = hp_after;
    event.initial_instr_param_0x6 = action.initial_instr_param_0x6;
    event.final_instr_param_0x6 = action.instr_param_0x6;
    event.basic_attack_execution_route =
        basic_attack_execution_route_name(action.execution_route);
    event.detail = "basic attack helper; final_instr_param_0x6="
        + std::to_string(action.instr_param_0x6)
        + "; execution_route="
        + basic_attack_execution_route_name(action.execution_route);
    append_event(result, std::move(event));

    const auto instruction_mode = model_combatant_instruction_mode_transition({
        .actor_slot = action.actor_slot,
        .target_slot = action.target_slot,
        .action_kind = instruction_action_kind_for(action),
        .attack_result = attack.attack_result,
        .attack_landed = attack.attack_result != 0,
        .counter_follow_up = false,
        .queued_command_parameter = action.instr_param_0x6,
        .execution_route = action.execution_route,
    });
    append_instruction_mode_transition_event(result, action, instruction_mode);
    if (frame_runtime != nullptr && frame_runtime->initialized
        && action.frame_action_ordinal.has_value()) {
        const bool target_dead = damage_applies && hp_after == 0;
        if (!notify_first_turn_action_resolution(
                *frame_runtime,
                BattleFrameActionResolution{
                    .action_ordinal = *action.frame_action_ordinal,
                    .attack_result = attack.attack_result,
                    .attack_landed = attack.attack_result != 0,
                    .target_dead = target_dead,
                })) {
            append_event(result, {
                .phase = "frame_scheduler",
                .label = "action_resolution_notification_rejected",
                .status = BattlePredictionEventStatus::Ambiguous,
                .actor_slot = action.actor_slot,
                .target_slot = action.target_slot,
                .action_ordinal = action.frame_action_ordinal,
                .detail = "active action ordinal no longer matches attack resolution",
            });
            return;
        }
    }

    const bool target_dead = damage_applies && hp_after == 0;
    bool counter_triggered = false;
    if (damage_applies && attack.attack_result != 2) {
        if (hp_before > attack.damage) {
            counter_triggered = append_counter_check(
                result,
                state,
                slots,
                action,
                false);
        } else {
            append_counter_skipped_lethal_damage(result, action, hp_before, attack.damage);
        }
    }

    if (frame_runtime != nullptr && frame_runtime->initialized
        && action.frame_action_ordinal.has_value()) {
        const auto target_reaction = model_battle_target_reaction({
            .action_kind = BattleTargetReactionActionKind::BasicAttack,
            .origin_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .hit_check = attack.hit_check,
            .pending_damage = attack.damage,
            .target_hp_before_flush = hp_before,
            .target_dead = target_dead,
            .counter_accepted = counter_triggered,
        });
        if (!publish_first_turn_target_reaction(
                *frame_runtime,
                BattleFrameTargetReactionPublication{
                    .action_ordinal = *action.frame_action_ordinal,
                    .reaction = target_reaction,
                })) {
            append_event(result, {
                .phase = "frame_scheduler",
                .label = "target_reaction_publication_rejected",
                .status = target_reaction.status
                        == BattleTargetReactionStatus::MissingInput
                    ? BattlePredictionEventStatus::MissingInput
                    : BattlePredictionEventStatus::Ambiguous,
                .actor_slot = action.actor_slot,
                .target_slot = action.target_slot,
                .action_ordinal = action.frame_action_ordinal,
                .detail = target_reaction.provenance,
            });
            return;
        }
    }

    if (counter_triggered) {
        append_counter_follow_up(result, state, context, slots, frame_runtime, action);
        if (result.has_missing_input_events) {
            return;
        }
    }

    if (frame_runtime != nullptr && frame_runtime->initialized
        && action.frame_action_ordinal.has_value()) {
        constexpr int kVisualPublicationVisitCap = 64;
        int visits = 0;
        while (battle_frame_action_visual_publication_pending(
                   *frame_runtime, *action.frame_action_ordinal)
            && visits < kVisualPublicationVisitCap) {
            const auto visual_visit = run_first_turn_frame(*frame_runtime, state);
            ++visits;
            append_frame_scheduler_events(
                result,
                visual_visit,
                visual_visit.ok
                    ? BattlePredictionEventStatus::Provisional
                    : BattlePredictionEventStatus::Ambiguous);
            if (!visual_visit.ok) {
                return;
            }
        }
        if (battle_frame_action_visual_publication_pending(
                *frame_runtime, *action.frame_action_ordinal)) {
            append_event(result, {
                .phase = "frame_scheduler",
                .label = "action_visual_publication_frame_cap",
                .status = BattlePredictionEventStatus::Ambiguous,
                .actor_slot = action.actor_slot,
                .target_slot = action.target_slot,
                .action_ordinal = action.frame_action_ordinal,
                .detail = "queued transition, ordered action-view selector, "
                    "mode-11 gate, action-motion playback, STD-row publication, "
                    "or mode-1 pathing remained pending after "
                    + std::to_string(kVisualPublicationVisitCap)
                    + " owning-thread visits; no effect RNG was scheduled past the unresolved boundary",
            });
            return;
        }
    }

    append_damage_application(result, slots, action, attack);
    sync_frame_runtime_from_slots(frame_runtime, slots);
    if (target_dead) {
        append_death_and_drop(result, state, context, action);
    }

    if (attack.attack_result != 0) {
        append_post_attack_effect_chunks_frame(
            result,
            state,
            action,
            true,
            attack.attack_result == 2,
            false,
            instruction_mode.instruction_mode_0x6,
            frame_runtime);
        if (result.has_missing_input_events) {
            return;
        }
    }

    if (frame_runtime != nullptr && frame_runtime->initialized
        && action.frame_action_ordinal.has_value()) {
        if (!open_first_turn_action_completion(
                *frame_runtime,
                *action.frame_action_ordinal)) {
            append_event(result, {
                .phase = "frame_scheduler",
                .label = "action_completion_gate_rejected",
                .status = BattlePredictionEventStatus::Ambiguous,
                .actor_slot = action.actor_slot,
                .target_slot = action.target_slot,
                .action_ordinal = action.frame_action_ordinal,
                .detail = "could not open completion gate for active action ordinal",
            });
            return;
        }
        const auto drain = run_first_turn_action_until_complete(
            *frame_runtime,
            state,
            *action.frame_action_ordinal,
            4096);
        append_frame_scheduler_events(
            result,
            drain,
            drain.ok
                ? BattlePredictionEventStatus::Provisional
                : BattlePredictionEventStatus::Ambiguous);
        if (!drain.ok) {
            return;
        }
    }

    if (target_dead) {
        return;
    }
}

void append_action_execution(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const soa::battle::ctx::BattleContext& context,
    std::vector<BattlePredictionSlotState>& slots,
    const std::vector<QueuedPredictionAction>& actions,
    const std::vector<int>& execution_slots,
    const BattlePredictionOptions& options,
    BattleFrameRuntime* frame_runtime) {
    int last_executed_actor_slot = -1;
    int next_game_action_ordinal = 0;
    for (const int slot : execution_slots) {
        const auto* action = find_action_for_slot(actions, slot);
        if (action == nullptr) {
            append_event(result, {
                .phase = "action_execution",
                .label = "missing_input_action",
                .status = BattlePredictionEventStatus::MissingInput,
                .actor_slot = slot,
                .detail = "turn-order slot had no queued action in predictor state",
            });
            break;
        }

        QueuedPredictionAction resolved_action = *action;
        resolved_action.game_action_ordinal = next_game_action_ordinal++;
        auto* actor = find_slot(slots, action->actor_slot);
        if (actor == nullptr || !actor->alive) {
            append_event(result, {
                .phase = "action_execution",
                .label = "skipped_dead_actor",
                .status = BattlePredictionEventStatus::Skipped,
                .actor_slot = action->actor_slot,
                .target_slot = action->target_slot,
                .action_ordinal = resolved_action.game_action_ordinal,
                .detail = "actor died before its queued action",
            });
            continue;
        }

        last_executed_actor_slot = resolved_action.actor_slot;
        const bool movement_can_continue = append_movement_setup(
                result,
                state,
                context,
                slots,
                frame_runtime,
                resolved_action);
        if (result.has_missing_input_events) {
            break;
        }
        if (!movement_can_continue) {
            continue;
        }

        if (options.include_visual_rng_gap_events) {
            append_event(result, {
                .phase = "frame_scheduler",
                .label = "visual_dispatcher_owns_action_view_rng",
                .status = BattlePredictionEventStatus::Provisional,
                .actor_slot = resolved_action.actor_slot,
                .target_slot = resolved_action.target_slot,
                .action_ordinal = resolved_action.frame_action_ordinal,
                .detail = "SET COMMAND and SYSTEM CAMERA rows publish from the combatant instruction timeline",
            });
        }

        append_attack_resolution(result, state, context, slots, frame_runtime, resolved_action);
        if (result.has_missing_input_events) {
            break;
        }
        if (!has_alive_enemy(slots) || !has_alive_pc(slots)) {
            break;
        }
    }

    if (frame_runtime != nullptr && frame_runtime->initialized && !result.has_missing_input_events) {
        if (last_executed_actor_slot >= 0
            && has_alive_enemy(slots)
            && has_alive_pc(slots)) {
            schedule_first_turn_end_view_placement(
                *frame_runtime,
                last_executed_actor_slot);
        }
        const auto frame_result = run_first_turn_until_idle(*frame_runtime, state, 512);
        append_frame_scheduler_events(
            result,
            frame_result,
            frame_result.ok
                ? BattlePredictionEventStatus::Provisional
                : BattlePredictionEventStatus::Ambiguous);
    }
}

void configure_visual_dispatcher_resources(
    BattlePredictionResult& result,
    BattleFrameRuntime& runtime,
    const BattlePredictionScenario& scenario,
    const soa::battle::ctx::BattleContext& context,
    const BattlePredictionOptions& options) {
    configure_battle_frame_visual_pathing_profile(runtime, scenario.profile_name);
    for (int slot = 0; slot < soa::battle::ctx::SLOT_COUNT; ++slot) {
        const auto& combatant = context.slots_[slot];
        if (combatant.present == 0) {
            continue;
        }
        const auto identity = derive_battle_std_resource_identity(
            slot,
            combatant.is_player != 0,
            combatant.id);
        if (identity.status != BattleStdResourceIdentityStatus::Exact) {
            append_event(result, {
                .phase = "frame_scheduler",
                .label = "visual_resource_identity_missing",
                .status = identity.status == BattleStdResourceIdentityStatus::Unsupported
                    ? BattlePredictionEventStatus::Unsupported
                    : BattlePredictionEventStatus::MissingInput,
                .actor_slot = slot,
                .detail = identity.provenance,
            });
            continue;
        }
        const CombatantVisualResourceBinding binding{
            .slot = slot,
            .resource_stem = identity.stem,
        };
        CombatantVisualResource resource;
        resource.binding = binding;
        const auto std_filename = action_view_std0_companion_filename_for_std_resource(
            binding.resource_stem + ".std");
        const auto json_path = options.action_view_std_json_dir / (std_filename + ".json");
        const auto action_rows_path = options.action_view_std_json_dir
            / (binding.resource_stem + ".std.json");

        SpiceStdVisualJsonLoadResult loaded;
        SpiceStdActionRowsLoadResult action_rows;
        if (!options.action_view_std_json_dir.empty()) {
            loaded = load_spice_std_visual_resource_from_json_file(json_path);
            action_rows = load_spice_std_action_rows_from_json_file(action_rows_path);
        } else {
            loaded.errors.push_back("action-view STD JSON directory is unavailable");
            action_rows.errors.push_back("primary STD JSON directory is unavailable");
        }

        std::ostringstream detail;
        detail << "slot=" << binding.slot
               << "; resource_stem=" << binding.resource_stem
               << "; std_json=" << json_path.generic_string();
        if (loaded.ok) {
            resource = std::move(loaded.resource);
            resource.binding = binding;
            detail << "; records=" << resource.records.size()
                   << "; visual_records=" << loaded.visual_records_decoded
                   << "; complete_payload_bytes=1";
        } else {
            resource.provenance = "resource unavailable; visual dispatcher remains zero-draw";
            detail << "; resource_unavailable=1; draws=0";
            for (const auto& error : loaded.errors) {
                detail << "; error=" << error;
            }
        }
        resource.binding = binding;
        if (action_rows.ok) {
            resource.action_rows = std::move(action_rows.rows);
        }
        detail << "; action_rows=" << resource.action_rows.size();
        if (!action_rows.ok) {
            for (const auto& error : action_rows.errors) {
                detail << "; action_row_error=" << error;
            }
        }
        const bool configured = configure_battle_frame_visual_resource(
            runtime,
            std::move(resource));
        append_event(result, {
            .phase = "frame_scheduler",
            .label = loaded.ok
                ? "visual_resource_loaded"
                : "visual_resource_unavailable",
            .status = loaded.ok && action_rows.ok && configured
                ? BattlePredictionEventStatus::Exact
                : (loaded.ok && configured
                    ? BattlePredictionEventStatus::MissingInput
                    : BattlePredictionEventStatus::Provisional),
            .actor_slot = binding.slot,
            .visual_resource = binding.resource_stem,
            .detail = detail.str(),
        });
    }

    const auto publication = publish_configured_battle_frame_std_resources(
        runtime,
        "STD pending-resource producer grouped loaded combatants by canonical resource identity");
    BattlePredictionEventStatus publication_status =
        BattlePredictionEventStatus::Provisional;
    switch (publication.status) {
    case BattleFrameEventStatus::Matched:
        publication_status = BattlePredictionEventStatus::Exact;
        break;
    case BattleFrameEventStatus::Provisional:
        publication_status = BattlePredictionEventStatus::Provisional;
        break;
    case BattleFrameEventStatus::MissingInput:
        publication_status = BattlePredictionEventStatus::MissingInput;
        break;
    case BattleFrameEventStatus::Unsupported:
        publication_status = BattlePredictionEventStatus::Unsupported;
        break;
    case BattleFrameEventStatus::Ambiguous:
        publication_status = BattlePredictionEventStatus::Ambiguous;
        break;
    case BattleFrameEventStatus::Skipped:
        publication_status = BattlePredictionEventStatus::Skipped;
        break;
    }
    append_event(result, {
        .phase = "frame_scheduler",
        .label = "std_resource_thread_publication",
        .status = publication_status,
        .detail = publication.provenance,
    });
}

void add_unique_contract_violation(
    std::vector<std::string>& violations,
    std::string violation) {
    if (std::find(violations.begin(), violations.end(), violation) == violations.end()) {
        violations.push_back(std::move(violation));
    }
}

std::vector<std::string> prediction_contract_violations(
    const BattlePredictionProfile& profile,
    const BattlePredictionInput& input) {
    std::vector<std::string> violations;

    if (input.turn_index != std::optional<int>{profile.supported_turn_index}) {
        add_unique_contract_violation(
            violations,
            "turn_index must be " + std::to_string(profile.supported_turn_index));
    }
    if (input.scenario_name.has_value()) {
        const auto scenario = battle_prediction_scenario_by_name(*input.scenario_name);
        if (!scenario.has_value()) {
            add_unique_contract_violation(
                violations,
                "unsupported scenario " + *input.scenario_name);
        } else {
            const auto scenario_profile = battle_prediction_profile_by_name(scenario->profile_name);
            if (!scenario_profile.has_value() || scenario_profile->name != profile.name) {
                add_unique_contract_violation(
                    violations,
                    "scenario profile must be " + scenario->profile_name);
            }
            if (scenario->source_selection.producer_kind
                    != BattleSourceProducerKind::ScriptedBattleRequest
                || scenario->source_selection.scripted_request.script_identity.empty()
                || scenario->source_selection.scripted_request.section_identity.empty()
                || scenario->source_selection.scripted_request.instruction_payload_offset < 0) {
                add_unique_contract_violation(
                    violations,
                    "scenario must select a complete scripted battle request source");
            }
        }
    }

    return violations;
}

std::string profile_contract_detail(
    const BattlePredictionProfile& profile,
    const BattlePredictionInput& input,
    const std::vector<std::string>& violations) {
    std::ostringstream detail;
    detail << "profile=" << profile.name
           << "; scenario=" << input.scenario_name.value_or("none")
           << "; turn_index=";
    if (input.turn_index.has_value()) {
        detail << *input.turn_index;
    } else {
        detail << "missing";
    }
    if (!violations.empty()) {
        detail << "; violations=";
        for (std::size_t i = 0; i < violations.size(); ++i) {
            if (i > 0) {
                detail << " | ";
            }
            detail << violations[i];
        }
    }
    return detail.str();
}

} // namespace

BattlePredictionProfile first_battle_soldiers_prediction_profile() {
    return BattlePredictionProfile{
        .name = std::string(kFirstBattleSoldiersProfileName),
        .supported_turn_index = 1,
        .supports_status_effects = false,
        .supports_non_soldier_ai = false,
    };
}

BattlePredictionProfile first_battle_prediction_profile() {
    return first_battle_soldiers_prediction_profile();
}

std::optional<BattlePredictionProfile> battle_prediction_profile_by_name(std::string_view name) {
    if (is_first_battle_soldiers_profile_name(name)) {
        return first_battle_soldiers_prediction_profile();
    }
    return std::nullopt;
}

BattlePredictionResult predict_battle(const BattlePredictionInput& input) {
    BattlePredictionResult result;
    const auto resolved_profile = battle_prediction_profile_by_name(input.profile.name);
    result.profile = resolved_profile.value_or(input.profile);
    result.scenario_name = input.scenario_name;
    result.turn_index = input.turn_index;
    result.starting_rng_seed = input.starting_rng_seed;
    result.start_boundary = input.start_boundary;
    result.final_rng_seed = input.starting_rng_seed;

    if (!resolved_profile.has_value()) {
        result.outcome = BattlePredictionOutcome::Unsupported;
        result.errors.push_back("unsupported battle prediction profile: " + input.profile.name);
        append_validation_statuses(result);
        return result;
    }

    const auto& profile = *resolved_profile;

    const auto contract_violations = prediction_contract_violations(profile, input);
    if (!contract_violations.empty()) {
        const auto detail = profile_contract_detail(profile, input, contract_violations);
        if (!input.options.allow_profile_overrides) {
            append_event(result, {
                .phase = "profile",
                .label = "profile_contract_rejected",
                .status = BattlePredictionEventStatus::Unsupported,
                .detail = detail,
            });
            result.errors.push_back("prediction input violates " + profile.name + " profile contract");
            result.outcome = BattlePredictionOutcome::Unsupported;
            append_validation_statuses(result);
            return result;
        }
        append_event(result, {
            .phase = "profile",
            .label = "profile_contract_override",
            .status = BattlePredictionEventStatus::Provisional,
            .detail = detail,
        });
        result.warnings.push_back("prediction is using explicit profile overrides: " + detail);
    } else {
        append_event(result, {
            .phase = "profile",
            .label = "profile_contract_validated",
            .status = BattlePredictionEventStatus::Exact,
            .detail = profile_contract_detail(profile, input, contract_violations),
        });
    }

    std::optional<BattlePredictionScenario> resolved_scenario;
    if (input.scenario_name.has_value()) {
        resolved_scenario = battle_prediction_scenario_by_name(*input.scenario_name);
    } else {
        resolved_scenario = battle_prediction_scenario_by_name(profile.name);
    }

    auto slots = make_initial_slots(profile, input.context, result.warnings);
    auto state = input.starting_rng_seed;
    auto finalize = [&]() {
        result.final_rng_seed = state;
        result.final_slots = std::move(slots);

        if (result.has_missing_input_events) {
            result.outcome = BattlePredictionOutcome::MissingInput;
        } else if (result.has_unsupported_events || !result.errors.empty()) {
            result.outcome = BattlePredictionOutcome::Unsupported;
        } else if (result.has_ambiguous_events) {
            result.outcome = BattlePredictionOutcome::Ambiguous;
        } else if (result.has_provisional_events) {
            result.outcome = BattlePredictionOutcome::Provisional;
        } else if (!has_alive_pc(result.final_slots)) {
            result.outcome = BattlePredictionOutcome::Defeat;
        } else if (!has_alive_enemy(result.final_slots)) {
            result.outcome = BattlePredictionOutcome::Victory;
        } else {
            result.outcome = BattlePredictionOutcome::ReachedNextTurn;
        }

        append_validation_statuses(result);
        return result;
    };

    if (is_first_battle_soldiers_profile_name(profile.name)
        && input.turn_plan.fake_attack_count != 0) {
        append_event(result, {
            .phase = "profile",
            .label = "fake_attack_input_unsupported",
            .status = BattlePredictionEventStatus::Unsupported,
            .detail = "first-battle-soldiers accepts only fake_attack_count=0; "
                "macro-derived fake attacks are not a supported predictor input",
        });
        return finalize();
    }

    if (!resolved_scenario.has_value()) {
        append_event(result, {
            .phase = "source",
            .label = "source_scenario_missing",
            .status = BattlePredictionEventStatus::MissingInput,
            .detail = "prediction requires an operational scenario or profile default",
        });
        return finalize();
    }

    const auto source_selection = input.source_selection.value_or(
        resolved_scenario->source_selection);
    const auto source_resolution = resolve_battle_source_bundle(
        source_selection,
        input.options.battle_source_manifest_root);
    const auto source_resolution_event_status = [&]() {
        switch (source_resolution.status) {
        case BattleSourceResolutionStatus::Exact:
            return BattlePredictionEventStatus::Exact;
        case BattleSourceResolutionStatus::Provisional:
            return BattlePredictionEventStatus::Provisional;
        case BattleSourceResolutionStatus::MissingInput:
            return BattlePredictionEventStatus::MissingInput;
        case BattleSourceResolutionStatus::Unsupported:
            return BattlePredictionEventStatus::Unsupported;
        case BattleSourceResolutionStatus::Ambiguous:
            return BattlePredictionEventStatus::Ambiguous;
        case BattleSourceResolutionStatus::Invalid:
            return BattlePredictionEventStatus::MissingInput;
        }
        return BattlePredictionEventStatus::MissingInput;
    }();
    std::ostringstream source_resolution_detail;
    source_resolution_detail
        << "source_producer="
        << battle_source_producer_kind_name(source_selection.producer_kind);
    if (source_selection.producer_kind
        == BattleSourceProducerKind::ScriptedBattleRequest) {
        source_resolution_detail
            << "; script=" << source_selection.scripted_request.script_identity
            << "; section=" << source_selection.scripted_request.section_identity
            << "; payload_offset="
            << source_selection.scripted_request.instruction_payload_offset;
    }
    if (!source_resolution.bundle.manifest.key.empty()) {
        source_resolution_detail
            << "; manifest_key=" << source_resolution.bundle.manifest.key
            << "; requested_encounter_id="
            << source_resolution.bundle.snapshot.requested_encounter_id
            << "; effective_encounter_id="
            << source_resolution.bundle.snapshot.encounter_id
            << "; stage=" << source_resolution.bundle.snapshot.stage_id
            << "; sst_record=" << source_resolution.bundle.snapshot.sst_record_index;
        if (source_resolution.bundle.manifest.scripted_battle_request.has_value()) {
            const auto& request =
                *source_resolution.bundle.manifest.scripted_battle_request;
            source_resolution_detail
                << "; event_mode=" << request.event_mode
                << "; event_or_encounter_id=" << request.event_or_encounter_id
                << "; stage_id=" << request.stage_id
                << "; transition_selector=" << request.transition_selector;
        }
    }
    if (!source_resolution.provenance.empty()) {
        source_resolution_detail << "; provenance=" << source_resolution.provenance;
    }
    for (const auto& error : source_resolution.errors) {
        source_resolution_detail << "; error=" << error;
    }
    append_event(result, {
        .phase = "source",
        .label = "source_catalog_resolution",
        .status = source_resolution_event_status,
        .detail = source_resolution_detail.str(),
    });
    if (source_resolution.status != BattleSourceResolutionStatus::Exact
        && source_resolution.status != BattleSourceResolutionStatus::Provisional) {
        return finalize();
    }

    const auto& source_bundle = source_resolution.bundle;
    auto source_validation_input = input.source_validation;
    source_validation_input.source_selection = source_selection;
    const auto source_validation = validate_battle_source_bundle(
        source_bundle,
        input.context,
        source_validation_input);
    if (!source_bundle.ok || !source_validation.ok) {
        std::ostringstream detail;
        detail << "manifest_key=" << source_bundle.manifest.key;
        for (const auto& error : source_bundle.errors) {
            detail << "; error=" << error;
        }
        for (const auto& error : source_validation.errors) {
            detail << "; error=" << error;
        }
        append_event(result, {
            .phase = "source",
            .label = "source_manifest_validation_failed",
            .status = BattlePredictionEventStatus::MissingInput,
            .detail = detail.str(),
        });
        return finalize();
    }
    result.source_manifest_key = source_bundle.manifest.key;
    result.source_manifest_sha256 = source_bundle.manifest_sha256;
    result.source_producer_kind = source_bundle.manifest.producer_kind;
    result.scripted_battle_request = source_bundle.manifest.scripted_battle_request;
    result.encounter_source_kind = source_bundle.snapshot.encounter_source_kind;
    result.encounter_id = source_bundle.snapshot.encounter_id;
    const std::string savestate_provenance =
        !source_validation.savestate_fingerprint_provided
        ? "not_provided"
        : source_validation.savestate_fingerprint_recognized
            ? "recognized"
            : "unrecognized_diagnostic_only";
    append_event(result, {
        .phase = "source",
        .label = "source_manifest_validated",
        .status = BattlePredictionEventStatus::Exact,
        .detail = "manifest_key=" + source_bundle.manifest.key
            + "; roster=" + source_validation.observed_roster_fingerprint
            + "; producer="
            + battle_source_producer_kind_name(source_bundle.manifest.producer_kind)
            + "; encounter=" + source_bundle.manifest.encounter_identity
            + "; requested_encounter_id="
            + std::to_string(source_bundle.snapshot.requested_encounter_id)
            + "; effective_encounter_id="
            + std::to_string(source_bundle.snapshot.encounter_id)
            + "; stage=" + source_bundle.manifest.stage_identity
            + "; snapshot_sha256=" + source_bundle.manifest.snapshot_sha256
            + "; savestate_provenance=" + savestate_provenance,
    });

    const auto initial_turn_type = resolve_prediction_initial_turn_type(
        result,
        state,
        input,
        source_bundle.snapshot);
    if (!initial_turn_type.has_value()) {
        return finalize();
    }

    if (!apply_source_start_positions(result, source_bundle.snapshot, slots)) {
        return finalize();
    }

    bool footprint_missing = false;
    for (const auto& slot : slots) {
        if (!slot.present) {
            continue;
        }
        const auto* source = context_slot(input.context, slot.slot);
        if (source == nullptr
            || source->instance.width == 0
            || source->instance.depth == 0) {
            footprint_missing = true;
            append_event(result, {
                .phase = "initial_state",
                .label = "combatant_footprint_missing",
                .status = BattlePredictionEventStatus::MissingInput,
                .actor_slot = slot.slot,
                .detail = "present combatant requires nonzero BattleContext width and depth; "
                    "the predictor no longer substitutes a 1x1 footprint",
            });
        }
    }
    if (footprint_missing) {
        return finalize();
    }

    auto frame_runtime = initialize_first_battle_frame_runtime(
        source_bundle.snapshot.encounter_id,
        movement_slots_from_prediction_slots(slots, input.context),
        source_bundle.snapshot.terrain_source_9x9,
        *initial_turn_type);
    if (frame_runtime.has_value()) {
        configure_visual_dispatcher_resources(
            result,
            *frame_runtime,
            *resolved_scenario,
            input.context,
            input.options);
        append_event(result, {
            .phase = "frame_scheduler",
            .label = "runtime_initialized",
            .status = BattlePredictionEventStatus::Provisional,
            .frame_index = frame_runtime->state.frame_index,
            .turn_type = static_cast<int>(*initial_turn_type),
            .detail = "persistent first-battle frame runtime initialized; "
                "initial_turn_type="
                + std::string(battle_turn_type_name(*initial_turn_type)),
        });
        for (const auto& combatant : frame_runtime->state.combatants) {
            append_event(result, {
                .phase = input.start_boundary
                        == BattlePredictionStartBoundary::BattleCoordinatorStart
                    ? "battle_coordinator"
                    : "initial_state",
                .label = "initial_facing_seeded",
                .status = BattlePredictionEventStatus::Exact,
                .actor_slot = combatant.slot,
                .turn_type = static_cast<int>(*initial_turn_type),
                .facing_angle_0x2c = combatant.combatant_facing_angle_0x2c,
                .detail = "formation facing copied into CombatantWorksheet+0x2C; "
                    "side="
                    + std::string(combatant.is_player ? "player" : "enemy"),
            });
        }
    } else {
        append_event(result, {
            .phase = "frame_scheduler",
            .label = "runtime_init_failed",
            .status = BattlePredictionEventStatus::MissingInput,
            .detail = "frame scheduler could not initialize from the validated source snapshot",
        });
        return finalize();
    }

    const int pc_count = present_alive_pc_count(slots);
    append_pre_ai_events(
        result,
        state,
        static_cast<int>(input.turn_plan.fake_attack_count),
        pc_count);
    if (result.has_missing_input_events) {
        return finalize();
    }

    std::vector<QueuedPredictionAction> actions;
    append_player_commands(result, input.turn_plan, slots, actions);
    if (result.has_missing_input_events) {
        return finalize();
    }
    append_enemy_ai(result, state, profile, slots, actions);
    if (result.has_missing_input_events) {
        return finalize();
    }

    std::vector<int> execution_slots;
    append_turn_order(result, state, slots, actions, execution_slots);
    if (result.has_missing_input_events) {
        return finalize();
    }
    append_action_execution(
        result,
        state,
        input.context,
        slots,
        actions,
        execution_slots,
        input.options,
        frame_runtime.has_value() ? &*frame_runtime : nullptr);
    return finalize();
}

const char* battle_prediction_outcome_name(BattlePredictionOutcome outcome) {
    switch (outcome) {
    case BattlePredictionOutcome::ReachedNextTurn: return "ReachedNextTurn";
    case BattlePredictionOutcome::Victory: return "Victory";
    case BattlePredictionOutcome::Defeat: return "Defeat";
    case BattlePredictionOutcome::MissingInput: return "MissingInput";
    case BattlePredictionOutcome::Unsupported: return "Unsupported";
    case BattlePredictionOutcome::Ambiguous: return "Ambiguous";
    case BattlePredictionOutcome::Provisional: return "Provisional";
    }
    return "Unsupported";
}

const char* battle_prediction_event_status_name(BattlePredictionEventStatus status) {
    switch (status) {
    case BattlePredictionEventStatus::Exact: return "Exact";
    case BattlePredictionEventStatus::Provisional: return "Provisional";
    case BattlePredictionEventStatus::Skipped: return "Skipped";
    case BattlePredictionEventStatus::MissingInput: return "MissingInput";
    case BattlePredictionEventStatus::Unsupported: return "Unsupported";
    case BattlePredictionEventStatus::Ambiguous: return "Ambiguous";
    }
    return "Unsupported";
}

const char* battle_prediction_validation_status_name(BattlePredictionValidationStatus status) {
    switch (status) {
    case BattlePredictionValidationStatus::Exact: return "Exact";
    case BattlePredictionValidationStatus::Validated: return "Validated";
    case BattlePredictionValidationStatus::Provisional: return "Provisional";
    case BattlePredictionValidationStatus::NotExercised: return "NotExercised";
    case BattlePredictionValidationStatus::MissingInput: return "MissingInput";
    case BattlePredictionValidationStatus::Unsupported: return "Unsupported";
    case BattlePredictionValidationStatus::Ambiguous: return "Ambiguous";
    }
    return "Unsupported";
}

const char* battle_prediction_start_boundary_name(BattlePredictionStartBoundary boundary) {
    switch (boundary) {
    case BattlePredictionStartBoundary::CapturedTurnStart:
        return "captured_turn_start";
    case BattlePredictionStartBoundary::BattleCoordinatorStart:
        return "battle_coordinator_start";
    }
    return "captured_turn_start";
}

void write_battle_prediction_text(const BattlePredictionResult& result, std::ostream& out) {
    out << "SavorPredict predict-battle\n";
    out << "  profile: " << result.profile.name << "\n";
    if (result.scenario_name.has_value()) {
        out << "  scenario: " << *result.scenario_name << "\n";
    }
    if (result.turn_index.has_value()) {
        out << "  turn_index: " << *result.turn_index << "\n";
    }
    out << "  start_boundary: "
        << battle_prediction_start_boundary_name(result.start_boundary) << "\n";
    out << "  outcome: " << battle_prediction_outcome_name(result.outcome) << "\n";
    out << "  start_seed: " << result.starting_rng_seed << " ("
        << hex_seed(result.starting_rng_seed) << ")\n";
    if (result.source_producer_kind.has_value()) {
        out << "  source_producer: "
            << battle_source_producer_kind_name(*result.source_producer_kind)
            << "\n";
    }
    if (result.scripted_battle_request.has_value()) {
        const auto& request = *result.scripted_battle_request;
        out << "  scripted_battle_request: "
            << request.identity.script_identity << '/'
            << request.identity.section_identity
            << " payload=" << request.identity.instruction_payload_offset
            << " operands={" << request.event_mode << ','
            << request.event_or_encounter_id << ',' << request.stage_id << ','
            << request.transition_selector << "}\n";
    }
    if (result.encounter_source_kind.has_value()) {
        out << "  encounter_source: "
            << battle_encounter_source_kind_name(*result.encounter_source_kind)
            << "\n";
    }
    if (result.encounter_id.has_value()) {
        out << "  encounter_id: " << *result.encounter_id << "\n";
    }
    if (result.initial_turn_type.has_value()) {
        out << "  initial_turn_type: "
            << battle_turn_type_name(*result.initial_turn_type)
            << " (" << static_cast<int>(*result.initial_turn_type) << ")\n";
    }
    out << "  final_seed: " << result.final_rng_seed << " ("
        << hex_seed(result.final_rng_seed) << ")\n";
    out << "  total_draws_consumed: " << result.total_draws_consumed << "\n";
    out << "  exact_through_turn_order: " << (result.exact_through_turn_order ? "true" : "false") << "\n";
    out << "  exact_draws_through_turn_order: " << result.exact_draws_through_turn_order << "\n";
    out << "  has_missing_input_events: " << (result.has_missing_input_events ? "true" : "false") << "\n";
    out << "  has_provisional_events: " << (result.has_provisional_events ? "true" : "false") << "\n";
    out << "  has_ambiguous_events: " << (result.has_ambiguous_events ? "true" : "false") << "\n";
    out << "  has_unsupported_events: " << (result.has_unsupported_events ? "true" : "false") << "\n";
    if (!result.validation.empty()) {
        out << "  validation:\n";
        for (const auto& item : result.validation) {
            out << "    - " << item.scope
                << ": " << battle_prediction_validation_status_name(item.status);
            if (item.draws_exact_through >= 0) {
                out << " draws_exact_through=" << item.draws_exact_through;
            }
            if (!item.detail.empty()) {
                out << " - " << item.detail;
            }
            out << "\n";
        }
    }
    if (!result.warnings.empty()) {
        out << "  warnings:\n";
        for (const auto& warning : result.warnings) {
            out << "    - " << warning << "\n";
        }
    }
    if (!result.errors.empty()) {
        out << "  errors:\n";
        for (const auto& error : result.errors) {
            out << "    - " << error << "\n";
        }
    }

    out << "\nEvents:\n";
    for (const auto& event : result.events) {
        out << "  [" << event.sequence << "] "
            << event.phase << "." << event.label
            << " status=" << battle_prediction_event_status_name(event.status)
            << " draws=" << event.draws_consumed;
        if (event.actor_slot >= 0) {
            out << " actor=" << event.actor_slot;
        }
        if (event.target_slot >= 0) {
            out << " target=" << event.target_slot;
        }
        if (event.rng_seed_before.has_value()) {
            out << " rng_before=" << hex_seed(*event.rng_seed_before);
        }
        if (event.rng_seed_after.has_value()) {
            out << " rng_after=" << hex_seed(*event.rng_seed_after);
        }
        if (event.damage.has_value()) {
            out << " damage=" << *event.damage;
        }
        if (event.hp_before.has_value() && event.hp_after.has_value()) {
            out << " hp=" << *event.hp_before << "->" << *event.hp_after;
        }
        if (event.effect_source_key.has_value()) {
            out << " effect_source_key=" << *event.effect_source_key;
        }
        if (event.instr_param_0x6.has_value()) {
            out << " instr_param_0x6=" << *event.instr_param_0x6;
        }
        if (event.initial_instr_param_0x6.has_value()) {
            out << " initial_instr_param_0x6="
                << *event.initial_instr_param_0x6;
        }
        if (event.final_instr_param_0x6.has_value()) {
            out << " final_instr_param_0x6="
                << *event.final_instr_param_0x6;
        }
        if (!event.basic_attack_execution_route.empty()) {
            out << " basic_attack_execution_route="
                << event.basic_attack_execution_route;
        }
        if (event.queued_std_action_state.has_value()) {
            out << " queued_std_action_state="
                << *event.queued_std_action_state;
        }
        if (event.instruction_mode_0x6.has_value()) {
            out << " instruction_mode_0x6=" << *event.instruction_mode_0x6;
        }
        if (event.movement_reachability.has_value()) {
            out << " movement_reachability=" << *event.movement_reachability;
        }
        if (!event.movement_worker.empty()) {
            out << " movement_worker=" << event.movement_worker;
        }
        if (!event.movement_controller_family.empty()) {
            out << " movement_controller_family="
                << event.movement_controller_family;
        }
        if (!event.movement_relation_route.empty()) {
            out << " movement_relation_route=" << event.movement_relation_route;
        }
        if (!event.movement_action_phase.empty()) {
            out << " movement_action_phase=" << event.movement_action_phase;
        }
        if (!event.movement_activation_timing.empty()) {
            out << " movement_activation_timing="
                << event.movement_activation_timing;
        }
        if (event.item_id.has_value()) {
            out << " item_id=" << *event.item_id
                << " amount=" << event.amount.value_or(0);
        }
        if (event.queue_index.has_value()) {
            out << " queue_index=" << *event.queue_index;
        }
        if (event.quick.has_value()) {
            out << " quick=" << *event.quick;
        }
        if (event.fixed_priority_result.has_value()) {
            out << " fixed_priority_result=" << *event.fixed_priority_result;
        }
        if (event.jitter_modulus.has_value()) {
            out << " jitter_modulus=" << *event.jitter_modulus;
        }
        if (event.assigned_priority.has_value()) {
            out << " assigned_priority=" << *event.assigned_priority;
        }
        if (event.qsort_index.has_value()) {
            out << " qsort_index=" << *event.qsort_index;
        }
        if (event.execution_index.has_value()) {
            out << " execution_index=" << *event.execution_index;
        }
        if (event.action_ordinal.has_value()) {
            out << " action_ordinal=" << *event.action_ordinal;
        }
        if (event.frame_index.has_value()) {
            out << " frame_index=" << *event.frame_index;
        }
        if (event.turn_type.has_value()) {
            out << " turn_type=" << *event.turn_type;
        }
        if (event.facing_angle_0x2c.has_value()) {
            out << " facing_angle_0x2c=" << hex_seed(*event.facing_angle_0x2c);
        }
        if (event.movement_thread_order_index.has_value()) {
            out << " movement_thread_order_index="
                << *event.movement_thread_order_index;
        }
        if (event.movement_callback_pc.has_value()) {
            out << " movement_callback_pc=" << hex_seed(*event.movement_callback_pc);
        }
        if (event.movement_deferred_callback_pc.has_value()) {
            out << " movement_deferred_callback_pc="
                << hex_seed(*event.movement_deferred_callback_pc);
        }
        if (event.passive_completion_mask_before.has_value()) {
            out << " passive_completion_mask=0x" << std::hex
                << *event.passive_completion_mask_before << "->0x"
                << event.passive_completion_mask_after.value_or(
                    *event.passive_completion_mask_before) << std::dec;
        }
        if (!event.passive_completion_reason.empty()) {
            out << " passive_completion_reason=" << event.passive_completion_reason;
        }
        if (!event.visual_command_kind.empty()) {
            out << " visual_command_kind=" << event.visual_command_kind;
        }
        if (!event.visual_resource.empty()) {
            out << " visual_resource=" << event.visual_resource;
        }
        if (event.visual_record_index.has_value()) {
            out << " visual_record_index=" << *event.visual_record_index;
        }
        if (event.visual_epoch.has_value()) {
            out << " visual_epoch=" << *event.visual_epoch;
        }
        if (event.visual_task_sequence.has_value()) {
            out << " visual_task_sequence=" << *event.visual_task_sequence;
        }
        if (event.visual_payload_mode.has_value()) {
            out << " visual_payload_mode=" << *event.visual_payload_mode;
        }
        if (event.visual_effective_mode.has_value()) {
            out << " visual_effective_mode=" << *event.visual_effective_mode;
        }
        if (!event.visual_child_kind.empty()) {
            out << " visual_child_kind=" << event.visual_child_kind;
        }
        if (event.pathing_accepted_candidates.has_value()) {
            out << " pathing_accepted_candidates=" << *event.pathing_accepted_candidates;
        }
        if (event.pathing_aggregate_score.has_value()) {
            out << " pathing_aggregate_score=" << *event.pathing_aggregate_score;
        }
        if (!event.instruction_mode_provenance.empty()) {
            out << " instruction_mode_provenance=" << event.instruction_mode_provenance;
        }
        if (!event.detail.empty()) {
            out << " detail=\"" << event.detail << "\"";
        }
        out << "\n";
    }
}

void write_battle_prediction_json(const BattlePredictionResult& result, std::ostream& out) {
    out << "{\n";
    out << "  \"profile\": \"" << json_escape(result.profile.name) << "\",\n";
    if (result.scenario_name.has_value()) {
        out << "  \"scenario\": \"" << json_escape(*result.scenario_name) << "\",\n";
    }
    if (result.turn_index.has_value()) {
        out << "  \"turn_index\": " << *result.turn_index << ",\n";
    }
    out << "  \"start_boundary\": \""
        << battle_prediction_start_boundary_name(result.start_boundary) << "\",\n";
    out << "  \"outcome\": \"" << battle_prediction_outcome_name(result.outcome) << "\",\n";
    out << "  \"starting_rng_seed\": " << result.starting_rng_seed << ",\n";
    out << "  \"starting_rng_seed_hex\": \"" << hex_seed(result.starting_rng_seed) << "\",\n";
    if (result.source_producer_kind.has_value()) {
        out << "  \"source_producer\": \""
            << battle_source_producer_kind_name(*result.source_producer_kind)
            << "\",\n";
    }
    if (result.scripted_battle_request.has_value()) {
        const auto& request = *result.scripted_battle_request;
        out << "  \"source_script\": \""
            << json_escape(request.identity.script_identity) << "\",\n";
        out << "  \"source_section\": \""
            << json_escape(request.identity.section_identity) << "\",\n";
        out << "  \"source_instruction_offset\": "
            << request.instruction_offset << ",\n";
        out << "  \"source_payload_offset\": "
            << request.identity.instruction_payload_offset << ",\n";
        out << "  \"source_event_mode\": " << request.event_mode << ",\n";
        out << "  \"source_event_or_encounter_id\": "
            << request.event_or_encounter_id << ",\n";
        out << "  \"source_stage_id\": " << request.stage_id << ",\n";
        out << "  \"source_transition_selector\": "
            << request.transition_selector << ",\n";
    }
    if (result.encounter_source_kind.has_value()) {
        out << "  \"encounter_source\": \""
            << battle_encounter_source_kind_name(*result.encounter_source_kind)
            << "\",\n";
    }
    if (result.encounter_id.has_value()) {
        out << "  \"encounter_id\": " << *result.encounter_id << ",\n";
    }
    if (result.initial_turn_type.has_value()) {
        out << "  \"initial_turn_type\": "
            << static_cast<int>(*result.initial_turn_type) << ",\n";
        out << "  \"initial_turn_type_name\": \""
            << battle_turn_type_name(*result.initial_turn_type) << "\",\n";
    }
    out << "  \"final_rng_seed\": " << result.final_rng_seed << ",\n";
    out << "  \"final_rng_seed_hex\": \"" << hex_seed(result.final_rng_seed) << "\",\n";
    out << "  \"total_draws_consumed\": " << result.total_draws_consumed << ",\n";
    out << "  \"exact_through_turn_order\": " << (result.exact_through_turn_order ? "true" : "false") << ",\n";
    out << "  \"exact_draws_through_turn_order\": " << result.exact_draws_through_turn_order << ",\n";
    out << "  \"has_missing_input_events\": " << (result.has_missing_input_events ? "true" : "false") << ",\n";
    out << "  \"has_provisional_events\": " << (result.has_provisional_events ? "true" : "false") << ",\n";
    out << "  \"has_ambiguous_events\": " << (result.has_ambiguous_events ? "true" : "false") << ",\n";
    out << "  \"has_unsupported_events\": " << (result.has_unsupported_events ? "true" : "false") << ",\n";
    out << "  \"validation\": [\n";
    for (std::size_t i = 0; i < result.validation.size(); ++i) {
        const auto& item = result.validation[i];
        out << "    {";
        out << "\"scope\": \"" << json_escape(item.scope) << "\"";
        out << ", \"status\": \"" << battle_prediction_validation_status_name(item.status) << "\"";
        if (item.draws_exact_through >= 0) {
            out << ", \"draws_exact_through\": " << item.draws_exact_through;
        }
        out << ", \"detail\": \"" << json_escape(item.detail) << "\"";
        out << "}";
        if (i + 1 != result.validation.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"warnings\": [";
    for (std::size_t i = 0; i < result.warnings.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "\"" << json_escape(result.warnings[i]) << "\"";
    }
    out << "],\n";
    out << "  \"errors\": [";
    for (std::size_t i = 0; i < result.errors.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "\"" << json_escape(result.errors[i]) << "\"";
    }
    out << "],\n";
    out << "  \"events\": [\n";
    for (std::size_t i = 0; i < result.events.size(); ++i) {
        const auto& event = result.events[i];
        out << "    {";
        out << "\"sequence\": " << event.sequence;
        out << ", \"phase\": \"" << json_escape(event.phase) << "\"";
        out << ", \"label\": \"" << json_escape(event.label) << "\"";
        out << ", \"status\": \"" << battle_prediction_event_status_name(event.status) << "\"";
        out << ", \"actor_slot\": " << event.actor_slot;
        out << ", \"target_slot\": " << event.target_slot;
        out << ", \"draws_consumed\": " << event.draws_consumed;
        if (event.rng_seed_before.has_value()) {
            out << ", \"rng_seed_before\": " << *event.rng_seed_before;
            out << ", \"rng_seed_before_hex\": \"" << hex_seed(*event.rng_seed_before) << "\"";
        }
        if (event.rng_seed_after.has_value()) {
            out << ", \"rng_seed_after\": " << *event.rng_seed_after;
            out << ", \"rng_seed_after_hex\": \"" << hex_seed(*event.rng_seed_after) << "\"";
        }
        if (event.rand_value.has_value()) {
            out << ", \"rand_value\": " << *event.rand_value;
        }
        if (event.turn_type.has_value()) {
            out << ", \"turn_type\": " << *event.turn_type;
        }
        if (event.facing_angle_0x2c.has_value()) {
            out << ", \"facing_angle_0x2c\": " << *event.facing_angle_0x2c;
            out << ", \"facing_angle_0x2c_hex\": \""
                << hex_seed(*event.facing_angle_0x2c) << "\"";
        }
        if (event.attack_result.has_value()) {
            out << ", \"attack_result\": " << *event.attack_result;
        }
        if (event.damage.has_value()) {
            out << ", \"damage\": " << *event.damage;
        }
        if (event.hp_before.has_value()) {
            out << ", \"hp_before\": " << *event.hp_before;
        }
        if (event.hp_after.has_value()) {
            out << ", \"hp_after\": " << *event.hp_after;
        }
        if (event.effect_source_key.has_value()) {
            out << ", \"effect_source_key\": " << *event.effect_source_key;
        }
        if (event.instr_param_0x6.has_value()) {
            out << ", \"instr_param_0x6\": " << *event.instr_param_0x6;
        }
        if (event.initial_instr_param_0x6.has_value()) {
            out << ", \"initial_instr_param_0x6\": "
                << *event.initial_instr_param_0x6;
        }
        if (event.final_instr_param_0x6.has_value()) {
            out << ", \"final_instr_param_0x6\": "
                << *event.final_instr_param_0x6;
        }
        if (!event.basic_attack_execution_route.empty()) {
            out << ", \"basic_attack_execution_route\": \""
                << json_escape(event.basic_attack_execution_route) << "\"";
        }
        if (event.queued_std_action_state.has_value()) {
            out << ", \"queued_std_action_state\": "
                << *event.queued_std_action_state;
        }
        if (event.instruction_mode_0x6.has_value()) {
            out << ", \"instruction_mode_0x6\": " << *event.instruction_mode_0x6;
        }
        if (event.movement_reachability.has_value()) {
            out << ", \"movement_reachability\": " << *event.movement_reachability;
        }
        if (!event.movement_worker.empty()) {
            out << ", \"movement_worker\": \"" << json_escape(event.movement_worker) << "\"";
        }
        if (!event.movement_controller_family.empty()) {
            out << ", \"movement_controller_family\": \""
                << json_escape(event.movement_controller_family) << "\"";
        }
        if (!event.movement_relation_route.empty()) {
            out << ", \"movement_relation_route\": \""
                << json_escape(event.movement_relation_route) << "\"";
        }
        if (!event.movement_action_phase.empty()) {
            out << ", \"movement_action_phase\": \""
                << json_escape(event.movement_action_phase) << "\"";
        }
        if (!event.movement_activation_timing.empty()) {
            out << ", \"movement_activation_timing\": \""
                << json_escape(event.movement_activation_timing) << "\"";
        }
        if (event.item_id.has_value()) {
            out << ", \"item_id\": " << *event.item_id;
        }
        if (event.amount.has_value()) {
            out << ", \"amount\": " << *event.amount;
        }
        if (event.queue_index.has_value()) {
            out << ", \"queue_index\": " << *event.queue_index;
        }
        if (event.quick.has_value()) {
            out << ", \"quick\": " << *event.quick;
        }
        if (event.fixed_priority_result.has_value()) {
            out << ", \"fixed_priority_result\": " << *event.fixed_priority_result;
        }
        if (event.jitter_modulus.has_value()) {
            out << ", \"jitter_modulus\": " << *event.jitter_modulus;
        }
        if (event.assigned_priority.has_value()) {
            out << ", \"assigned_priority\": " << *event.assigned_priority;
        }
        if (event.qsort_index.has_value()) {
            out << ", \"qsort_index\": " << *event.qsort_index;
        }
        if (event.execution_index.has_value()) {
            out << ", \"execution_index\": " << *event.execution_index;
        }
        if (event.action_ordinal.has_value()) {
            out << ", \"action_ordinal\": " << *event.action_ordinal;
        }
        if (event.frame_index.has_value()) {
            out << ", \"frame_index\": " << *event.frame_index;
        }
        if (event.movement_thread_order_index.has_value()) {
            out << ", \"movement_thread_order_index\": "
                << *event.movement_thread_order_index;
        }
        if (event.movement_callback_pc.has_value()) {
            out << ", \"movement_callback_pc\": " << *event.movement_callback_pc;
        }
        if (event.movement_deferred_callback_pc.has_value()) {
            out << ", \"movement_deferred_callback_pc\": "
                << *event.movement_deferred_callback_pc;
        }
        if (event.passive_completion_mask_before.has_value()) {
            out << ", \"passive_completion_mask_before\": "
                << *event.passive_completion_mask_before;
        }
        if (event.passive_completion_mask_after.has_value()) {
            out << ", \"passive_completion_mask_after\": "
                << *event.passive_completion_mask_after;
        }
        if (!event.passive_completion_reason.empty()) {
            out << ", \"passive_completion_reason\": \""
                << json_escape(event.passive_completion_reason) << "\"";
        }
        if (!event.visual_command_kind.empty()) {
            out << ", \"visual_command_kind\": \""
                << json_escape(event.visual_command_kind) << "\"";
        }
        if (!event.visual_resource.empty()) {
            out << ", \"visual_resource\": \""
                << json_escape(event.visual_resource) << "\"";
        }
        if (event.visual_record_index.has_value()) {
            out << ", \"visual_record_index\": " << *event.visual_record_index;
        }
        if (event.visual_epoch.has_value()) {
            out << ", \"visual_epoch\": " << *event.visual_epoch;
        }
        if (event.visual_task_sequence.has_value()) {
            out << ", \"visual_task_sequence\": " << *event.visual_task_sequence;
        }
        if (event.visual_payload_mode.has_value()) {
            out << ", \"visual_payload_mode\": " << *event.visual_payload_mode;
        }
        if (event.visual_effective_mode.has_value()) {
            out << ", \"visual_effective_mode\": " << *event.visual_effective_mode;
        }
        if (!event.visual_child_kind.empty()) {
            out << ", \"visual_child_kind\": \""
                << json_escape(event.visual_child_kind) << "\"";
        }
        if (event.pathing_accepted_candidates.has_value()) {
            out << ", \"pathing_accepted_candidates\": " << *event.pathing_accepted_candidates;
        }
        if (event.pathing_aggregate_score.has_value()) {
            out << ", \"pathing_aggregate_score\": " << *event.pathing_aggregate_score;
        }
        if (!event.instruction_mode_provenance.empty()) {
            out << ", \"instruction_mode_provenance\": \""
                << json_escape(event.instruction_mode_provenance) << "\"";
        }
        out << ", \"detail\": \"" << json_escape(event.detail) << "\"";
        out << "}";
        if (i + 1 != result.events.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

} // namespace savor::predict
