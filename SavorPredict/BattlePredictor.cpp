#include "BattlePredictor.h"

#include "BattleVisualRngModel.h"
#include "FirstBattleDataModel.h"
#include "MovementModel.h"
#include "PreAiCameraModel.h"
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
    int instr_param_0x6 = 0;
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

int element_effectiveness_tenths(const soa::ElementalEffectiveness& effectiveness, int element) {
    switch (element) {
    case 0: return effectiveness.green;
    case 1: return effectiveness.red;
    case 2: return effectiveness.purple;
    case 3: return effectiveness.blue;
    case 4: return effectiveness.Yellow;
    case 5: return effectiveness.Silver;
    default: return 10;
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
    state.agile = source.instance.current_base_stats.Agility;
    state.attack = source.instance.current_derived_stats.Attack;
    state.defense = source.instance.current_derived_stats.Defense;
    state.hit = source.instance.current_derived_stats.HitChance;
    state.dodge = source.instance.current_derived_stats.DodgeChance;
    state.element = source.instance.current_weapon_element;
    state.attack_inputs_known =
        state.attack > 0 && state.hit > 0 && state.defense >= 0 && state.dodge >= 0;

    if (profile.name == "first-battle") {
        if (const auto first_battle = first_battle_actor_by_slot(slot_index); first_battle.has_value()) {
            state.quick = first_battle->quick;
            state.quick_known = true;
            if (state.agile == 0) {
                state.agile = first_battle->agile;
            }
            if (state.attack == 0 && first_battle->attack_known) {
                state.attack = first_battle->attack;
            }
            if (state.hit == 0 && first_battle->hit_known) {
                state.hit = first_battle->hit;
            }
            if (state.defense == 0 && first_battle->defense_known) {
                state.defense = first_battle->defense;
            }
            if (state.dodge == 0 && first_battle->dodge_known) {
                state.dodge = first_battle->dodge;
            }
            if (state.element < 0 || state.element > 5) {
                state.element = first_battle->element;
            }
            if (state.base_counter_chance == 0) {
                state.base_counter_chance = first_battle->counter_chance;
            }
            if (state.counter_chance_increment == 0) {
                state.counter_chance_increment = first_battle->counter_chance;
            }
            if (state.present && source.instance.counter_chance == 0 && state.counter_chance_increment != 0) {
                warnings.push_back(
                    std::string("counter chance increment was not materialized in BattleContext ")
                    + "field for slot " + std::to_string(slot_index)
                    + "; first-battle profile uses the actor counter chance as a fallback");
            }
            if (state.movement_flags == 0) {
                state.movement_flags = static_cast<std::uint16_t>(first_battle->movement_flags);
            }
            state.attack_inputs_known =
                state.attack > 0 && state.hit > 0 && state.defense >= 0 && state.dodge >= 0;
        }
    }

    if (state.present && !state.quick_known) {
        warnings.push_back(
            "quick stat is not materialized in BattleContext for slot "
            + std::to_string(slot_index)
            + "; non-first-battle turn-order prediction needs another data source");
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
        int width = 1;
        int depth = 1;
        if (const auto* source = context_slot(context, slot.slot); source != nullptr) {
            if (source->instance.width != 0) {
                width = source->instance.width;
            }
            if (source->instance.depth != 0) {
                depth = source->instance.depth;
            }
        }
        result.push_back(MovementSlotState{
            .slot = slot.slot,
            .present = slot.present,
            .is_player = slot.is_player,
            .alive = slot.alive,
            .status_flags = slot.status_flags,
            .movement_flags = slot.movement_flags,
            .width = width,
            .depth = depth,
            .start_position = slot.start_position,
        });
    }
    return result;
}

void append_event(BattlePredictionResult& result, BattlePredictionEvent event) {
    event.sequence = static_cast<int>(result.events.size()) + 1;
    result.total_draws_consumed += event.draws_consumed;
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

bool apply_enemy_event_start_positions(
    BattlePredictionResult& result,
    std::optional<int> enemy_event_id,
    std::vector<BattlePredictionSlotState>& slots) {
    if (!enemy_event_id.has_value()) {
        return true;
    }

    const auto layout = enemy_event_start_positions(*enemy_event_id);
    if (!layout.has_value()) {
        append_event(result, {
            .phase = "encounter_setup",
            .label = "unsupported_enemy_event_start_positions",
            .status = BattlePredictionEventStatus::Unsupported,
            .detail = "no static enemyevent.csv start-position row is encoded for enemy_event_id="
                + std::to_string(*enemy_event_id),
        });
        return false;
    }

    for (const auto& position : layout->positions) {
        if (auto* slot = find_slot(slots, position.slot); slot != nullptr) {
            slot->start_position = position;
        }
        if (!position.present) {
            continue;
        }
        append_event(result, {
            .phase = "encounter_setup",
            .label = "enemy_event_start_position",
            .status = BattlePredictionEventStatus::Exact,
            .actor_slot = position.slot,
            .detail = "enemy_event_id=" + std::to_string(*enemy_event_id)
                + "; " + start_position_detail(position),
        });
    }
    return true;
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

    add_validation(
        result,
        "pre_ai",
        BattlePredictionValidationStatus::Provisional,
        "v1 macro contract is fake_attacks + 1 + pc_count; direct 800608DC capture remains perturbing");

    if (has_event_status_in_phase(result, "encounter_setup", BattlePredictionEventStatus::Unsupported)) {
        add_validation(
            result,
            "start_positions",
            BattlePredictionValidationStatus::Unsupported,
            "requested enemy-event start-position row is not encoded");
    } else if (has_event(result, "encounter_setup", "enemy_event_start_position")) {
        add_validation(
            result,
            "start_positions",
            BattlePredictionValidationStatus::Exact,
            "combatant start positions are sourced from encoded ALX 5.0.0 enemyevent.csv rows");
    } else {
        add_validation(
            result,
            "start_positions",
            BattlePredictionValidationStatus::NotExercised,
            "no enemy_event_id was supplied for static start-position seeding");
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

    if (result.exact_through_turn_order) {
        add_validation(
            result,
            "turn_order",
            BattlePredictionValidationStatus::Validated,
            "draw count and execution order are exact for the modeled queue; qsort live snapshots are still required for unresolved tie cases",
            result.exact_draws_through_turn_order);
    } else if (has_event_status_in_phase(result, "turn_order", BattlePredictionEventStatus::Ambiguous)) {
        add_validation(
            result,
            "turn_order",
            BattlePredictionValidationStatus::Ambiguous,
            "turn order is ambiguous because priorities or qsort ordering were not exact",
            result.exact_draws_through_turn_order);
    } else {
        add_validation(
            result,
            "turn_order",
            BattlePredictionValidationStatus::Provisional,
            "turn-order prediction did not reach the exact validation boundary",
            result.exact_draws_through_turn_order);
    }

    if (has_event_status_in_phase(result, "movement_setup", BattlePredictionEventStatus::Unsupported)) {
        add_validation(
            result,
            "movement_setup",
            BattlePredictionValidationStatus::Unsupported,
            "movement setup encountered an unsupported action or backend");
    } else if (has_event_status_in_phase(result, "movement_setup", BattlePredictionEventStatus::Ambiguous)) {
        add_validation(
            result,
            "movement_setup",
            BattlePredictionValidationStatus::Ambiguous,
            "direct/fallback movement mode requires movement worksheet/path fields that are not fully materialized yet");
    } else if (has_phase_event(result, "movement_setup")) {
        add_validation(
            result,
            "movement_setup",
            BattlePredictionValidationStatus::Provisional,
            "handler-level first-battle movement setup is modeled; representative live worker-selection validation remains open");
    } else {
        add_validation(
            result,
            "movement_setup",
            BattlePredictionValidationStatus::NotExercised,
            "no movement setup events were predicted");
    }

    if (has_event_with_status(result, "movement_setup", "worker_select", BattlePredictionEventStatus::Ambiguous)) {
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
            "first-battle Soldier drop row order and stop-after-success are validated for Electri Box, Moonberry, and no-drop outcomes");
    } else {
        add_validation(
            result,
            "drop",
            BattlePredictionValidationStatus::NotExercised,
            "no lethal enemy damage reached the drop table");
    }

    if (has_event(result, "action_visual_rng", "ambiguous_effect_source_key")) {
        add_validation(
            result,
            "effect_burst",
            BattlePredictionValidationStatus::Ambiguous,
            "effect burst source key is ambiguous for at least one action");
    } else if (has_event(result, "action_visual_rng", "unsupported_effect_source_key")) {
        add_validation(
            result,
            "effect_burst",
            BattlePredictionValidationStatus::Unsupported,
            "an effect source key lacked a supported first-battle burst model");
    } else if (has_event(result, "action_visual_rng", "combat_effect_burst")) {
        add_validation(
            result,
            "effect_burst",
            BattlePredictionValidationStatus::Validated,
            "supported first-battle source keys 4/5 use 110 draws and observed crit key 8 uses 100 draws");
    } else {
        add_validation(
            result,
            "effect_burst",
            BattlePredictionValidationStatus::NotExercised,
            "no landed attack effect burst was predicted");
    }

    if (has_phase_event(result, "action_visual_rng")) {
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
    if (result.has_unsupported_events || !result.errors.empty()) {
        total_status = BattlePredictionValidationStatus::Unsupported;
        total_detail =
            "unsupported events or errors prevent total draw-count validation";
    } else if (result.has_ambiguous_events) {
        total_status = BattlePredictionValidationStatus::Ambiguous;
        total_detail =
            "ambiguous events prevent total draw-count validation";
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
    inputs.target_element_effectiveness_tenths =
        element_effectiveness_tenths(target_context_slot->instance.current_elemental_eff, attacker->element);
    if (inputs.target_element_effectiveness_tenths == 0) {
        inputs.target_element_effectiveness_tenths = 10;
    }
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
            int target_slot = soa::battle::actions::resolveTargetIndex(command.params.target_slot);
            if (target_slot < 0) {
                target_slot = first_alive_enemy_slot(slots);
            }
            if (target_slot < 0) {
                append_event(result, {
                    .phase = "player_command",
                    .label = "unsupported_missing_attack_target",
                    .status = BattlePredictionEventStatus::Unsupported,
                    .actor_slot = actor_slot,
                    .detail = "attack has no concrete live target",
                });
                continue;
            }
            actions.push_back({
                .actor_slot = actor_slot,
                .target_slot = target_slot,
                .attack = true,
                .enemy_owned = false,
                .instr_param_0x6 = 0,
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
        if (profile.name == "first-battle" && slot != 4 && slot != 5) {
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
                .instr_param_0x6 = soldier_attack_param_from_rand(*decision.attack_param_rand),
                .source = "soldier_ai",
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
                .label = "unsupported_missing_quick",
                .status = BattlePredictionEventStatus::Unsupported,
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

        append_event(result, {
            .phase = "turn_order",
            .label = "entry",
            .status = entry.assigned_priority.has_value()
                ? BattlePredictionEventStatus::Exact
                : BattlePredictionEventStatus::Ambiguous,
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
    if (!turn_order.execution_order_exact) {
        detail << "; priority ties or unresolved priorities make order ambiguous";
    }

    append_event(result, {
        .phase = "turn_order",
        .label = "resolve_turn_order",
        .status = turn_order.execution_order_exact
            ? BattlePredictionEventStatus::Exact
            : BattlePredictionEventStatus::Ambiguous,
        .rng_seed_before = before,
        .rng_seed_after = state,
        .draws_consumed = turn_order.draws_consumed,
        .detail = detail.str(),
    });

    result.exact_through_turn_order =
        !result.has_unsupported_events && !result.has_ambiguous_events && turn_order.execution_order_exact;
    result.exact_draws_through_turn_order = result.total_draws_consumed;
}

BattlePredictionEventStatus battle_event_status_from_movement_status(MovementSimulationStatus status);
int movement_reachability_code(MovementReachabilityStatus status);

bool append_movement_setup(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const soa::battle::ctx::BattleContext& context,
    const std::vector<BattlePredictionSlotState>& slots,
    QueuedPredictionAction& action) {
    const auto before = state;
    MovementModelInputs movement_inputs{
        .backend = MovementBackend::HandlerLevelFirstBattle,
        .rng_state = state,
        .actor_slot = action.actor_slot,
        .target_slot = action.target_slot,
        .queued_instruction = action.attack ? 3 : (action.guard ? 4 : 0),
        .instr_param_0x6 = action.instr_param_0x6,
        .enemy_owned = action.enemy_owned,
        .slots = movement_slots_from_prediction_slots(slots, context),
    };
    if (result.enemy_event_id == 0) {
        movement_inputs.actor_worksheet =
            project_enemy_event0_movement_worksheet_snapshot(movement_inputs);
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

    std::ostringstream detail;
    detail << "initial_instr_param_0x6=" << movement.initial_instr_param_0x6
           << "; final_instr_param_0x6=" << movement.final_instr_param_0x6
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
        .movement_reachability = movement_reachability_code(movement.reachability),
        .movement_worker = movement_selected_worker_name(movement.selected_worker),
        .detail = detail.str(),
    });

    for (const auto& route : movement.passive_routes) {
        append_event(result, {
            .phase = "movement_setup",
            .label = "passive_route",
            .status = battle_event_status_from_movement_status(route.status),
            .actor_slot = action.actor_slot,
            .target_slot = route.slot,
            .movement_worker = movement_selected_worker_name(route.selected_worker),
            .detail = passive_movement_route_kind_name(route.route),
        });
    }

    return true;
}

BattlePredictionEventStatus battle_event_status_from_visual_status(BattleVisualRngStepStatus status) {
    switch (status) {
    case BattleVisualRngStepStatus::Exact:
        return BattlePredictionEventStatus::Exact;
    case BattleVisualRngStepStatus::Provisional:
        return BattlePredictionEventStatus::Provisional;
    case BattleVisualRngStepStatus::Ambiguous:
        return BattlePredictionEventStatus::Ambiguous;
    case BattleVisualRngStepStatus::Unsupported:
        return BattlePredictionEventStatus::Unsupported;
    }
    return BattlePredictionEventStatus::Unsupported;
}

BattlePredictionEventStatus battle_event_status_from_movement_status(MovementSimulationStatus status) {
    switch (status) {
    case MovementSimulationStatus::Exact:
        return BattlePredictionEventStatus::Exact;
    case MovementSimulationStatus::Provisional:
        return BattlePredictionEventStatus::Provisional;
    case MovementSimulationStatus::Skipped:
        return BattlePredictionEventStatus::Skipped;
    case MovementSimulationStatus::Unsupported:
        return BattlePredictionEventStatus::Unsupported;
    case MovementSimulationStatus::Ambiguous:
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

bool append_visual_rng_steps(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const QueuedPredictionAction& action,
    const BattleVisualRngModelResult& visual) {
    for (const auto& step : visual.steps) {
        BattlePredictionEvent event;
        event.phase = step.phase;
        event.label = step.label;
        event.status = battle_event_status_from_visual_status(step.status);
        event.actor_slot = action.actor_slot;
        event.target_slot = action.target_slot;
        event.draws_consumed = step.draws_consumed;
        event.effect_source_key = step.effect_source_key;
        event.detail = step.detail;
        if (step.draws_consumed > 0) {
            event.rng_seed_before = state;
            advance_without_rand_values(state, step.draws_consumed);
            event.rng_seed_after = state;
        }
        append_event(result, std::move(event));
    }
    return !visual.has_ambiguous_steps && !visual.has_unsupported_steps;
}

bool append_pre_attack_visual_rng(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const BattlePredictionProfile& profile,
    const QueuedPredictionAction& action) {
    const auto visual = model_first_battle_basic_attack_visual_rng({
        .profile_name = profile.name,
        .actor_slot = action.actor_slot,
        .target_slot = action.target_slot,
        .include_action_view_camera = true,
        .include_effect_bursts = false,
    });
    return append_visual_rng_steps(result, state, action, visual);
}

bool append_post_attack_visual_rng(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const BattlePredictionProfile& profile,
    const QueuedPredictionAction& action,
    bool attack_landed,
    bool attack_was_critical,
    bool counter_follow_up) {
    const auto visual = model_first_battle_basic_attack_visual_rng({
        .profile_name = profile.name,
        .actor_slot = action.actor_slot,
        .target_slot = action.target_slot,
        .attack_landed = attack_landed,
        .attack_was_critical = attack_was_critical,
        .counter_follow_up = counter_follow_up,
        .include_action_view_camera = false,
        .include_effect_bursts = true,
    });
    return append_visual_rng_steps(result, state, action, visual);
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
    const BattlePredictionProfile& profile,
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
            .label = "unsupported_missing_attack_inputs",
            .status = BattlePredictionEventStatus::Unsupported,
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
    if (auto* updated_target = find_slot(slots, counter_action.target_slot);
        updated_target != nullptr && !updated_target->alive) {
        append_death_and_drop(result, state, context, counter_action);
        return;
    }

    append_post_attack_visual_rng(
        result,
        state,
        profile,
        counter_action,
        attack.attack_result != 0,
        false,
        true);
}

void append_attack_resolution(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const soa::battle::ctx::BattleContext& context,
    std::vector<BattlePredictionSlotState>& slots,
    const BattlePredictionProfile& profile,
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
            .label = "unsupported_missing_attack_inputs",
            .status = BattlePredictionEventStatus::Unsupported,
            .actor_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .detail = "attack, hit, defense, dodge, or elemental effectiveness data is unavailable",
        });
        return;
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
    event.detail = "basic attack helper; instr_param_0x6=" + std::to_string(action.instr_param_0x6);
    append_event(result, std::move(event));

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

    if (counter_triggered) {
        append_counter_follow_up(result, state, context, slots, profile, action);
    }

    if (attack.attack_result != 0) {
        append_post_attack_visual_rng(
            result,
            state,
            profile,
            action,
            true,
            attack.attack_result == 2,
            false);
    }

    append_damage_application(result, slots, action, attack);

    if (!target->alive) {
        append_death_and_drop(result, state, context, action);
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
    const BattlePredictionProfile& profile,
    const BattlePredictionOptions& options) {
    for (const int slot : execution_slots) {
        const auto* action = find_action_for_slot(actions, slot);
        if (action == nullptr) {
            append_event(result, {
                .phase = "action_execution",
                .label = "unsupported_missing_action",
                .status = BattlePredictionEventStatus::Unsupported,
                .actor_slot = slot,
                .detail = "turn-order slot had no queued action in predictor state",
            });
            continue;
        }

        auto* actor = find_slot(slots, action->actor_slot);
        if (actor == nullptr || !actor->alive) {
            append_event(result, {
                .phase = "action_execution",
                .label = "skipped_dead_actor",
                .status = BattlePredictionEventStatus::Skipped,
                .actor_slot = action->actor_slot,
                .target_slot = action->target_slot,
                .detail = "actor died before its queued action",
            });
            continue;
        }

        QueuedPredictionAction resolved_action = *action;
        if (!append_movement_setup(result, state, context, slots, resolved_action)) {
            continue;
        }

        if (options.include_visual_rng_gap_events) {
            const bool visual_exact = append_pre_attack_visual_rng(result, state, profile, resolved_action);
            if (!visual_exact && !options.continue_after_visual_rng_gap) {
                continue;
            }
        }

        append_attack_resolution(result, state, context, slots, profile, resolved_action);
        if (!has_alive_enemy(slots) || !has_alive_pc(slots)) {
            break;
        }
    }
}

} // namespace

BattlePredictionProfile first_battle_prediction_profile() {
    return BattlePredictionProfile{
        .name = "first-battle",
        .supported_turn_index = 1,
        .supports_status_effects = false,
        .supports_non_soldier_ai = false,
    };
}

std::optional<BattlePredictionProfile> battle_prediction_profile_by_name(std::string_view name) {
    if (name == "first-battle") {
        return first_battle_prediction_profile();
    }
    return std::nullopt;
}

BattlePredictionResult predict_battle(const BattlePredictionInput& input) {
    BattlePredictionResult result;
    result.profile = input.profile;
    result.starting_rng_seed = input.starting_rng_seed;
    result.final_rng_seed = input.starting_rng_seed;
    result.enemy_event_id = input.enemy_event_id;

    if (input.profile.name != "first-battle") {
        result.outcome = BattlePredictionOutcome::Unsupported;
        result.errors.push_back("unsupported battle prediction profile: " + input.profile.name);
        append_validation_statuses(result);
        return result;
    }

    auto slots = make_initial_slots(input.profile, input.context, result.warnings);
    auto state = input.starting_rng_seed;
    if (!apply_enemy_event_start_positions(result, input.enemy_event_id, slots)) {
        result.final_rng_seed = state;
        result.final_slots = std::move(slots);
        result.outcome = BattlePredictionOutcome::Unsupported;
        append_validation_statuses(result);
        return result;
    }

    const int pc_count = std::max(1, present_alive_pc_count(slots));
    append_pre_ai_events(
        result,
        state,
        static_cast<int>(input.turn_plan.fake_attack_count),
        pc_count);

    std::vector<QueuedPredictionAction> actions;
    append_player_commands(result, input.turn_plan, slots, actions);
    append_enemy_ai(result, state, input.profile, slots, actions);

    std::vector<int> execution_slots;
    append_turn_order(result, state, slots, actions, execution_slots);
    append_action_execution(
        result,
        state,
        input.context,
        slots,
        actions,
        execution_slots,
        input.profile,
        input.options);

    result.final_rng_seed = state;
    result.final_slots = std::move(slots);

    if (result.has_unsupported_events || !result.errors.empty()) {
        result.outcome = BattlePredictionOutcome::Unsupported;
    } else if (result.has_ambiguous_events) {
        result.outcome = BattlePredictionOutcome::Ambiguous;
    } else if (!has_alive_pc(result.final_slots)) {
        result.outcome = BattlePredictionOutcome::Defeat;
    } else if (!has_alive_enemy(result.final_slots)) {
        result.outcome = BattlePredictionOutcome::Victory;
    } else {
        result.outcome = BattlePredictionOutcome::ReachedNextTurn;
    }

    append_validation_statuses(result);

    return result;
}

const char* battle_prediction_outcome_name(BattlePredictionOutcome outcome) {
    switch (outcome) {
    case BattlePredictionOutcome::ReachedNextTurn: return "ReachedNextTurn";
    case BattlePredictionOutcome::Victory: return "Victory";
    case BattlePredictionOutcome::Defeat: return "Defeat";
    case BattlePredictionOutcome::Unsupported: return "Unsupported";
    case BattlePredictionOutcome::Ambiguous: return "Ambiguous";
    }
    return "Unsupported";
}

const char* battle_prediction_event_status_name(BattlePredictionEventStatus status) {
    switch (status) {
    case BattlePredictionEventStatus::Exact: return "Exact";
    case BattlePredictionEventStatus::Provisional: return "Provisional";
    case BattlePredictionEventStatus::Skipped: return "Skipped";
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
    case BattlePredictionValidationStatus::Unsupported: return "Unsupported";
    case BattlePredictionValidationStatus::Ambiguous: return "Ambiguous";
    }
    return "Unsupported";
}

void write_battle_prediction_text(const BattlePredictionResult& result, std::ostream& out) {
    out << "SavorPredict predict-battle\n";
    out << "  profile: " << result.profile.name << "\n";
    out << "  outcome: " << battle_prediction_outcome_name(result.outcome) << "\n";
    out << "  start_seed: " << result.starting_rng_seed << " ("
        << hex_seed(result.starting_rng_seed) << ")\n";
    if (result.enemy_event_id.has_value()) {
        out << "  enemy_event_id: " << *result.enemy_event_id << "\n";
    }
    out << "  final_seed: " << result.final_rng_seed << " ("
        << hex_seed(result.final_rng_seed) << ")\n";
    out << "  total_draws_consumed: " << result.total_draws_consumed << "\n";
    out << "  exact_through_turn_order: " << (result.exact_through_turn_order ? "true" : "false") << "\n";
    out << "  exact_draws_through_turn_order: " << result.exact_draws_through_turn_order << "\n";
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
        if (event.movement_reachability.has_value()) {
            out << " movement_reachability=" << *event.movement_reachability;
        }
        if (!event.movement_worker.empty()) {
            out << " movement_worker=" << event.movement_worker;
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
        if (!event.detail.empty()) {
            out << " detail=\"" << event.detail << "\"";
        }
        out << "\n";
    }
}

void write_battle_prediction_json(const BattlePredictionResult& result, std::ostream& out) {
    out << "{\n";
    out << "  \"profile\": \"" << json_escape(result.profile.name) << "\",\n";
    out << "  \"outcome\": \"" << battle_prediction_outcome_name(result.outcome) << "\",\n";
    out << "  \"starting_rng_seed\": " << result.starting_rng_seed << ",\n";
    out << "  \"starting_rng_seed_hex\": \"" << hex_seed(result.starting_rng_seed) << "\",\n";
    if (result.enemy_event_id.has_value()) {
        out << "  \"enemy_event_id\": " << *result.enemy_event_id << ",\n";
    }
    out << "  \"final_rng_seed\": " << result.final_rng_seed << ",\n";
    out << "  \"final_rng_seed_hex\": \"" << hex_seed(result.final_rng_seed) << "\",\n";
    out << "  \"total_draws_consumed\": " << result.total_draws_consumed << ",\n";
    out << "  \"exact_through_turn_order\": " << (result.exact_through_turn_order ? "true" : "false") << ",\n";
    out << "  \"exact_draws_through_turn_order\": " << result.exact_draws_through_turn_order << ",\n";
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
        if (event.movement_reachability.has_value()) {
            out << ", \"movement_reachability\": " << *event.movement_reachability;
        }
        if (!event.movement_worker.empty()) {
            out << ", \"movement_worker\": \"" << json_escape(event.movement_worker) << "\"";
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
